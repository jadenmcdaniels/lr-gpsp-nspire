/* GBA link between two gpSPs (nspire_link.h).
 *
 * The two run frame for frame: a side starts its frame f only once it has
 * the other side's frame f - LINK_DELAY, and that frame's packets reach the
 * core then, not as they arrive. So both games see the same thing whichever
 * runs faster, a side in its menu holds the other one (instead of the game
 * saying "communication error"), and the PC tests give the same result
 * every run. Each frame is one message each way.
 *
 * With the link cable (SERIAL_MODE_CABLE) it's the GBA's own cable: every
 * transfer swaps the two GBAs' registers at the same cycle, as the wires
 * do, so any game's link works. Player 1 (the parent, the one that starts
 * transfers) runs ahead; player 2 never runs past player 1's time (its
 * "horizon": no transfer starts before it). Player 1 tells it at its frame
 * ends, its transfers, every CABLE_TELL cycles between while the game uses
 * the cable, and when its CPU halts, as far as it surely stays halted
 * (halted_cycles: no transfer can start meanwhile), so player 2 runs
 * alongside it instead of after it. When player 1's game starts a transfer
 * at cycle T, it sends T and its word and runs on; player 2 runs to T and
 * answers with its word, and may run on to the transfer's end (no other
 * transfer can start before). Both finish the transfer (words in,
 * interrupt) at T + the transfer's time: player 1 waits there for player
 * 2's word if it hasn't come (nothing reads it sooner: serial.c lands both
 * words then). And when player 2's CPU halts, its word can't change until
 * it wakes, so it promises it for player 1's next transfer if that starts
 * before then, and doesn't answer that one: player 1 has its word before
 * it asks. Player 2 takes player 1's messages only at its horizon, one at
 * a time, so it stops at the same cycles at any link speed. Player 1 waits
 * for player 2's frame f - CABLE_LEAD before its frame f (player 2's
 * answers carry its frame too), so they stay within a few frames.
 *
 * Still, player 2's word for a transfer can depend on player 1's last one,
 * and some games make hundreds a second, so the cable as wired goes only
 * as fast as the USB answers: a transfer costs player 1 the round trip less
 * its time since the last one ended (tests/link's matrix). For
 * games gpSP knows (the stand-ins table below) the cable is instead one of
 * libretro's stand-ins (serial_proto.c), serial_fe.c's or serial_msio.c's:
 * each GBA plays the other's link itself and the two swap what was said
 * once a frame, as the wireless adapter does, which a slow cable doesn't
 * hold up.
 *
 * Messages: an 8-byte head (type, flags, length of what follows, frame)
 * and what follows: for frames, the core's packets (wireless adapter), each
 * a 2-byte length and its bytes, or the sender's cycle count (cable). */

#include <os.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "nspire_frontend.h"
#include "nspire_link.h"
#include "nspire_paths.h"
#include "nspire_os.h"
#include "nspire_usbport.h"
#include "nspire_arbiter.h"
#include "nspire_usbpad_glue.h"   /* (2026-11-16) what the pad glue knows of the port's device */
/* (weak: a build or a test without the pad glue has neither) */
int nspire_usbpad_other_device(void) __attribute__((weak));
int nspire_usbpad_os_stuck(void) __attribute__((weak));
#include "nspire_padlog.h"
#include "nspire_audio.h"

#define LINK_MAGIC    0x4b4c5047   /* "GPLK" */
/* The hello's version field: 1 in every build, forever (the hello's
 * comment below). The protocol spoken is the extension's. */
#define LINK_VERSION  1
/* The newest protocol this build speaks; it speaks every older one too.
 * 1: e24876d's (the frame lockstep, the cable's XFER and REPLY, every
 * transfer answered). 2: the hello's extension, the cable's PROMISE (no
 * REPLY for a promised transfer) and player 1 telling player 2 it may run
 * to a transfer's end. */
#define LINK_PROTOCOL 3
/* Frames between a side's frame and the other's that it waits for: room
 * for the USB's own delay without either side ever waiting. 2 with an older
 * gpSP (protocol 2 or 1); from protocol 3 both measure the round trip as
 * they link (ping_phase) and use the frames the slower one needs, up to
 * LINK_DELAY_MAX (nspire_link.h; link_delay). Two calculators' NavNet took 55 to 530 ms
 * for a round trip (2026-11-06), and with 2 frames the games ran at 8 a
 * second; the PC's model runs Emerald's trade at 18 a second at 100 ms one
 * way with 2 frames. */
#define LINK_DELAY    2
/* On the cable, how many frames player 1 waits for instead: only so it
 * doesn't run far ahead, as the cable is exact whatever it is. Player 2's
 * frame end then has two frames to come back (a round trip of up to 33 ms)
 * before player 1 waits for it when the game isn't using the cable. */
#define CABLE_LEAD    3

#define MSG_HELLO     1
#define MSG_FRAME     2   /* packets of one frame; flags FRAME_END: it's over */
#define MSG_MENU      3   /* flags 1: in the menu, 0: back */
#define MSG_BYE       4
#define MSG_XFER      5   /* cable: player 1 starts a transfer (cycle, time, word) */
#define MSG_REPLY     6   /* cable: player 2's word for it */
#define MSG_PROMISE   7   /* cable: player 2's word for the next if it starts
                             before a cycle (from, until, word) */
#define MSG_HELLO_EXT 8   /* the hello with its extension (protocol 2 on) */
#define MSG_PING      9   /* protocol 2: the link's round trip at its start (ping_phase) */
#define MSG_PONG      10  /* the ping's answer: its body back */
#define MSG_DELAY     11  /* protocol 3: the frames of delay this side needs (u16) */

/* The hello's extension's fields (tag, length, value; the hello's comment). */
#define EXT_PROTOCOL  1   /* u16: the newest protocol the sender speaks */
#define EXT_LINK      2   /* u8 flags (EXT_*), u8 its Link setting (LINK_DEVICE_*),
                             u16 the cable it plays (a stand-in's serial_mode,
                             or SERIAL_MODE_CABLE: as wired) */
#define EXT_FAST      3   /* u32: the fast transport this side can be (link_cable_fast_offer) */
#define EXT_WANTS_ADAPTER 1   /* Wireless, or Auto with a game that has the adapter */
#define EXT_EXACT         2   /* Link cable (exact) */
#define EXT_ADAPTER_GAME  4   /* gba_over.h gives the game the adapter */
/* Hellos that tie (the same nonce) before giving up. */
#define HELLO_TRIES   16

#define FRAME_END     1

/* The link cable: while its CPU runs, player 1 tells player 2 its time
 * when it has run this long (cycles) since it last did, by a frame end,
 * transfer, halt or this; each is a message (0.1 ms on a CX II, 0.3 on a
 * CX: tests/link's matrix chose an eighth of a frame). Only
 * while the game is using the cable, up to CABLE_QUIET cycles after its
 * last transfer started: else player 2 keeps up in the time player 1 has
 * to spare each frame, from its frame ends. A halt is told when it goes at
 * least CABLE_AHEAD cycles further (a 64th of a frame: as fast as every
 * one on tests/host/rom/link.s, with fewer messages). */
#define CABLE_TELL    (280896 / 8)
#define CABLE_QUIET   (2 * 280896)
#define CABLE_AHEAD   (280896 / 64)

#define HEAD          8
#define MSG_MAX       LINK_MSG_MAX
/* Packets that came before the frame they're for: up to 2 x link_delay
 * frames of the other side's, each frame's message at most MSG_MAX and its
 * packets' heads here 6 bytes (2 in the message): room for twice that at
 * the most delay (LINK_DELAY_MAX: 15). */
#define QUEUE_BYTES   (2 * 2 * LINK_DELAY_MAX * MSG_MAX)

/* Waiting for the other side: a message after this long, unplugged after
 * the second (unless it's in its menu, which can take as long as it likes). */
#define WAIT_SLICE_MS     20
/* R4: 2 s (400 ms before): the calculators' round trips are 100-850 ms
 * and the status flashed on each; a wait of 2 s is a stall, not frames
 * coming slowly. */
#define WAIT_SHOW_MS      LINK_WAIT_SHOW_MS
#define WAIT_GIVE_UP_MS   15000
/* LNK-1: the other in its menu holds a wait this long at most (a side gone
 * with its menu open, never saying so, can't hold this one for ever); the
 * give-up counts from the last MSG_MENU, so 15 s after the other leaves it. */
#define MENU_GIVE_UP_MS   (5 * 60 * 1000)

/* The round trip, measured as two protocol-2 sides link (ping_phase): each
 * pings the other PINGS times, every other ping PING_BIG bytes (a frame
 * with packets) and the rest a bare head, and answers the other's. */
#define PINGS         32
#define PING_BIG      136

uint32_t nspire_link_device = LINK_DEVICE_AUTO;

static enum link_state state = LINK_OFF;
/* DETECT: a plug-in's come-up (bring_*, by usb_timeline_frame). */
static void bring_start(u32 bits, int port_change);
static void bring_stop(void);
static void bring_end(const char *how);
static int bring_running(void);
static u32 my_nonce;
static int hello_sent, hello_tries;
static u32 other_fast;   /* the other side's hello's EXT_FAST */
static char status[128] = "Not linked";
char nspire_link_help[200];
/* The protocol both speak (the hello's), while linked. */
static int protocol = 1;
/* The frames of delay both use (LINK_DELAY), and the other side's ask. */
static u32 link_delay = LINK_DELAY;
/* The start's delay agreed (delay_phase): a MSG_DELAY after it can only
 * raise it (after the fast transport fell back, remeasure). */
static int delay_agreed;
/* After a fall back of the fast transport (remeasure_start, below). */
#define REMEASURE_SEQ   0x10000u
static int remeasuring, remeasure_got;
static void remeasure_pong(void);
static int other_delay;   /* -1 until its MSG_DELAY came */
/* The game's own serial_mode (gba_over.h), before the setting's. */
static int game_mode = SERIAL_MODE_AUTO;

static u32 my_frame;          /* frames run since linking */
static s32 other_done;        /* the other side's last whole frame */
static int other_in_menu;
static unsigned menu_msgs;   /* MSG_MENUs taken: a wait's give-up restarts at one */

static u8 out_msg[MSG_MAX];
static u32 out_len = HEAD;

/* The other side's packets not yet handed to the core: each is its frame
 * (4 bytes), length (2) and bytes, one after another from queue_head. */
static u8 queue[QUEUE_BYTES];
static u32 queue_head, queue_tail;

static u8 in_msg[MSG_MAX];

/* The link cable. Cycles run since linking (both start at 0 at the frame
 * they linked), how far player 2 may run, the transfer player 2 has to give
 * its word for, and the words a transfer ends with. */
static u64 cable_clock;
static u64 horizon;
static int xfer_pending;
static u64 xfer_at;
static u32 xfer_time, xfer_word;
static int reply_got;
static u32 reply_word;
static u16 words[2];
/* Player 1's: how far player 2 may run from what it was sent, till when
 * it's told more often (CABLE_QUIET after a transfer), a transfer (started
 * at reply_for) whose word hasn't been taken, and player 2's last promise
 * not used yet: its word for a transfer from promise_at (the first after)
 * if it starts before promise_until. */
static u64 told, tell_till;
static int reply_owed;
static u64 reply_for;
static u32 xfer_sent_at;   /* when (ticks) the last transfer's start went, for the log */
static int have_promise;
static u64 promise_at, promise_until;
static u32 promise_word;
/* Player 2's: its last promise, until a transfer uses it, and the
 * transfer (from count_at) to count at its end, as player 1 does. */
static int promised;
static u64 promised_until;
static u32 promised_word;
static int count_owed;
static u64 count_at;
/* The CPU's halt this side has looked at (cable_next_event), till it wakes. */
static int halt_seen;
/* Cable transfers since starting, and their sum (nspire_link.h). */
uint32_t nspire_link_transfers;
uint32_t nspire_link_transfer_sum;

static void put16(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; }
static void put32(u8 *p, u32 v) { put16(p, v); put16(p + 2, v >> 16); }
static u32 get16(const u8 *p) { return p[0] | (p[1] << 8); }
static u32 get32(const u8 *p) { return get16(p) | (get16(p + 2) << 16); }
static void put64(u8 *p, u64 v) { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); }
static u64 get64(const u8 *p) { return get32(p) | ((u64)get32(p + 4) << 32); }

/* ---- The log (nspire_link.h) ----
 *
 * On the calculators nothing else shows what the cable did, so the link
 * keeps a log, written to gpsp_link.txt.tns in the saves folder: the
 * transport's calls and their results (nspire_link_usb.c), the hellos and
 * what was chosen, the round trip measured at the start (ping_phase), and
 * at each link's end the messages each way and how long the waits for the
 * other side took. Kept in memory and written whole (appending has garbled
 * files on the calculator): when linking starts and links (between frames:
 * nspire_link_frame_begin), at the next frame after a link ends (end_link
 * can come from inside the core, which mustn't write files), when gpSP
 * leaves the game and when it closes. Times are ms since Link was picked
 * (or, before any link, since gpSP started).
 *
 * The heartbeat (every HEARTBEAT_FRAMES linked frames) goes in a ring of
 * its own, the oldest lines dropped for new ones, written after the rest:
 * so a long link's last minute before a crash is there however long it
 * ran. It's written every HEARTBEAT_WRITE heartbeats or when the USB
 * port's or the screen's state changed (a write takes the game's time,
 * which the other side then waits out), and at the link's end it joins the
 * rest. The first write of a run keeps the last run's file as
 * gpsp_link_prev.txt.tns: a crash's log isn't lost to the next start. */
#define LOG_BYTES     12288
#define LOG_LINE      520
#define BEAT_BYTES    12288
static char log_text[LOG_BYTES];
static unsigned log_len, log_lost;
static int log_new, log_flush_wanted, log_written;
static u32 log_t0;
static char beat_text[BEAT_BYTES];
static unsigned beat_len, beats_dropped;

static unsigned ticks_us(u32 ticks)
{
  return (unsigned)((u64)ticks * 1000000 / NSPIRE_TICK_HZ);
}

/* A line, with its time (at: nspire_ticks' value), into a buffer: 0, or -1
 * if it didn't fit. */
static int log_into_at(u32 at, char *text, unsigned *len, unsigned bytes, const char *fmt, va_list ap)
{
  char line[LOG_LINE];
  unsigned n;

  n = (unsigned)snprintf(line, sizeof(line), "%6u ", ticks_us(at - log_t0) / 1000);
  vsnprintf(line + n, sizeof(line) - n, fmt, ap);
  n = strlen(line);
  if (*len + n + 1 > bytes)
    return -1;
  memcpy(text + *len, line, n);
  text[*len + n] = '\n';
  *len += n + 1;
  log_new = 1;
  return 0;
}

static int log_into(char *text, unsigned *len, unsigned bytes, const char *fmt, va_list ap)
{
  return log_into_at(nspire_ticks(), text, len, bytes, fmt, ap);
}

/* The log's first lines, put in whenever the log is empty (a run's first
 * event, or the one after the log was cleared for a new link): which build
 * ran, with which driver, on what calculator, whether the OS can sleep for
 * us, and the settings. Written with the first event, not at start: a game
 * played with nothing on the port leaves the last log alone. */
static int pad_os_time = -1;   /* the gamepad glue's os_time; -1 unknown */

void nspire_link_log_pad_setting(int os_time)
{
  pad_os_time = os_time;
}

#if __has_include("nspire_build_id.h")
#include "nspire_build_id.h"
#else
#define NSPIRE_BUILD_GIT    "unknown"
#define NSPIRE_BUILD_DATE   "unknown"
#define NSPIRE_BUILD_PADS   0
#define NSPIRE_BUILD_DRIVER "unknown"
#endif

static void log_raw(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_raw(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  /* Full: the rest is counted (the file says how many lines were lost). */
  if (log_into(log_text, &log_len, LOG_BYTES - 64, fmt, ap) < 0)
    log_lost++;
  va_end(ap);
}

static int clock_logged;   /* R11t: the clock select's line is in this log */

/* R11t: the clock select's line (in text already); R12: a CX II's time base
 * lines after it (its mode and why, the timer blocks as found, the speeds) */
static void log_timer_lines(char *text, unsigned size)
{
  int i;

  log_raw("%s", text);
  for (i = 1; nspire_timer_header_text(i, text, size); i++)
    log_raw("%s", text);
}

static void log_header(void)
{
  char text[LOG_LINE - 120];

  nspire_os_diag_system(text, sizeof(text));
  log_raw("gpSP build %s %s; gamepads: %s; %s", NSPIRE_BUILD_GIT, NSPIRE_BUILD_DATE,
          NSPIRE_BUILD_PADS ? "built, driver " NSPIRE_BUILD_DRIVER : "not built", text);
  nspire_os_report(text, sizeof(text));
  log_raw("%s", text);
  log_raw("settings: Link mode %u (%s), os_time %s", (unsigned)(nspire_link_device % LINK_DEVICES),
          nspire_link_device_name(), pad_os_time < 0 ? "n/a" : pad_os_time ? "1" : "0");
  /* R11t: what the OS had in the timer block's shared clock select (a log
   * started before nspire_init has it at its first line after: log_clock) */
  if ((clock_logged = nspire_timer_clock_text(text, sizeof(text))))
    log_timer_lines(text, sizeof(text));
}

static void log_clock(void)
{
  char text[LOG_LINE - 120];

  if (!clock_logged && (clock_logged = nspire_timer_clock_text(text, sizeof(text))))
    log_timer_lines(text, sizeof(text));
}

/* What was shown on screen (nspire_osd_message), as lines in the log
 * with their times. A toast before the log has started (nothing on the port
 * yet) waits in a few slots: they go in behind the header when the log
 * starts, and a run that never has an event still writes no file. */
#define TOASTS_KEPT 6
static struct { u32 at; char text[64]; } toasts[TOASTS_KEPT];
static unsigned toasts_n;   /* ever kept; the slot is n % TOASTS_KEPT */
static char last_status[64];
#define TOASTS_LOGGED_MAX 20
static unsigned toasts_logged;

static void log_toasts_kept(void)
{
  unsigned i, first = toasts_n > TOASTS_KEPT ? toasts_n - TOASTS_KEPT : 0;

  for (i = first; i < toasts_n; i++)
  {
    char line[LOG_LINE];
    unsigned n = (unsigned)snprintf(line, sizeof(line), "%6u toast (before the log): %s",
                                    ticks_us(toasts[i % TOASTS_KEPT].at - log_t0) / 1000,
                                    toasts[i % TOASTS_KEPT].text);

    if (log_len + n + 1 > LOG_BYTES - 64)
      break;
    memcpy(log_text + log_len, line, n);
    log_text[log_len + n] = '\n';
    log_len += n + 1;
    log_new = 1;
  }
  toasts_n = 0;
}

/* R2 (the CX II crashed 78 s into a link and its log stopped at the last
 * heartbeat: the flush policy keeps file writes off the linked frames, so
 * the last seconds were only in memory). A small ring of the latest events
 * (every log line, a heartbeat line, and once a second a line on the frame
 * and the other side's), written to gpsp_link_last.txt.tns every
 * LAST_FLUSH_MS while linked, at a frame start where the other side has
 * given at least LAST_SLACK frames more than this one needs (so a write,
 * ~1 ms for 3 KB, can't hold anyone up), and a marker file
 * (gpsp_link_run.txt.tns: "running" from a run's first log write, "clean"
 * once gpSP quit) so the next run's log says it didn't end cleanly and
 * shows that ring. */
/* R4: 6 KB (3 KB in R2-R3): the game's serial writes go in too. 10 KB after
 * merging final/ff and final/pic (2026-11-13): the second's line grew (drawn
 * frames, idle sleeps) and the picture log line joined it, so 6 KB rolled the
 * link's first lines out within 23 s (test_unclean_exit). */
#define RECENT_BYTES   10240
#define LAST_FLUSH_MS  4000
#define LAST_SLACK     3
/* R3: the CX II died 3.3 to 3.7 s after its ring's last write (its last
 * line at 49325 ms; the CX CAS saw the cable break at most 53047 ms by the
 * CX II's clock, by the two hellos' times; the next write was due at 53.3
 * s): on a CX II, written each second while linked (3 KB, ~1.5 ms at a
 * frame start with slack: 0.15% of its time). */
#define LAST_FLUSH_CX2_MS 1000
static unsigned last_flush_ms(void)
{
  return is_cx2 ? LAST_FLUSH_CX2_MS : LAST_FLUSH_MS;
}
static char recent_text[RECENT_BYTES];
static unsigned recent_len;
static u32 last_flushed_at, recent_second_at;

static void sio_session(const char *why);
/* R4: the last state loaded, said again in the log when a link starts (the
 * log isn't started by a game alone: an unlinked run leaves the last one). */
static char state_note[200];
static void recent_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void recent_add(const char *fmt, ...)
{
  char line[LOG_LINE];
  unsigned n;
  va_list ap;

  n = (unsigned)snprintf(line, sizeof(line), "%6u ", ticks_us(nspire_ticks() - log_t0) / 1000);
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof(line) - n, fmt, ap);
  va_end(ap);
  n = (unsigned)strlen(line);
  if (n + 1 > RECENT_BYTES)
    return;
  while (recent_len + n + 1 > RECENT_BYTES)
  {
    char *end = memchr(recent_text, '\n', recent_len);
    unsigned cut = end ? (unsigned)(end + 1 - recent_text) : recent_len;
    recent_len -= cut;
    memmove(recent_text, recent_text + cut, recent_len);
  }
  memcpy(recent_text + recent_len, line, n);
  recent_text[recent_len + n] = '\n';
  recent_len += n + 1;
}

/* One small file, whole (appending has garbled files on the calculator). */
static void write_small(const char *name, const char *text, unsigned len)
{
  char path[600];
  FILE *f;

  nspire_save_file(path, sizeof(path), name);
  f = fopen(path, "w");
  if (!f)
    return;
  fwrite(text, 1, len, f);
  fclose(f);
}

static int last_written;   /* this run wrote the ring (check_last_run leaves it then) */

static void last_flush(void)
{
  last_flushed_at = nspire_ticks();
  if (recent_len)
  {
    write_small("gpsp_link_last.txt.tns", recent_text, recent_len);
    last_written = 1;
  }
}

/* EXIT-R4: the clock in this run's clean marker (nspire_link_exit_close),
 * repeated in the file written after the hand-back. */
static unsigned exit_clock;
#define EXIT_AFTER_FILE "gpsp_exit_after.txt.tns"

static void marker_write(int clean)
{
  char text[100];

  snprintf(text, sizeof(text), "%s at clock %u s, build %s\n", clean ? "clean" : "running",
           (unsigned)nspire_rtc_seconds(), NSPIRE_BUILD_GIT);
  write_small("gpsp_link_run.txt.tns", text, (unsigned)strlen(text));
}

/* Whether this run has a log (lines, or a file written): a first line would
 * start one (and rotate the older logs when it's written). */
int nspire_link_log_started(void)
{
  return log_written || log_new;
}

void nspire_link_log(const char *fmt, ...)
{
  va_list ap;

  if (!log_len)
  {
    log_header();
    log_toasts_kept();
  }
  else
    log_clock();
  va_start(ap, fmt);
  /* Full: the rest is counted (the file says how many lines were lost). */
  if (log_into(log_text, &log_len, LOG_BYTES - 64, fmt, ap) < 0)
  {
    /* R5: and the line still goes into the ring (R4's CX CAS: a full log
     * put the last line it had there again, blank, instead of the new one). */
    char line[LOG_LINE];
    va_end(ap);
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log_lost++;
    recent_add("%s", line);
    return;
  }
  va_end(ap);
  {
    /* The last line, again, into the ring of recent events. */
    char *start = log_text + log_len;
    if (log_len > 1)
    {
      start--;
      while (start > log_text && start[-1] != '\n')
        start--;
      /* (R5: the length after the time's 7 characters: it was 7 more, so
       * each copy took the line's end and what lay past it: a blank line.) */
      int n = (int)(log_text + log_len - 1 - start) - 7;
      recent_add("%.*s", n > 0 ? n : 0, start + 7);
    }
  }
}

/* nspire_video_present.c's nspire_osd_message calls this with each toast.
 * The link's own status lines (set_status) are logged as "status" and their
 * toast twin is skipped. */
void nspire_link_log_toast(const char *text)
{
  if (!strcmp(text, last_status))
  {
    last_status[0] = 0;
    return;
  }
  if (log_len)
  {
    /* Few and early: the log's room is for what's diagnosed (a long
     * session's toasts would fill it and the newest lines be the lost). */
    if (toasts_logged < TOASTS_LOGGED_MAX && log_len < LOG_BYTES / 2)
    {
      toasts_logged++;
      nspire_link_log("toast: %s", text);
    }
    return;
  }
  toasts[toasts_n % TOASTS_KEPT].at = nspire_ticks();
  snprintf(toasts[toasts_n % TOASTS_KEPT].text, sizeof(toasts[0].text), "%s", text);
  toasts_n++;
}

/* A heartbeat line: into the ring, its oldest lines out to make room. */
static void beat_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void beat_log(const char *fmt, ...)
{
  va_list ap;

  for (;;)
  {
    char *end;
    int fitted;
    va_start(ap, fmt);
    fitted = log_into(beat_text, &beat_len, BEAT_BYTES, fmt, ap);
    va_end(ap);
    if (fitted == 0 || !beat_len)
      return;
    end = memchr(beat_text, '\n', beat_len);
    if (!end)
    {
      beat_len = 0;
      continue;
    }
    beat_len -= (unsigned)(end + 1 - beat_text);
    memmove(beat_text, end + 1, beat_len);
    beats_dropped++;
  }
}

/* The heartbeat's lines into the log (the link's over): after it, as
 * many as fit. */
static void beats_into_log(void)
{
  if (beats_dropped)
    nspire_link_log("(%u older heartbeat lines dropped)", beats_dropped);
  if (beat_len && log_len + beat_len <= LOG_BYTES - 64)
  {
    memcpy(log_text + log_len, beat_text, beat_len);
    log_len += beat_len;
  }
  else if (beat_len)
    log_lost++;
  beat_len = beats_dropped = 0;
}

void nspire_link_log_request_flush(void)
{
  log_flush_wanted = 1;
}

/* The kept copy of the log of the last link that came up (gpsp_link_linked
 * .txt.tns): written when the link comes up, at its end and at exit, and
 * rotated only by the next link, so a later run that never links can't
 * take it. (2026-11-10, the CX CAS's log of the linked session was gone:
 * each run's first write rotates the file to _prev, and the next run's
 * rotation put a run without it over the one before.) */
static int keep_linked;
static unsigned rotation_failed;
static char rotation_note[120];

/* The old logs go one place down (prev -> prev2 -> prev3), each step
 * checked: a rename that fails (the calculator's file system) is copied
 * instead and said in the new log. */
static void copy_file(const char *from, const char *to)
{
  FILE *a = fopen(from, "rb"), *b;
  char buf[512];
  size_t n;

  if (!a)
    return;
  b = fopen(to, "wb");
  if (b)
  {
    while ((n = fread(buf, 1, sizeof(buf), a)) > 0)
      fwrite(buf, 1, n, b);
    fclose(b);
  }
  fclose(a);
}

static void move_file(const char *from, const char *to)
{
  FILE *f = fopen(from, "rb");

  if (!f)
    return;
  fclose(f);
  remove(to);
  if (rename(from, to) != 0)
  {
    /* Not renamed: copied, then the old name removed. */
    copy_file(from, to);
    remove(from);
    rotation_failed++;
  }
}

static void rotate_logs(void)
{
  char path[600], p1[600], p2[600], p3[600];

  nspire_save_file(path, sizeof(path), "gpsp_link.txt.tns");
  nspire_save_file(p1, sizeof(p1), "gpsp_link_prev.txt.tns");
  nspire_save_file(p2, sizeof(p2), "gpsp_link_prev2.txt.tns");
  nspire_save_file(p3, sizeof(p3), "gpsp_link_prev3.txt.tns");
  move_file(p2, p3);
  move_file(p1, p2);
  move_file(path, p1);
}

/* EXIT-R4: a clean marker that said a file comes after the hand-back:
 * that file, if it is the same run's (the marker's clock in it), or that
 * it never came: the main task stopped after the marker, in the hand-back
 * or in the OS's file call (keys dead after quitting, on both calculators
 * in SEAM1). */
static void check_last_exit(const char *marker)
{
  char path[600], after[2000], want[60];
  unsigned clock = (unsigned)strtoul(marker + 15, NULL, 10);
  FILE *f;
  size_t n = 0;
  char *nl;

  if (!strstr(marker, "after the hand-back: " EXIT_AFTER_FILE " follows"))
    return;
  nspire_save_file(path, sizeof(path), EXIT_AFTER_FILE);
  f = fopen(path, "rb");
  if (f)
  {
    n = fread(after, 1, sizeof(after) - 1, f);
    fclose(f);
  }
  after[n] = 0;
  snprintf(want, sizeof(want), "marked clean at clock %u s,", clock);
  nl = strchr(after, '\n');
  if (!n || !strstr(after, want) || !nl)
  {
    nspire_link_log("the last run's exit: its marker (clean at clock %u s) said %s follows, and %s: its main task "
                    "stopped after the marker (in the hand-back or the OS's file call)", clock, EXIT_AFTER_FILE,
                    n ? "it is an older run's" : "it is missing");
    return;
  }
  while (*nl == '\n')
    nl++;
  n = strlen(nl);
  while (n && (nl[n - 1] == '\n' || nl[n - 1] == '\r'))
    nl[--n] = 0;
  /* R11t: the hand-back's words, then (a line of its own) the timer's clock
   * select after the put-back */
  {
    char *more = strchr(nl, '\n');

    if (more)
      *more++ = 0;
    nspire_link_log("the last run's exit (clean at clock %u s): %s", clock, nl);
    /* QUIT6: and the interrupt lines', each a line of its own */
    while (more && *more)
    {
      char *next = strchr(more, '\n');
      if (next)
        *next++ = 0;
      if (*more)
        nspire_link_log("the last run's exit, after the hand-back: %s", more);
      more = next;
    }
  }
}

/* The last run's marker and last events, before this run's first write:
 * "running" means gpSP never got to say "clean". */
static void check_last_run_report(void);
static void check_last_run(void)
{
  char ring[600];

  check_last_run_report();
  /* bt10: the ring is written only while linked, so one left on the
   * calculator is the last LINKED run's, however many runs ago: an r16
   * hardware report printed a 2026-11-11 ring (byte for byte) as its frozen
   * run's "last events". Read once here, then gone: a later crash shows a
   * ring only if that run linked. */
  if (last_written)
    return;
  nspire_save_file(ring, sizeof(ring), "gpsp_link_last.txt.tns");
  remove(ring);
}

static void check_last_run_report(void)
{
  /* EXIT-R4: the clean marker (its exit lines: ~1.5 KB; with 2026-11-11's
   * picture lines ~4.5 KB). Only "after the hand-back" is looked for, and it
   * comes before the picture lines, so a longer marker is read far enough. */
  static char text[5000];
  static char last[RECENT_BYTES + 1];   /* static: 10 KB is too much stack, and this runs once */
  char path[600];
  FILE *f;
  size_t n;

  nspire_save_file(path, sizeof(path), "gpsp_link_run.txt.tns");
  f = fopen(path, "rb");
  if (!f)
    return;
  n = fread(text, 1, sizeof(text) - 1, f);
  fclose(f);
  text[n] = 0;
  if (!strncmp(text, "clean at clock ", 15))
  {
    check_last_exit(text);
    return;
  }
  if (strncmp(text, "running", 7))
    return;
  while (n && (text[n - 1] == '\n' || text[n - 1] == '\r'))
    text[--n] = 0;
  nspire_link_log("the last run did not exit cleanly (its marker: %s); its last events follow", text);
  nspire_save_file(path, sizeof(path), "gpsp_link_last.txt.tns");
  f = fopen(path, "rb");
  if (!f)
  {
    nspire_link_log("  (no gpsp_link_last.txt.tns: it ended before the first write of that ring)");
    return;
  }
  n = fread(last, 1, RECENT_BYTES, f);
  fclose(f);
  last[n] = 0;
  {
    /* The ring's last lines (the oldest ones are older than the crash's seconds). */
    char *line = last, *end, *from = last + n;
    unsigned kept = 0;

    while (from > last && kept < 30)
    {
      from--;
      if (from > last && from[-1] == '\n')
        kept++;
    }
    line = from;
    while (*line)
    {
      end = strchr(line, '\n');
      if (end)
        *end = 0;
      if (*line)
        nspire_link_log("  last run: %s", line);
      if (!end)
        break;
      line = end + 1;
    }
  }
}

static unsigned audio_held_flushes;   /* R11t: for the host tests */
unsigned nspire_link_log_audio_held(void) { return audio_held_flushes; }

void nspire_link_log_flush(void)
{
  char path[600];
  FILE *f;

  /* R11t: no file write while a sound adapter plays (a write stalls the USB
   * callbacks 100-290 ms, ~48 ms of sound is queued): the lines stay in
   * memory and the next flush point after the adapter is gone writes them
   * (the exit's comes after the audio's shutdown). */
  if (log_new && nspire_usbaudio_active())
  {
    log_flush_wanted = 1;
    audio_held_flushes++;
    return;
  }
  log_flush_wanted = 0;
  if (!log_new)
    return;
  nspire_save_file(path, sizeof(path), "gpsp_link.txt.tns");
  if (!log_written)
  {
    log_written = 1;
    check_last_run();
    nspire_link_speed_check();   /* the fast transport off after a crash? */
    rotate_logs();
    if (rotation_failed)
    {
      snprintf(rotation_note, sizeof(rotation_note), "(%u log file renames failed this run: copied instead)",
               rotation_failed);
      nspire_link_log("%s", rotation_note);
    }
    marker_write(0);
  }
  f = fopen(path, "w");
  if (!f)
    return;
  fwrite(log_text, 1, log_len, f);
  if (log_lost)
    fprintf(f, "(%u more lines didn't fit)\n", log_lost);
  if (beats_dropped)
    fprintf(f, "(%u older heartbeat lines dropped)\n", beats_dropped);
  fwrite(beat_text, 1, beat_len, f);
  fclose(f);
  log_new = 0;
  if (keep_linked)
  {
    char copy[600];
    keep_linked = 0;
    nspire_save_file(copy, sizeof(copy), "gpsp_link_linked.txt.tns");
    copy_file(path, copy);
  }
}

static void log_link_end(const char *why);

void nspire_link_log_close(void)
{
  if (state == LINK_LINKED)
    log_link_end("gpSP closed while linked");
  nspire_link_fast_end();   /* a clean exit: no crash in the fast transport */
  nspire_link_log_flush();
  if (log_written)
    marker_write(1);   /* (the PC's quit: the calculator's is nspire_link_shutdown) */
}

/* A new link's lines: the log keeps the last links that fit, from the
 * start of one. DETECT: and when that's too much, the last LOG_KEPT_TAIL
 * bytes before this link (whole lines) rather than nothing: they are the
 * plug-in's come-up (the port's changes, the owners, the looks), which the
 * link's own lines time against. */
#define LOG_KEPT_TAIL (LOG_BYTES / 4)
static void log_begin(void)
{
  if (log_len > LOG_BYTES / 2 || log_lost)
  {
    unsigned from = log_len > LOG_KEPT_TAIL ? log_len - LOG_KEPT_TAIL : 0;
    char *nl;

    while (from > 0 && from < log_len && log_text[from - 1] != '\n')
      from++;
    nl = from < log_len ? log_text + from : NULL;
    if (nl && from > 0)
    {
      static const char cut[] = "(older lines dropped: the log keeps the lines just before a link)\n";
      unsigned keep = log_len - from;
      memmove(log_text + sizeof(cut) - 1, nl, keep);
      memcpy(log_text, cut, sizeof(cut) - 1);
      log_len = (unsigned)(sizeof(cut) - 1) + keep;
    }
    else if (!nl)
      log_len = 0;
    log_lost = 0;
  }
  beat_len = beats_dropped = 0;
  log_t0 = nspire_ticks();
}

/* Waits for the other side, by kind: how many, their total and longest, and
 * a histogram (up to 1/4 ms, 1/2, 1, 2, ... 64 ms, longer). */
#define WAIT_BUCKETS  10
struct waits
{
  const char *name;
  u32 count, total, longest;
  u32 bucket[WAIT_BUCKETS];
};
static struct waits wait_frame = { .name = "frame starts" }, wait_reply = { .name = "cable words (player 1)" },
                    wait_horizon = { .name = "cable time (player 2)" }, wait_round = { .name = "transfer to word" };

static void count_wait(struct waits *w, u32 ticks)
{
  unsigned us = ticks_us(ticks), b = 0;

  while (b < WAIT_BUCKETS - 1 && us >= 250u << b)
    b++;
  w->count++;
  w->total += ticks;
  if (ticks > w->longest)
    w->longest = ticks;
  w->bucket[b]++;
}

static void log_waits(const struct waits *w)
{
  char hist[100];
  unsigned i, n = 0;

  if (!w->count)
    return;
  for (i = 0; i < WAIT_BUCKETS; i++)
    n += (unsigned)snprintf(hist + n, sizeof(hist) - n, "%s%u", i ? " " : "", (unsigned)w->bucket[i]);
  nspire_link_log("  waits, %s: %u, total %u ms, longest %u.%02u ms; <1/4 <1/2 <1 <2 <4 <8 <16 <32 <64 more ms: %s",
                  w->name, (unsigned)w->count, ticks_us(w->total) / 1000, ticks_us(w->longest) / 1000,
                  ticks_us(w->longest) / 10 % 100, hist);
}

static void waits_reset(void)
{
  struct waits *all[] = { &wait_frame, &wait_reply, &wait_horizon, &wait_round };
  unsigned i;

  for (i = 0; i < 4; i++)
  {
    const char *name = all[i]->name;
    memset(all[i], 0, sizeof(*all[i]));
    all[i]->name = name;
  }
}

/* While linked: messages and bytes each way, frames, and when it linked. */
static u32 sent_msgs, sent_bytes, got_msgs, got_bytes, linked_frames, linked_at;

/* ping_phase's: this side's pings answered (each one's round trip, in
 * ticks) and the other's this side answered. */
static int pongs_got, pings_answered;
static u32 ping_at, ping_round[PINGS];

static int send_msg(const void *msg, unsigned len)
{
  sent_msgs++;
  sent_bytes += len;
  {
    int rc = link_cable_send(msg, len);
    /* A frame's end may wait a few frames to go with the next (the
     * transport); anything else is what the other side is waiting for. */
    if (rc >= 0 && ((const u8 *)msg)[0] != MSG_FRAME)
      link_cable_flush();
    return rc;
  }
}

static void log_link_end(const char *why)
{
  u32 ms = ticks_us(nspire_ticks() - linked_at) / 1000;
  char line[LOG_LINE];

  beats_into_log();
  nspire_link_log("unlinked: %s", why);
  nspire_os_diag_usb(line, sizeof(line));
  nspire_link_log("  %s", line);
  if (!linked_at)
    return;
  nspire_link_log("  %u frames in %u.%u s (%u.%u a second); messages sent %u (%u bytes), received %u (%u bytes)",
                  (unsigned)linked_frames, (unsigned)(ms / 1000), (unsigned)(ms / 100 % 10),
                  ms ? (unsigned)((u64)linked_frames * 1000 / ms) : 0,
                  ms ? (unsigned)((u64)linked_frames * 10000 / ms % 10) : 0,
                  (unsigned)sent_msgs, (unsigned)sent_bytes, (unsigned)got_msgs, (unsigned)got_bytes);
  log_waits(&wait_frame);
  log_waits(&wait_reply);
  log_waits(&wait_round);
  log_waits(&wait_horizon);
  linked_at = 0;
}

static void set_help(void)
{
  /* r19: the band's one line on the Link page (40 characters): what enter does now, or why the link stopped */
  if (state == LINK_LINKED)
    snprintf(nspire_link_help, sizeof(nspire_link_help), "%s. Enter unlinks.", status);
  else if (state == LINK_LOOKING)
    snprintf(nspire_link_help, sizeof(nspire_link_help), "Waiting: pick Link on the other one.");
  else if (strcmp(status, "Not linked"))
    snprintf(nspire_link_help, sizeof(nspire_link_help), "%s", status);
  else
    snprintf(nspire_link_help, sizeof(nspire_link_help), "Plug in the cable, pick Link on both.");
}

static void set_status(const char *text)
{
  snprintf(status, sizeof(status), "%s", text);
  nspire_link_log("status: %s", text);
  snprintf(last_status, sizeof(last_status), "%s", text);
  set_help();
}

/* Whether a serial_mode is the cable played by a stand-in. */
static int quick_cable(u32 mode)
{
  return mode == SERIAL_MODE_SERIAL_POKE || mode == SERIAL_MODE_SERIAL_AW1 ||
         mode == SERIAL_MODE_SERIAL_AW2 || mode == SERIAL_MODE_SERIAL_FE ||
         mode == SERIAL_MODE_SERIAL_MSIO;
}

/* Whether gpSP links a game with this device plugged in (serial_mode). */
static int linkable(int mode)
{
  return mode == SERIAL_MODE_RFU || mode == SERIAL_MODE_CABLE || quick_cable(mode);
}

const char *nspire_link_device_name(void)
{
  if (serial_mode == SERIAL_MODE_RFU)
    return "Wireless Adapter";
  if (serial_mode == SERIAL_MODE_CABLE)
    return "Link cable, exact";
  if (quick_cable(serial_mode))
    return "Link cable, quick";
  return NULL;
}

/* The link cable's stand-ins: games (their codes, as gba_over.h; 3 letters
 * for every region) whose cable gpSP plays with a stand-in (serial_mode), on
 * Auto and Link cable; every other game, and Link cable (exact), has the
 * cable as wired. Both GBAs must play it the same way: the hello checks,
 * and a stand-in meeting the cable as wired plays it as wired too (Golden
 * Sun's with The Lost Age, which has none).
 * A game goes in once its link has worked that way on the PC
 * (tests/link/run.py; gpsp-host --stand-in tries one first). */
static const struct
{
  char code[5];
  int mode;
} stand_ins[] =
{
  /* Pokemon Ruby, Sapphire, Emerald, FireRed, LeafGreen, every language (3
   * letters: the code without its last): Gen 3's link, serialpoke
   * (tests/link/scenarios/pokemon.py). On Auto the last three keep the
   * wireless adapter. */
  { "AXV", SERIAL_MODE_SERIAL_POKE }, { "AXP", SERIAL_MODE_SERIAL_POKE },
  { "BPE", SERIAL_MODE_SERIAL_POKE }, { "BPR", SERIAL_MODE_SERIAL_POKE },
  { "BPG", SERIAL_MODE_SERIAL_POKE },
  /* Advance Wars (US, Europe): tests/link/scenarios/advance_wars.py */
  { "AWRE", SERIAL_MODE_SERIAL_AW1 }, { "AWRP", SERIAL_MODE_SERIAL_AW1 },
  /* Advance Wars 2 (US, Europe): libretro's own choice (gba_over.h), not
   * tried here (no game to try it with) */
  { "AW2E", SERIAL_MODE_SERIAL_AW2 }, { "AW2P", SERIAL_MODE_SERIAL_AW2 },
  { "BE8E", SERIAL_MODE_SERIAL_FE },   /* Fire Emblem: The Sacred Stones (US) */
  { "BE8P", SERIAL_MODE_SERIAL_FE },   /* (Europe) */
  { "BE8J", SERIAL_MODE_SERIAL_FE },   /* (Japan) */
  /* Golden Sun (US/Europe, Japan, Germany, France, Italy, Spain) */
  { "AGSE", SERIAL_MODE_SERIAL_MSIO }, { "AGSJ", SERIAL_MODE_SERIAL_MSIO },
  { "AGSD", SERIAL_MODE_SERIAL_MSIO }, { "AGSF", SERIAL_MODE_SERIAL_MSIO },
  { "AGSI", SERIAL_MODE_SERIAL_MSIO }, { "AGSS", SERIAL_MODE_SERIAL_MSIO },
  { "", SERIAL_MODE_CABLE }   /* the end */
};

#ifdef NSPIRE_HOST
static char try_code[5];
static int try_mode;

int nspire_link_try_stand_in(const char *code_and_mode)
{
  static const struct { const char *name; int mode; } modes[] =
  {
    { "poke", SERIAL_MODE_SERIAL_POKE }, { "aw1", SERIAL_MODE_SERIAL_AW1 },
    { "aw2", SERIAL_MODE_SERIAL_AW2 }, { "msio", SERIAL_MODE_SERIAL_MSIO },
    { "exact", SERIAL_MODE_CABLE }, { "fe", SERIAL_MODE_SERIAL_FE }   /* serial_fe.c */
  };
  unsigned i;

  if (strlen(code_and_mode) < 6 || code_and_mode[4] != ':')
    return -1;
  for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
    if (!strcmp(code_and_mode + 5, modes[i].name))
    {
      memcpy(try_code, code_and_mode, 4);
      try_mode = modes[i].mode;
      return 0;
    }
  return -1;
}
#endif

/* The cable for the game just loaded: its stand-in, or as wired. */
static int cable_mode(void)
{
  unsigned i;

#ifdef NSPIRE_HOST
  if (try_code[0] && !memcmp(try_code, gamepak_code, 4))
    return try_mode;
#endif
  /* An entry of 3 letters is the game in every region. */
  for (i = 0; stand_ins[i].code[0]; i++)
    if (!memcmp(stand_ins[i].code, gamepak_code, strlen(stand_ins[i].code)))
      return stand_ins[i].mode;
  return SERIAL_MODE_CABLE;
}

/* What's plugged into the GBA: the setting's, or on Auto the wireless
 * adapter for the games gpSP's list (gba_over.h) gives one and the link
 * cable for every other game. The cable is the game's stand-in or as wired
 * (cable_mode), or as wired on Link cable (exact); gba_over.h's Advance
 * Wars stand-ins count only once they're in the table. The Game Boy
 * Player stays. */
/* A GBA's link as its setting and game have it (this side's, or the other
 * side's from its hello): whether it wants the wireless adapter (Wireless,
 * or Auto with a game that has one), asked for the cable as wired, has a
 * game the adapter's for, its setting, and the cable it would play: its
 * game's stand-in, or as wired (SERIAL_MODE_CABLE) on Link cable (exact)
 * or for a game without one. */
struct side
{
  int wants_adapter, exact, adapter_game, setting;
  int cable;
  u32 fast;   /* EXT_FAST, 0 if none */
};

static struct side my_side(void)
{
  struct side s;

  s.setting = nspire_link_device % LINK_DEVICES;
  s.adapter_game = game_mode == SERIAL_MODE_RFU;
  s.wants_adapter = s.setting == LINK_DEVICE_WIRELESS ||
                    (s.setting == LINK_DEVICE_AUTO && s.adapter_game);
  s.exact = s.setting == LINK_DEVICE_EXACT;
  s.cable = s.exact ? SERIAL_MODE_CABLE : cable_mode();
  s.fast = nspire_link_fast_allowed() ? link_cable_fast_offer() : 0;
  return s;
}

/* What a GBA plays alone: the adapter if it wants it, else its cable. */
static int alone(const struct side *s)
{
  return s->wants_adapter ? SERIAL_MODE_RFU : s->cable;
}

static int device_mode(void)
{
  struct side me = my_side();

  if (game_mode == SERIAL_MODE_GBP && me.setting == LINK_DEVICE_AUTO)
    return SERIAL_MODE_GBP;
  return alone(&me);
}

/* A game just loaded: nothing plugged in has anything to forget yet. */
void nspire_link_apply_device(void)
{
  game_mode = serial_mode;
  nspire_link_disconnect();
  state_note[0] = 0;
  sio_session("a game loaded");
  nspire_link_device %= LINK_DEVICES;
  serial_mode = device_mode();
  if (!nspire_link_help[0])
    set_help();
}

/* Puts mode's device in the GBA's link port (serial_mode) as a real one
 * would be while the game runs: the one there comes out, the new one starts
 * fresh, the wireless adapter from its reset (the game finds it as any
 * adapter it hasn't set up; one it was talking to is gone, as if pulled
 * out) and a stand-in with nothing said yet. A transfer under way ends as
 * it would have (serial.c counts it down whatever's plugged in). */
static void plug_in(int mode)
{
  if (mode == serial_mode)
    return;
  if (serial_mode == SERIAL_MODE_RFU || mode == SERIAL_MODE_RFU)
    rfu_reset();
  if (quick_cable(serial_mode) || quick_cable(mode))
  {
    serialproto_reset();   /* serialpoke, serialaw and serial_fe.c */
    serialmsio_reset();
  }
  serial_mode = mode;
}

void nspire_link_device_changed(void)
{
  nspire_link_disconnect();
  nspire_link_device %= LINK_DEVICES;
  plug_in(device_mode());
  if (!nspire_link_help[0])
    set_help();
}

enum link_state nspire_link_state(void)
{
  return state;
}

const char *nspire_link_status(void)
{
  return status;
}

int nspire_link_protocol(void)
{
  return protocol;
}

static void send_head(int type, int flags, u32 frame)
{
  u8 msg[HEAD];
  msg[0] = type;
  msg[1] = flags;
  put16(msg + 2, 0);
  put32(msg + 4, frame);
  send_msg(msg, HEAD);
}

/* Linking on its own (the cable put in is found by itself and the link
 * said to be up, without a visit to the menu): with no link up
 * and something on the USB port (nspire_usb_plugged, the controller's own
 * state: no cost, and the OS isn't let run without it), each game frame
 * lets the transport watch for the other calculator's gpSP
 * (link_cable_watch), and it links once that's there (auto_link): only
 * both ways (link_cable_open), without the game waiting for the other's
 * hello (it may have only taken the connection: its menu, its file list),
 * and only if the two games' links go together (games_link: the same
 * game, the same stand-in, both with the adapter): else any two games
 * would run in lockstep. A game with no link in gpSP (the Game Boy
 * Player's) doesn't watch.
 *
 * After a link the user ended (unlinked here or there, a refused one) it
 * doesn't link again on its own until the other side connects anew,
 * nothing's on the cable any more (it came out: the port's state, each
 * frame, or a look that sees nothing), or Link is picked: else two that
 * unlinked would link again at once. After one the cable ended (it came
 * out, the other stopped answering) it tries again after AUTO_RETRY_MS.
 * auto_off: this calculator can't link (its OS), so no watching. */
static int auto_held, auto_off, auto_link;
static u32 auto_retry_at;
/* What the last watch saw on the cable (link_cable_watch's 0-2). */
static int cable_seen;
/* Linking on its own: when the hello went (the other's is given
 * AUTO_HELLO_MS to come, the game going on meanwhile). */
static u32 auto_hello_at;
#define AUTO_HELLO_MS 5000
#define AUTO_RETRY_MS 5000

/* The USB port's state as last seen (the log's plug-in timeline): its
 * bits, when they changed and the OS's sleeps then, and whether a node
 * and a connection were seen since. */
static u32 usb_bits, usb_changed_at, usb_changed_sleeps;
/* LINK2: a host's end that never ran (stuck_host_frame). */
static int stuck_step;      /* 0 watching, 1 a cycle made, 2 told */
static u32 stuck_at;        /* the cycle's end */
static u32 looking_at;      /* when the link started looking */
static u32 stuck_change;    /* usb_changed_at when told */
static int usb_was_empty = 1;   /* DETECT: the port read EMPTY at its last change (or never changed) */
static u32 usb_bits_on;         /* DETECT: its last bits with something on it (a CX's gate reads 0) */
/* P2: the link's watch gives the OS a tick each frame for this long after
 * the port's change (link_cable_watch's 'fresh'). */
#define PORT_FRESH_MS 10000
/* Since the port last changed: the controller ran / the port was enabled at some frame. */
static int usb_ran, usb_enabled;
static int usb_timeline, usb_node_seen, usb_connected, node_told;
static u32 usb_node_at;

/* Who has the USB port (a pad, the pad driver's Direct mode, a drive) is
 * the arbiter's (nspire_arbiter.c; its weak defaults are noipa for LTO, as
 * nspire-usbpad's docs/gpsp-requests.md item 1 asks). */
/* Whether the pad driver has seen a device (or an event about one: refused,
 * unsupported, replug) at this port state: then the adapter's end is right
 * and no hint about it. Without the driver, never. */
/* The heartbeat's pad line (nspire_usbpad_glue.c): 1 = text written. */
int nspire_usbpad_heartbeat(char *line, unsigned size) __attribute__((weak, noipa));
int nspire_usbpad_heartbeat(char *line, unsigned size) { (void)line; (void)size; return 0; }

int nspire_usbpad_saw_device(void) __attribute__((weak, noipa));
int nspire_usbpad_saw_device(void) { return 0; }
int nspire_link_cable_present(void)
{
  return state != LINK_OFF || cable_seen > 0;
}

/* The port keeps resetting (the flap toast was said within the last minute):
 * the Trade / Link card says to swap the cable's ends. Declared below
 * with the flap state. */
static int flap_swap_hint(void);
int nspire_link_swap_hint(void)
{
  return flap_swap_hint() || nspire_arb_swap_hint();
}
/* The link's figures every HEARTBEAT_FRAMES frames into its log's ring
 * (beat_log), written every HEARTBEAT_WRITE (30 s: a write is the game's
 * time, which the other side waits out) or when the port or the screen
 * changed: what came before a crash is there. */
#define HEARTBEAT_FRAMES 600
#define HEARTBEAT_WRITE  3
/* R2: at 8 frames a second 600 frames are 74 s: a heartbeat at least this
 * often (the ring keeps the last 8 KB of them), whatever the frame rate. */
#define HEARTBEAT_MS     10000
static u32 heartbeat_at, heartbeat_frames, heartbeats, beat_usb_bits;
/* When the game's buttons (the GBA's REG_P1, as gpSP last set it) last
 * changed: the heartbeat says how long ago (the OS's auto power down counts
 * 3 minutes without a key, by default, and gpSP reads the keys itself). */
static u32 keys_at, keys_last;
static char beat_lcd[LOG_LINE];
static int watchdog_logged;
static u32 second_sleeps;
/* LINK2: this second's drawn frames and its OS sleeps outside nspire_wait_until's
 * idle wait (nspire_os_busy_slept's count): the rest were the frames' spare time's. */
static u32 second_drawn;
static uint32_t second_busy;   /* (nspire_os.h's type: unsigned long on the calculator) */
/* LINK2: frames run while not linked since the last heartbeat (r6: the
 * heartbeats after the cable's pull said "0 frames", the linked ones only,
 * while the game ran on alone). */
static u32 alone_frames;
static unsigned packets_out, packets_in;   /* the game's netpackets this second */

/* R4 (R3's logs: both games' SIOCNT read 2008/201c, port closed, at the
 * Cable Club's "Please wait"): the game's own writes to SIOCNT and RCNT,
 * into the ring with the frame (frame_counter): the first SIO_LOGGED of a
 * session (a game's start, a state loaded, a link up), then a write that
 * changes what the game wrote last, at most SIO_LOGGED_SECOND a second (the
 * rest counted). "now" is what the register holds after: the game's bits
 * and this side's (serial.c serial_mul_siocnt: SD, SI, the id). */
#define SIO_LOGGED         40
#define SIO_LOGGED_SECOND  8
#define SIO_LOGGED_LOG     16   /* R5: 60 filled R4's log (lines lost) */
static unsigned sio_logged, sio_logged_second, sio_unlogged, sio_writes_second;
static u16 sio_last[2] = { 0xFFFF, 0xFFFF };
static u32 sio_starts_at, sio_irqs_at;


static void sio_session(const char *why)
{
  sio_logged = sio_logged_second = sio_unlogged = 0;
  sio_last[0] = sio_last[1] = 0xFFFF;
  if (state != LINK_OFF && state_note[0])
    nspire_link_log("(before linking) %s", state_note);
  (state != LINK_OFF ? nspire_link_log : recent_add)("serial port log from here (%s): SIOCNT %04x RCNT %04x IE %04x IME %u, serial mode %d, "
             "player id %u", why, (unsigned)read_ioreg(REG_SIOCNT), (unsigned)read_ioreg(REG_RCNT),
             (unsigned)read_ioreg(REG_IE), (unsigned)read_ioreg(REG_IME), serial_mode,
             (unsigned)netplay_client_id);
}

void nspire_link_sio_write(int is_rcnt, u16 written, u16 now)
{
  sio_writes_second++;
  if (sio_logged >= SIO_LOGGED && written == sio_last[is_rcnt])
    return;
  sio_last[is_rcnt] = written;
  if (sio_logged >= SIO_LOGGED && sio_logged_second >= SIO_LOGGED_SECOND)
  {
    sio_unlogged++;
    return;
  }
  if (sio_logged >= SIO_LOGGED)
    sio_logged_second++;
  sio_logged++;
  /* The log has the first SIO_LOGGED_LOG of a session (the ring the rest). */
  (sio_logged <= SIO_LOGGED_LOG && state != LINK_OFF ? nspire_link_log : recent_add)(
             "game wrote %s %04x (now %04x), frame %u, IE %04x, transfers %u, IRQs %u",
             is_rcnt ? "RCNT" : "SIOCNT", (unsigned)written, (unsigned)now, (unsigned)frame_counter,
             (unsigned)read_ioreg(REG_IE), (unsigned)serial_transfer_starts, (unsigned)serial_irqs_raised);
}

/* R4: Pokemon Emerald's (BPEE, pret/pokeemerald's symbols) link variables:
 * gWirelessCommType (1: the wireless adapter's code runs OpenLink), its
 * gReceivedRemoteLinkPlayers, gLink's state (0-1 opening, 2 handshake, 4
 * connected) and sLinkOpen. A state saved with the first two at 1 (a link
 * that ended badly) opens the adapter's link at the Cable Club, never the
 * cable's: "Please wait" for good (tests/link: rig resume_stale_link). */
static int emerald_vars(char *out, size_t size)
{
  if (memcmp(gamepak_code, "BPEE", 4))
  {
    out[0] = 0;
    return 0;
  }
  snprintf(out, size, "; Emerald: wireless %u, remote players %u, link state %u, link open %u",
           (unsigned)read_memory8(0x030030FC), (unsigned)read_memory8(0x03003124),
           (unsigned)read_memory8(0x03003171), (unsigned)read_memory8(0x020229C4));
  return 1;
}

/* nspire_states.c: a state was loaded (its RAM and registers are the game's
 * now, this side's serial emulation is as it was). */
void nspire_link_state_loaded(int slot)
{
  char em[120];
  emerald_vars(em, sizeof(em));
  snprintf(state_note, sizeof(state_note), "state loaded (slot %d): SIOCNT %04x RCNT %04x IE %04x, serial mode %d, "
           "%s%s", slot, (unsigned)read_ioreg(REG_SIOCNT), (unsigned)read_ioreg(REG_RCNT),
           (unsigned)read_ioreg(REG_IE), serial_mode, state == LINK_LINKED ? "linked" : "not linked", em);
  (state != LINK_OFF ? nspire_link_log : recent_add)("%s", state_note);
  sio_session("state loaded");
  /* A state saved while a link was going (the other calculator reset at the
   * Cable Club, the game was left and saved its state there): Emerald's
   * cable link then never opens at the Cable Club. A restart clears it. */
  if (!memcmp(gamepak_code, "BPEE", 4) && read_memory8(0x030030FC) == 1 && read_memory8(0x03003124) == 1 &&
      !read_memory8(0x020229C4))
  {
    nspire_link_log("this state was saved during a link (Emerald: wireless 1, remote players 1): the Cable "
                    "Club will wait for good; restart the game before linking");
    nspire_osd_message("State saved mid-link: restart the game before trading");
  }
}

/* nspire_backup.c: the game's save is about to be written to its file
 * (start) or was (end, its bytes and time). R4: the CX II reset at the
 * Cable Club, which saves the game just before "Please wait", and never
 * walking about linked: while linked the ring says so, and is written
 * first, so a crash in the write leaves it as the last line. */
static u32 game_save_at;
void nspire_link_game_save(int end, unsigned bytes)
{
  if (state != LINK_LINKED)
    return;
  if (!end)
  {
    game_save_at = nspire_ticks();
    nspire_link_log("game save: writing its file now (frame %u, linked frame %u)", (unsigned)frame_counter,
               (unsigned)my_frame);
    last_flush();
    return;
  }
  nspire_link_log("game save: written, %u bytes in %u ms", bytes, ticks_us(nspire_ticks() - game_save_at) / 1000);
}

/* How a link ended, for linking on its own after it: the user's (held)
 * or the cable's (tried again after a while). */
enum { END_HELD, END_RETRY };
/* When a link last ended (the live log flush keeps off the retry's time). */
static u32 link_ended_at;
static int link_ever_ended;

static void end_link_as(const char *why, int how)
{
  if (state == LINK_OFF)
    return;
  /* A wait that ended with no connection: which of the OS's steps never
   * happened since the port last changed (R1). */
  if (usb_timeline && !usb_connected && state == LINK_LOOKING)
    nspire_link_log("gave up waiting, %u ms after the USB port's last change: OS sleeps +%u%s; host controller %s; "
                    "port %s; NavNet node %s", ticks_us(nspire_ticks() - usb_changed_at) / 1000,
                    (unsigned)(nspire_os_sleeps() - usb_changed_sleeps),
                    nspire_os_sleeps() == usb_changed_sleeps ? " (the OS got no time)" : "",
                    usb_ran ? "ran" : "NEVER RAN (stopped)", usb_enabled ? "was enabled" : "NEVER ENABLED",
                    usb_node_seen || link_cable_last_nodes() ? "seen" : "NEVER SEEN");
  keep_linked = state == LINK_LINKED;   /* a link that came up: its log is kept (gpsp_link_linked.txt.tns) */
  log_link_end(why);
  log_flush_wanted = 1;
  link_cable_close();
  nspire_link_fast_end();
  state = LINK_OFF;
  cable_linked = 0;
  reply_owed = have_promise = promised = count_owed = 0;
  netplay_client_id = 0;
  netplay_num_clients = 0;
  queue_head = queue_tail = 0;
  out_len = HEAD;
  serialmsio_reset();
  set_status(why);
  if (bring_running())
    bring_end("the link ended (see above)");   /* DETECT: why it ended is the last word */
  nspire_osd_message("%s", why);
  auto_held = how == END_HELD;
  link_ended_at = nspire_ticks();
  link_ever_ended = 1;
  auto_retry_at = nspire_ticks() + (how == END_RETRY ? AUTO_RETRY_MS * (NSPIRE_TICK_HZ / 1000) : 0);
  auto_link = 0;
}

static void end_link(const char *why)
{
  end_link_as(why, END_HELD);
}

/* The link broke (the cable, the other calculator): linking on its own
 * tries again after a while. */
static void link_broke(const char *why)
{
  end_link_as(why, END_RETRY);
}

/* Starts linking: picked (Link) or on its own (watch_cable). */
static void start_link(int on_its_own)
{
  char line[LOG_LINE];
  const char *why;

  if (state != LINK_OFF)
    return;
  /* The pad driver's Direct mode, or a controller in the port (the cable
   * can't be): the arbiter's rule. */
  if ((why = nspire_arb_link_refusal()))
  {
    set_status(why);
    nspire_osd_message("%s", status);
    return;
  }
  auto_held = 0;
  /* The setting's device again, if the last link chose another. */
  plug_in(device_mode());
  if (!nspire_link_device_name())
  {
    set_status("This game has no link in gpSP.");
    nspire_osd_message("%s", status);
    auto_held = 1;   /* on its own: not again until something changes */
    return;
  }
  log_begin();
  nspire_link_log("Link %s: %s, game %.4s, setting %u, device %s", on_its_own ? "on its own" : "picked",
                  is_cx2 ? "CX II" : "CX", gamepak_code, (unsigned)(nspire_link_device % LINK_DEVICES),
                  nspire_link_device_name());
  nspire_os_diag_system(line, sizeof(line));
  nspire_link_log("%s", line);
  nspire_os_diag_usb(line, sizeof(line));
  nspire_link_log("%s", line);
  nspire_os_diag_screen(line, sizeof(line));
  nspire_link_log("%s", line);
  if (link_cable_open(on_its_own) < 0)
  {
    nspire_link_log("couldn't start: %s", link_cable_error() ? link_cable_error() : "?");
    nspire_link_log_flush();
    set_status(link_cable_error() ? link_cable_error() : "Can't use the USB port.");
    nspire_osd_message("%s", status);
    auto_held = 1;
    return;
  }
  my_nonce = link_cable_nonce();
  hello_sent = hello_tries = 0;
  other_in_menu = 0;   /* LNK-3: the last link's other may have left from its menu */
  auto_link = on_its_own;
  state = LINK_LOOKING;
  looking_at = nspire_ticks();
  stuck_step = 0;
  if (on_its_own)
  {
    nspire_link_log("linking on its own: the other calculator's gpSP is on the cable");
    set_status("Cable in: linking with the other calculator...");
  }
  else
    set_status("Looking for the other GBA...");
  nspire_osd_message("%s", status);
  /* DETECT: Link picked with something on the port: its status kept up and
   * the steps timed from now (on its own, the port's change started that). */
  if (!bring_running() && nspire_usb_plugged() > 0)
    bring_start(nspire_usb_bits(), 0);
}

void nspire_link_connect(void)
{
  start_link(0);
}

void nspire_link_disconnect(void)
{
  if (state == LINK_LINKED)
    send_head(MSG_BYE, 0, 0);
  end_link("Not linked");
}

void nspire_link_shutdown(void)
{
  nspire_link_disconnect();
  link_cable_shutdown();
  nspire_link_fast_end();   /* FAST: a clean quit: no crash in the fast transport (its marker) */
  /* EXIT-3: the log and the clean marker are written once, last
   * (nspire_link_exit_close, from nspire_exit.c): this flush and marker
   * were two of the CX's slow writes at every quit. */
}

int nspire_link_shutdown_within(unsigned ms)
{
  int r;

  nspire_link_disconnect();
  r = link_cable_shutdown_within(ms);
  nspire_link_fast_end();
  return r;
}

void nspire_link_exit_close(const char *lines)
{
  u32 t0 = nspire_ticks();
  unsigned len, ms = 0;
  char *text;
  const char *line;

  /* The log gets the lines only if this run has one (a first write would
   * rotate the older logs: renames and copies, for an exit's lines). */
  if (log_written || log_new)
  {
    if (state == LINK_LINKED)
      log_link_end("gpSP closed while linked");
    for (line = lines; *line;)
    {
      const char *end = strchr(line, '\n');
      unsigned n = end ? (unsigned)(end - line) : (unsigned)strlen(line);
      if (n)
        nspire_link_log("exit: %.*s", (int)n, line);
      line += n + (end ? 1 : 0);
    }
    nspire_link_log_flush();
    ms = (unsigned)((u64)(nspire_ticks() - t0) * 1000 / NSPIRE_TICK_HZ);
  }
  /* The marker: "clean ..." first (check_last_run reads that), then the
   * exit's lines, which only it has when the run wrote no log. */
  len = (unsigned)strlen(lines) + 200;
  text = (char *)malloc(len);
  if (!text)
  {
    marker_write(1);
    return;
  }
  exit_clock = (unsigned)nspire_rtc_seconds();
  snprintf(text, len, "clean at clock %u s, build %s\n%slog written in %u ms\n", exit_clock, NSPIRE_BUILD_GIT,
           lines, ms);
  write_small("gpsp_link_run.txt.tns", text, (unsigned)strlen(text));
  free(text);
}

/* EXIT-R4: after the hand-back (nspire_exit.c), only with no key down: the
 * words read back, in a file of its own (the log and the marker are closed
 * by then). The next run (check_last_run) pairs it with the marker by the
 * marker's clock: missing, it says the main task stopped between them. */
void nspire_link_exit_after(const char *line)
{
  char text[2000];   /* R11t: the clock select's line too; QUIT6: the timer's interrupt, the lines */

  snprintf(text, sizeof(text), "after the hand-back of the run marked clean at clock %u s, build %s\n%s\n",
           exit_clock, NSPIRE_BUILD_GIT, line);
  write_small(EXIT_AFTER_FILE, text, (unsigned)strlen(text));
}

/* ---- The hello ----
 *
 * The rule, for this build and every later one: a gpSP with the link links
 * with every other gpSP with the link, older or newer, whatever the two
 * are set to and play. So:
 *
 * A hello's body is magic (4 bytes, "GPLK"), version (2), mode (2), nonce
 * (4) and game code (4), and from protocol 2 on the extension: its length
 * (2), then fields of a tag (1), a length (1) and the value (EXT_*). These
 * 16 bytes keep this meaning forever: version is 1 in every build, and
 * mode what e24876d (protocol 1, the first link) would pick for this game
 * and setting, the Wireless Adapter or the cable as wired, so that it
 * links with it. What a build is and wants goes in the extension; a later
 * build adds fields (new tags, or more bytes at a field's end) and a
 * higher EXT_PROTOCOL, and a reader skips tags it doesn't know and bytes
 * past the part of a field it knows.
 *
 * A build says hello first as MSG_HELLO_EXT, a message protocol 1 doesn't
 * know and skips. A hello without an extension is protocol 1's (e24876d;
 * or 8229804, which also played stand-ins under version 1), and that build
 * links only with its own mode: so this side takes it (or, for a stand-in
 * it doesn't play the same way, the cable as wired, which 8229804 then
 * plays too) and answers with a MSG_HELLO of that mode and a nonce that
 * isn't the other's, and speaks protocol 1 on the wire: nothing protocol 1
 * doesn't understand, and never waiting for something it never sends. Two
 * hellos with extensions speak the highest protocol both do (a build keeps
 * speaking every older one), and both sides make the same choice from the
 * two (link_choice): what a real pair of GBAs would use. A nonce tie rolls
 * a new nonce and says hello again. The only refusals left: a hello that
 * isn't gpSP's (its magic, or a version other than 1, which no build
 * sends), a protocol-1 mode no gpSP sends, and HELLO_TRIES ties in a row. */

/* The mode e24876d would pick for this side's game and setting. */
static int legacy_mode(const struct side *s)
{
  return s->wants_adapter ? SERIAL_MODE_RFU : SERIAL_MODE_CABLE;
}

static void send_hello(int type, int mode)
{
  u8 msg[HEAD + 48];
  u8 *body = msg + HEAD, *ext = body + 18;
  struct side me = my_side();
  u32 n = 0;

  memset(msg, 0, sizeof(msg));
  put32(body, LINK_MAGIC);
  put16(body + 4, LINK_VERSION);
  put16(body + 6, mode);
  put32(body + 8, my_nonce);
  memcpy(body + 12, gamepak_code, 4);
  ext[n++] = EXT_PROTOCOL;
  ext[n++] = 2;
  put16(ext + n, LINK_PROTOCOL);
  n += 2;
  ext[n++] = EXT_LINK;
  ext[n++] = 4;
  ext[n++] = (me.wants_adapter ? EXT_WANTS_ADAPTER : 0) | (me.exact ? EXT_EXACT : 0) |
             (me.adapter_game ? EXT_ADAPTER_GAME : 0);
  ext[n++] = me.setting;
  put16(ext + n, me.cable);
  n += 2;
  if (me.fast)
  {
    ext[n++] = EXT_FAST;
    ext[n++] = 4;
    put32(ext + n, me.fast);
    n += 4;
  }
  put16(body + 16, n);
  msg[0] = type;
  put16(msg + 2, 18 + n);
  send_msg(msg, HEAD + 18 + n);
  hello_sent = 1;
  nspire_link_log("hello sent (%s): mode %d, nonce %08x, protocol %d, setting %d, cable %d%s%s, fast %04x",
                  type == MSG_HELLO ? "protocol 1's" : "with extension", mode, (unsigned)my_nonce,
                  LINK_PROTOCOL, me.setting, me.cable, me.wants_adapter ? ", wants the adapter" : "",
                  me.exact ? ", exact" : "", (unsigned)me.fast);
}

/* The other side's hello's extension: 1 and its side and protocol, or 0 if
 * it has none (protocol 1). */
static int read_extension(const u8 *body, u32 len, struct side *s, int *proto)
{
  u32 end, at;

  if (len < 18)
    return 0;
  end = 18 + get16(body + 16);
  if (end > len)
    end = len;
  memset(s, 0, sizeof(*s));
  s->cable = SERIAL_MODE_CABLE;
  *proto = 2;
  for (at = 18; at + 2 <= end && at + 2 + body[at + 1] <= end; at += 2 + body[at + 1])
  {
    const u8 *v = body + at + 2;
    if (body[at] == EXT_PROTOCOL && body[at + 1] >= 2)
      *proto = get16(v);
    else if (body[at] == EXT_LINK && body[at + 1] >= 4)
    {
      s->wants_adapter = !!(v[0] & EXT_WANTS_ADAPTER);
      s->exact = !!(v[0] & EXT_EXACT);
      s->adapter_game = !!(v[0] & EXT_ADAPTER_GAME);
      s->setting = v[1];
      s->cable = get16(v + 2);
    }
    else if (body[at] == EXT_FAST && body[at + 1] >= 4)
      s->fast = get32(v);
  }
  return 1;
}

/* The link two GBAs use, from their two sides (the same both ways round):
 * the Wireless Adapter if both want it; else the cable, as every GBA has a
 * link port: the stand-in if both would play the same one and neither
 * asked for exact, else as wired, which carries any games' link (two
 * different games too). So two settings that disagree (Wireless and Link
 * cable or exact, Link cable and exact) get the cable, as wired over a
 * stand-in. */
static int link_choice(const struct side *a, const struct side *b)
{
  if (a->wants_adapter && b->wants_adapter)
    return SERIAL_MODE_RFU;
  if (!a->exact && !b->exact && a->cable == b->cable && quick_cable(a->cable))
    return a->cable;
  return SERIAL_MODE_CABLE;
}

/* Why the link chosen isn't what this side's setting gives alone, or NULL
 * if it is (old: the other side speaks protocol 1, whose mode it was). */
static const char *why(int chosen, const struct side *me, const struct side *other, int old)
{
  if (chosen == alone(me))
    return NULL;
  if (chosen == SERIAL_MODE_RFU)   /* only a protocol-1 side makes this one */
    return "the other gpSP is older and uses it";
  if (me->wants_adapter)
    return "the other GBA has no Wireless Adapter";
  if (other->exact)
    return "the other GBA is set to exact";
  if (other->cable == SERIAL_MODE_CABLE)
    return old ? "the other gpSP is older: exact only" : "the other game needs the exact cable";
  return "the other game's link is different";
}

/* Linking on its own: whether the two games' links go together, so the
 * link is worth running (else each game would wait on the other's frames
 * for nothing): the same game (its code's first three letters: any
 * region), the same stand-in (Gen 3 Pokemon with each other), or both with
 * the wireless adapter. The same both ways round. code: the other's game
 * code (its hello's); other: its side (for an older gpSP, from its mode). */
static int games_link(const struct side *me, const struct side *other, const u8 *code)
{
  if (!memcmp(code, gamepak_code, 3))
    return 1;
  if (quick_cable(me->cable) && me->cable == other->cable)
    return 1;
  return me->adapter_game && other->adapter_game;
}

/* The other side's hello (type: MSG_HELLO or MSG_HELLO_EXT). Who's player 1:
 * the higher nonce (on a GBA cable, the one at its purple end, the parent). */
static void got_hello(int type, const u8 *body, u32 len)
{
  struct side me = my_side(), other;
  const char *reason;
  int proto, old, chosen, i;
  u32 nonce;

  (void)type;
  if (len < 16 || get32(body) != LINK_MAGIC || get16(body + 4) != LINK_VERSION)
  {
    end_link("The other side isn't a gpSP link");
    return;
  }
  nonce = get32(body + 8);
  old = !read_extension(body, len, &other, &proto);
  nspire_link_log("hello came: game %.4s, mode %u, nonce %08x, %s", body + 12, (unsigned)get16(body + 6),
                  (unsigned)nonce, old ? "no extension (protocol 1)" : "with extension");
  other_fast = old ? 0 : other.fast;
  if (!old)
    nspire_link_log("  its protocol %d, setting %d, cable %d, fast %04x%s%s%s", proto, other.setting, other.cable,
                    (unsigned)other.fast,
                    other.wants_adapter ? ", wants the adapter" : "", other.exact ? ", exact" : "",
                    other.adapter_game ? ", adapter game" : "");
  if (old)
  {
    /* Protocol 1 links only in its own mode: the adapter, or a cable. */
    u32 mode = get16(body + 6);
    if (mode != SERIAL_MODE_RFU && mode != SERIAL_MODE_CABLE && !quick_cable(mode))
    {
      end_link("The other gpSP's link is one this one doesn't know");
      return;
    }
    memset(&other, 0, sizeof(other));
    other.wants_adapter = other.adapter_game = mode == SERIAL_MODE_RFU;
    other.cable = quick_cable(mode) ? (int)mode : SERIAL_MODE_CABLE;
  }
  if (auto_link && !games_link(&me, &other, body + 12))
  {
    /* The other hears it (its hello's taken, then this), and both go on
     * alone; Link picked links them anyway. */
    nspire_link_log("linking on its own: game %.4s doesn't link with %.4s: not linked", gamepak_code, body + 12);
    send_head(MSG_BYE, 0, 0);
    end_link("Cable in: the other game doesn't link with this one");
    return;
  }
  if (old)
  {
    u32 mode = get16(body + 6);
    chosen = mode == SERIAL_MODE_RFU ? SERIAL_MODE_RFU : link_choice(&me, &other);
    for (i = 0; my_nonce == nonce && i < HELLO_TRIES; i++)
      my_nonce = link_cable_nonce();
    if (my_nonce == nonce)   /* a transport whose nonces don't change */
      my_nonce = ~nonce;
    send_hello(MSG_HELLO, chosen);
    protocol = 1;
  }
  else
  {
    if (nonce == my_nonce)
    {
      /* Both rolled the same: both roll again and say hello again. */
      if (++hello_tries >= HELLO_TRIES)
      {
        end_link("Couldn't pick player 1: try again");
        return;
      }
      my_nonce = link_cable_nonce();
      send_hello(MSG_HELLO_EXT, legacy_mode(&me));
      return;
    }
    chosen = link_choice(&me, &other);
    protocol = proto < 1 ? 1 : proto > LINK_PROTOCOL ? LINK_PROTOCOL : proto;
  }
  plug_in(chosen);
  reason = why(chosen, &me, &other, old);
  netplay_client_id = nonce < my_nonce ? 0 : 1;
  netplay_num_clients = netplay_client_id ? 0 : 1;
  my_frame = 0;
  other_done = -1;
  other_in_menu = 0;
  link_delay = LINK_DELAY;
  other_delay = -1;
  delay_agreed = 0;
  remeasuring = 0;
  queue_head = queue_tail = 0;
  out_len = HEAD;
  cable_clock = horizon = told = tell_till = reply_for = 0;
  cable_linked = serial_mode == SERIAL_MODE_CABLE;
  xfer_pending = reply_got = reply_owed = have_promise = promised = count_owed = halt_seen = 0;
  serialmsio_reset();
  state = LINK_LINKED;
  snprintf(status, sizeof(status), "Linked: player %u (%s)%s%s", (unsigned)netplay_client_id + 1,
           nspire_link_device_name(), reason ? ": " : "", reason ? reason : "");
  set_help();
  nspire_link_log("%s; protocol %d, serial mode %d", status, protocol, serial_mode);
  sio_session("linked");
  sent_msgs = sent_bytes = got_msgs = got_bytes = linked_frames = 0;
  pongs_got = pings_answered = 0;
  waits_reset();
  heartbeat_frames = heartbeats = alone_frames = 0;
  heartbeat_at = nspire_ticks();
  second_sleeps = nspire_os_sleeps();
  nspire_os_busy_slept(&second_busy);
  second_drawn = 0;
  beat_usb_bits = 0;
  beat_lcd[0] = 0;
  linked_at = nspire_ticks() | 1;
  log_flush_wanted = 1;
  /* One line on the screen: what was chosen and why, if not the setting's. */
  if (reason)
    nspire_osd_message("%s: %s", serial_mode == SERIAL_MODE_RFU ? "Wireless Adapter" : "Link cable",
                       reason);
  else
    nspire_osd_message("%s", status);
}

static void queue_packets(const u8 *p, u32 len, u32 frame)
{
  while (len >= 2)
  {
    u32 n = get16(p);
    if (n + 2 > len)
      break;
    if (queue_tail + 6 + n > QUEUE_BYTES)
    {
      /* Wrap: move what's left to the start (it's little). */
      memmove(queue, queue + queue_head, queue_tail - queue_head);
      queue_tail -= queue_head;
      queue_head = 0;
      if (queue_tail + 6 + n > QUEUE_BYTES)
      {
        end_link("Link: too much to keep up with");
        return;
      }
    }
    put32(queue + queue_tail, frame);
    put16(queue + queue_tail + 4, n);
    memcpy(queue + queue_tail + 6, p + 2, n);
    queue_tail += 6 + n;
    p += 2 + n;
    len -= 2 + n;
  }
}

/* Player 1: player 2's promise, if it's for the transfer started at at
 * (made before it), is its word for it (if it started in time) and used
 * up; else it's for a later one. */
static void use_promise(u64 at)
{
  if (!have_promise || promise_at > at)
    return;
  have_promise = 0;
  if (at < promise_until)
  {
    reply_word = promise_word;
    reply_got = 1;
  }
}

/* The other side sent this in its frame: the ones before it are over. */
static void other_in_frame(u32 frame)
{
  if ((s32)frame - 1 > other_done)
    other_done = (s32)frame - 1;
}

/* One message from the other side, waiting up to wait_ms. 0 if none. */
static int receive(unsigned wait_ms)
{
  int len = link_cable_receive(in_msg, sizeof(in_msg), wait_ms);
  u32 body_len;

  if (len < 0)
  {
    /* The transport's reason if it has one (R6: a damaged stream). */
    const char *e = link_cable_error();
    link_broke(e ? e : "The link cable came out.");
    return 0;
  }
  if (len < HEAD)
    return 0;
  body_len = get16(in_msg + 2);
  if (HEAD + body_len > (u32)len)
    return 0;
  got_msgs++;
  got_bytes += (u32)len;
  switch (in_msg[0])
  {
  case MSG_PING:
    /* Answered at once with its body (ping_phase): any time, any state. */
    in_msg[0] = MSG_PONG;
    send_msg(in_msg, HEAD + body_len);
    pings_answered++;
    break;
  case MSG_DELAY:
    if (body_len >= 2)
    {
      other_delay = get16(in_msg + HEAD);
      /* After the start: only ever raised, never past LINK_DELAY_MAX. */
      if (delay_agreed && state == LINK_LINKED && (u32)other_delay > link_delay)
      {
        link_delay = (u32)other_delay > LINK_DELAY_MAX ? LINK_DELAY_MAX : (u32)other_delay;
        nspire_link_log("delay: raised to %u frames at frame %u (the other side asked)", (unsigned)link_delay,
                        (unsigned)my_frame);
      }
    }
    break;
  case MSG_PONG:
    if (get32(in_msg + 4) == (u32)pongs_got && pongs_got < PINGS)
      ping_round[pongs_got++] = nspire_ticks() - ping_at;
    else if (remeasuring && get32(in_msg + 4) == REMEASURE_SEQ + (u32)remeasure_got)
      remeasure_pong();
    break;
  case MSG_HELLO:
  case MSG_HELLO_EXT:
    if (state == LINK_LOOKING)
      got_hello(in_msg[0], in_msg + HEAD, body_len);
    break;
  case MSG_FRAME:
    if (state != LINK_LINKED)
      break;
    if (serial_mode == SERIAL_MODE_CABLE)
    {
      if (body_len >= 8 && get64(in_msg + HEAD) > horizon)
        horizon = get64(in_msg + HEAD);
    }
    else
      queue_packets(in_msg + HEAD, body_len, get32(in_msg + 4));
    if (in_msg[1] & FRAME_END)
      other_done = (s32)get32(in_msg + 4);
    break;
  case MSG_XFER:
    if (state != LINK_LINKED || body_len < 16)
      break;
    xfer_at = get64(in_msg + HEAD);
    xfer_time = get32(in_msg + HEAD + 8);
    xfer_word = get32(in_msg + HEAD + 12);
    xfer_pending = 1;
    /* Player 1 can't start another before this one's end. */
    if (xfer_at + xfer_time > horizon)
      horizon = xfer_at + xfer_time;
    break;
  case MSG_REPLY:
    if (state != LINK_LINKED || body_len < 4)
      break;
    reply_word = get32(in_msg + HEAD);
    if (reply_owed && !reply_got)
      count_wait(&wait_round, nspire_ticks() - xfer_sent_at);
    reply_got = 1;
    other_in_frame(get32(in_msg + 4));
    break;
  case MSG_PROMISE:
    if (state != LINK_LINKED || body_len < 20)
      break;
    have_promise = 1;
    promise_at = get64(in_msg + HEAD);
    promise_until = get64(in_msg + HEAD + 8);
    promise_word = get32(in_msg + HEAD + 16);
    /* For the transfer waiting for its word? (Not one answered already:
     * player 2 sends promises after it gave those.) */
    if (reply_owed && !reply_got)
      use_promise(reply_for);
    other_in_frame(get32(in_msg + 4));
    break;
  case MSG_MENU:
    /* LNK-3: only a linked other's menu holds this side; one while looking
     * is from a link before this one (as a BYE then). */
    if (state != LINK_LINKED)
      break;
    other_in_menu = in_msg[1] & 1;
    menu_msgs++;
    break;
  case MSG_BYE:
    /* Before the other's hello it's from a link before this one: on the
     * calculators, a connection's callback starts late (nspire_link_usb.c),
     * and a BYE from the other's last link came as this one said hello. */
    if (state == LINK_LOOKING)
      nspire_link_log("a BYE from an earlier link: left");
    else
      end_link("The other GBA unlinked");
    break;
  }
  return 1;
}

/* The packets of the other side's frames up to through, to the core. */
static void deliver(s32 through)
{
  u16 other = netplay_client_id ? 0 : 1;

  /* R5: the packet goes to the stand-in from a word-aligned copy. In the
   * queue it is 6 bytes past its entry's start (2 bytes off a word), and
   * serialpoke_net_receive (and serialaw's, rfu's) read it as u32 words:
   * the calculator checks alignment (nspire.c enableAlignmentExceptions), so
   * the first packet the other GBA sent crashed this one: the multiplayer
   * child, as the parent's game opened its link at the Cable Club's "Please
   * wait" (R1-R4: the child reset each time; x86 doesn't care). */
  static u32 aligned[(MSG_MAX + 3) / 4];

  while (queue_head < queue_tail && (s32)get32(queue + queue_head) <= through)
  {
    u32 n = get16(queue + queue_head + 4);
    const u8 *p = (const u8 *)aligned;

    memcpy(aligned, queue + queue_head + 6, n <= sizeof(aligned) ? n : sizeof(aligned));
    if (n > sizeof(aligned))
      n = sizeof(aligned);

    switch (serial_mode)
    {
    case SERIAL_MODE_RFU:
      rfu_net_receive(p, n, other);
      break;
    case SERIAL_MODE_SERIAL_POKE:
      serialpoke_net_receive(p, n, other);
      break;
    case SERIAL_MODE_SERIAL_AW1:
    case SERIAL_MODE_SERIAL_AW2:
      serialaw_net_receive(p, n, other);
      break;
    case SERIAL_MODE_SERIAL_FE:
      serialfe_net_receive(p, n, other);
      break;
    case SERIAL_MODE_SERIAL_MSIO:
      serialmsio_net_receive(p, n, other);
      break;
    }
    queue_head += 6 + get16(queue + queue_head + 4);
    packets_in++;
  }
  if (queue_head == queue_tail)
    queue_head = queue_tail = 0;
}

/* The frontend's (nspire_link.h); the unit tests have none. */
void nspire_link_waiting(unsigned waited_ms, int other_in_menu) __attribute__((weak));
void nspire_link_waiting(unsigned waited_ms, int other_in_menu) {}
int nspire_link_wait_cancelled(unsigned waited_ms) __attribute__((weak));
int nspire_link_wait_cancelled(unsigned waited_ms) { return 0; }

/* Whether a wait that waited (its slices) is past WAIT_GIVE_UP_MS. Without
 * a timer a slice is one block in a read, as long as the OS makes it (the
 * CX CAS, whose waits each ended at once: it gave up 0.3 s after linking):
 * then it's the real-time clock's seconds since the wait's first slice,
 * one more for the second it started in; or, if that doesn't move either,
 * 20 times the slices (a block is at least a ms). */
static int waited_too_long(unsigned waited, u32 since, unsigned limit)
{
  u32 seconds;

  if (nspire_timer_runs())
    return waited >= limit;
  seconds = nspire_rtc_seconds() - since;
  if (seconds)
    return seconds > limit / 1000;
  return waited >= 20 * limit;
}

/* Another slice of waiting for the other side went by with nothing. */
static void waited_more(unsigned *waited)
{
  static u32 since;   /* the real-time clock at the wait's first slice */
  static unsigned from, menus;   /* the waited at the last MSG_MENU, and their count then */

  if (!*waited || menus != menu_msgs)
  {
    since = nspire_rtc_seconds();
    from = *waited;
    menus = menu_msgs;
  }
  *waited += WAIT_SLICE_MS;
  if (*waited >= WAIT_SHOW_MS)
    nspire_link_waiting(*waited, other_in_menu);
  if (nspire_link_wait_cancelled(*waited))
    nspire_link_disconnect();
  else if (waited_too_long(*waited - from, since, other_in_menu ? MENU_GIVE_UP_MS : WAIT_GIVE_UP_MS))
  {
    send_head(MSG_BYE, 0, 0);
    link_broke("The other GBA stopped answering.");
  }
}

/* Protocol 2, as the two link: each side pings the other PINGS times, one
 * at a time, and answers the other's (receive), so the log has the round
 * trip, calculator to calculator, before the game's link adds its own.
 * Both sides stay until their pings are answered and they've answered the
 * other's PINGS, so neither takes the other's game messages here. A few
 * dozen ms; the PC's model counts them as any message. */
static void ping_phase(void)
{
  u8 msg[HEAD + PING_BIG];
  u32 sorted[PINGS / 2], t;
  unsigned waited = 0;
  int i, j, k, big;

  memset(msg, 0, sizeof(msg));
  msg[0] = MSG_PING;
  for (i = 0; i < PINGS && state == LINK_LINKED; i++)
  {
    big = i & 1;
    put16(msg + 2, big ? PING_BIG : 0);
    put32(msg + 4, (u32)i);
    ping_at = nspire_ticks();
    /* R4: each ping its own wait (they were one: 32 round trips of the
     * calculators' 0.1-3 s summed toward the 15 s give-up, and the waiting
     * screen came up mid-way for no stall). */
    waited = 0;
    if (send_msg(msg, HEAD + (big ? PING_BIG : 0)) < 0)
    {
      link_broke("The link cable came out.");
      break;
    }
    while (state == LINK_LINKED && pongs_got <= i)
      if (!receive(WAIT_SLICE_MS))
        waited_more(&waited);
  }
  waited = 0;
  while (state == LINK_LINKED && pings_answered < PINGS)
    if (!receive(WAIT_SLICE_MS))
      waited_more(&waited);
  if (state != LINK_LINKED)
    return;
  /* Min, median, 9 of 10 under, max: the bare heads, then the big ones. */
  for (big = 0; big < 2; big++)
  {
    for (i = 0, k = 0; i < PINGS; i++)
      if ((i & 1) == big)
      {
        t = ping_round[i];
        for (j = k++; j > 0 && sorted[j - 1] > t; j--)
          sorted[j] = sorted[j - 1];
        sorted[j] = t;
      }
    nspire_link_log("round trip, %d-byte messages (%d): min %u, median %u, 90%% %u, max %u us", HEAD + (big ? PING_BIG : 0),
                    k, ticks_us(sorted[0]), ticks_us(sorted[k / 2]), ticks_us(sorted[k * 9 / 10]),
                    ticks_us(sorted[k - 1]));
  }
  /* The link's figures from here (its waits, frames a second). */
  sent_msgs = sent_bytes = got_msgs = got_bytes = 0;
  linked_at = nspire_ticks() | 1;
}

/* Protocol 3, after the pings: the frames of delay this side needs (its
 * round trips' 9 in 10, half of it one way, plus a frame for the other's to
 * run and a half for luck), told to the other, and the larger of the two
 * used by both: each side then starts its frame f with the other's frame
 * f - link_delay there, both seeing the same. The cable as wired keeps
 * CABLE_LEAD (it needs a round trip per transfer whatever this is). */
static void delay_phase(void)
{
  u32 sorted[PINGS], t, mine, frame_us = 16743;
  unsigned waited = 0;
  int i, j;
  u8 msg[HEAD + 2];

  for (i = 0; i < PINGS; i++)
  {
    t = ticks_us(ping_round[i]);
    for (j = i; j > 0 && sorted[j - 1] > t; j--)
      sorted[j] = sorted[j - 1];
    sorted[j] = t;
  }
  t = sorted[PINGS * 9 / 10];
  mine = (t / 2 + frame_us + frame_us / 2 + frame_us - 1) / frame_us;
  if (mine < LINK_DELAY)
    mine = LINK_DELAY;
  if (mine > LINK_DELAY_MAX)
    mine = LINK_DELAY_MAX;
  memset(msg, 0, sizeof(msg));
  msg[0] = MSG_DELAY;
  put16(msg + 2, 2);
  put16(msg + HEAD, mine);
  if (send_msg(msg, HEAD + 2) < 0)
  {
    link_broke("The link cable came out.");
    return;
  }
  while (state == LINK_LINKED && other_delay < 0)
    if (!receive(WAIT_SLICE_MS))
      waited_more(&waited);
  if (state != LINK_LINKED)
    return;
  link_delay = (u32)other_delay > mine ? (u32)other_delay : mine;
  if (link_delay > LINK_DELAY_MAX)
    link_delay = LINK_DELAY_MAX;
  nspire_link_log("delay: %u frames (this side asked %u for a 90%% round trip of %u ms, the other %d)",
                  (unsigned)link_delay, (unsigned)mine, (unsigned)(t / 1000), other_delay);
  delay_agreed = 1;
}

/* After the fast transport fell back to NavNet mid-link (its RESUME:
 * nothing lost), the start's delay (2 frames, for a round trip of a ms)
 * would hold every frame for a NavNet round trip (the rig: 9 frames a
 * second). So the round trip is measured again, a ping at a time between
 * frames (the other answers them as they come, ping_phase's MSG_PING), the
 * delay raised by delay_phase's rule (here from the longest of
 * REMEASURE_PINGS) and told to the other (MSG_DELAY), which takes the
 * larger. A delay raised mid-link is a lag spike to the stand-ins (the
 * other's frame f comes at f + delay, whatever the delay: the matrix runs
 * them at 0-15 frames), and while the two differ each side still waits
 * only for what it needs, so neither waits on the other forever. The
 * cable as wired keeps CABLE_LEAD (a round trip per transfer anyway). */
#define REMEASURE_PINGS 6
static u32 remeasure_at, remeasure_round[REMEASURE_PINGS];

static void remeasure_ping(void)
{
  u8 msg[HEAD];
  memset(msg, 0, sizeof(msg));
  msg[0] = MSG_PING;
  put32(msg + 4, REMEASURE_SEQ + (u32)remeasure_got);
  remeasure_at = nspire_ticks();
  if (send_msg(msg, HEAD) < 0)
    remeasuring = 0;
}

static void remeasure_start(void)
{
  if (protocol < 3 || serial_mode == SERIAL_MODE_CABLE)
  {
    nspire_link_log("fast transport fell back: the delay stays %u frames (%s)", (unsigned)link_delay,
                    protocol < 3 ? "the other side's protocol has no MSG_DELAY" : "the cable as wired: CABLE_LEAD");
    return;
  }
  nspire_link_log("fast transport fell back at frame %u: the round trip measured again over NavNet (delay %u now)",
                  (unsigned)my_frame, (unsigned)link_delay);
  remeasuring = 1;
  remeasure_got = 0;
  remeasure_ping();
}

static void remeasure_pong(void)
{
  u32 longest = 0, shortest = 0xFFFFFFFFu, t, mine, frame_us = 16743;
  u8 msg[HEAD + 2];
  int i;

  remeasure_round[remeasure_got++] = nspire_ticks() - remeasure_at;
  if (remeasure_got < REMEASURE_PINGS)
  {
    remeasure_ping();
    return;
  }
  remeasuring = 0;
  for (i = 0; i < REMEASURE_PINGS; i++)
  {
    t = ticks_us(remeasure_round[i]);
    if (t > longest)
      longest = t;
    if (t < shortest)
      shortest = t;
  }
  mine = (longest / 2 + frame_us + frame_us / 2 + frame_us - 1) / frame_us;
  if (mine > LINK_DELAY_MAX)
    mine = LINK_DELAY_MAX;
  nspire_link_log("delay: round trips over NavNet after the fall back: min %u, max %u ms: this side asks %u frames (%u now)",
                  (unsigned)(shortest / 1000), (unsigned)(longest / 1000), (unsigned)mine, (unsigned)link_delay);
  if (mine > link_delay)
    link_delay = mine;
  memset(msg, 0, sizeof(msg));
  msg[0] = MSG_DELAY;
  put16(msg + 2, 2);
  put16(msg + HEAD, mine);
  send_msg(msg, HEAD + 2);
}

/* Waits for what the other side sends, until there: count_wait's time. */
static void wait_until_other(s32 need)
{
  unsigned waited = 0;
  u32 start;

  if (other_done >= need)
    return;
  start = nspire_ticks();
  while (state == LINK_LINKED && other_done < need)
    if (!receive(WAIT_SLICE_MS))
      waited_more(&waited);
  count_wait(&wait_frame, nspire_ticks() - start);
}

/* What nspire_usb_bits' role bit says, in words: this calculator is the
 * USB host (the adapter's end) or the device (the cable's end). A CX II's
 * is OTGCSR bit 21 (bit 5 of the bits' low byte: 1 the device's end), a CX's
 * OTGSC bit 8 (bit 0: 1 the device's end). nspire_os.c:269-276, 284-286. */
static int usb_is_host(uint32_t bits)
{
  switch (bits >> 28)
  {
  case 2:
    return !(bits & 0x20);
  case 1:
    return !(bits & 0x01);
  }
  return -1;
}

const char *nspire_usb_role_text(uint32_t bits)
{
  int host = usb_is_host(bits);

  return host < 0 ? "role unknown" :
         host ? "this calculator is USB host (adapter end)" : "this calculator is USB device (cable end)";
}

/* The USB port's state, each frame (a few register reads): its changes
 * into the log with the time and the OS's sleeps so far (did the OS get to
 * set up what was plugged in?), then the first node and the first
 * connection seen after one. */
/* DETECT (2026-11-11): from a plug-in to "Linked", what the user sees and
 * the log's time of each step. The r5 hardware run: 10.3 s from the port
 * coming up to "Linked" with nothing on screen. Now a message at once on
 * the cable's end (a host is on the other end: a calculator or a PC), or
 * at the adapter's end once BRING_HOST_QUIET_MS passed with nobody
 * claiming the port (a pad that the OS offers says "Controller connected"
 * itself, in a frame or two), kept on screen (nspire_osd_extend: no new
 * toast, nothing logged) while it comes up; a progress message of the
 * link's or the arbiter's own (one ending "...") is kept up in its place;
 * a final one (Linked, a failure, a hint) ends it, as do the pad's or a
 * drive's claim, the port going EMPTY and BRING_MAX_MS. One string compare
 * a frame while it runs, nothing otherwise. */
static int watched_frame;      /* the watch looked at the cable this frame */
static u32 watch_from, watched_at;   /* DETECT: since when it has, and the last frame it did */
#define BRING_HOST_QUIET_MS 1000
#define BRING_MAX_MS        30000
#define BRING_CABLE_TEXT    "Link cable in: connecting..."
#define BRING_DEVICE_TEXT   "USB device in: connecting..."
#define BRING_NODE_TEXT     "Calculator found: connecting..."
#define BRING_NO_ANSWER     "No answer on the link cable"
static struct
{
  int on;
  int port_change;      /* started by the port (1), or by Link picked with something on it (0) */
  u32 at, sleeps;       /* the port's change (or the pick), and the OS's sleeps then */
  u32 bits;
  unsigned seen;        /* LINK_UP_* seen so far */
  u32 seen_ms[4];
  char text[64];        /* the message kept up */
} bring;

static void bring_stop(void)
{
  bring.on = 0;
  bring.text[0] = 0;
}

static int bring_running(void)
{
  return bring.on;
}

static void bring_start(u32 bits, int port_change)
{
  memset(&bring, 0, sizeof(bring));
  bring.on = 1;
  bring.port_change = port_change;
  bring.at = nspire_ticks();
  bring.sleeps = nspire_os_sleeps();
  bring.bits = bits;
  /* (No line here: the port's change has its own; the come-up's last line
   * says how it ended and each step's time from it.) */
}

static u32 bring_ms(void)
{
  return ticks_us(nspire_ticks() - bring.at) / 1000;
}

/* The message kept up: shown if nothing is on screen, kept if it is the
 * one shown, another progress message ("...") taken over, any other left
 * to run its two seconds (then this one again). */
static void bring_keep(void)
{
  const char *vis = nspire_osd_visible();
  size_t n;

  if (!bring.text[0])
    return;
  if (!vis)
  {
    nspire_osd_message("%s", bring.text);
    return;
  }
  n = strlen(vis);
  if (!strcmp(vis, bring.text) || (n > 3 && !strcmp(vis + n - 3, "...") && n < sizeof(bring.text)))
  {
    if (strcmp(vis, bring.text))
      strcpy(bring.text, vis);
    nspire_osd_extend();
  }
}

/* The come-up's last line: how it ended, when (ms after the port's change or
 * the pick), each step's time (-1: not seen) and the OS's sleeps meanwhile. */
static void bring_end(const char *how)
{
  /* Nothing shown and nothing of the link's seen (a pad the OS offered at
   * once, a drive): no line. */
  if (bring.text[0] || bring.seen || state != LINK_OFF)
    nspire_link_log("come-up: %s %u ms after the %s; node listed %d, this side's channel %d, the other's (its knock) %d, "
                  "ready %d ms; OS sleeps +%u", how, (unsigned)bring_ms(), bring.port_change ? "USB port's change" : "pick",
                  bring.seen & 1 ? (int)bring.seen_ms[0] : -1, bring.seen & 2 ? (int)bring.seen_ms[1] : -1,
                  bring.seen & 4 ? (int)bring.seen_ms[2] : -1, bring.seen & 8 ? (int)bring.seen_ms[3] : -1,
                  (unsigned)(nspire_os_sleeps() - bring.sleeps));
  bring_stop();
}

/* Each frame start (in a game) after the watch and the arbiter. */
static void bring_frame(void)
{
  unsigned progress, i;
  u32 ms;
  enum arb_owner owner;
  int empty;

  if (!bring.on)
    return;
  ms = bring_ms();
  progress = state != LINK_OFF || watched_frame ? link_cable_progress() : 0;
  /* The steps' times, for the come-up's last line (one line, not one a
   * step: the 12 KB log keeps the link's last sessions only). A step first
   * seen with the link up counts at the link's frame. */
  for (i = 0; i < 4; i++)
    if ((progress & (1u << i)) && !(bring.seen & (1u << i)))
    {
      bring.seen |= 1u << i;
      bring.seen_ms[i] = ms;
    }
  if (state == LINK_LINKED)
  {
    bring_end("linked");
    return;
  }
  owner = nspire_arb_owner();
  /* (The observer's state, not another register read each frame: S8's budget.) */
  empty = !(usb_bits >> 28) && nspire_usbport_state() == PORT_EMPTY;
  if (empty || owner == ARB_PAD || owner == ARB_OTHER || nspire_arb_stage() == ARB_STAGE_GAVE_UP ||
      nspire_usbhub_present())
  {
    /* (hub: "USB device in: connecting..." stayed up to 30 s over an empty hub) */
    bring_end(empty ? "over: the port is empty" : owner == ARB_PAD ? "over: a controller has the port" :
              owner == ARB_OTHER ? "over: a drive or the pad driver has the port" :
              nspire_usbhub_present() ? "over: a USB hub is in" : "over: the arbiter gave up");
    return;
  }
  /* The other calculator is listed: said, in place of the plug's message. */
  if (state == LINK_OFF && (bring.seen & LINK_UP_NODE) &&
      (!strcmp(bring.text, BRING_CABLE_TEXT) || !strcmp(bring.text, BRING_DEVICE_TEXT)))
  {
    strcpy(bring.text, BRING_NODE_TEXT);
    nspire_osd_message(BRING_NODE_TEXT);
  }
  if (ms >= BRING_MAX_MS)
  {
    /* Nothing linked, nothing else said why: the cable's end with no node
     * (a PC, or a calculator whose OS never listed us). */
    if (bring.text[0] && !(bring.seen & 1) && usb_is_host(bring.bits) == 0)
      nspire_osd_message(BRING_NO_ANSWER);
    bring_end("not linked");
    return;
  }
  if (!bring.text[0])
  {
    /* The first message: Link picked or linking on its own (their own
     * status is on screen), the cable's end at once where the watch runs,
     * the adapter's end once nobody claimed it for a moment. */
    if (state == LINK_LOOKING)
      snprintf(bring.text, sizeof(bring.text), "%.63s", status);
    else if (!watched_frame)
      return;   /* no link in this game, menus: nothing here will come up */
    else if (bring.seen & LINK_UP_NODE)
      strcpy(bring.text, BRING_NODE_TEXT);   /* (state LINK_OFF here) */
    /* R11w part 2 (2026-11-15): at the cable's plain end with Link not
     * picked, nothing until the other calculator's node is listed: a
     * computer's cable (charging) is that too, and "Link cable in:
     * connecting..." then "No answer on the link cable" was said for it
     * (no message when it isn't its case) */
    else if (usb_is_host(bring.bits) == 0)
      return;
    else if (usb_is_host(bring.bits) == 1 && (nspire_usbport_bits() & 0x100) && owner == ARB_WAIT &&
             ms >= BRING_HOST_QUIET_MS)
      strcpy(bring.text, BRING_DEVICE_TEXT);
    else
      return;
    nspire_link_log("come-up: \"%s\" shown %u ms after the %s", bring.text, (unsigned)ms,
                    bring.port_change ? "USB port's change" : "pick");
  }
  bring_keep();
}

#define FLAP_GAP_MS 8000
#define LINK_FLAP_TEXT "The link cable keeps resetting: swap its ends"
#define FLAPS_TOAST 4
static u32 flap_at, flap_told_at;
static unsigned flaps;
static int flap_told, flap_said;

static int flap_swap_hint(void)
{
  return flap_told && flap_said && ticks_us(nspire_ticks() - flap_told_at) / 1000 < 60000;
}

static void usb_timeline_frame(void)
{
  u32 bits = nspire_usb_bits(), now;
  int run = nspire_usb_running();

  if (run > 0)
    usb_ran = 1;
  if (bits >> 28 && (bits & 0x400))
    usb_enabled = 1;
  if (bits == usb_bits)
    return;
  now = nspire_ticks();
  /* R2 (cx2-gpsp_link_prev, 2026-11-10): the CX CAS as the host never
   * enumerated, and this port went 20000031 <-> 00000000 every 1.5 s (a
   * device that is reset again and again): the cable's ends are the wrong
   * way round, and nothing said so. Four such changes within a few seconds
   * of each other: a toast (and a log line), again after 30 s. */
  if (usb_timeline && (bits == 0 || usb_bits == 0) && !nspire_arb_cycling())   /* not our own power cycle's */
  {
    if (ticks_us(now - flap_at) / 1000 > FLAP_GAP_MS)
      flaps = 0;
    flap_at = now;
    /* R11w part 2: said only for R2's case, the cable's plain end with
     * something on it flapping to nothing (a calculator host on the other
     * end that never enumerates): an adapter pulled and put back (the
     * adapter's end: a controller user's), or a CX II's empty port that its
     * OS switches off and on, flaps the same way and had nothing to do with
     * a cable's ends (with controllers it said to switch the cable's
     * ends; R11w had Link mode on with controllers); logged either
     * way */
    if (++flaps >= FLAPS_TOAST && (!flap_told || ticks_us(now - flap_told_at) / 1000 >= 30000) &&
        !nspire_usbpad_saw_device())
    {
      u32 on = bits ? bits : usb_bits;
      int empty = ((on >> 28) == 2 && (on & 0x33u) == 0x31u) || ((on >> 28) == 1 && (on & 0x1Fu) == 0x11u);

      flap_told = 1;
      flap_told_at = now;
      flap_said = (on >> 28) && usb_is_host(on) == 0 && !empty;
      nspire_link_log("USB port flapping (%u changes in a row, the last %08x <-> %08x): ends of the cable the wrong way "
                      "round?%s", flaps, (unsigned)bits, (unsigned)usb_bits,
                      flap_said ? "" : " (not said: not the cable's plain end with a device on it)");
      if (flap_said)
      {
        if (bring_running())
          bring_end("the port keeps resetting");
        nspire_osd_message(LINK_FLAP_TEXT);
      }
    }
  }
  /* Per change: did the OS get time since the last one (sleeps), is the
   * host controller running, is the port enabled (R1: a host's OS that
   * never gets ticks never enables the port). */
  if (usb_timeline)
    nspire_link_log("USB port: %08x (was %08x, %u ms before); %u OS sleeps (+%u); controller %s, port %s; %s",
                    (unsigned)bits, (unsigned)usb_bits, ticks_us(now - usb_changed_at) / 1000,
                    (unsigned)nspire_os_sleeps(), (unsigned)(nspire_os_sleeps() - usb_changed_sleeps),
                    run > 0 ? "running" : run == 0 ? "stopped" : "unreadable",
                    bits & 0x400 ? "enabled" : "not enabled", nspire_usb_role_text(bits));
  else if (nspire_usb_plugged())
  {
    /* (Nothing on the port at the start: no line, so a game played
     * without the cable leaves the last run's log as it is.) */
    char line[LOG_LINE];
    nspire_os_diag_usb(line, sizeof(line));
    nspire_link_log("USB port: %08x at the start; %s; %s", (unsigned)bits, nspire_usb_role_text(bits), line);
  }
  /* DETECT: something came on the port (a plug-in, or its controller came
   * up: r5's CX II, 2000010e -> 2000050e): the watch looks at once and
   * then often, and the user is told while it comes up (bring_*). Not our
   * own power cycle's changes, not while linked. */
  if (state != LINK_LINKED && (bits >> 28) && !nspire_arb_cycling())
  {
    link_cable_hurry();
    /* A new come-up: from an empty port (not a CX's gate, whose bits read 0
     * too: the arbiter's session goes on through it), or a host's end that
     * just got enabled with none running (r5's CX II). */
    if (usb_was_empty || (!bring_running() && (bits & 0x400) && !(usb_bits_on & 0x400)))
      bring_start(bits, 1);
  }
  if (bits >> 28)
  {
    usb_was_empty = 0;
    usb_bits_on = bits;
  }
  else if (nspire_usb_plugged() == 0)
    usb_was_empty = 1;
  usb_timeline = 1;
  usb_bits = bits;
  usb_changed_at = now;
  usb_changed_sleeps = nspire_os_sleeps();
  usb_ran = run > 0;
  usb_enabled = (bits >> 28) && (bits & 0x400);
  usb_node_seen = usb_connected = 0;
  node_told = 0;
}

/* What a watch saw, after a change of the USB port: the first node, the
 * first connection, how long after it and after how many OS sleeps. */
static void usb_timeline_seen(int seen)
{
  if (!usb_timeline || seen <= 0 || (seen == 1 && usb_node_seen) || (seen == 2 && usb_connected))
    return;
  nspire_link_log("NavNet: %s %u ms after the USB port's change, %u OS sleeps since",
                  seen == 2 ? "connected" : "a node seen", ticks_us(nspire_ticks() - usb_changed_at) / 1000,
                  (unsigned)(nspire_os_sleeps() - usb_changed_sleeps));
  usb_node_seen = 1;
  usb_node_at = nspire_ticks();
  if (seen == 2)
    usb_connected = 1;
}

/* S7: a NavNet node is on the cable but our service isn't answering it for
 * NODE_HINT_MS: the other calculator is on, gpSP isn't running there. One
 * toast per port change. (The old 5 s / 10 s orientation hints are the
 * arbiter's last-resort toast now: nspire_arbiter.c, stage 4.) */
#define NODE_HINT_MS 10000
static void node_hint(int seen)
{
  if (seen != 1 || !usb_node_seen || usb_connected || node_told || auto_held)
    return;
  if (ticks_us(nspire_ticks() - usb_node_at) / 1000 < NODE_HINT_MS)
    return;
  node_told = 1;
  if (bring_running())
    bring_end("a node without gpSP's service: the hint");   /* DETECT: the last word on this come-up */
  nspire_osd_message("Other calculator found: start gpSP there");
}

/* SEAM-2/3: the watch's NavNet looks, for the arbiter's power cycle (a
 * cycle drops the bus under a calculator whose node isn't listed yet). The
 * real logs: a CX at the device end connected 570-578 ms after its port
 * changed (r2-r4), a CX II's node 247 ms after Link was picked (R2); none
 * has a hot plug at a CX II host end, so the margin is a look's period
 * (WATCH_LOOK_MS 1.5 s) over 1 s: the last of two looks at least
 * CYCLE_LOOK_MS after the change, then one more look just before.
 * DETECT (2026-11-11): the looks are every 250 ms after a port change now,
 * which would have let the cycle come at 2.5 s and after two looks 250 ms
 * apart; the guard keeps its old timing instead (the first cycle ~3.0 s
 * after the change, frontend_tests): the last look at least 2.75 s after
 * the change (the 1.5 s looks put it at 3.0 s before) and the watch
 * looking at least CYCLE_WATCH_MS (one old look period). r5 (the
 * first hot plug at a CX II host end) listed the node ~5-6 s after its
 * port came up: what held the cycle off there was SEAM-8, the OS's offer
 * to our driver 215 ms after ("no power cycle yet: the OS offered the
 * device to a driver", cx2-gpsp_link.txt 116497). */
#define CYCLE_LOOK_MS  2750
#define CYCLE_WATCH_MS 1500
static u32 look_t[2];          /* the last two looks' ticks, [0] the latest */
static unsigned looks_noted;

const char *nspire_link_cycle_veto(uint32_t change_at, int look)
{
  unsigned nodes;

  if (state != LINK_OFF || cable_seen > 0)
    return "the link has the cable";
  if (!watched_frame)
    return "the link's watch isn't running (menus, no link in this game, or an OS without NavNet)";
  if (looks_noted < 2 || (s32)(look_t[1] - change_at) < 0 ||
      ticks_us(look_t[0] - change_at) / 1000 < CYCLE_LOOK_MS ||
      ticks_us(look_t[0] - watch_from) / 1000 < CYCLE_WATCH_MS)
    return "the link's watch hasn't looked long enough since the port changed";
  if (!look)
    return 0;
  nodes = link_cable_look_now();
  if (nodes)
  {
    cable_seen = 1;   /* the arbiter's LINK from now */
    return "a fresh NavNet look lists a node (a calculator)";
  }
  return 0;
}

/* R9 (PAD3): the Controller page's power restart by hand asks this
 * first: 1 when the device may be the other calculator (the link has the
 * cable, or a fresh NavNet look lists a node). Not the timing guards of
 * nspire_link_cycle_veto: the page is in the menus, where the watch never
 * runs; the player picked it. A NavNet that isn't open lists nothing. */
int nspire_link_calc_on_cable(void)
{
  if (state != LINK_OFF || cable_seen > 0)
    return 1;
  if (link_cable_look_now())
  {
    cable_seen = 1;   /* the arbiter's LINK from now */
    return 1;
  }
  return 0;
}

/* LINK2: a host's end whose controller never ran (r5 and r6, the CX II with
 * the adapter, Link picked: "host controller NEVER RAN (stopped); port NEVER
 * ENABLED", PORTSC 00000803/0000080b: connected, a connect change pending,
 * not enabled; USBCMD 00080b00: stopped; OTGCSR 000e0010: the A end, VBUS
 * on; 29-38 s, its OS given +1000 ticks). Both times it came up 46-53 s
 * after the plug, 0.4 s after the CX CAS's end blinked (its PORTSC1 attach
 * bit off ~430 ms, VBUS on all along: that OS let go of the bus and
 * attached again): a fresh connect is what the CX II's OS acts on, the
 * ticks aren't. So: Link picked and stuck so (the pick STUCK_PICK_MS ago,
 * the port's change STUCK_PORT_MS ago), one bus power cycle (the arbiter's
 * S4: VBUS off 300 ms and on, the device detaches and attaches again); if
 * the controller still hasn't run STUCK_CYCLE_MS later, or no cycle can be
 * made, the screen says to replug instead of "Looking..." for 30 s. A
 * replug (a port change) lets it start over. */
#define STUCK_PICK_MS  1500
#define STUCK_PORT_MS  2500
#define STUCK_CYCLE_MS 3000
#define STUCK_HINT     "Unplug the link cable and plug it in again"

static void stuck_tell(const char *why)
{
  char line[LOG_LINE];

  stuck_step = 2;
  stuck_change = usb_changed_at;
  nspire_os_diag_usb(line, sizeof(line));
  nspire_link_log("come-up: the host controller still hasn't run (%s): told to replug; %s", why, line);
  nspire_link_log_request_flush();
  set_status(STUCK_HINT);
  if (bring_running())
    snprintf(bring.text, sizeof(bring.text), "%s", STUCK_HINT);
  nspire_osd_message(STUCK_HINT);
}

static void stuck_host_frame(void)
{
  int stuck;

  if (state != LINK_LOOKING)
  {
    stuck_step = 0;
    return;
  }
  /* A replug after the hint: from the start (the arbiter's cycles are the session's). */
  if (stuck_step == 2 && usb_changed_at != stuck_change && !nspire_arb_cycling())
    stuck_step = 0;
  stuck = usb_is_host(usb_bits) == 1 && (usb_bits & 0x100) && !(usb_bits & 0x400) && nspire_usb_running() == 0;
  if (!stuck)
    return;
  if (stuck_step == 0)
  {
    char line[LOG_LINE];
    int r;

    if (ticks_us(nspire_ticks() - looking_at) / 1000 < STUCK_PICK_MS ||
        ticks_us(nspire_ticks() - usb_changed_at) / 1000 < STUCK_PORT_MS)
      return;
    nspire_os_diag_usb(line, sizeof(line));
    nspire_link_log("come-up: a device on the port %u ms, the host controller never ran (Link %u ms ago, OS sleeps +%u): "
                    "a bus power cycle; %s", ticks_us(nspire_ticks() - usb_changed_at) / 1000,
                    ticks_us(nspire_ticks() - looking_at) / 1000, (unsigned)(nspire_os_sleeps() - usb_changed_sleeps), line);
    r = nspire_arb_link_cycle();
    if (r > 0)
    {
      stuck_step = 1;
      stuck_at = nspire_ticks();
      link_cable_hurry();   /* the node may come soon now: looked for often */
    }
    else
      stuck_tell(r == 0 ? "the power cycle's VBUS didn't come back" : r == -3 ? "no power cycle left this session" :
                 r == -1 ? "nothing connected to power cycle any more" : "no power cycle can be made here");
  }
  else if (stuck_step == 1 && ticks_us(nspire_ticks() - stuck_at) / 1000 >= STUCK_CYCLE_MS)
    stuck_tell("a power cycle made no difference");
}

/* No link up: is the other calculator's gpSP there? Then link. Only with
 * something on the USB port (or a port that can't tell), and a game with
 * a link in gpSP. 1 while watching (the OS wanted then). */
static int watch_cable(void)
{
  int seen, plugged, gated;
  unsigned looks;

  /* The gamepad's own driver has the port: nothing here touches it. */
  if (nspire_usbpad_owns_usb())
  {
    cable_seen = 0;
    return 0;
  }
  plugged = nspire_usb_plugged();
  /* A CX's controller with its bus access off reads "can't tell", but its
   * OS does that only with nothing on the port: as nothing, but with a
   * tick now and then (link_cable_gated), not a watch's looks and spare
   * time in every game. */
  gated = plugged < 0 && nspire_usb_gated();
  /* The setting's device, as linking would plug in: none (the Game Boy
   * Player's games on Auto), or nothing on the port: no watching, and
   * what watching held is let go. */
  /* 2026-11-16: nor with a USB hub in (no link through one: its devices are
   * the drivers'), a device the OS offered that is neither a calculator nor
   * anything gpSP uses (headphones the sound driver can't play, a keyboard),
   * or the OS's USB found stopped: the watch's looks and its OS tick every
   * 100 ms went on for as long as such a thing stayed in. */
  if (nspire_arb_link_blocked() || !plugged || gated || !linkable(device_mode()) || nspire_usbhub_present() ||
      (nspire_usbpad_other_device && nspire_usbpad_other_device()) ||
      (nspire_usbpad_os_stuck && nspire_usbpad_os_stuck()))
  {
    link_cable_unwatch();
    cable_seen = 0;
    if (!plugged || gated)
    {
      /* The cable's out: the next time it's in, link, at once. */
      auto_held = 0;
      auto_retry_at = nspire_ticks();
    }
    if (gated && !nspire_arb_link_blocked() && linkable(device_mode()) &&
        link_cable_gated() < 0)
      auto_off = 1;
    return 0;
  }
  looks = link_cable_looks();
  seen = link_cable_watch(!auto_held && (s32)(nspire_ticks() - auto_retry_at) >= 0, plugged,
                          !usb_timeline || nspire_ticks() - usb_changed_at < (u32)PORT_FRESH_MS * (NSPIRE_TICK_HZ / 1000));
  cable_seen = seen;
  if (seen < 0)
  {
    auto_off = 1;   /* this OS can't: the Link line says why when picked */
    return 0;
  }
  /* DETECT: the watch's run since (a gap of half a second: menus, the file
   * list, a game without it: a new run). */
  if (ticks_us(nspire_ticks() - watched_at) / 1000 > 500)
    watch_from = nspire_ticks();
  watched_at = nspire_ticks();
  watched_frame = 1;
  if (link_cable_looks() != looks)
  {
    look_t[1] = look_t[0];
    look_t[0] = nspire_ticks();
    if (looks_noted < 2)
      looks_noted++;
  }
  /* SEAM-8: what the OS lists, for the arbiter (forgotten at a port change). */
  if (seen > 0)
    nspire_usbport_os_report(seen == 2 ? PORT_OS_NODE_SERVICE : PORT_OS_NODE_NO_SERVICE, 0, 0);
  usb_timeline_seen(seen);
  node_hint(seen);
  if (auto_held)
  {
    /* Held until nothing's on the cable (it came out: the next time links)
     * or the other side connects anew (it picked Link, or its gpSP started
     * again: link now). */
    if (seen == 1)
      return 1;
    auto_held = 0;
    if (seen == 0)
      return 1;
  }
  if (seen == 2)
    start_link(1);
  return 1;
}

/* The log while a pad or the link is live (a power-off or hang loses
 * what's still in memory): new lines are written at most every
 * LOG_LIVE_FLUSH_MS (a write rewrites the file, up to 16 KB: ~0.5 ms a KB
 * on a CX II, 8 ms at worst, once in 2 s only when something's new), and
 * with no link, a heartbeat line every LOG_LIVE_BEAT_MS stands in for the
 * linked frames' (a pad's, the backlight, the OS's sleeps). Checked only
 * once per LOG_LIVE_FLUSH_MS: a tick compare per frame. */
#define LOG_LIVE_FLUSH_MS 2000
#define LOG_LIVE_BEAT_MS  10000
#define LOG_LIVE_MAX      6
static u32 live_checked_at;
static unsigned live_writes;
/* For the host tests: how many writes the live policy made. */
unsigned nspire_link_log_live_writes(void) { return live_writes; }
static void heartbeat(void);

static void log_live_policy(void)
{
  u32 now = nspire_ticks();

  /* Never while linked: a write there is the game's time, which the other
   * side waits out (the link keeps its HEARTBEAT_WRITE throttle). And only
   * while something may be diagnosed: LOG_LIVE_MAX writes a run, then the
   * events' own flushes only (a pad plugged for hours isn't rewritten
   * every 10 s). */
  if (state != LINK_OFF || live_writes >= LOG_LIVE_MAX || !log_len ||
      ticks_us(now - live_checked_at) / 1000 < LOG_LIVE_FLUSH_MS)
    return;
  /* Nor while a link is pending: the other gpSP's cable is seen (linking
   * starts any frame, and a write before it is the other side's wait), or
   * a link just ended (it is retried; a stall under load broke one in the
   * host test, and the write after it was the retry's wait). */
  if (cable_seen > 0 ||
      (link_ever_ended && ticks_us(now - link_ended_at) / 1000 < LOG_LIVE_BEAT_MS))
    return;
  live_checked_at = now;
  if (nspire_usb_plugged() == 1 && ticks_us(now - heartbeat_at) / 1000 >= LOG_LIVE_BEAT_MS)
    heartbeat();
  /* A heartbeat alone (nothing changed) stays in the ring for the next write. */
  if (log_new || log_flush_wanted)
  {
    live_writes++;
    nspire_link_log_flush();
  }
}

/* R3 (PAD3): the pad log's writes keep to the same rule as the
 * live writes above: the link off, the other gpSP's cable not seen, no link
 * ended within a beat (its retry's come-up). */
int nspire_link_quiet_for_writes(void)
{
  return state == LINK_OFF && cable_seen <= 0 &&
         !(link_ever_ended && ticks_us(nspire_ticks() - link_ended_at) / 1000 < LOG_LIVE_BEAT_MS);
}

void nspire_link_frame_begin(void)
{
  s32 need;
  unsigned waited = 0;

  /* R3: a watchdog the OS turned on (it resets the calculator "like the
   * reset button", as the CX II did twice, linked only) is turned off, as
   * Ndless does at a program's start; said once (and counted each second). */
  if ((nspire_os_watchdog_off() || nspire_os_watchdog_offs()) && !watchdog_logged)
  {
    char wd[160];
    watchdog_logged = 1;
    nspire_os_diag_watch(wd, sizeof(wd));
    nspire_link_log("watchdog: the OS had turned it on: turned off (%s)", wd);
  }
  /* Here, between frames, the log can be written (end_link's wish). */
  if (log_flush_wanted)
  {
    if (state == LINK_LINKED)
      recent_add("log file write (frame %u)", (unsigned)my_frame);
    nspire_link_log_flush();
  }
  else
    log_live_policy();
  nspire_padlog_write_point();   /* PAD3: the pad log's own file (never while linked) */
  if (state == LINK_LINKED && link_cable_fell_back())
    remeasure_start();
  if (state != LINK_LINKED)
    alone_frames++;
  if (read_ioreg(REG_P1) != keys_last)
  {
    keys_last = read_ioreg(REG_P1);
    keys_at = nspire_ticks();
  }
  nspire_usbport_poll();
  watched_frame = 0;
  if (!nspire_usbpad_owns_usb())
    usb_timeline_frame();
  if (state == LINK_OFF)
    nspire_os_want(OS_WANT_LINK, !auto_off && watch_cable());
  else
    /* LINK2 (r6 logs): a linked frame's spare time went to the OS's tick
     * (nspire_wait_until) also over the fast transport, where the link
     * needs none; on the CX CAS each such tick came back after 50-363 ms
     * (1.6 a second), its frames 45-46 a second without one, 30 with. */
    nspire_os_want(OS_WANT_LINK, state != LINK_LINKED || link_cable_linked_os());
  /* After the watch (SEAM-2: what it saw this frame counts): the only place
   * the arbiter may cycle the bus power (SEAM-12: between frames). */
  nspire_arb_frame();
  if (!nspire_usbpad_owns_usb())
    stuck_host_frame();   /* LINK2: a host's end that never ran: a power cycle, then the replug hint */
  bring_frame();   /* DETECT: the plug-in's messages and steps */
  if (state == LINK_LOOKING)
  {
    /* The game goes on until the cable's there; then both say hello and
     * wait for each other, so each links at the frame it asked to. */
    int ready = link_cable_ready();
    if (ready < 0)
    {
      end_link(link_cable_error() ? link_cable_error() : "Couldn't link.");
      return;
    }
    if (!ready)
    {
      /* What the transport tried, while it's looking (it logs changes). */
      nspire_link_log_flush();
      return;
    }
    if (!hello_sent)
    {
      struct side me = my_side();
      send_hello(MSG_HELLO_EXT, legacy_mode(&me));
      auto_hello_at = nspire_ticks();
    }
    if (auto_link)
    {
      /* On its own: the other's hello is taken as it comes, the game going
       * on meanwhile (the other may never send one), up to AUTO_HELLO_MS. */
      while (state == LINK_LOOKING && receive(0))
        ;
      if (state == LINK_LOOKING)
      {
        if (ticks_us(nspire_ticks() - auto_hello_at) / 1000 >= AUTO_HELLO_MS)
        {
          send_head(MSG_BYE, 0, 0);
          end_link("Cable in: the other calculator didn't link");
        }
        return;
      }
    }
    while (state == LINK_LOOKING)
      if (!receive(WAIT_SLICE_MS))
        waited_more(&waited);
    waited = 0;
    /* The fast transport, if both can (nspire_link_fast.c): before the
     * pings, so they measure what the link will run on. */
    if (state == LINK_LINKED)
    {
      const char *off = nspire_link_fast_off_why();
      if (off)
        nspire_link_log("fast transport: not used, NavNet carries the link: this side: %s", off);
      else
      {
        /* The marker first (nspire_link_speed.c): a reset from here on
         * turns the fast transport off for the next runs. */
        const char *refused = NULL;
        if (other_fast && !nspire_link_fast_begin())
          refused = "the crash marker could not be written";
        /* The arbiter is the port's one owner: the controller is taken
         * only with its yes (owner LINK, no power cycle, no pad), asked
         * afresh now; each of its ticks after can make it let go. */
        nspire_arb_tick();
        if (!refused)
          refused = nspire_arb_fast_refusal();
        if (!link_cable_go_fast(other_fast, refused))
          nspire_link_fast_end();
      }
    }
    if (state == LINK_LINKED && protocol >= 2)
      ping_phase();
    if (state == LINK_LINKED && protocol >= 3)
      delay_phase();
    keep_linked = state == LINK_LINKED;
    last_flushed_at = nspire_ticks();
    nspire_link_log_flush();
  }
  if (state != LINK_LINKED)
    return;

  /* On the cable, player 2 waits mid-frame instead (cable_update), and
   * takes messages only there. */
  if (serial_mode == SERIAL_MODE_CABLE && netplay_client_id)
    return;
  need = (s32)my_frame - (serial_mode == SERIAL_MODE_CABLE ? CABLE_LEAD : (s32)link_delay);
  wait_until_other(need);
  if (state == LINK_LINKED && serial_mode != SERIAL_MODE_CABLE)
    deliver(need);
  /* A safe point: the other side has given more frames than this needs, so
   * a small write here is nobody's wait (LAST_SLACK frames, or after two
   * times the interval whatever, as a crash's last seconds matter more). */
  if (state == LINK_LINKED)
  {
    unsigned since = ticks_us(nspire_ticks() - last_flushed_at) / 1000, every = last_flush_ms();
    if (since >= every && (other_done - need >= LAST_SLACK || since >= 2 * every))
      last_flush();
  }
}

/* Every HEARTBEAT_FRAMES linked frames, into the heartbeat's ring: the
 * frames and their time, the real-time clock, the OS's sleeps, the last
 * key gpSP saw (the OS powers down after 3 minutes without one, by
 * default), the transport's work, the USB port's and the screen's state.
 * Written when those changed, and every HEARTBEAT_WRITE heartbeats. */
static void heartbeat(void)
{
  char line[LOG_LINE], usb[LOG_LINE], screen[LOG_LINE];
  u32 now = nspire_ticks(), us = ticks_us(now - heartbeat_at), bits = nspire_usb_bits();
  int changed, every;
  const char *irq, *lcd_from;
  size_t lcd;

  link_cable_stats(line, sizeof(line));
  beat_log("heartbeat: %u frames in %u.%u s (delay %u), %u alone; clock %u s; OS sleeps %u; last key %u s ago; frame %u, "
           "other's %d, %u B of the other's packets queued",
           (unsigned)heartbeat_frames, us / 1000000, us / 100000 % 10, (unsigned)link_delay, (unsigned)alone_frames,
           (unsigned)nspire_rtc_seconds(), (unsigned)nspire_os_sleeps(),
           ticks_us(now - keys_at) / 1000000, (unsigned)my_frame, (int)other_done,
           (unsigned)(queue_tail - queue_head));
  /* (The transport's own line is long: the first lines of the old ones were cut at "its tas"
   * and the service's stack went unseen.) */
  beat_log("  transport: %s", line);
  recent_add("heartbeat: %u frames in %u.%u s; OS sleeps %u; %s", (unsigned)heartbeat_frames, us / 1000000,
             us / 100000 % 10, (unsigned)nspire_os_sleeps(), line);
  nspire_os_diag_usb(usb, sizeof(usb));
  nspire_os_diag_screen(screen, sizeof(screen));
  /* A change is the USB port's role and session bits (nspire_usb_bits) or
   * the LCD's words (the screen line up to the interrupts): the rest of
   * both lines (the USB's status, the timers, the clock) moves on its own. */
  /* R3: not the LCD's base, which gpSP's two screens swap (R2's heartbeats
   * were each "changed" by it: a 16 KB write every 10 s while linked). */
  irq = strstr(screen, "; IRQ");
  lcd = irq ? (size_t)(irq - screen) : strlen(screen);
  lcd_from = strstr(screen, "control");
  if (!lcd_from || lcd_from >= screen + lcd)
    lcd_from = screen;
  lcd -= (size_t)(lcd_from - screen);
  changed = bits != beat_usb_bits || strlen(beat_lcd) != lcd || strncmp(lcd_from, beat_lcd, lcd);
  every = ++heartbeats % HEARTBEAT_WRITE == 0;
  if (state == LINK_OFF)
    every = 0;   /* no link: written when something changed (log_live_policy) */
  beat_log("  %s", usb);
  beat_log("  %s", screen);
  {
    char wd[160];
    nspire_os_diag_watch(wd, sizeof(wd));
    beat_log("  %s", wd);
  }
  {
    /* R12: a CX II's time base now (the speed in use, its refinements) */
    char tb[LOG_LINE - 120];
    if (nspire_timer_header_text(1, tb, sizeof(tb)))
      beat_log("  %s", tb);
  }
  {
    char pad[240];

    pad[0] = 0;
    if (nspire_usbpad_heartbeat(pad, sizeof(pad)) && pad[0])
      beat_log("  %s", pad);
  }
  beat_usb_bits = bits;
  snprintf(beat_lcd, sizeof(beat_lcd), "%.*s", (int)lcd, lcd_from);
  if (changed || every)
    log_flush_wanted = 1;
  heartbeat_frames = alone_frames = 0;
  heartbeat_at = now;
}

void nspire_link_frame_end(void)
{
  if (state != LINK_LINKED)
    return;
  if (serial_mode == SERIAL_MODE_CABLE)
  {
    /* This side's time: as far as player 2 may go (player 1's, unless a
     * halt told it already; player 2's frame is what player 1 needs). */
    put64(out_msg + HEAD, cable_clock);
    out_len = HEAD + 8;
    halt_seen = 0;   /* the next frame's halt goes further */
  }
  if (serial_mode != SERIAL_MODE_CABLE || netplay_client_id || cable_clock > told)
  {
    out_msg[0] = MSG_FRAME;
    out_msg[1] = FRAME_END;
    put16(out_msg + 2, out_len - HEAD);
    put32(out_msg + 4, my_frame);
    if (send_msg(out_msg, out_len) < 0)
    {
      link_broke("The link cable came out.");
      return;
    }
    if (serial_mode == SERIAL_MODE_CABLE && !netplay_client_id)
      told = cable_clock;
  }
  out_len = HEAD;
  my_frame++;
  linked_frames++;
  if (!skip_next_frame)
    second_drawn++;
  if (++heartbeat_frames >= HEARTBEAT_FRAMES || ticks_us(nspire_ticks() - heartbeat_at) / 1000 >= HEARTBEAT_MS)
    heartbeat();
  if (ticks_us(nspire_ticks() - recent_second_at) / 1000 >= 1000)
  {
    recent_second_at = nspire_ticks();
    char svc[200], wd[160];
    unsigned sleeps = (unsigned)nspire_os_sleeps();
    uint32_t busy;
    nspire_os_busy_slept(&busy);
    /* R3: and what the OS did in gpSP's time this second (its sleeps, the
     * longest: a tick is 10 ms, more is the OS busy), the service's task,
     * the watchdog, the backlight, the busy cursor, the interrupts. */
    link_cable_second(svc, sizeof(svc));
    nspire_os_diag_watch(wd, sizeof(wd));
    /* R3 (R2: the CX II's frames were 10 bytes each the whole session: no
     * transfer of the game's ever went out on it): the emulated serial port
     * (SIOCNT, RCNT, the Pokemon protocol's peers' states and offset) and
     * the game's packets sent and delivered this second. */
    {
      char em[120];
      emerald_vars(em, sizeof(em));
      /* R4: and the game's serial writes, transfers started and serial
       * events this second (sio_writes_second; serial.c's counters), the
       * writes not logged, and Emerald's link variables. */
      recent_add("frame %u, other's %d, delay %u, queued %u B, %s; sio %04x rcnt %04x mode %u poke %05x, packets %u out "
                 "%u in, sio writes %u (%u unlogged), transfers %u, IRQs %u%s; drawn %u; OS sleeps %u (longest %u ms, %u idle); "
                 "%s; %s",
                 (unsigned)my_frame, (int)other_done, (unsigned)link_delay, (unsigned)(queue_tail - queue_head),
                 other_in_menu ? "menu" : "game", (unsigned)read_ioreg(REG_SIOCNT), (unsigned)read_ioreg(REG_RCNT),
                 (unsigned)serial_mode, (unsigned)serialpoke_states(), packets_out, packets_in, sio_writes_second,
                 sio_unlogged, (unsigned)(serial_transfer_starts - sio_starts_at),
                 (unsigned)(serial_irqs_raised - sio_irqs_at), em, (unsigned)second_drawn, sleeps - second_sleeps,
                 ticks_us(nspire_os_sleep_longest()) / 1000, (sleeps - second_sleeps) - (unsigned)(busy - second_busy), svc, wd);
    }
    packets_out = packets_in = 0;
    sio_writes_second = sio_logged_second = sio_unlogged = 0;
    sio_starts_at = serial_transfer_starts;
    sio_irqs_at = serial_irqs_raised;
    second_sleeps = sleeps;
    second_busy = busy;
    second_drawn = 0;
  }
  /* This frame's end goes out within half the frames the other side has
   * to spare (it waits for this side's frame need frames back). */
  {
    u32 lead = serial_mode == SERIAL_MODE_CABLE ? CABLE_LEAD : link_delay;
    link_cable_frame_done(lead > 2 ? lead / 2 : 1);
  }
  /* Take what's there already, so the cable never backs up (player 2 on
   * the cable takes each where it stops instead, the same cycles each run). */
  if (serial_mode != SERIAL_MODE_CABLE || !netplay_client_id)
    while (state == LINK_LINKED && receive(0))
      ;
}

void nspire_link_menu(int open)
{
  if (state == LINK_LINKED)
  {
    send_head(MSG_MENU, open ? 1 : 0, my_frame);
    link_cable_push();   /* LNK-2: the menu gives the OS's task no time to send it */
  }
}

/* ---- The core's side (libretro's netpacket interface) ---- */

void netpacket_send(uint16_t client_id, const void *buf, size_t len)
{
  (void)client_id;   /* two players: it's for the other one */
  if (state != LINK_LINKED || len + 2 > MSG_MAX - HEAD)
    return;
  if (out_len + 2 + len > MSG_MAX)
  {
    /* A full message before the frame's end (not its end). */
    out_msg[0] = MSG_FRAME;
    out_msg[1] = 0;
    put16(out_msg + 2, out_len - HEAD);
    put32(out_msg + 4, my_frame);
    send_msg(out_msg, out_len);
    out_len = HEAD;
  }
  put16(out_msg + out_len, len);
  memcpy(out_msg + out_len + 2, buf, len);
  out_len += 2 + len;
  packets_out++;
}

/* The core asks for packets mid-frame to answer sooner; here they come at
 * frames' starts only (the same each run), so there's nothing to do. */
void netpacket_poll_receive(void) {}

/* ---- The link cable (serial.c's SERIAL_MODE_CABLE) ---- */

static void send_words(int type, u64 at, u32 time, u32 word)
{
  u8 msg[HEAD + 16];
  msg[0] = type;
  msg[1] = 0;
  put32(msg + 4, my_frame);
  if (type == MSG_XFER)
  {
    put16(msg + 2, 16);
    put64(msg + HEAD, at);
    put32(msg + HEAD + 8, time);
    put32(msg + HEAD + 12, word);
    send_msg(msg, HEAD + 16);
  }
  else
  {
    put16(msg + 2, 4);
    put32(msg + HEAD, word);
    send_msg(msg, HEAD + 4);
  }
}

/* A transfer's words (at its start, cycle at) into the sum the PC tests
 * compare (nspire_link.h): each GBA's the same at any link speed. */
static void count_transfer(u64 at, u32 word0, u32 word1)
{
  u32 h = ((u32)at * 0x9E3779B1u) ^ ((u32)(at >> 32) * 0x85EBCA77u) ^
          (((word0 & 0xFFFF) << 16 | (word1 & 0xFFFF)) * 0xC2B2AE3Du);
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  nspire_link_transfer_sum += h ^ (h >> 12);
}

/* Player 1: player 2's word for the transfer it started last, from its
 * reply or promise. */
static void take_reply(void)
{
  unsigned waited = 0;
  u32 start = nspire_ticks();

  if (!reply_got)
  {
    while (state == LINK_LINKED && !reply_got)
      if (!receive(WAIT_SLICE_MS))
        waited_more(&waited);
    count_wait(&wait_reply, nspire_ticks() - start);
  }
  if (reply_got)
  {
    words[1] = reply_word;
    count_transfer(reply_for, words[0], words[1]);
  }
  reply_owed = 0;
}

/* Player 1's time to player 2, mid-frame: it may run to at. */
static void tell_time(u64 at)
{
  u8 msg[HEAD + 8];
  msg[0] = MSG_FRAME;
  msg[1] = 0;
  put16(msg + 2, 8);
  put32(msg + 4, my_frame);
  put64(msg + HEAD, at);
  send_msg(msg, sizeof(msg));
  told = at;
}

/* Player 1's game started a multiplayer transfer, at the cycle this run of
 * cycles started: its word goes to player 2, whose word it needs only at
 * the end (cable_transfer_done), unless a promise has it. Unlinked,
 * nothing's on the cable's other end (0xFFFF, as on the GBA). */
void cable_transfer_start(u16 siocnt, u32 cycles)
{
  /* The last one's word, if it didn't end in multiplayer mode. */
  if (reply_owed)
    take_reply();
  words[0] = read_ioreg(REG_SIOMLT_SEND);
  words[1] = 0xFFFF;
  if (state != LINK_LINKED)
    return;
#ifdef NSPIRE_HOST
  if (cable_clock < told)
    fprintf(stderr, "link: a transfer at cycle %llu, before the %llu told\n",
            (unsigned long long)cable_clock, (unsigned long long)told);
#endif
  reply_got = 0;
  reply_owed = 1;
  reply_for = cable_clock;
  tell_till = cable_clock + CABLE_QUIET;
  use_promise(reply_for);
  nspire_link_transfers++;
  xfer_sent_at = nspire_ticks();
  send_words(MSG_XFER, cable_clock, cycles, words[0]);
  /* Player 2 may run to its end (receive), from protocol 2 on; protocol 1's
   * runs only to its start. */
  told = cable_clock + (protocol >= 2 ? cycles : 0);
}

/* The transfer's end: the words, as each GBA's SIOMULTI0-3; player 1's
 * from player 2 now if they haven't come. */
void cable_transfer_done(void)
{
  if (reply_owed)
    take_reply();
  if (count_owed)
    count_transfer(count_at, words[0], words[1]);
  count_owed = halt_seen = 0;   /* a halt from now on is a new one */
  write_ioreg(REG_SIOMULTI0, words[0]);
  write_ioreg(REG_SIOMULTI1, words[1]);
  write_ioreg(REG_SIOMULTI2, 0xFFFF);
  write_ioreg(REG_SIOMULTI3, 0xFFFF);
}

/* Player 2's word for a transfer now: SIOMLT_SEND, or nothing on this end
 * of the wire (0xFFFF) when it's not in multiplayer mode. */
static int multiplayer(void)
{
  return !(read_ioreg(REG_RCNT) & 0x8000) && (read_ioreg(REG_SIOCNT) & 0x3000) == 0x2000;
}

static u32 my_word(void)
{
  return multiplayer() ? read_ioreg(REG_SIOMLT_SEND) : 0xFFFF;
}

/* Player 2 at player 1's transfer: its word goes back (unless its promise
 * had it), and its own transfer runs from the same cycle. cycles: what
 * update_serial is about to count down, part of it from before the
 * transfer. */
static void give_word(unsigned cycles)
{
  u32 late = (u32)(cable_clock - xfer_at);
  u32 word = my_word();
  int kept = promised && xfer_at < promised_until;

  xfer_pending = promised = halt_seen = 0;
  nspire_link_transfers++;
#ifdef NSPIRE_HOST
  if (kept && word != promised_word)
    fprintf(stderr, "link: promised %04x for the transfer at cycle %llu, gave %04x\n",
            (unsigned)promised_word, (unsigned long long)xfer_at, (unsigned)word);
#endif
  if (!kept)
    send_words(MSG_REPLY, 0, 0, word);
  if (count_owed)   /* the last one didn't end in multiplayer mode */
    count_transfer(count_at, words[0], words[1]);
  count_owed = 0;
  if (!multiplayer())
  {
    count_transfer(xfer_at, xfer_word, word);
    return;
  }
  words[0] = xfer_word;
  words[1] = word;
  count_owed = 1;
  count_at = xfer_at;
  serial_cable_busy(xfer_time + cycles - MIN(late, cycles));
}

/* Player 2's CPU halted with nothing to wake it before until: its word for
 * player 1's next transfer, if that starts before then. Only if it can
 * (the next starts at the horizon or later). */
static void promise(u64 until)
{
  u8 msg[HEAD + 20];
  u64 next = xfer_pending ? xfer_at : horizon;

  if (next >= until)
    return;
  promised = 1;
  promised_until = until;
  promised_word = my_word();
  msg[0] = MSG_PROMISE;
  msg[1] = 0;
  put16(msg + 2, 20);
  put32(msg + 4, my_frame);
  put64(msg + HEAD, cable_clock);
  put64(msg + HEAD + 8, until);
  put32(msg + HEAD + 16, promised_word);
  send_msg(msg, sizeof(msg));
}

void cable_update(unsigned cycles)
{
  unsigned waited = 0;
  u32 start = 0;

  if (state != LINK_LINKED)
    return;
  cable_clock += cycles;
  if (!netplay_client_id)
  {
    if (cable_clock >= told + CABLE_TELL && cable_clock < tell_till)
      tell_time(cable_clock);
    return;
  }
  /* Player 2: never past player 1's time. */
  for (;;)
  {
    if (xfer_pending && cable_clock >= xfer_at)
      give_word(cycles);
    if (state != LINK_LINKED || cable_clock < horizon)
      break;
    if (!start)
      start = nspire_ticks() | 1;
    if (!receive(WAIT_SLICE_MS))
      waited_more(&waited);
    else
      waited = 0;
  }
  if (start)
    count_wait(&wait_horizon, nspire_ticks() - start);
}

/* Between the core's events: a halt just begun is told (player 1: as far
 * as no transfer can start) or promised (player 2), and when player 2 has
 * to stop: at player 1's time or transfer. */
u32 cable_next_event(void)
{
  u64 until;

  if (state != LINK_LINKED)
    return ~0U;
  if (reg[CPU_HALT_STATE] != CPU_HALT)
    halt_seen = 0;
  else if (!halt_seen)
  {
    halt_seen = 1;
    until = cable_clock + halted_cycles(serial_busy_cycles());
    if (netplay_client_id && protocol >= 2)   /* protocol 1 waits for every reply */
      promise(until);
    else if (until > told + CABLE_AHEAD)
      tell_time(until);
  }
  if (!netplay_client_id)
    return ~0U;
  until = xfer_pending && xfer_at < horizon ? xfer_at : horizon;
  if (until <= cable_clock)
    return 1;
  return until - cable_clock > 0x10000000 ? 0x10000000 : (u32)(until - cable_clock);
}
