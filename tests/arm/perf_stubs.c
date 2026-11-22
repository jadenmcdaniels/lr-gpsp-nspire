/* What gpSP's core and picture output need from the calculator frontend,
 * for the speed harness (perf.c): core_stubs.c's stand-ins, plus screen
 * buffers that take turns as nspire/nspire.c's do. */

#include "common.h"
#include "nspire.h"
#include "nspire_os.h"

#include <sys/stat.h>

void nspire_jit_cache_sync(void *base, void *end)
{
  (void)base;
  (void)end;
}

void nspire_fps_overlay_reset(void) {}
void nspire_backup_mark_dirty(void) {}
u32 nspire_ticks(void) { return 0; }
u32 nspire_os_long_sleeps;   /* (nspire_pictures.c: the OS's long waits, none here) */
void states_timing_note(const char *fmt, ...) { (void)fmt; }
u32 nspire_render_ticks;
u32 nspire_perf_on;

int mkdir(const char *path, mode_t mode)
{
  (void)path;
  (void)mode;
  return -1;
}

static u32 old_key;

bool input_check_savestate(const u8 *src)
{
  const u8 *p = bson_find_key(src, "input");
  return (p && bson_contains_key(p, "prevkey", BSON_TYPE_INT32));
}

bool input_read_savestate(const u8 *src)
{
  const u8 *p = bson_find_key(src, "input");
  if (p)
    return bson_read_int32(p, "prevkey", &old_key);
  return false;
}

unsigned input_write_savestate(u8 *dst)
{
  u8 *wbptr1, *startp = dst;
  bson_start_document(dst, "input", wbptr1);
  bson_write_int32(dst, "prevkey", old_key);
  bson_finish_document(dst, wbptr1);
  return (unsigned int)(dst - startp);
}

/* The screen buffers, as on the calculator: never read by the CPU, so
 * never in its data cache. */
#define SCREEN_PIXELS (320 * 240)
#define SCREENS 3
static u16 screens[SCREENS][SCREEN_PIXELS] __attribute__((aligned(32)));
static int drawing_screen = 1;

void *nspire_screen = screens[1];
void *nspire_displayed_screen = screens[0];
u16 nspire_landscape[SCREEN_PIXELS] __attribute__((aligned(32)));
int nspire_lcd_portrait = 1;

int nspire_frameskip_before_flip(void) { return 1; }
/* (nspire_frameskip.c: the OS's tick as it comes; nothing is watched here.)
 * speed9a: oswatch=1 osclobber=1 keeps it armed and
 * each call trashes what a C call may (r0-r3, r12, the flags), as the OS's
 * sleep would: the lines reach it from update_gba and from the dynarec's
 * store handlers (a store that makes the waiting lines drawn), and the
 * frames must be the same (tests/arm/run.sh). */
int perf_os_clobber;
/* nspire_lines.c's inline check reads the tick timer's count through this
 * (nspire_os.h): oswatch=1 alone keeps it armed and never near (21 against
 * 2: the armed lines' cost: a line handed over loads and compares inline, a
 * line drawn calls nspire_os_watch_line, which returns after the same
 * compare); osclobber=1 near on every line (the call goes on) */
uint32_t perf_tt_count = 21;
struct nspire_os_watch nspire_os_watch_at = { &perf_tt_count, 2 };
void nspire_os_watch_line(void)
{
  if (*nspire_os_watch_at.reg - 1u >= nspire_os_watch_at.value)
    return;                            /* (nspire_frameskip.c's first test: not near; a line drawn) */
  if (!perf_os_clobber)
  {
    nspire_os_watching = 0;
    return;
  }
  __asm__ volatile("mov r0, #0x5a\n\tmvn r1, #0\n\tmov r2, #0x80000000\n\tmov r3, #0x3c\n\tmov r12, #0xc3\n\t"
                   "msr cpsr_f, #0xf0000000" ::: "r0", "r1", "r2", "r3", "r12", "cc", "memory");
}

void nspire_flip(void)
{
  nspire_displayed_screen = nspire_screen;
  drawing_screen = (drawing_screen + 1) % SCREENS;
  nspire_screen = screens[drawing_screen];
}

int nspire_screen_index(void)
{
  return drawing_screen;
}

void update_at_vblank(void) {}

/* The picture output's bands ask whether a link is up and a controller is in: neither is. */
const char *nspire_link_status(void) { return ""; }
const char *nspire_usbpad_name(void) { return 0; }
