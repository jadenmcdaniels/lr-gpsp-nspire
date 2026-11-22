/* Bluetooth in gpSP: the glue between nspire-bt (nspire_bt.h, bt_port.h)
 * and the frontend. See nspire_bt_glue.h for where each part runs. */
#include "nspire_bt_glue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the settings themselves are nspire_config.c's, word 22) */
static const volatile uint32_t zero_word;
const volatile uint32_t *nspire_bt_wake = &zero_word;
unsigned nspire_bt_live;

#ifdef NSPIRE_NO_BT

int nspire_bt_built(void) { return 0; }
void nspire_bt_glue_start(void) {}
void nspire_bt_glue_apply(void) {}
uint32_t nspire_bt_glue_poll(int in_game) { (void)in_game; return 0; }
void nspire_bt_glue_frame(uint32_t hotkeys) { (void)hotkeys; }
int nspire_bt_os_time(uint32_t due, uint32_t began, int paced) { (void)due; (void)began; (void)paced; return 0; }
int nspire_bt_glue_audio_frame(int mode, int usb_streaming) { (void)mode; (void)usb_streaming; return 0; }
void nspire_bt_glue_audio_write(const int16_t *lr, unsigned frames) { (void)lr; (void)frames; }
void nspire_bt_glue_audio_write_mix(uint32_t *w1, unsigned n1, uint32_t *w2, unsigned n2, unsigned flags)
{
  (void)w1; (void)n1; (void)w2; (void)n2; (void)flags;
}
unsigned nspire_bt_glue_audio_delay_ms(void) { return 0; }
int nspire_bt_glue_headphones(void) { return 0; }
const char *nspire_bt_glue_audio_summary(void) { return NULL; }
const char *nspire_bt_glue_headphones_name(void) { return NULL; }
const char *nspire_bt_glue_pad_name(void) { return NULL; }
int nspire_bt_glue_pad_kind(void) { return 0; }
int nspire_bt_glue_pad_count(void) { return 0; }
const char *nspire_bt_glue_slot_name(int slot) { (void)slot; return NULL; }
int nspire_bt_glue_slot_number(int slot) { (void)slot; return 0; }
void nspire_bt_glue_codec(void) {}
void nspire_bt_glue_atexit(void) {}
void nspire_bt_glue_flush(void) {}
const char *nspire_bt_glue_status(void) { return NULL; }
int nspire_bt_usb_active(void) { return 0; }
uint32_t nspire_bt_usb_traffic(void) { return 0; }
int nspire_bt_glue_log_text(char *out, unsigned size) { if (size) out[0] = 0; return 0; }
int nspire_bt_glue_stop_within(unsigned ms) { (void)ms; return 0; }
void nspire_bt_glue_write_log(void) {}
const char *nspire_bt_glue_note(unsigned *seq) { if (seq) *seq = 0; return ""; }
int nspire_bt_glue_note_event(int *type, int *player, int *kind, int *err, char *name, unsigned size)
{
  (void)type; (void)player; (void)kind; (void)err; (void)name; (void)size;
  return 0;
}
int nspire_bt_glue_toast_text(int type, int player, int kind, int err, const char *name, char *out, unsigned size)
{
  (void)type; (void)player; (void)kind; (void)err; (void)name;
  if (size)
    out[0] = 0;
  return 0;
}

#else

#include <os.h>
#include "common.h"
#include "nspire.h"
#include "nspire_os.h"
#include "nspire_frontend.h"
#include "nspire_paths.h"
#include "nspire_padlog.h"
#include "nspire_link.h"
#include "nspire_audio.h"
#include "nspire_usbpad_glue.h"
#include "nspire_exit.h"
#include "nspire_bluetooth.h"
#include "nspire_bt.h"
#include "bt_port.h"

void nspire_bt_glue_flush(void);
void nspire_bt_glue_write_log(void);

#define TOAST_GAP_MS    1000    /* toasts a second apart at the most */
#define HOLD_MS         2000    /* gpSP's own message after a hotkey, a menu: its time first */
#define QUIET_START_MS  10000   /* "Bluetooth on" not said this soon after gpSP's start (the dongle was in) */
#define TASK_FORCE_MS   250     /* the OS's task time while the dongle comes or goes, forced (the pad glue's PAD_TASK_MS) */
#define MENU_TASK_MS    40      /* menus: a sleep this often while the dongle comes or goes */
#define TOASTS          8
#define FAILED_SAME_MS  10000   /* the same "failed" words not toasted twice this close */
/* The live log (gpsp_bt_live.txt.tns): the port's latest lines and the
 * counters, rewritten as things happen, so a run that freezes (the BT log
 * is otherwise written at the exit) leaves its last Bluetooth moments; the
 * exit removes it, and the next start keeps one it finds as
 * gpsp_bt_froze.txt.tns. A write is ~7 KB (~4 ms on a CX II) but stalls the
 * USB callbacks 100-290 ms (R11t): written at once (LIVE_URGENT_MS apart)
 * when the port's state, the watchdog, the pads or the headphones change
 * (the moments a freeze would follow); other new lines (the heartbeat line
 * the glue puts in the port's log every LIVE_BEAT_MS while a dongle is in)
 * only while nothing streams (no sound adapter, no headphones, no
 * controller on Bluetooth), LIVE_GAP_MS apart. Never while the link runs
 * (nspire_link_quiet_for_writes); LIVE_MAX writes a run. */
#define LIVE_URGENT_MS  500
#define LIVE_GAP_MS     2000
#define LIVE_BEAT_MS    60000
#define LIVE_LINES      64
#define LIVE_MAX        300

static int started, stopped, exit_set;
static unsigned flags;                  /* the last nspire_bt_poll's */
static uint32_t keys;
static uint32_t start_ms;
static int bt_mode = -1;                /* the headphones' last frame_begin mode */
static int on_seen;                     /* Bluetooth was on once (a later "on" is news) */

/* the clock: ms from the timer's ticks, wrapping at 2^32 */
static uint32_t clock_ms(void)
{
  static u32 last, ms, rem;
  u32 t = nspire_ticks();
  u64 x = (u64)(t - last) * 1000u + rem;
  last = t;
  ms += (u32)(x / NSPIRE_TICK_HZ);
  rem = (u32)(x % NSPIRE_TICK_HZ);
  return ms;
}

static void sleep_tick(void)
{
  (void)nspire_os_sleep();
}

/* ---- counters for the logs and the tests ---- */
static uint32_t n_windows, n_sleeps, n_polls, n_polls_frame, n_flushes, n_toasts, n_events, n_audio_frames;

int nspire_bt_built(void) { return 1; }

/* ---- the toasts and the page's note ---- */
static char toast_q[TOASTS][64];
static unsigned toast_rd, toast_wr;
static uint32_t toast_at, hold_until;
static int toast_any, holding, menu_seen;
static char note[96];
static unsigned note_seq;
static struct { int type, player, kind, err; char name[40]; } note_ev;   /* (the page fits its own words) */

static void toast(const char *text)
{
  if (toast_wr - toast_rd >= TOASTS)
    toast_rd++;
  snprintf(toast_q[toast_wr++ % TOASTS], sizeof(toast_q[0]), "%s", text);
}

const char *nspire_bt_glue_note(unsigned *seq)
{
  if (seq)
    *seq = note_seq;
  return note;
}

int nspire_bt_glue_note_event(int *type, int *player, int *kind, int *err, char *name, unsigned size)
{
  if (!note_seq)
    return 0;
  *type = note_ev.type;
  *player = note_ev.player;
  *kind = note_ev.kind;
  *err = note_ev.err;
  snprintf(name, size, "%s", note_ev.name);
  return 1;
}

/* A toast's words, OSD_TEXT_MAX (63) characters at the most whatever the
 * name: the controllers' and the headphones' in ui7's one form
 * (nspire_bt_toast_text: "Controller 2 connected (Bluetooth)"), the rest
 * short, a name cut to fit. */
int nspire_bt_glue_toast_text(int type, int player, int kind, int err, const char *name, char *out, unsigned size)
{
  int n;
  (void)kind;
  switch (type)
  {
  case NSPIRE_BT_EV_PAD_CONNECTED:
  case NSPIRE_BT_EV_PAD_DISCONNECTED:
    nspire_bt_toast_text(out, size, BT_CONTROLLER, player, type == NSPIRE_BT_EV_PAD_CONNECTED);
    return (int)strlen(out);
  case NSPIRE_BT_EV_AUDIO_CONNECTED:
  case NSPIRE_BT_EV_AUDIO_DISCONNECTED:
    nspire_bt_toast_text(out, size, BT_HEADPHONES, 0, type == NSPIRE_BT_EV_AUDIO_CONNECTED);
    return (int)strlen(out);
  case NSPIRE_BT_EV_ON:
    n = snprintf(out, size, "Bluetooth on");
    break;
  case NSPIRE_BT_EV_OFF:
    n = snprintf(out, size, "Bluetooth off");
    break;
  case NSPIRE_BT_EV_FAILED:
  {
    bt_port_info_t in;
    bt_port_info(&in);
    /* (the port's watchdog: a dongle that stopped answering, no detach:
     * restarted once, else given up; nspire-bt bt_port_expect_in) */
    if (in.mem_fail_what && err != NSPIRE_BT_ENODONGLE && strstr(bt_port_problem(), "not enough memory"))
    {
      /* (the start refused, or the dongle given up: an allocation failed) */
      n = snprintf(out, size, "Bluetooth off: not enough memory (%lu KB)",
                   (unsigned long)(in.mem_fail_bytes + 1023) / 1024);
      break;
    }
    n = snprintf(out, size, "%s", err == NSPIRE_BT_ENODONGLE ? "Bluetooth: dongle unplugged" :
                 in.hung == 1 ? "Bluetooth dongle stopped answering: restarting it" :
                 in.hung == 2 ? "Bluetooth dongle stopped answering: replug it" :
                 in.firmware == 3 ? "Bluetooth: the dongle's firmware file is missing" :
                 /* (offered again after a hub's brown-out, no detach for the old one: a new start) */
                 bt_port_state() == BT_PORT_STARTING ? "Bluetooth: restarting the dongle" :
                 "Bluetooth stopped: see Settings > Bluetooth");
    break;
  }
  case NSPIRE_BT_EV_PAD_REFUSED:
    n = snprintf(out, size, "Two controllers in already: %.24s not used", name);
    break;
  case NSPIRE_BT_EV_PAIRED:
    n = snprintf(out, size, "Paired: %.40s", name);
    break;
  case NSPIRE_BT_EV_CONNECT_FAILED:
#if NSPIRE_BT_API_LEVEL >= 3
    if (err == NSPIRE_BT_ERESTART)
    {
      /* (the dongle was restarted under the connect: it is as it was, enter tries again) */
      n = snprintf(out, size, "Bluetooth restarted: %.24s stopped", name);
      break;
    }
#endif
    n = snprintf(out, size, "Couldn't connect to %.36s", name);
    break;
  case NSPIRE_BT_EV_NOT_SUPPORTED:
    n = snprintf(out, size, "%.28s: not a controller or headphones", name);
    break;
  default:
    n = snprintf(out, size, "Bluetooth");
    break;
  }
  return n < 0 ? 0 : n >= (int)size ? (int)size - 1 : n;
}

/* ---- pads ---- */

/* the number a pad is shown by: after the highest USB slot in use (the USB
 * pads keep their slots: Controller 1 and 2 are slots 0 and 1), then the
 * Bluetooth ones in their slots' order (all drive the GBA's one player) */
static int pad_number(int slot)
{
  unsigned m = nspire_usbpad_slots();
  int n = 0, i;

  while (m)
  {
    n++;
    m >>= 1;
  }
  for (i = 0; i < slot && i < NSPIRE_BT_PADS; i++)
    if (nspire_bt_pad(i)->connected)
      n++;
  return n + 1;
}

/* What the menus and the picture's corner ask every draw, kept as the pads
 * come and go (no library call on a draw): the first pad's name and kind,
 * each slot's kind name and number (its lights say it). */
static char cached_name[48];
static int cached_kind, slot_numbers[NSPIRE_BT_PADS];
static const char *slot_names[NSPIRE_BT_PADS];

static void pads_changed(void)
{
  int i, first = -1;

  for (i = 0; i < NSPIRE_BT_PADS; i++)
  {
    const struct usbpad_state *s = nspire_bt_pad(i);
    if (!s->connected)
    {
      slot_names[i] = NULL;
      slot_numbers[i] = 0;
      continue;
    }
    if (first < 0)
      first = i;
    slot_names[i] = nspire_bt_kind_name(s->kind);
    if (slot_numbers[i] != pad_number(i))
    {
      slot_numbers[i] = pad_number(i);
      nspire_bt_pad_set_player(i, slot_numbers[i]);   /* its lights say the same number */
    }
  }
  cached_kind = first >= 0 ? nspire_bt_pad(first)->kind : 0;
  if (first >= 0)
    snprintf(cached_name, sizeof(cached_name), "%s (Bluetooth)", nspire_bt_kind_name(cached_kind));
  else
    cached_name[0] = 0;
}

static uint32_t pad_keys_now(void)
{
  uint32_t k = 0;
  int i;

  for (i = 0; i < NSPIRE_BT_PADS; i++)
  {
    const struct usbpad_state *s = nspire_bt_pad(i);
    uint32_t b = s->buttons;

    if (!s->connected)
      continue;
    /* "labels" swaps bottom and right, left and top (as the USB pads') */
    if (nspire_usbpad_layout_labels(s->vid, s->pid))
      b = (b & ~0xF0u) | (b & (USBPAD_SOUTH | USBPAD_WEST)) << 1 | (b & (USBPAD_EAST | USBPAD_NORTH)) >> 1;
    k |= b;
  }
  return k;
}

const char *nspire_bt_glue_pad_name(void)
{
  return started && cached_name[0] ? cached_name : NULL;
}

int nspire_bt_glue_pad_kind(void)
{
  return started ? cached_kind : 0;
}

int nspire_bt_glue_pad_count(void)
{
  return started ? nspire_bt_pad_count() : 0;
}

const char *nspire_bt_glue_slot_name(int slot)
{
  return started && slot >= 0 && slot < NSPIRE_BT_PADS ? slot_names[slot] : NULL;
}

int nspire_bt_glue_slot_number(int slot)
{
  return started && slot >= 0 && slot < NSPIRE_BT_PADS ? slot_numbers[slot] : 0;
}

/* ---- events ---- */

static char failed_pair[64];        /* a "failed" of BTstack going off, waiting for the port's verdict */

/* hub17's (nspire_usbpad_glue.c): 1 from a power fall or a dead hub seen under
 * watch, through the hub's restart, until 5 s after its devices are offered
 * again (or the recovery gives up). The dongle's "unplugged" and "replug it"
 * are then that restart's, and the hub's own toast says it: log only. Weak:
 * 0 in a build without it; host_bt_hub_recovering is the PC tests' stand-in. */
int nspire_hub_recovering(void) __attribute__((weak));
int host_bt_hub_recovering(void) __attribute__((weak));

static int hub_recovering(void)
{
  if (host_bt_hub_recovering && host_bt_hub_recovering())
    return 1;
  return nspire_hub_recovering ? nspire_hub_recovering() : 0;
}
static uint32_t failed_pair_at;

static void take_events(int in_game)
{
  struct nspire_bt_event ev;
  char text[64], who[40];

  while (nspire_bt_next_event(&ev))
  {
    int say = 1;

    n_events++;
    if (ev.type == NSPIRE_BT_EV_PAD_CONNECTED || ev.type == NSPIRE_BT_EV_PAD_DISCONNECTED)
    {
      int slot = ev.player;

      if (slot >= 0)
        ev.player = (int8_t)(pad_number(slot) - 1);   /* (pads_changed sets its lights to it) */
      nspire_padlog_add(PL_BT, PL_BT_PAD, ev.type == NSPIRE_BT_EV_PAD_CONNECTED, (u32)(slot & 0xFF), ev.kind,
                        (u32)ev.addr[3] << 16 | (u32)ev.addr[4] << 8 | ev.addr[5], 0);
      nspire_padlog_milestone(PL_BT, PL_BT_PAD, ev.type == NSPIRE_BT_EV_PAD_CONNECTED, (u32)(slot & 0xFF),
                              ev.kind, (u32)ev.addr[3] << 16 | (u32)ev.addr[4] << 8 | ev.addr[5], 0);
    }
    else if (ev.type == NSPIRE_BT_EV_AUDIO_CONNECTED || ev.type == NSPIRE_BT_EV_AUDIO_DISCONNECTED)
    {
      nspire_padlog_add(PL_BT, PL_BT_AUDIO, ev.type == NSPIRE_BT_EV_AUDIO_CONNECTED, 0, 0,
                        (u32)ev.addr[3] << 16 | (u32)ev.addr[4] << 8 | ev.addr[5], 0);
      nspire_padlog_milestone(PL_BT, PL_BT_AUDIO, ev.type == NSPIRE_BT_EV_AUDIO_CONNECTED, 0, 0,
                              (u32)ev.addr[3] << 16 | (u32)ev.addr[4] << 8 | ev.addr[5], 0);
    }
    else if (ev.type == NSPIRE_BT_EV_ON || ev.type == NSPIRE_BT_EV_FAILED || ev.type == NSPIRE_BT_EV_OFF)
    {
      nspire_padlog_add(PL_BT, PL_BT_STATE, ev.type, (u32)(ev.err & 0xFFFF), (u32)nspire_bt_state(), 0, 0);
      nspire_padlog_milestone(PL_BT, PL_BT_STATE, ev.type, (u32)(ev.err & 0xFFFF), (u32)nspire_bt_state(), 0, 0);
      /* the dongle coming in at the start (it was in) needs no word; a
       * dongle plugged in later, or back, does */
      if (ev.type == NSPIRE_BT_EV_ON)
        say = on_seen || clock_ms() - start_ms >= QUIET_START_MS;
      if (ev.type == NSPIRE_BT_EV_ON)
        on_seen = 1;
      if (ev.type == NSPIRE_BT_EV_OFF)
        say = 0;   /* (the user turned it off: the page says so) */
    }
    else if (ev.type == NSPIRE_BT_EV_SCAN_DONE)
      continue;   /* (the page's list says it) */
    else if (!in_game)
      say = 0;    /* (pairing, a failure, "not a controller": the page's band says it, not a later toast) */
    if (ev.name[0])
      snprintf(who, sizeof(who), "%s", ev.name);
    else
      snprintf(who, sizeof(who), "%02X:%02X:%02X:%02X:%02X:%02X", ev.addr[0], ev.addr[1], ev.addr[2], ev.addr[3],
               ev.addr[4], ev.addr[5]);
    nspire_bt_glue_toast_text(ev.type, ev.player + 1, ev.kind, ev.err, who, text, sizeof(text));
    if (ev.type == NSPIRE_BT_EV_ON)
      failed_pair[0] = 0;
    if (ev.type == NSPIRE_BT_EV_FAILED)
    {
      /* (BTstack off, then the port's verdict on the same dongle: two
       * "failed" events with the same words, nothing on between: said once.
       * Any other repeat, a second unplug after an "on", is said again) */
      if (say && ev.err == NSPIRE_BT_EPORT && failed_pair[0] && !strcmp(text, failed_pair) &&
          clock_ms() - failed_pair_at < FAILED_SAME_MS)
        say = 0;
      failed_pair[0] = 0;
      if (ev.err != NSPIRE_BT_EPORT)
      {
        snprintf(failed_pair, sizeof(failed_pair), "%s", text);
        failed_pair_at = clock_ms();
      }
      /* (a hub being restarted: its dongle gone or given up is the restart's
       * doing, not something to replug; the page and the logs keep it) */
      if (say && (ev.err == NSPIRE_BT_ENODONGLE || strstr(text, ": replug it")) && hub_recovering())
      {
        say = 0;
        bt_port_log("glue: \"%s\" not shown: the hub is being restarted", text);
      }
    }
    snprintf(note, sizeof(note), "%s", text);
    note_ev.type = ev.type;
    note_ev.player = ev.player + 1;
    note_ev.kind = ev.kind;
    note_ev.err = ev.err;
    snprintf(note_ev.name, sizeof(note_ev.name), "%s", who);
    note_seq++;
    if (say)
      toast(text);
  }
}

/* ---- the OS's time ---- */

static void window(void)
{
  u32 old;
  volatile unsigned spin;

  nspire_os_keypad_keep_off();
  old = nspire_irqs_unmask();
  for (spin = 0; spin < 4; spin++)
    ;   /* (a pending interrupt is taken at the first instruction with them on) */
  nspire_irqs_restore(old);
  nspire_os_after_os_time();
  n_windows++;
}

static void poll_now(int in_game);
static void audio_second(u32 t0, u32 t1, u32 t2);
static void hp_delay_update(void);

int nspire_bt_os_time(uint32_t due, uint32_t began, int paced)
{
  u32 since;

  (void)began;
  if (!started)
    return 0;
  {
    u32 t0 = nspire_ticks(), t1;
    /* the dongle's transfers complete where the CPU takes the USB interrupt:
     * a moment of it (as the pad glue's windows; the sound's take it too) */
    if (flags & NSPIRE_BT_WANT_IRQ)
      window();
    /* then the run loop: what the window brought, and what blocks (an HCI
     * command is a control request of a few ms, one a poll at most; a file
     * read of the firmware a chunk a poll) out of the frame's spare time, in
     * front of its wait, not in its keys (keys_poll ORs the pads' buttons kept
     * here) */
    t1 = nspire_ticks();
    poll_now(1);
    audio_second(t0, t1, nspire_ticks());   /* (bt10: the headphones' seconds, gpsp_bt.txt.tns) */
  }
  if (!(flags & NSPIRE_BT_WANT_TASK))
    return 0;
  /* the dongle comes or goes: the OS's USB task. The frames' waits sleep in
   * it (OS_WANT_BT: nspire_wait_until, free); a sound adapter streaming
   * gives it its sleeps anyway; else (fast forward has no waits) one every
   * TASK_FORCE_MS, the pad glue's rate */
  (void)due;
  (void)paced;
  if (nspire_audio_serves_os())
    return 0;
  since = nspire_os_since_sleep();
  if (since < (u32)TASK_FORCE_MS * (NSPIRE_TICK_HZ / 1000) || !nspire_os_sleep())
    return 0;
  n_sleeps++;
  return 1;
}

/* ---- the live log ---- */

static uint32_t live_at, live_beat_at, live_count_seen, n_live;
static int live_any;
static uint32_t live_critical, live_other;   /* what the last write saw: the watchdog, the port; pads, headphones */
static char beat_line[160];
static uint32_t beat_seq, beat_seq_seen;

static void live_path(char *out, unsigned size, const char *name)
{
  nspire_save_file(out, size, name);
}

/* At the start: a live log left by the last run (it did not end cleanly)
 * kept as gpsp_bt_froze.txt.tns, said in this run's log. */
static void live_keep_last(void)
{
  char from[600], to[600];
  FILE *f;

  live_path(from, sizeof(from), "gpsp_bt_live.txt.tns");
  f = fopen(from, "rb");
  if (!f)
    return;
  fclose(f);
  live_path(to, sizeof(to), "gpsp_bt_froze.txt.tns");
  remove(to);
  if (rename(from, to) != 0)
    remove(from);
  bt_port_log("glue: the last run did not end cleanly: its live Bluetooth log kept as gpsp_bt_froze.txt.tns");
  nspire_link_log("bluetooth: the last run did not end cleanly: its live Bluetooth log is gpsp_bt_froze.txt.tns");
}

/* (no heap taken: the counters from a buffer on the stack, the port's lines
 * as they are kept) */
static void live_write(uint32_t now)
{
  char path[600], text[1200];
  FILE *f;
  int k;

  live_path(path, sizeof(path), "gpsp_bt_live.txt.tns");
  f = fopen(path, "wb");
  if (!f)
    return;
  nspire_bt_glue_log_text(text, sizeof(text));
  fprintf(f, "gpSP Bluetooth log, live at %lu ms (write %lu; the exit removes this file: here, the run did not end "
          "cleanly)\n%s\n\n", (unsigned long)(now - start_ms), (unsigned long)n_live + 1, text);
  for (k = LIVE_LINES - 1; k >= 0; k--)
  {
    const char *l = bt_port_log_recent((unsigned)k);
    if (l && l[0])
      fprintf(f, "%s\n", l);
  }
  if (beat_line[0])
    fprintf(f, "%s (the glue's last heartbeat)\n", beat_line);
  fclose(f);
  n_live++;
  live_any = 1;
}

/* Each frame's (or menu pass's) poll, after it: the heartbeat line and the
 * live write when they are due (a compare or two otherwise). */
static void live_tick(void)
{
  uint32_t now, critical, other;
  bt_port_info_t in;
  int streaming;

  if (n_live >= LIVE_MAX)
    return;
  now = clock_ms();
  if (now - live_at < LIVE_URGENT_MS)
    return;
  bt_port_info(&in);
  if (!in.dongles_seen && !bt_port_problem()[0])
    return;
  if (bt_port_usb_active() && now - live_beat_at >= LIVE_BEAT_MS)
  {
    /* the heartbeat: the glue's own line (never in the port's ring, where
     * a long session's would push the incident's lines out), replaced */
    live_beat_at = now;
    beat_seq++;
    snprintf(beat_line, sizeof(beat_line), "[%4lu.%03lu] glue: alive: polls %lu (%lu framed), windows %lu, "
             "sleeps %lu; events %lu, ACL in %lu out %lu, hung %u", (unsigned long)(now / 1000),
             (unsigned long)(now % 1000), (unsigned long)n_polls, (unsigned long)n_polls_frame,
             (unsigned long)n_windows, (unsigned long)n_sleeps, (unsigned long)in.events_in,
             (unsigned long)in.acl_in, (unsigned long)in.acl_out, (unsigned)in.hung);
  }
  if ((bt_port_log_count() == live_count_seen && beat_seq == beat_seq_seen) || !nspire_link_quiet_for_writes())
    return;
  /* critical: the watchdog, the port given up (written whatever plays: the
   * moments a freeze would follow); other changes (pads, headphones, the
   * port's other states) and new lines only while nothing plays that a
   * 100-290 ms stall of the USB callbacks would be heard or felt in (R11t):
   * a sound adapter, headphones, a controller on the cable */
  critical = (in.watchdog_fires & 0xFF) | (u32)in.hung << 8 | (u32)(bt_port_state() == BT_PORT_FAILED) << 12;
  other = (u32)bt_port_state() | (u32)nspire_bt_pad_count() << 4 | (u32)(nspire_bt_audio_active() != 0) << 8;
  streaming = nspire_usbaudio_active() || nspire_bt_audio_active() || nspire_usbpad_pads() > 0;
  if (critical == live_critical)
  {
    if (streaming)
      return;
    if (other == live_other && now - live_at < LIVE_GAP_MS)
      return;
    if (other == live_other && nspire_bt_pad_count())
      return;   /* (new lines alone: not under a Bluetooth controller's play either) */
  }
  live_critical = critical;
  live_other = other;
  live_count_seen = bt_port_log_count();
  beat_seq_seen = beat_seq;
  live_at = now;
  live_write(now);
}

/* r19-bt (a freeze at quit that left nothing of the exit in any
 * log): the stop's steps (bt_port_config_t.stop_step: the library's, the
 * port's, the USB transport's), each appended to the live log BEFORE it
 * runs (the line alone: a small write), after a fresh live log at the
 * stop's start; a stop that never comes back leaves the step it was in.
 * Not while the link runs (nspire_link_quiet_for_writes, as ever). */
static int stop_marks;
static uint32_t stop_marks_n, stop_marks_ms;
static void live_append(const char *line)
{
  char path[600];
  FILE *f;
  uint32_t t;

  if (!stop_marks)
    return;
  t = clock_ms();
  live_path(path, sizeof(path), "gpsp_bt_live.txt.tns");
  f = fopen(path, "ab");
  if (!f)
    return;
  fprintf(f, "%s\n", line);
  fclose(f);
  live_any = 1;
  stop_marks_n++;
  stop_marks_ms += clock_ms() - t;
}

/* r19-bt, a freeze after a dongle was plugged and Bluetooth switched off and on in its start-up (the live log's
 * last line its first USB requests): the bring-up's steps ("bt start: step N: ...") the same way, during play
 * too: a fresh live log at the first of a start-up, then each step's line appended before it runs (a small
 * write: the USB callbacks wait for it, R11t, a few times a plug-in), the trail closed at WORKING. Not while
 * the link runs; START_MARKS_MAX a run. */
#define START_MARKS_MAX 64
static int start_trail;
static uint32_t start_marks_n;
static void start_mark(const char *line)
{
  int up = strstr(line, ": up: ") != NULL;

  if (start_marks_n >= START_MARKS_MAX || !nspire_link_quiet_for_writes())
    return;
  start_marks_n++;
  if (!start_trail)
  {
    uint32_t n0 = n_live;   /* (not one of the run's live writes: counted with the trail) */
    live_write(clock_ms());   /* (its lines end with this one) */
    n_live = n0;
    start_trail = !up;
    return;
  }
  stop_marks++;
  live_append(line);
  stop_marks--;
  stop_marks_n--;   /* (counted here, not as the stop's) */
  if (up)
    start_trail = 0;
}

static void stop_step(const char *line)
{
  if (!strncmp(line, "bt start:", 9) && !stop_marks)
    start_mark(line);
  else
    live_append(line);
}

/* the exit: the full log is written; the live one is not left behind */
static void live_remove(void)
{
  char path[600];

  if (!live_any)
    return;
  live_path(path, sizeof(path), "gpsp_bt_live.txt.tns");
  remove(path);
  live_any = 0;
}

/* ---- start, poll ---- */

static void codec_apply(void)
{
  int mono = nspire_bt_codec == NSPIRE_BT_CODEC_MONO || (nspire_bt_codec == NSPIRE_BT_CODEC_AUTO && !is_cx2);

  /* the CX: mono (SBC stereo costs ~12% of its CPU, docs/profiles.md); the
   * CX II joint stereo, bitpool 35 (the library's default); High: joint
   * stereo, bitpool 53 (A2DP's "high quality", what phones send: the
   * encoder ~7% more, 69 packets a second of 5 frames, not 43 of 8) */
  nspire_bt_audio_set_codec(mono, nspire_bt_codec == NSPIRE_BT_CODEC_HIGH ? 53 : 0);
}

/* An exit() that didn't come through quit(): the store, the stop within
 * what the exit's USB clock leaves (half, 1.5 s at most), the log; a USB
 * callback still inside gpSP: the exit's restart, as quit()'s. Registered
 * after the pad driver's handler (nspire_bt_glue_atexit), so it runs first,
 * while the pad driver and its keep-awake are still up. */
static void stop_at_exit(void)
{
  unsigned left;

  if (!started)
    return;
  nspire_bt_glue_flush();
  left = nspire_exit_usb_left_ms() / 2;
  if (nspire_bt_glue_stop_within(left < 1500 ? left : 1500) != 0)
  {
    nspire_bt_glue_write_log();
    nspire_exit_usb_timed_out("Bluetooth");
  }
  nspire_bt_glue_write_log();
}

void nspire_bt_glue_atexit(void)
{
  if (!exit_set)
    exit_set = atexit(stop_at_exit) == 0;
}

/* The key store written now, from quit() before its USB shutdowns (the stop
 * writes no file), or a menu pass. */
void nspire_bt_glue_flush(void)
{
  if (started && !bt_port_dead() && bt_port_tlv_dirty())
  {
    nspire_bt_flush();
    n_flushes++;
  }
}

/* A BTstack assert with no guarded call to come back to (bt_port.h's
 * backstop; the library's own calls and polls are guarded, the dongle given
 * up and the game carries on): the game's save and the logs written, then
 * the exit's restart (its message), as a USB shutdown that timed out. */
static void fatal(const char *why)
{
  nspire_link_log("bluetooth: %s: the game's save and the logs, then a restart", why);
  update_backup_force();
  nspire_bt_glue_write_log();
  nspire_usbpad_exit_log();
  nspire_exit_usb_timed_out("Bluetooth (an internal check failed)");
}

static void start(void)
{
  static char dir[512];
  bt_port_config_t cfg;

  if (started || stopped)
    return;
  nspire_save_file(dir, sizeof(dir), "bt");
  memset(&cfg, 0, sizeof(cfg));
  cfg.now_ms = clock_ms;
  cfg.sleep_tick = sleep_tick;
  cfg.data_dir = dir;
  cfg.log_level = 1;          /* + every HCI command and event in short: the first hardware runs */
  cfg.keep_usb_awake = 0;     /* (the pad glue keeps the OS's USB awake) */
  cfg.fatal = fatal;          /* (a BTstack assert outside the library's guarded calls: the backstop) */
  cfg.stop_step = stop_step;  /* (the stop's steps, each in the live log before it runs) */
  start_ms = clock_ms();
  if (nspire_bt_start(&cfg) != 0)
  {
    char text[64];
    nspire_padlog_add(PL_BT, PL_BT_STATE, 0, 0xFFFF, (u32)nspire_bt_state(), 0, 0);
    nspire_padlog_milestone(PL_BT, PL_BT_STATE, 0, 0xFFFF, (u32)nspire_bt_state(), 0, 0);
    snprintf(note, sizeof(note), "%s", nspire_bt_status());
    note_seq++;
    /* said on screen too (the game's first frames: nspire_bt_glue_frame runs
     * while the toast waits, nspire_bt_live) */
    nspire_bt_glue_toast_text(NSPIRE_BT_EV_FAILED, 0, 0, NSPIRE_BT_EPORT, "", text, sizeof(text));
    toast(text);
    nspire_bt_live = 1;
    return;
  }
  started = 1;
  live_keep_last();
  live_beat_at = clock_ms();
  nspire_bt_wake = bt_port_idle_word();
  nspire_bt_live = 1;     /* (one poll: the state as it starts) */
  nspire_bt_audio_set_input_rate(sound_frequency);
  codec_apply();
}

void nspire_bt_page_install(void);

void nspire_bt_glue_start(void)
{
  nspire_bt_page_install();   /* (Settings lists Bluetooth: its page, nspire_bluetooth.h) */
  if (nspire_bt_enabled)
    start();
}

void nspire_bt_glue_codec(void)
{
  if (started && !bt_port_dead())
    codec_apply();
}

void nspire_bt_glue_apply(void)
{
  if (nspire_bt_enabled)
  {
    if (!started)
      start();
    else
      (void)nspire_bt_power(1);
  }
  else if (started)
    (void)nspire_bt_power(0);
  if (started)
    codec_apply();
}

uint32_t nspire_bt_glue_poll(int in_game)
{
  if (!started)
    return 0;
  if (in_game)
    return keys;   /* (the frame's poll ran in its OS-time slot: nspire_bt_os_time) */
  /* a menu pass: no frame loop places the dongle's time here */
  menu_seen = 1;
  if (flags & NSPIRE_BT_WANT_IRQ)
    window();
  /* (r19-bt) also while Bluetooth waits for its dongle with a USB hub in: a device on a hub is set up by the
   * OS's USB task, which runs only in the OS's time; a hardware log had the dongle's port enabled 6 s before
   * the OS offered it, the menus giving the OS no time meanwhile */
  if (((flags & NSPIRE_BT_WANT_TASK) || (nspire_bt_state() == NSPIRE_BT_NO_DONGLE && nspire_usbhub_present())) &&
      nspire_os_since_sleep() >= (u32)MENU_TASK_MS * (NSPIRE_TICK_HZ / 1000) && nspire_os_sleep())
    n_sleeps++;
  poll_now(0);
  return keys;
}

static void poll_now(int in_game)
{
  n_polls++;
  n_polls_frame += in_game;
  flags = (unsigned)nspire_bt_poll();
  if (flags & NSPIRE_BT_EVENTS)
    take_events(in_game);
  /* (HCI commands go out with the CPU's interrupts on, the OS running
   * meanwhile: its keypad line off again, its busy cursor) */
  if (flags & NSPIRE_BT_WANT_IRQ)
    nspire_os_after_os_time();
  nspire_os_want(OS_WANT_BT, (flags & NSPIRE_BT_WANT_TASK) != 0);
  keys = 0;
  {
    /* a pad in or out (Bluetooth or USB), or its kind known later: the
     * numbers, the lights, the names kept (bits 0-7 the USB slots; a byte a
     * Bluetooth slot: connected, its kind) */
    static unsigned seen = ~0u;
    unsigned now_set = nspire_usbpad_slots() & 0xFFu, i;

    for (i = 0; i < NSPIRE_BT_PADS && i < 3; i++)
    {
      const struct usbpad_state *p = nspire_bt_pad((int)i);
      if (p->connected)
        now_set |= (0x80u | (p->kind & 0x7Fu)) << (8 + 8 * i);
    }
    if (now_set != seen)
    {
      seen = now_set;
      pads_changed();
    }
  }
  if (nspire_bt_pad_count())
    keys = pad_keys_now();
  /* the key store's file: a menu's time, never a game frame's */
  if (!in_game && (flags & NSPIRE_BT_WANT_FLUSH))
  {
    nspire_bt_flush();
    n_flushes++;
  }
  /* the glue's own work: pads, headphones, toasts, the OS's time; with the
   * port's word 0 too, the per-frame sites stop calling (NSPIRE_BT_IDLE) */
  nspire_bt_live = keys || nspire_bt_pad_count() || toast_rd != toast_wr || bt_mode > 0 ||
                   (flags & (NSPIRE_BT_WANT_IRQ | NSPIRE_BT_WANT_TASK | NSPIRE_BT_AUDIO | NSPIRE_BT_EVENTS));
  live_tick();
}

/* ---- toasts in the game ---- */

void nspire_bt_glue_frame(uint32_t hotkeys)
{
  uint32_t now;

  if (!started && toast_rd == toast_wr)
  {
    nspire_bt_live = 0;   /* (a start that failed: its toast is out) */
    return;
  }
  now = clock_ms();
  if (hotkeys || menu_seen)
  {
    holding = 1;
    hold_until = now + HOLD_MS;
    menu_seen = 0;
  }
  if (toast_rd == toast_wr)
    return;
  if (holding && (int32_t)(now - hold_until) < 0)
    return;
  holding = 0;
  if (toast_any && now - toast_at < TOAST_GAP_MS)
    return;
  nspire_osd_message("%s", toast_q[toast_rd++ % TOASTS]);
  toast_any = 1;
  toast_at = now;
  n_toasts++;
}

/* ---- sound ---- */

static unsigned bt_volume = ~0u;

/* bt10: one record a second while headphones stream, for gpsp_bt.txt.tns
 * (an r16 hardware run: the game 55-58 frames, 27-30 pictures while they
 * streamed, the cost not known part by part): the frame's window (the USB
 * interrupt's completions: the dongle's and a sound adapter's), the poll
 * (BTstack, the media clock's blocks: SBC, packets, the transport), the
 * sound's write (the mixer read and resampled into the ring), in the
 * timer's ticks summed; and the library's counters' steps that second. */
#define BT_SEC_KEEP 180                 /* three minutes: 4.7 KB */
struct bt_sec
{
  uint16_t t, frames, polls;
  uint16_t window_10us, poll_10us, poll_max_10us, write_10us;
  uint16_t packets, sbc, cached;
  uint8_t ring_ms, late, under, trims;
  int16_t speed;                  /* the rate's factor - 1, 1/10000 (saturated: +3.2767 at most) */
  uint16_t delay_ms;              /* bt10: how late they play (nspire_bt_glue_audio_delay_ms) */
};
static struct bt_sec bt_secs[BT_SEC_KEEP];
static unsigned bt_sec_n;
static u32 sec_at, sec_win, sec_poll, sec_poll_max, sec_write, sec_frames, sec_polls;
static struct nspire_bt_audio_stats sec_st;
static int sec_on;

static uint16_t ticks_10us(u32 t)
{
  u64 v = (u64)t * 100000u / NSPIRE_TICK_HZ;
  return (uint16_t)(v > 65535 ? 65535 : v);
}

static void audio_second(u32 t0, u32 t1, u32 t2)
{
  struct nspire_bt_audio_stats st;
  struct bt_sec *r;

  if (!nspire_bt_audio_active())
  {
    sec_on = 0;
    return;
  }
  if (!sec_on)
  {
    sec_on = 1;
    sec_at = t0;
    sec_win = sec_poll = sec_poll_max = sec_write = sec_frames = sec_polls = 0;
    nspire_bt_audio_stats(&sec_st);
    return;
  }
  sec_win += t1 - t0;
  sec_poll += t2 - t1;
  if (t2 - t1 > sec_poll_max)
    sec_poll_max = t2 - t1;
  sec_polls++;
  if (t2 - sec_at < NSPIRE_TICK_HZ)
    return;
  nspire_bt_audio_stats(&st);
  r = &bt_secs[bt_sec_n++ % BT_SEC_KEEP];
  r->t = (uint16_t)((clock_ms() - start_ms) / 1000u);
  r->frames = (uint16_t)sec_frames;
  r->polls = (uint16_t)sec_polls;
  r->window_10us = ticks_10us(sec_win);
  r->poll_10us = ticks_10us(sec_poll);
  r->poll_max_10us = ticks_10us(sec_poll_max);
  r->write_10us = ticks_10us(sec_write);
  r->packets = (uint16_t)(st.packets - sec_st.packets);
  r->sbc = (uint16_t)(st.encoded - sec_st.encoded);
  r->cached = (uint16_t)(st.cached - sec_st.cached);
  r->ring_ms = (uint8_t)(st.ring_ms > 255 ? 255 : st.ring_ms);
  r->late = (uint8_t)(st.late - sec_st.late > 255 ? 255 : st.late - sec_st.late);
  r->under = (uint8_t)(st.underruns - sec_st.underruns > 255 ? 255 : st.underruns - sec_st.underruns);
  r->trims = (uint8_t)(st.trims - sec_st.trims > 255 ? 255 : st.trims - sec_st.trims);
  {
    /* (bt10: in 64 bits, saturated: fast forward's 16x is 15 x 10000) */
    int64_t sp = ((int64_t)st.speed - 65536) * 10000 / 65536;
    r->speed = (int16_t)(sp > 32767 ? 32767 : sp < -32768 ? -32768 : sp);
  }
  {
    unsigned d = nspire_bt_glue_audio_delay_ms();
    r->delay_ms = (uint16_t)(d > 65535 ? 65535 : d);
  }
  sec_st = st;
  sec_at = t2;
  sec_win = sec_poll = sec_poll_max = sec_write = sec_frames = sec_polls = 0;
}

static void audio_seconds_write(FILE *f)
{
  unsigned first = bt_sec_n > BT_SEC_KEEP ? bt_sec_n - BT_SEC_KEEP : 0, i;

  if (!bt_sec_n)
    return;
  fprintf(f, "\nheadphones' seconds (%u kept of %u): t s; game frames the sound went to them; polls; the frame's"
          " window (USB completions: the dongle's, an adapter's), the poll (BTstack, SBC, packets) and the"
          " sound's write (the mixer resampled into the ring), ms in that second, the longest poll; media"
          " packets, SBC frames encoded (cached silent ones); the ring ms; the rate (+327.67%% at most: fast"
          " forward's); late, underruns, trims; how late they play (their ring and lead averaged, and their"
          " own buffer: a USB card beside them is held back by that, less its own queue)\n",
          bt_sec_n - first, bt_sec_n);
  for (i = first; i < bt_sec_n; i++)
  {
    const struct bt_sec *r = &bt_secs[i % BT_SEC_KEEP];
    int sp = r->speed;
    fprintf(f, "bt %us %u, %u polls; window %u.%02u, poll %u.%02u (max %u.%02u), write %u.%02u ms; %u pkts, %u sbc"
            " (%u cached); ring %u; rate %c%d.%02d%%; late %u under %u trims %u; delay %u ms\n", r->t, r->frames, r->polls,
            r->window_10us / 100, r->window_10us % 100, r->poll_10us / 100, r->poll_10us % 100,
            r->poll_max_10us / 100, r->poll_max_10us % 100, r->write_10us / 100, r->write_10us % 100,
            r->packets, r->sbc, r->cached, r->ring_ms, sp < 0 ? '-' : '+', (sp < 0 ? -sp : sp) / 100,
            (sp < 0 ? -sp : sp) % 100, r->late, r->under, r->trims, r->delay_ms);
  }
}

int nspire_bt_glue_headphones(void)
{
  return started && nspire_bt_audio_active();
}

int nspire_bt_glue_audio_frame(int mode, int usb_streaming)
{
  int take, want;

  if (!started)
    return 0;
  hp_delay_update();
  /* Automatic: the headphones and the USB adapter both, whichever are in
   * (bt10: both outputs play at once, not one of the two; bt9 gave
   * the headphones the sound alone); Bluetooth: the headphones only; USB
   * adapter: the adapter only (nspire_audio.c reads the setting for the
   * adapter: it plays unless Output is Bluetooth). The glue's fix 8
   * stands: each output follows the setting every frame, the adapter never
   * plays under Output Bluetooth, the headphones are paused under Output
   * USB adapter. */
  take = nspire_bt_audio_active() && nspire_audio_output != NSPIRE_OUTPUT_USB;
  (void)usb_streaming;
  want = take && mode && nspire_audio_enabled && nspire_audio_volume ? mode : 0;
  if (take && bt_volume != nspire_audio_volume % (NSPIRE_VOLUME_MAX + 1))
  {
    bt_volume = nspire_audio_volume % (NSPIRE_VOLUME_MAX + 1);
    nspire_bt_audio_set_volume(bt_volume * 256u / NSPIRE_VOLUME_MAX);
  }
  if (nspire_bt_audio_active() || bt_mode > 0)
  {
    /* (each frame while they stream: pause in the menus and the speed
     * test, normal, fast forward, as the USB adapter's; paused too while
     * they don't take the sound: silence from the library's cached frame,
     * no encoding) */
    nspire_bt_audio_frame_begin(want);
    bt_mode = want;
  }
  if (want)
  {
    n_audio_frames++;
    sec_frames++;
  }
  return want != 0;
}

void nspire_bt_glue_audio_write(const int16_t *lr, unsigned frames)
{
  if (started && frames)
    (void)nspire_bt_audio_write(lr, frames);
}

/* bt10: the mixer's frames read in place by the library, one call a frame
 * (nspire_bt.h nspire_bt_audio_write_mix) */
void nspire_bt_glue_audio_write_mix(uint32_t *w1, unsigned n1, uint32_t *w2, unsigned n2, unsigned flags)
{
  if (started && n1 + n2)
  {
    u32 t = nspire_ticks();
    (void)nspire_bt_audio_write_mix(w1, n1, w2, n2, flags);
    sec_write += nspire_ticks() - t;
  }
}

/* The headphones' delay from the game's frame to the ear, ms: the library's
 * ring and what it sends ahead (its targets, steady), and the headphones'
 * own: their A2DP delay report, or, as most earbuds send none (a test
 * pair's "Example Headphones": 0), BT_SINK_DELAY_MS, what SBC earbuds buffer
 * (100-200 ms by the published measurements; a guess either way). 0 while
 * none stream. */
#define BT_SINK_DELAY_MS 150
/* bt10: how late the headphones play the sound written now: what
 * the library's ring and lead measure (ring_ms, ahead_ms), averaged over
 * ~32 frames (the ring wanders a frame either way, the lead a packet), not
 * their targets (after a link stall or a burst the ring sits off its target
 * for a while), and the sink's own buffer (its delay report, or
 * BT_SINK_DELAY_MS). Averaged each frame (nspire_bt_glue_audio_frame) while
 * they stream. */
static uint32_t hp_delay_q8;
static uint16_t hp_sink_ms;
static int hp_delay_have;

static void hp_delay_update(void)
{
  struct nspire_bt_audio_stats st;
  uint32_t v;

  if (!nspire_bt_audio_active())
  {
    hp_delay_have = 0;
    return;
  }
  nspire_bt_audio_stats(&st);
  v = st.ring_ms + st.ahead_ms;
  if (v > 4000)
    v = 4000;
  if (!hp_delay_have)
    hp_delay_q8 = v << 8;
  else
  {
    int32_t d = (int32_t)(v << 8) - (int32_t)hp_delay_q8;
    hp_delay_q8 = (uint32_t)((int32_t)hp_delay_q8 + (d + (d < 0 ? -16 : 16)) / 32);
  }
  hp_sink_ms = (uint16_t)(st.sink_delay_ms ? st.sink_delay_ms : BT_SINK_DELAY_MS);
  hp_delay_have = 1;
}

unsigned nspire_bt_glue_audio_delay_ms(void)
{
  if (!started || !nspire_bt_audio_active() || !hp_delay_have)
    return 0;
  return ((hp_delay_q8 + 128) >> 8) + hp_sink_ms;
}

const char *nspire_bt_glue_headphones_name(void)
{
  int i;
  const struct nspire_bt_device *d = NULL;

  if (!started || !nspire_bt_audio_active())
    return NULL;
  for (i = 0; i < nspire_bt_paired_count(); i++)
    if (nspire_bt_paired(i)->connected && nspire_bt_paired(i)->type == NSPIRE_BT_DEV_HEADPHONES)
      d = nspire_bt_paired(i);
  return d && d->name[0] ? d->name : "headphones";
}

const char *nspire_bt_glue_audio_summary(void)
{
  static char text[80];
  struct nspire_bt_audio_stats st;
  const char *name = nspire_bt_glue_headphones_name();

  if (!name)
    return NULL;
  /* the Sound page's band holds 44 characters: the rate goes first when the
   * name needs the room (bt10: "Bluetooth: Example Headphones, 44.1 kHz
   * stereo" was cut), then the name is cut */
  nspire_bt_audio_stats(&st);
  {
    char suffix[24];
    int room;
    snprintf(suffix, sizeof(suffix), ", %u.%u kHz %s", (unsigned)(st.rate / 1000), (unsigned)(st.rate % 1000 / 100),
             st.channels == 1 ? "mono" : "stereo");
    if (11 + strlen(name) + strlen(suffix) > 44)
      snprintf(suffix, sizeof(suffix), ", %s", st.channels == 1 ? "mono" : "stereo");
    room = 44 - 11 - (int)strlen(suffix);
    snprintf(text, sizeof(text), "Bluetooth: %.*s%s", room, name, suffix);
  }
  return text;
}

/* ---- what is going on ---- */

/* (2026-11-18, the pad glue's look at a hub after its power fell) what the
 * dongle has brought in so far: HCI events and ACL packets. Reads only. */
uint32_t nspire_bt_usb_traffic(void)
{
  bt_port_info_t in;

  if (!started)
    return 0;
  bt_port_info(&in);
  return in.events_in + in.acl_in;
}

int nspire_bt_usb_active(void)
{
  /* (given up: no longer the driver's working device; the port's restart
   * the watchers make is the replug its message asks for) */
  return started && bt_port_usb_active() && bt_port_state() != BT_PORT_FAILED;
}

const char *nspire_bt_glue_status(void)
{
  if (!started && !stopped && !nspire_bt_enabled)
    return NULL;
  return nspire_bt_status();
}

int nspire_bt_glue_log_text(char *out, unsigned size)
{
  bt_port_info_t in;
  int n;

  if (!size)
    return 0;
  out[0] = 0;
  if (!started && !stopped)
    return snprintf(out, size, "bt: %s", nspire_bt_enabled ? "not started" : "off (setting)");
  bt_port_info(&in);
  n = snprintf(out, size,
               "bt: %s; port: %s%s%s\n"
               "bt: dongle %04x:%04x port %u, firmware %u (%lu B), power-ons %lu, HCI events %lu, ACL in %lu out %lu,"
               " commands %lu, USB errors %lu, drops %lu\n"
               "bt: glue polls %lu (%lu in the frames' OS time), windows %lu, OS sleeps %lu, flushes %lu, events %lu, toasts %lu, sound frames %lu;"
               " the dongle a working device of gpSP's: %s\n"
               "bt: watchdog fired %lu, restarts that worked %lu, now %s; live log writes %lu\n"
               "bt: memory: %lu KB held now, peak %lu KB (USB slots, rings, the firmware's copies while uploading);"
               " the last that failed: %lu B (%s)",
               nspire_bt_status(), bt_port_status(), bt_port_problem()[0] ? "; " : "", bt_port_problem(),
               (unsigned)in.vid, (unsigned)in.pid, (unsigned)in.usb_port, (unsigned)in.firmware,
               (unsigned long)in.firmware_bytes, (unsigned long)in.power_ons, (unsigned long)in.events_in,
               (unsigned long)in.acl_in, (unsigned long)in.acl_out, (unsigned long)in.cmds_out,
               (unsigned long)in.usb_errors, (unsigned long)in.drops, (unsigned long)n_polls, (unsigned long)n_polls_frame,
               (unsigned long)n_windows, (unsigned long)n_sleeps, (unsigned long)n_flushes, (unsigned long)n_events,
               (unsigned long)n_toasts, (unsigned long)n_audio_frames, nspire_bt_usb_active() ? "yes" : "no",
               (unsigned long)in.watchdog_fires, (unsigned long)in.recoveries,
               in.hung == 1 ? "restarting the dongle" : in.hung == 2 ? "given up (replug)" : "fine",
               (unsigned long)n_live, (unsigned long)(in.heap_now + 1023) / 1024,
               (unsigned long)(in.heap_peak + 1023) / 1024, (unsigned long)in.mem_fail_bytes,
               in.mem_fail_what == 1 ? "the USB slots at the start" : in.mem_fail_what == 2 ? "the dongle's rings" :
               in.mem_fail_what >= 3 ? "the dongle's firmware" : "none");
  return n < 0 ? 0 : n >= (int)size ? (int)size - 1 : n;
}

/* ---- the exit ---- */

int nspire_bt_glue_stop_within(unsigned ms)
{
  u32 t0;
  int r;

  if (!started)
    return stopped && nspire_bt_stop_pending() ? -1 : 0;
  started = 0;
  stopped = 1;
  t0 = nspire_ticks();
  nspire_os_keypad_keep_off();
  bt_port_log("glue: bt stop begins (within %u ms): each step is in the live log before it runs", ms);
  if (nspire_link_quiet_for_writes())
  {
    uint32_t n0 = n_live;   /* (the exit's: not one of the run's live writes, counted with the step lines) */
    live_write(clock_ms());
    n_live = n0;
    stop_marks_n++;
    stop_marks = 1;
  }
  r = nspire_bt_stop_within(ms);
  /* a USB callback still inside gpSP: waited for within the budget, else
   * the caller restarts the calculator (nothing is unloaded under it) */
  if (r < 0 && nspire_bt_stop_pending())
  {
    unsigned sleeps = 0;   /* (and by sleeps: with the timer stopped its ticks move only in waits) */
    bt_port_stop_step("glue: a USB callback still inside the OS waited for");
    while (nspire_bt_stop_pending() && (u64)(nspire_ticks() - t0) * 1000 < (u64)ms * NSPIRE_TICK_HZ &&
           sleeps++ < ms / 5 + 1)
      if (!nspire_os_sleep())
        break;
    if (nspire_bt_stop_pending())
      bt_port_log("glue: the callback still inside the OS after %lu ms, %u sleeps (the deadline): the restart",
                  (unsigned long)((u64)(nspire_ticks() - t0) * 1000 / NSPIRE_TICK_HZ), sleeps);
  }
  bt_port_log("glue: bt start-up trail lines written this run: %lu", (unsigned long)start_marks_n);
  bt_port_log("glue: bt stop done in %lu ms (%s); %lu step lines written in %lu ms",
              (unsigned long)((u64)(nspire_ticks() - t0) * 1000 / NSPIRE_TICK_HZ),
              r < 0 && nspire_bt_stop_pending() ? "a callback in the OS" : r > 0 ? "memory left to the OS" : "clean",
              (unsigned long)stop_marks_n, (unsigned long)stop_marks_ms);
  live_append(bt_port_log_recent(0));
  stop_marks = 0;
  nspire_os_keypad_keep_off();
  nspire_os_after_os_time();
  nspire_os_want(OS_WANT_BT, 0);
  flags = 0;
  keys = 0;
  nspire_bt_live = 0;
  nspire_bt_wake = &zero_word;
  nspire_padlog_add(PL_BT, PL_BT_STOP, r < 0 ? 2 : (u32)r, 0,
                    (u32)((u64)(nspire_ticks() - t0) * 1000 / NSPIRE_TICK_HZ), 0, 0);
  if (r < 0 && nspire_bt_stop_pending())
    return -1;
  return 0;
}

/* gpsp_bt.txt.tns: the port's log (its first lines kept, then the latest),
 * the library's and the port's counters. After the USB shutdowns. */
void nspire_bt_glue_write_log(void)
{
  char path[600], buf[2048];
  FILE *f;
  bt_port_info_t in;

  if (!started && !stopped)
    return;
  live_remove();   /* (the run ends cleanly: the full log below says it all) */
  /* (only when there is something to read: a dongle was seen, or Bluetooth
   * has a problem; the pad log's Bluetooth lines say the rest) */
  bt_port_info(&in);
  if (!in.dongles_seen && !bt_port_problem()[0] && nspire_bt_state() != NSPIRE_BT_FAILED)
    return;
  nspire_save_file(path, sizeof(path), "gpsp_bt.txt.tns");
  f = fopen(path, "w");
  if (!f)
    return;
  /* (no heap taken: a memory shortage is what this log may have to show) */
  nspire_bt_glue_log_text(buf, sizeof(buf));
  fprintf(f, "gpSP Bluetooth log\n%s\n\n", buf);
  bt_port_log_fwrite(f);
  if (beat_line[0])
    fprintf(f, "%s (the glue's last heartbeat)\n", beat_line);
  fputs("\n-- the library --\n", f);
  nspire_bt_diag(buf, sizeof(buf));
  fputs(buf, f);
  fputs("\n-- the port --\n", f);
  bt_port_diag(buf, sizeof(buf));
  fputs(buf, f);
  fputc('\n', f);
  audio_seconds_write(f);
  fclose(f);
}

#endif
