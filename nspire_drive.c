/* USB games, read-only: all storage transfers happen in menus/loading;
 * a running USB game has its complete ROM in memory and no open file.
 * And, since 2026-11-17, saves and states as whole files on the drive
 * (nspire_drive_put and the calls after it), at safe points only. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nspire_drive.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "nspire_os.h"
#include "nspire_usbport.h"
#include "nspire_padlog.h"
/* nspire_exit.h's (R11w; that header wants common.h's types; weak: the unit
 * tests' builds have no nspire_exit.c) */
void nspire_drive_saves_start(void) __attribute__((weak));   /* (nspire_drive_saves.h; weak: builds without it) */
long states_drop_caches(void) __attribute__((weak));   /* (nspire_states.c; weak: the unit tests build without it) */
void nspire_link_log(const char *fmt, ...) __attribute__((format(printf, 1, 2), weak));
unsigned nspire_exit_usb_left_ms(void) __attribute__((weak));
void nspire_exit_usb_timed_out(const char *step) __attribute__((weak));
#ifndef NSPIRE_TICK_HZ
#define NSPIRE_TICK_HZ 32768u   /* (nspire/nspire.h's) */
#endif

int nspire_drive_path(const char *path)
{
  return path && !strncmp(path, DRIVE_PREFIX, sizeof(DRIVE_PREFIX) - 1);
}

int nspire_drive_is_game(const char *name)
{
  size_t n = strlen(name);
  if (n > 4 && !strcasecmp(name + n - 4, ".tns")) n -= 4;
  return n > 4 && (!strncasecmp(name + n - 4, ".gba", 4) ||
         !strncasecmp(name + n - 4, ".agb", 4) || !strncasecmp(name + n - 4, ".gbz", 4) ||
         !strncasecmp(name + n - 4, ".zip", 4));
}

#ifndef NSPIRE_NO_USBMS
#include "usbms.h"
#include "usbms_platform.h"

/* These only observe the host's fake drive in tests. */
#ifdef NSPIRE_HOST
int fake_plug(const char *path);
int host_usbms_active(void) __attribute__((weak));
#endif
void nspire_osd_message(const char *format, ...);
uint32_t nspire_ticks(void);
extern uint32_t nspire_rom_load_need_mb;

#pragma weak usbms_platform_diag
static enum drive_state state;
static unsigned epoch, generation;
static int started, exit_set, game_from_drive, resident, removed_note;
static unsigned nomem_tries;   /* openings tried again after no memory (the log's count) */
static uint32_t menu_at;

int nspire_drive_enabled(void)
{
#ifdef NSPIRE_HOST
  const char *image = getenv("GPSP_USB_IMAGE");
  return image && image[0];
#else
  return 1;
#endif
}

/* Includes setup and cleanup after detach: the OS may still hold a pipe
 * or transfer even after the physical drive is gone. The arbiter must not
 * restart power or take over the controller until those resources retire. */
int nspire_usbms_active(void)
{
#ifdef NSPIRE_HOST
  if (host_usbms_active && host_usbms_active()) return 1;
#endif
  return usbms_platform_owns_usb();
}

/* R11w (2026-11-15): nspire_drive_os.c's count of the drive driver's
 * callbacks inside gpSP (weak: 0 without it, the host's fake drive). */
unsigned nspire_drive_os_callbacks(void) __attribute__((weak));
/* (r19-usb) the exit's trail (nspire_exit.c; weak: drive_tests have none) */
void nspire_exit_trail(const char *what) __attribute__((weak));

int nspire_drive_stop_within(unsigned ms)
{
  uint32_t t0 = nspire_ticks(), cpsr;

  if (!started)
    return 0;
  /* usbms_platform_stop waits for a callback inside gpSP with no end:
   * waited for here first, bounded (the OS runs it in its sleeps) */
  if (nspire_drive_os_callbacks && nspire_drive_os_callbacks())
  {
    cpsr = nspire_irqs_unmask();
    while (nspire_drive_os_callbacks() && (uint64_t)(nspire_ticks() - t0) * 1000 < (uint64_t)ms * NSPIRE_TICK_HZ)
      if (!nspire_os_sleep())
        break;
    nspire_irqs_restore(cpsr);
    if (nspire_drive_os_callbacks())
      return -1;   /* the caller restarts the calculator (never usbms_stop's endless wait) */
  }
  if ((nspire_drive_os_callbacks && nspire_drive_os_callbacks()) || (!ms && nspire_usbms_active()))
    return -1;
  /* (r19-usb) usbms_stop's own waits sleep through nspire_os_sleep (the
   * exit's guard cuts one that never ends); its unmount writes the drive */
  if (nspire_exit_trail)
    nspire_exit_trail(state >= DRIVE_ATTACHED ? "usbms_stop: the drive unmounted, its pipes aborted and closed, its "
                                              "callbacks waited for" : "usbms_stop (no drive)");
  usbms_stop();
  started = 0;
  generation = 0;
  state = DRIVE_OFF;
  nspire_os_want(OS_WANT_MS, 0);
  return 0;
}

/* (r19-usb-exit) The quit's stop with no device ever offered to the OS's
 * drivers and the port quiet (nspire_usbpad_exit_untouched): no drive was
 * attached and no callback of the driver's ever ran or can start, so
 * usbms_stop's waits for one (its 10 OS ticks for callback epilogues after
 * the retire, ~100 ms: a CX's hardware run) are for nothing. Its sleeps
 * (nspire_os_sleep, the driver's own) return at once while held: every one
 * of its loops is bounded by its count, and the one that isn't (a callback
 * inside) is never entered: none is (checked just before). A drive in, or a
 * callback inside: -2, and the caller's usual stop. */
int nspire_drive_stop_quiet(void)
{
  if (!started)
    return 0;
  if (state >= DRIVE_ATTACHED || nspire_usbms_active() || (nspire_drive_os_callbacks && nspire_drive_os_callbacks()))
    return -2;
  nspire_os_sleep_hold(1);
  usbms_stop();
  nspire_os_sleep_hold(0);
  started = 0;
  generation = 0;
  state = DRIVE_OFF;
  nspire_os_want(OS_WANT_MS, 0);
  return 0;
}

void nspire_drive_stop(void)
{
  if (nspire_drive_stop_within(nspire_exit_usb_left_ms ? nspire_exit_usb_left_ms() : 4000) != 0 &&
      nspire_exit_usb_timed_out)
    nspire_exit_usb_timed_out("USB drive");
}

void nspire_drive_start(void)
{
  if (started || !nspire_drive_enabled()) return;
  {
    int failed = usbms_start() != 0;
#ifdef USBMS_HAS_MEM_HELD
    /* ms17: memory earlier runs' drives left with the OS (transfers it never gave back: kept for
     * good); at 8 such the driver does not start */
    usbms_platform_diag_t d = { 0 };
    if (usbms_platform_diag)
      usbms_platform_diag(&d);   /* (weak: the PC's fake drive has none) */
    if (d.kept && nspire_link_log)
      nspire_link_log("drive: %u blocks of transfer memory kept from earlier drives (%lu bytes)%s", (unsigned)d.kept,
                      (unsigned long)d.kept_bytes, failed ? ": no more can be kept, the driver did not start" : "");
#endif
    if (failed) { state = DRIVE_OFF; return; }
  }
  started = 1;
  state = DRIVE_NONE;
  generation = 0;
  if (!exit_set) exit_set = atexit(nspire_drive_stop) == 0;
#ifdef USBMS_WRITE_API
  /* saves on the drive: the menus' second place (nspire_places.h) is this drive's from here */
  if (nspire_drive_saves_start)
    nspire_drive_saves_start();
#endif
#ifdef NSPIRE_HOST
  fake_plug(getenv("GPSP_USB_IMAGE"));
#endif
  nspire_padlog_add(PL_DRIVE, DRIVE_NONE, 0, 0, 0, 0, 0);
}

void nspire_drive_poll(void)
{
  if (!started) return;
  unsigned now = usbms_platform_attached();
  nspire_os_want(OS_WANT_MS, now != 0 || usbms_platform_owns_usb());
  if (now == generation) return;
  generation = now;
  epoch++;
  state = now ? DRIVE_ATTACHED : DRIVE_NONE;
  if (now)
    nspire_usbport_os_report(PORT_OS_DRIVE, 8, 0);
  else
  {
    usbms_poll();                 /* unmount only, never touches the drive */
    if (nspire_usbport_os() == PORT_OS_DRIVE)
      nspire_usbport_os_report(PORT_OS_NONE, 0, 0);
    removed_note = game_from_drive && resident;
  }
  nspire_padlog_add(PL_DRIVE, state, resident, 0, now, epoch, 0);
}

void nspire_drive_frame(void)
{
  nspire_drive_poll();
#ifdef USBMS_HAS_MEM_HELD
  if (started)
    usbms_tick();   /* (the write buffer given back 3 s after a burst of writes; never touches the drive) */
#endif
  if (removed_note)
  {
    removed_note = 0;
    nspire_osd_message("USB drive removed; game is in memory");
  }
}

enum drive_state nspire_drive_state(void) { return state; }
unsigned nspire_drive_epoch(void) { return epoch; }

const char *nspire_drive_summary(void)
{
  if (state == DRIVE_OFF) return nspire_drive_enabled() ? "USB drive driver unavailable" : "USB drives not enabled";
  if (state == DRIVE_NONE) return "Plug in a USB drive (into the adapter or a USB hub)";
  if (state == DRIVE_ATTACHED) return "Opening USB drive...";
  return usbms_status();   /* (DRIVE_NOMEM: "Not enough memory for the USB drive") */
}

/* Opens (mounts) a drive that is attached and not opened yet: the first
 * commands a drive gets, up to a few seconds for one that spins up. Only
 * from the menus and the loader, never inside a game's frame. */
/* (2026-11-18, Hub 5) a drive on a hub whose devices may be dead now (its
 * power fell and it is watched, a recovery due or under way, given up): not
 * asked anything. Each command to a dead drive waits for its time limit;
 * its saves go to the calculator meanwhile, as with the drive out. The
 * game's own file (nspire_drive_fopen, the ROM's pages) is not stopped. */
int nspire_usbhub_doubtful(void) __attribute__((weak));
static int drive_live(void)
{
  return state == DRIVE_READY && !(nspire_usbhub_doubtful && nspire_usbhub_doubtful());
}

static void drive_open(void)
{
  if (state != DRIVE_ATTACHED || (nspire_usbhub_doubtful && nspire_usbhub_doubtful()))
    return;
  unsigned attached = generation;
  int result = usbms_poll();
  nspire_drive_poll();
  if (attached == generation && generation)
  {
    state = result == USBMS_READY ? DRIVE_READY : DRIVE_ERROR;
#ifdef USBMS_HAS_NO_MEMORY
    if (state == DRIVE_ERROR && usbms_no_memory())
      state = DRIVE_NOMEM;
#endif
    epoch++;
    usbms_info_t info;
    usbms_info(&info);
    nspire_padlog_add(PL_DRIVE, state, info.filesystem, 0,
                     info.commands, (uint32_t)(info.bytes >> 20), 0);
  }
}

/* A drive that is in and could not be opened (a card reader whose card
 * went in later, a drive that was not ready, a read error) gets another
 * try without being pulled out: the drive's page, on enter. */
void nspire_drive_retry(void)
{
#ifdef USBMS_HAS_RETRY
  if ((state != DRIVE_ERROR && state != DRIVE_NOMEM) || !generation)
    return;
  if (state == DRIVE_NOMEM)
  {
    /* ms17: memory first: the menus' caches given up (the saves' kept memory stays) */
    long freed = states_drop_caches ? states_drop_caches() : 0;
    if (nspire_link_log)
      nspire_link_log("drive: not enough memory to open it: the menus' caches given up (%ld bytes), "
                      "the driver holds %lu bytes; tried again", freed, nspire_drive_mem_held());
  }
  usbms_retry();
  state = DRIVE_ATTACHED;
  epoch++;
  nspire_padlog_add(PL_DRIVE, state, 0, 1, generation, epoch, 0);
#endif
}

/* Opens a drive that is attached (drive_open); ms17: one that could not be
 * opened for lack of memory is tried again, once each time, after the
 * menus' caches are given up (nspire_drive_retry): before a save or a load
 * that may open it. (A try short of memory sends the drive nothing.) */
static void open_now(void)
{
  drive_open();
  if (state == DRIVE_NOMEM && generation)
  {
    nomem_tries++;
    nspire_drive_retry();
    drive_open();
    if (nspire_link_log)
      nspire_link_log("drive: tried again after no memory (%u): %s", nomem_tries,
                      state == DRIVE_READY ? "opened" : usbms_status());
  }
}

unsigned nspire_drive_menu_tick(void)
{
  unsigned before = epoch;
  nspire_drive_poll();
  /* Menus allow the USB task to enumerate/detach. No recurring command
   * is sent to a drive after it has mounted. */
  uint32_t now = nspire_ticks();
  if (started && (uint32_t)(now - menu_at) >= 32768u / 20)
  {
    menu_at = now;
    nspire_os_sleep();
    nspire_drive_poll();
  }
  drive_open();
  return before != epoch;
}

static int name_compare(const void *a, const void *b)
{
  return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

int nspire_drive_list(const char *dir, char ***games, unsigned *ng, char ***dirs, unsigned *nd)
{
  usbms_dir_t *d = NULL;
  usbms_entry_t e;
  int r = -1;
  *games = *dirs = NULL;
  *ng = *nd = 0;
  if (!drive_live() || !(d = usbms_opendir(dir))) return -1;
  while ((r = usbms_readdir(d, &e)) == 1)
  {
    if (e.name[0] == '.' || !strcasecmp(e.name, "System Volume Information") ||
        (!e.is_dir && !nspire_drive_is_game(e.name))) continue;
    /* Reject rather than silently truncate an enormous/damaged listing. */
    if (*ng + *nd >= 4096) { r = -1; break; }
    char ***list = e.is_dir ? dirs : games;
    unsigned *count = e.is_dir ? nd : ng;
    char *name = strdup(e.name);
    char **grown = name ? realloc(*list, (*count + 1) * sizeof(**list)) : NULL;
    if (!grown) { free(name); r = -1; break; }
    *list = grown;
    (*list)[(*count)++] = name;
  }
  usbms_closedir(d);
  if (r < 0)
  {
    for (unsigned i = 0; i < *ng; i++) free((*games)[i]);
    for (unsigned i = 0; i < *nd; i++) free((*dirs)[i]);
    free(*games); free(*dirs);
    *games = *dirs = NULL; *ng = *nd = 0;
    return -1;
  }
  if (*ng) qsort(*games, *ng, sizeof(**games), name_compare);
  if (*nd) qsort(*dirs, *nd, sizeof(**dirs), name_compare);
  return 0;
}

#ifdef NSPIRE_HOST
typedef off64_t drive_offset;
#elif defined(__LARGE64_FILES)
typedef _off64_t drive_offset;
#else
typedef off_t drive_offset;
#endif

static ssize_t cookie_read(void *cookie, char *buf, size_t count)
{
  if (count > INT_MAX) count = INT_MAX;
  long got = usbms_read(cookie, buf, count);
  if (got < 0) errno = EIO;
  return got;
}

static int cookie_seek(void *cookie, drive_offset *offset, int whence)
{
  uint64_t size = usbms_size(cookie), at;
  if (whence == SEEK_SET) at = 0;
  else if (whence == SEEK_CUR) at = usbms_tell(cookie);
  else if (whence == SEEK_END) at = size;
  else { errno = EINVAL; return -1; }
  if ((int64_t)*offset < -(int64_t)at || (int64_t)*offset > (int64_t)size - (int64_t)at)
  { errno = EINVAL; return -1; }
  int64_t target = (int64_t)at + (int64_t)*offset;
  if (usbms_seek(cookie, target)) { errno = EIO; return -1; }
  *offset = (drive_offset)usbms_tell(cookie);
  return 0;
}

static int cookie_close(void *cookie) { usbms_close(cookie); return 0; }

FILE *nspire_drive_fopen(const char *path, const char *mode)
{
  if (!nspire_drive_path(path) || !mode || (strcmp(mode, "r") && strcmp(mode, "rb")))
  { errno = EACCES; return NULL; }
  /* A drive plugged back in (the same hub port or another) since the game
   * list was made is attached and not opened: only the drive's own page
   * opened one, so a USB game picked anywhere else ("Other games", the last
   * game) could not be read and was called "not a GBA ROM". */
  nspire_drive_poll();
  open_now();
  usbms_file_t *file = usbms_open(path + 4);
  if (!file) { errno = EIO; return NULL; }
  /* Newlib's fseek/ftell and the archive readers use signed 32-bit longs. */
  if (usbms_size(file) > INT32_MAX) { usbms_close(file); errno = EFBIG; return NULL; }
  cookie_io_functions_t io = { cookie_read, NULL, cookie_seek, cookie_close };
  FILE *stream = fopencookie(file, "rb", io);
  if (!stream) { usbms_close(file); return NULL; }
  setvbuf(stream, NULL, _IONBF, 0);
  return stream;
}

/* ---- Saves and states on the drive (2026-11-17, final/usbsave8) ---- */

#ifdef USBMS_WRITE_API
static int write_error;
/* The drive (its generation) that said it is write-protected: the switch's
 * bit, or a write it refused. Known without asking it again. */
static unsigned protected_gen;

/* After a call that may have found the drive gone or left it unusable. */
static void after_write(int failed)
{
  nspire_drive_poll();
  if (state == DRIVE_READY && usbms_poll() != USBMS_READY)
  {
    state = DRIVE_ERROR;
    epoch++;
  }
  else if (failed && state == DRIVE_READY)
    epoch++;   /* (the driver read the file system again: what was listed or opened before is stale) */
}

static int drive_error_of(int e)
{
  return e == USBMS_OK ? DRIVE_E_NONE : e == USBMS_E_GONE ? DRIVE_E_GONE :
         e == USBMS_E_PROTECTED ? DRIVE_E_PROTECTED : e == USBMS_E_FULL ? DRIVE_E_FULL : DRIVE_E_FAILED;
}

int nspire_drive_error(void) { return write_error; }

static int drive_ready(int may_open)
{
  if (!started)
  {
    write_error = DRIVE_E_GONE;
    return 0;
  }
  nspire_drive_poll();
  if (may_open)
    open_now();
  if (!drive_live())
  {
    write_error = state == DRIVE_NOMEM ? DRIVE_E_MEMORY : DRIVE_E_GONE;
    return 0;
  }
  if (protected_gen == generation || !usbms_writable())
  {
    after_write(0);
    if (state == DRIVE_READY)
      protected_gen = generation;
    write_error = state == DRIVE_READY ? DRIVE_E_PROTECTED : DRIVE_E_GONE;
    return 0;
  }
  return 1;
}

int nspire_drive_ready(void) { return drive_ready(1); }
int nspire_drive_ready_now(void) { return drive_ready(0); }

/* In, opened, and not known to be write-protected: nothing is asked of the
 * drive (no MODE SENSE: a drive that is only read gets none; the first
 * write, or nspire_drive_ready, asks). */
int nspire_drive_usable(int may_open)
{
  if (!started)
  {
    write_error = DRIVE_E_GONE;
    return 0;
  }
  nspire_drive_poll();
  if (may_open)
    open_now();
  if (!drive_live())
  {
    write_error = state == DRIVE_NOMEM ? DRIVE_E_MEMORY : DRIVE_E_GONE;
    return 0;
  }
  if (protected_gen == generation)
  {
    write_error = DRIVE_E_PROTECTED;
    return 0;
  }
  write_error = DRIVE_E_NONE;
  return 1;
}

int nspire_drive_put(const char *path, const void *data, unsigned long size)
{
  uint32_t t0 = nspire_ticks();
  int e;

  if (!nspire_drive_path(path) || !drive_live())
  {
    write_error = DRIVE_E_GONE;
    return 0;
  }
  e = usbms_put(path + 4, data, size);
  if (e == USBMS_E_NAME)
  {
    /* its folder is not there yet: made, with the ones above it */
    char dir[300];
    const char *name = path + 4, *slash = strrchr(name, '/');
    if (slash && slash > name && (size_t)(slash - name) < sizeof(dir))
    {
      memcpy(dir, name, (size_t)(slash - name));
      dir[slash - name] = 0;
      e = usbms_mkdir(dir);
      if (e == USBMS_OK)
        e = usbms_put(name, data, size);
    }
  }
  write_error = drive_error_of(e);
  if (e == USBMS_E_PROTECTED)
    protected_gen = generation;
  /* (no drive was written on a calculator yet: what each write took, and how it ended, in the link's log) */
  if (nspire_link_log)
    nspire_link_log("drive: %s, %lu bytes: %s, %u ms", path + 4, size, e == USBMS_OK ? "written" : usbms_strerror(e),
                    (unsigned)((uint64_t)(nspire_ticks() - t0) * 1000 / NSPIRE_TICK_HZ));
#ifdef USBMS_HAS_MEM_HELD
  {
    /* ms17: no memory for the driver's write buffer: written all the same, block by block (slower) */
    static uint32_t unbuffered_seen;
    usbms_info_t info;
    usbms_info(&info);
    if (info.unbuffered != unbuffered_seen && nspire_link_log)
      nspire_link_log("drive: no memory for the driver's 64 KB write buffer (%u times): written unbuffered, slower",
                      (unsigned)(info.unbuffered - unbuffered_seen));
    unbuffered_seen = info.unbuffered;
  }
#endif
  after_write(e == USBMS_E_IO || e == USBMS_E_GONE);
  return e == USBMS_OK;
}

static int settle(const char *path, int keep_new)
{
  int e;
  if (!nspire_drive_path(path) || !drive_live())
  {
    write_error = DRIVE_E_GONE;
    return 0;
  }
  e = keep_new ? usbms_settle(path + 4) : usbms_discard(path + 4);
  write_error = drive_error_of(e);
  if (e == USBMS_E_PROTECTED)
    protected_gen = generation;
  after_write(e == USBMS_E_IO || e == USBMS_E_GONE);
  return e == USBMS_OK;
}

int nspire_drive_settle(const char *path)
{
  return settle(path, 1);
}

int nspire_drive_discard(const char *path)
{
  return settle(path, 0);
}

/* save17: into the caller's buffer (cap bytes), errno saying why not */
int nspire_drive_get_into(const char *path, void *buf, unsigned long cap, unsigned long *size)
{
  usbms_file_t *file;
  uint64_t n;
  unsigned long at = 0;

  *size = 0;
  if (!nspire_drive_path(path) || !drive_live() || !(file = usbms_open(path + 4)))
  {
    errno = ENOENT;
    return 0;
  }
  n = usbms_size(file);
  if (n > cap)
  {
    usbms_close(file);
    errno = EFBIG;
    return 0;
  }
  while (at < n)
  {
    long got = usbms_read(file, (unsigned char *)buf + at, (unsigned long)(n - at));
    if (got <= 0)
      break;
    at += (unsigned long)got;
  }
  usbms_close(file);
  if (at < n)
  {
    errno = EIO;
    return 0;
  }
  *size = (unsigned long)n;
  return 1;
}

void *nspire_drive_get(const char *path, unsigned long limit, unsigned long *size)
{
  unsigned long n = 0;
  unsigned char *data;

  *size = 0;
  if (!nspire_drive_stat(path, &n, NULL))
  {
    errno = ENOENT;
    return NULL;
  }
  if (n > limit)
  {
    errno = EFBIG;
    return NULL;
  }
  data = malloc(n ? (size_t)n : 1);
  if (!data)
  {
    errno = ENOMEM;   /* (save17: memory, told apart from the drive) */
    return NULL;
  }
  if (!nspire_drive_get_into(path, data, n, size))
  {
    int err = errno;
    free(data);
    errno = err;
    return NULL;
  }
  return data;
}

int nspire_drive_remove(const char *path)
{
  int e;
  if (!nspire_drive_path(path) || !drive_live())
  {
    write_error = DRIVE_E_GONE;
    return 0;
  }
  e = usbms_remove(path + 4);
  write_error = drive_error_of(e);
  after_write(e == USBMS_E_IO || e == USBMS_E_GONE);
  return e == USBMS_OK;
}

int nspire_drive_rename(const char *from, const char *to)
{
  int e;
  if (!nspire_drive_path(from) || !nspire_drive_path(to) || !drive_live())
  {
    write_error = DRIVE_E_GONE;
    return 0;
  }
  e = usbms_rename(from + 4, to + 4);
  write_error = drive_error_of(e);
  after_write(e == USBMS_E_IO || e == USBMS_E_GONE);
  return e == USBMS_OK;
}

int nspire_drive_stat(const char *path, unsigned long *size, unsigned long *mtime)
{
  usbms_entry_t e;
  if (!nspire_drive_path(path) || !drive_live() || usbms_stat(path + 4, &e) != USBMS_OK || e.is_dir)
    return 0;
  if (size)
    *size = (unsigned long)e.size;
  if (mtime)
    *mtime = e.mtime;
  return 1;
}

int nspire_drive_dir(const char *dir,
                     int (*each)(const char *name, unsigned long size, unsigned long mtime, void *arg), void *arg)
{
  usbms_dir_t *d;
  usbms_entry_t e;
  int r;
  if (!nspire_drive_path(dir) || !drive_live())
    return -1;
  if (!(d = usbms_opendir(dir + 4)))
    return usbms_stat(dir + 4, &e) == USBMS_E_NOENT ? 0 : -1;   /* (no such folder yet: no names) */
  while ((r = usbms_readdir(d, &e)) == 1)
    if (!e.is_dir && !each(e.name, (unsigned long)e.size, e.mtime, arg))
    {
      r = 0;
      break;
    }
  usbms_closedir(d);
  return r < 0 ? -1 : 0;
}
#else
/* (a nspire-usbms without the writing calls: games are read, saves stay on the calculator) */
int nspire_drive_error(void) { return DRIVE_E_GONE; }
int nspire_drive_ready(void) { return 0; }
int nspire_drive_ready_now(void) { return 0; }
int nspire_drive_usable(int may_open) { (void)may_open; return 0; }
int nspire_drive_put(const char *p, const void *d, unsigned long n) { (void)p; (void)d; (void)n; return 0; }
int nspire_drive_settle(const char *p) { (void)p; return 0; }
int nspire_drive_discard(const char *p) { (void)p; return 0; }
void *nspire_drive_get(const char *p, unsigned long l, unsigned long *n) { (void)p; (void)l; *n = 0; return NULL; }
int nspire_drive_get_into(const char *p, void *b, unsigned long c, unsigned long *n)
{ (void)p; (void)b; (void)c; *n = 0; errno = ENOENT; return 0; }
int nspire_drive_remove(const char *p) { (void)p; return 0; }
int nspire_drive_rename(const char *f, const char *t) { (void)f; (void)t; return 0; }
int nspire_drive_stat(const char *p, unsigned long *s, unsigned long *m) { (void)p; (void)s; (void)m; return 0; }
int nspire_drive_dir(const char *d, int (*each)(const char *, unsigned long, unsigned long, void *), void *a)
{ (void)d; (void)each; (void)a; return -1; }
#endif

void nspire_drive_game_begin(const char *path)
{
  game_from_drive = nspire_drive_path(path);
  resident = removed_note = 0;
}
void nspire_drive_game_end(int loaded) { resident = game_from_drive && loaded; }
const char *nspire_drive_load_error(const char *reason)
{
  if (game_from_drive && !nspire_usbms_active())
    return "The USB drive was removed while the game was loading. Replug it and try again.";
  if (game_from_drive && nspire_rom_load_need_mb)
    return "This USB game must fit entirely in memory. Close open documents, or copy it to the calculator and try again.";
  return reason ? reason : "Couldn't read this game. Is it a GBA ROM?";
}

#ifdef USBMS_HAS_MEM_HELD
unsigned long nspire_drive_mem_held(void) { return (unsigned long)usbms_mem_held(); }
/* What a drive takes from its opening to its first save (usbms.h): the read cache 4 x 64 KB, the
 * transfer memory 64 KB, the write buffer 64 KB and a first file's handle (FatFs's FIL with its 4 KB
 * sector buffer and its cluster map: ~4.6 KB, 5 here): 389 KB. */
unsigned long nspire_drive_open_room(void) { return started && state != DRIVE_READY ? 389ul * 1024 : 0; }
void nspire_drive_trim(void) { usbms_trim(); }
#else
/* (an older nspire-usbms: its memory was taken at its start, before any of this) */
unsigned long nspire_drive_mem_held(void) { return 0; }
unsigned long nspire_drive_open_room(void) { return 0; }
void nspire_drive_trim(void) {}
#endif

#else
#ifdef NSPIRE_HOST
int host_usbms_active(void) __attribute__((weak));
int nspire_usbms_active(void) { return host_usbms_active && host_usbms_active(); }
#endif
int nspire_drive_enabled(void) { return 0; }
void nspire_drive_start(void) {}
void nspire_drive_stop(void) {}
int nspire_drive_stop_within(unsigned ms) { (void)ms; return 0; }
int nspire_drive_stop_quiet(void) { return 0; }
void nspire_drive_poll(void) {}
void nspire_drive_frame(void) {}
unsigned nspire_drive_menu_tick(void) { return 0; }
void nspire_drive_retry(void) {}
enum drive_state nspire_drive_state(void) { return DRIVE_OFF; }
unsigned nspire_drive_epoch(void) { return 0; }
const char *nspire_drive_summary(void) { return "USB drives not enabled"; }
int nspire_drive_list(const char *p, char ***g, unsigned *ng, char ***d, unsigned *nd)
{ (void)p; *g = *d = NULL; *ng = *nd = 0; return -1; }
FILE *nspire_drive_fopen(const char *p, const char *m) { (void)p; (void)m; return NULL; }
void nspire_drive_game_begin(const char *p) { (void)p; }
void nspire_drive_game_end(int loaded) { (void)loaded; }
const char *nspire_drive_load_error(const char *r) { return r ? r : "Couldn't read this game."; }
unsigned long nspire_drive_mem_held(void) { return 0; }
unsigned long nspire_drive_open_room(void) { return 0; }
void nspire_drive_trim(void) {}
int nspire_drive_error(void) { return DRIVE_E_GONE; }
int nspire_drive_ready(void) { return 0; }
int nspire_drive_ready_now(void) { return 0; }
int nspire_drive_usable(int may_open) { (void)may_open; return 0; }
int nspire_drive_put(const char *p, const void *d, unsigned long n) { (void)p; (void)d; (void)n; return 0; }
int nspire_drive_settle(const char *p) { (void)p; return 0; }
int nspire_drive_discard(const char *p) { (void)p; return 0; }
void *nspire_drive_get(const char *p, unsigned long l, unsigned long *n) { (void)p; (void)l; *n = 0; return NULL; }
int nspire_drive_get_into(const char *p, void *b, unsigned long c, unsigned long *n)
{ (void)p; (void)b; (void)c; *n = 0; errno = ENOENT; return 0; }
int nspire_drive_remove(const char *p) { (void)p; return 0; }
int nspire_drive_rename(const char *f, const char *t) { (void)f; (void)t; return 0; }
int nspire_drive_stat(const char *p, unsigned long *s, unsigned long *m) { (void)p; (void)s; (void)m; return 0; }
int nspire_drive_dir(const char *d, int (*each)(const char *, unsigned long, unsigned long, void *), void *a)
{ (void)d; (void)each; (void)a; return -1; }
#endif
