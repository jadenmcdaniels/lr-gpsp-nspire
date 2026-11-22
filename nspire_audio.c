/* The emulator produces PCM in MAIN. Only the USB task consumes it.
 * Menus and fast forward keep the device clock running with silence;
 * neither a full ring nor a missing adapter can wait on emulation.
 *
 * OS time (2026-11-15, final/snd). The OS
 * re-queues a transfer in its completion callback, which runs only while the
 * CPU takes interrupts: gpSP's frames run with them masked. After the OS
 * ran, (UA_NX - 1) x UA_NF ms are queued ahead of the bus at the least (4
 * transfers of 14 ms, 56 descriptors: 42 ms ahead after the OS ran), so gpSP
 * may stay away AWAY_MS (36 ms fixed, or to Q_MARGIN_MS by ua_queue_ms)
 * before the device hears a gap.
 *  - Round 1 (faf629e -> 26deb46) gave the OS that time in task sleeps
 *    (TCC_Sleep: back at the OS's next tick, CX II 10.25 ms), placed in the
 *    spare time where it could. The CX II's R11u log (2026-11-15): 26-43
 *    sleeps a second costing 140-195 ms of the game's time, 45 or 30
 *    pictures where the game alone draws all 60; and the IRQ window probe
 *    delivered completions in 209 of 215 windows.
 *  - Now: the queue is served in IRQ windows (irq_window: the interrupts on
 *    only until the pending completions have run, 0.3 ms more at most when
 *    none comes), at a frame's end once SERVE_MS has gone by since the OS
 *    last served it, or earlier if the next frame's end would pass the
 *    limit. A window that should have delivered (a transfer has come back
 *    since) and did not is a miss; WIN_MISSES in a row and windows are not
 *    trusted: task sleeps serve the queue as in round 1, and a window is
 *    tried once a second to trust them again. A window that delivers
 *    nothing when the queue needs it now is followed by a sleep at once.
 *  - Task sleeps stay: free ones in the spare time (nspire_wait_until), and
 *    at least one every TASK_MS for the OS's own USB work (a detach, an
 *    attach), placed where it costs least (nspire_audio_os_time's look at
 *    the frames ahead, round 1's rule).
 *  - A device that stops taking packets without a detach (R11u seconds
 *    130-135: no completion for 6 s, the ring full, 247157 frames dropped):
 *    no completion for STALL_MS with the OS served is a stall: the sound is
 *    stopped (nothing produced, nothing dropped) until completions come
 *    again, then it starts clean.
 * The rate (rate_update): at full speed the resampler's factor is 1 plus a
 * slow drift term (the game's clock against the adapter's) within +-0.2%,
 * moving at most 0.01% a frame; the ring's fill is held by that alone. A
 * game below 97% of full speed for two seconds is slow: the factor glides
 * (0.1% a frame) to its speed and follows it slowly; back at 99% for two
 * seconds it glides back. After a menu, a state or fast forward the sound
 * starts clean from the target fill (priming) at the factor it had. Fill
 * far off the target (a stall of gpSP's own: a file write) is not chased by
 * the rate: under a quarter of it twice the sound re-primes (a short
 * silence), above it by 3 frames' sound for 3 frames a frame's sound is
 * left out (a splice). Frameskip off is automatic meanwhile, one picture
 * at a time (nspire_frameskip.c skip_for_sound). One line a second of all
 * this is kept for the perf log at the quit. */
#include "nspire_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NSPIRE_RUNTIME_AUDIO
#include "common.h"
#include "nspire.h"
#include "nspire_os.h"
#include "nspire_link.h"
#include "nspire_frameskip.h"
#include "nspire_padlog.h"
#include "nspire_bt_glue.h"
#include "pcm.h"
#include "ua_os.h"
#include "nspire_audio_mix.h"

#define barrier() __asm__ __volatile__("" ::: "memory")

static struct ring output;
static struct resampler resampler;
static int initialized, exit_set, rendering, priming, last_state;
static uint32_t rate, generation, settings, dropped;

/* ---- OS time ---- */
#define MS_TICKS(ms) ((uint32_t)((uint64_t)NSPIRE_TICK_HZ * (ms) / 1000))
#define AWAY_MS ((UA_NX - 1) * UA_NF - 6)   /* 36 with 4 of 14 ms (56 descriptors, 42 ms ahead after the OS ran); 42 with 7 of 8 */
#define SETUP_MS 16                         /* configuring: the OS's control transfers, the old cadence */
#define SERVE_MS 24                         /* a window this long after the OS last served the queue */
#define OS_ROOM MS_TICKS(2)                 /* (sleep_cost: the room a wait's sleep needs before its deadline) */
#define TASK_MS 2000                        /* a task sleep at least this often (windows serving the queue) */
#define STALL_MS 100                        /* no completion this long, the OS served: the device stalls */
#define TASK_WATCH_MS 30                    /* the task's tick by the frame loop's watch: this before the limit */
#define WIN_MISSES 3                        /* windows in a row that should have delivered and did not */
#define WIN_SPIN ((uint32_t)NSPIRE_TICK_HZ * 3 / 10000)   /* a window waits 0.3 ms for a completion, at most */
static uint32_t work_avg;                   /* a frame's work (nspire_audio_os_time), ticks */
static uint32_t work_peak;                  /* speed18: the longest of the last frames' (1/16 off a frame) */
static uint32_t os_call_ticks, os_call_t0;  /* speed18: an OS call's time since the last frame (served in it: not work) */
static uint32_t last_poll;                  /* the last poll's time */
static uint32_t sleeps_forced, sleeps_near; /* nspire_audio's own sleeps since start: forced, placed */
static int win_trusted = 1;                 /* windows deliver (R11u: 209 of 215) until WIN_MISSES say not */
static unsigned win_misses;
static uint32_t served_at;                  /* a window last delivered completions */
static uint32_t comp_last, comp_at;         /* the completions counted at the last look, when they last rose */
static uint32_t win_tried_at;               /* the last window's end */
static int stalled;
static uint32_t windows, windows_served, window_ticks, window_completions, stalls, distrusts;
static uint32_t probe_at;                   /* the once-a-second window while windows are not trusted */
/* speed18: the queue as the driver's frame clock sees
 * it (ua_queue_ms). With 4 transfers of 14 ms (56 descriptors: 42 ms ahead
 * after the OS ran, 71 completions a second vs 125 with 7 of 8) the fixed
 * limit, AWAY_MS, is 36 ms, which forces more windows than the queue needs.
 * The queue holds 42-56 ms after the OS ran (56 less how far the first
 * transfer has played), and the driver knows which: from each transfer's
 * frame as the OS links it (its code: right after the last packet, or 4
 * frames on), FRINDEX now. So a window when the queue would hold less than
 * Q_MARGIN_MS at the next frame's end and a completion waits for it; else none. The estimate errs short (nspire-usbaudio ua18's tests:
 * never longer than the simulated OS's; equal in a stream). Q_STARVES gaps
 * on the wire (the driver's restarts) in one stream and it is not trusted:
 * the fixed limit for the rest of that stream. */
#ifndef Q_MARGIN_MS
#define Q_MARGIN_MS 6                       /* left at the next service: the OS's 4 frames and 2 to spare */
#endif
#define Q_STARVES 3
static int q_trusted = 1;
static uint32_t q_starved0, q_distrusts;
static uint32_t q_hold_at;                  /* a stall ended then: the queue's clock not used for Q_HOLD_MS */
static int q_hold;
#define Q_HOLD_MS 1000

/* ---- the rate ---- */
/* A GBA frame (280896 cycles at 16.78 MHz) in timer ticks, Q8. */
#define FRAME_Q8 ((uint32_t)((uint64_t)280896u * NSPIRE_TICK_HZ * 256u / 16777216u))
#define RC_ONE 65536
#define RC_MIN (RC_ONE / 4)        /* a game at a quarter of its speed: sound at a quarter */
#define RC_MAX (RC_ONE * 102 / 100)
#define RC_MAX_FF (RC_ONE * 16)    /* fast forward speeds up audio proportionally (up to 16x) */
#define RC_DRIFT 131               /* +-0.2%: the drift term's reach at full speed */
#define RC_KP 33                   /* 0.05% at a fill error of the whole target */
#define RC_KI 1                    /* 1.5 ppm a measurement at the whole target */
#define RC_STEP_FULL 7             /* 0.01% a frame at full speed */
#define RC_STEP_SLOW 328           /* 0.5% a frame into, in and out of the slow mode */
#define RC_SLOW_IN (RC_ONE * 97 / 100)
#define RC_SLOW_OUT (RC_ONE * 99 / 100)
#define RC_HOLD 120                /* frames (two seconds) a mode's condition must hold */
#define RC_WINDOW 128              /* frames the game's speed is taken over */
#define RC_FF_WINDOW 24            /* shorter window in fast forward so pitch tracks speed changes */
static uint32_t rc_frames, rc_first, rc_ends[RC_WINDOW];
static uint32_t base_step;         /* the resampler's step at the device's nominal rate */
static uint32_t rc_fill;           /* the ring after the OS's turns, averaged (frames, Q4) */
static int32_t rc_i;               /* the drift term (Q16), kept over pauses */
static uint32_t rc_speed = RC_ONE; /* the factor now (Q16): < 1 stretches, > 1 speeds up */
static uint32_t rc_game = RC_ONE;  /* the game's speed, averaged (Q16) */
static uint32_t rc_last_end, rc_target;
static uint32_t rc_base;          /* (r19-snd) the target without a save's bank: re-primes and priming go by it */
static int rc_fill_valid, rc_slow, ff_mode, ff_seeded, last_mode;
static uint32_t rc_below;            /* (r19-fix) frames in a row the game's speed below RC_SLOW_IN (a save's too) */
static unsigned rc_hold, rc_low, rc_high;
static unsigned low_sync, sync_settle;      /* bt10: the card's fill under where it settled (rate_update) */
static uint32_t sync_slow;                  /* ...that fill (Q4) averaged over ~500 services (~8 s) */
static uint32_t trims, reprimes, trimmed;

/* ---- the log: one record a second while the adapter streams ---- */
#define SEC_KEEP 300               /* five minutes */
struct sec_rec {
  uint32_t t;                      /* seconds since gpSP's audio started */
  uint8_t frames, drawn, sleeps, forced, near, flags;
  uint16_t os_ms, ring_min, ring_max, longest;
  uint8_t ahead_min, silent_ms, late_ms, restarts, failed_ms;
  uint8_t away_ms;                 /* the longest seen without the OS serving the queue */
  uint8_t win, win_served;         /* IRQ windows that second, of them with completions */
  uint16_t win_10us;               /* their time, 10 us */
  uint8_t trims, reprimes;
  int16_t speed;                   /* the rate's factor - 1, in 1/10000 */
};
#define SF_RENDER 1                /* the game's sound playing (else paused: menu, fast forward, muted) */
#define SF_PRIMING 2
#define SF_SLOW 4                  /* the slow-game mode */
#define SF_STALL 8                 /* the device took nothing */
#define SF_NOWIN 16                /* windows not trusted */
#define SF_RESET 32                /* the frame counters started over in it (a menu, a state, fast forward) */
#define SF_BT 64                   /* bt10: Bluetooth headphones took the game's sound (beside the adapter or alone) */
#define SF_IDLE 128                /* bt10: the card idle (Output Bluetooth: nothing queued but a keep-alive a second) */
static struct sec_rec secs[SEC_KEEP];
static unsigned sec_n;             /* records made (the last SEC_KEEP kept) */
static uint32_t sec_start, sec_t0, sec_frames, sec_drawn, sec_sleeps, sec_os, sec_forced, sec_near;
static uint32_t sec_ring_min, sec_ring_max, sec_longest, sec_away;
static uint32_t sec_win, sec_win_served, sec_win_ticks, sec_trims, sec_reprimes;
static int sec_on, sec_flags;
static int log_pending;

static uint32_t setting_word(void)
{
  return (nspire_audio_enabled & 1) | (nspire_audio_volume % (NSPIRE_VOLUME_MAX + 1)) << 16 |
    (nspire_audio_mono & 1) << 8 | (nspire_audio_buffer % 3) << 12;
}

int nspire_usbaudio_active(void) { return initialized && ua_attached(); }
int nspire_usbaudio_owns_usb(void) { return initialized && ua_owns_usb(); }
/* Hub (the pad glue's hub_poll): a callback of the sound driver is inside
 * the OS's USB task now (an attach's requests, a detach's abort). */
#ifdef UA_HAS_BUSY
int nspire_usbaudio_busy(void) { return initialized && ua_busy(); }
#else
int nspire_usbaudio_busy(void) { return 0; }
#endif
/* (Bluetooth headphones taking the game's sound count too: nspire_bt_glue.c) */
static int bt_rendering;
static uint32_t bt_generation;
int nspire_usbaudio_playing(void) { return (initialized && rendering) || bt_rendering; }

/* Discovery needs OS time before an audio callback can own anything,
 * including builds with both other USB drivers disabled. Asking for OS
 * time is not an ownership claim and never restarts the controller. */
/* 2026-11-16: not for ever. This said yes for as long as anything was at
 * the adapter's end, so a controller, a drive, a device no driver takes
 * (headphones this driver can't use, a keyboard) or an empty adapter cost
 * the game an OS sleep every AWAY_MS (42 ms: ~24 a second, up to a tick
 * each) until it came out: the game ran much slower with anything at all
 * plugged in. The OS offers a device within a second or two of its
 * time after the port's bits last moved; DISCOVER_MS after that the OS is
 * done with what is on the port and only an adapter this driver owns
 * needs its time. What is plugged into a hub later is the pad glue's (its
 * hub tick and its time in one go: nspire_usbpad_glue.c), and with the
 * OS's USB found stopped nothing is offered whatever time it gets. */
#define DISCOVER_MS 8000
int nspire_usbhub_present(void) __attribute__((weak));
int nspire_usbpad_os_stuck(void) __attribute__((weak));
int usbpad_os_explore_pending(void) __attribute__((weak));
static uint32_t port_bits_seen, port_moved_at;
static int port_seen, port_state_seen;

static int service_wanted(void)
{
  uint32_t bits, now;
  if (ua_owns_usb()) return 1;
  if (link_cable_fast_up() || nspire_usb_plugged() <= 0) { port_seen = 0; return 0; }
  bits = nspire_usb_bits();
  if (bits & 0x20000000u) { if (bits & 0x20u) { port_seen = 0; return 0; } }       /* CX II: not the host's end */
  else if (bits & 0x10000000u) { if (bits & 0x01u) { port_seen = 0; return 0; } }  /* CX: not the host's end */
  now = nspire_ticks();
  /* The driver's state moving counts as the port moving: its adapter pulled
   * out (UA_GONE) is followed by what is plugged in next, and that needs
   * the OS's time whether or not the port's bits were seen to change. */
  if (!port_seen || bits != port_bits_seen || (int) ua_state() != port_state_seen) {
    port_seen = 1;
    port_bits_seen = bits;
    port_state_seen = (int) ua_state();
    port_moved_at = now;
  }
  if (nspire_usbpad_os_stuck && nspire_usbpad_os_stuck()) return 0;
  if (nspire_usbhub_present && nspire_usbhub_present()) return 0;
  return now - port_moved_at < MS_TICKS(DISCOVER_MS);
}

static void sleep_tick(void) { (void)nspire_os_sleep(); }
void nspire_audio_draining(void) __attribute__((weak));

static int streaming(void) { return ua_state() == UA_STREAMING; }

/* bt10: the card idle (nspire-usbaudio's ua_set_idle, when the driver has
 * it): asked while nothing should come out of it, Output Bluetooth (with or
 * without headphones: the card plays silence under that setting either
 * way). Nothing is queued then but one transfer of silence a second (the
 * driver's keep-alive, made by the ua_set_idle(1) asked at every main-loop
 * pass: poll()), so no completion every 8 ms, no IRQ window, no OS sleep
 * for the card's queue: the card's ~40 ms of CPU a second goes. The menus
 * of another Output keep the card streaming silence (its resume would put
 * its first sound ~16 ms after a menu, and the menus have the CPU to
 * spare). The OS-time code below takes an idle card as one that needs
 * only the OS's task now and then (by_windows, away_limit, check_stall,
 * nspire_audio_serves_os) and opens no window for it (the keep-alive's
 * completion comes in a sleep, or in the headphones' dongle's windows). */
static int card_idle;
static uint32_t card_idles;
#ifdef UA_HAS_IDLE
static int card_idle_wanted(void)
{
  return initialized && streaming() && nspire_audio_output == NSPIRE_OUTPUT_BT;
}
#endif
int nspire_usbaudio_streaming(void) { return initialized && streaming() && !stalled; }

/* The windows serve the queue now: streaming, trusted, the device not stalled. */
static int by_windows(void) { return streaming() && !card_idle && win_trusted && !stalled; }
/* (2026-11-18, Hub 5: the pad glue's look at a hub after its power fell)
 * -1 not streaming, 1 the device takes nothing (a stall), 0 its packets come. */
int nspire_usbaudio_stalled(void)
{
#ifdef UA_HAS_IDLE
  /* (nspire-usbaudio ua17: idle while the game's sound goes to Bluetooth
   * headphones: a healthy card with nothing queued brings no completions:
   * that says nothing of its hub) */
  if (initialized && ua_idle()) return -1;
#endif
  return initialized && streaming() ? stalled != 0 : -1;
}

/* How long gpSP may stay away from the OS's task now (ticks): the queue's
 * limit unless windows serve it (then the task's own, TASK_MS).
 * 2026-11-17 (final/pace6): with a USB hub in, NSPIRE_AUDIO_HUB_MS: the
 * hub's port changes reach the OS's USB task only in its time, and the pad
 * glue's hub tick did that with sleeps of its own at the key read, near
 * ticks from 33 ms and forced at 100 ms for 30 s after the hub's last
 * device came (a CX II with the adapter on a hub: 22 sleeps a
 * second, 80-100 ms of the game's time, 60/50-53 pictures; 4-9 and 5-40 ms
 * after). These sleeps are placed where they cost least (the look ahead in
 * nspire_audio_os_time: in a wait to come when its tick fits there).
 * speed18: with the OS's explore flag known (usbpad_os_explore_pending >= 0),
 * hot-plug status-pipe completions set it in an IRQ window and the pad glue
 * gives fast ticks at once, so NSPIRE_AUDIO_HUB_MS is 1000 ms (falling back
 * to NSPIRE_AUDIO_HUB_FALLBACK_MS = 250 ms when unknown or Bluetooth wants). */
static uint32_t task_ms(void)
{
  if (nspire_os_wanted & OS_WANT_BT)
    return NSPIRE_AUDIO_HUB_FALLBACK_MS;
  if (!(nspire_usbhub_present && nspire_usbhub_present()))
    return TASK_MS;
  if (!usbpad_os_explore_pending || usbpad_os_explore_pending() < 0)
    return NSPIRE_AUDIO_HUB_FALLBACK_MS;
  return NSPIRE_AUDIO_HUB_MS;
}

static int queue_now(int32_t *ahead, int32_t *head);
static uint32_t away_limit(void)
{
  int st = ua_state();
  int32_t ahead, head;
  if (st == UA_SETUP || st == UA_READY) return MS_TICKS(SETUP_MS);
  if (card_idle) return MS_TICKS(task_ms());   /* (idle: nothing queued, the OS's task now and then) */
  if (by_windows() || (streaming() && stalled)) return MS_TICKS(task_ms());
  /* speed18: sleeps serve the queue (windows not trusted): as late as the
   * queue's clock allows, up to the fixed limit's queue (a transfer's less) */
  if (streaming() && queue_now(&ahead, &head)) {
    int32_t left = ahead - (int32_t)MS_TICKS(Q_MARGIN_MS);
    return nspire_os_since_sleep() + (left > 0 ? (uint32_t)left : 0);
  }
  return MS_TICKS(AWAY_MS);
}

int nspire_audio_serves_os(void)
{
  return initialized && streaming() && !card_idle && (nspire_os_wanted & OS_WANT_AUDIO);
}

static uint32_t completions_now(void)
{
  struct ua_counters c;
  barrier();
  ua_counters(&c);
  barrier();
  return c.completions;
}
/* (2026-11-18, the pad glue's look at a hub) the stream's completions so far */
uint32_t nspire_usbaudio_completions(void) { return initialized ? completions_now() : 0; }

/* The completions counted so far: when they last rose. */
static void note_completions(uint32_t now)
{
  uint32_t c = completions_now();
  if (c != comp_last) {
    comp_last = c;
    comp_at = now;
    if (stalled) {
      stalled = 0;               /* it takes packets again: the sound starts clean (frame_begin) */
      q_hold = 1;                /* (speed18: what was queued through the stall went out who knows when) */
      q_hold_at = now;
    }
  }
}

/* Since the OS last served the queue (a task sleep's wake or a window). */
static uint32_t since_served(uint32_t now)
{
  uint32_t s = nspire_os_since_sleep(), w = (int32_t)(now - served_at) > 0 ? now - served_at : 0;
  return w < s ? w : s;
}

/* speed18: the queue now, in ticks (see q_trusted above): 1 known. *ahead:
 * until the wire runs dry; *head: until a completion waits (<= 0: one does). */
static int queue_now(int32_t *ahead, int32_t *head)
{
#ifdef UA_HAS_QUEUE_MS
  struct ua_counters c;
  int a, h;
  if (q_hold && nspire_ticks() - q_hold_at >= MS_TICKS(Q_HOLD_MS))
    q_hold = 0;
  if (!q_trusted || q_hold || card_idle || stalled || !ua_queue_ms(&a, &h))
    return 0;
  ua_counters(&c);
  if (c.starved - q_starved0 >= Q_STARVES) {
    q_trusted = 0;              /* gaps on the wire: the fixed limit from now on */
    q_distrusts++;
    return 0;
  }
  *ahead = (int32_t)NSPIRE_TICK_HZ * a / 1000;
  *head = (int32_t)NSPIRE_TICK_HZ * h / 1000;
  return 1;
#else
  (void)ahead;
  (void)head;
  return 0;
#endif
}

/* By windows: serve the queue if it must be before 'horizon' more ticks
 * pass without the OS (the next frame's end, or the next poll), or, the
 * queue's clock not known, 'fixed' more against the fixed limit. since: the
 * time since it was last served. A window that delivers nothing when the
 * queue cannot wait is followed by a sleep. 1 if it slept. */
static int os_sleep_counted(uint32_t *counter);
static int serve_window(uint32_t now);
static void check_stall(uint32_t now);
static int serve_queue(uint32_t now, uint32_t since, uint32_t horizon, uint32_t fixed)
{
  int32_t ahead, head;
  if (queue_now(&ahead, &head)) {
    if (ahead > (int32_t)(horizon + MS_TICKS(Q_MARGIN_MS)))
      return 0;                 /* it holds past the next frame's end */
    if (head > 0 && (ahead >= (int32_t)MS_TICKS((UA_NX - 1) * UA_NF) || since + fixed < MS_TICKS(AWAY_MS)))
      return 0;                 /* nothing to take yet (a full queue: a window can't help, the next look will;
                                   a short one filling again: the fixed limit) */
    if (serve_window(now))
      return 0;
    /* nothing came: the head's transfer is not done if it was linked later
     * than the driver took it (ua_queue_not_done moves it on, within what
     * that can account for); else the completions did not run: a sleep */
    if (head <= 0 && ua_queue_not_done() > 0 && queue_now(&ahead, &head) &&
        ahead > (int32_t)(horizon + MS_TICKS(Q_MARGIN_MS)))
      return 0;                 /* (it was linked later: it holds after all) */
    check_stall(now);
    return !stalled && os_sleep_counted(&sleeps_forced);
  }
  if (since >= MS_TICKS(SERVE_MS) || since + fixed >= MS_TICKS(AWAY_MS)) {
    if (!serve_window(now) && since + fixed >= MS_TICKS(AWAY_MS)) {
      check_stall(now);
      return !stalled && os_sleep_counted(&sleeps_forced);
    }
  }
  return 0;
}

/* A sleep of nspire_audio's own, counted: 1 if it slept. */
static int os_sleep_counted(uint32_t *counter)
{
  uint32_t t = nspire_ticks();
  if (!nspire_os_sleep())
    return 0;
  (*counter)++;
  t = nspire_ticks() - t;
  if (t > sec_longest) sec_longest = t;
  note_completions(nspire_ticks());
  return 1;
}

/* The CPU's interrupts on until the pending completions have run (they run
 * in the interrupt, before this code goes on: R11u's probe), WIN_SPIN at
 * most when none comes; the keypad line kept off (R6). 1 if completions
 * came. */
static int irq_window(void)
{
  uint32_t before = completions_now(), c, t0, t, old;
  unsigned spins = 0;
  nspire_os_keypad_keep_off();
  t0 = nspire_ticks();
  old = nspire_irqs_unmask();
  do {
    barrier();
    c = completions_now();
  } while (c == before && nspire_ticks() - t0 < WIN_SPIN && ++spins < 4096);   /* (a timer standing still) */
  nspire_irqs_restore(old);
  t = nspire_ticks() - t0;
  win_tried_at = t0 + t;
  nspire_os_after_os_time();
  windows++;
  window_ticks += t;
  sec_win++;
  sec_win_ticks += t;
  if (c == before)
    return 0;
  windows_served++;
  window_completions += c - before;
  sec_win_served++;
  served_at = nspire_ticks();
  note_completions(served_at);
  return 1;
}

/* A window because the queue wants serving: misses counted (a transfer has
 * come back since the last delivery, and none came), trust lost after
 * WIN_MISSES; a delivery while not trusted earns it back. */
static int serve_window(uint32_t now)
{
  int expected = (int32_t)(nspire_ticks() - comp_at) >= (int32_t)MS_TICKS(UA_NF + 4);
  (void)now;
  if (irq_window()) {
    win_misses = 0;
    win_trusted = 1;
    return 1;
  }
  if (expected && win_trusted && ++win_misses >= WIN_MISSES) {
    win_trusted = 0;
    distrusts++;
  }
  return 0;
}

/* A stall: streaming, no completion for STALL_MS, and the OS served since
 * (a sleep's wake or a window tried after it). */
static void check_stall(uint32_t now)
{
  int32_t quiet;
  (void)now;
  if (!streaming() || stalled || card_idle)
    return;                       /* (idle: no completion comes, by design) */
  now = nspire_ticks();           /* (a window since the caller's time may have moved comp_at past it) */
  quiet = (int32_t)(now - comp_at);
  if (quiet >= (int32_t)MS_TICKS(STALL_MS) &&
      ((int32_t)nspire_os_since_sleep() < quiet || (int32_t)(win_tried_at - comp_at) > 0)) {
    stalled = 1;
    stalls++;
  }
}

/* (r19-snd: a save's bank, below) */
static uint32_t bank, bank_ff_ms;
static uint32_t owe;                /* frames of an OS call's time to make up (the bank couldn't pay it all) */
static uint32_t owed, repaid, owe_dropped;   /* (r19-fix: the stats' counts) */
/* (r19-fix, core fix 2) this save's own time (its OS calls and the take as timed, its CPU parts
 * at CPU_PART_MS each) and what it owed: never more than that; and the frames since the last made up */
static uint32_t save_cost_ticks, save_owed, repay_n;
static int save_last_next = -1;             /* (r19-fix) the last part's kind (a take after others: a new save) */
static int take_late;                        /* (r19-fix) that save's take: TAKE_LATE_FRAMES after the last */
#define TAKE_LATE_FRAMES 30
#define SLOW_NO_BANK 30                      /* (r19-fix) frames the game's speed is below the slow-game mode's */
static int save_active;             /* a save under way, or its bank or debt not settled */
static uint32_t frame_sound(void);
static uint32_t bank_frames(void);
static unsigned take_waited;
static void bank_reset(void)
{
  bank = bank_ff_ms = owe = 0;
  save_cost_ticks = save_owed = repay_n = 0;
  save_active = 0;
  take_late = take_waited = 0;   /* (core fix 2) */
}

/* (a save's call or hold moved the clock: the game's speed taken afresh, not slow for it) */
static void speed_window_restart(void)
{
  rc_first = rc_frames;
  rc_hold = 0;
}

static void rate_reset(void)
{
  rc_fill_valid = 0;
  rc_fill = 0;
  rc_first = rc_frames;
  rc_hold = rc_low = rc_high = rc_below = 0;   /* (rc_i, rc_slow, rc_game, rc_speed: as they were) */
  low_sync = sync_settle = 0;
}

/* Every discontinuity drops PCM before it enables fresh game audio. A
 * consumer-side request leaves rd alone if a USB completion is suspended.
 * Already submitted packets may finish (at most UA_NX x 8 ms); they are
 * never overwritten while the OS owns them. */
static int stopping;               /* (r19-snd) soft_stop's tail playing: the pause and the clear still to come */
static uint32_t stop_end;           /* ...the ring's frame counter where its faded frames end */
static void discard_keep(void);
static void discard(void)
{
  stopping = 0;
  ua_set_pause(1);
  ring_request_clear(&output);
  discard_keep();
}

/* discard but the device's: the producer side (the mixer, the resampler, the rate) started over */
static void discard_keep(void)
{
  if (!bt_rendering) {
    /* (bt10: the mixer goes on for the headphones while they take its sound beside the adapter) */
    sound_set_output_enabled(0);
    sound_discard_samples();
  }
  rs_init(&resampler, sound_frequency, rate ? rate : 48000);
  base_step = resampler.step;
  if (rc_speed > RC_MAX)
    rc_speed = (uint32_t)(RC_ONE + rc_i);
  resampler.step = (uint32_t)((uint64_t)base_step * rc_speed / RC_ONE);
  if (!resampler.step) resampler.step = 1;
  ff_mode = ff_seeded = last_mode = 0;
  rate_reset();
  rendering = 0;
  priming = 1;
  generation = sound_output_generation;
  bank_reset();
}

static void soft_stop(void);
static void stop_poll(void);

void nspire_audio_pause(void)
{
  if (initialized && rendering) soft_stop();
  /* Bluetooth headphones: paused too (the menus run no frames) */
  if (bt_rendering) {
    bt_rendering = 0;
    sound_set_output_enabled(0);
    sound_discard_samples();
  }
  if (nspire_bt_live)
    (void)nspire_bt_glue_audio_frame(0, 0);
}

/* ---- An OS file call with the sound going on (2026-11-17, final/pace6) ----
 * The sound used to cut out on saves: a state's save paused the sound
 * (a cut, then silence until the ring was primed again) and the game's own
 * save (the .sav) left the queue unserved for the whole write: the OS's file
 * calls take 28-205 ms on the CX II (gpsp_states_time.txt.tns) with the
 * CPU's interrupts masked, the queue holds 48 ms. Now the interrupts are on
 * for the call (the OS's USB completions run in it as in a window, the
 * keypad line kept off), so the ring plays on: 48 ms of it and the queue's
 * 48 cover the usual call. In case it runs dry, its newest FADE_FRAMES
 * fade out first (kept): if the call ended before the USB task reached
 * them, they are put back as they were (nothing heard); else the sound
 * stops on the faded end, the ring is primed again, and the next sound fades
 * in: silence for the rest of the call, no click at either end. */
#define FADE_FRAMES 96                 /* 2 ms at 48 kHz */
static int16_t fade_kept[FADE_FRAMES * 2];
static uint32_t fade_from;             /* the ring's frame counter where the faded end starts */
static unsigned fade_n, fade_in_left;
static int fading;
static uint32_t os_calls, os_call_dry;
static uint32_t soft_stops;
static void bank_after_call(uint32_t ticks);

static int16_t *ring_frame(uint32_t at)
{
  return &output.buf[(at & (RING_FRAMES - 1)) * 2];
}

/* ---- The sound stopped without a click (2026-11-21, r19-snd) ----
 * A pause (the menu, a load, the Sound page) was discard(): the driver
 * paused at once, so the transfers queued (42-56 ms) played and the next
 * packet was silence: the waveform cut wherever it was (a click), and a
 * state's load, masked for its 135 ms, left the queue empty on the wire.
 * Now, while the sound plays: the next FADE_FRAMES the device takes fade to
 * nothing, the rest of the ring is dropped, STOP_ZEROS_MS of silence follow
 * (the queue keeps taking packets, the device hears silence), and the pause
 * and the ring's clear come once the faded frames went out (poll: stop_poll)
 * or at the next start (discard). The start after it fades in. Called with
 * the interrupts masked: the USB task, in its completions, isn't in the ring
 * now, and wr is the producer's. */
#define STOP_ZEROS_MS 250                    /* (a load key's 135 ms in it, a poll or two of the menu's) */
static void soft_stop(void)
{
  static const int16_t zeros[256 * 2];
  uint32_t used, n, i, z;
  if (!(streaming() && !stalled && !card_idle && rendering && !priming && rate &&
        output.cleared_seq == output.clear_seq)) {
    discard();
    return;
  }
  used = ring_used(&output);
  n = used < FADE_FRAMES ? used : FADE_FRAMES;
  for (i = 0; i < n; i++) {
    int16_t *f = &output.buf[((output.rd + i) & (RING_FRAMES - 1)) * 2];
    int32_t g = (int32_t)(n - 1 - i);
    f[0] = (int16_t)(f[0] * g / (int32_t)n);
    f[1] = (int16_t)(f[1] * g / (int32_t)n);
  }
  output.wr = output.rd + n;          /* (the rest of the ring dropped: the producer's index) */
  stop_end = output.wr;
  for (z = rate * STOP_ZEROS_MS / 1000; z; ) {
    unsigned k = z < 256 ? z : 256, put = ring_put(&output, zeros, k);
    if (!put) break;
    z -= put;
  }
  discard_keep();
  stopping = 1;
  soft_stops++;
}

/* (each poll) the faded frames gone out: the pause and the clear, as discard's */
static void stop_poll(void)
{
  if (stopping && (int32_t)(output.rd - stop_end) >= 0) {
    stopping = 0;
    ua_set_pause(1);
    ring_request_clear(&output);
  }
}

/* (r19-snd) The fade is armed only for a call the ring may not cover (the
 * save's gate let it go short of its need: save_fade_next), and a call is dry
 * only when the device played silence in it (the driver's silent packets
 * rose): pace6 took the ring's newest 2 ms reached as dry, but the queue
 * still held 42-56 ms then, and the re-prime made a silence of its own
 * (the model's r19 save: a 205 ms call on a 207 ms ring re-primed). The
 * faded end reached with no silence played: the next sound fades in (a dip
 * of 4 ms, counted: os_call_dips), nothing primed. */
static int save_fade_next;
static uint32_t call_silent0, os_call_dips;

static uint32_t silent_pkts_now(void)
{
  struct ua_counters c;
  ua_counters(&c);
  return c.underrun_pkts;
}

/* (r19-snd) A stretch with the interrupts on outside a game's frame (the
 * menu's save, a load: nspire_states.c with states_serve_os): the queue
 * served from the ring all through it; the OS calls in it are in it (no
 * call of their own, nothing timed for the save's bank). */
static int serving;

int nspire_audio_serve_begin(uint32_t *cpsr)
{
  if (!initialized || !streaming() || stalled || serving)
    return 0;
  serving = 1;
  nspire_os_keypad_keep_off();
  *cpsr = nspire_irqs_unmask();
  return 1;
}

void nspire_audio_serve_end(int began, uint32_t cpsr)
{
  uint32_t now;
  if (!began)
    return;
  nspire_irqs_restore(cpsr);
  nspire_os_after_os_time();
  serving = 0;
  now = nspire_ticks();
  if (completions_now() != comp_last)
    served_at = now;
  note_completions(now);
}

int nspire_audio_os_call_begin(uint32_t *cpsr)
{
  unsigned i;
  if (!initialized || !streaming() || stalled || serving)
    return 0;
  fading = 0;
  if (save_fade_next && rendering && !priming && output.cleared_seq == output.clear_seq) {
    uint32_t used = ring_used(&output);
    fade_n = used < FADE_FRAMES ? used : FADE_FRAMES;
    fade_from = output.wr - fade_n;
    for (i = 0; i < fade_n; i++) {
      int16_t *f = ring_frame(fade_from + i);
      int32_t g = (int32_t)(fade_n - 1 - i);
      fade_kept[2 * i] = f[0];
      fade_kept[2 * i + 1] = f[1];
      f[0] = (int16_t)(f[0] * g / (int32_t)fade_n);
      f[1] = (int16_t)(f[1] * g / (int32_t)fade_n);
    }
    fading = fade_n > 0;
  }
  save_fade_next = 0;
  call_silent0 = silent_pkts_now();
  os_calls++;
  nspire_os_keypad_keep_off();
  os_call_t0 = nspire_ticks();
  *cpsr = nspire_irqs_unmask();
  return 1;
}

void nspire_audio_os_call_end(int began, uint32_t cpsr)
{
  unsigned i;
  uint32_t now;
  int reached, dry;
  if (!began)
    return;
  nspire_irqs_restore(cpsr);
  nspire_os_after_os_time();
  now = nspire_ticks();
  os_call_ticks += now - os_call_t0;
  if (completions_now() != comp_last)
    served_at = now;
  note_completions(now);
  bank_after_call(now - os_call_t0);
  dry = rendering && !priming && silent_pkts_now() != call_silent0;
  reached = fading && (int32_t)(output.rd - fade_from) > 0;
  if (fading && !reached && !dry) {
    for (i = 0; i < fade_n; i++) {   /* not reached: as it was */
      int16_t *f = ring_frame(fade_from + i);
      f[0] = fade_kept[2 * i];
      f[1] = fade_kept[2 * i + 1];
    }
  }
  fading = 0;
  if (!dry) {
    if (reached) {
      os_call_dips++;                /* (the faded end went out, nothing silent: the next sound fades in) */
      fade_in_left = FADE_FRAMES;
    }
    return;
  }
  /* silence played in it: primed again, the next sound faded in */
  os_call_dry++;
  fade_in_left = FADE_FRAMES;
  reprimes++;
  sec_reprimes++;
  priming = 1;
  ua_set_pause(1);
  rc_fill_valid = 0;
}

void nspire_audio_serve_ahead(unsigned ms)
{
  uint32_t now, h;
  if (!initialized || !(nspire_os_wanted & OS_WANT_AUDIO) || !by_windows())
    return;
  now = nspire_ticks();
  note_completions(now);
  h = MS_TICKS(ms) + work_peak + OS_ROOM;
  (void)serve_queue(now, since_served(now), h, h);
}

/* The game's sound in the ring (ms) once the queue is served (a window:
 * the transfers that came back are filled from the ring, which an OS call
 * with the interrupts on does first thing), -1 when none plays (no adapter,
 * paused, priming): what an OS call may take before the ring runs dry. */
int nspire_audio_ring_ms(void)
{
  if (!initialized || !streaming() || stalled || !rendering || priming || !rate)
    return -1;
  if (by_windows())
    (void)serve_window(nspire_ticks());
  return (int)((uint64_t)ring_used(&output) * 1000u / rate);
}

/* (final/r19-pace) The same as it is now, nothing served (the frame loop's
 * frameskip asks each frame: nspire_frameskip.c np_ring_low). */
int nspire_audio_ring_level_ms(void)
{
  if (!initialized || !streaming() || stalled || !rendering || priming || !rate)
    return -1;
  return (int)((uint64_t)ring_used(&output) * 1000u / rate);
}

/* ---- A save's OS calls: the ring deep enough first (2026-11-21, r19-snd) ----
 * A CX II (7baa00e, Emerald, autosave every minute): a state's save is not
 * to interrupt the sound. Its log: a minute apart (the
 * autosaves) the ring ran to 0-30 ms, 56 ms of silent packets and a re-prime
 * at one, a frame's sound left out after it; the state's OS write took 229.7
 * ms (gpsp_states_time.txt.tns). pace6's gate (SAVE_RING_MS 70, 4 frames run
 * ahead at once) covered ~110 ms of a call with the queue: a longer one ran
 * the ring dry (faded, re-primed: silence), and the frames run ahead stayed
 * in the ring over its target (the rate then left a frame's sound out).
 * Now, for each OS call of a save (nspire_host_main.c save_job_frame, at
 * each frame's end while a save is under way):
 *  - the bank: the game runs ahead a frame at a time (nspire_frameskip_
 *    run_ahead(1), its picture left out by the rule, never while the clock is
 *    still behind the last one) until the ring and the queue hold the longest
 *    OS call yet (SAVE_OS_FLOOR_MS at least: the log's 229.7) and
 *    SAVE_MARGIN_MS; the rate's target goes up by the bank (rc_target: it is
 *    no fill error, nothing is left out for it); fast forward has no clock:
 *    its target is raised and its rate fills it;
 *  - the call then; after it, its time is not made up (nspire_frameskip_hold
 *    of the call's frames, up to the bank): the bank is what paid for it, the
 *    clock stays where the ring is;
 *  - the save done, the rest of the bank is given back a frame at a time
 *    (a hold every BANK_HOLD_EVERY frames: a picture shown a refresh longer)
 *    while the ring still holds it.
 * The fade on a call that runs the ring dry anyway (nspire_audio_os_call_
 * begin) stays as the last resort. */
/* (weak: builds of nspire_audio.c without the frame loop, tests/audio's sink_test, bank nothing) */
#pragma weak nspire_frameskip_run_ahead
#pragma weak nspire_frameskip_hold
#pragma weak nspire_frameskip_behind
#pragma weak nspire_frameskip_spare
static void clock_ahead(void) { if (nspire_frameskip_run_ahead) nspire_frameskip_run_ahead(1); }
static void clock_hold(uint32_t n) { if (nspire_frameskip_hold) nspire_frameskip_hold(n); }
static int clock_late(void)
{
  return !nspire_frameskip_behind || nspire_frameskip_behind() >= (FRAME_Q8 >> 9);   /* (half a frame behind) */
}
/* (final/r19-bank) What a save's OS calls need is learned, not assumed: the
 * longest call of the saves so far (save_call_ms, in nspire_config.c's word
 * 2 across starts: nspire_save_call_ms), eased off a quarter at each save
 * whose calls were all shorter (one spike costs the next few saves, not all
 * of them), SAVE_FLOOR_MS at least: what the ring's own fill covers, so a
 * calculator whose calls are short runs nothing ahead and leaves no
 * picture out. A call longer than what was learned is learned at once (the
 * same save's later calls covered), and faded first (os_call_begin: the
 * fade is armed for every save's call now; covered, it is put back). */
#define SAVE_FLOOR_MS 10
#define SAVE_MARGIN_MS (UA_NF + 4)           /* left in the ring after the call: the queue's next refill (18 ms) */
#define SAVE_SETTLE 120                      /* frames after a save its calls' dip isn't a slow game or a gap */
#define BANK_EVERY 3                         /* a frame run ahead at most every 3 frames: one picture each */
#define REPAY_EVERY 2                        /* (r19-fix) an owed frame made up at most every other frame */
extern uint32_t nspire_save_call_ms;      /* (nspire_config.c: word 2 of gpsp.cfg.tns) */
#pragma weak nspire_save_call_ms
static uint32_t save_call_local;             /* (builds without nspire_config.c) */
static uint32_t *save_call(void) { return &nspire_save_call_ms ? &nspire_save_call_ms : &save_call_local; }
static uint32_t save_longest_now;            /* ticks: this save's longest OS call */
static uint32_t bank_peak_ms;                /* this save's most bank (ms), for the timing file */
static uint32_t bank_every_n;
static int save_was_busy, call_is_take;
static unsigned save_settle, cpu_waited, late_waited, take_waited;
#define TAKE_WAIT_FRAMES 6
#define LATE_WAIT_FRAMES 8
#define SAVE_TAKE_MS 30                      /* the state's take (the r19 log's copy 25.9 ms), interrupts on */
#define SAVE_WAIT_MS 1500                    /* the call goes then, ready or not */
#define BANK_HOLD_EVERY 4
#define BANK_FF_STEP_MS 2                    /* fast forward: its target raised this much a frame */
#define BANK_FF_OFF_MS 30                    /* ...and eased off this much a second after the save */
static uint32_t bank_ff_at;
static void save_reset(void);
static uint32_t save_wait_t0, bank_hold_n;
static int save_waiting;
static uint32_t os_call_longest;             /* ticks: the longest OS call of a save this session */
static uint32_t banked, bank_held, bank_held_back, save_calls, save_short;

/* a GBA frame's sound in the ring's frames */
static uint32_t frame_sound(void)
{
  return rate ? (uint32_t)((uint64_t)rate * 280896u / 16777216u) : 0;
}

static uint32_t bank_frames(void)
{
  return bank * frame_sound() + (uint32_t)((uint64_t)rate * bank_ff_ms / 1000u);
}

/* the ring's most with a bank (ms): three frames' sound short of full (a
 * frame's sound comes before the next service, and the rate's own room) */
static uint32_t bank_cap_ms(void)
{
  /* (fast forward: its fill held by the rate, not the clock, swings more: 7/8 of the ring and two frames
   * less, the rate's 3 frames over it still in the ring) */
  uint32_t most = ff_mode ? RING_FRAMES * 7 / 8 - 2 * frame_sound() : RING_FRAMES - 3 * frame_sound();
  return rate ? (uint32_t)((uint64_t)most * 1000u / rate) : 0;
}

/* what the ring must hold before a save's OS call (ms), the queue served:
 * the call's time and SAVE_MARGIN_MS (the queue, full then, is full after:
 * it refills from the ring all along) */
/* (one spike a save: a hardware log's one long call of 229.7 among short ones; a call of three quarters
 * of what was learned or more is this save's spike, the calls after it expect what its other calls took,
 * so the bank it spent isn't built again for them) */
static uint32_t save_spike_ticks, save_other_ticks;
static uint32_t save_need_ms(void)
{
  uint32_t learned = *save_call();
  if (save_spike_ticks)
    learned = (uint32_t)((uint64_t)save_other_ticks * 1000u / NSPIRE_TICK_HZ);
  learned = learned > SAVE_FLOOR_MS ? learned : SAVE_FLOOR_MS;
  return learned + SAVE_MARGIN_MS + learned / 8;   /* (a long call's: an eighth more, the queue's refills after it) */
}

/* a save done: what its calls took learned (the most of it and three quarters of what was) */
static void save_learn(void)
{
  uint32_t ms = (uint32_t)((uint64_t)save_longest_now * 1000u / NSPIRE_TICK_HZ), *l = save_call();
  uint32_t eased = *l - *l / 4;
  *l = ms > eased ? ms : eased;
  if (*l < SAVE_FLOOR_MS / 2)
    *l = 0;                            /* (nothing to keep: the floor covers it) */
  save_longest_now = save_spike_ticks = save_other_ticks = 0;
}

static void save_reset(void)
{
  bank_reset();
  save_settle = 0;
  save_was_busy = save_waiting = call_is_take = save_fade_next = 0;
  cpu_waited = late_waited = take_waited = bank_hold_n = bank_every_n = 0;
  save_longest_now = bank_peak_ms = save_spike_ticks = save_other_ticks = 0;
}

uint32_t nspire_audio_save_bank_ms(void)
{
  uint32_t b = bank_peak_ms;
  bank_peak_ms = 0;
  return b;
}

static int bank_playing(void)
{
  return initialized && streaming() && !stalled && !card_idle && rendering && rate;
}

#define CPU_PART_MS 3                        /* a save's CPU part (16 KB packed: ~2 ms on the CX II) */
#define CPU_WAIT_FRAMES 3                    /* ...run where the frame has that to spare, or spread a part in 4 frames */

int nspire_audio_save_frame(int busy, int next)
{
  int os_next = next != NSPIRE_SAVE_CPU;
  int ms;
  uint32_t need, have, base_ms, cap, want, fs_ms;
  if (busy && next == NSPIRE_SAVE_CPU && nspire_frameskip_spare && !ff_mode && rendering &&
      nspire_frameskip_spare() < MS_TICKS(CPU_PART_MS) && cpu_waited < CPU_WAIT_FRAMES) {
    /* (r19-bank) a CPU part in the frame's spare time only: no frame made late by it, no picture lost */
    cpu_waited++;
    return 0;
  }
  if (next == NSPIRE_SAVE_CPU)
    cpu_waited = 0;
  if (!bank_playing()) {
    save_waiting = 0;
    save_active = 0;
    return 1;                          /* (no sound to keep: the call at once, as before) */
  }
  if (busy && save_was_busy && next == NSPIRE_SAVE_TAKE && save_last_next != NSPIRE_SAVE_TAKE) {
    /* (r19-fix, core fix 5: a save asked for behind the last one begins at the frame's end after
     * its last part, with no frame between: the last one's calls learned, its spike not this one's) */
    save_learn();
    save_cost_ticks = save_owed = 0;
    take_late = 1;                     /* (its take after the last one's calls: the ring given time to come back) */
  }
  save_last_next = busy ? next : -1;
  if (busy && !save_was_busy)
    save_cost_ticks = save_owed = 0;   /* (a save begins: its own time from here) */
  if (save_was_busy && !busy) {
    save_learn();
    save_settle = SAVE_SETTLE;
    /* (core fix 2, r19: the wait of a take behind another save ends with the saves: one ended by the menu, a
     * restart, the link or a bad jump left it set, and the next save's state came half a second late) */
    take_late = 0;
    take_waited = 0;
  }
  save_was_busy = busy;
  if (busy)
    save_settle = SAVE_SETTLE;
  else if (save_settle && !--save_settle)
    speed_window_restart();            /* (the game's speed taken afresh after the save) */
  /* (r19-fix, core fix 2) nothing owed once the save and its settling are over, nor while the
   * game is in the slow-game mode: its lateness then is its own (it never got back to 2 frames behind to
   * make it up: owe grew a frame in 5 for as long as the game was slow, the save's state stuck on, and
   * a light scene later paid it back as seconds of a game at twice its speed) */
  if (owe && ((!busy && !save_settle) || rc_slow)) {
    owe_dropped += owe;
    owe = 0;
  }
  save_active = busy || bank || save_settle;
  {
    uint32_t b_ms = (uint32_t)((uint64_t)bank_frames() * 1000u / rate);
    if (b_ms > bank_peak_ms)
      bank_peak_ms = b_ms;
  }
  if (busy && !ff_mode && !rc_slow && nspire_frameskip_behind) {
    /* (r19-bank) a save's parts and calls make the frames late; a game without much spare time can't
     * catch that up at once, and the frame loop drops what is more than 6 frames late (its start-over):
     * the sound's ring would stay short of it for good. Past 4 frames late the rest is held and owed,
     * made up a frame at a time as the game has the time. (r19-fix: while the save is under way only, and
     * no more than its own time: a game slower than its clock is late by itself; past that, the frame
     * loop's start-over as without a save) */
    uint32_t f = FRAME_Q8 >> 8, late = nspire_frameskip_behind(), cap = (save_cost_ticks + f - 1) / f;
    if (late > 4 * f && save_owed < cap) {
      uint32_t n = (late - 4 * f) / f + 1;
      if (n > cap - save_owed)
        n = cap - save_owed;
      clock_hold(n);
      owe += n;
      owed += n;
      save_owed += n;
    }
  }
  if (owe && !ff_mode && (!nspire_frameskip_behind || nspire_frameskip_behind() < 2 * (FRAME_Q8 >> 8)) &&
      ++repay_n >= REPAY_EVERY) {
    /* an OS call's time the bank couldn't pay: made up a frame at a time, up to 2 frames behind (a game
     * with no spare time makes it up with pictures left out: the frame loop's rule for frames run ahead);
     * r19-fix: every other frame at most (no run of frames at twice the game's speed) */
    repay_n = 0;
    clock_ahead();
    owe--;
    repaid++;
  }
  if (!busy) {
    save_waiting = 0;
    if (bank_ff_ms) {
      /* fast forward's eased off at BANK_FF_OFF_MS a second: its rate takes the fill back that fast with
       * the fill within the 3 frames over its target a frame's sound is left out at (rate_update's rc_high) */
      uint32_t now = nspire_ticks(), off = (uint32_t)((uint64_t)(now - bank_ff_at) * BANK_FF_OFF_MS / NSPIRE_TICK_HZ);
      if (off) {
        bank_ff_ms = bank_ff_ms > off ? bank_ff_ms - off : 0;
        bank_ff_at = now;
      }
    }
    save_active = bank || bank_ff_ms || save_settle;
    if (!bank)
      return 1;
    if (ff_mode) {
      bank = 0;                        /* (fast forward: no clock; its rate takes the fill back) */
      return 1;
    }
    if (++bank_hold_n < BANK_HOLD_EVERY)
      return 1;
    bank_hold_n = 0;
    /* given back while the ring holds it (a menu, a load, a clock started over: gone with it) */
    if (rc_fill_valid && (rc_fill >> 4) > rc_base + frame_sound() + frame_sound() / 2) {
      clock_hold(1);
      speed_window_restart();
      bank_held++;
      bank--;
    } else
      bank = 0;
    save_active = bank || save_settle;
    return 1;
  }
  if (priming)
    return 1;                          /* (the sound only starting: nothing heard to keep) */
  if (!save_waiting) {
    save_waiting = 1;
    save_wait_t0 = nspire_ticks();
  }
  /* (the take: the game's copy, the frame's end right after it: a transfer's margin; nothing banked for
   * it, TAKE_WAIT_FRAMES at most for a frame's end that holds it) */
  need = next == NSPIRE_SAVE_TAKE ? SAVE_TAKE_MS + UA_NF + 4 : save_need_ms();
  /* (an OS call next: the queue served first, as the call's own start does; a CPU part: the ring as it is,
   * no window's time spent) */
  ms = os_next ? nspire_audio_ring_ms() : (int)((uint64_t)ring_used(&output) * 1000u / rate);
  if (ms < 0)
    return 1;
  have = (uint32_t)ms;
  {
    /* the ring's own level after the OS's turns without the bank (rate_update's average; the rate
     * holds it near its target only slowly) */
    uint32_t fill = rc_fill_valid ? rc_fill >> 4 : rc_base, b = bank_frames();
    base_ms = (uint32_t)((uint64_t)(fill > b ? fill - b : 0) * 1000u / rate);
  }
  fs_ms = (uint32_t)((uint64_t)frame_sound() * 1000u / rate);
  cap = bank_cap_ms();
  if (need > cap)
    need = cap;
  /* the bank this needs: the ring at need (it holds about its target, less what the windows take); a
   * need within a frame's sound of the fill is waited for (the ring's level moves by a transfer from one
   * frame's end to the next): nothing run ahead, no picture left out for it */
  want = need > base_ms + fs_ms && next != NSPIRE_SAVE_TAKE ? need - base_ms : 0;
  if (next == NSPIRE_SAVE_TAKE && take_late && take_waited < TAKE_LATE_FRAMES) {
    take_waited++;
    return 0;                          /* (r19-fix: a save after the last one: the ring and the queue back first) */
  }
  if (next == NSPIRE_SAVE_TAKE && have < need && take_waited++ >= (take_late ? 2 : 1) * TAKE_WAIT_FRAMES)
    have = need;                       /* (waited enough: taken now, its end faded first) */
  /* (r19-bank) and the frames caught up from the last part first (a call at every frame's end in a game
   * with little spare time made the frames late on and on: the ring fell by what they couldn't catch up),
   * LATE_WAIT_FRAMES at most */
  if (os_next && !ff_mode && clock_late() && late_waited < LATE_WAIT_FRAMES) {
    late_waited++;
    return 0;
  }
  /* (r19-fix: a game that can't keep its speed runs no frame ahead: no bank comes. In the slow-game mode,
   * or measured below it for SLOW_NO_BANK frames (the mode itself isn't entered in a save's frames), the
   * call now, not after SAVE_WAIT_MS each: a save with a long call learned took 20 s in a game at 46 a
   * second, its state on all along: no slow-game mode, no re-prime) */
  if (os_next && (have >= need || nspire_ticks() - save_wait_t0 >= MS_TICKS(SAVE_WAIT_MS) ||
                  (!ff_mode && (rc_slow || rc_below >= SLOW_NO_BANK)) ||
                  (!ff_mode && want && bank * fs_ms >= want && !clock_late()))) {
    late_waited = take_waited = 0;
    if (next == NSPIRE_SAVE_TAKE)
      take_late = 0;
    save_waiting = 0;
    save_calls++;
    if (have < need)
      save_short++;
    /* (a call let go short, or held by a transfer's margin only: its end faded first, put back if the
     * call didn't reach it; well covered: no fade to reach) */
    save_fade_next = have < need + UA_NF;
    call_is_take = next == NSPIRE_SAVE_TAKE;
    return 1;
  }
  if (ff_mode) {
    /* (its rate holds the target itself: the bank over the target, not over the fill) */
    uint32_t t_ms = (uint32_t)((uint64_t)rc_base * 1000u / rate);
    want = need > t_ms ? need - t_ms : 0;
    bank_ff_ms = want > bank_ff_ms + BANK_FF_STEP_MS ? bank_ff_ms + BANK_FF_STEP_MS : want;   /* (eased on: its rate) */
    bank_ff_at = nspire_ticks();
  }
  else if (bank * fs_ms < want && !clock_late() && ++bank_every_n >= BANK_EVERY) {
    /* a frame's sound more: its picture left out by the rule; spread out (one, then the frames
     * catch up in their spare time), never while the clock is still behind */
    bank_every_n = 0;
    clock_ahead();
    bank++;
    banked++;
  }
  if (!os_next)
    save_cost_ticks += MS_TICKS(CPU_PART_MS);   /* (a CPU part now: what it may make the frames late) */
  return !os_next;
}

/* after a save's OS call of 'ticks': its time not made up (the bank paid it) */
static void bank_after_call(uint32_t ticks)
{
  uint32_t f = FRAME_Q8 >> 8, n, k, r, ms;
  int take = call_is_take;
  call_is_take = 0;
  if (ticks > os_call_longest)
    os_call_longest = ticks;
  if (!take && save_active) {
    /* (the take is the game's copy, not the OS's: not learned) */
    uint32_t learned = *save_call();
    if (ticks > save_longest_now)
      save_longest_now = ticks;
    ms = (uint32_t)((uint64_t)ticks * 1000u / NSPIRE_TICK_HZ);
    if (!save_spike_ticks && learned > SAVE_FLOOR_MS && ms >= learned - learned / 4)
      save_spike_ticks = ticks;     /* (this save's spike) */
    else if (ticks > save_other_ticks)
      save_other_ticks = ticks;
    if (ms > learned)
      *save_call() = ms;            /* (learned at once) */
  }
  if (save_active)
    save_cost_ticks += ticks;       /* (r19-fix: the save's own time, what it may owe) */
  if (ff_mode || !save_active || !rendering)
    return;
  n = (ticks + f / 2) / f;
  k = n < bank ? n : bank;
  if (!k && n < 5)
    return;                         /* (no bank: the frame loop takes a short call's time back itself) */
  bank -= k;
  bank_held_back += k;
  r = n - k;                        /* (what the bank couldn't pay: up to 4 frames caught up by the frame loop,
                                     * the rest made up a frame at a time: never the clock's start-over) */
  r = r > 4 ? r - 4 : 0;
  if (rc_slow || save_owed + r > (save_cost_ticks + f - 1) / f)
    r = rc_slow ? 0 : (save_cost_ticks + f - 1) / f - save_owed;   /* (r19-fix: the save's own time at most) */
  owe += r;
  owed += r;
  save_owed += r;
  clock_hold(k + r);
  speed_window_restart();
}

/* frames of the resampler's output: the first FADE_FRAMES after a call that
 * ran the ring dry fade in */
static void fade_in(int16_t *lr, unsigned frames)
{
  unsigned i;
  for (i = 0; i < frames && fade_in_left; i++, fade_in_left--) {
    int32_t g = (int32_t)(FADE_FRAMES - fade_in_left);
    lr[2 * i] = (int16_t)(lr[2 * i] * g / FADE_FRAMES);
    lr[2 * i + 1] = (int16_t)(lr[2 * i + 1] * g / FADE_FRAMES);
  }
}

int nspire_audio_stop_within(unsigned ms)
{
  /* An entered OS callback still executes our code. Do not hand the OS
   * its keypad back or unload until it and its DMA storage are retired,
   * bounded by the caller's exit USB budget (nspire_exit.h). */
  if (!initialized) return 0;
  discard();
  int shown = 0;
  uint32_t t0 = nspire_ticks();
  for (;;) {
    uint32_t old = nspire_irqs_unmask();
    int pending = ua_shutdown(sleep_tick);
    nspire_irqs_restore(old);
    nspire_os_after_os_time();
    if (!pending) break;
    if ((uint64_t)(nspire_ticks() - t0) * 1000 >= (uint64_t)ms * NSPIRE_TICK_HZ)
      return -1;
    if (!shown && nspire_audio_draining) {
      nspire_audio_draining();
      shown = 1;
    }
    sleep_tick();
  }
  initialized = 0;
  rate = 0;
  sec_on = 0;
  nspire_os_want(OS_WANT_AUDIO, 0);
  return 0;
}

/* (r19-usb-exit) The quit's stop when no device was ever offered to the
 * OS's drivers and the port is quiet (nspire_usbpad_exit_untouched): the
 * driver never had a card (registered, waiting) and no callback of its ran
 * or can start, so its shutdown's sleeps (10 OS ticks after the retire, for
 * callback epilogues: ~100 ms, and on a run that never slept before, the
 * first look for the OS's sleep with them: the CX's 669 ms) wait for
 * nothing: ua_shutdown without its sleep (its own path for none). Anything
 * else (a card, a callback inside): -2, and the caller's usual stop. */
int nspire_audio_stop_quiet(void)
{
  if (!initialized) return 0;
  if (ua_state() != UA_WAITING) return -2;
  discard();
  {
    uint32_t old = nspire_irqs_unmask();
    int pending = ua_shutdown(NULL);
    nspire_irqs_restore(old);
    if (pending) return -2;   /* (a callback inside after all: the usual stop finishes it) */
  }
  initialized = 0;
  rate = 0;
  sec_on = 0;
  nspire_os_want(OS_WANT_AUDIO, 0);
  return 0;
}

void nspire_audio_stop(void)
{
  (void)nspire_audio_stop_within(1500);
}

void nspire_audio_start(void)
{
  if (initialized) return;
  sound_set_output_enabled(0);
  if (ua_init()) return;
  initialized = 1;
  ring_init(&output);             /* no stream exists yet */
  rendering = 0;
  priming = 1;
  rate = dropped = 0;
  last_state = UA_WAITING;
  generation = sound_output_generation;
  settings = setting_word();
  work_avg = work_peak = 0;
  rc_speed = rc_game = RC_ONE;
  rc_i = 0;
  rc_slow = 0;
  win_trusted = 1;
  win_misses = 0;
  stalled = 0;
  windows = windows_served = window_ticks = window_completions = stalls = distrusts = 0;
  sleeps_forced = sleeps_near = trims = reprimes = trimmed = 0;
  comp_last = 0;
  comp_at = served_at = nspire_ticks();
  rate_reset();
  save_reset();                     /* (r19-bank: a save's gate as at the start; what was learned kept) */
  if (!sec_t0) sec_t0 = nspire_ticks() | 1;
  if (!exit_set) exit_set = atexit(nspire_audio_stop) == 0;
}

/* The controller's frame counter for the driver's timing (late transfers,
 * the queue's least): FRINDEX, read only (as the music player reads it). */
static uint32_t usb_frame_register(void)
{
  switch (nspire_usb_kind()) {
  case 2: return 0xB000001Cu;     /* CX II FOTG210 */
  case 1: return 0xB000014Cu;     /* CX ChipIdea */
  default: return 0;
  }
}

/* ---- the per-second record ---- */
static void sec_begin(uint32_t now)
{
  uint32_t n;
  sec_start = now;
  sec_frames = nspire_perf_totals.frames;
  sec_drawn = nspire_perf_totals.drawn;
  sec_sleeps = nspire_os_sleeps();
  sec_os = nspire_os_busy_slept(&n);
  sec_forced = sleeps_forced;
  sec_near = sleeps_near;
  sec_ring_min = 0xFFFFFFFFu;
  sec_ring_max = 0;
  sec_longest = sec_away = 0;
  sec_win = sec_win_served = sec_win_ticks = sec_trims = sec_reprimes = 0;
  sec_flags = 0;
}

static void note_away(uint32_t since)
{
  if (since > sec_away) sec_away = since;
}

/* (the frame counters start over at a menu, a state, fast forward:
 * nspire_reset_timing; the second then counts from 0) */
static uint32_t since_count(uint32_t now_v, uint32_t then_v)
{
  if (now_v < then_v) {
    sec_flags |= SF_RESET;
    return now_v;
  }
  return now_v - then_v;
}

static uint8_t u8_of(uint32_t v) { return v > 255 ? 255 : (uint8_t)v; }
static uint16_t u16_of(uint32_t v) { return v > 65535 ? 65535 : (uint16_t)v; }

static void sec_take(uint32_t now)
{
  struct sec_rec *r = &secs[sec_n % SEC_KEEP];
  struct ua_second s;
  uint32_t n, os = nspire_os_busy_slept(&n);
  int64_t sp = ((int64_t)rc_speed - RC_ONE) * 10000 / RC_ONE;
  ua_take_second(&s);
  memset(r, 0, sizeof(*r));
  r->t = (now - sec_t0) / NSPIRE_TICK_HZ;
  r->frames = u8_of(since_count(nspire_perf_totals.frames, sec_frames));
  r->drawn = u8_of(since_count(nspire_perf_totals.drawn, sec_drawn));
  r->sleeps = u8_of(nspire_os_sleeps() - sec_sleeps);
  r->forced = u8_of(sleeps_forced - sec_forced);
  r->near = u8_of(sleeps_near - sec_near);
  r->flags = (uint8_t)sec_flags;
  r->os_ms = u16_of((uint32_t)((uint64_t)since_count(os, sec_os) * 1000 / NSPIRE_TICK_HZ));
  r->longest = u16_of((uint32_t)((uint64_t)sec_longest * 1000 / NSPIRE_TICK_HZ));
  if (rate) {
    r->ring_min = sec_ring_min == 0xFFFFFFFFu ? 0 : u16_of(sec_ring_min * 1000 / rate);
    r->ring_max = u16_of(sec_ring_max * 1000 / rate);
  }
  r->ahead_min = s.ahead_min == 0xFFFFFFFFu ? 255 : u8_of(s.ahead_min);
  r->silent_ms = u8_of(s.silence_ms);
  r->late_ms = u8_of(s.late_ms);
  r->restarts = u8_of(s.restarts);
  r->failed_ms = u8_of(s.failed_ms);
  r->away_ms = u8_of((uint32_t)((uint64_t)sec_away * 1000 / NSPIRE_TICK_HZ));
  r->win = u8_of(sec_win);
  r->win_served = u8_of(sec_win_served);
  r->win_10us = u16_of((uint32_t)((uint64_t)sec_win_ticks * 100000 / NSPIRE_TICK_HZ));
  r->trims = u8_of(sec_trims);
  r->reprimes = u8_of(sec_reprimes);
  r->speed = (int16_t)(sp < -32768 ? -32768 : sp > 32767 ? 32767 : sp);
  sec_n++;
  log_pending = 1;
}

void nspire_osd_message(const char *format, ...) __attribute__((weak));

/* Why the sound device on the port is not used, in the screen's words (at
 * most 50 characters): one whose formats this driver has none of (USB Audio
 * Class 2 only: most USB-C headphones with their own DAC at high speed; a
 * microphone alone; no 8/16-bit PCM), or one whose start-up stopped at a
 * step (the Sound page's log has which). */
static const char *failed_text(void)
{
  unsigned step = 0, status = 0;
  ua_failure(&step, &status);
  if (step == S_CHOOSE)
    return ua_desc()->info.any_uac2 ? "USB sound: not supported (USB Audio 2 device)" :
                                      "USB sound: this device isn't supported";
  return "USB sound didn't start. Plug it in again.";
}

/* Once a second while the adapter streams: kept in memory (a file write
 * while it plays stalls the USB callbacks 100-290 ms), written at exit. */
static void sec_poll(void)
{
  uint32_t now = nspire_ticks();
  if (!streaming()) {
    sec_on = 0;
    return;
  }
  if (!sec_on) {
    sec_on = 1;
    ua_take_second(&(struct ua_second){0});   /* the driver's second starts here */
    sec_begin(now);
    return;
  }
  if (rendering) sec_flags |= SF_RENDER;
  if (priming && rendering) sec_flags |= SF_PRIMING;
  if (rc_slow) sec_flags |= SF_SLOW;
  if (stalled) sec_flags |= SF_STALL;
  if (!win_trusted) sec_flags |= SF_NOWIN;
  if (bt_rendering) sec_flags |= SF_BT;
  if (card_idle) sec_flags |= SF_IDLE;
  if (now - sec_start >= NSPIRE_TICK_HZ) {
    sec_take(now);
    sec_begin(now);
  }
}

/* The queue's service where no frame loop plans it (menus, other callers,
 * the backstop at a frame's end). anticipate: the next poll is taken to
 * come as long after this one as this one after the last (not at a frame's
 * end, where nspire_audio_os_time comes next). */
static void serve_at_poll(int anticipate)
{
  uint32_t now = nspire_ticks(), gap = now - last_poll, ahead;
  last_poll = now;
  if (!(nspire_os_wanted & OS_WANT_AUDIO))
    return;
  note_completions(now);
  ahead = anticipate ? (gap < MS_TICKS(AWAY_MS) / 2 ? gap : MS_TICKS(AWAY_MS) / 2) : 0;
  if (by_windows()) {
    uint32_t since = since_served(now);
    note_away(since);
    /* (a sleep if the queue needs it and the window gave nothing; the
     * queue's clock looks as far as the gap since the last poll, up to the
     * queue at a completion) */
    (void)serve_queue(now, since, anticipate ? (gap < MS_TICKS((UA_NX - 1) * UA_NF) ? gap : MS_TICKS((UA_NX - 1) * UA_NF)) : 0,
                      ahead);
    if (nspire_os_since_sleep() + ahead >= away_limit())
      os_sleep_counted(&sleeps_forced);   /* the OS's task time */
  } else {
    uint32_t need = nspire_os_since_sleep();
    if (streaming() && !card_idle)
      note_away(need);
    if (need + ahead >= away_limit())
      os_sleep_counted(&sleeps_forced);
  }
  check_stall(nspire_ticks());
}

static void poll(int anticipate)
{
  struct ua_event e;
  int st;
  if (!initialized) return;
  /* State loads and settings changes must clear the old producer queue
   * before the first yield can refill another USB transfer from it. */
  uint32_t changed = setting_word();
  if (generation != sound_output_generation || settings != changed) {
    discard();
    settings = changed;
  }
  nspire_os_want(OS_WANT_AUDIO, service_wanted());
  serve_at_poll(anticipate);
  stop_poll();
  st = ua_state();
  if (st == UA_READY) {
    rate = ua_desc()->rate;
    discard();
    ua_set_frame_clock(usb_frame_register());
    uint32_t old = nspire_irqs_unmask();
    if (rate && rate <= 192000) (void)ua_start(&output);
    else ua_stop(NULL);
    nspire_irqs_restore(old);
    nspire_os_after_os_time();
    comp_at = served_at = nspire_ticks();   /* a new stream: nothing missed yet */
    stalled = 0;
    win_misses = 0;
    {
      struct ua_counters c;
      ua_counters(&c);
      q_starved0 = c.starved;     /* (speed18: the queue's clock trusted anew for each stream) */
      q_trusted = 1;
    }
    st = ua_state();
  }
  if (st == UA_FAILED || st == UA_GONE) {
    if (last_state != st || rendering) discard();
    stalled = 0;
    if (st == UA_FAILED) {
      /* While still attached in failure, drain without blocking the loop;
       * once UA_GONE, detach already freed or quarantined its block. */
      uint32_t old = nspire_irqs_unmask();
      ua_stop(NULL);
      nspire_irqs_restore(old);
      nspire_os_after_os_time();
    }
  }
  /* Hub (2026-11-16): the sound adapter's comings and goings into the pad
   * log (its own lines were the perf log's seconds only: the r13-hub run's
   * last sign of it was "sound 505s", then nothing of how its detach went). */
  if (ua_state() != last_state) {
    struct ua_counters c;
    uint32_t w2 = (ua_owns_usb() ? 1u : 0) | (ua_attached() ? 2u : 0);
    /* 2026-11-16: a sound device that can't be used says so on the screen
     * (it said nothing: such headphones looked as if never seen); the Sound page has
     * the same words (nspire_audio_summary). */
    if (ua_state() == UA_FAILED && nspire_osd_message)
      nspire_osd_message("%s", failed_text());
    ua_counters(&c);
#ifdef UA_HAS_MARKS
    /* (TI's abort marks: taken off, not as expected, stops without an abort; a byte each) */
    w2 |= (ua_osinfo()->marks_off > 255 ? 255u : ua_osinfo()->marks_off) << 8 |
          (ua_osinfo()->marks_odd > 255 ? 255u : ua_osinfo()->marks_odd) << 16 |
          (ua_osinfo()->drained > 255 ? 255u : ua_osinfo()->drained) << 24;
#endif
    nspire_padlog_add(PL_SOUND, ua_state(), last_state, 0, (uint32_t)ua_desc()->vid << 16 | ua_desc()->pid,
                      c.completions, w2);
    nspire_padlog_milestone(PL_SOUND, ua_state(), last_state, 0, (uint32_t)ua_desc()->vid << 16 | ua_desc()->pid,
                            c.completions, w2);
  }
  last_state = ua_state();
#ifdef UA_HAS_IDLE
  if (card_idle_wanted()) {
    if (!card_idle) {
      if (rendering) discard();
      card_idle = 1;
      card_idles++;
    }
    ua_set_idle(1);               /* every pass: the driver's keep-alive; a detach cleared it: asked again */
  } else if (card_idle) {
    card_idle = 0;
    if (streaming()) {
      discard();                  /* (refilled from the next frame; the driver drops what was in the ring) */
      comp_at = served_at = nspire_ticks();   /* the stream starts again: nothing missed yet */
      stalled = 0;
      win_misses = 0;
    }
    ua_set_idle(0);               /* now, not after the clear's acknowledgement (the driver's rule) */
  }
#endif
  nspire_os_want(OS_WANT_AUDIO, service_wanted());
  sec_poll();
  /* Drain diagnostic events without formatting or file I/O per frame. */
  for (unsigned i = 0; i < 64 && ua_take_event(&e); i++) { }
}

void nspire_audio_poll(void)
{
  poll(1);
}

/* A sleep that starts at 'at': the OS's first tick at or after it (from the
 * phase: next tick 'tick' after now, 'period' apart; unknown: a whole tick
 * on), and what of it comes past 'due', the next frame's time: 0 if none.
 * *free: the tick comes sleep_before's room before 'due' (the wait takes it). */
static uint32_t sleep_cost(uint32_t at, uint32_t due, uint32_t tick, uint32_t period, int known, int *free)
{
  if (!known || !period)
    tick = at + period;
  else if ((int32_t)(at - tick) > 0)
    tick += ((at - tick + period - 1) / period) * period;
  *free = (int32_t)(due - tick) >= (int32_t)OS_ROOM;
  if ((int32_t)(due - at) < 0)
    due = at;                                   /* late already: all of it */
  return (int32_t)(tick - due) > 0 ? tick - due : 0;
}

#define LOOK_FRAMES 4

/* nspire_audio.h: the frame's work is done, the next frame may start at
 * 'due' (paced: else the frames go back to back); the frame just done
 * started at 'began' (after its wait): its work.
 * First the queue: by windows, one now if SERVE_MS has gone by since it was
 * last served or the next frame's end would pass AWAY_MS (a sleep at once
 * if that one gave nothing and the queue cannot wait). Then the OS's task
 * time (the queue's too while windows are not trusted): the candidates are
 * this frame's end and the next ones' (their ends predicted from the work
 * and the frames' times) up to the last whose time since the OS's task last
 * ran is under the limit. A sleep at one costs what of it comes past the
 * next frame's time. If the wait now, or a later frame's, has the OS's tick
 * in its spare time, nothing now (free); if there is no later candidate, a
 * sleep now (forced); if now costs least for the time it buys, a sleep now
 * (placed); else the later one will. */
int nspire_audio_os_time(uint32_t due, uint32_t began, int paced)
{
  uint32_t now, since, limit, tick_in, period, cost0, e, d, work;
  uint64_t best_c = 1, best_t = 0;   /* the least cost per tick away so far (cost / time since the OS ran) */
  int known, free0, any = 0;
  if (!initialized || !(nspire_os_wanted & OS_WANT_AUDIO))
    return 0;
  now = nspire_ticks();
  if (!paced)
    due = now;
  work = began ? now - began : 0;
  work = work > os_call_ticks ? work - os_call_ticks : 0;   /* (speed18: the queue was served in an OS call) */
  os_call_ticks = 0;
  if (work > MS_TICKS(200)) work = MS_TICKS(200);    /* a pause, not a frame */
  work_avg = work_avg ? work_avg + (((int32_t)work - (int32_t)work_avg) >> 2) : work;
  if (work < work_avg) work = work_avg;               /* the next one as long as the longer */
  /* speed18: the queue's clock serves the queue only as late as the next
   * frame's end allows, so the next frame is taken as long as the longest
   * of the last ones (a drawn frame after skipped ones, a save's part) */
  work_peak = work > work_peak ? work : work_peak - (work_peak >> 4);
  if (!by_windows() && streaming() && work < work_peak)
    work = work_peak;                                 /* (sleeps serving the queue: as late as allowed too) */
  note_completions(now);
  if (by_windows()) {
    uint32_t s = since_served(now), spare = (int32_t)(due - now) > 0 ? due - now : 0;
    note_away(s);
    if (serve_queue(now, s, spare + (work_peak > work ? work_peak : work) + OS_ROOM, spare + work + OS_ROOM))
      return 1;                                       /* the queue could not wait: a sleep served it */
  } else if (streaming() && !stalled && !card_idle && now - probe_at >= NSPIRE_TICK_HZ &&
             since_served(now) >= MS_TICKS(12)) {
    probe_at = now;
    (void)serve_window(now);                          /* windows again? (trusted once one delivers) */
  }
  check_stall(now);
  since = nspire_os_since_sleep();
  if (!by_windows() && streaming())
    note_away(since);
  limit = away_limit();
  /* (2026-11-17, final/speed9a) the task's time with the tick's timer read:
   * the frame loop takes a tick as it comes, the first TASK_WATCH_MS short
   * of the limit or later (nspire_frameskip.c "A pad's OS time"), at the
   * cost of the USB task's 0.5-1 ms before it and the OS's own time, not the
   * wait for the tick; at the limit, a sleep now as before. (Its phase was
   * known only 82 ms after a wake: these come 250 ms apart, so they waited
   * for their tick, ~5 ms each.) */
  if (limit > MS_TICKS(AWAY_MS) && limit > MS_TICKS(TASK_WATCH_MS) &&
      nspire_frameskip_os_task(limit - MS_TICKS(TASK_WATCH_MS)))
    return since + work + OS_ROOM >= limit ? os_sleep_counted(&sleeps_forced) : 0;
  if (limit > MS_TICKS(AWAY_MS) && since + work < limit / 2)
    return 0;                                         /* the task's time: not yet looked for */
  known = nspire_os_next_tick_in(&tick_in, &period);
  cost0 = sleep_cost(now, due, now + tick_in, period, known, &free0);
  if (free0)
    return 0;
  e = now;
  d = due;
  for (unsigned k = 1; k <= LOOK_FRAMES; k++) {
    int free_k;
    uint32_t c;
    e = ((int32_t)(d - e) > 0 ? d : e) + work;        /* frame k's end */
    d = paced ? d + (FRAME_Q8 >> 8) : e;              /* frame k + 1's time */
    if (since + (e - now) + OS_ROOM >= limit)
      break;                                          /* (2 ms for a frame longer than its guess) */
    c = sleep_cost(e, d, now + tick_in, period, known, &free_k);
    if (free_k && since + (e - now) + k * OS_ROOM < limit)
      return 0;                                       /* a wait to come has the tick */
    /* c / (since + e - now) below best_c / best_t: a later sleep buys more
     * time away for what it costs */
    if (!any || (uint64_t)c * best_t < best_c * (since + (e - now))) {
      best_c = c;
      best_t = since + (e - now);
    }
    any = 1;
    if (limit > MS_TICKS(AWAY_MS) && k >= 2)
      break;                                          /* (the task's long limit: a near look will do) */
  }
  if (!any)
    return os_sleep_counted(&sleeps_forced);
  if (!cost0 || (uint64_t)cost0 * best_t <= best_c * (since ? since : 1))
    return os_sleep_counted(&sleeps_near);
  return 0;
}

/* speed18: the queue's audio beyond (UA_NX - 2) transfers, in the ring's
 * frames (0 when not known): served late and in twos and threes, the queue
 * holds what the ring did (the ring's low points are lower by a transfer or
 * two, the queue's higher); what the next completion needs from the ring is
 * the rest */
static uint32_t q_spare_frames(void)
{
  int32_t ahead, head, spare;
  if (!rate || !queue_now(&ahead, &head))
    return 0;
  spare = ahead - (int32_t)MS_TICKS((UA_NX - 2) * UA_NF);
  return spare > 0 ? (uint32_t)((uint64_t)spare * rate / NSPIRE_TICK_HZ) : 0;
}

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi)
{
  return v < lo ? lo : v > hi ? hi : v;
}

void nspire_osd_message(const char *format, ...) __attribute__((weak));

/* bt10: while the headphones and the adapter both play (Output Automatic,
 * both in), the adapter's sound is held back to come out with the
 * headphones' (theirs is late by their ring, what is sent ahead and their
 * own buffer: nspire_bt_glue_audio_delay_ms, the library's measured ring
 * and lead averaged, ~120 ms, and the sink's report or 150 ms): its ring's
 * target is that, less what the adapter's own queue adds (USB_PIPE_MS).
 * The ring (RING_FRAMES 16384 since bt10: 341 ms at 48 kHz) is the delay
 * line: no buffer of its own, no allocation, nothing costs more; the rate
 * control holds whatever target it is given. 3/4 of the ring less two
 * frames' room at the most: 307 ms at 44.1 kHz, 214 at 48; a card the
 * driver runs at 96 kHz (one with none of 48/44.1/32/24/22.05/16 kHz) is
 * held back 94 ms at most, the frames capped counted in the log.
 * bt10: the card not playing (it starts at the next frame), the
 * delay is taken as it is. Playing, the headphones coming or going re-sync
 * it at once; their delay moving by more than SYNC_SLACK_MS re-syncs it
 * once that has held SYNC_HOLD frames (2 s: not at every wobble), to the
 * average over them. Its ring's fill SYNC_SLACK_MS under where it had
 * settled 3 services running (a gap in the game's sound: an OS call drained
 * it, the card now ahead of the headphones) re-syncs it too (rate_update).
 * A re-sync steps
 * the card's volume down over SYNC_FADE - 1 frames (2/3, 1/3, 0: what the
 * driver refills from then on), starts its sound over at the new delay (a
 * re-prime: a moment of silence) and fades it in: no cut heard, no pitch
 * dragged for seconds. */
#define USB_PIPE_MS ((UA_NX - 1) * UA_NF + UA_NF / 2)   /* the queue ahead of the bus, and half a transfer */
#define SYNC_SLACK_MS 30
#define SYNC_HOLD 120
#define SYNC_FADE 4
static uint32_t sync_ms, syncs, sync_capped, sync_gaps;
static uint32_t sync_cand_sum, sync_cand_n, sync_next;
static unsigned sync_fade;

static uint32_t sync_max_ms(void)
{
  uint32_t m = rate ? (uint32_t)((uint64_t)(RING_FRAMES * 3 / 4) * 1000u / rate) : 0;
  return m > 2 * 17 ? m - 2 * 17 : 0;
}

/* a faded re-sync of the playing card to a delay of ms (sync_step: the fade, then the cut) */
static void resync(uint32_t ms)
{
  sync_next = ms;
  if (!sync_fade)
    sync_fade = priming ? 1 : SYNC_FADE;   /* (nothing heard yet: the cut at once) */
}

static void sync_with_headphones(int both)
{
  uint32_t want = 0, d, diff;
  if (both) {
    d = nspire_bt_glue_audio_delay_ms();
    want = d > USB_PIPE_MS ? d - USB_PIPE_MS : 0;
    if (want > sync_max_ms()) {
      want = sync_max_ms();
      sync_capped++;
    }
  }
  if (!(initialized && rendering)) {
    if (want != sync_ms)
      syncs++;
    sync_ms = want;               /* (primed at it from the start) */
    sync_fade = 0;
    sync_cand_n = sync_cand_sum = 0;
    return;
  }
  diff = want > sync_ms ? want - sync_ms : sync_ms - want;
  if ((want != 0) != (sync_ms != 0)) {
    resync(want);                 /* the headphones came or went */
    sync_cand_n = sync_cand_sum = 0;
    return;
  }
  if (diff <= SYNC_SLACK_MS) {
    sync_cand_n = sync_cand_sum = 0;
    return;
  }
  sync_cand_sum += want;
  if (++sync_cand_n < SYNC_HOLD)
    return;
  resync(sync_cand_sum / sync_cand_n);
  sync_cand_n = sync_cand_sum = 0;
}

/* each frame_begin: a re-sync's fade under way, then its cut (the sound
 * primed again at the new delay: rendering starts over below, faded in);
 * the card's volume now */
static unsigned sync_step(unsigned vol)
{
  if (!sync_fade)
    return vol;
  if (--sync_fade)
    return vol * (sync_fade - 1) / (SYNC_FADE - 1);   /* 2/3, 1/3, 0: the queue's sound steps down before the cut */
  sync_ms = sync_next;
  syncs++;
  if (initialized && rendering) {
    discard();
    fade_in_left = FADE_FRAMES;
  }
  return vol;
}

void nspire_audio_frame_begin(int mode)
{
  static int last_streaming;
  int want, is_streaming, ff, bt;
  if (initialized && rendering && !mode) discard();
  nspire_audio_poll();
  /* Bluetooth headphones (nspire_bt_glue.c): the game's sound goes to them
   * while they stream and the Output setting lets them have it (Automatic
   * or Bluetooth); the adapter's goes on beside them unless Output is
   * Bluetooth (bt10: both at once). A state load or a settings change
   * starts their sound clean (the generation). */
  is_streaming = initialized && streaming() && !stalled;
  bt = nspire_bt_live ? nspire_bt_glue_audio_frame(mode, is_streaming) : 0;
  if (bt) {
    if (!bt_rendering || bt_generation != sound_output_generation) {
      if (!(initialized && rendering))
        sound_discard_samples();  /* (the adapter's sound, playing, keeps the mixer's) */
      sound_set_output_enabled(1);
      bt_rendering = 1;
      bt_generation = sound_output_generation;
    }
  } else if (bt_rendering) {
    bt_rendering = 0;
    if (!(initialized && rendering)) {
      sound_set_output_enabled(0);
      sound_discard_samples();
    }
  }
  if (!initialized) {
    last_streaming = 0;
    last_mode = ff_mode = ff_seeded = 0;
    return;
  }
  ua_set_volume(sync_step((nspire_audio_volume % (NSPIRE_VOLUME_MAX + 1)) * 256u / NSPIRE_VOLUME_MAX));
  if (is_streaming && !last_streaming && nspire_osd_message) {
    if (!nspire_audio_enabled)
      nspire_osd_message("USB sound connected (Volume 0/%u)", (unsigned)NSPIRE_VOLUME_MAX);
    else if (!nspire_audio_volume)
      nspire_osd_message("USB sound connected (Volume 0/%u)", (unsigned)NSPIRE_VOLUME_MAX);
    else
      nspire_osd_message("USB sound connected");   /* (2026-11-16: it said nothing when all was well) */
  }
  last_streaming = is_streaming;
  ff = mode > 1;
  want = mode && nspire_audio_enabled && nspire_audio_volume && is_streaming &&
         nspire_audio_output != NSPIRE_OUTPUT_BT;   /* (Output Bluetooth: the headphones only) */
  if (want)
    sync_with_headphones(bt);     /* (not rendering: the delay as it was, until it renders again) */
  if (want && !rendering) {
    discard();
    ff_mode = ff;
    ff_seeded = 0;
    sound_set_output_enabled(1);
    rendering = 1;
    fade_in_left = FADE_FRAMES;   /* (r19-snd: the first sound after a pause fades in: no click) */
  } else if (!want && rendering) {
    discard();
  } else if (want && rendering && ff != ff_mode) {
    /* Switching between normal play and fast forward keeps the buffered stream
     * live (zero gap/pop) and resets the speed estimator for the new pace. */
    ff_mode = ff;
    ff_seeded = 0;
    rate_reset();
    if (!ff) {
      rc_slow = 0;
      rc_speed = (uint32_t)clamp32(RC_ONE + rc_i, RC_MIN, RC_MAX);
      resampler.step = (uint32_t)((uint64_t)base_step * rc_speed / RC_ONE);
      if (!resampler.step) resampler.step = 1;
    }
  }
  last_mode = mode;
}

/* The ring after the OS's turns (the Sound page's Buffer): Short holds a
 * game two frames behind its time (the frame loop lets it fall behind and
 * catch up), Medium and Long more (32/64/96 ms before 2026-11-15: Short ran
 * dry when a game at the edge of full speed fell behind). The delay to the
 * ear is this, the sound made since the OS's turn and the transfers
 * queued: Short ~110 ms at most in the pacing model (tests/audio). */
/* speed18: a window serves the queue as late as it can, so it takes two
 * or three of its transfers from the ring at once (28-42 ms with 14 ms
 * transfers, 16-24 with 8): the ring's low point after the OS's turn is
 * lower by as much; the target is higher by that (Short 48 -> 60 ms with 4
 * of 14; the ring plus the queue to the ear about as before: the queue is
 * shorter on average). */
#ifndef RING_EXTRA_MS
#define RING_EXTRA_MS (UA_NF > 8 ? 2 * (UA_NF - 8) : 0)
#endif
static uint32_t target_ms(void)
{
  uint32_t t = 48u + RING_EXTRA_MS + 24u * (nspire_audio_buffer % 3);
  return sync_ms > t ? sync_ms : t;   /* (bt10: with the headphones, their delay) */
}

static uint32_t target_frames(void)
{
  uint32_t target = rate * target_ms() / 1000;
  if (target > RING_FRAMES * 3 / 4) target = RING_FRAMES * 3 / 4;
  return target;
}

/* The resampler's factor for this frame (see the top). 'level' is the
 * ring before this frame's samples; 'serviced': the OS served the queue
 * since the last frame's end, so 'level' is what the USB task left.
 * Returns 1 to leave this frame's sound out (the ring far over its target). */
static int rate_update(uint32_t now, uint32_t level, int serviced)
{
  int32_t e = 0, target, step;
  uint32_t sp = 0;
  int have_sp = 0;
  rc_last_end = now;
  /* The game's speed: its last RC_WINDOW frames against as many GBA
   * frames (a frame late by an OS tick that the next ones catch up moves it
   * by little; the mode needs two seconds of it either way). */
  rc_ends[rc_frames % RC_WINDOW] = now;
  rc_frames++;
  {
    uint32_t max_win = ff_mode ? RC_FF_WINDOW : RC_WINDOW;
    uint32_t min_win = ff_mode ? 3 : RC_WINDOW / 8;
    uint32_t max_sp = ff_mode ? RC_MAX_FF : 2 * RC_ONE;
    uint32_t n = rc_frames - rc_first < max_win ? rc_frames - rc_first - 1 : max_win - 1;
    uint32_t span = now - rc_ends[(rc_frames - 1 - n) % RC_WINDOW];
    if (n >= min_win && span) {
      uint64_t v = (uint64_t)FRAME_Q8 * n * RC_ONE / ((uint64_t)span << 8);
      sp = v < RC_MIN ? RC_MIN : v > max_sp ? max_sp : (uint32_t)v;
      have_sp = 1;
    }
  }
  if (serviced && !priming) {
    rc_fill = rc_fill_valid ? rc_fill + (((int32_t)(level << 4) - (int32_t)rc_fill) >> 3) : level << 4;
    rc_fill_valid = 1;
  }
  if (rc_fill_valid && rc_target && !priming) {
    int64_t t16 = (int64_t)rc_target << 4;
    e = (int32_t)clamp32((int32_t)(((int64_t)rc_fill - t16) * RC_ONE / t16), -RC_ONE, RC_ONE);
    if (serviced) {
      rc_i = clamp32(rc_i + (int32_t)((int64_t)e * RC_KI / RC_ONE), -RC_DRIFT, RC_DRIFT);
      /* Far off the target (gpSP away: a file write, a long frame): not
       * chased by the rate. Under a quarter twice: re-prime; the latest
       * level over the target by 3 frames' sound 3 times: leave one out. */
      /* (speed18: with what the queue holds beyond its least at a
       * completion: served late, it holds more, the ring less) */
      /* (r19-bank: not while a save's calls drain it on purpose, what the bank paid made up after: a
       * call that did play silence re-primes itself, nspire_audio_os_call_end) */
      rc_low = level + q_spare_frames() < rc_base / 4 && !save_active ? rc_low + 1 : 0;
      /* bt10: held back for the headphones, the ring's fill dropping
       * SYNC_SLACK_MS under where it had settled (its slow average: the
       * rate holds the fill a frame and part of a transfer under the target,
       * and moves it by little) 3 services running: a gap in the game's
       * sound (an OS call drained it): the card now ahead of them: re-
       * synced (faded) */
      if (sync_ms && !sync_fade && !save_active) {
        if (sync_settle < 16) {
          sync_settle++;
          sync_slow = rc_fill;
        } else {
          sync_slow = (uint32_t)((int32_t)sync_slow + (((int32_t)rc_fill - (int32_t)sync_slow) >> 9));
        }
        low_sync = sync_settle >= 16 && rc_fill + ((rate * SYNC_SLACK_MS / 1000) << 4) < sync_slow ? low_sync + 1 : 0;
        if (low_sync >= 3) {
          low_sync = 0;
          sync_gaps++;
          resync(sync_ms);
        }
      } else if (!save_active) {
        sync_settle = low_sync = 0;
      } else
        low_sync = 0;                 /* (a save's call drained it: made up, not re-synced) */
    }
    /* (r19-bank: while a save's calls and their make-up move the fill, only a ring nearly full) */
    rc_high = level > (save_active ? (uint32_t)RING_FRAMES - 2 * (rate * 17 / 1000) : rc_target + 3 * (rate * 17 / 1000))
              ? rc_high + 1 : 0;
  }
  /* Fast forward: track the measured emulation speed smoothly up to 16x
   * with proportional ring-fill stabilization so the pitch speeds up
   * continuously without overflowing or starving the USB ring. */
  if (ff_mode) {
    int32_t corr, diff;
    if (have_sp) {
      if (!ff_seeded) {
        rc_game = sp;
        ff_seeded = 1;
      } else {
        uint32_t win = rc_frames - rc_first < RC_FF_WINDOW ? rc_frames - rc_first - 1 : RC_FF_WINDOW - 1;
        int shift = win < 8 ? 2 : 3;
        rc_game = (uint32_t)((int32_t)rc_game + (((int32_t)sp - (int32_t)rc_game) >> shift));
      }
    } else if (!ff_seeded) {
      rc_game = rc_speed;
    }
    corr = (rc_fill_valid && rc_target && !priming) ? clamp32(e / 5, -RC_ONE / 5, RC_ONE / 3) : 0;
    target = clamp32((int32_t)((int64_t)rc_game * (RC_ONE + corr) / RC_ONE), RC_MIN, RC_MAX_FF);
    diff = target - (int32_t)rc_speed;
    if (diff < 0) diff = -diff;
    step = diff / 4;
    if (step < RC_STEP_FULL) step = RC_STEP_FULL;
  } else {
    /* The slow-game mode: in after two seconds below 97% of full speed, or
     * at once (8 frames) below it with the ring half empty: the sound cannot
     * wait two seconds of a shortfall (Short holds 48 ms); out after two
     * seconds at 99% or more. */
    if (have_sp) {
      rc_below = sp < RC_SLOW_IN ? rc_below + 1 : 0;
      if (!rc_slow) {
        rc_hold = sp < RC_SLOW_IN && !save_active ? rc_hold + 1 : 0;   /* (r19-bank: not a save's frames) */
        if (rc_hold >= RC_HOLD || (rc_hold >= 8 && e < -RC_ONE / 2)) {
          rc_slow = 1;
          rc_hold = 0;
          rc_game = sp;
        }
      } else {
        rc_game = (uint32_t)((int32_t)rc_game + (((int32_t)sp - (int32_t)rc_game) >> 5));
        rc_hold = sp >= RC_SLOW_OUT ? rc_hold + 1 : 0;
        if (rc_hold >= RC_HOLD) {
          rc_slow = 0;
          rc_hold = 0;
        }
      }
    }
    if (rc_slow) {
      /* the game's speed, the fill held within +-2% */
      target = (int32_t)((int64_t)rc_game * (RC_ONE + clamp32(e / 50, -RC_ONE / 50, RC_ONE / 50)) / RC_ONE);
      step = RC_STEP_SLOW;
    } else {
      target = RC_ONE + clamp32((int32_t)((int64_t)e * RC_KP / RC_ONE) + rc_i, -RC_DRIFT, RC_DRIFT);
      step = (int32_t)rc_speed > RC_ONE + RC_DRIFT || (int32_t)rc_speed < RC_ONE - RC_DRIFT ? RC_STEP_SLOW
                                                                                           : RC_STEP_FULL;
    }
    target = clamp32(target, RC_MIN, RC_MAX);
  }
  rc_speed = (uint32_t)clamp32(target, (int32_t)rc_speed - step, (int32_t)rc_speed + step);
  resampler.step = (uint32_t)((uint64_t)base_step * rc_speed / RC_ONE);
  if (!resampler.step) resampler.step = 1;
  if (rc_low >= 2) {
    rc_low = 0;
    reprimes++;
    sec_reprimes++;
    priming = 1;                  /* silence until the ring holds its target again */
    ua_set_pause(1);
    rc_fill_valid = 0;
  }
  if (rc_high >= 3) {
    rc_high = 0;
    trims++;
    sec_trims++;
    return 1;
  }
  return 0;
}

/* The headphones' share of the frame's sound (bt10): the mixer read in
 * place by the library in one pass (nspire_bt_audio_write_mix: no copy, one
 * write a frame, as its rate control counts them), cleared by it unless the
 * adapter reads the same frames after it (both playing: its audio_mix does,
 * and moves the mixer's base). The old way (sound_read_samples's copy, 512
 * frames a write) where the mixer's buffer isn't word aligned (a damaged
 * state only). */
static void bt_frame_end(int usb_reads_too)
{
  s16 *buffer;
  u32 base, n, first;
  unsigned flags = (usb_reads_too ? 0u : 1u) | (nspire_audio_mono ? 2u : 0u);   /* NSPIRE_BT_MIX_CLEAR, _MONO */
  n = sound_samples_ready(&buffer, &base);
  if (n > 8 * 512)
    n = 8 * 512;
  if (((uintptr_t)buffer & 3) || (base & 1)) {
    int16_t in[512 * 2];
    if (usb_reads_too)
      return;                     /* (the adapter's three passes take them: the headphones skip a frame) */
    for (unsigned chunk = 0; chunk < 8; chunk++) {
      unsigned k = sound_read_samples(in, 512);
      if (!k) break;
      if (nspire_audio_mono)
        for (unsigned i = 0; i < k; i++)
          in[2*i] = in[2*i+1] = (int16_t)(((int32_t)in[2*i] + in[2*i+1]) / 2);
      nspire_bt_glue_audio_write(in, k);
    }
    return;
  }
  first = AUDIO_MIX_FRAMES - (base >> 1);
  if (n <= first)
    nspire_bt_glue_audio_write_mix((uint32_t *)(void *)buffer + (base >> 1), n, NULL, 0, flags);
  else
    nspire_bt_glue_audio_write_mix((uint32_t *)(void *)buffer + (base >> 1), first, (uint32_t *)(void *)buffer,
                                   n - first, flags);
  if (!usb_reads_too)
    sound_samples_taken(n);
}

void nspire_audio_frame_end(void)
{
  int16_t in[512 * 2], out[512 * 2];
  int usb = initialized && rendering && streaming();
  if (bt_rendering)
    bt_frame_end(usb);
  if (usb) {
    uint32_t now = nspire_ticks(), level = ring_used(&output);
    int leave_out;
    rc_base = target_frames();
    rc_target = rc_base + bank_frames();   /* (r19-snd: a save's bank is no fill error) */
    if (rc_target > RING_FRAMES - 3 * frame_sound())
      rc_target = RING_FRAMES - 3 * frame_sound();
    leave_out = rate_update(now, level, since_served(now) < now - rc_last_end);
    if (level < sec_ring_min) sec_ring_min = level;
    /* A frame is ~1097 input frames. The bound also handles unusual
     * callers without letting a backlog stall the emulator. */
    /* speed9a: read, resampled and put in one pass (nspire_audio_mix.h), the
     * same frames; the three passes below where it can't (a frame's sound
     * left out, the fade in after a dry ring, a ring that might not hold it) */
    int mixed = !leave_out && !fade_in_left && audio_mix(&resampler, &output, 8 * 512, nspire_audio_mono) >= 0;
    for (unsigned chunk = 0; chunk < 8 && !mixed; chunk++) {
      unsigned n = sound_read_samples(in, 512), used = 0;
      if (!n) break;
      if (leave_out) {
        trimmed += n;             /* (the resampler keeps its phase: a splice, not a jump) */
        continue;
      }
      while (used < n) {
        unsigned consumed = n - used;
        unsigned made = rs_run(&resampler, in + 2 * used, &consumed, out, 512);
        used += consumed;
        if (nspire_audio_mono)
          for (unsigned i = 0; i < made; i++)
            out[2*i] = out[2*i+1] = ((int32_t)out[2*i] + out[2*i+1]) / 2;
        if (fade_in_left)
          fade_in(out, made);
        dropped += made - ring_put(&output, out, made);
        if (!made && !consumed) break;
      }
    }
    level = ring_used(&output);
    if (level > sec_ring_max) sec_ring_max = level;
    if (priming && output.cleared_seq == output.clear_seq && level >= rc_base) {
      priming = 0;
      ua_set_pause(0);
    }
  }
  poll(0);
}

void nspire_audio_stats(struct nspire_audio_stats *s)
{
  struct ua_counters c;
  memset(s, 0, sizeof(*s));
  if (!initialized) return;
  ua_counters(&c);
  s->rate = rate; s->buffered = ring_used(&output); s->dropped = dropped;
  s->completions = c.completions; s->underruns = c.underrun_frames;
  s->rendering = rendering; s->priming = priming;
  s->speed = rc_speed;
  s->sleeps_forced = sleeps_forced; s->sleeps_near = sleeps_near;
  s->windows = windows; s->windows_served = windows_served; s->window_ticks = window_ticks;
  s->stalled = stalled; s->stalls = stalls; s->slow = rc_slow;
  s->trims = trims; s->reprimes = reprimes; s->win_trusted = win_trusted;
  s->sync_ms = sync_ms; s->syncs = syncs; s->sync_gaps = sync_gaps;
  s->card_idle = card_idle; s->keepalives = 0;
#ifdef UA_HAS_IDLE
  s->keepalives = c.keepalives;
#endif
  s->bank = bank;
  s->banked = banked;
  s->bank_held = bank_held;
  s->bank_held_back = bank_held_back;
  s->save_calls = save_calls;
  s->save_short = save_short;
  s->os_call_dry = os_call_dry;
  s->os_call_longest_ms = (uint32_t)((uint64_t)os_call_longest * 1000u / NSPIRE_TICK_HZ);
  s->os_call_dips = os_call_dips;
  s->owe = owe;
  s->owed = owed;
  s->repaid = repaid;
  s->owe_dropped = owe_dropped;
  s->save_active = save_active;
}

const char *nspire_audio_summary(void)
{
  static char text[112];
  if (!initialized) return ua_init_text();
  switch (ua_state()) {
  case UA_WAITING: case UA_GONE: return "Plug in a USB audio adapter";
  case UA_SETUP: case UA_READY: return "Opening USB audio...";
  case UA_STREAMING:
    if (stalled)
      return "USB audio: the adapter takes no sound";
    if (card_idle)
      return "USB audio idle (Output Bluetooth)";   /* (bt10: nothing queued) */
    snprintf(text, sizeof(text), "USB audio connected: %u Hz", (unsigned)rate);
    return text;
  case UA_FAILED:
    return failed_text();
  default:
    snprintf(text, sizeof(text), "USB audio stopped; unplug and reconnect");
    return text;
  }
}

int nspire_audio_log_pending(void)
{
  return log_pending && !initialized;
}

/* At exit (nspire_perf_log_flush, after nspire_audio_stop): the seconds
 * kept, oldest first. Nothing while the adapter is still open. */
void nspire_audio_log_write(FILE *f)
{
  unsigned first, i;
  struct ua_counters c;
  if (!nspire_audio_log_pending() || !f)
    return;
  ua_counters(&c);
  first = sec_n > SEC_KEEP ? sec_n - SEC_KEEP : 0;
  fprintf(f, "sound: %u transfers of %u ms queued, away at most %u ms; buffer %s (%u ms); %u seconds kept of %u;"
          " in all %lu completions, %lu silent packets (%lu frames), %lu restarts, %lu late packets, %lu errors,"
          " %lu frames dropped (ring full), least queued %lu ms; sleeps %lu forced, %lu placed\n",
          (unsigned)UA_NX, (unsigned)UA_NF, (unsigned)AWAY_MS,
          nspire_audio_buffer % 3 == 0 ? "short" : nspire_audio_buffer % 3 == 1 ? "medium" : "long",
          (unsigned)target_ms(), sec_n - first, sec_n,
          (unsigned long)c.completions, (unsigned long)c.underrun_pkts, (unsigned long)c.underrun_frames,
          (unsigned long)c.starved, (unsigned long)c.late_frames, (unsigned long)c.errors, (unsigned long)dropped,
          (unsigned long)(c.min_queued_ms == 0xFFFFFFFFu ? 0 : c.min_queued_ms),
          (unsigned long)sleeps_forced, (unsigned long)sleeps_near);
  fprintf(f, "sound: IRQ windows %lu, %lu with completions in them (%lu completions), %lu.%02lu ms in them in all"
          " (%lu us a window); trust lost %lu times; stalls %lu; re-primes %lu, frames left out %lu (%lu input frames);"
          " delay set to the headphones' %lu times (%lu after a gap in the sound), last %lu ms, capped %lu frames;"
          " the card idle %lu times\n",
          (unsigned long)windows, (unsigned long)windows_served, (unsigned long)window_completions,
          (unsigned long)((uint64_t)window_ticks * 1000 / NSPIRE_TICK_HZ),
          (unsigned long)((uint64_t)window_ticks * 100000 / NSPIRE_TICK_HZ % 100),
          (unsigned long)(windows ? (uint64_t)window_ticks * 1000000 / NSPIRE_TICK_HZ / windows : 0),
          (unsigned long)distrusts, (unsigned long)stalls, (unsigned long)reprimes, (unsigned long)trims,
          (unsigned long)trimmed / 2, (unsigned long)syncs, (unsigned long)sync_gaps, (unsigned long)sync_ms,
          (unsigned long)sync_capped, (unsigned long)card_idles);
  fprintf(f, "sound seconds: t game/pictures; OS sleeps (forced, placed) ms outside waits, the longest sleep ms;"
          " windows (with completions) ms; away the longest without the queue served ms; ring min-max ms;"
          " queue least ms; silent/late/failed ms, restarts; rate; p = paused (menu, fast forward), s = priming,"
          " w = slow game, x = stall (the adapter takes nothing), n = windows not trusted, r = counters reset,"
          " t/R = frames left out / re-primes, b = Bluetooth headphones took it too, i = the card idle\n");
  for (i = first; i < sec_n; i++) {
    const struct sec_rec *r = &secs[i % SEC_KEEP];
    int sp = r->speed;
    fprintf(f, "sound %lus %u/%u; os %u (%u, %u) %u ms, %u; win %u (%u) %u.%02u ms; away %u; ring %u-%u; queue %u;"
            " silent %u late %u failed %u, restarts %u; rate %c%d.%02d%%%s%s%s%s%s%s",
            (unsigned long)r->t, r->frames, r->drawn, r->sleeps, r->forced, r->near, r->os_ms, r->longest,
            r->win, r->win_served, r->win_10us / 100, r->win_10us % 100,
            r->away_ms, r->ring_min, r->ring_max, r->ahead_min, r->silent_ms, r->late_ms, r->failed_ms, r->restarts,
            sp < 0 ? '-' : '+', (sp < 0 ? -sp : sp) / 100, (sp < 0 ? -sp : sp) % 100,
            r->flags & SF_RENDER ? "" : " p", r->flags & SF_PRIMING ? " s" : "", r->flags & SF_SLOW ? " w" : "",
            r->flags & SF_STALL ? " x" : "", r->flags & SF_NOWIN ? " n" : "", r->flags & SF_RESET ? " r" : "");
    if (r->flags & SF_BT)
      fprintf(f, " b");
    if (r->flags & SF_IDLE)
      fprintf(f, " i");
    if (r->trims || r->reprimes)
      fprintf(f, " t%u R%u", r->trims, r->reprimes);
    fputc('\n', f);
  }
  log_pending = 0;
}

#else
#include "nspire_bt_glue.h"
void nspire_audio_start(void) { }
void nspire_audio_stop(void) { }
int nspire_audio_stop_within(unsigned ms) { (void)ms; return 0; }
int nspire_audio_stop_quiet(void) { return 0; }
void nspire_audio_poll(void) { }
/* (no mixing in this build: Bluetooth headphones get the modes, never sound;
 * gpsp-host's tests read the modes) */
void nspire_audio_frame_begin(int mode) { if (nspire_bt_live) (void)nspire_bt_glue_audio_frame(mode, 0); }
void nspire_audio_frame_end(void) { }
void nspire_audio_pause(void) { if (nspire_bt_live) (void)nspire_bt_glue_audio_frame(0, 0); }
int nspire_audio_os_time(uint32_t due, uint32_t began, int paced) { (void)due; (void)began; (void)paced; return 0; }
#ifdef NSPIRE_HOST
/* (the PC tests' stand-in for a sound adapter on a hub: host_usbpad.c "pad in sound") */
int host_usbaudio_active(void) __attribute__((weak));
int nspire_usbaudio_active(void) { return host_usbaudio_active && host_usbaudio_active(); }
#else
int nspire_usbaudio_active(void) { return 0; }
#endif
int nspire_usbaudio_owns_usb(void) { return 0; }
int nspire_usbaudio_busy(void) { return 0; }
int nspire_usbaudio_playing(void) { return 0; }
int nspire_usbaudio_streaming(void) { return nspire_usbaudio_active(); }
#ifdef NSPIRE_HOST
/* (2026-11-18: the stand-in's sound stalls on a hub whose power fell: host_usbpad.c "pad hub blip") */
int host_usbaudio_stalled(void) __attribute__((weak));
int host_usbaudio_idle(void) __attribute__((weak));
int nspire_usbaudio_stalled(void)
{
  if (host_usbaudio_idle && host_usbaudio_idle()) return -1;   /* (as the real one with UA_HAS_IDLE) */
  return host_usbaudio_stalled ? host_usbaudio_stalled() : -1;
}
uint32_t host_usbaudio_completions(void) __attribute__((weak));
uint32_t nspire_usbaudio_completions(void) { return host_usbaudio_completions ? host_usbaudio_completions() : 0; }
#else
int nspire_usbaudio_stalled(void) { return -1; }
uint32_t nspire_usbaudio_completions(void) { return 0; }
#endif
#ifdef NSPIRE_HOST
/* (2026-11-17: the PC tests' stand-in for a sound adapter that streams, the
 * OS's task time its sleeps': host_usbpad.c "pad sound serves") */
int host_audio_serves_os(void) __attribute__((weak));
int nspire_audio_serves_os(void) { return host_audio_serves_os && host_audio_serves_os(); }
#else
int nspire_audio_serves_os(void) { return 0; }
#endif
int nspire_audio_os_call_begin(uint32_t *cpsr) { (void)cpsr; return 0; }
int nspire_audio_serve_begin(uint32_t *cpsr) { (void)cpsr; return 0; }
void nspire_audio_serve_end(int began, uint32_t cpsr) { (void)began; (void)cpsr; }
int nspire_audio_ring_ms(void) { return -1; }
int nspire_audio_ring_level_ms(void) { return -1; }
void nspire_audio_serve_ahead(unsigned ms) { (void)ms; }
int nspire_audio_save_frame(int busy, int next) { (void)busy; (void)next; return 1; }
uint32_t nspire_audio_save_bank_ms(void) { return 0; }
void nspire_audio_os_call_end(int began, uint32_t cpsr) { (void)began; (void)cpsr; }
const char *nspire_audio_summary(void) { return "This build has no USB audio driver"; }
void nspire_audio_stats(struct nspire_audio_stats *s) { memset(s, 0, sizeof(*s)); }
int nspire_audio_log_pending(void) { return 0; }
void nspire_audio_log_write(FILE *f) { (void)f; }
#endif
