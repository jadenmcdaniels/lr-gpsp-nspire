/* USB gamepads in gpSP: nspire_usbpad_glue.h.
 *
 * nspire-usbpad's driver (Backend A) plugs into the calculator OS's own USB
 * host stack; its callbacks run in the OS's USB task and hand reports over
 * through its own buffers. Everything here runs in gpSP's main task, from
 * the keys layer (nspire_input.c), so it may use the C library and Ndless.
 *
 * The OS's USB task has a lower priority than gpSP's, which never waits on
 * the OS and runs with interrupts masked, so it runs only while gpSP sleeps
 * an OS tick (nspire_os.h). The driver says when that's needed
 * (USBPAD_WANT_OS: a pad attaching, about every 0.5 s while something that
 * isn't a pad yet is on the port, and while a pad is in: every poll with
 * os_time on, its reports come only then): then this sets its bit with
 * nspire_os_want(), so frames sleep in their spare time, and sleeps a tick
 * itself if none came for SLEEP_GAP (about 2 frames).
 *
 * The driver asks for time only once in 30 polls until the OS offers it a
 * device, but the OS's hub work before that (debounce, reset, the adapter's
 * VBUS: delays counted in its ticks, which with interrupts masked move a
 * tick or two a sleep) needs it more: for ATTACH_POLLS after the port's
 * bits change (and after the start), while something's at the host's end
 * of the port and no pad's in, it's wanted on every poll too. The menus and
 * the file list pace without sleeping, so these sleeps are all the OS gets
 * there.
 *
 * UNVERIFIED (nothing here has run on a calculator):
 * - that a tick every ~2 frames for ATTACH_POLLS is enough for the OS to
 *   attach a pad;
 * - how much the per-frame OS tick costs in heavy games: gpSP masks
 *   interrupts, so a pad's reports come only in the OS's ticks (gate G2 in
 *   nspire-usbpad's docs, answered by gpSP's link log): os_time=1 by
 *   default, every frame's spare time plus a tick at least every ~2 frames;
 * - the ID pin reading of usb_port (below), as nspire_os.c reads it;
 * - that the OS's idle power-down doesn't count on while it gets those
 *   ticks (gpSP reads the keys itself, so the OS sees no key presses): a
 *   pad-only game longer than its 3 minutes, watching for the screen
 *   dimming, a power-down or the OS's busy cursor, is the test;
 * - that usbpad_shutdown()'s pipe aborts and closes need the interrupts
 *   (stop() turns them on around it, gpSP's hardware put back by then).
 *
 * The pad settings are a small text file of their own in the saves folder
 * (SETTINGS_FILE), not in gpSP's settings, whose size is checked when read:
 * written with the defaults when the first pad connects, if it isn't there. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nspire_usbpad_glue.h"
#include "nspire_bt_glue.h"
#ifndef NSPIRE_NO_USBPAD
#include "usbpad.h"
#endif

/* A controller's kind (usbpad.h's USBPAD_KIND_*: USB or Bluetooth) as the
 * Controls screen's family of button names. */
static int bt_family(int kind)
{
#ifdef NSPIRE_NO_USBPAD
  (void)kind;
  return PAD_FAMILY_NONE;
#else
  switch (kind)
  {
  case USBPAD_KIND_XINPUT: return PAD_FAMILY_XBOX360;
  case USBPAD_KIND_GIP: return PAD_FAMILY_XBOX;
  case USBPAD_KIND_DS4: return PAD_FAMILY_DS4;
  case USBPAD_KIND_DUALSENSE: return PAD_FAMILY_DUALSENSE;
  case USBPAD_KIND_SWITCHPRO: return PAD_FAMILY_SWITCH;
  default: return PAD_FAMILY_NONE;
  }
#endif
}

#ifdef NSPIRE_NO_USBPAD

void nspire_usbpad_start(void) {}
uint32_t nspire_usbpad_keys(void) { return 0; }
void nspire_usbpad_frame(uint32_t hotkeys) { (void)hotkeys; }
/* (the Bluetooth pads stand in on the Controls screen) */
const char *nspire_usbpad_name(void) { return nspire_bt_glue_pad_name(); }
int nspire_usbpad_family(void) { return bt_family(nspire_bt_glue_pad_kind()); }
int nspire_usbpad_labels(void) { return 0; }
int nspire_usbpad_pads(void) { return 0; }
unsigned nspire_usbpad_slots(void) { return 0; }
const char *nspire_usbpad_slot_name(int slot) { (void)slot; return NULL; }
int nspire_usbpad_layout_labels(uint16_t vid, uint16_t pid) { (void)vid; (void)pid; return 0; }
void nspire_usbpad_menu_flush(void) {}
void nspire_usbpad_exit_log(void) {}
int nspire_usbpad_stop_within(unsigned ms) { (void)ms; return 0; }
int nspire_usbpad_exit_untouched(void) { return 0; }   /* (no driver to count offers: the usual stops) */
int nspire_usbhub_present(void) { return 0; }
int nspire_usbhub_busy(void) { return 0; }
int nspire_usbhub_recovering(void) { return 0; }
int nspire_usbhub_doubtful(void) { return 0; }
int nspire_hub_recovering(void) { return 0; }
int nspire_usbpad_os_stuck(void) { return 0; }
int nspire_usbpad_os_dead(void) { return 0; }
int nspire_usbpad_other_device(void) { return 0; }

#else

#include "common.h"
#include <stdarg.h>
#include "nspire.h"
#include "nspire_frontend.h"
#include "nspire_keys.h"
#include "nspire_link.h"
#include "nspire_os.h"
#include "nspire_exit.h"
/* (r19-usb) the exit's trail (nspire_exit.c; weak: padsim's build has none) */
void nspire_exit_trail(const char *what) __attribute__((weak));
#include "nspire_usbport.h"
#include "nspire_arbiter.h"
#include "nspire_paths.h"
#include "nspire_padlog.h"
#include "nspire_padstatus.h"
#include "nspire_ui.h"
#include "nspire_audio.h"
#include "nspire_frameskip.h"
#include "usbpad.h"
#include "nspire_usbpad.h"
#include "src/backend_os.h"   /* usbpad_os_set_config: os_time */

#define SETTINGS_FILE "gpsp_usbpad.cfg.tns"
/* The driver counts its timeouts in polls, about 60 a second: no more than
 * 80 (some loops read the keys as fast as they can). */
#define POLLS_PER_SEC 80
#define POLL_GAP      (NSPIRE_TICK_HZ / POLLS_PER_SEC)
/* While the OS is wanted: a tick's sleep at least every ~2 frames when the
 * frames have no spare time (or none at all: the menus), or a pad's input
 * would stall (its reports come only while the OS runs) and its attach
 * would crawl. */
#define SLEEP_GAP     (NSPIRE_TICK_HZ / 30)
/* After the port's bits change: the OS wanted on every poll this long
 * (12 s at 80 polls a second, 15 s at 60), while no pad's in. */
#define ATTACH_POLLS  (POLLS_PER_SEC * 12)
#define TOAST_GAP     NSPIRE_TICK_HZ
/* How long gpSP's own messages stay (nspire_video_present.c): the toasts
 * wait that long where one may have just been posted. */
#define MESSAGE_TIME  (NSPIRE_TICK_HZ * 2)
#define LAYOUTS       8
#define QUEUE         8

/* The fifth key word is usbpad.h's bits as they are. */
typedef char pad_keys_are_usbpad_bits[(1u << (KEY_PAD_B - KEY_PAD_FIRST)) == USBPAD_SOUTH &&
                                      (1u << (KEY_PAD_A - KEY_PAD_FIRST)) == USBPAD_EAST &&
                                      (1u << (KEY_PAD_Y - KEY_PAD_FIRST)) == USBPAD_WEST &&
                                      (1u << (KEY_PAD_X - KEY_PAD_FIRST)) == USBPAD_NORTH &&
                                      (1u << (KEY_PAD_SELECT - KEY_PAD_FIRST)) == USBPAD_SELECT &&
                                      (1u << (KEY_PAD_HOME - KEY_PAD_FIRST)) == USBPAD_HOME &&
                                      (1u << (KEY_PAD_AUX - KEY_PAD_FIRST)) == USBPAD_AUX &&
                                      KEY_PAD_FIRST + USBPAD_NBUTTONS <= NUM_KEYS + 1 ? 1 : -1];

static int started, polled, toasted, holding, settings_missing;
static u32 last_poll, toast_at, hold_until, keys, keys_calls;
/* Without a timer: the RTC second, and the keys reads and polls in it. */
static u32 rtc_at, rtc_calls, rtc_polls, stride = 1, skipped;
/* The port's bits as last seen (nspire_usb_bits), and the polls left of
 * the time after they changed (ATTACH_POLLS). */
static u32 port_bits, attach_left;
static int attach_quiet;  /* ...without asking for the OS's time (the OS is done with the port: cu_poll) */
static int device_seen;   /* a pad or a driver event since the port last changed */
static int events_now;    /* events taken in this poll (newer than a port change seen in it) */
static int gone_now;      /* hub: ...a pad's DISCONNECTED among them */
static int gone_poll;     /* ...and the same for this poll's whole length (attaching(): hub_poll clears gone_now) */
static int os_stuck;      /* the OS's USB has stopped (watch_stuck), until the adapter comes out or an offer */
/* (2026-11-17) the OS's sleep of what nobody uses (watch_awake, watch_phy) */
static int awake_asked;   /* usbpad_os_awake(1) was called (the adapter was seen once) */
static int awake_bits;    /* ...and what it returned: 7 the OS's USB is kept awake, -1 not the known OS */
static int phy_cycle_due; /* a device waits on the transceiver we woke, or is held suspended: the come-up's restart */
static int asleep_noted;  /* the bus suspended under what is plugged in: said once a plug */
static u32 phy_wakes;
static int os_dead;       /* ...and nothing has been offered since (the adapter out doesn't clear this: quit's restart) */
static long matches_mark; /* SEAM-8: the OS's match offers (usbpad_diag) at the start or when the device last left */
/* PAD3: what the Controller page and the pad log say about the driver: the
 * last driver event since the device came, the 12 s wait ran out, the
 * port's connected bit and the wires' counter at the last poll (a device
 * leaving or coming where the bits stay), the status poll's time. */
static struct usbpad_event last_ev;
static int last_ev_set, waited_out;
/* R11u (2026-11-15, driver USBPAD_OS_HAS_NOT_PAD): the device on the port
 * is not a controller (the driver kept an interface of it quietly: a sound
 * card's volume keys, a keyboard): not a failure. Since the device came:
 * it was seen (not_pad_now), when, its ids, whether its try was ended. */
static int not_pad_now, not_pad_done, taken_since;   /* taken_since: an offer of it scored > 0 */
static int iface_offered;   /* an interface of it was offered (not only the device level) */
static u32 not_pad_at, not_pads_seen;
static uint16_t not_pad_vid, not_pad_pid;
static uint8_t not_pad_if;
/* The rest of the device's interfaces come in the same enumeration: a pad
 * interface after a non-pad one still gets the OS's time this long. */
#define NOT_PAD_SETTLE_MS 1000

/* The device on the port is not a controller and the rest of its
 * interfaces have had their offers: its try is over, nothing failed. */
static int not_pad_settled(void)
{
  return not_pad_now && !usbpad_count() &&
         (u32)((u64)(nspire_ticks() - not_pad_at) * 1000 / NSPIRE_TICK_HZ) >= NOT_PAD_SETTLE_MS;
}
static u32 wires_seen, status_at, seen_detaches;
static int status_polled;
static enum pad_verdict verdict_was = PV_NOTHING;
static u32 offer_count(void);
/* S10: per slot, whether its first report was logged, and the buttons seen
 * on this attach (each new button logged once: at most 18 lines a pad). */
static u32 reports_logged, buttons_seen[USBPAD_PLAYERS];
/* (r19-usb) A pad attached that sends nothing. A CX log (r19, 926fa73): an
 * 8BitDo 2dc8:310b behind a hub
 * attached at 230416 ms with its OUT pipe's open timed out ("out_open ep05: OS
 * 12 timed out", its LED packet "transfer failed (-1)"), the page and the
 * pad log said "working: The controller works." for 23 s, and its detach
 * said "0 reports" (its IN transfer still queued, never called back). So:
 * PAD_SILENT_MS after a pad's attach with no report from it yet, the pad log
 * says so once (a milestone: the bring-up lines above it say how far it got)
 * and the verdict is PV_SILENT, not PV_WORKING, until its first report.
 * pads_on: the slots seen connected; pad_on_at: since when; silent_said. */
#define PAD_SILENT_MS 3000
static u32 pads_on, silent_said, pad_on_at[USBPAD_PLAYERS];
/* The driver's events not shown yet (toasts come in the game only). */
static struct usbpad_event queue[QUEUE];
static unsigned queued;

/* ---- The settings file ---- */

static struct usbpad_config config;
/* The driver's want_live: the OS gets time while a pad is in. On (1) by
 * default: gpSP runs with interrupts masked (the CPSR's I bit; gpSP's
 * link log, 2026-11-08: "OS sleep: none (interrupts are off)"), so a
 * pad's reports reach us only while the OS runs, in nspire_os_sleep()'s
 * tick. os_time=0 in the settings file goes back to once in 30 polls. */
static int os_time = 1;
static int labels_all;       /* layout=labels */
static struct
{
  unsigned vid, pid;
  int labels;
} layouts[LAYOUTS];
static unsigned num_layouts;

static int is_labels(const char *value)
{
  while (*value == ' ')
    value++;
  return !strncmp(value, "labels", 6);
}

static u8 percent(const char *value)
{
  long n = strtol(value, NULL, 10);
  return (u8)(n < 0 ? 0 : n > 255 ? 255 : n);   /* out of range: usbpad_set_config refuses it */
}

/* "045e:028e" (1 to 4 hex digits each, as sscanf's "%4x:%4x" took them):
 * 1 and the two numbers, else 0. R6b: sscanf's code was 16 KB of the .tns. */
static int hex4(const char **p, unsigned *out)
{
  unsigned v = 0, n = 0;
  int d;

  while (**p == ' ' || **p == '\t')
    (*p)++;
  for (; n < 4; n++, (*p)++)
  {
    char c = **p;
    d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (d < 0)
      break;
    v = v * 16 + (unsigned)d;
  }
  *out = v;
  return n > 0;
}

static int vid_pid(const char *line, unsigned *vid, unsigned *pid)
{
  const char *p = line;
  if (!hex4(&p, vid) || *p++ != ':')
    return 0;
  return hex4(&p, pid);
}

static void read_settings(const char *path)
{
  char line[120];
  FILE *f = fopen(path, "r");

  if (!f)
  {
    settings_missing = 1;
    return;
  }
  while (fgets(line, sizeof(line), f))
  {
    char *value = strchr(line, '=');
    unsigned vid, pid;

    if (line[0] == '#' || !value)
      continue;
    *value++ = 0;
    if (!strcmp(line, "stick_dpad"))
      config.stick_dpad = strtol(value, NULL, 10) != 0;
    else if (!strcmp(line, "press_at"))
      config.on_pct = percent(value);
    else if (!strcmp(line, "release_at"))
      config.off_pct = percent(value);
    else if (!strcmp(line, "os_time"))
      os_time = strtol(value, NULL, 10) != 0;
    else if (!strcmp(line, "layout"))
      labels_all = is_labels(value);
    else if (vid_pid(line, &vid, &pid) && num_layouts < LAYOUTS)
    {
      layouts[num_layouts].vid = vid;
      layouts[num_layouts].pid = pid;
      layouts[num_layouts++].labels = is_labels(value);
    }
  }
  fclose(f);
}

/* The defaults, with the first pad's own USB id in the example. */
static void write_settings(const char *path, const struct usbpad_state *pad)
{
  FILE *f = fopen(path, "w");

  if (!f)
    return;
  fprintf(f,
          "# gpSP: USB controller settings. Delete this file to get the defaults back.\n"
          "# The left stick moves like the D-pad: 1 yes, 0 no.\n"
          "stick_dpad=%u\n"
          "# How far (%% of the way) the stick or a trigger goes before it counts as\n"
          "# pressed, and comes back before it counts as let go.\n"
          "press_at=%u\n"
          "release_at=%u\n"
          "# Which face buttons are Pad A and Pad B (the GBA's A and B, unless bound to\n"
          "# something in Configure input): position (A the right one, B the bottom one,\n"
          "# as on a Nintendo pad) or labels (A the bottom one, B the right one, as on an\n"
          "# Xbox pad). For every controller:\n"
          "layout=%s\n"
          "# or for one, by its USB id (vendor:product), like the one plugged in first:\n"
          "#%04x:%04x=labels\n"
          "# 1: the OS gets each frame's spare time while a controller is in, as its\n"
          "# buttons come only then. 0: twice a second (a controller's buttons then come\n"
          "# late), for games too slow with it on (the OS's own keys and power-down work\n"
          "# during games while it's on).\n"
          "os_time=%d\n",
          (unsigned)config.stick_dpad, (unsigned)config.on_pct, (unsigned)config.off_pct,
          labels_all ? "labels" : "position", (unsigned)pad->vid, (unsigned)pad->pid, os_time);
  fclose(f);
}

/* ---- The driver ---- */

/* usbpad_set_hooks' sleep: shutdown waits in it for a USB callback that is
 * half-way through. */
static void sleep_tick(void)
{
  nspire_os_sleep();
}

/* usbpad_set_hooks' port check, 1 only for the end of a cable a pad can be
 * on: something on the port (nspire_usb_plugged) at the host's end, the
 * ID pin grounded (an OTG adapter, or the A end of a calculator-to-calculator
 * cable, where the OS offers us the other calculator). The device's end (a
 * computer, the other calculator's A end) is 0: no OS time asked for there,
 * and no "Plug the controller in again" while charging from a computer.
 * The ID bit is nspire_usb_bits' (FOTG210 OTGCSR bit 21 as its bit 5, or
 * ChipIdea OTGSC bit 8 as its bit 0: 1 the device's end). UNVERIFIED. */
static int usb_port(void)
{
  int plugged = nspire_usb_plugged();
  u32 bits;

  if (plugged <= 0)
    return plugged;
  bits = nspire_usb_bits();
  if (bits & 0x20000000u)
    return !(bits & 0x20u);
  if (bits & 0x10000000u)
    return !(bits & 0x01u);
  return -1;
}

/* ---- The pad lines of the link's log (gpsp_link.txt.tns) ----
 *
 * One file says what the port, the OS and the driver did: the lines go
 * through nspire_link_log, each asking for the file to be written at the
 * next frame's start (never here: this runs in the game's frame).
 * The start-up lines (the driver's init, its status) wait until something
 * happens: a game played with nothing on the port leaves the last log
 * alone (nspire_link.c). */
static int logged, started_logged, init_result, forced_ticks, polls_taken;
/* R1: the longest wait without an OS sleep a pad poll saw while the OS was
 * wanted (timer ticks; the heartbeat prints it and starts over): a pad's
 * reports come only while the OS runs, so this is its input's worst wait. */
static u32 port_changed_at;
/* Pad lines not in the file yet, and when the menus last wrote it. */
static int log_pending;
static u32 menu_flushed_at;
static unsigned menu_flushes;

static void log_startup(void);
static void cu_point(int in_game);
static void hub_point(int in_game);
static void cu_view(struct pad_driver_view *d);

static void plog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void plog(const char *fmt, ...)
{
  static char line[480];   /* (a USB controller line is long; nspire_link.c's lines go to 520) */
  va_list ap;

  if (!started_logged)
  {
    started_logged = 1;
    log_startup();
  }
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  nspire_link_log("pad: %s", line);
  nspire_link_log_request_flush();
  logged = 1;
  log_pending = 1;
}

/* The menus and the file list never reach a game frame's start, where the
 * log is written (nspire_link.c): from get_gui_input a pad line is written
 * here instead, at most once in MENU_FLUSH_MS and MENU_FLUSHES times a
 * run (a write rewrites the file: ms on a calculator), so a reset or hang
 * in the OS's match/attach/first report while only the menus run leaves the
 * lines up to it (RDY-4). Never in a game. */
#define MENU_FLUSH_MS 2000
#define MENU_FLUSHES  30

void nspire_usbpad_menu_flush(void)
{
  u32 now;

  cu_point(0);                   /* PAD4: the come-up's OS time in one go, in the menus at once */
  hub_point(0);                  /* hub: and a hub's device's */
  nspire_padlog_write_point();   /* PAD3: its own file, its own limits */
  /* Never while the link is up or coming up (as log_live_policy: a write is
   * the other side's wait). */
  if (!log_pending || menu_flushes >= MENU_FLUSHES || nspire_link_state() != LINK_OFF)
    return;
  now = nspire_ticks();
  if (menu_flushes && now - menu_flushed_at < MENU_FLUSH_MS * (NSPIRE_TICK_HZ / 1000))
    return;
  menu_flushed_at = now;
  menu_flushes++;
  log_pending = 0;
  nspire_link_log_flush();
}

/* usbpad_diag's text, a line each. level 0: the counters of what the OS
 * offered and the driver did, and the last attach (the first three lines);
 * 1: also what the OS offered (each match call and the answer), the closes,
 * the first report; 2: also the addresses it found. */
static void log_diag(const char *when, int level)
{
  static char text[3200];   /* the driver's match-fix lines (offer, dump) fit */
  char *line = text, *end;
  int n = 0;

  usbpad_diag(text, sizeof(text));
  while (*line && (level >= 2 || (level == 1 && strncmp(line, "addresses:", 10)) || n < 3))
  {
    end = strchr(line, '\n');
    if (end)
      *end = 0;
    plog("diag (%s): %s", when, line);
    n++;
    if (!end)
      break;
    line = end + 1;
  }
  if (!n)
    plog("diag (%s): none (the driver never started)", when);
}

static void log_startup(void)
{
  plog("usbpad_init -> %d (%s); os_time %d; USB port %08x", init_result, usbpad_status(), os_time,
       (unsigned)nspire_usb_bits());
  log_diag("at start", 2);
}

/* ---- R11w (2026-11-15): the milestones' driver side (nspire_padlog.h) ---- */

#ifdef USBPAD_OS_HAS_R11W
/* The counters as a milestone: at each detach's end and at exit. */
static void milestone_counters(void)
{
  const struct usbpad_os_info *f = usbpad_os_info();
  uint32_t w[6];

  w[0] = f->matches;
  w[1] = f->attaches;
  w[2] = f->detaches;
  w[3] = f->out_ok_start;
  w[4] = f->out_ok_main;
  w[5] = f->out_ok_cb;
  nspire_padlog_milestone6(PL_MS_COUNTERS, 0, 0, 0, w);
}

int nspire_usbpad_counters_text(char *out, unsigned size)
{
  const struct usbpad_os_info *f;

  if (!started || !size)
    return 0;
  f = usbpad_os_info();
  return snprintf(out, size, "Driver now: offers %lu, attaches %lu, detaches %lu (with an OUT in flight %lu, an abort "
                  "that did not call back inside it %lu), reports %lu; OUT sent from the attach %lu, from the main task "
                  "%lu (%lu after waiting for an IN completion that never came), from the IN completion %lu (refused "
                  "%lu), dropped at a detach %lu; aborts %lu, frees %lu, closes %lu (the last: %ld, %ld, %ld); kept past "
                  "a detach %lu, retired later %lu, still kept %lu",
                  (unsigned long)f->matches, (unsigned long)f->attaches, (unsigned long)f->detaches,
                  (unsigned long)f->detach_out_live, (unsigned long)f->detach_late, (unsigned long)f->in_ok,
                  (unsigned long)f->out_ok_start, (unsigned long)f->out_ok_main, (unsigned long)f->out_fallback,
                  (unsigned long)f->out_ok_cb, (unsigned long)f->out_err_cb, (unsigned long)f->out_waiting_dropped,
                  (unsigned long)f->aborts, (unsigned long)f->frees, (unsigned long)f->closes, (long)f->last_abort,
                  (long)f->last_free, (long)f->last_close, (unsigned long)f->kept_at_detach,
                  (unsigned long)f->retired_late, (unsigned long)f->kept_now);
}
#else
static void milestone_counters(void) {}
#endif

int nspire_usbpad_diag_text(char *out, unsigned size)
{
  if (!started || !size)
    return 0;
  return usbpad_diag(out, size);
}

/* The library's nspire_usbpad_active, as gpSP's copy of it names it (the
 * Makefile's usbpad_gpsp.o). */
int usbpad_lib_active(void);

/* While a pad's attached the USB port is its, and nspire_link.c doesn't
 * watch the cable (its weak default: never). Here, in LTO code, as LTO
 * otherwise keeps that default (the Makefile, usbpad_gpsp.o). */
int nspire_usbpad_active(void)
{
  return usbpad_lib_active();
}

/* For nspire_link.c's orientation hint: not while a pad (or an event
 * about one) is there. */
int nspire_usbpad_saw_device(void)
{
  return device_seen;
}

/* The OS's match offers so far (usbpad_diag's "matches="): a device the OS
 * offered whose attach isn't done (or failed) shows only there. */
static long diag_matches(void)
{
  static char text[900];
  const char *m;

  usbpad_diag(text, sizeof(text));
  m = strstr(text, "matches=");
  return m ? strtol(m + 8, NULL, 10) : 0;
}

/* SEAM-8: for the arbiter, right before a power cycle (not every frame: a
 * diag text): the OS offered something (a match, an attach, a refusal, an
 * unsupported device) since the port was last empty: it's being handled. */
int nspire_usbpad_offered(void)
{
  if (!started)
    return 0;
  return device_seen || (long)offer_count() != matches_mark;
}

/* At exit (atexit: quit() has put gpSP's hardware back by then). The
 * driver's shutdown aborts and closes the pads' pipes in this task, and
 * the OS's USB stack may wait on its interrupts there (usbshim.h: "don't
 * keep IRQs masked while waiting on the device"); gpSP runs with them
 * masked, so they're on around it (FIQs as they were). The OS's keypad
 * line is still off here (nspire_exit.c gives it back after this). */
/* R11w (2026-11-15): the exit in two parts. The R11w run wrote the pad log
 * here and then never finished (keys dead, a reset): every file is written
 * first now (nspire_usbpad_exit_log, from quit() before any USB shutdown),
 * then the shutdown is bounded (nspire_usbpad_stop_within): what doesn't
 * finish in its time restarts the calculator (nspire_exit.c) instead of
 * unloading code the OS can still call or handing back dead keys. */
static int exit_logged, exit_quiet, exit_say, exit_pads, stopped, exit_untouched;
#if USBPAD_API_LEVEL >= 6
static void drain_trace(void);
#endif

/* (r19-usb-exit) The cable's plain end (the bits of nspire_usb_bits, as
 * nspire_usbport.c's classify): the ID pin the B-device's (a CX II's OTGCSR
 * bit 21, here 0x20; a CX's OTGSC bit 8, here bit 0): no adapter grounds
 * it, and TI's OS is never the host then (no role swap); on a CX, its
 * USBMODE (bits 12-13) not the host's (3) either. */
static int device_mode(u32 bits)
{
  switch (bits >> 28)
  {
  case 2:
    return (bits & 0x20) != 0;
  case 1:
    return (bits & 1) && (bits >> 12 & 3) != 3;
  }
  return 0;
}

int nspire_usbpad_exit_untouched(void)
{
  return exit_logged && exit_untouched;
}

void nspire_usbpad_exit_log(void)
{
  int plugged, gated;
  u32 bits;

  if (!started || exit_logged)
    return;
  exit_logged = 1;
  exit_pads = usbpad_count();   /* before the shutdown, which empties the slots */
  plugged = nspire_usb_plugged();
  exit_say = logged || plugged > 0;
  /* EXIT-3: nothing on the port now, as at the last poll, for a second at
   * least, and no pad: no USB callback can be on its way into this code,
   * so the shutdown doesn't let the OS run for one (its 10 ticks of
   * draining, 30-100 ms): a quit with nothing plugged in waits on nothing. */
  /* EXIT-R2: a CX whose OS gated its controller off (0x900B0018 bit 5,
   * as after a link ends: the CX CAS's log, "controller gated off (can't
   * tell)") reads -1, not 0: its controller has no clock, so no device and
   * no callback can be on its way either: as empty. */
  gated = nspire_usb_gated();
  bits = nspire_usb_bits();
  /* (r19-usb-exit) and the cable's plain end, its controller a USB device
   * (a computer's cable: a CX's hardware run of 2026-11-21, "device end"
   * all along): no host's driver is called in device mode, so nothing can
   * be on its way into this code either (its 10 ticks were 101 ms there) */
  exit_quiet = exit_pads == 0 && (plugged == 0 || (plugged < 0 && gated) || (plugged > 0 && device_mode(bits))) &&
               bits == port_bits && nspire_ticks() - port_changed_at >= NSPIRE_TICK_HZ;
  /* ...and no device was ever offered to the OS's drivers in this run (the
   * pad driver, registered first, is offered every one): no callback of the
   * sound's or the drive's driver ever ran, none can start, so their
   * shutdowns' waits for one are for nothing (nspire_usbpad_exit_untouched) */
  exit_untouched = exit_quiet && offer_count() == 0;
  /* A hang or reset inside the shutdown (pipes aborted and closed in this
   * task, the OS's USB task waited for) leaves no trace unless this is on
   * the disk before it starts (RDY-1, RDY-9); only then: EXIT-3, with
   * nothing on the port nothing waits there (one log write less). */
  if (exit_say)
  {
    plog("exit: shutting down, %d pad(s) in, %d polls, %d forced OS ticks, %u OS sleeps in all; port: plugged %d, "
         "gated %d, bits %08lx (%08lx at the last poll, %lu ms ago): %s", exit_pads, polls_taken, forced_ticks,
         (unsigned)nspire_os_sleeps(), plugged, gated, (unsigned long)bits, (unsigned long)port_bits,
         (unsigned long)((u64)(nspire_ticks() - port_changed_at) * 1000 / NSPIRE_TICK_HZ),
         exit_quiet ? (plugged < 0 ? "the controller is gated off: no wait" :
                       plugged > 0 ? "the port is the cable's plain end (a USB device): no wait" :
                       "the port is empty: no wait") :
                      "the driver's shutdown lets the OS run");
    log_diag("before shutdown", 1);
    if (!exit_quiet)
      nspire_link_log_flush();
  }
#if USBPAD_API_LEVEL >= 6
  drain_trace();
#endif
  milestone_counters();
  nspire_padlog_exit_mark(PL_EXIT_LOGGED, (uint32_t)exit_pads);
  nspire_padlog_exit();   /* PAD3: before the shutdown empties the slots; R11w: the milestones and the driver's block */
}

int nspire_usbpad_stop_within(unsigned ms)
{
  u32 cpsr, t0;
  int done = 1;

  if (!started || stopped)
    return 0;
  stopped = 1;
  t0 = nspire_ticks();
  nspire_usbpad_exit_log();   /* (an exit() that didn't come through quit()) */
  nspire_exit_begin();
  nspire_exit_step("pad log");
  if (exit_quiet)
    usbpad_set_hooks(NULL, usb_port);
#ifdef USBPAD_OS_HAS_R11W
  {
    /* the driver's waits within this budget (its OS ticks: ~10 ms on a CX
     * II, ~3 ms on a CX: counted as 10, the shorter tick only shortens) */
    struct usbpad_os_config os;
    unsigned ticks = ms ? (ms + 9) / 10 : 1;
    usbpad_os_get_config(&os);
    os.shutdown_ticks = (uint16_t)(ticks > 1000 ? 1000 : ticks);
    usbpad_os_set_config(&os);
  }
#endif
  /* EXIT-R6: the keypad line off again first, if an OS slice with no check
   * after it turned it on: with the CPU's interrupts on, the OS's keypad
   * handler would queue a key pressed now (up to ~7 s with a hung attach). */
  nspire_os_keypad_keep_off();
  if (nspire_exit_trail)
    nspire_exit_trail(exit_quiet ? "the pad driver's shutdown (the port quiet: no sleeps)" :
                                   "the pad driver's shutdown (its pipes closed, its callbacks waited for)");
  cpsr = nspire_irqs_unmask();
  usbpad_shutdown();
#ifdef USBPAD_OS_HAS_SHUTDOWN_PENDING
  /* A retired trampoline only guards a future callback. An attach that
   * already entered this image must return before Ndless can unload it:
   * waited for, but only within the budget (R11w: this wait had no end);
   * past it the caller restarts the calculator, which unloads nothing. */
  if (usbpad_os_shutdown_pending())
  {
    unsigned drain;
    plog("exit: a USB callback is still inside gpSP after the driver's shutdown: waiting for it (%lu ms left)",
         (unsigned long)(ms > (u32)((u64)(nspire_ticks() - t0) * 1000 / NSPIRE_TICK_HZ) ?
                         ms - (u32)((u64)(nspire_ticks() - t0) * 1000 / NSPIRE_TICK_HZ) : 0));
    while (usbpad_os_shutdown_pending() && (u64)(nspire_ticks() - t0) * 1000 < (u64)ms * NSPIRE_TICK_HZ)
      if (!nspire_os_sleep())
        break;
    if (usbpad_os_shutdown_pending())
      done = 0;
    else
    {
      /* The flag clears before the callback's final return instructions. */
      for (drain = 0; drain < USBPAD_OS_DRAIN_TICKS; drain++)
        nspire_os_sleep();
      nspire_exit_step("USB callback returned");
    }
  }
#endif
  nspire_irqs_restore(cpsr);
  if (!done)
  {
    plog("exit: the pad driver's shutdown did not finish in %u ms (a callback still inside gpSP)", ms);
    return -1;
  }
  nspire_exit_step(exit_quiet ? "pad driver shut down (no wait)" : "pad driver shut down");
  if (exit_say)
  {
    plog("exit: usbpad_shutdown returned (%d pad(s) were in)", exit_pads);
    log_diag("after shutdown", 0);
    /* The log and the clean marker are written once, last, by
     * nspire_exit.c (EXIT-3): a first log write here still ends as
     * "clean" (RDY-9). */
  }
  nspire_os_want(OS_WANT_PAD, 0);
  return 0;
}

static void awake_end(void);

/* At exit (atexit), for an exit() that didn't come through quit(). */
static void stop(void)
{
  /* (2026-11-16) the OS's USB had stopped (watch_stuck): the calculator
   * restarts instead of being handed back frozen; quit() does this itself
   * before its shutdowns, this is for an exit() that didn't come through it */
  if (os_dead)
  {
    nspire_usbpad_exit_log();   /* (the pad log first: its file is written before any restart) */
    nspire_exit_usb_timed_out("the OS's USB (it had stopped before the exit)");
  }
  if (nspire_usbpad_stop_within(nspire_exit_usb_left_ms()) != 0)
    nspire_exit_usb_timed_out("pad driver");
  /* (2026-11-17) the exit's last USB step: the sound's and the drive's
   * shutdowns ran before this one (their atexits were registered after
   * it), on a bus the OS could not suspend under them */
  if (nspire_exit_trail)
    nspire_exit_trail("the pad glue's exit handler: the OS's USB code put back (the awake patch)");
  awake_end();
}

#ifdef USBPAD_API_LEVEL
/* usbpad_set_wait_hook: shutdown is waiting for the OS's USB task, which is
 * inside our attach on a pad that doesn't answer (R11w: within the exit's
 * budget). Into the log's memory only (R11w: no file write while a USB
 * shutdown is under way; the files were written before it). */
static void shutdown_waiting(void)
{
  plog("exit: waiting for the USB controller (an attach is blocked in the OS; within the exit's USB budget)");
}
#endif

void nspire_usbpad_start(void)
{
  char path[600];
  struct usbpad_os_config os;

  if (started)
    return;
  started = 1;
  usbpad_get_config(&config);
  nspire_save_file(path, sizeof(path), SETTINGS_FILE);
  read_settings(path);
  if (usbpad_set_config(&config) != 0)
  {
    /* Thresholds the driver refuses (out of range, or release_at not below
     * press_at): its defaults for those two, the stick as the file says. */
    struct usbpad_config def;

    usbpad_get_config(&def);
    config.on_pct = def.on_pct;
    config.off_pct = def.off_pct;
    usbpad_set_config(&config);
  }
  nspire_link_log_pad_setting(os_time);
  usbpad_os_get_config(&os);
  os.want_live = (uint8_t)os_time;
  os.replug_hint = 0;   /* S4: the arbiter's power cycle is the first resort, not a toast to the user */
  usbpad_os_set_config(&os);
  usbpad_set_hooks(sleep_tick, usb_port);
#ifdef USBPAD_API_LEVEL
  usbpad_set_wait_hook(shutdown_waiting);
#endif
  port_bits = nspire_usb_bits();
  attach_left = ATTACH_POLLS;
  /* A failure (an OS without the USB calls...) leaves no pads: polls and
   * states then read as nothing attached. */
  init_result = usbpad_init(USBPAD_AUTO);
  matches_mark = diag_matches();
  port_changed_at = nspire_ticks();
  /* Something on the port at the start: the log is wanted now. */
  if (nspire_usb_plugged() > 0)
  {
    started_logged = 1;
    log_startup();
    nspire_link_log_request_flush();
    logged = 1;
  }
  atexit(stop);
}

/* A pad's buttons in its layout: "labels" swaps bottom and right, left and
 * top (usbpad.h's are by position). */
static int labels_of(const struct usbpad_state *s)
{
  int labels = labels_all;
  unsigned i;

  for (i = 0; i < num_layouts; i++)
    if (layouts[i].vid == s->vid && layouts[i].pid == s->pid)
      labels = layouts[i].labels;
  return labels;
}

static u32 pad_keys(int player)
{
  const struct usbpad_state *s = usbpad_state(player);
  u32 b = s->buttons;

  if (!s->connected)
    return 0;
  if (labels_of(s))
    b = (b & ~0xF0u) | (b & (USBPAD_SOUTH | USBPAD_WEST)) << 1 | (b & (USBPAD_EAST | USBPAD_NORTH)) >> 1;
  return b;
}

/* S10: what the pad itself sends, in the log: the first report, then each
 * button the first time it's seen. A few compares per poll; the lines come
 * a handful of times a session. */
static void note_input(void)
{
  static const char *const bn[USBPAD_NBUTTONS] = { "up", "down", "left", "right", "south", "east", "west", "north",
                                                   "L1", "R1", "L2", "R2", "select", "start", "home", "L3", "R3", "aux" };
  unsigned p, i;

  for (p = 0; p < USBPAD_PLAYERS; p++)
  {
    const struct usbpad_state *s = usbpad_state(p);
    u32 fresh;

    if (!s->connected)
    {
      reports_logged &= ~(1u << p);   /* a replug is a new pad: its story again */
      buttons_seen[p] = 0;
      pads_on &= ~(1u << p);
      silent_said &= ~(1u << p);
      continue;
    }
    if (!(pads_on & (1u << p)))
    {
      pads_on |= 1u << p;
      pad_on_at[p] = nspire_ticks();
    }
    if (!s->reports && !(silent_said & (1u << p)) &&
        (u64)(nspire_ticks() - pad_on_at[p]) * 1000 >= (u64)PAD_SILENT_MS * NSPIRE_TICK_HZ)
    {
      silent_said |= 1u << p;
      plog("silent: slot %u, %04x:%04x attached %u ms ago and has sent no report", p, (unsigned)s->vid,
           (unsigned)s->pid, PAD_SILENT_MS);
      nspire_padlog_milestone(PL_MS_INPUT, 2, 0, 0, (u32)s->vid << 16 | s->pid, p, PAD_SILENT_MS);
    }
    if (s->reports && !(reports_logged & (1u << p)))
    {
      reports_logged |= 1u << p;
      plog("first report: slot %u, %04x:%04x, %u OS sleeps", p, (unsigned)s->vid, (unsigned)s->pid,
           (unsigned)nspire_os_sleeps());
      nspire_padlog_milestone(PL_MS_INPUT, 1, 0, 0, (u32)s->vid << 16 | s->pid, p, 0);
    }
    fresh = s->buttons & ~buttons_seen[p];
    if (fresh)
    {
      buttons_seen[p] |= fresh;
      for (i = 0; i < USBPAD_NBUTTONS; i++)
        if (fresh & (1u << i))
          plog("button: slot %u, %s", p, bn[i]);
    }
  }
}

/* Whether to poll the driver on this read of the keys. With a timer: 1/80 s
 * apart at least. Without one (an older CX's) gpSP's time moves only as
 * frames wait, not in the loops that read the keys flat out, and the RTC's
 * second is the only real clock: polls are spread over each second by how
 * often the keys were read in the last one, and stop at POLLS_PER_SEC in
 * any one second. */
static int poll_due(u32 now)
{
  u32 sec;

  if (nspire_timer_runs())
    return !polled || now - last_poll >= POLL_GAP;
  sec = nspire_rtc_seconds();
  if (sec != rtc_at)
  {
    stride = (rtc_calls + POLLS_PER_SEC - 1) / POLLS_PER_SEC;
    if (!stride)
      stride = 1;
    rtc_at = sec;
    rtc_calls = rtc_polls = 0;
  }
  rtc_calls++;
  if (polled && (++skipped < stride || rtc_polls >= POLLS_PER_SEC))
    return 0;
  skipped = 0;
  rtc_polls++;
  return 1;
}

/* An event a newer state makes wrong, not to be shown: "Plug the
 * controller in again" once a pad is in (the driver says it only when the
 * OS offered it nothing at all) or one connects later in the queue, and
 * while nothing's at the host's end of the port (a CX's controller off with
 * nothing plugged in reads "can't tell", which the driver takes for
 * something) or the link has the cable (a calculator's on it: the OS
 * offers the driver nothing then); "may need more power" once that pad (its
 * USB id) is in or connects later. */
static int stale(unsigned i)
{
  const struct usbpad_event *e = &queue[i];
  int replug = e->type == USBPAD_EV_REPLUG_NEEDED, p;
  unsigned j;

#if USBPAD_API_LEVEL >= 3
  /* "Stopped answering": only when the same pad (USB id) connected again
   * after it in the queue (it was unplugged and plugged in, or came back):
   * not by the pad states, two pads of one kind could be in. */
  if (e->type == USBPAD_EV_FAILED)
  {
    for (j = i + 1; j < queued; j++)
      if (queue[j].type == USBPAD_EV_CONNECTED && queue[j].vid == e->vid && queue[j].pid == e->pid)
        return 1;
    return 0;
  }
#endif
  if (!replug && e->type != USBPAD_EV_NEEDS_POWER)
    return 0;
  if (replug && (usb_port() <= 0 || nspire_link_cable_present()))
    return 1;
  for (p = 0; p < USBPAD_PLAYERS; p++)
    if (usbpad_state(p)->connected &&
        (replug || (usbpad_state(p)->vid == e->vid && usbpad_state(p)->pid == e->pid)))
      return 1;
  for (j = i + 1; j < queued; j++)
    if (queue[j].type == USBPAD_EV_CONNECTED && (replug || (queue[j].vid == e->vid && queue[j].pid == e->pid)))
      return 1;
  return 0;
}

static void drop(unsigned i)
{
  memmove(queue + i, queue + i + 1, sizeof(queue[0]) * (queued - i - 1));
  queued--;
}

#if USBPAD_API_LEVEL >= 6
/* ---- R10b (2026-11-15): a bring-up that failed, the driver's trace ----
 *
 * Driver API 6 says where a failed bring-up stopped (struct usbpad_event's
 * step, ep, err) and keeps a trace of every bring-up step (usbpad_trace_*).
 * The Controller page names the pad and the step in a few words, the pad
 * log keeps the numbers (and every trace record), and the port is free for
 * the next pad as soon as this one leaves (the failure's PORT_OS_OFFER
 * report, which holds the arbiter off this device, goes with it). */
static char fail_text[PADSTATUS_VALUE];   /* the Driver line while the failed device is on the port */

static const char *step_plain(int step)
{
  switch (step)
  {
  /* (at most 23 characters: the page's Driver line holds 63 with the
   * longest name, "DualShock 4", and the numbers) */
  case USBPAD_STEP_DESC: return "bad USB descriptors";
  case USBPAD_STEP_CLAIM: return "no room for a pad";
  case USBPAD_STEP_CTL_FORM: return "setup requests failed";
  case USBPAD_STEP_OUT_OPEN: return "OS gave no send pipe";
  case USBPAD_STEP_CONTROL: return "a setup request failed";
  case USBPAD_STEP_OUT: return "start-up refused";
  case USBPAD_STEP_OUT_DONE: return "start-up message failed";
  case USBPAD_STEP_SET_REPORT: return "setup report failed";
  case USBPAD_STEP_START: return "its start-up failed";
  case USBPAD_STEP_IN_OPEN: return "OS gave no read pipe";
  case USBPAD_STEP_LIVE: return "it sent no buttons";
  case USBPAD_STEP_GAVE_UP: return "no answer to start-up";
  case USBPAD_STEP_OUT_STUCK: return "message never sent";
  case USBPAD_STEP_IN_ERR:
  case USBPAD_STEP_IN_STALE: return "buttons stopped coming";
  }
  return "it didn't start";
}

/* "ep 81, OS 11" / "error -1" / "" */
static void step_numbers(int ep, int err, char *out, unsigned size)
{
  int n = ep ? snprintf(out, size, ", ep %02x", (unsigned)ep) : (out[0] = 0, 0);

  if (err && n >= 0 && (unsigned)n < size)
    snprintf(out + n, size - n, err > 0 ? ", OS %d" : ", error %d", err);
}

static void note_failure(const struct usbpad_event *ev)
{
  char name[24], nums[32], detail[80];

  if (ev->kind)
    snprintf(name, sizeof(name), "%s", ev->name);
  else
    snprintf(name, sizeof(name), "%04x:%04x", (unsigned)ev->vid, (unsigned)ev->pid);
  step_numbers(ev->ep, ev->err, nums, sizeof(nums));
  /* (cut to the page's line where it must be: the log line below has it all) */
  if (snprintf(fail_text, sizeof(fail_text), "%s: %s (%s%s)", name, step_plain(ev->step), usbpad_step_name(ev->step),
               nums) < 0)
    fail_text[0] = 0;
  usbpad_event_detail(ev, detail, sizeof(detail));
  plog("bring-up failed: %04x:%04x %s: step %s (%u), ep %02x, error %d: %s", (unsigned)ev->vid, (unsigned)ev->pid,
       ev->name, usbpad_step_name(ev->step), (unsigned)ev->step, (unsigned)ev->ep, (int)ev->err, detail);
}

/* The pad log's words for its PL_TRACE and PL_EVENT numbers (nspire_padlog.h). */
void nspire_usbpad_trace_words(int step, int task, int port, const uint32_t w[6], char *out, unsigned size)
{
  struct usbpad_trace t;
  char text[112], lost[64] = "";

  memset(&t, 0, sizeof(t));
  t.seq = w[3];
  t.poll = w[4];
  t.vid = (uint16_t)(w[0] >> 16);
  t.pid = (uint16_t)w[0];
  t.step = (uint8_t)step;
  t.ep = (uint8_t)(w[2] >> 16);
  t.port = (int8_t)port;
  t.task = (char)task;
  t.err = (int16_t)(int32_t)w[1];
  t.arg = (uint16_t)w[2];
  usbpad_trace_text(&t, text, sizeof(text));
  if (w[5] & 0xFFFF)
    snprintf(lost, sizeof(lost), " (%lu record(s) lost before it)", (unsigned long)(w[5] & 0xFFFF));
  else if (w[5] >> 16)
    snprintf(lost, sizeof(lost), " (%lu routine output record(s) not logged before it)", (unsigned long)(w[5] >> 16));
  snprintf(out, size, "bring-up #%lu, poll %lu: %s%s", (unsigned long)t.seq, (unsigned long)t.poll, text, lost);
}

void nspire_usbpad_fail_words(uint32_t facts, uint32_t err, char *out, unsigned size)
{
  struct usbpad_event e;

  memset(&e, 0, sizeof(e));
  e.type = USBPAD_EV_UNSUPPORTED;
  e.step = (uint8_t)facts;
  e.ep = (uint8_t)(facts >> 8);
  e.err = (int16_t)(int32_t)err;
  usbpad_event_detail(&e, out, size);
}

/* Every record of the driver's trace into the pad log, but the routine
 * output of a pad that works (its lights, the Xbox pads' ACKs: past
 * ROUTINE_LOGGED of them a pad, counted): a few compares a poll, none
 * when nothing is new. */
#define ROUTINE_LOGGED 6
static u32 trace_last[2];
static u8 port_live[8], routine_n[8];
static u32 routine_skipped;

static void drain_trace(void)
{
  struct usbpad_trace t;
  unsigned n;

  for (n = 0; n < 64 && usbpad_trace_next(&t); n++)
  {
    int r = t.task == 'U' ? 0 : 1, p = t.port >= 0 && t.port < 8 ? t.port : -1;
    uint32_t w[6], lost = 0;

    if (t.seq > trace_last[r] + 1)
      lost = t.seq - trace_last[r] - 1;
    trace_last[r] = t.seq;
    /* R11w: every record a milestone too (repeats counted there) */
    w[0] = (u32)t.vid << 16 | t.pid;
    w[1] = (u32)(int32_t)t.err;
    w[2] = (u32)t.ep << 16 | t.arg;
    w[3] = t.seq;
    w[4] = t.poll;
    w[5] = lost > 0xFFFF ? 0xFFFF : lost;
    nspire_padlog_milestone6(PL_TRACE, t.step, t.task, t.port + 1, w);
#if USBPAD_API_LEVEL >= 7
    if (t.step == USBPAD_STEP_DETACH_END)
      milestone_counters();
#endif
    if (p >= 0 && (t.step == USBPAD_STEP_START || t.step == USBPAD_STEP_DETACH || t.step == USBPAD_STEP_CLAIM))
      port_live[p] = routine_n[p] = 0;
    if (p >= 0 && t.step == USBPAD_STEP_LIVE)
      port_live[p] = 1;
    if (p >= 0 && port_live[p] && !lost && (t.step == USBPAD_STEP_OUT || t.step == USBPAD_STEP_OUT_DONE) &&
        (t.err == 0 || t.err == 2) && routine_n[p] >= ROUTINE_LOGGED)
    {
      routine_skipped++;
      continue;
    }
    if (p >= 0 && port_live[p] && (t.step == USBPAD_STEP_OUT || t.step == USBPAD_STEP_OUT_DONE))
      routine_n[p]++;
    w[0] = (u32)t.vid << 16 | t.pid;
    w[1] = (u32)(int32_t)t.err;
    w[2] = (u32)t.ep << 16 | t.arg;
    w[3] = t.seq;
    w[4] = t.poll;
    w[5] = (lost > 0xFFFF ? 0xFFFF : lost) | (routine_skipped > 0xFFFF ? 0xFFFF : routine_skipped) << 16;
    routine_skipped = 0;
    nspire_padlog_add6(PL_TRACE, t.step, t.task, t.port + 1, w);
  }
}

#endif

/* R10b: the host-halt watch (after the come-up, which it starts). */
static int halted;
static u32 halted_at, offered_at;
static void watch_halt(const struct pad_status *s);

static void log_event(const struct usbpad_event *ev)
{
  static const char *const names[] = { "?", "attach (connected)", "detach (disconnected)",
                                       "replug hint (the OS offered the driver no device)",
                                       "power refusal (needs more bus power)", "unsupported (no driver could start it)",
                                       "failed (a working pad stopped answering; given up until it is unplugged)" };
  u32 facts = 0, err = 0;

  /* (2026-11-17, final/speed9a) a pad's detach tells of a device gone, not
   * of one there: taken in the poll that saw the port change (device_left,
   * then device_seen = events_now) or after it, it held the arbiter's power
   * restart off the next device for good, the order PAD4 fixed for the
   * other way round (tests/host pad4_in_turn met it once the frame loop's
   * sleeps moved the polls) */
  if (ev->type != USBPAD_EV_DISCONNECTED)
    device_seen = events_now = 1;
#if USBPAD_API_LEVEL >= 6
  if ((ev->type == USBPAD_EV_UNSUPPORTED || ev->type == USBPAD_EV_FAILED) && ev->step)
  {
    facts = PL_EVENT_FACTS | ev->step | (u32)ev->ep << 8;
    err = (u32)(int32_t)ev->err;
    drain_trace();   /* the records that led to it go first */
    note_failure(ev);
  }
#endif
  nspire_padlog_add(PL_EVENT, ev->type, ev->kind, ev->player + 1, (u32)ev->vid << 16 | ev->pid, facts, err);
  nspire_padlog_milestone(PL_EVENT, ev->type, ev->kind, ev->player + 1, (u32)ev->vid << 16 | ev->pid, facts, err);
  if (ev->type != USBPAD_EV_DISCONNECTED)
  {
    last_ev = *ev;
    last_ev_set = 1;
    gone_now = 0;   /* (what came after a pad left is the next device's) */
    gone_poll = 0;
  }
  else
  {
    gone_now = 1;
    gone_poll = 1;
  }
  if (ev->type == USBPAD_EV_CONNECTED)
    nspire_padlog_try_end(PL_TRY_PAD);
  else if (ev->type != USBPAD_EV_DISCONNECTED && ev->type != USBPAD_EV_REPLUG_NEEDED)
    nspire_padlog_try_end(PL_TRY_FAILED);
  /* SEAM-8: the OS offered a device the driver refused or can't power: it
   * stays on the bus, the port its (no power cycle, no toast) until the port
   * changes. (An attached pad is the arbiter's PAD already; one still
   * attaching shows in usbpad_diag's matches: nspire_usbpad_offered.) */
  if (ev->type == USBPAD_EV_UNSUPPORTED || ev->type == USBPAD_EV_NEEDS_POWER
#if USBPAD_API_LEVEL >= 3
      /* a pad that stopped answering: the driver gave up on it and it stays
       * on the bus, so the OS did offer it: no power cycle for it either */
      || ev->type == USBPAD_EV_FAILED
#endif
     )
    nspire_usbport_os_report(PORT_OS_OFFER, ev->kind, ev->vid);
  plog("event: %s, slot %d, %04x:%04x, class %s (%u), %u OS sleeps",
       ev->type < sizeof(names) / sizeof(names[0]) ? names[ev->type] : names[0],
       ev->player, (unsigned)ev->vid, (unsigned)ev->pid, ev->name, (unsigned)ev->kind,
       (unsigned)nspire_os_sleeps());
  log_diag(ev->type == USBPAD_EV_CONNECTED || ev->type == USBPAD_EV_DISCONNECTED ? "event" : "failure",
           ev->type != USBPAD_EV_CONNECTED && ev->type != USBPAD_EV_DISCONNECTED ? 2 : 1);
}

/* The driver's new events into the queue: when it's full, a stale one
 * goes first, else the oldest (as the driver's own queue does). */
static void take_events(void)
{
  struct usbpad_event ev;
  unsigned i;

  while (usbpad_next_event(&ev))
  {
    if (queued == QUEUE)
    {
      for (i = 0; i < queued && !stale(i); i++)
        ;
      drop(i < queued ? i : 0);
    }
    queue[queued++] = ev;
    log_event(&ev);
  }
}

#ifdef USBPAD_API_LEVEL
/* How far the OS's USB task got, one line the first time each counter moves
 * (RDY-4): a reset or hang in the OS's match, attach or first report leaves
 * the last of these lines as the stage it reached. Reads the driver's
 * counters, no OS call; a few compares per poll. */
static u32 seen_matches, seen_attaches, seen_in_err, seen_synth;
/* The USB port's changes (attaching()), and the change whose offers the
 * log has: the driver's diag again at the first offer after each change
 * (the second device's ring, its reasons and raw words), OFFER_LOGS a run. */
static u32 port_changes, offer_logged_change;
static int offer_logs;
#define OFFER_LOGS 6
static int first_report_logged;
static unsigned first_report_wait;
/* Polls to wait for a pad to parse the first raw report before saying it didn't. */
#define FIRST_REPORT_POLLS 80

static void log_first_report(const struct usbpad_os_info *f)
{
  char hex[40];
  unsigned i;
  int p, slot = -1;

  for (p = 0; p < USBPAD_PLAYERS; p++)
    if (usbpad_state(p)->connected && usbpad_state(p)->reports && slot < 0)
      slot = p;
  if (slot < 0 && ++first_report_wait < FIRST_REPORT_POLLS)
    return;
  first_report_logged = 1;
  for (i = 0; i < sizeof(f->first_report) && i < f->first_len; i++)
    snprintf(hex + 2 * i, sizeof(hex) - 2 * i, "%02x", (unsigned)f->first_report[i]);
  hex[2 * i] = 0;
  if (slot < 0)
  {
    plog("stage: the first report (%u bytes) came but no pad parsed it in %u polls: %s", (unsigned)f->first_len,
         FIRST_REPORT_POLLS, hex);
    return;
  }
  {
    const struct usbpad_state *s = usbpad_state(slot);

    plog("stage: the first report, %u bytes: %s; parsed for slot %d: buttons %05x, sticks %d %d %d %d, triggers %u %u",
         (unsigned)f->first_len, hex, slot, (unsigned)s->buttons, s->lx, s->ly, s->rx, s->ry, s->lt, s->rt);
  }
}

/* PAD3: each new entry of the driver's match ring into the pad log (the
 * OS's offer and the driver's answer; the ring keeps the last 8). */
static u32 offers_logged;

static void log_offers(const struct usbpad_os_info *f)
{
  u32 n = f->match_n;

  if (n - offers_logged > USBPAD_OS_MATCH_RING)
    offers_logged = n - USBPAD_OS_MATCH_RING;
  for (; offers_logged < n; offers_logged++)
  {
    const struct usbpad_os_match *m = &f->match[offers_logged % USBPAD_OS_MATCH_RING];
    if (m->score > 0)
      taken_since = 1;   /* R11u: the driver took an interface of this device */
    if (m->iface)
      iface_offered = 1;
#ifdef USBPAD_OS_HAS_HUB
    u32 port = (u32)m->port << 24;   /* the hub port it is on (the pad log's "port N") */
#else
    u32 port = 0;
#endif
    nspire_padlog_add(PL_OFFER, (u8)m->score, m->iface, m->flags, (u32)m->vid << 16 | m->pid,
                      port | (u32)m->cls << 16 | (u32)m->sub << 8 | m->proto, offers_logged + 1);
    nspire_padlog_milestone(PL_OFFER, (u8)m->score, m->iface, m->flags, (u32)m->vid << 16 | m->pid,
                            port | (u32)m->cls << 16 | (u32)m->sub << 8 | m->proto, offers_logged + 1);
  }
}

#ifdef USBPAD_OS_HAS_NOT_PAD
/* R11u: the driver kept an interface as "not a game controller" (one
 * attach that fails after a match made TI's OS offer nothing more for the
 * session, so the driver never refuses now). Logged once each, and the
 * device counts as not a controller from here (the verdict, no toast). */
static void note_not_pad(const struct usbpad_os_info *f)
{
  not_pad_now = 1;
  not_pad_at = nspire_ticks();
  not_pad_vid = f->not_pad_vid;
  not_pad_pid = f->not_pad_pid;
  not_pad_if = f->not_pad_if;
  nspire_padlog_add(PL_NOT_PAD, f->not_pad_if, 0, 0, (u32)f->not_pad_vid << 16 | f->not_pad_pid,
                    (u32)f->not_pad_cls << 16 | (u32)f->not_pad_sub << 8 | f->not_pad_proto, f->not_pads);
  nspire_padlog_milestone(PL_NOT_PAD, f->not_pad_if, 0, 0, (u32)f->not_pad_vid << 16 | f->not_pad_pid,
                          (u32)f->not_pad_cls << 16 | (u32)f->not_pad_sub << 8 | f->not_pad_proto, f->not_pads);
  plog("not a controller: %04x:%04x interface %u (%02x/%02x/%02x): the driver keeps it quietly (no pipe, no OS time); "
       "the OS goes on offering the next device",
       (unsigned)f->not_pad_vid, (unsigned)f->not_pad_pid, (unsigned)f->not_pad_if, (unsigned)f->not_pad_cls,
       (unsigned)f->not_pad_sub, (unsigned)f->not_pad_proto);
}
#endif

static void watch_stages(void)
{
  const struct usbpad_os_info *f = usbpad_os_info();
  static u32 seen_out_errors, seen_out_done;

#ifdef USBPAD_OS_HAS_NOT_PAD
  if (f->not_pads != not_pads_seen)
  {
    not_pads_seen = f->not_pads;
    note_not_pad(f);
  }
#endif
  if (f->attaches != seen_attaches)
  {
    nspire_padlog_add(PL_DRIVER_ATTACH, f->attach_step, f->ifc_in, f->ifc_out,
                      (u32)f->ifc_vid << 16 | f->ifc_pid, (u32)f->attach_err, f->attaches);
    nspire_padlog_milestone(PL_DRIVER_ATTACH, f->attach_step, f->ifc_in, f->ifc_out,
                            (u32)f->ifc_vid << 16 | f->ifc_pid, (u32)f->attach_err, f->attaches);
  }
#ifdef USBPAD_OS_HAS_SHUTDOWN_PENDING
  if (f->out_errors != seen_out_errors || f->out_done != seen_out_done)
  {
    nspire_padlog_add(PL_DRIVER_OUT, f->out_ep, f->out_sync_status, f->out_cb_status,
                      (u32)f->out_vid << 16 | f->out_pid, f->out_errors, f->out_done);
    nspire_padlog_milestone(PL_DRIVER_OUT, f->out_ep, f->out_sync_status, f->out_cb_status,
                            (u32)f->out_vid << 16 | f->out_pid, f->out_errors, f->out_done);
    seen_out_errors = f->out_errors;
    seen_out_done = f->out_done;
  }
#else
  (void)seen_out_errors;
  (void)seen_out_done;
#endif

  if (f->match_n != offers_logged)
    log_offers(f);
  if (f->matches != seen_matches)
  {
    offered_at = nspire_ticks();
    if (!seen_matches)
    {
      plog("stage: the OS called the driver's match (%u so far), %u OS sleeps", (unsigned)f->matches,
           (unsigned)nspire_os_sleeps());
      log_diag("first match", 1);
      offer_logged_change = port_changes;
    }
    else if (offer_logged_change != port_changes && offer_logs < OFFER_LOGS)
    {
      offer_logs++;
      offer_logged_change = port_changes;
      plog("stage: the OS offered the driver a device after the port changed (%u offers so far), %u OS sleeps",
           (unsigned)f->matches, (unsigned)nspire_os_sleeps());
      log_diag("offer", 1);
    }
    seen_matches = f->matches;
  }
  if (f->attaches != seen_attaches)
  {
    if (!seen_attaches)
    {
      plog("stage: the OS called the driver's attach (%u so far), %u OS sleeps", (unsigned)f->attaches,
           (unsigned)nspire_os_sleeps());
      log_diag("first attach", 0);
    }
    seen_attaches = f->attaches;
  }
  if (f->synth_cfg != seen_synth)
  {
    if (!seen_synth)
      plog("stage: the OS's configuration descriptor wasn't the whole set (%u): the HID descriptor comes from the "
           "device",
           (unsigned)f->synth_cfg);
    seen_synth = f->synth_cfg;
  }
  if (f->in_errors != seen_in_err)
  {
    if (!seen_in_err)
      plog("stage: the first IN error: status %u (the pad's buttons are released; %u silent polls fail it)",
           (unsigned)f->last_in_status, USBPAD_OS_STALE_POLLS);
    seen_in_err = f->in_errors;
  }
  if (f->first_len && !first_report_logged)
    log_first_report(f);
}

/* nspire_link.c's 10-second heartbeat, while a pad is in (RDY-8): raw reports
 * against parsed ones, errors, the ring's drops, the OS time. */
int nspire_usbpad_heartbeat(char *line, unsigned size)
{
  static u32 last_reports;
  const struct usbpad_os_info *f;
  unsigned parsed = 0;
  int p;

  if (!started || !usbpad_count())
    return 0;
  f = usbpad_os_info();
  for (p = 0; p < USBPAD_PLAYERS; p++)
    parsed += usbpad_state(p)->reports;
  snprintf(line, size,
           "pad: reports %lu (+%lu), parsed %u, in_err %lu (last status %u), dropped %lu, %d polls, %u OS sleeps, %d forced, "
           "OS gap %u ms",
           (unsigned long)f->reports, (unsigned long)(f->reports - last_reports), parsed, (unsigned long)f->in_errors,
           (unsigned)f->last_in_status, (unsigned long)usbpad_dropped(), polls_taken, (unsigned)nspire_os_sleeps(),
           forced_ticks, (unsigned)(nspire_os_gap_longest() * 1000u / NSPIRE_TICK_HZ));
  last_reports = f->reports;
  return 1;
}
#else
static void watch_stages(void) {}
int nspire_usbpad_heartbeat(char *line, unsigned size)
{
  (void)line;
  (void)size;
  return 0;
}
#endif

/* R11u (2026-11-15): the device on the port is not a controller, and the
 * rest of its interfaces have had their offers (NOT_PAD_SETTLE_MS without a
 * new one): the driver kept an interface of it quietly (not_pad_now), or it
 * said 0 to every offer of it at match time (a sound card, a boot keyboard
 * or mouse: no class for it), interfaces offered and none taken (a device
 * offered at device level only is the driver's power verdict's, ~1.5 s).
 * Not while a pad is in or the driver posted an event about it (a pad that
 * failed says so itself). */
static int not_controller_settled(void)
{
  if (usbpad_count() || last_ev_set)
    return 0;
  if (not_pad_settled())
    return 1;
#ifdef USBPAD_API_LEVEL
  {
    const struct usbpad_os_info *f = usbpad_os_info();
    const struct usbpad_os_match *m;

    if (taken_since || not_pad_now || !iface_offered || (long)f->matches == matches_mark || !f->match_n)
      return 0;
    m = &f->match[(f->match_n - 1) % USBPAD_OS_MATCH_RING];
    return m->score == 0 && (u32)((u64)(nspire_ticks() - offered_at) * 1000 / NSPIRE_TICK_HZ) >= NOT_PAD_SETTLE_MS;
  }
#else
  return 0;
#endif
}

/* ...its try ends there, as one that worked out: no 12 s wait, no OS time
 * for it from the attach wait or the come-up, no "no pad came in". */
static void watch_not_controller(void)
{
  if (not_pad_done || !not_controller_settled())
    return;
  not_pad_done = 1;
  attach_left = 0;
  plog("try ended: the device is not a controller (%s): nothing failed, no more OS time for it",
       not_pad_now ? "the driver keeps it quietly" : "the driver said 0 to each of its offers");
  nspire_padlog_try_end(PL_TRY_NOT_PAD);
}

/* The wording by what the OS did (RDY-6): matches 0 = it offered nothing at
 * all; more = it offered, and the driver attached none (TI's own driver may
 * have won, or the attach failed: the diag's attach and match lines say). The
 * controller's registers too (RDY-3): a stopped controller or an unpowered
 * port reads there. */
static void gave_up(u32 bits)
{
  char usb[300];
  unsigned matches = 0, attaches = 0;

#ifdef USBPAD_API_LEVEL
  matches = (unsigned)usbpad_os_info()->matches;
  attaches = (unsigned)usbpad_os_info()->attaches;
#endif
  if (!matches)
    plog("OS offered no device in %u s: port %08x, %u OS sleeps, %u forced OS ticks", ATTACH_POLLS / POLLS_PER_SEC,
         (unsigned)bits, (unsigned)nspire_os_sleeps(), (unsigned)forced_ticks);
  else
    plog("OS offered the driver %u time(s) in %u s, %u attach(es), no pad came in: port %08x, %u OS sleeps, %u forced "
         "OS ticks",
         matches, ATTACH_POLLS / POLLS_PER_SEC, attaches, (unsigned)bits, (unsigned)nspire_os_sleeps(),
         (unsigned)forced_ticks);
  nspire_os_diag_usb(usb, sizeof(usb));
  plog("%s", usb);
  log_diag("gave up", 2);
  waited_out = 1;
  nspire_padlog_add(PL_ATTACH_OS, 0, 0, 0, (u32)forced_ticks, (u32)nspire_os_sleeps(), 0);
  nspire_padlog_try_end(PL_TRY_NO_OFFER);
}

/* nspire_usb_bits that say nothing is plugged in: a CX II's empty port
 * (the device's end, acting as one, B session ended, no B session), a CX's
 * (the ID pin's device end, B session ended, no session). */
static int nothing_bits(u32 bits)
{
  if ((bits >> 28) == 2)
    return (bits & 0x33u) == 0x31u;
  if ((bits >> 28) == 1)
    return (bits & 0x1Fu) == 0x11u;
  return 0;
}

static u32 offer_count(void)
{
#ifdef USBPAD_API_LEVEL
  return usbpad_os_info()->matches;
#else
  return (u32)diag_matches();
#endif
}

/* The device is gone: offers and the driver's word from now are the next one's. */
static void device_left(void)
{
  matches_mark = (long)offer_count();
  last_ev_set = 0;
  waited_out = 0;
  not_pad_now = not_pad_done = taken_since = iface_offered = 0;   /* R11u: the next device is its own */
  /* R10b: and the port is free for the next pad at once: a failed (or
   * underpowered) device's report goes with it, also where the port's bits
   * don't change (an adapter that stays in; the arbiter's OTHER held the
   * next pad off its own try then), and the halt watch starts over. */
  if (nspire_usbport_os() == PORT_OS_OFFER)
  {
    nspire_usbport_os_report(PORT_OS_NONE, 0, 0);
    device_seen = 0;
    nspire_arb_tick();   /* the port's owner now: a come-up for the next device in this poll asks it */
  }
  halted = 0;
#if USBPAD_API_LEVEL >= 6
  fail_text[0] = 0;
#endif
  /* PAD4: and the pad's own detach (the event that told of it going) is not
   * the next device's: device_seen held the arbiter's power restart off a
   * device that came onto the wires of an adapter that stayed in, where the
   * port's bits don't change (never latched as connected), for good. */
  device_seen = 0;
}

/* Whether the OS's hub work may be under way before the driver sees a
 * device (the top of this file): the polls after the port's bits changed,
 * while something's at the host's end, no pad's in and the link hasn't the
 * cable. */
static int attaching(void)
{
  u32 bits = nspire_usb_bits();

  if (bits != port_bits)
  {
    u32 now = nspire_ticks();
    /* PAD3: the CX II's OS switches an empty port's controller off and on
     * (~120 ms every 1.5 s: R9F): from nothing to nothing is no news here
     * (the pad log counts them). It filled the link log in under a minute. */
    int idle = nspire_usb_plugged() <= 0 && (!port_bits || nothing_bits(port_bits));

    if (!idle)
      plog("USB port %08x (%s) (was %08x), %u ms after the last change, %u OS sleeps, %u forced OS ticks", (unsigned)bits,
           nspire_usb_role_text(bits), (unsigned)port_bits, (unsigned)((u64)(now - port_changed_at) * 1000 / NSPIRE_TICK_HZ),
           (unsigned)nspire_os_sleeps(), (unsigned)forced_ticks);
    /* PAD3 (F1): the device left (nothing on the port, or the adapter's
     * end lost its connection: a controller or the other calculator's
     * cable out of an adapter that stays in): what's offered from now is
     * the next device's. Before, only a reading of 0 did, which a CX II's
     * empty port (20000031) isn't: the SEAM1 log's 8BitDo was held off the
     * power cycle by the link's own offer of the other calculator. */
    if (!bits || nothing_bits(bits) || ((port_bits & 0x100) && !(bits & 0x100)))
      device_left();
    port_changed_at = now;
    port_bits = bits;
    port_changes++;
    attach_left = ATTACH_POLLS;
    attach_quiet = 0;
    waited_out = 0;
    /* an event taken in this poll is about what's there now; (2026-11-17)
     * not a pad's own leaving: taken in the same poll as the port's change
     * it left "a device was seen" set for what came next, and the arbiter's
     * restart for a device the port never latched was refused for it
     * (pad_tests pad4_in_turn, when the two fell into one poll) */
    device_seen = events_now && !gone_poll;
  }
  if (!attach_left)
    return 0;
  if (usbpad_count())
  {
    device_seen = 1;
    attach_left = 0;
    return 0;
  }
  if (!--attach_left && usb_port() > 0 && !nspire_link_cable_present() && !nspire_usbhub_present())
  {
    gave_up(bits);
  }
  /* PAD3: the OS's time while a device comes up is wanted whatever the
   * link's flag says, but while linked (the link paces its frames itself).
   * The flag lasts until the watch's next look, which the menus never
   * make: a controller plugged in there after a calculator got no time. */
  return !attach_quiet && usb_port() > 0 && nspire_link_state() != LINK_LINKED;
}

/* PAD3: the driver's part of the Controller status (nspire_padstatus.h). */
void nspire_usbpad_status(struct pad_driver_view *d)
{
  int p;

  memset(d, 0, sizeof(*d));
  d->slot = -1;
  if (!started)
    return;
  d->started = init_result == 0 ? 1 : -1;
  snprintf(d->status, sizeof(d->status), "%s", usbpad_status());
  d->pads = (int8_t)usbpad_count();
  for (p = USBPAD_PLAYERS - 1; p >= 0; p--)
    if (usbpad_state(p)->connected)
    {
      d->slot = (int8_t)p;
      snprintf(d->name, sizeof(d->name), "%s", usbpad_kind_name(usbpad_state(p)->kind));
    }
#ifdef USBPAD_API_LEVEL
  {
    const struct usbpad_os_info *f = usbpad_os_info();
    d->matches = f->matches;
    d->attaches = f->attaches;
    if ((long)f->matches != matches_mark && f->match_n)
    {
      const struct usbpad_os_match *m = &f->match[(f->match_n - 1) % USBPAD_OS_MATCH_RING];
      d->offered = 1;
      d->vid = m->vid;
      d->pid = m->pid;
      d->iface = m->iface;
      d->cls = m->cls;
      d->sub = m->sub;
      d->proto = m->proto;
      d->score = m->score;
    }
  }
#endif
  if (last_ev_set)
  {
    d->event = last_ev.type;
    d->ev_vid = last_ev.vid;
    d->ev_pid = last_ev.pid;
  }
#if USBPAD_API_LEVEL >= 4
  if (d->pads || d->event == USBPAD_EV_UNSUPPORTED || d->event == USBPAD_EV_FAILED)
    usbpad_detail(d->slot, d->detail, sizeof(d->detail));
#endif
#if USBPAD_API_LEVEL >= 6
  /* R10b: the failed bring-up of the device on the port, in a few words
   * ("Switch Pro: OS gave no read pipe (in_open, ep 81, OS 11)") */
  if (!d->pads && fail_text[0])
    snprintf(d->detail, sizeof(d->detail), "%s", fail_text);
#endif
  /* R11u: a device the driver keeps quietly as not a controller (the
   * verdict's "not_a_pad", no toast) */
  d->not_pad = (uint8_t)not_pad_now;
  if (!d->pads && not_pad_now && !d->detail[0] && !last_ev_set)
    snprintf(d->detail, sizeof(d->detail), "Not a controller (%04x:%04x if %u)", (unsigned)not_pad_vid,
             (unsigned)not_pad_pid, (unsigned)not_pad_if);
#ifdef USBPAD_OS_HAS_SHUTDOWN_PENDING
  if (!d->pads && (usbpad_os_info()->abort_err || usbpad_os_info()->close_fail))
    snprintf(d->detail, sizeof(d->detail), "USB cleanup failed: quit and reopen gpSP");
  if (!d->detail[0] && d->offered && usbpad_os_info()->attach_err && usbpad_os_info()->quiet_attaches)
  {
    const struct usbpad_os_info *f = usbpad_os_info();
    static const char *const step[] = { "identify", "descriptors", "claim", "control", "start", "input", "ready" };
    snprintf(d->detail, sizeof(d->detail), "%04x:%04x: %s failed (%ld)", f->ifc_vid, f->ifc_pid,
             f->attach_step < 7 ? step[f->attach_step] : "USB", (long)f->attach_err);
  }
#endif
  d->gave_up = (u8)waited_out;
  for (p = 0; p < USBPAD_PLAYERS; p++)
    if (usbpad_state(p)->connected && !usbpad_state(p)->reports && (pads_on & (1u << p)) &&
        (u64)(nspire_ticks() - pad_on_at[p]) * 1000 >= (u64)PAD_SILENT_MS * NSPIRE_TICK_HZ)
      d->silent = 1;
  d->hub = (uint8_t)nspire_usbhub_present();
  d->hub_busy = (uint8_t)nspire_usbhub_busy();
  d->os_stuck = (uint8_t)os_stuck;
  cu_view(d);
}

/* PAD3: the status, every PADSTATUS_POLL_MS (a few register reads): its
 * verdict's changes into the pad log, the toast's clock, the pad log's
 * timer, and a try that ended with the device unplugged. */
/* The OS's USB has stopped (2026-11-16). Four hardware sessions ended the
 * same way (hardware logs: r10-r11u after a refused attach, r11-r11w and
 * r12-e7fee48 after the 8BitDo's unplug, 20261116-r13-hub after the hub
 * was pulled out with the sound playing): every device plugged in from then
 * on was reset and enabled by the OS and never offered to a driver, until
 * the OTG adapter itself came out; quit hung (r12) or the calculator froze
 * after gpSP's clean exit (r13-hub). What the port shows then is the same
 * in all four and in none of the 17 plugs that worked: the port enabled
 * with its connect-change bit still set (PORTSC 0000000f / 0000080f) for
 * good. On a plug that works the OS takes that bit ~50 ms after the port is
 * enabled, as it offers the device (0000080f at "the port is enabled",
 * 0000080d at "the OS offered it": r9-r11t 806712/806761, ui4-r11z
 * 333197/333245). The OS's own code says why the bit matters: it resets the
 * port first and reads the device's descriptors next, and takes the bit
 * only once that worked; so its requests to a new device no longer get
 * through.
 *
 * One such plug could be a device or a cable that doesn't answer. So: only
 * after the OS had offered something this session (its USB worked), and
 * once two plugs in a row (the wires' counter between them) each sat
 * STUCK_MS in all enabled with the bit set, the host controller running,
 * nothing offered (the time before and after the arbiter's own power
 * restart of that plug added up): os_stuck, until the OS offers anything
 * again. The Controller
 * page then says to take the adapter out and put it back (what ended it in
 * R11u), and quit() restarts the calculator rather than hand it back
 * frozen (nspire_usbpad_os_stuck; the restart is the exit's own, as for a
 * USB shutdown that timed out). A CX II's FOTG210 only: the older CX's port
 * word was never seen in this state. */
#define STUCK_MS    4000
#define STUCK_PLUGS 2
static int stuck_on, stuck_counted, stuck_plugs;
static u32 stuck_at, stuck_ms, stuck_offers, stuck_wires;

int nspire_usbpad_os_stuck(void)
{
  return started && os_stuck;
}

int nspire_usbpad_os_dead(void)
{
  return started && os_dead;
}

int nspire_usbpad_other_device(void)
{
  return started && not_pad_done && !usbpad_count();
}

/* ---- (2026-11-17) the OS's sleep of what nobody uses ----
 *
 * TI's USB stack suspends a hub with nothing active on it, and whatever no
 * driver uses, a second or two after its attach: the whole bus stops
 * (USBCMD 00080000, PORTSC 0000088d). A suspended hub has no status pipe:
 * a controller plugged into it then is never seen. And once the suspended
 * device is pulled out nothing wakes the transceiver again: the next
 * device is reset on it and never starts, until the OTG adapter itself is
 * pulled (nspire-usbshim usbshim_awake.h and nspire_usbcycle.c have the
 * OS's code; a hardware log of 2026-11-17: the hub offered
 * at 13.7 s, the bus suspended at 15.2 s, and from the hub's drop at 19 s
 * every plug for 270 s "connected, not enabled").
 *
 * watch_awake: the first time the adapter is seen at this calculator, the
 * OS's USB is kept awake for the run (usbpad_os_awake(1): three words of
 * the OS's code, on the OS build they were read from; stop() puts them
 * back as the exit's last USB step). Not before an adapter was ever in: a
 * run without USB changes nothing of the OS.
 *
 * watch_phy, a CX II at the host's end, each status poll:
 *  - the transceiver asleep with nothing on the port, or under a device
 *    that is connected and not enabled: woken (nspire_usb_phy_wake; never
 *    under a device the OS holds suspended, never in a reset). What was
 *    suspended before this run, or by an OS whose code is not the known
 *    one. A device that waited on it was reset in vain: the come-up's
 *    power restart starts it over (the arbiter told that this device is
 *    not one the OS works on).
 *  - the OS holds what is plugged in suspended with its controller stopped,
 *    and nothing of gpSP's has it (plugged in before gpSP's drivers were
 *    there: the OS found nobody for it): the same restart; its drop wakes
 *    the transceiver (nspire_usb_reenumerate), the OS starts over with the
 *    device and the drivers. Not with a hub known (its devices' business)
 *    and only once a plug. */
static void watch_awake(const struct pad_status *s)
{
#ifdef USBPAD_OS_HAS_AWAKE
  if (awake_asked || s->role != PS_ROLE_HOST)
    return;
  awake_asked = 1;
  awake_bits = usbpad_os_awake(1);
  nspire_padlog_add(PL_HC, 4, 0, 0, (u32)awake_bits, s->raw[2], s->raw[4]);
  nspire_padlog_milestone(PL_HC, 4, 0, 0, (u32)awake_bits, s->raw[2], s->raw[4]);
  plog(awake_bits == 7 ? "awake: the OS's USB is kept awake for this run (3 words of its code changed, put back at exit)" :
       awake_bits < 0 ? "awake: not the OS build whose code is known: nothing changed" :
       "awake: the OS's code changed only in part (bits %x)", (unsigned)awake_bits);
#else
  (void)s;
#endif
}

/* The exit's last USB step (stop(), after every driver's shutdown). */
static void awake_end(void)
{
#ifdef USBPAD_OS_HAS_AWAKE
  if (!awake_asked || awake_bits <= 0)
    return;
  awake_bits = usbpad_os_awake(0);
  awake_asked = 0;
  nspire_padlog_add(PL_HC, 5, 0, 0, (u32)awake_bits, 0, 0);
  plog("awake: the OS's code is put back (%d)", awake_bits);
#endif
}

int nspire_usbpad_awake(void)
{
  return awake_asked ? awake_bits : 0;
}

static void watch_phy(const struct pad_status *s)
{
  static u32 wires = (u32)-1;
  int mine;

  if (s->kind != 2 || s->role != PS_ROLE_HOST)
  {
    asleep_noted = 0;
    return;
  }
  if (nspire_padlog_wires_counter() != wires)
  {
    wires = nspire_padlog_wires_counter();
    if (!nspire_arb_cycling())
      asleep_noted = 0;   /* another plug */
  }
  mine = usbpad_count() || nspire_usbms_active() || nspire_usbaudio_active() || nspire_usbaudio_owns_usb() ||
         nspire_bt_usb_active() ||
         nspire_link_state() != LINK_OFF || nspire_link_cable_present();
  if (s->connected && s->enabled && s->suspended && !s->running && !s->resetting)
  {
    if (!asleep_noted && !mine && !nspire_usbhub_present())
    {
      asleep_noted = 1;
      phy_cycle_due = 1;
      nspire_padlog_add(PL_HC, 7, 0, 0, 0, s->raw[2], s->raw[4]);
      plog("the OS holds what is plugged in suspended (USBCMD %08lx PORTSC %08lx): the port's power restart starts it "
           "over with gpSP's drivers", (unsigned long)s->raw[2], (unsigned long)s->raw[4]);
    }
    return;
  }
  if (nspire_usb_phy_asleep() <= 0 || !nspire_usb_phy_wake())
    return;
  phy_wakes++;
  nspire_padlog_add(PL_HC, 6, s->connected != 0, 0, phy_wakes, s->raw[2], s->raw[4]);
  nspire_padlog_milestone(PL_HC, 6, s->connected != 0, 0, phy_wakes, s->raw[2], s->raw[4]);
  plog("the USB transceiver was asleep with %s (USBCMD %08lx PORTSC %08lx): woken (%lu)",
       s->connected ? "a device waiting on it" : "nothing on the port", (unsigned long)s->raw[2],
       (unsigned long)s->raw[4], (unsigned long)phy_wakes);
  if (s->connected && !mine)
  {
    nspire_arb_phy_woken();
    phy_cycle_due = 1;
  }
}

static void watch_stuck(const struct pad_status *s)
{
  u32 offers = offer_count(), now;
  int sig;

  if (offers != stuck_offers)
  {
    stuck_offers = offers;
    stuck_on = stuck_plugs = 0;
    stuck_ms = 0;
    if (os_stuck || os_dead)
    {
      os_stuck = os_dead = 0;
      plog("the OS offers devices again: its USB is back");
      nspire_padlog_add(PL_HC, 3, 0, 0, 0, s->raw[2], s->raw[4]);
    }
    return;
  }
  /* The adapter out (what the page says to do): what is plugged in after it
   * gets the OS's time again and is judged anew. Whether the OS's USB came
   * back is known only by an offer: until one comes quit still restarts
   * the calculator (os_dead). */
  if (s->role != PS_ROLE_HOST && (os_stuck || stuck_plugs))
  {
    if (os_stuck)
      plog("the adapter is out: the next plug gets the OS's time again and is judged anew");
    os_stuck = 0;
    stuck_on = stuck_plugs = 0;
    stuck_ms = 0;
  }
  /* (2026-11-17) and the other face of it (that hardware log of 2026-11-17:
   * 270 s of it): connected, the port's suspend bit set, never enabled,
   * the controller running, no reset under way */
  sig = s->kind == 2 && s->role == PS_ROLE_HOST && s->running && s->connected && !s->resetting &&
        (s->enabled ? (s->raw[4] & 2) != 0 : s->suspended != 0) &&
        !usbpad_count() && nspire_link_state() == LINK_OFF && !nspire_link_cable_present();
  if (nspire_padlog_wires_counter() != stuck_wires)
  {
    stuck_wires = nspire_padlog_wires_counter();
    /* another plug is counted on its own; the arbiter's power restart
     * brings the same device back (its time before and after add up: a
     * restart comes ~2 s into a plug nothing was offered for) */
    if (!nspire_arb_cycling())
      stuck_ms = stuck_counted = 0;
    stuck_on = 0;
  }
  if (!sig)
  {
    stuck_on = 0;
    return;
  }
  now = nspire_ticks();
  if (stuck_on)
  {
    u32 ms = (u32)((u64)(now - stuck_at) * 1000 / NSPIRE_TICK_HZ);
    stuck_ms += ms > 1000 ? 1000 : ms;   /* (a long gap between two looks counts as one second) */
  }
  stuck_on = 1;
  stuck_at = now;
  if (stuck_counted || stuck_ms < STUCK_MS)
    return;
  stuck_counted = 1;
  stuck_plugs++;
  plog("the port is %s, nothing offered in %u ms (PORTSC %08lx): plug %d of this kind in a row",
       s->enabled ? "enabled with its connect change never taken" : "connected and suspended, never enabled", STUCK_MS,
       (unsigned long)s->raw[4], stuck_plugs);
  if (stuck_plugs >= STUCK_PLUGS && offers && !os_stuck)
  {
    os_stuck = os_dead = 1;
    plog("the OS's USB has stopped: it enables the port and its requests to the device don't get through (%d plugs in "
         "a row); the OTG adapter out and in again restarts it; quit restarts the calculator", stuck_plugs);
    nspire_padlog_add(PL_HC, 2, 0, 0, (u32)stuck_plugs, s->raw[2], s->raw[4]);
    nspire_padlog_milestone(PL_HC, 2, 0, 0, (u32)stuck_plugs, s->raw[2], s->raw[4]);
  }
}
/* (r19-usb) The adapter's power. A CX II log (567381a): the adapter's end
 * at 211730 ms with OTGCSR
 * 00012e10 (A_BUS_REQ set, A_BUS_DROP clear: the power turned on; A VBUS and
 * both sessions off, B session end: below 0.8 V) for 8.8 s and 236 OS
 * sleeps, then on, a hub on it 366 ms later; the pad log called the power's
 * coming "the device was unplugged with nothing working". The earlier logs'
 * adapters came up from A_BUS_DROP set in 108-440 ms. So: power turned on and
 * still off PADSTATUS_STABLE_MS later is said once (a milestone: what is on
 * the adapter draws more than the port gives, or its supply failed), its
 * coming up after that is said with its time and ends the try as that, and
 * the OS's own start of the port (the power not yet asked for, then on) is
 * said with its time and ends nothing. Returns the verdict the power was off
 * under when it came on now (0: it didn't). */
static int power_off_was, power_held_said;
static u32 power_off_at, power_off_sleeps;

static int watch_power(const struct pad_status *s, enum pad_verdict v)
{
  u32 ms;
  int was;

  if (s->role == PS_ROLE_HOST && !s->power && (v == PV_NO_POWER || v == PV_POWER_HELD))
  {
    if (!power_off_was)
    {
      power_off_at = nspire_ticks();
      power_off_sleeps = nspire_os_sleeps();
      power_held_said = 0;
    }
    if (!power_off_was || v == PV_POWER_HELD)
      power_off_was = v;
    ms = (u32)((u64)(nspire_ticks() - power_off_at) * 1000 / NSPIRE_TICK_HZ);
    if (v == PV_POWER_HELD && !power_held_said && ms >= PADSTATUS_STABLE_MS)
    {
      power_held_said = 1;
      nspire_padlog_add(PL_POWER, 1, 0, 0, ms, s->raw[0], 0);
      nspire_padlog_milestone(PL_POWER, 1, 0, 0, ms, s->raw[0], 0);
    }
    return 0;
  }
  was = power_off_was;
  power_off_was = 0;
  if (!was || s->role != PS_ROLE_HOST || !s->power)
    return 0;
  ms = (u32)((u64)(nspire_ticks() - power_off_at) * 1000 / NSPIRE_TICK_HZ);
  nspire_padlog_add(PL_POWER, was == PV_POWER_HELD ? 2 : 3, 0, 0, ms, nspire_os_sleeps() - power_off_sleeps, 0);
  nspire_padlog_milestone(PL_POWER, was == PV_POWER_HELD ? 2 : 3, 0, 0, ms, nspire_os_sleeps() - power_off_sleeps, 0);
  return was;
}


static void status_poll(void)
{
  struct pad_status s;
  enum pad_verdict v;
  int powered_up;

  if (status_polled && nspire_ticks() - status_at < (u32)PADSTATUS_POLL_MS * (NSPIRE_TICK_HZ / 1000))
    return;
  status_polled = 1;
  status_at = nspire_ticks();
  nspire_padstatus_read(&s);
  watch_awake(&s);
  watch_phy(&s);
  watch_halt(&s);
  watch_stuck(&s);
  s.drv.os_stuck = (uint8_t)os_stuck;
  if (!last_ev_set)
    s.drv.event = 0;   /* (the halt watch may have just let the failure's word go) */
  v = nspire_padstatus_verdict(&s);
  nspire_padstatus_note(&s, v);
  powered_up = watch_power(&s, v);
  if (v != verdict_was)
  {
    /* R2: the OS's idle off-and-on (nothing <-> unreadable, a CX II
     * every ~1.5 s, a CX CAS every ~0.1 s) is the pad log's counted idle
     * line, not a verdict line: one here between its flips broke that
     * line's merge, and the ring churned the lines that matter out. */
    if (!((v == PV_NOTHING || v == PV_UNREADABLE) && (verdict_was == PV_NOTHING || verdict_was == PV_UNREADABLE)))
      nspire_padlog_add(PL_VERDICT, v, verdict_was, 0, 0, 0, 0);
    /* (r19-usb) the adapter's power coming on is no unplug: by the OS's own
     * start nothing ends; after it was held down, the try ends as that */
    if (powered_up == PV_POWER_HELD)
      nspire_padlog_try_end(PL_TRY_POWER_UP);
    else if (!powered_up && nspire_padstatus_failing(verdict_was) && verdict_was != PV_EMPTY_ADAPTER &&
             (v == PV_NOTHING || v == PV_UNREADABLE || v == PV_EMPTY_ADAPTER || v == PV_DEVICE_END))
      nspire_padlog_try_end(PL_TRY_UNPLUGGED);
    verdict_was = v;
  }
  /* R3: failing for the pad log's timer only as for the toast: not
   * while Link is picked or the other calculator is seen (a write then is in
   * a link's come-up). */
  /* R11t: a working sound adapter is not "plugged in, nothing working" */
  nspire_padlog_failing(nspire_padstatus_failing(v) && s.link == LINK_OFF && !s.link_cable && !nspire_usbaudio_active());
}

/* ---- PAD4: the come-up, from the plug to the pad's first report ----
 *
 * R10a's CX II (its log of 2026-11-14, gpsp_pad.txt.tns + the link
 * log's OS sleep counts): once its host controller ran, every step of the
 * OS's enumeration went by the OS's own running time, not the clock: the
 * port's reset took ~13 OS sleeps both when they came 1.5 a second (the
 * 8BitDo: 9.0 s, sleeps 1066 -> 1079) and ~20 a second (the Switch Pro:
 * 0.58 s); the offer, the attach and the first report 1, 3 and 7 sleeps
 * after the port was enabled. gpSP gave the OS that time a tick at a time,
 * and after its 12 s wait almost none. So, the moment a device shows on the
 * wires at the adapter's end and nothing has it: a message, then at the
 * next point between frames (or in the menus at once) the OS alone for up
 * to CU_BURST_MS (CU_BURST_MAX_MS while its end of the port moves): one OS
 * tick after another with nothing of gpSP's between them, the game stood
 * still, until the pad's first report (or the driver's answer, the device
 * gone, a key). After that, while nothing answered, the OS's tick on every
 * poll for CU_WANT_MS (it was 12 s of polls after each port change).
 *
 * What started the stopped host controller is not known (it came 44 s and
 * 92 s after a device was on the wires here, 46-53 s in R9F, after ~690 to
 * ~1360 OS sleeps: neither the clock nor a count of our sleeps; none of the
 * nine power restarts started it within 17 s): the come-up gives the OS its
 * time and says, step by step, when each thing happened (the pad log's
 * "come-up:" lines, the Controller page's "Since plug" line). The one power
 * restart the log proves useful (the port showed a device on its wires but
 * never latched it as connected: PORTSC 00000800 / 0000088a; both such
 * restarts latched it within 0.26 s) is made at once (CU_NOCONN_MS), not
 * after the arbiter's 3 s; a restart while the OS works on a connected
 * device (it set the Switch Pro back twice) is never made here
 * (nspire_arbiter.c restart_helps). */
#define CU_BURST_MS     3000
#define CU_BURST_MAX_MS 5000
/* R11t (2026-11-15): with a device on the wires at the adapter's end and the
 * OS's host controller NOT running, the burst goes on until the controller
 * runs (then CU_BURST_MS..CU_BURST_MAX_MS more, counted from that moment, for
 * the hub's reset and the offer), at most CU_HC_WAIT_MS (and CU_HC_WAIT_SLEEPS
 * OS sleeps, should the timer be wrong). Why: R8's CX II (a sweep's
 * log of 2026-11-14) gave the OS 3001 ms (293 sleeps) in one go, then a tick
 * now and then: +952 sleeps in 29 s and the controller never ran; the music
 * player (nspire-usbaudio, same calculator, the OS's time all along) saw it
 * run ~5.9 s (~650 OS sleeps) after the plug. The bound: 15 s is 2.5 times
 * that, ~1460 OS sleeps at the CX II's 10.25 ms tick, more than the +1280 /
 * +1362 sleeps after which R9F's and r5's CX II started it spread over 46-53 s;
 * past it the OS is not going to start it for this device (PAD3's R9F
 * adapter: never) and the game must go on. A key ends it sooner. */
#define CU_HC_WAIT_MS   15000
#define CU_HC_WAIT_SLEEPS (CU_HC_WAIT_MS / 2)   /* the CX's tick is 2.98 ms: 5000 sleeps in 15 s */
#define CU_NOTICE_MS    700    /* in the menus: the box over them once the wait is this long */
#define CU_MOVING_MS    500    /* the burst goes on past CU_BURST_MS while the port moved this recently, or the OS works on it */
#define CU_IDLE_MS      800    /* enabled, running, nothing offered, nothing moving: the OS is done with it (S4's case) */
#define CU_IDLE_MIN_MS  1000
#define CU_NOCONN_MS    300    /* a device on the wires, never latched as connected: the power restart */
#define CU_NOCONN_GIVE_MS 2000 /* ...and still not latched this long after it: the come-up ends (the arbiter's word) */
#define CU_REPORT_MS    300    /* the burst waits this long after the attach for the first report */
#define CU_WANT_MS      60000
#define CU_TEXT         "USB device in: connecting..."   /* DETECT's own words (nspire_link.c BRING_DEVICE_TEXT) */
#define CU_NOTICE_TEXT  "Waiting for USB to start. Any key: stop."   /* R11t: the menus' box */

static struct
{
  int on;              /* under way */
  int at_start;        /* the device was there at gpSP's start */
  int msg;             /* the message posted (it was in a frame by the next point) */
  int burst, restart;  /* done: the burst, the power restart */
  int outcome;         /* PL_CU_*: how it ended */
  u32 at, sleeps;      /* the plug's ticks, the OS sleeps then */
  u32 offers;          /* offer_count() before the device came */
  u32 step[PADSTATUS_STEPS];   /* ms after the plug + 1; 0 not seen */
  u32 sig, moved_at;   /* the port's run/connect/enable/reset bits, their last change */
  uint32_t portsc;     /* the last PORTSC read */
  u32 burst_ms, burst_sleeps;
  u32 restart_ms;      /* when the restart was made (or found not possible) */
} cu;
static u32 cu_wires = (u32)-1, cu_gone;
static int bursting, first_polls, cu_any;

static u32 cu_ms(void)
{
  return (u32)((u64)(nspire_ticks() - cu.at) * 1000 / NSPIRE_TICK_HZ);
}

static void cu_step(int i)
{
  if (cu.step[i])
    return;
  cu.step[i] = cu_ms() + 1;
  nspire_padlog_add(PL_STEP, i, 0, 0, cu.step[i] - 1, nspire_os_sleeps() - cu.sleeps, cu.portsc);
  /* R12: the timer block when the device shows connected and when the OS
   * resets the port (the OS's USB tick, 900C0020, counted ~5 ms): the next
   * hardware run says how fast it counts while gpSP runs */
  if (i == CU_CONN || i == CU_RESET)
  {
    uint32_t w[6];

    nspire_timer_usb_probe(w);
    nspire_padlog_add6(PL_TIMER, i, 0, 0, w);
  }
}

static void cu_end(int outcome)
{
  if (!cu.on)
    return;
  cu.on = 0;
  cu.outcome = outcome;
  nspire_padlog_add(PL_COMEUP, outcome, cu.at_start, cu.burst ? 1 : 0, cu_ms(), nspire_os_sleeps() - cu.sleeps,
                    cu.burst_ms);
}

/* Nothing in the way of the OS's time in one go: the link is off and no
 * calculator seen, the fast transport doesn't hold the controller, the
 * adapter's end, no pad in, nobody else owns the port. A hardware timer
 * must bound the burst: the fallback clock only moves between frames.
 * Without it, the normal per-frame OS sleeps continue discovery. */
static int cu_may(void)
{
  enum arb_owner o = nspire_arb_owner();

  /* (2026-11-16: not with the OS's USB found stopped: no time brings an offer then) */
  return started && nspire_link_state() == LINK_OFF && !nspire_link_cable_present() && !link_cable_fast_up() &&
         usb_port() > 0 && !usbpad_count() && (o == ARB_WAIT || o == ARB_NONE) && nspire_os_ready() && nspire_timer_runs() &&
         !os_stuck;
}

/* A device came onto the wires at the adapter's end (the pad log's counter:
 * the lines J/K or connected): a new come-up, nothing of the one before. */
static void cu_begin(void)
{
  memset(&cu, 0, sizeof(cu));
  cu.on = cu_any = 1;
  cu.at_start = first_polls < 3;
  cu.at = cu.moved_at = nspire_ticks();
  cu.sleeps = nspire_os_sleeps();
  cu.offers = (u32)matches_mark;
  cu.sig = (u32)-1;
  cu_gone = nspire_padlog_wires_gone();
  nspire_padlog_add(PL_COMEUP_BEGIN, cu.at_start, 0, 0, 0, 0, 0);
}

/* R10a fact 3, as the pad log reads it: 68 ms after the Switch Pro's failed
 * attach the OS stopped its host controller (USBCMD 00080000, the port
 * enabled and suspended) and offered no device again for the rest of the
 * run. The driver no longer refuses a pad it matched (38aa2c1), so this
 * should not come back; if it does (a device no driver keeps), the port is
 * the next pad's at once: the failure's report goes, the verdict is the
 * stopped host's, and a come-up gives the OS its time in one go as for a
 * new plug (the fast-connect work, PAD4). UNVERIFIED that the OS starts its
 * host again then: R10a's ran again only 98 s later. The pad log's PL_HC
 * lines say when it stopped and when it ran again. Every status poll (a
 * few compares). */
static void watch_halt(const struct pad_status *s)
{
  u32 now = nspire_ticks();
  unsigned ms;

  if (!halted)
  {
    /* (not a drive's or a sound adapter's, nor the other calculator's: the
     * port is theirs, and their drivers' or the link's business) */
    if ((long)offer_count() == matches_mark || usbpad_count() || s->role != PS_ROLE_HOST || !s->kind || s->running ||
        !(s->connected || s->lines) || nspire_usbms_active() || nspire_usbaudio_active() || nspire_usbaudio_owns_usb() ||
        nspire_bt_usb_active() || nspire_link_state() != LINK_OFF || nspire_link_cable_present())
      return;
    halted = 1;
    halted_at = now;
    ms = (unsigned)((u64)(now - offered_at) * 1000 / NSPIRE_TICK_HZ);
    nspire_padlog_add(PL_HC, 1, 0, 0, ms, s->raw[2], s->raw[4]);
    plog("the OS's host controller stopped %u ms after it offered a device no pad came of (USBCMD %08lx PORTSC %08lx): "
         "the port is free, a come-up gives the OS its time", ms, (unsigned long)s->raw[2], (unsigned long)s->raw[4]);
    if (nspire_usbport_os() == PORT_OS_OFFER)
      nspire_usbport_os_report(PORT_OS_NONE, 0, 0);
    last_ev_set = 0;
    device_seen = 0;
    matches_mark = (long)offer_count();
    nspire_arb_tick();   /* the port's owner now, not at the next tick: the come-up below asks it */
    /* a come-up whose OS time in one go is spent (or none): a new one, its
     * burst at the next point (inside a burst, that burst goes on: never
     * the state under its feet) */
    if (started && !bursting && (!cu.on || cu.burst || cu.step[CU_OFFER]))
    {
      cu_end(PL_CU_DRIVER);
      cu_begin();
    }
    return;
  }
  if (halted == 1 && s->running)
  {
    halted = 2;
    ms = (unsigned)((u64)(now - halted_at) * 1000 / NSPIRE_TICK_HZ);
    nspire_padlog_add(PL_HC, 0, 0, 0, ms, s->raw[2], s->raw[4]);
    plog("the OS's host controller runs again, %u ms after it stopped", ms);
  }
}

/* Each poll: the steps seen, and how it ends. A few compares, the six raw
 * words only while one is under way. */
static void cu_poll(void)
{
  uint32_t w[NSPIRE_USB_RAW];
  int p, kind;

  if (nspire_padlog_wires_counter() != cu_wires)
  {
    int first = cu_wires == (u32)-1;

    cu_wires = nspire_padlog_wires_counter();
    /* (not our own restart's drop and return: the same device) */
    if (!(first && !cu_wires) && !nspire_arb_cycling() && usb_port() > 0 && nspire_link_state() == LINK_OFF &&
        !nspire_link_cable_present())
      cu_begin();
  }
  if (!cu.on)
    return;
  /* hub: its own come-up is over at its offer (hub_begin), also when the
   * wires' counter is read after it: what it carries comes by the OS's
   * offers (hub_poll), not by this port's lines */
  if (nspire_usbhub_present())
  {
    cu_end(PL_CU_HUB);
    return;
  }
  kind = nspire_usb_raw(w);
  if (kind)
  {
    u32 sig = (w[2] & 1) | (w[4] & 0x105) << 1;

    cu.portsc = w[4];
    if (sig != cu.sig)
    {
      cu.sig = sig;
      cu.moved_at = nspire_ticks();
    }
    if (w[2] & 1)
      cu_step(CU_HC);
    if (w[4] & 1)
      cu_step(CU_CONN);
    if (w[4] & 0x100)
      cu_step(CU_RESET);
    if (w[4] & 4)
      cu_step(CU_ENABLED);
  }
  if ((long)offer_count() != (long)cu.offers)
    cu_step(CU_OFFER);
  if (usbpad_count())
    cu_step(CU_ATTACH);
  for (p = 0; p < USBPAD_PLAYERS; p++)
    if (usbpad_state(p)->connected && usbpad_state(p)->reports)
    {
      cu_step(CU_REPORT);
      cu_end(PL_CU_WORKS);
      return;
    }
  if (last_ev_set && last_ev.type != USBPAD_EV_CONNECTED && last_ev.type != USBPAD_EV_DISCONNECTED &&
      last_ev.type != USBPAD_EV_REPLUG_NEEDED)
    cu_end(PL_CU_DRIVER);
#ifdef USBPAD_API_LEVEL
  else if (cu.step[CU_OFFER] && !usbpad_count() && usbpad_os_info()->match_n &&
           usbpad_os_info()->match[(usbpad_os_info()->match_n - 1) % USBPAD_OS_MATCH_RING].score == -2)
    cu_end(PL_CU_CALC);   /* the other calculator (refused, TI's): the link's */
#endif
  else if (nspire_padlog_wires_gone() != cu_gone && !nspire_arb_cycling())
    cu_end(PL_CU_GONE);
  else if (nspire_link_state() != LINK_OFF || nspire_link_cable_present() || nspire_arb_owner() == ARB_LINK)
    cu_end(PL_CU_LINK);
  else if (nspire_arb_owner() == ARB_OTHER)
    cu_end(PL_CU_OTHER);
  else if (not_pad_done)
    cu_end(PL_CU_NOT_PAD);   /* R11u: not a controller: no more OS time for it */
  else if (cu_ms() >= CU_WANT_MS)
    cu_end(PL_CU_TIMEOUT);
  else if (usb_port() <= 0)
    cu_end(PL_CU_GONE);
  /* the OS is done with an enabled port it offered nothing for (S4's device,
   * there before the driver registered): the arbiter's restart is next */
  else if ((cu.sig & 0xB) == 0xB && !cu.step[CU_OFFER] && cu_ms() >= CU_IDLE_MIN_MS &&
           (u32)((u64)(nspire_ticks() - cu.moved_at) * 1000 / NSPIRE_TICK_HZ) >= CU_IDLE_MS)
  {
    cu_end(PL_CU_IDLE);
    /* 2026-11-16: and the OS's tick on every poll for the rest of the 12 s
     * ends with it: the OS is done with this port (the arbiter's restart,
     * if it makes one, moves the port's bits and starts them over). The
     * wait itself runs on to its "no device offered in 12 s" (gave_up). */
    attach_quiet = 1;
  }
  /* on the wires, never latched as connected, the restart made (or not
   * possible) and still nothing: the device's or the cable's (the arbiter's
   * last resort says so) */
  else if (cu.restart && !(cu.portsc & 1) && cu_ms() >= cu.restart_ms + CU_NOCONN_GIVE_MS)
    cu_end(PL_CU_NOCONN);
}

/* PAD4: the OS has the device (connected) and hasn't enabled it with its
 * controller running: the come-up waits for it, its message kept on the
 * screen; the arbiter's last resort and the page's toast wait too (a replug
 * would start the OS's wait over). */
int nspire_usbpad_coming_up(void)
{
  return cu.on && !usbpad_count() && (cu.sig & 2) && (cu.sig & 0xB) != 0xB;
}

/* The OS's tick on every poll while a come-up waits for the OS (not while
 * linked: the link paces its frames itself). */
static int cu_wants(void)
{
  return cu.on && !usbpad_count() && nspire_link_state() != LINK_LINKED;
}

/* The one power restart the hardware showed working: the wires show a
 * device (J or K), the port never latched it as connected, CU_NOCONN_MS
 * after the plug, nothing offered. Only between frames or in the menus (a
 * restart blocks ~0.8 s in OS sleeps). The arbiter's guards decide. */
static void cu_restart_point(void)
{
  if (!cu.on || cu.restart || cu.step[CU_OFFER] || cu_ms() < CU_NOCONN_MS || (cu.portsc & 1) || !(cu.portsc & 0xC00))
    return;
  cu.restart = 1;
  nspire_arb_comeup_cycle();
  cu.restart_ms = cu_ms();
}

/* R11t: a device on the wires at the adapter's end (the lines J or K, or
 * connected: PORTSC 11:10 or bit 0) and the OS's host controller not running
 * (USBCMD bit 0), as the last poll read them. */
static int cu_hc_waiting(void)
{
  return cu.on && cu.sig != (u32)-1 && !(cu.sig & 1) && (cu.portsc & 0xC01u) != 0;
}

/* R11t: in the menus (the file list, the menu) nothing is drawn while the
 * burst runs: a box over the screen as it is (ui_fb holds what is shown)
 * while the OS is waited for, the pixels under it put back after. Returns
 * whether it is up. */
#define NOTICE_X 16
#define NOTICE_Y 88
#define NOTICE_W 288
#define NOTICE_H 60
static u16 notice_under[NOTICE_W * NOTICE_H];

static int menu_notice(int on)
{
  int y;

  if (ui_defer_present)
    return 0;   /* a dialog's ground being drawn: not now */
  if (on)
  {
    for (y = 0; y < NOTICE_H; y++)
      memcpy(notice_under + y * NOTICE_W, ui_fb + (NOTICE_Y + y) * UI_W + NOTICE_X, NOTICE_W * sizeof(u16));
    ui_rect(NOTICE_X, NOTICE_Y, NOTICE_W, NOTICE_H, UI_ACCENT);
    ui_clip(NOTICE_X + 4, NOTICE_X + NOTICE_W - 4);
    ui_text_center(UI_W / 2, NOTICE_Y + 24, CU_TEXT, &ui_font_small_strong, UI_FOCUS_INK);
    ui_text_center(UI_W / 2, NOTICE_Y + 44, CU_NOTICE_TEXT, &ui_font_small, UI_FOCUS_INK);
    ui_clip(0, UI_W);
  }
  else
    for (y = 0; y < NOTICE_H; y++)
      memcpy(ui_fb + (NOTICE_Y + y) * UI_W + NOTICE_X, notice_under + y * NOTICE_W, NOTICE_W * sizeof(u16));
  ui_present();
  return on;
}

/* ---- A USB hub on the port (2026-11-16) ----
 *
 * Everything above reads one device's story off the root port's lines: a
 * device came onto the wires, the OS reset the port, offered it, it left
 * the wires. With a hub on the adapter the root port says one thing for as
 * long as the hub stays (connected, enabled, the host controller running)
 * and nothing of what is plugged into or pulled out of the hub's ports. The
 * calculator's log of a hub (r13's hub run of 2026-11-16, CX II, OS
 * 6.4.0.74) shows what does tell:
 *  - TI's OS offers the hub itself to every registered driver at device
 *    level before its own hub driver takes it ("OS offer #46: 214b:7260
 *    device 09/00/01"), and then every device behind it as it would on the
 *    root port (device level, then each interface). The pad driver counts
 *    the hub offers (usbpad_os_info()->hubs) and all offers (matches); all
 *    three of gpSP's drivers see the same offers.
 *  - A USB sound adapter behind the hub played without one error for
 *    minutes; three controllers in turn on the root port worked.
 *  - A controller plugged into the hub while the sound played was offered,
 *    and its attach then sat 8.3 s in the OS's usbd_open_pipe_intr, which
 *    returned "timed out" (the OS sends the device a CLEAR_FEATURE(HALT)
 *    there, with its 5 s limit: read from its code). The OS had ~22 ticks a
 *    second between the game's frames in that time. On the root port the
 *    same open takes no time: there the come-up gave the OS the CPU in one
 *    go, because the port's lines had moved.
 *  - When the hub was then pulled out with the sound still playing, the OS
 *    offered nothing ever again and the calculator froze after gpSP's exit.
 *
 * So, while a hub is in (from its offer until the port's device leaves the
 * wires or the port reads empty):
 *  - no power restart of the port, by anyone (nspire_usbhub_present, the
 *    arbiter's veto): it cuts the power of everything on the hub;
 *  - the OS's tick at least every HUB_TICK_FORCE (HUB_TICK_QUIET once
 *    nothing came or went for HUB_QUIET_MS), whoever else wants it or not:
 *    the hub's port changes reach the OS's USB task only in its time;
 *  - when the OS shows it is at work on a hub's device (an offer, an
 *    attach or a detach counted, or a callback of one of the three drivers
 *    inside the OS right now), the OS alone, one tick after another, until
 *    HUB_SETTLE_MS after the last such sign (hub_burst: HUB_BURST_MS a go,
 *    HUB_EPISODE_MS for one stretch of work; a key ends it), as the
 *    come-up does for the root port: an attach's or a detach's requests
 *    then run in the OS's own time, not a tick at a time against a 5 s
 *    limit;
 *  - a pad that leaves a hub port takes what was said of it with it
 *    (device_left: the root port's bits never do that here), so the next
 *    one is a new device and no "USB problem" is said of an empty hub;
 *  - the hub's own come-up ends at its offer, without the 12 s wait for "a
 *    pad" and its "no pad came in".
 * A hub the OS does not offer (UNVERIFIED on the older CX's OS 4.5) is
 * told by what only a hub allows: a device offered at device level while
 * another one is attached (one port carries one device).
 * UNVERIFIED on hardware: all of it but the log's facts above. The pad
 * log's "hub:" lines say what happened. */
#ifdef USBPAD_OS_HAS_HUB
#define HUB_SETTLE_MS   300
#define HUB_BURST_MS    2000
#define HUB_EPISODE_MS  8000
#define HUB_TICK_FROM   (NSPIRE_TICK_HZ / 30)
#define HUB_TICK_FORCE  (NSPIRE_TICK_HZ / 10)
#define HUB_TICK_QUIET  (NSPIRE_TICK_HZ / 4)   /* ...every 250 ms once the hub has been quiet HUB_QUIET_MS */
#define HUB_QUIET_MS    30000
#define HUB_DEAD_MS     700    /* (2026-11-17) the port under the hub not the OS's working port this long: hub_poll */
/* (2026-11-17, final/usb7) A port of the hub changed: the OS's own flag for
 * it (usbpad_os_explore_pending: TI's hub driver sets it where the hub's
 * status pipe completes, its explore task clears it when it starts). From
 * its last sighting the OS's tick comes on every poll for HUB_NEWS_MS, as
 * while the OS is at work on a device (hub_active): the explore task needs
 * some 20 to 40 runs from a device's plug to its first offer (the reset,
 * the address, the descriptors), and a first offer makes hub_active. This
 * is what finds a controller plugged into the hub while a sound adapter
 * streams: the glue's own hub tick is off then (final/pace6: the sound's
 * sleeps give the OS its task time, one every 250 ms at the most, which is
 * 5 to 10 s for those runs), and so is its USBSTS rule (the sound's
 * completions hold bit 0). The flag says a hub's port changed and nothing
 * else: no cost while nothing is plugged or pulled. */
#define HUB_NEWS_MS     2500
/* ...and a flag that stays set this long through all those ticks is not
 * news any more (the OS's explore task does not run: nothing the ticks can
 * help): no more of them for it until it has read clear once. */
#define HUB_NEWS_STUCK_MS 10000
/* (2026-11-17, final/usb8) The hub that says nothing any more. An r15
 * hardware run (a CX II's log): an 8BitDo worked on the
 * hub for 197 s and was pulled; a PlayStation and a Switch controller
 * plugged in after it: no news from the hub in 93 s, no offer, while the
 * sound adapter on the same hub played on. Whether the hub's status pipe
 * died or the controllers never showed on its ports (power) the log could
 * not say. So, while a hub is in (hub_watch):
 *  - every HUB_WATCH_MS with nothing else going on the OS is asked to
 *    explore (usbpad_os_hub_explore: TI's own call, what a hub's status
 *    pipe triggers): its USB task then asks every port of the hub itself.
 *    The flag it sets is ours (hub_forced): not "news" until read clear;
 *  - every HUB_TABLE_MS the hub's ports are read as the OS's hub driver
 *    last saw them (usbpad_os_hub_read) and written to the pad log when
 *    they change: the next log says whether a controller that is not found
 *    showed on its port at all;
 *  - a port that is connected with no device made of it yet (or holds a
 *    change the OS has not taken) is news as the flag is: the OS's tick on
 *    every poll, for HUB_PORT_GIVE_MS a port at the most; past that the
 *    device does not start (said once, on screen and in the log), and no
 *    more ticks for it until the port changes;
 *  - the hub's over-current bit is said once, on screen and in the log. */
#define PAD_QUIET_MS      500    /* (2026-11-17) no report this long with sound streaming: the pad's own tick after all */
/* (2026-11-17, final/usb8) The pad's interrupt windows. A controller's
 * reports complete in the controller's interrupt, in the interrupt itself
 * (R11u's probe, nspire_audio.c irq_window: the sound's completions run
 * there before the code goes on). So a controller needs the CPU's
 * interrupts on for a moment, not the OS's task: at each poll a window
 * (pad_irq_window: the interrupts unmasked for a few instructions; what is
 * pending is taken at once), and the OS's task time (a controller's unplug,
 * a hub's port) by a sleep every PAD_TASK_MS only. The forced tick every 33
 * ms was ~30 sleeps and ~150 ms of each second: at 2x speed with a picture
 * at each refresh there is no wait for them to fall into.
 * Checked as it goes: if PADWIN_GIVE sleeps in a row brought reports and no
 * window ever did (an OS on which the completions do not run in the
 * interrupt), the windows end for the run and the tick every 33 ms is back
 * (said in the pad log). Not while a sound adapter streams (its own
 * windows and sleeps take the interrupt: above), not while the OS is at
 * work on a device (attach). */
#define PAD_TASK_MS       250
#define PADWIN_GIVE       3
#define HUB_WATCH_MS      2000
#define HUB_TABLE_MS      250
#define HUB_PORT_GIVE_MS  8000
/* (one line of 63 characters at the most: OSD_TEXT_MAX; the UI's words) */
#define HUB_NOSTART_TEXT  "A hub device didn't start: try another port"
#define HUB_OVERCUR_TEXT  "Hub draws too much current: use a powered hub"
#define HUB_TEXT        "USB hub: connecting..."
/* (2026-11-18, Hub 5) The hub gone dead under the OS. An r16 hardware run
 * (a CX II's log): a bus-powered hub with a
 * drive, a Switch controller, a sound adapter and the Bluetooth dongle; at
 * 153987 ms the port's power fell for ~100 ms (OTGCSR A_VBUS off, OTGISR
 * 00000420: the A-device's VBUS error and over-current), the root port
 * stayed connected and enabled (no connect change: the OS was told
 * nothing), the sound stalled at once, the looks gpSP asked of the OS
 * every 2 s were not taken for 10 s four times in 140 s (its explore runs
 * again and again, each run tens of seconds: the hub's port requests time
 * out, read from TI's code: the hub's status pipe ignores errors other
 * than a stall, 10a03de0, and its explore skips a port whose status can't
 * be read, 10a042c0 -> 10a044f0), and nothing was offered again until the
 * hub was pulled out. A hub that lost its power lost its address and
 * configuration too; the OS keeps talking to the one it had.
 * The OS's own way out is a device that leaves the root port and comes
 * back: what pulling the hub out did. So (hub_power_watch, hub_wedge_watch,
 * hub_recover_now):
 *  - the power falling under a hub is noted; once it is back, the hub is
 *    watched HUB_BLIP_GRACE_MS: a controller's reports or the sound's
 *    packets coming = alive (left as it is); an offer or a detach = the
 *    OS saw it itself (left to it); neither = dead;
 *  - a look asked of the OS (or its own news) not taken in HUB_WEDGE_MS
 *    while it had HUB_WEDGE_SLEEPS sleeps and did no work on a device =
 *    dead;
 *  - dead: between frames (hub_point) the port's power is restarted
 *    (nspire_arb_hub_cycle: the one restart allowed with a hub in), the
 *    OS sees the hub leave and come back and offers it and everything on
 *    it again; a toast says what happened (one line). At most
 *    HUB_RECOVER_MAX for one hub plug, HUB_RECOVER_GAP_MS apart; a hub not
 *    offered again within HUB_RECOVER_WAIT_MS: "still dead", replug it.
 * UNVERIFIED on hardware: that the restart brings the hub back after a real
 * brown-out (the arbiter's restarts brought devices back on both
 * calculators; a pull of the hub did in this very log). */
#define HUB_BLIP_GRACE_MS     3000
#define HUB_WEDGE_MS          12000
#define HUB_WEDGE_SLEEPS      10      /* speed18: at NSPIRE_AUDIO_HUB_MS = 1000 ms, 12 s has ~12 sleeps */
#define HUB_RECOVER_MAX       3
#define HUB_RECOVER_GAP_MS    30000
#define HUB_RECOVER_WAIT_MS   40000   /* (the OS may finish a ~25 s explore run against the dead hub first) */
#define HUB_PROBE_AGAIN_MS    60000   /* a hub that answered is not asked again before this */
#define HUB_RETRY_MS          5000    /* a restart refused now (the link) is tried again after this */
#define HUB_RECOVERING_TAIL_MS 5000   /* nspire_hub_recovering: this long after the hub's offer anew */
#define HUB_RECOVER_SETTLE_MS 6000
#define HUB_LOSTPOWER_TEXT    "Hub lost power, restarted it. A powered hub helps"
#define HUB_WEDGED_TEXT       "USB hub stopped answering: restarted it"
#define HUB_STILLDEAD_TEXT    "USB hub still dead: unplug it and plug it back in"
#define OTGCSR_DEV_B_BIT      (1u << 21)
#define OTGCSR_A_VBUS_BIT     (1u << 19)

int nspire_usbaudio_stalled(void) __attribute__((weak));
uint32_t nspire_usbaudio_completions(void) __attribute__((weak));
uint32_t nspire_bt_usb_traffic(void) __attribute__((weak));
int states_save_busy(void) __attribute__((weak));
int nspire_backup_busy(void) __attribute__((weak));
int nspire_usbaudio_busy(void) __attribute__((weak));
int nspire_usbaudio_active(void) __attribute__((weak));
int nspire_audio_serves_os(void) __attribute__((weak));
unsigned nspire_drive_os_callbacks(void) __attribute__((weak));
int nspire_usbms_active(void) __attribute__((weak));

static int hub_in, hub_active, hub_msg, hub_stop;
static int hub_news;      /* a port of the hub changed HUB_NEWS_MS ago at the most (the OS's flag) */
static u32 hub_news_at, hub_news_n, hub_news_since;
static int hub_news_set, hub_news_stuck;
/* hub_watch (HUB_WATCH_MS above) */
static int hub_forced;            /* the OS's explore flag is set by us and has not read clear since */
static u32 hub_force_at, hub_forced_n, hub_forced_since, hub_table_at;
static int hub_table_read, hub_forced_said;
static u32 hub_ports_w0, hub_ports_w1, hub_ports_meta;
static u8 hub_wait_mask, hub_gave_mask, hub_overcur_said;
static u32 hub_wait_at[8];
static int hub_coming;   /* this stretch of the OS's work brought an offer (a device coming, not only one going) */
static int hub_had;      /* something of ours was attached at the poll before this one */
static u32 hub_mark, hub_wires, hub_offers, hub_attaches, hub_detaches, hub_devices;
static u32 hub_active_at, hub_episode_ms, hub_bursts;
/* Hub 5: the power under the hub, the wedged explore, the recovery */
static int blip_on, blip_watch;
static u32 blip_at, blip_back_at, blip_offers, blip_detaches;
static int wedge_on;
static u32 wedge_since, wedge_sleeps0, wedge_work;
static int rec_due, rec_why, rec_n, rec_wait, rec_said, rec_refused, wedge_quiet, rec_probing, rec_probe_id;
static u32 rec_at, rec_hubs0, rec_dev0, rec_hub_at, rec_gen, rec_traffic, rec_work, rec_retry_at, wedge_quiet_at;
static u32 hub_gen, blip_traffic, wedge_traffic;

int nspire_usbhub_present(void)
{
  return started && hub_in;
}

/* The OS is at work on a device behind the hub (or was HUB_SETTLE_MS ago). */
int nspire_usbhub_busy(void)
{
  return started && hub_in && hub_active;
}

static u32 hub_ms(u32 since)
{
  return (u32)((u64)(nspire_ticks() - since) * 1000 / NSPIRE_TICK_HZ);
}

/* Something of ours other than a pad is attached (the sound adapter, a drive, the Bluetooth dongle). */
static int hub_others(void)
{
  return (nspire_usbaudio_active && nspire_usbaudio_active()) || (nspire_usbms_active && nspire_usbms_active()) ||
         nspire_bt_usb_active();
}

static void hub_recover_cancel(int why);

static void hub_begin(const struct usbpad_os_info *f, int offered)
{
  hub_in = 1;
  hub_wires = nspire_padlog_wires_gone();
  hub_offers = f->matches;
  hub_attaches = f->attaches;
  hub_detaches = f->detaches;
  hub_active_at = nspire_ticks();
  hub_active = 1;   /* what is plugged into it already comes next */
  hub_msg = hub_stop = 0;
  hub_coming = 1;
  hub_episode_ms = 0;
  /* (Hub 5) a hub of its own: a recovery due for the one before ends here
   * (one under way is the same hub coming back: hub_recover_poll) */
  hub_gen++;
  if (rec_due)
    hub_recover_cancel(2);
  blip_on = blip_watch = wedge_on = wedge_quiet = 0;
  if (offered)
    plog("a USB hub is on the port (%04x:%04x): no power restart while it is in; its devices come by the OS's offers",
         (unsigned)f->hub_vid, (unsigned)f->hub_pid);
  else
    plog("a USB hub is on the port (not offered: told by a second device coming while one is attached): no power "
         "restart while it is in");
  nspire_padlog_add(PL_HUB, PL_HUB_IN, 0, 0, offered ? (u32)f->hub_vid << 16 | f->hub_pid : 0, 0, 0);
  nspire_padlog_milestone(PL_HUB, PL_HUB_IN, 0, 0, offered ? (u32)f->hub_vid << 16 | f->hub_pid : 0, 0, 0);
  /* The hub's own come-up is over, and so is the 12 s wait for "a pad" on
   * the port's lines (its "no pad came in", the verdict's "didn't start"). */
  cu_end(PL_CU_HUB);
  attach_left = 0;
  waited_out = 0;
}

/* Each poll, after the driver's events were taken. */
#ifdef USBPAD_OS_HAS_HUBTREE
/* (2026-11-17) The hub's ports as the OS sees them, and the OS asked to
 * look again (HUB_WATCH_MS, above). Each poll while a hub is in. */
static void hub_watch(void)
{
  struct usbpad_os_hub h;
  u32 now = nspire_ticks();
  int r;

  if (!hub_table_read || hub_ms(hub_table_at) >= HUB_TABLE_MS)
  {
    hub_table_read = 1;
    hub_table_at = now;
    memset(&h, 0, sizeof(h));
    r = usbpad_os_hub_read(&h);
    if (r == 1)
    {
      unsigned n = h.nports < 8 ? h.nports : 8, i;
      u32 w0 = 0, w1 = 0, meta;
      int waiting = 0, gave = 0, overcur = 0;

      for (i = 0; i < n; i++)
      {
        unsigned b = h.port[i];

        if (i < 4)
          w0 |= (u32)b << (8 * i);
        else
          w1 |= (u32)b << (8 * (i - 4));
        if (b & USBPAD_OS_HUBP_OVERCUR)
          overcur = 1;
        /* (r19-usb) Only a port the hub said is CONNECTED, with no device
         * made of it yet, waits on the OS. The change bits ("!") are the
         * hub's last answer as TI's explore keeps it (usbshim_hub.h: hub + 48
         * + 20 p + 2, written by its get port status, never cleared by the
         * clear feature that follows), so a pulled controller's port reads
         * "p!" (powered, not connected, a change) until the next explore
         * reads it again, long after the OS has detached the device. A
         * CX II log (r19, 926fa73): the Xbox pad's detach at
         * 131187, its port "p!" from 131433; the 8BitDo's at 253055, "p!"
         * from 253260. Taken for a device on its way, it held the OS's tick
         * on and the looks asked of the OS off for HUB_PORT_GIVE_MS, then said
         * "A hub device didn't start: try another port" for a port with
         * nothing on it (139438, 261275), and only the look after that read
         * the port again ("p" at 141764, 263650): a controller plugged in
         * right after the pull was looked for ~10 s late. */
        if ((b & USBPAD_OS_HUBP_CONNECTED) && !(b & USBPAD_OS_HUBP_DEVICE))
        {
          if (!(hub_wait_mask & (1u << i)))
          {
            hub_wait_mask |= (u8)(1u << i);
            hub_wait_at[i] = now;
          }
          if (hub_ms(hub_wait_at[i]) < HUB_PORT_GIVE_MS)
            waiting = 1;
          else
          {
            gave = 1;
            if (!(hub_gave_mask & (1u << i)))
            {
              hub_gave_mask |= (u8)(1u << i);
              plog("hub: port %u: connected %u ms and the OS has made no device of it: it does not start", i + 1,
                   HUB_PORT_GIVE_MS);
              nspire_osd_message("%s", HUB_NOSTART_TEXT);
            }
          }
        }
        else
        {
          hub_wait_mask &= (u8)~(1u << i);
          hub_gave_mask &= (u8)~(1u << i);
        }
      }
      if (overcur && !hub_overcur_said)
      {
        hub_overcur_said = 1;
        plog("hub: the hub reports over-current on a port");
        nspire_osd_message("%s", HUB_OVERCUR_TEXT);
      }
      else if (!overcur)
        hub_overcur_said = 0;
      meta = n | (u32)(h.running != 0) << 8 | (u32)(h.suspended != 0) << 9 | (u32)(gave != 0) << 10 |
             (u32)(overcur != 0) << 11 | (u32)h.active << 16;
      if (w0 != hub_ports_w0 || w1 != hub_ports_w1 || meta != hub_ports_meta)
      {
        hub_ports_w0 = w0;
        hub_ports_w1 = w1;
        hub_ports_meta = meta;
        /* b: running | suspended << 1 | a device does not start << 2 | over-current << 3;
         * c: the ports' number | the active children << 4 */
        nspire_padlog_add(PL_HUB, PL_HUB_PORTS, (int)(meta >> 8 & 0xF), (int)(n | (h.active < 15 ? h.active : 15u) << 4),
                          w0, w1, hub_forced_n);
      }
      if (waiting && nspire_link_state() != LINK_LINKED)
      {
        if (!hub_news)
        {
          hub_news_n++;
          nspire_padlog_add(PL_HUB, PL_HUB_NEWS, 2, 0, hub_news_n, 0, 0);
        }
        hub_news = 1;
        hub_news_at = now;
      }
    }
  }
  /* the OS asked to look at the hub's ports again */
  if (!hub_active && !hub_news && !hub_forced && !bursting && !os_stuck && nspire_link_state() == LINK_OFF &&
      !link_cable_fast_up() && hub_ms(hub_force_at) >= HUB_WATCH_MS)
  {
    hub_force_at = now;
    if (usbpad_os_hub_explore() == 0)
    {
      hub_forced = 1;
      hub_forced_n++;
      hub_forced_since = now;
      hub_forced_said = 0;
    }
  }
}
#endif

/* Hub 5: everything the devices on the hub bring in, from any of them: a
 * controller's reports (on-change ones too, when they change), the sound's
 * completions, the Bluetooth dongle's events and packets. A drive brings
 * nothing while it is not read: the hub's own answer (hub_probe) covers it. */
static u32 hub_traffic(const struct usbpad_os_info *f)
{
  return f->reports + (nspire_usbaudio_completions ? nspire_usbaudio_completions() : 0) +
         (nspire_bt_usb_traffic ? nspire_bt_usb_traffic() : 0);
}

/* Hub 5: the hub's own answer to a request, never waited for (the game
 * goes on): hub_probe_start queues it (usbpad_os_hub_probe_start: 0 queued,
 * -1 can't be asked: another OS, a driver without it), hub_probe_poll reads
 * it at each point after (2 under way, 1 it answered, 0 not within its 1 s
 * limit, -1 none). */
static int hub_probe_start(void)
{
#if defined(USBPAD_OS_HAS_HUB_PROBE) && USBPAD_OS_HAS_HUB_PROBE >= 3
  int r = usbpad_os_hub_probe_start();

  plog("hub: the hub itself asked whether it answers: %s", r > 0 ? "queued" : "can't ask now");
  return r;
#else
  return -1;
#endif
}

/* (any poll also frees a request block that has ended, whoever asked) */
static int hub_probe_poll(int id)
{
#if defined(USBPAD_OS_HAS_HUB_PROBE) && USBPAD_OS_HAS_HUB_PROBE >= 3
  int r = usbpad_os_hub_probe_poll(id);

  if (r == 0 || r == 1)
    plog("hub: the hub's answer: %s (OS status %ld)", r ? "yes" : "NO", (long)usbpad_os_info()->hub_probe_status);
  return r;
#else
  return -1;
#endif
}

/* Hub 5: the hub may be dead (why: PL_HUBR_WHY_*): checked again and its
 * power restarted at the next point between game frames (hub_point), tied
 * to this hub (hub_gen), or said not to be made. */
static void hub_recover_ask(const struct usbpad_os_info *f, int why)
{
  if (rec_due || rec_wait)
    return;
  if (rec_n >= HUB_RECOVER_MAX)
  {
    if (!rec_said)
    {
      rec_said = 1;
      plog("hub: dead again after %d recoveries: none more for this hub (take it out and put it back)", rec_n);
      nspire_padlog_add(PL_HUB, PL_HUB_RECOVER, why, PL_HUBR_NOT_MADE, (u32)rec_n, 0, 0);
      nspire_padlog_milestone(PL_HUB, PL_HUB_RECOVER, why, PL_HUBR_NOT_MADE, (u32)rec_n, 0, 0);
      nspire_osd_message("%s", HUB_STILLDEAD_TEXT);
    }
    return;
  }
  rec_due = 1;
  rec_probing = 0;
  rec_why = why;
  rec_gen = hub_gen;
  rec_traffic = hub_traffic(f);
  rec_work = f->matches + f->attaches + f->detaches;
  rec_retry_at = nspire_ticks();
  rec_refused = 0;
  plog("hub: maybe dead (%s): checked again at the next point between game frames",
       why == PL_HUBR_WHY_POWER ? "its power fell and nothing on it answers since" : "the OS's explore does not run");
}

/* Hub 5: the port's power under the hub (CX II: OTGCSR's A_VBUS seen low;
 * OTGISR's A-device VBUS error, latched until the OS's handler takes it, is
 * logged with it but starts nothing alone), and once it is back, whether
 * anything on the hub still answers. Each poll while a hub is in. */
static void hub_power_watch(const struct usbpad_os_info *f)
{
  uint32_t w[NSPIRE_USB_RAW];
  int kind = nspire_usb_raw(w), off;
  u32 now = nspire_ticks();

  if (kind != 2 || (w[0] & OTGCSR_DEV_B_BIT))
    return;
  off = !(w[0] & OTGCSR_A_VBUS_BIT);
  if (off && !blip_on)
  {
    blip_on = 1;
    blip_watch = 0;
    blip_at = now;
    plog("hub: the port's power fell under the hub (OTGCSR %08lx OTGISR %08lx)", (unsigned long)w[0],
         (unsigned long)w[1]);
    nspire_padlog_add(PL_HUB, PL_HUB_BLIP, PL_HUBP_FELL, 0, w[0], w[1], 0);
    nspire_padlog_milestone(PL_HUB, PL_HUB_BLIP, PL_HUBP_FELL, 0, w[0], w[1], 0);
    return;
  }
  if (!off && blip_on)
  {
    blip_on = 0;
    blip_watch = 1;
    blip_back_at = now;
    blip_offers = f->matches;
    blip_detaches = f->detaches;
    blip_traffic = hub_traffic(f);
    /* (the time between the two looks that saw it off and on: not a measured length) */
    plog("hub: the port's power is back (seen off for %u ms between looks)", (unsigned)hub_ms(blip_at));
    nspire_padlog_add(PL_HUB, PL_HUB_BLIP, PL_HUBP_BACK, 0, w[0], w[1], hub_ms(blip_at));
    nspire_padlog_milestone(PL_HUB, PL_HUB_BLIP, PL_HUBP_BACK, 0, w[0], w[1], hub_ms(blip_at));
    return;
  }
  if (!blip_watch || blip_on)
    return;
  if (hub_traffic(f) != blip_traffic)
  {
    blip_watch = 0;
    plog("hub: after the power fell its devices still bring data in: the hub is left as it is");
    nspire_padlog_add(PL_HUB, PL_HUB_BLIP, PL_HUBP_ALIVE, 1, 0, 0, 0);
  }
  else if (f->matches != blip_offers || f->detaches != blip_detaches)
  {
    blip_watch = 0;
    plog("hub: after the power fell the OS offered or detached a device itself: left to it");
    nspire_padlog_add(PL_HUB, PL_HUB_BLIP, PL_HUBP_OS_TOOK, 0, f->matches - blip_offers, f->detaches - blip_detaches, 0);
  }
  else if (hub_ms(blip_back_at) >= HUB_BLIP_GRACE_MS)
  {
    blip_watch = 0;
    hub_recover_ask(f, PL_HUBR_WHY_POWER);
  }
}

#ifdef USBPAD_OS_HAS_EXPLORE
/* Hub 5: the OS's "a hub has news" flag (ours or its own) set without a
 * break HUB_WEDGE_MS, with HUB_WEDGE_SLEEPS of the OS's sleeps in it, no
 * work done on any device (an offer, an attach, a detach, a callback inside
 * the OS) and nothing brought in by any device on the hub. A device that
 * keeps failing to start keeps the flag set too: the hub's own answer
 * decides (hub_recover_point), and a hub that answered is not asked again
 * for HUB_PROBE_AGAIN_MS. */
static void hub_wedge_watch(const struct usbpad_os_info *f, int busy)
{
  u32 work = f->matches + f->attaches + f->detaches, traffic = hub_traffic(f);

  if (usbpad_os_explore_pending() <= 0 || busy || work != wedge_work || traffic != wedge_traffic || rec_due ||
      rec_wait || (wedge_quiet && hub_ms(wedge_quiet_at) < HUB_PROBE_AGAIN_MS))
  {
    wedge_on = 0;
    wedge_work = work;
    wedge_traffic = traffic;
    return;
  }
  wedge_quiet = 0;
  if (!wedge_on)
  {
    wedge_on = 1;
    wedge_since = nspire_ticks();
    wedge_sleeps0 = nspire_os_sleeps();
    return;
  }
  if (hub_ms(wedge_since) >= HUB_WEDGE_MS && nspire_os_sleeps() - wedge_sleeps0 >= HUB_WEDGE_SLEEPS)
  {
    wedge_on = 0;
    plog("hub: the OS's explore flag set %u ms through %u of its sleeps, no device work, nothing brought in: its "
         "explore does not get through", (unsigned)hub_ms(wedge_since), (unsigned)(nspire_os_sleeps() - wedge_sleeps0));
    hub_recover_ask(f, PL_HUBR_WHY_WEDGE);
  }
}
#endif

/* Hub 5: the recovery's state ends (the hub left by hand, a new hub, the
 * port empty or at the cable's device end): said when one was under way. */
static void hub_recover_cancel(int why)
{
  if (rec_due || rec_wait)
  {
    plog("hub: the recovery %s ends: %s", rec_wait ? "under way" : "due",
         why == 1 ? "the hub left the port" : why == 2 ? "another hub came" : "the port has no hub any more");
    nspire_padlog_add(PL_HUB, PL_HUB_RECOVER, rec_why, PL_HUBR_CANCELLED, (u32)why, (u32)rec_wait, 0);
  }
  rec_due = rec_wait = rec_probing = 0;
}

/* Hub 5: after a recovery's restart, the hub's offer anew and its devices'
 * (each poll, with a hub in or not). The hub may come back long after the
 * restart (the OS's explore finishing a run against the dead hub first):
 * "still dead" is said at HUB_RECOVER_WAIT_MS and a later return is still
 * taken as the recovery's. */
static void hub_recover_poll(const struct usbpad_os_info *f)
{
  if (rec_wait && usb_port() <= 0)
  {
    hub_recover_cancel(3);
    return;
  }
  if (rec_wait == 1 || rec_wait == 3)
  {
    if (f->hubs != rec_hubs0)
    {
      if (hub_in)
        hub_mark = f->hubs;   /* (the same hub, back: its mode went on through the restart) */
      plog("hub: recovery: the hub is offered again %u ms after the restart%s", (unsigned)hub_ms(rec_at),
           rec_wait == 3 ? " (late: after \"still dead\" was said)" : "");
      nspire_padlog_add(PL_HUB, PL_HUB_RECOVER, rec_why, PL_HUBR_BACK, hub_ms(rec_at), rec_wait == 3, 0);
      nspire_padlog_milestone(PL_HUB, PL_HUB_RECOVER, rec_why, PL_HUBR_BACK, hub_ms(rec_at), rec_wait == 3, 0);
      rec_wait = 2;
      rec_hub_at = nspire_ticks();
    }
    else if (rec_wait == 1 && hub_ms(rec_at) >= HUB_RECOVER_WAIT_MS)
    {
      rec_wait = 3;
      plog("hub: recovery: the hub was not offered again within %u ms (still watched for)", HUB_RECOVER_WAIT_MS);
      nspire_padlog_add(PL_HUB, PL_HUB_RECOVER, rec_why, PL_HUBR_STILL_DEAD, HUB_RECOVER_WAIT_MS, 0, 0);
      nspire_padlog_milestone(PL_HUB, PL_HUB_RECOVER, rec_why, PL_HUBR_STILL_DEAD, HUB_RECOVER_WAIT_MS, 0, 0);
      nspire_osd_message("%s", HUB_STILLDEAD_TEXT);
    }
  }
  else if (rec_wait == 2 && hub_ms(rec_hub_at) >= HUB_RECOVER_SETTLE_MS)
  {
    rec_wait = 0;
    plog("hub: recovery: %u device(s) on the hub offered again", (unsigned)(f->devices - rec_dev0));
    nspire_padlog_add(PL_HUB, PL_HUB_RECOVER, rec_why, PL_HUBR_OFFERED, f->devices - rec_dev0, hub_ms(rec_at), 0);
    nspire_padlog_milestone(PL_HUB, PL_HUB_RECOVER, rec_why, PL_HUBR_OFFERED, f->devices - rec_dev0, hub_ms(rec_at), 0);
  }
}

/* A restart is under way (from the cut until the hub's devices were
 * offered again, or the hub was given up): a drive on the hub is coming
 * back (nspire_drive_saves.c, gui.c say so for a save in that time). */
int nspire_usbhub_recovering(void)
{
  return started && rec_wait != 0 && rec_wait != 3;
}

/* (2026-11-18, for Bluetooth's toasts: bt10 holds back "dongle stopped
 * answering" and "dongle unplugged" while this says 1, so only the hub's
 * toast is seen) A hub recovery is in the air: from a fall of the power
 * under the hub (the watch after it), a dead hub's restart due (its probe
 * included) or under way, until 5 s after the hub and its devices are
 * offered again; 0 once the hub is found alive, the recovery gives up
 * ("still dead") or the hub is pulled; always 0 where none of it runs (no
 * hub, the CX: no power watch there). */
int nspire_hub_recovering(void)
{
  if (!started)
    return 0;
  if (rec_wait == 1 || (rec_wait == 2 && hub_ms(rec_hub_at) < HUB_RECOVERING_TAIL_MS))
    return 1;
  return hub_in && (blip_on || blip_watch || rec_due);
}

/* The hub's devices may be dead now (its power fell and it is being
 * watched, a recovery is due or under way, or it was given up as still
 * dead): a drive on it is not written or read (nspire_drive.c), its saves
 * go to the calculator instead of waiting on a drive that can't answer.
 * 1 it may come back, 2 given up. */
int nspire_usbhub_doubtful(void)
{
  if (!started || !hub_in)
    return 0;
  if (blip_on || blip_watch || rec_due || rec_wait)
    return 1;
  return rec_said && rec_n >= HUB_RECOVER_MAX ? 2 : 0;   /* 2: given up (said "still dead") */
}

/* Hub 5: right before the cut: the same hub, still silent, and not
 * answering a request itself; then the restart. In a game only (never from
 * the menus, where a save or a load of the drive may come next), never
 * while a state or the game's save is being written. 1 when something was
 * done (asked, cut or let go). */
static int hub_recover_point(int in_game)
{
  const struct usbpad_os_info *f = usbpad_os_info();
  int r, alive;

  if (!rec_due)
    return 0;
  if (!hub_in || rec_gen != hub_gen)
  {
    hub_recover_cancel(1);
    return 0;
  }
  if (!in_game || !started || bursting || nspire_link_state() != LINK_OFF || link_cable_fast_up() ||
      !nspire_os_ready() || !nspire_timer_runs() || (nspire_drive_os_callbacks && nspire_drive_os_callbacks()) ||
      (states_save_busy && states_save_busy()) || (nspire_backup_busy && nspire_backup_busy()))
    return 0;
  if (hub_ms(rec_retry_at) < (rec_refused ? HUB_RETRY_MS : 0u))
    return 0;
  if (rec_n && hub_ms(rec_at) < HUB_RECOVER_GAP_MS)
    return 0;
  /* still silent since it was found so? */
  alive = hub_traffic(f) != rec_traffic || f->matches + f->attaches + f->detaches != rec_work ? 1 : 0;
  if (!alive)
  {
    /* the hub asked, and its answer read at the points after: never waited
     * for (the request runs in the OS's interrupt, its limit 1 s) */
    int p;

    if (!rec_probing)
    {
      int id = hub_probe_start();

      if (id == -2)
        return 0;   /* another hub's request still under way: asked again at a later point */
      p = id > 0 ? 2 : -1;
      if (p == 2)
      {
        rec_probing = 1;
        rec_probe_id = id;   /* (its answer is this hub's only: a new hub's due drops it, hub_recover_cancel) */
      }
    }
    else
      p = hub_probe_poll(rec_probe_id);
    if (p == 2)
      return 0;
    rec_probing = 0;
    if (p == 1)
      alive = 2;
    else if (p < 0 && !(rec_why == PL_HUBR_WHY_POWER && nspire_usbaudio_stalled && nspire_usbaudio_stalled() == 1))
      alive = 3;   /* can't ask the hub, and nothing says it is dead: left as it is */
  }
  if (alive)
  {
    rec_due = rec_probing = 0;
    wedge_quiet = 1;
    wedge_quiet_at = nspire_ticks();
    plog("hub: not cut: %s", alive == 1 ? "its devices brought data in or the OS worked on one since" :
         alive == 2 ? "the hub answers a request" : "the hub can't be asked and nothing on it says it is dead");
    nspire_padlog_add(PL_HUB, PL_HUB_RECOVER, rec_why, PL_HUBR_ALIVE, (u32)alive, 0, 0);
    return 1;
  }
  rec_hubs0 = f->hubs;
  rec_dev0 = f->devices;
  r = nspire_arb_hub_cycle();
  if (r <= -2)
  {
    /* refused (the fast link, the cable, a calculator on it): not a recovery, tried again later */
    rec_retry_at = nspire_ticks();
    if (!rec_refused)
    {
      plog("hub: the restart was refused now (%d): tried again in %u ms", r, HUB_RETRY_MS);
      nspire_padlog_add(PL_HUB, PL_HUB_RECOVER, rec_why, PL_HUBR_NOT_MADE, (u32)rec_n, (u32)r, 0);
    }
    rec_refused = 1;
    return 1;
  }
  rec_due = 0;
  if (r == -1)
  {
    hub_recover_cancel(3);   /* nothing on the port to restart */
    return 1;
  }
  rec_n++;
  rec_at = nspire_ticks();
  blip_watch = blip_on = 0;
  wedge_on = 0;
  plog("hub: recovery #%d: the port's power restarted -> %d", rec_n, r);
  nspire_padlog_add(PL_HUB, PL_HUB_RECOVER, rec_why, PL_HUBR_RESTART, (u32)r, (u32)rec_n, 0);
  nspire_padlog_milestone(PL_HUB, PL_HUB_RECOVER, rec_why, PL_HUBR_RESTART, (u32)r, (u32)rec_n, 0);
  if (r == 1)
  {
    rec_wait = 1;
#ifdef USBPAD_OS_HAS_EXPLORE
    hub_news = hub_news_set = hub_news_stuck = hub_forced = 0;   /* the hub's flag starts over with it */
#endif
    nspire_osd_message("%s", rec_why == PL_HUBR_WHY_POWER ? HUB_LOSTPOWER_TEXT : HUB_WEDGED_TEXT);
  }
  else
    nspire_osd_message("%s", HUB_STILLDEAD_TEXT);   /* the power did not come back */
  if (in_game)
    nspire_reset_timing();
  return 1;
}

static void hub_poll(void)
{
  const struct usbpad_os_info *f = usbpad_os_info();
  int busy;

  if (!rec_probing)
    (void)hub_probe_poll(0);   /* (an ended request block freed at once, whatever became of its hub) */
  hub_recover_poll(f);

  if (hub_in && (usb_port() <= 0 || nspire_padlog_wires_gone() != hub_wires))
  {
    hub_in = hub_active = hub_msg = hub_news = 0;
    blip_on = blip_watch = wedge_on = 0;
    if (!rec_wait || usb_port() <= 0)
    {
      hub_recover_cancel(1);
      rec_n = rec_said = 0;   /* taken out by hand: the next hub is a new one (its recoveries anew) */
    }
    rec_due = 0;
    plog(rec_wait ? "the USB hub left the port (the recovery's restart)" : "the USB hub left the port");
    nspire_padlog_add(PL_HUB, PL_HUB_OUT, 0, 0, 0, 0, 0);
    nspire_padlog_milestone(PL_HUB, PL_HUB_OUT, 0, 0, 0, 0, 0);
    nspire_osd_withdraw(HUB_TEXT);
    device_left();
  }
  /* (2026-11-17) The port under the hub is no longer the OS's working port,
   * HUB_DEAD_MS of it: connected and not enabled with no reset under way
   * (the hub dropped and came back, or was swapped, between two looks, onto
   * a transceiver a suspend left asleep), or held suspended by the OS with
   * its controller stopped (an OS whose sleep is not kept away). The hub as
   * the OS had it is gone: its mode ends as when it leaves, and the port's
   * own recovery takes over (watch_phy, the arbiter's restart, which a hub
   * at work forbids); a hub that comes back is offered anew. A CX II's
   * port only (the registers these words are). */
  if (hub_in)
  {
    static int dead_on;
    static u32 dead_at;
    uint32_t w[NSPIRE_USB_RAW];

    if (nspire_usb_raw(w) == 2 && (w[4] & 1) && !(w[4] & 0x100) &&
        (!(w[4] & 4) || ((w[4] & 0x80) && !(w[2] & 1))) && !usbpad_count() && !hub_others())
    {
      if (!dead_on)
      {
        dead_on = 1;
        dead_at = nspire_ticks();
      }
      else if (hub_ms(dead_at) >= HUB_DEAD_MS)
      {
        dead_on = 0;
        hub_in = hub_active = hub_msg = hub_news = 0;
        blip_on = blip_watch = wedge_on = 0;
        hub_recover_cancel(3);
        hub_mark = f->hubs;
        plog("the port under the hub is no longer the OS's working port (USBCMD %08lx PORTSC %08lx): the hub's mode "
             "ends", (unsigned long)w[2], (unsigned long)w[4]);
        nspire_padlog_add(PL_HUB, PL_HUB_DEAD, 0, 0, w[2], w[4], 0);
        nspire_padlog_milestone(PL_HUB, PL_HUB_DEAD, 0, 0, w[2], w[4], 0);
        nspire_osd_withdraw(HUB_TEXT);
        device_left();
      }
    }
    else
      dead_on = 0;
  }
  if (!hub_in)
  {
    /* A device offered at device level while one of ours was attached
     * already at the poll before (a device's own offer and its attach come
     * within one poll: that is one device, not two). */
    int second = f->devices != hub_devices && hub_had;

    hub_devices = f->devices;
    hub_had = usbpad_count() || hub_others();
    if (f->hubs == hub_mark && !second)
      return;
    if (usb_port() <= 0)
    {
      hub_mark = f->hubs;
      return;
    }
    hub_begin(f, f->hubs != hub_mark);
    hub_mark = f->hubs;
  }
  hub_devices = f->devices;
  hub_had = usbpad_count() || hub_others();
  hub_power_watch(f);   /* Hub 5 */
#ifdef USBPAD_OS_HAS_HUBTREE
  hub_watch();
#endif
#ifdef USBPAD_OS_HAS_EXPLORE
  /* (2026-11-17) a port of the hub changed (HUB_NEWS_MS, above); a flag our
   * own ask set (hub_watch) is not news */
  if (usbpad_os_explore_pending() <= 0)
    hub_forced = 0;
  if (usbpad_os_explore_pending() > 0 && !hub_forced)
  {
    if (!hub_news_set)
    {
      hub_news_set = 1;
      hub_news_since = nspire_ticks();
    }
    if (hub_news_stuck)
      ;
    else if (hub_ms(hub_news_since) >= HUB_NEWS_STUCK_MS)
    {
      hub_news_stuck = 1;
      hub_news = 0;
      plog("hub: the OS's \"a hub has news\" flag stayed set %u ms through the OS's ticks: no more ticks for it until "
           "it clears", HUB_NEWS_STUCK_MS);
      nspire_padlog_add(PL_HUB, PL_HUB_NEWS, 1, 0, hub_news_n, 0, 0);
    }
    else
    {
      if (!hub_news)
      {
        hub_news_n++;
        nspire_padlog_add(PL_HUB, PL_HUB_NEWS, 0, 0, hub_news_n, 0, 0);
      }
      hub_news = 1;
      hub_news_at = nspire_ticks();
    }
  }
  else
  {
    if (!hub_forced)
      hub_news_set = hub_news_stuck = 0;
    if (hub_news && hub_ms(hub_news_at) >= HUB_NEWS_MS)
      hub_news = 0;
    /* our own ask not taken in HUB_NEWS_STUCK_MS: the OS's explore task
     * does not run (said once a hub) */
    if (hub_forced && !hub_forced_said && hub_ms(hub_forced_since) >= HUB_NEWS_STUCK_MS)
    {
      hub_forced_said = 1;
      plog("hub: the look asked of the OS %u ms ago was not taken: its explore task does not run", HUB_NEWS_STUCK_MS);
      nspire_padlog_add(PL_HUB, PL_HUB_NEWS, 1, 0, hub_news_n, 0, 0);
      nspire_padlog_milestone(PL_HUB, PL_HUB_NEWS, 1, 0, hub_news_n, 0, 0);
    }
  }
#endif
  /* A pad left a port of the hub: by its event (a working one), or by the
   * driver's detach alone (a quiet one posts none), in a poll that brought
   * nothing of another device. */
  if ((gone_now || (f->detaches != hub_detaches && !events_now)) && !usbpad_count())
  {
    nspire_padlog_add(PL_HUB, PL_HUB_LEFT, 0, 0, f->detaches, 0, 0);
    device_left();
  }
  gone_now = 0;
  busy = f->busy || (nspire_usbaudio_busy && nspire_usbaudio_busy()) ||
         (nspire_drive_os_callbacks && nspire_drive_os_callbacks());
#ifdef USBPAD_OS_HAS_EXPLORE
  hub_wedge_watch(f, busy);   /* Hub 5 */
#endif
  if (busy || f->matches != hub_offers || f->attaches != hub_attaches || f->detaches != hub_detaches)
  {
    if (!hub_active)
    {
      hub_msg = hub_stop = hub_coming = 0;   /* a new stretch of the OS's work */
      hub_episode_ms = 0;
    }
    if (f->matches != hub_offers)
      hub_coming = 1;
    hub_offers = f->matches;
    hub_attaches = f->attaches;
    hub_detaches = f->detaches;
    hub_active_at = nspire_ticks();
    hub_active = 1;
  }
  else if (hub_active && hub_ms(hub_active_at) >= HUB_SETTLE_MS)
  {
    hub_active = 0;
    nspire_osd_withdraw(HUB_TEXT);
  }
}

static int poll_driver(int *attach);

/* The OS alone while it works on a device behind the hub. */
static void hub_burst(int in_game)
{
  u32 t0 = nspire_ticks(), s0 = nspire_os_sleeps(), offers0 = offer_count(), ms = 0;
  int held = keys_raw_down(), why = PL_HUBB_TIME, attach;

  bursting = 1;
  nspire_timer_lend();
  for (;;)
  {
    u32 now;

    if (!nspire_os_sleep())
    {
      why = PL_HUBB_NO_SLEEP;
      break;
    }
    if (in_game)
      nspire_osd_extend();
    nspire_usbport_poll();
    nspire_arb_tick();
    now = nspire_ticks();
    if (poll_due(now))
    {
      polled = 1;
      last_poll = now;
      poll_driver(&attach);
      note_input();
    }
    ms = hub_ms(t0);
    if (!hub_in)
    {
      why = PL_HUBB_GONE;
      break;
    }
    if (!hub_active)
    {
      why = PL_HUBB_DONE;
      break;
    }
    if (held)
      held = keys_raw_down();
    else if (keys_raw_down())
    {
      why = PL_HUBB_KEY;
      break;
    }
    if (ms >= HUB_BURST_MS)
      break;
  }
  nspire_timer_retake();
  hub_episode_ms += ms ? ms : 1;
  hub_bursts++;
  if (why == PL_HUBB_KEY || why == PL_HUBB_NO_SLEEP)
    hub_stop = 1;   /* this stretch goes on a tick at a time */
  nspire_padlog_add(PL_HUB, PL_HUB_BURST, in_game, why, ms, nspire_os_sleeps() - s0, offer_count() - offers0);
  bursting = 0;
  if (in_game)
    nspire_reset_timing();   /* the frames' pacing doesn't catch up the stillness */
}

/* Between frames and in the menus (as cu_point). */
static void hub_point(int in_game)
{
  if (hub_recover_point(in_game))
    return;
  if (!started || bursting || !hub_in || !hub_active || hub_stop || hub_episode_ms >= HUB_EPISODE_MS)
    return;
  if (nspire_link_state() != LINK_OFF || link_cable_fast_up() || !nspire_os_ready() || !nspire_timer_runs())
    return;
  if (in_game && hub_coming && !hub_msg)
    return;   /* its message goes into a frame first (nspire_usbpad_frame); a device only going has none */
  hub_burst(in_game);
}
#else
int nspire_usbhub_present(void) { return 0; }
int nspire_usbhub_busy(void) { return 0; }
int nspire_usbhub_recovering(void) { return 0; }
int nspire_usbhub_doubtful(void) { return 0; }
int nspire_hub_recovering(void) { return 0; }
#define hub_in 0
#define hub_active 0
#define hub_news 0
static void hub_poll(void) {}
static void hub_point(int in_game) { (void)in_game; }
#endif

/* One poll of the driver and of what the port, the OS and the driver did
 * (the keys' and the burst's). Returns whether the OS is wanted; *attach:
 * for the OS's hub work (the tick rule). */
static int poll_driver(int *attach)
{
  int want = (usbpad_poll() & USBPAD_WANT_OS) != 0;

  if (first_polls < 3)
    first_polls++;
  events_now = 0;
  gone_poll = 0;
  take_events();
#if USBPAD_API_LEVEL >= 6
  drain_trace();
#endif
  watch_stages();
  nspire_padlog_poll();
  /* PAD3 (F1's other half): a device off the wires of an adapter that
   * stays in, where the port's bits may not change (the OS's host
   * controller stopped: R9F's heartbeat, PORTSC 00000800 then 00000000).
   * (Not at a device's arrival: the OS's offer can come before the next
   * look at the wires, and would be forgotten.) */
  if (nspire_padlog_wires_gone() != wires_seen)
  {
    wires_seen = nspire_padlog_wires_gone();
    device_left();
  }
#ifdef USBPAD_API_LEVEL
  /* On a USB hub, unplugging a P_QUIET controller (EV_UNSUPPORTED, where
   * detach posts no DISCONNECTED event and root PORTSC stays up) advances
   * detaches in a later poll without new events: clear the stale verdict. */
  {
    const struct usbpad_os_info *fi = usbpad_os_info();
    if (fi->detaches != seen_detaches)
    {
      seen_detaches = fi->detaches;
      if (!hub_in && !events_now && !usbpad_count() && last_ev_set && last_ev.type != USBPAD_EV_CONNECTED &&
          last_ev.type != USBPAD_EV_FAILED)
        device_left();
    }
  }
#endif
#ifdef USBPAD_OS_HAS_HUB
  /* 2026-11-16: stale "being aborted" marks the driver took off the OS's
   * free transfer blocks (usbshim_xfer.h): said once each time there were
   * some (they came from a sound adapter's abort earlier in this OS's
   * uptime; each could have hung the OS's USB at a later unplug) */
  {
    static u32 cleaned_seen;
    const struct usbpad_os_info *fi = usbpad_os_info();

    if (fi->pool_cleaned != cleaned_seen)
    {
      plog("the driver took TI's stale abort mark off %u free transfer block(s) of the OS (%u so far)",
           (unsigned)(fi->pool_cleaned - cleaned_seen), (unsigned)fi->pool_cleaned);
      nspire_padlog_add(PL_HUB, PL_HUB_POOL, 0, 0, fi->pool_cleaned - cleaned_seen, fi->pool_cleaned, 0);
      nspire_padlog_milestone(PL_HUB, PL_HUB_POOL, 0, 0, fi->pool_cleaned - cleaned_seen, fi->pool_cleaned, 0);
      cleaned_seen = fi->pool_cleaned;
    }
  }
#endif
  hub_poll();               /* hub: before the port's own waits (it ends them) */
  watch_not_controller();   /* R11u: before the attach wait counts down */
  *attach = attaching();
  status_poll();
  cu_poll();
  *attach = *attach || cu_wants();
  /* hub: the OS at work on a device behind it: its tick on every poll (the
   * time in one go is hub_point's, between frames) */
  /* (2026-11-17) and for HUB_NEWS_MS after a port of the hub changed */
  if (hub_in && (hub_active || hub_news) && nspire_link_state() != LINK_LINKED)
    *attach = 1;
  return *attach || want;
}

/* The OS alone, one tick after another, up to CU_BURST_MS (see the come-up
 * above); the driver polled at its rate meanwhile, the restart made when it
 * is due. A key pressed (not one held when it began) ends it. */
static void burst(int in_game)
{
  u32 t0 = nspire_ticks(), s0 = nspire_os_sleeps(), ms = 0, limit = CU_BURST_MS;
  /* R11t: from: the ms the normal budget counts from (0, or where the wait
   * for the host controller ended) */
  u32 from = 0;
  int held = keys_raw_down(), why = PL_BURST_TIME, attach, waited = 0, notice = 0;

  cu.burst = 1;
  bursting = 1;
  /* R12: a CX II on the legacy time base gives the OS its timer block back
   * as it had it for the burst (nothing elsewhere) */
  nspire_timer_lend();
  for (;;)
  {
    u32 now;

    if (!nspire_os_sleep())
    {
      why = PL_BURST_NO_SLEEP;
      break;
    }
    if (in_game)
      nspire_osd_extend();
    nspire_usbport_poll();
    nspire_arb_tick();
    now = nspire_ticks();
    if (poll_due(now))
    {
      polled = 1;
      last_poll = now;
      poll_driver(&attach);
      note_input();
    }
    cu_restart_point();
    ms = (u32)((u64)(nspire_ticks() - t0) * 1000 / NSPIRE_TICK_HZ);
    if (!cu.on)
    {
      why = cu.outcome == PL_CU_WORKS ? PL_BURST_WORKS : PL_BURST_ENDED;
      break;
    }
    /* (2026-11-17) the device waits on a transceiver that was asleep, or the
     * OS holds it suspended: more of the OS's time changes nothing, the
     * power restart at the next point does (watch_phy) */
    if (phy_cycle_due)
    {
      why = PL_BURST_ENDED;
      break;
    }
    if (cu.step[CU_ATTACH] && cu_ms() + 1 >= cu.step[CU_ATTACH] + CU_REPORT_MS)
    {
      why = PL_BURST_WORKS;
      break;
    }
    if (held)
      held = keys_raw_down();
    else if (keys_raw_down())
    {
      why = PL_BURST_KEY;
      break;
    }
    if (nspire_link_state() != LINK_OFF || nspire_link_cable_present() || link_cable_fast_up() || usb_port() <= 0 ||
        nspire_arb_owner() == ARB_LINK || nspire_arb_owner() == ARB_OTHER)
    {
      why = PL_BURST_ENDED;
      break;
    }
    /* the OS is done with an enabled port it offered nothing for (a device
     * it enumerated before the driver registered: the arbiter's S4 cycle) */
    if ((cu.sig & 0xB) == 0xB && ms >= CU_IDLE_MIN_MS && !cu.step[CU_OFFER] &&
        (u32)((u64)(nspire_ticks() - cu.moved_at) * 1000 / NSPIRE_TICK_HZ) >= CU_IDLE_MS)
    {
      why = PL_BURST_IDLE;
      break;
    }
    /* R11t: a device on the wires and the host controller not running: the
     * OS keeps the CPU until it runs (CU_HC_WAIT_MS at most) */
    if (cu_hc_waiting())
    {
      waited = 1;
      from = 0;
      if (!in_game && !notice && ms >= CU_NOTICE_MS)
        notice = menu_notice(1);
      if (ms >= CU_HC_WAIT_MS || nspire_os_sleeps() - s0 >= CU_HC_WAIT_SLEEPS)
      {
        why = PL_BURST_HC_WAIT;
        break;
      }
      continue;
    }
    if (waited && !from)
      from = ms ? ms : 1;
    if (ms - from >= limit)
    {
      /* longer while the port moves, or the OS works on a connected device
       * (its controller running, the port not enabled yet) */
      int working = (cu.sig & 0x3) == 0x3 && !(cu.sig & 0x8);
      if (limit < CU_BURST_MAX_MS &&
          (working || (u32)((u64)(nspire_ticks() - cu.moved_at) * 1000 / NSPIRE_TICK_HZ) < CU_MOVING_MS))
        limit = ms - from + 100 < CU_BURST_MAX_MS ? ms - from + 100 : CU_BURST_MAX_MS;
      else
        break;
    }
  }
  if (notice)
    menu_notice(0);
  nspire_timer_retake();
  cu.burst_ms = ms;
  cu.burst_sleeps = nspire_os_sleeps() - s0;
  nspire_padlog_add(PL_BURST, why, in_game, waited, ms, cu.burst_sleeps, cu.sig != (u32)-1 && (cu.sig & 1));
  bursting = 0;
  if (in_game)
    nspire_reset_timing();   /* the frames' pacing doesn't catch up the stillness */
}

/* Between frames (nspire_arb_frame) and in the menus (nspire_usbpad_menu_flush):
 * the burst once a come-up is under way and its message was in a frame (in
 * the game), the restart when it's due. */
static void cu_point(int in_game)
{
  if (!started || bursting)
    return;
  /* (2026-11-17) the restart watch_phy asked for (the arbiter's guards
   * decide; it blocks ~0.8 s in OS sleeps: here, between frames or in the
   * menus, as the come-up's own) */
  if (phy_cycle_due)
  {
    phy_cycle_due = 0;
    nspire_arb_comeup_cycle();
    return;
  }
  if (!cu.on)
    return;
  if (!cu.burst && !cu.step[CU_OFFER] && cu_may() && (!in_game || cu.msg))
    burst(in_game);
  else
    cu_restart_point();
}

void nspire_usbpad_frame_point(void)
{
  cu_point(1);
  hub_point(1);
}

/* PAD3/PAD4: the Controller page's and the pad log's view of the last come-up. */
static void cu_view(struct pad_driver_view *d)
{
  unsigned i;

  d->cu_on = (uint8_t)cu_any;
  d->cu_live = (uint8_t)cu.on;
  d->cu_age_ms = cu_any ? cu_ms() : 0;
  for (i = 0; i < PADSTATUS_STEPS; i++)
    d->cu_step[i] = cu.step[i];
}

void nspire_usbpad_message_posted(void)
{
  holding = 1;
  hold_until = nspire_ticks() + MESSAGE_TIME;
}

/* (2026-11-17) The pad's interrupt window and what it brought
 * (PAD_TASK_MS, above). Returns 1 while the windows serve the pad (the
 * caller makes no tick of its own then), 0 once they were found not to. */
static int padwin_off;            /* the windows bring no reports on this OS: the tick as before */
static u32 padwin_in_windows, padwin_in_sleeps, padwin_windows, padwin_pads;
static int padwin_said;

static void pad_irq_window(void)
{
  u32 old;
  volatile unsigned spin;

  nspire_os_keypad_keep_off();
  old = nspire_irqs_unmask();
  for (spin = 0; spin < 4; spin++)
    ;   /* (a pending interrupt is taken at the first instruction with them on) */
  nspire_irqs_restore(old);
  nspire_os_after_os_time();
}

static int pad_windows(u32 since)
{
  u32 r0, r1;

  /* (not while the link has the port's time: its frames are paced by it) */
  if (padwin_off || !nspire_os_ready() || nspire_link_state() != LINK_OFF || link_cable_fast_up())
    return 0;
  if (padwin_pads != usbpad_os_info()->attaches)
  {
    /* another controller: judged anew (what was learnt of the OS stays) */
    padwin_pads = usbpad_os_info()->attaches;
    padwin_in_sleeps = 0;
    padwin_said = 0;
  }
  r0 = usbpad_os_info()->reports;
  pad_irq_window();
  padwin_windows++;
  r1 = usbpad_os_info()->reports;
  if (r1 != r0)
  {
    padwin_in_windows += r1 - r0;
    if (!padwin_said)
    {
      padwin_said = 1;
      plog("pad: its reports come in interrupt windows: no OS tick of its own (a task sleep every %u ms)", PAD_TASK_MS);
      nspire_padlog_add(PL_HC, 8, 0, 0, padwin_windows, padwin_in_windows, 0);
      nspire_padlog_milestone(PL_HC, 8, 0, 0, padwin_windows, padwin_in_windows, 0);
    }
  }
  /* the OS's task time */
  if (since >= (u32)PAD_TASK_MS * (NSPIRE_TICK_HZ / 1000))
  {
    if (nspire_os_sleep())
      forced_ticks++;
    if (usbpad_os_info()->reports != r1 && !padwin_in_windows && ++padwin_in_sleeps >= PADWIN_GIVE)
    {
      padwin_off = 1;
      plog("pad: %lu interrupt windows brought no report and %u task sleeps did: the tick every 33 ms as before",
           (unsigned long)padwin_windows, PADWIN_GIVE);
      nspire_padlog_add(PL_HC, 9, 0, 0, padwin_windows, PADWIN_GIVE, 0);
      nspire_padlog_milestone(PL_HC, 9, 0, 0, padwin_windows, PADWIN_GIVE, 0);
      return 0;
    }
  }
  return 1;
}

uint32_t nspire_usbpad_keys(void)
{
  u32 now;
  int want, attach;

  if (!started)
    return 0;
  nspire_usbport_poll();
  nspire_arb_tick();
  keys_calls++;
  now = nspire_ticks();
  if (bursting || !poll_due(now))
    return keys;
  polled = 1;
  last_poll = now;
  want = poll_driver(&attach);
  /* LINK2: while the link's fast transport holds the USB controller (its
   * interrupt line masked) the OS's USB task can attach nothing: the
   * driver's idle "look now and then" (every 30 polls with the cable at the
   * host's end) asks for no OS time then. */
  if (link_cable_fast_up())
    want = 0;
  /* 2026-11-16: the OS's USB has stopped (watch_stuck): it offers nothing
   * whatever time it gets, so the game keeps its own. The adapter out ends
   * that (watch_stuck): what is plugged in after it gets the OS's time as
   * any plug does, and is judged anew. */
  if (os_stuck && !usbpad_count())
    want = attach = 0;
  /* hub: the frames' spare time is the OS's while a hub is in (its ports'
   * changes reach the OS's USB task only in its time) */
  nspire_os_want(OS_WANT_PAD, want || (hub_in && !link_cable_fast_up()));
  polls_taken++;
  if (!want)
    nspire_frameskip_os_none();
  /* P2, R1: while attaching, a tick every SLEEP_GAP as before; a pad
   * that's in (or one wanting the OS otherwise) gets the tick when it's
   * near from 1/60 s on, forced at SLEEP_GAP as before P2: its reports
   * stay at most 33 ms apart in a game with no spare time
   * (nspire_usbpad_tick_rule, nspire_usbpad_glue.h). */
  if (want)
  {
    u32 since = nspire_os_since_sleep();
    int rule = nspire_usbpad_tick_rule(since, attach);

    /* (2026-11-17, final/usb8) A controller that is in while a USB sound
     * adapter streams (both on a hub): its reports come where the
     * controller's interrupt is taken, and the sound code takes it some 25
     * times a second already (its interrupt windows, or its own sleeps
     * when the windows are not trusted): the EHCI handler completes the
     * controller's transfer in the same pass as the sound's. The forced
     * tick every 33 ms on top of that cost a CX II 22 OS sleeps
     * and ~110 ms of each second (r15's hardware log:
     * 57 pictures of 60 before the controller, 41-51 with it). So: no tick
     * of the pad's own then; one only when no report came for
     * PAD_QUIET_MS (a controller that reports only what changes, or one
     * whose transfer needs the OS's task: at most two a second). The
     * attach and the hub's own times (attach) are as they were. */
    if (!attach && usbpad_count() && !(nspire_audio_serves_os && nspire_audio_serves_os()) && pad_windows(since))
      rule = USBPAD_TICK_NONE;
    else if (rule != USBPAD_TICK_NONE && !attach && usbpad_count() && nspire_audio_serves_os && nspire_audio_serves_os())
    {
      static u32 reports_seen, reports_at;
      static int reports_set;
      u32 n = usbpad_os_info()->reports, now2 = nspire_ticks();

      if (!reports_set || n != reports_seen)
      {
        reports_set = 1;
        reports_seen = n;
        reports_at = now2;
      }
      if (now2 - reports_at < PAD_QUIET_MS * (NSPIRE_TICK_HZ / 1000))
        rule = USBPAD_TICK_NONE;
      else
        reports_at = now2;   /* (this tick; the next one PAD_QUIET_MS on) */
    }
    /* (the OS gap: nspire_os.c measures it at each sleep, speed9a: here,
     * at the key read, it missed the gaps the frame loop's sleeps end) */

    /* (r16, merging final/speed8-9a with final/usb8) the pad's own ticks are
     * usb8's, taken here: its reports come in interrupt windows (no tick),
     * ride the sound's sleeps (the quiet tick), or, where the windows bring
     * nothing, get the tick every 33 ms as before (hub_tests
     * test_pad_windows_not_here: 25 a second at the least). speed8's frame
     * loop watch for the pad (nspire_frameskip_os_keys) gave that case one
     * every ~60 ms in that test; with usb8's windows it has nothing left to
     * win, so it is not armed for the pad. The sound task's sleeps still go
     * through it (speed9a, nspire_frameskip_os_task). */
    nspire_frameskip_os_none();
    if (rule == USBPAD_TICK_SLEEP ? nspire_os_sleep() :
        rule == USBPAD_TICK_NEAR &&
        nspire_os_sleep_near(nspire_os_near_wait(since, USBPAD_TICK_FROM, USBPAD_TICK_FORCE)))
      forced_ticks++;
  }
#ifdef USBPAD_OS_HAS_HUB
  /* hub: and with nothing else asking, the OS's tick at least every
   * HUB_TICK_FORCE (near tries from HUB_TICK_FROM): a device plugged into
   * the hub is the OS's hub driver's to find, and it finds it in ~20 of its
   * own runs (the reset, the address, the descriptors). 2026-11-17: not
   * while a sound adapter streams: its sleeps give the OS that time every
   * NSPIRE_AUDIO_HUB_MS at the most, where they cost least (nspire_audio.c
   * task_ms; these at the key read cost a CX II 80-100 ms of each
   * second, 60/50-53 pictures). */
  else if (hub_in && !link_cable_fast_up() && nspire_link_state() != LINK_LINKED && !os_stuck &&
           !(nspire_audio_serves_os && nspire_audio_serves_os()))
  {
    u32 since = nspire_os_since_sleep();
    /* (HUB_QUIET_MS after the hub's last coming or going the tick is rarer:
     * what is plugged in then takes a few seconds longer to be found, and
     * the game pays less for a hub that just sits there) */
    u32 force = hub_ms(hub_active_at) < HUB_QUIET_MS ? HUB_TICK_FORCE : HUB_TICK_QUIET;
    u32 from = force == HUB_TICK_FORCE ? HUB_TICK_FROM : HUB_TICK_FORCE;

    /* (2026-11-17) and at once while the controller holds something for the
     * OS (a CX II's USBSTS bits 0-2: a transfer done or failed, the port
     * changed; not bit 3, the frame list's rollover, which the OS never
     * takes): the hub's status pipe completes when a device is plugged
     * into or pulled out of the hub, and each step of the OS's work on it
     * ends in one more completion. So a device plugged into a hub that sat
     * idle is found a step a frame, not a step a tick of the rare kind; the
     * OS's handler takes the bits in the tick. */
    {
      uint32_t w[NSPIRE_USB_RAW];

      if (nspire_usb_raw(w) == 2 && (w[3] & 7))
        force = from = 0;
    }
    if (since >= force ? nspire_os_sleep() :
        since >= from && nspire_os_sleep_near(nspire_os_near_wait(since, from, force)))
      forced_ticks++;
  }
#endif
  keys = pad_keys(0) | pad_keys(1);
  note_input();
  if (settings_missing && usbpad_count())
  {
    char path[600];
    const struct usbpad_state *pad = usbpad_state(usbpad_state(0)->connected ? 0 : 1);

    settings_missing = 0;
    nspire_save_file(path, sizeof(path), SETTINGS_FILE);
    write_settings(path, pad);
  }
  return keys;
}

/* A USB controller's, else a Bluetooth one's (the same kinds: usbpad.h's). */
const char *nspire_usbpad_name(void)
{
  int p;
  if (started)
    for (p = 0; p < USBPAD_PLAYERS; p++)
      if (usbpad_state(p)->connected)
        return usbpad_kind_name(usbpad_state(p)->kind);
  return nspire_bt_glue_pad_name();
}

int nspire_usbpad_family(void)
{
  int p;
  if (started)
    for (p = 0; p < USBPAD_PLAYERS; p++)
      if (usbpad_state(p)->connected)
        return bt_family(usbpad_state(p)->kind);
  return bt_family(nspire_bt_glue_pad_kind());
}

int nspire_usbpad_labels(void)
{
  int p;
  struct usbpad_state bt;
  if (started)
    for (p = 0; p < USBPAD_PLAYERS; p++)
      if (usbpad_state(p)->connected)
        return labels_of(usbpad_state(p));
  /* a Bluetooth pad: its kind's USB ids (nspire_bt.h), the same layout lines */
  memset(&bt, 0, sizeof(bt));
  switch (nspire_bt_glue_pad_kind())
  {
  case USBPAD_KIND_DS4: bt.vid = 0x054c; bt.pid = 0x05c4; break;
  case USBPAD_KIND_DUALSENSE: bt.vid = 0x054c; bt.pid = 0x0ce6; break;
  case USBPAD_KIND_SWITCHPRO: bt.vid = 0x057e; bt.pid = 0x2009; break;
  default: return labels_all;
  }
  return labels_of(&bt);
}

/* The USB controllers in (the Bluetooth ones are numbered after them). */
int nspire_usbpad_pads(void)
{
  return started ? usbpad_count() : 0;
}

/* The slots the USB controllers are in (bit n slot n: they keep theirs, the
 * Bluetooth ones are numbered after the highest), and a slot's controller's
 * name (NULL while empty). */
unsigned nspire_usbpad_slots(void)
{
  unsigned m = 0;
  int p;
  if (started)
    for (p = 0; p < USBPAD_PLAYERS; p++)
      if (usbpad_state(p)->connected)
        m |= 1u << p;
  return m;
}

const char *nspire_usbpad_slot_name(int slot)
{
  if (!started || slot < 0 || slot >= USBPAD_PLAYERS || !usbpad_state(slot)->connected)
    return NULL;
  return usbpad_kind_name(usbpad_state(slot)->kind);
}

/* The pad settings file's layout for a controller by its USB ids (a
 * Bluetooth pad's are its kind's): 1 by its labels. */
int nspire_usbpad_layout_labels(uint16_t vid, uint16_t pid)
{
  struct usbpad_state s;
  memset(&s, 0, sizeof(s));
  s.vid = vid;
  s.pid = pid;
  return labels_of(&s);
}

/* ---- Messages ---- */

/* The driver's text, but a player's number only while two pads are in
 * (both drive the GBA's one player). */
static void toast_text(const struct usbpad_event *ev, char *text, unsigned size)
{
  int two = ev->player >= 0 && ev->player < USBPAD_PLAYERS && usbpad_state(1 - ev->player)->connected;

  /* R11w part 2: plain words that say what to do, at most 50 characters
   * (the driver's own texts carry USB ids: those are the page's and the
   * log's) */
  if (ev->type == USBPAD_EV_CONNECTED && !two)
    snprintf(text, size, "Controller connected (%s)", ev->name);
  else if (ev->type == USBPAD_EV_DISCONNECTED && !two)
    snprintf(text, size, "Controller disconnected");
#if USBPAD_API_LEVEL >= 3
  else if (ev->type == USBPAD_EV_FAILED)
    snprintf(text, size, "Controller stopped. Plug it in again.");
#endif
#if USBPAD_API_LEVEL >= 6
  /* R10b: a pad the driver knows whose bring-up failed (the Controller
   * page says where): "Switch Pro didn't start", not "not supported" */
  else if (ev->type == USBPAD_EV_UNSUPPORTED && ev->step && ev->kind)
    snprintf(text, size, "%s didn't start. Plug it in again.", ev->name);
#endif
  else if (ev->type == USBPAD_EV_UNSUPPORTED)
    snprintf(text, size, "This controller isn't supported");
  else if (ev->type == USBPAD_EV_NEEDS_POWER)
    snprintf(text, size, "Controller needs more power: try a powered hub");
  else if (ev->type == USBPAD_EV_REPLUG_NEEDED)
    snprintf(text, size, "Plug the controller in again");
  else
    usbpad_event_text(ev, text, size);
}

void nspire_usbpad_frame(uint32_t hotkeys)
{
  struct usbpad_event ev;
  char text[64];
  u32 now;

  if (!started)
    return;
  now = nspire_ticks();
  /* PAD4: a come-up the OS hasn't answered: its message now, so that it is
   * on the screen while the next point between frames gives the OS its
   * time in one go (the game stands still then). */
  if (cu.on && !cu.msg && !cu.burst && !cu.step[CU_OFFER] && cu_may())
  {
    const char *vis = nspire_osd_visible();

    if (!vis || strcmp(vis, CU_TEXT))
      nspire_osd_message("%s", CU_TEXT);
    cu.msg = 1;
  }
#ifdef USBPAD_OS_HAS_HUB
  /* hub: the OS is at work on a device behind the hub: said before the game
   * stands still for it (hub_point, at the next point between frames) */
  if (hub_in && hub_active && hub_coming && !hub_msg && !hub_stop)
  {
    const char *vis = nspire_osd_visible();

    if (!vis || strcmp(vis, HUB_TEXT))
      nspire_osd_message("%s", HUB_TEXT);
    hub_msg = 1;
  }
#endif
  /* ...and kept up while the OS has the device and works on it (not a new
   * message: the same one longer) */
  if (nspire_usbpad_coming_up())
  {
    const char *vis = nspire_osd_visible();

    if (vis && !strcmp(vis, CU_TEXT))
      nspire_osd_extend();
  }
  /* gpSP's message line holds one message, and a toast would replace
   * gpSP's own at once. So the toasts wait MESSAGE_TIME where one may have
   * just been posted: after a hotkey (a state saved...), and as the game
   * starts or comes back from the menus or the file list (its "Resumed from
   * slot" comes just before), told by the keys having been read more than
   * twice since the last frame. UNVERIFIED that no other message is lost:
   * the link's come at any time (requests doc: a way to see the line is
   * busy). */
  if (hotkeys || keys_calls > 2)
  {
    holding = 1;
    hold_until = now + MESSAGE_TIME;
  }
  keys_calls = 0;
  if (holding && (s32)(now - hold_until) < 0)
    return;
  holding = 0;
  /* PAD3: the toast for a device at the adapter's end that doesn't work
   * (nspire_padstatus.h), over no message but a "..." one (a come-up's). */
  {
    const char *vis = nspire_osd_visible();
    size_t n = vis ? strlen(vis) : 0;

    /* (PAD4: not while a come-up is under way: it says so itself, and the
     * verdict may be a moment old when the OS's offer comes) */
    if ((!vis || (n > 3 && !strcmp(vis + n - 3, "..."))) && !cu_wants() && !usbpad_count() && !nspire_usbhub_busy())
    {
      const char *t = nspire_padstatus_toast_due();
      if (t)
      {
        nspire_osd_message("%s", t);
        nspire_padlog_add(PL_TOAST, 1, 0, 0, 0, 0, 0);
        toasted = 1;
        toast_at = now;
        return;
      }
    }
  }
  if (toasted && now - toast_at < TOAST_GAP)
    return;
  while (queued && stale(0))
    drop(0);
  if (!queued)
    return;
  ev = queue[0];
  drop(0);
  toast_text(&ev, text, sizeof(text));
  /* R11w part 2: a controller in: the plug's "do something" message is over
   * (taken down now, not left its 5 s); a failure's own message is the
   * plug's one that asks (no "USB not working" after it) */
  if (ev.type == USBPAD_EV_CONNECTED)
  {
    nspire_osd_withdraw(PADSTATUS_TOAST_TEXT);
    nspire_osd_withdraw(ARB_REPLUG_TEXT);
  }
  else if (ev.type != USBPAD_EV_DISCONNECTED)
    nspire_padstatus_asked();
  if (text[0])
    nspire_osd_message("%s", text);
  toasted = 1;
  toast_at = now;
}

#endif
