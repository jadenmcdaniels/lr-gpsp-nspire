/* The pacing model (2026-11-15, final/snd): gpSP's frame loop with the
 * adapter playing, in virtual time, against the audio driver's fake TI USB
 * stack. What it is made of:
 *   - the real nspire_audio.c, nspire_frameskip.c (pacing, automatic
 *     frameskip, fast forward), ua_os.c through nspire_audio_os.c, pcm.c,
 *     uac.c and the real APU (a tone channel), as the calculator builds them;
 *   - nspire_os.c's sleeps as the calculator has them (host_platform.c's
 *     model, here with the bus): TCC_Sleep(1) comes back at the OS's next
 *     tick (every OS_TICK ms of virtual time) plus the OS's own time at it
 *     (OS_WAKE); sleep_before only if that tick comes 2 ms before the
 *     deadline; sleep_near only if the phase is known and the tick near;
 *     nspire_wait_until as nspire/nspire.c's (sleeps while the OS is
 *     wanted, else spins);
 *   - the bus (fake_os.c, the CX II OS's scheduler read from its code): it
 *     runs all the time; the USB task (completions, re-queues) only while
 *     MAIN sleeps in the OS (fos_run), never while MAIN works (fos_stall);
 *   - the frame: run_frame's order (nspire_input_frame's keys_poll, then
 *     nspire_audio_frame_begin, nspire_frameskip_begin_frame, the work,
 *     nspire_frameskip_end_frame, nspire_audio_frame_end); the work is
 *     virtual time: a game part, plus a picture part when the frame is drawn,
 *     each with a spread.
 * Each case runs 2 s to come up and then MEASURE_S seconds measured: game
 * frames a second, pictures, OS sleeps and the time lost in them, silent
 * packets (the ring had nothing: the driver's underrun_pkts), frames of the
 * wire with nothing on it (the bus ran dry: fake_os starved_frames), and the
 * delay from game to ear (the ring plus the packets handed to the OS and
 * not on the wire, at a frame's end: the newest sample's), the longest
 * time without the OS, the resampler's factor. One line per case
 * (tests/audio/unit_pace.sh puts them in tests/audio/run.sh's PASS line;
 * tests/host/run.sh runs it with the sink tests).
 *
 *   pace_test [--case a|b|c|d|e|f|g|rule|lcd|save] [--tick MS] [--wake MS] [--irq-usb]
 *             [--town GAME SPREAD DRAW SPREAD] [--log] [--trace FROM TO] [--old]
 *             [--rule-off] [--lcd-period MS] [--lcd-window MS] [--lcd-wake MS]
 *             [--mid GAME SPREAD DRAW SPREAD] [--lcd-report] [--hub-old]
 * lcd (2026-11-17, final/pace6): the CX II's LCD as lcd_tune leaves it
 * (a refresh of --lcd-period, 16.70 ms by default), normal play and fast
 * forward with and without the adapter and a hub (case_lcd); --lcd-report
 * prints without checking; --hub-old has the pad glue's hub tick run while
 * the sound streams (2b8d4d6's; with PACE_SRC of that tree's files: the
 * before).
 * save (2026-11-17): a state's save and the game's own in Emerald with the
 * sound on, the parts at the CX II log's times, 2b8d4d6's way (-old) and
 * now (case_save); [--save-only LABEL] [--scale MS] (the picture's scaling).
 * rule: the pace rule's cases (2026-11-15, final/pace: case_rule), with no
 * adapter and with one streaming; --rule-off runs them with frameskip off
 * (the before on the reference calculators). The LCD is modelled since then
 * (a refresh each 1/60 s: fast forward's slots and its wait for the LCD).
 * --old: report only, no checks (tests/audio/pace_old.sh: faf629e's code,
 * which fails a-moving and b); --log: the perf log's sound lines, and each
 * silent packet or wire gap; --irq-usb: the OS's USB completions come in
 * an IRQ window too (the reading the probe's log line tells apart). */
#include "common.h"
#include "nspire_audio.h"
#include "nspire_os.h"
#include "nspire_frameskip.h"
#include "nspire_frontend.h"
#include "nspire_usbpad_glue.h"
#include "ua_os.h"
#include "fake/fake_os.h"
#include "corpus.h"
#include "t.h"
#include <math.h>
#include <stdlib.h>

extern u32 unit_ticks;
extern int unit_usb_plugged;
extern u32 current_frameskip_type, frameskip_value;
extern u32 skip_next_frame;

#define MEASURE_S 60
#define TICKS_PER_MS (NSPIRE_TICK_HZ / 1000.0)

/* ---- virtual time, the OS's ticks, the bus ---- */
static double os_tick_ms = 10.0;   /* the OS's tick (the CX II's measured 10.25; the task's 10) */
static double os_wake_ms = 1.0;    /* the OS's own time at its tick before the sleep returns (R11u: a
                                      sleep outside waits ~5.7 ms, its wait to the tick ~5 of it) */
static double vt;                  /* virtual time, in timer ticks */
static double last_wake;
static uint64_t bus_ms;            /* the bus's milliseconds so far */
static unsigned sleeps, sleeps_busy;
static double slept_busy;          /* ticks slept outside nspire_wait_until's idle wait */
static double away_max;            /* the longest the queue went without the OS serving it, ticks */
static double last_service;        /* a sleep's wake, an IRQ window that ran the completions */
static double os_gap_max;          /* the longest from a sleep's wake to the next sleep (a pad's reports' gap), ticks */
static void note_service(void)
{
  if (vt - last_service > away_max)
    away_max = vt - last_service;
  last_service = vt;
}
static int in_os;
static int old_code, show_log;
static uint32_t steady_seed;        /* --seed N (r19-pace): other frame costs, wakes, hiccups (a sweep's) */
static double trace_from = -1, trace_to = -1;   /* --trace FROM TO (s): each frame and sleep */
static int trace_run = 1;            /* (PACE_TRACE_RUN=LABEL: --trace in that save run only) */
static int tracing(void) { return trace_run && vt / NSPIRE_TICK_HZ >= trace_from && vt / NSPIRE_TICK_HZ <= trace_to; }
static unsigned ring_now(void)
{
  struct nspire_audio_stats s;
  nspire_audio_stats(&s);
  return s.buffered;
}

static void set_time(double t)
{
  vt = t;
  unit_ticks = (u32)(uint64_t)vt;
}

static void spend(double ticks)
{
  set_time(vt + ticks);
}

static uint64_t ms_now(void)
{
  return (uint64_t)(vt / TICKS_PER_MS);
}

/* MAIN's own time: the bus runs, the USB task does not. */
static void bus_masked(void)
{
  uint64_t ms = ms_now();
  if (ms > bus_ms) {
    fos_stall((unsigned)(ms - bus_ms));
    bus_ms = ms;
  }
}

static double next_os_tick(double t)
{
  double p = os_tick_ms * TICKS_PER_MS;
  return (floor(t / p) + 1) * p;
}

/* What the code can know
 * of the OS's tick, not the tick itself:
 *  - a wake comes the OS's own time after the tick (os_wake_ms) and a jitter
 *    on it (--jitter MS: 0 to MS more, each wake its own); r15's rule
 *    foretells the ticks from the last wake (anchored: late by that time).
 *    --oracle: the true ticks (the earlier model);
 *  - the tick's timer (nspire_os.c's probe passed: model_timer, the
 *    default; --no-timer: r15's rule): an SP804 of 21 counts a tick (the
 *    CX II's load 0x14), the tick as the count reaches 0, read as the code
 *    reads it (its count, nothing finer);
 *  - the USB task's time (--usb-task MS): the OS's USB task runs only until
 *    the tick (gpSP's task, higher, has the CPU after it), so a pad's report
 *    that needs MS of it comes in a sleep begun at least MS before its tick
 *    (pad_turns, pad_gap_max: the longest between two such sleeps). */
static int model_timer = 1, oracle;
static double wake_jitter_ms, usb_task_ms = 0.43;   /* (R11u: a report's completion took 0.43 ms of the USB task) */
static unsigned pad_turns;
static double last_pad_turn, pad_gap_max;
static uint32_t jrnd = 99991;
static double jitter(void)
{
  jrnd = jrnd * 1103515245u + 12345u;
  return wake_jitter_ms * ((double)(jrnd >> 8 & 0xFFFF) / 65536.0);
}
#define MODEL_LOAD 20u
static double count_ticks(void) { return os_tick_ms * TICKS_PER_MS / (MODEL_LOAD + 1); }
/* the timer's count at t: d counts to the tick (1..21; 21 is the 0 after the last tick) */
static uint32_t timer_value(double t)
{
  double c = count_ticks(), left = next_os_tick(t) - t;
  uint32_t d = (uint32_t)ceil(left / c - 1e-9);
  if (d < 1) d = 1;
  if (d > MODEL_LOAD + 1) d = MODEL_LOAD + 1;
  return d == MODEL_LOAD + 1 ? 0 : d;
}
/* the lines' inline check (nspire_lines.c os_watch): the count as a word */
static uint32_t model_reg, model_never;
struct nspire_os_watch nspire_os_watch_at = { &model_never, 0 };
static uint32_t model_reg_now(void)
{
  if (!model_timer)
    return 0;
  model_reg = timer_value(vt);
  return model_reg;
}
int nspire_os_tick_timer(void) { return model_timer; }
uint32_t nspire_os_tick_bracket(uint32_t *earliest, uint32_t *latest)
{
  double c = count_ticks();
  uint32_t v, d;
  if (!model_timer)
    return 0;
  v = timer_value(vt);
  d = v ? v : MODEL_LOAD + 1;
  *earliest = (uint32_t)((d - 1) * c);
  *latest = (uint32_t)ceil(d * c);
  return d;
}
uint32_t nspire_os_tick_need(uint32_t need)
{
  uint32_t d;
  if (!model_timer)
    return 0;
  d = (uint32_t)ceil(need / count_ticks() - 1e-9) + 1;
  return d <= MODEL_LOAD + 1 ? d : 0;
}
void nspire_os_watch_arm(uint32_t counts)
{
  nspire_os_watch_at.reg = model_timer ? &model_reg : &model_never;
  nspire_os_watch_at.value = model_timer && counts >= 2 ? counts : 0;   /* (the tick as the count reaches 0) */
}
static u32 gap_longest_t;
static uint32_t wake_late256_t;
uint32_t nspire_os_wake_late(void) { return (wake_late256_t + 128) >> 8; }
uint32_t nspire_os_gap_longest(void) { u32 g = gap_longest_t; gap_longest_t = 0; return g; }
void nspire_os_tick_rule_report(char *text, unsigned size)
{
  snprintf(text, size, "OS tick: %s (model)", model_timer ? "read from its timer" : "from the last wake");
}

/* (speed18) --fit: the OS's cost as fitted to the reference r16
 * log:
 * 0.41 ms a window plus 0.21 ms a completion run in it (and 0.21 more a
 * completion in a sleep); a window that runs none, its 0.3 ms spin. One
 * run's hand fit (Emerald, the dongle in): a prior, not a measurement. */
static double comp_ms = 0;
static uint32_t completions_now_model(void)
{
  struct ua_counters c;
  ua_counters(&c);
  return c.completions;
}

void (*nspire_os_wake_hook)(void);   /* (nspire_os.c's: called as a sleep wakes) */

int __wrap_nspire_os_sleep(void)
{
  double before = vt;
  uint64_t ms;
  if (in_os)
    return 0;
  in_os = 1;
  if (before - last_wake > gap_longest_t)
    gap_longest_t = (u32)(before - last_wake);
  /* the USB task's time before the tick: a pad's report if it is enough */
  if ((next_os_tick(vt) - vt) >= usb_task_ms * TICKS_PER_MS) {
    if (pad_turns && vt - last_pad_turn > pad_gap_max)
      pad_gap_max = vt - last_pad_turn;
    pad_turns++;
    last_pad_turn = vt;
  }
  if (show_log && before - last_service > 43 * TICKS_PER_MS)
    printf("  at %.3f s: a sleep (%s) %.1f ms after the queue was last served\n", before / NSPIRE_TICK_HZ,
           nspire_os_idle ? "in a wait" : "busy", (before - last_service) / TICKS_PER_MS);
  note_service();
  bus_masked();
  if (before - last_wake > os_gap_max)
    os_gap_max = before - last_wake;
  unsigned ring_before = tracing() ? ring_now() : 0;
  {
    /* (nspire_os.c measures the wake's lateness by the timer) */
    double tick = next_os_tick(vt);
    set_time(tick + (os_wake_ms + jitter()) * TICKS_PER_MS);
    if (model_timer) {
      uint32_t late = (uint32_t)(vt - tick);
      wake_late256_t = wake_late256_t ? wake_late256_t - wake_late256_t / 8 + (late << 5) : late << 8;
    }
  }
  ms = ms_now();
  {
    uint32_t c0 = completions_now_model();
    fos_run((unsigned)(ms - bus_ms));   /* serviced at once, then the bus with the USB task */
    bus_ms = ms;
    if (comp_ms > 0)
      set_time(vt + comp_ms * (completions_now_model() - c0) * TICKS_PER_MS);
  }
  if (tracing())
    printf("    %.4f sleep %s %.1f ms after %.1f away: ring %u -> %u\n", before / NSPIRE_TICK_HZ,
           nspire_os_idle ? "idle" : "busy", (vt - before) / TICKS_PER_MS, (before - last_wake) / TICKS_PER_MS,
           ring_before, ring_now());
  last_wake = last_service = vt;
  sleeps++;
  if (!nspire_os_idle) {
    sleeps_busy++;
    slept_busy += vt - before;
  }
  in_os = 0;
  if (nspire_os_wake_hook)
    nspire_os_wake_hook();
  return 1;
}

uint32_t nspire_os_since_sleep(void) { return (uint32_t)(vt - last_wake); }

/* nspire_os.c's: the phase is known for 8 ticks after a wake (r15's rule),
 * always by the timer. */
static int phase_known(void) { return model_timer || vt - last_wake < 8 * os_tick_ms * TICKS_PER_MS; }

/* nspire_os.c's next_tick: the timer's count's middle; else from the last
 * wake (as if it were the tick); --oracle: the true tick. */
static double predicted_tick(void)
{
  double p = os_tick_ms * TICKS_PER_MS;
  if (oracle)
    return next_os_tick(vt);
  if (model_timer) {
    uint32_t v = timer_value(vt), d = v ? v : MODEL_LOAD + 1;
    return vt + (d - 0.5) * count_ticks();
  }
  return last_wake + (floor((vt - last_wake) / p) + 1) * p;
}

int nspire_os_sleep_before(uint32_t deadline)
{
  double next = predicted_tick();
  if ((s32)(deadline - (u32)(uint64_t)next) < 66)
    return 0;
  return __wrap_nspire_os_sleep();
}

int nspire_os_sleep_near(uint32_t max_wait)
{
  if (!max_wait)
    max_wait = (u32)(os_tick_ms * TICKS_PER_MS / 10) < 16 ? 16 : (u32)(os_tick_ms * TICKS_PER_MS / 10);
  if (!phase_known() || predicted_tick() - vt > max_wait)
    return 0;
  return __wrap_nspire_os_sleep();
}

int nspire_os_next_tick_in(uint32_t *ticks, uint32_t *period)
{
  double d = predicted_tick() - vt;
  *period = (uint32_t)(os_tick_ms * TICKS_PER_MS + 0.5);
  *ticks = d > 0 ? (uint32_t)(d + 0.5) : 0;
  return phase_known();
}

/* nspire_os.c's */
uint32_t nspire_os_near_wait(uint32_t since, uint32_t from, uint32_t until)
{
  uint32_t w = (u32)(os_tick_ms * TICKS_PER_MS / 10) < 16 ? 16 : (u32)(os_tick_ms * TICKS_PER_MS / 10);
  if (until > from && since > from)
    w += (uint32_t)(os_tick_ms * TICKS_PER_MS / 2 * ((since > until ? until : since) - from) / (until - from));
  return w;
}

int nspire_usb_kind(void) { return 2; }
int nspire_os_keypad_keep_off(void) { return 0; }

uint32_t __wrap_nspire_os_sleeps(void) { return sleeps; }
uint32_t __wrap_nspire_os_busy_slept(uint32_t *count)
{
  if (count)
    *count = sleeps_busy;
  return (uint32_t)slept_busy;
}

/* nspire_irqs_unmask: an IRQ window. The CX II's R11u log: the OS's USB
 * completions come in a window (209 of 215 probes, 0.94 ms a window): the
 * default here; --no-irq-usb (or a case's own) only in a task's sleep, the
 * fallback's case. A window costs win_ms (the OS's handlers) when the
 * completions come in it, the 0.3 ms spin when none can. */
static int irq_usb = 1;
static double win_ms = 0.9;
static double win_spent;           /* ticks in windows */
void host_os_irq_window(void)
{
  double t;
  uint32_t c0;
  if (in_os)
    return;
  bus_masked();
  t = irq_usb ? win_ms : 0.3;
  if (irq_usb) {
    note_service();
    c0 = completions_now_model();
#ifdef UA_HAS_QUEUE_MS
    if (tracing()) {
      int qa = 0, qh = 0, qk = ua_queue_ms(&qa, &qh);
      printf("      window at %.4f: before %d %d %d wire %u frame %u", vt / NSPIRE_TICK_HZ, qk, qa, qh, fos_wire_left(), fos_frame());
    }
#endif
    fos_irq_window();
#ifdef UA_HAS_QUEUE_MS
    if (tracing()) {
      int qa = 0, qh = 0, qk = ua_queue_ms(&qa, &qh);
      printf("; after %d %d %d wire %u, %u completions\n", qk, qa, qh, fos_wire_left(), completions_now_model() - c0);
    }
#endif
    if (comp_ms > 0) {
      uint32_t n = completions_now_model() - c0;
      t = n ? win_ms + comp_ms * n : 0.3;
    }
  }
  spend(t * TICKS_PER_MS);
  win_spent += t * TICKS_PER_MS;
}
void nspire_os_after_os_time(void) { }

/* nspire/nspire.c's */
void nspire_wait_until(u32 tick)
{
  if ((s32)(tick - unit_ticks) > (s32)NSPIRE_TICK_HZ)
    return;
  nspire_os_idle = 1;
  while ((s32)(tick - unit_ticks) > 0) {
    if (nspire_os_wanted && nspire_os_sleep_before(tick))
      continue;
    set_time(vt + (double)(s32)(tick - unit_ticks));   /* the spin: no later tick fits either */
  }
  nspire_os_idle = 0;
}

/* What nspire_frameskip.c needs of the rest of the frontend. The LCD
 * (2026-11-15, final/pace; before: "no refresh known", "every picture
 * taken"): a refresh every 1/60 s of virtual time from 0, the newest picture
 * taken at each; nspire_flip_taken says a refresh began since the last
 * flip. Only fast forward asks (its slots, nspire_frameskip_before_flip). */
#define LCD_TICKS ((double)NSPIRE_TICK_HZ / 60.0)
static double last_flip = -1;
static long last_flip_refresh = -1;
static unsigned lcd_pictures;      /* refreshes that took a new picture */
/* 2026-11-17 (final/pace6): the CX II's LCD as nspire.c lcd_tune leaves it
 * (--lcd-cx2 or the lcd case): a refresh every lcd_period ticks (16.70 ms by
 * default: the reference log, "tuned to 16.7 ms (344 lines)"), in 344 lines of
 * which the first lcd_vb_lines are its sync and back porch (the base is
 * latched at the refresh's start), then the buffer's 320 lines, then the
 * front porch. As the hardware: nspire_flip_taken sees the LCD reading the
 * picture's lines only (in the porches the address is past the buffer), and
 * stays seen once seen (nspire.c's flip_taken); nspire_lcd_ticks_to_refresh
 * is the time to the next refresh's lines, 0 in the porches; a picture is
 * shown when a refresh latches it, lost when the next flip came first. */
static int lcd_cx2;
static double lcd_period_ms = 16.70, lcd_vb_lines = 7, lcd_all_lines = 344;
static int flip_seen;
static unsigned lcd_lost;          /* pictures replaced before a refresh took them */
static unsigned lcd_waits;         /* pictures that waited for the LCD (nspire_frameskip_before_flip) */
static double lcd_waited;          /* and the time, ticks */
static double lcd_p(void) { return lcd_period_ms * TICKS_PER_MS; }
static double lcd_line(void) { return lcd_p() / lcd_all_lines; }
static double lcd_start(double t) { return floor(t / lcd_p()) * lcd_p(); }   /* the refresh at or before t */
static int lcd_in_lines(double t)
{
  double into = t - lcd_start(t);
  return into >= lcd_vb_lines * lcd_line() && into < (lcd_vb_lines + 320) * lcd_line();
}
u32 nspire_lcd_ticks_to_refresh(void)
{
  if (lcd_cx2) {
    if (!lcd_in_lines(vt))
      return 0;
    double left = lcd_start(vt) + lcd_p() + lcd_vb_lines * lcd_line() - vt;
    return left < 1 ? 1 : (u32)left;
  }
  double left = (floor(vt / LCD_TICKS) + 1) * LCD_TICKS - vt;
  return left < 1 ? 1 : (u32)left;
}
u32 nspire_lcd_period16(void)
{
  if (lcd_cx2)
    return (u32)(lcd_p() * 65536.0);
  return (u32)(((u64)NSPIRE_TICK_HZ << 16) / 60);
}
int nspire_lcd_at_gba_rate(void)
{
  return lcd_cx2;   /* else the model's LCD is the 60 a second it always was: not moved (nspire.c lcd_tune) */
}
int nspire_flip_taken(void)
{
  if (lcd_cx2) {
    if (last_flip < 0 || flip_seen)
      return 1;
    if (lcd_start(vt) > last_flip && lcd_in_lines(vt))
      return flip_seen = 1;
    spend(1);
    return 0;
  }
  if (last_flip < 0 || floor(vt / LCD_TICKS) > floor(last_flip / LCD_TICKS))
    return 1;
  spend(1);   /* a poll of the LCD's address: the spin's time */
  return 0;
}
/* (2026-11-21, final/r19-pace) Each second of a measurement by the LCD's
 * refreshes: 60 of them make a "second" (1.002 s at 16.70 ms; a game locked
 * to the LCD runs at its 59.6-59.9 a second, so a whole second's integer
 * count would say 59 or 60 by where its edges fall). In each: the game's
 * frames that ended, the pictures drawn, the new pictures the LCD showed
 * (by the refresh that latched them), and of those, the ones more than one
 * game frame after the last shown (a jump: a picture left out). A refresh
 * without a new picture is a repeat (the picture before held): 60 less
 * those shown. Rates are a count over the 60 refreshes' time. */
#define BINS 160
static int bins_on;
static long bins_r0;                 /* the first refresh measured */
static unsigned bin_frames[BINS], bin_drawn[BINS], bin_shown[BINS], bin_jumps[BINS];
static unsigned game_frames_n, flip_frame_n, shown_frame_n, composed_frame_n;
static double refresh_ticks(void) { return lcd_cx2 ? lcd_p() : LCD_TICKS; }
static long refresh_of(double t) { return (long)floor(t / refresh_ticks()); }
static int bin_of(long refresh)
{
  long b = refresh - bins_r0;
  return bins_on && b >= 0 && b / 60 < BINS ? (int)(b / 60) : -1;
}
static void bins_begin(void)
{
  memset(bin_frames, 0, sizeof(bin_frames));
  memset(bin_drawn, 0, sizeof(bin_drawn));
  memset(bin_shown, 0, sizeof(bin_shown));
  memset(bin_jumps, 0, sizeof(bin_jumps));
  bins_r0 = refresh_of(vt) + 1;
  bins_on = 1;
}
/* a picture (of game frame 'frame') latched at refresh r */
static long last_latch_r = -1;
static void bin_latched(long r, unsigned frame)
{
  int b = bin_of(r);
  if (getenv("PACE_REPEATS") && b >= 0 && last_latch_r >= 0 && r - last_latch_r > 1)
    printf("      at %.3f s: %ld refresh(es) held the last picture (game frame %u, then %u)\n",
           r * refresh_ticks() / NSPIRE_TICK_HZ, r - last_latch_r - 1, shown_frame_n, frame);
  last_latch_r = r;
  if (b >= 0) {
    bin_shown[b]++;
    if (shown_frame_n && frame > shown_frame_n + 1)
      bin_jumps[b]++;
  }
  shown_frame_n = frame;
}
struct bins {
  unsigned n;                        /* whole seconds */
  double shown_min, shown_mean, drawn_min, drawn_mean, game_min, game_mean;
  unsigned repeats, jumps, worst_repeats;   /* in all; the most in one second */
  unsigned left[4];                  /* seconds with 0, 1, 2, 3 or more of the game's frames not drawn */
};
static struct bins bins_take(void)
{
  struct bins s;
  long n = (refresh_of(vt) - bins_r0) / 60;
  double secs = 60 * refresh_ticks() / NSPIRE_TICK_HZ;
  memset(&s, 0, sizeof(s));
  if (n > BINS) n = BINS;
  s.n = n > 0 ? (unsigned)n : 0;
  s.shown_min = s.drawn_min = s.game_min = 1e9;
  for (unsigned i = 0; i < s.n; i++) {
    double sh = bin_shown[i] / secs, dr = bin_drawn[i] / secs, g = bin_frames[i] / secs;
    unsigned rep = bin_shown[i] < 60 ? 60 - bin_shown[i] : 0;
    if (sh < s.shown_min) s.shown_min = sh;
    if (dr < s.drawn_min) s.drawn_min = dr;
    if (g < s.game_min) s.game_min = g;
    s.shown_mean += sh / s.n;
    s.drawn_mean += dr / s.n;
    s.game_mean += g / s.n;
    s.repeats += rep;
    s.jumps += bin_jumps[i];
    s.left[bin_frames[i] - bin_drawn[i] > 3 ? 3 : bin_frames[i] - bin_drawn[i]]++;
    if (rep > s.worst_repeats) s.worst_repeats = rep;
  }
  if (!s.n)
    s.shown_min = s.drawn_min = s.game_min = 0;
  if (getenv("PACE_BINS"))
    for (unsigned i = 0; i < s.n; i++)
      printf("      second %2u (from %.2f s): game %u, drawn %u, shown %u, jumps %u\n", i,
             (bins_r0 + 60.0 * i) * refresh_ticks() / NSPIRE_TICK_HZ, bin_frames[i], bin_drawn[i], bin_shown[i], bin_jumps[i]);
  bins_on = 0;
  return s;
}

static void flip(void)
{
  if (lcd_cx2) {
    /* the last picture: shown if a refresh began between its flip and this */
    if (last_flip >= 0) {
      if (lcd_start(vt) > last_flip) {
        lcd_pictures++;
        bin_latched(refresh_of(last_flip) + 1, flip_frame_n);
      }
      else {
        lcd_lost++;
        if (getenv("PACE_LOST"))
          printf("      at %.4f s: a picture replaced unseen (flipped %.2f ms before, at %.2f ms of its refresh)\n",
                 vt / NSPIRE_TICK_HZ, (vt - last_flip) / TICKS_PER_MS, (last_flip - lcd_start(last_flip)) / TICKS_PER_MS);
      }
    }
    flip_seen = 0;
    last_flip = vt;
    flip_frame_n = composed_frame_n;    /* (the frame whose picture it is: a flip may come a frame later) */
    return;
  }
  long refresh = (long)floor(vt / LCD_TICKS);   /* the refresh that takes it: the next one */
  if (refresh != last_flip_refresh) {
    lcd_pictures++;
    bin_latched(refresh + 1, composed_frame_n);
  }
  last_flip_refresh = refresh;
  last_flip = vt;
}

/* nspire.c's, for a picture fast forward flips itself (ff_flush): the
 * screen buffer moves on at each flip, as nspire_screen does */
static char model_screens[3];
static unsigned model_screen_i;
void *nspire_screen = &model_screens[0];
/* the game frame whose picture each buffer
 * holds (made into nspire_screen before nspire_frameskip_before_flip), and
 * the newest flipped: a flip of a buffer holding nothing newer shows a stale
 * picture (fast forward's deferred picture flipped twice: the second flip
 * showed the buffer after it, two pictures back) */
static unsigned model_buf_frame[3], model_flipped_frame, stale_flips;
/* another screen's picture (a menu's, the link's wait screen): flipped by
 * other_flip, never to be flipped as the game's; and the game's flips */
#define OTHER_PICTURE 0xFFFFFFFFu
static int other_flipping;
static unsigned game_flips;
u32 nspire_flip_serial;   /* (nspire.c's: every flip counted) */
void nspire_flip(void)
{
  nspire_flip_serial++;
  if (other_flipping) {
    flip();
    model_screen_i = (model_screen_i + 1) % 3;
    nspire_screen = &model_screens[model_screen_i];
    return;
  }
  game_flips++;
  if (model_buf_frame[model_screen_i] == OTHER_PICTURE || model_buf_frame[model_screen_i] <= model_flipped_frame) {
    stale_flips++;
    if (getenv("PACE_STALE"))
      printf("      at %.4f s: a stale buffer flipped (frame %u's, after %u's)\n", vt / NSPIRE_TICK_HZ,
             model_buf_frame[model_screen_i], model_flipped_frame);
  } else
    model_flipped_frame = model_buf_frame[model_screen_i];
  flip();
  model_screen_i = (model_screen_i + 1) % 3;
  nspire_screen = &model_screens[model_screen_i];
}

/* another screen drawn into nspire_screen and flipped (the menu's, the link's wait screen) */
static void other_flip(void)
{
  model_buf_frame[model_screen_i] = OTHER_PICTURE;
  other_flipping = 1;
  nspire_flip();
  other_flipping = 0;
}

/* The pad glue's tick with a USB hub in (nspire_usbpad_glue.c
 * nspire_usbpad_keys, its hub branch; --hub busy|quiet): the OS's tick at
 * least every 100 ms (the first 30 s after the hub's last coming or going,
 * near tries from 33 ms) or every 250 ms (quiet: near tries from 100 ms),
 * at the frame's key read; and the frames' spare time is the OS's
 * (OS_WANT_PAD). The reference CX II log (2026-11-17, sound on a hub):
 * seconds 306-334 22 sleeps a second, 80-100 ms of the game's time; 336 on
 * 4-9, 5-40 ms. */
static int hub_mode;               /* 0 none, 1 busy, 2 quiet */
int nspire_usbhub_present(void) { return hub_mode != 0; }
int usbpad_os_explore_pending(void) { return 0; } /* speed18: OS 6.4.0.74 explore flag known */
static int hub_old;                /* --hub-old: the pad glue's tick while the sound streams (2b8d4d6) */
int nspire_audio_serves_os(void) __attribute__((weak));   /* (PACE_SRC's older nspire_audio.c: none) */
static void hub_tick(void)
{
  uint32_t since, force, from;
  if (!hub_mode)
    return;
  nspire_os_want(OS_WANT_PAD, 1);
  /* (since final/pace6 the sound's own sleeps give a hub its time while it
   * streams: nspire_audio.c task_ms) */
  if (!hub_old && nspire_audio_serves_os && nspire_audio_serves_os())
    return;
  since = nspire_os_since_sleep();
  force = hub_mode == 1 ? NSPIRE_TICK_HZ / 10 : NSPIRE_TICK_HZ / 4;
  from = hub_mode == 1 ? NSPIRE_TICK_HZ / 30 : NSPIRE_TICK_HZ / 10;
  if (since >= force)
    (void)__wrap_nspire_os_sleep();
  else if (since >= from)
    (void)nspire_os_sleep_near(nspire_os_near_wait(since, from, force));
}

/* (2026-11-17, final/speed8) A controller in, on the hub: the pad glue's
 * want branch (nspire_usbpad_keys: a pad's reports come only in the OS's
 * time, so it asks for it while one is in; the hub branch is its else).
 * The rule is the glue's own (nspire_usbpad_glue.h). The reference CX II
 * (r15, Emerald, the sound adapter and an Xbox 360 pad on a hub,
 * a CX II run log): seconds 67-330 60 game frames,
 * 38-55 pictures, 21-25 OS sleeps a second outside waits, 85-155 ms of
 * them (5-7 ms each), none of the sound's own; without the pad 60/58-60,
 * 4-5 sleeps, 10-38 ms. */
static int pad_mode;
static int pad_old;                  /* --pad-old: the glue's rule at the key read only (r15's) */
static int pad_usb8;                 /* --usb8: final/usb8's rule composed (scratch/speed8-usb8 5317950):
                                        while the sound streams, no tick of the pad's own (its reports
                                        come in the sound's windows; usb8's 500 ms quiet tick never due) */
static unsigned pad_sleeps;          /* the pad's own sleeps at the key read (the glue's rule) */
static void pad_tick(void)
{
  uint32_t since;
  int rule;
  nspire_os_want(OS_WANT_PAD, 1);
  since = nspire_os_since_sleep();
  rule = nspire_usbpad_tick_rule(since, 0);
  if (pad_usb8 && nspire_audio_serves_os && nspire_audio_serves_os()) {
    nspire_frameskip_os_none();        /* usb8: the sound's windows bring the reports */
    pad_turns++;                       /* (each window a turn: counted as served) */
    last_pad_turn = vt;
    return;
  }
  if (!pad_old && nspire_frameskip_os_keys(USBPAD_TICK_FORCE))
    return;                            /* the frame loop places them (its points) */
  if (rule == USBPAD_TICK_SLEEP ? __wrap_nspire_os_sleep() :
      rule == USBPAD_TICK_NEAR &&
      nspire_os_sleep_near(nspire_os_near_wait(since, USBPAD_TICK_FROM, USBPAD_TICK_FORCE)))
    pad_sleeps++;
}
u32 synchronize_flag = 1;
s32 relative_frame_count;
int nspire_os_idle;
void nspire_timer_report(char *text, u32 size) { snprintf(text, size, "timer: model"); }
void nspire_clock_report(char *text, u32 size) { snprintf(text, size, "clock: model"); }
const char *nspire_keys_line(void) { return "keys: model"; }
u32 nspire_keys_probe(void) { return 0; }
u16 nspire_osd_color(int which) { return (u16)which; }
char nspire_fps_text[64], nspire_perf_text[80];
/* (r19-pace) the real-time clock's seconds by the model's time (host_stubs.c's
 * says 0: the readout went by the timer), for the readout's counts */
u32 __wrap_nspire_rtc_seconds(void);
u32 __wrap_nspire_rtc_seconds(void) { return (u32)(vt / NSPIRE_TICK_HZ) + 1000; }
/* the readout's changes while measured (steady_run) */
static char ro_last[64];
static unsigned ro_changes, ro_frames;
static void readout_note(void)
{
  if (strcmp(ro_last, nspire_fps_text)) {
    if (getenv("PACE_READOUT"))
      printf("      at %.3f s: the readout \"%s\"\n", vt / NSPIRE_TICK_HZ, nspire_fps_text);
    if (ro_frames)
      ro_changes++;
    snprintf(ro_last, sizeof(ro_last), "%s", nspire_fps_text);
  }
  ro_frames++;
}
u16 nspire_fps_color;
u32 nspire_fps_full;
void nspire_audio_draining(void) { }

/* ---- the game ---- */
cpu_alert_type function_cc write_io_register16(u32 address, u32 value);

static void tone(void)
{
  write_io_register16(0x84, 0x80);
  write_io_register16(0x82, 2);
  write_io_register16(0x80, 0x1177);
  write_io_register16(0x62, 0xF080);
  write_io_register16(0x64, 0x8400);
}

static uint32_t rnd = 12345;
static double spread(double ms)
{
  rnd = rnd * 1103515245u + 12345u;
  return ms * ((double)(rnd >> 8 & 0xFFFF) / 65535.0);
}

struct load {
  double game_ms, game_spread;   /* a frame's emulation: game_ms + 0..game_spread */
  double draw_ms, draw_spread;   /* and when drawn: drawing and the picture */
};

/* Counts for the report; reset at the measurement's start. */
static unsigned frames_run, frames_drawn;
static double delay_max, delay_sum;
static unsigned delay_n;
static uint32_t speed_min, speed_max;   /* the resampler's factor (Q16) while measured */
static void note_delay(void)
{
  struct nspire_audio_stats s;
  nspire_audio_stats(&s);
  if (!s.rate || !s.rendering || s.priming)
    return;
  if (s.speed < speed_min) speed_min = s.speed;
  if (s.speed > speed_max) speed_max = s.speed;
  /* The newest sample's way to the ear at a frame's end: the ring, then
   * the packets handed to the OS and not on the wire yet. */
  double d = s.buffered * 1000.0 / s.rate + fos_frames_ahead();
  if (d > delay_max) delay_max = d;
  delay_sum += d;
  delay_n++;
}

/* ---- A save in the game (2026-11-17, final/pace6; case_save) ----
 * The parts and their times on the reference CX II (gpsp_states_time.txt.tns,
 * Emerald, slots 34-36: copy 12.7, pack 48.7, picture 19.9, write 31.8,
 * replace 37.8 ms; outliers write 118.9 and 205.1, replace 67.6; r19's log
 * (7baa00e): copy 25.9, pack 56.0, write 229.7, replace 65.2). save_at: the
 * frame whose key read starts it; save_old: 2b8d4d6's way (the sound paused,
 * then all of it at once with the interrupts masked, the timing started
 * anew); else nspire_host_main.c's (the state and picture taken at the key
 * with the interrupts on, then a part at each frame's end: CPU parts as they
 * are, each OS call once nspire_audio_save_frame lets it, through
 * nspire_audio_os_call_begin/end, interrupts on: the bus's completions run
 * as they come; after the save nspire_audio_save_frame(0, 0) a frame).
 * 2026-11-21 (r19-snd): the OS's parts as nspire_states.c makes them now:
 * the file's create, its write in SJ_WRITE_BYTES parts, its close, the
 * replace's steps (nspire_paths.c nspire_file_replace_step), each one call;
 * an outlier (the slow kinds) in one of them. */
struct save_part { double ms; int os; };
static struct save_part save_parts[80];
static unsigned save_nparts, save_next;
static int save_at = -1, save_old;
static double save_scale_ms = 6.6;   /* the picture's scaling, of its 19.9: tests/libbench.sh state-save-emerald 8.7 ms of 21.4 in the model, real/model 0.76 */
static unsigned frame_no;
static double flip_gap_max, last_shown_flip = -1;
static double sound_off;             /* ticks the game's sound was not playing (paused or priming) */
static double last_frame_end;
static const char *save_only;        /* --save-only LABEL: that one (with --trace) */
static double save_key_ms;           /* save_key's part: the state's copy and scaling, the game save's copy */
static double save_os_longest;       /* ms: the longest OS part of the save (the picture's inherent hitch) */
/* (final/r19-bank) nspire_config.c's word 2, what this calculator's saves have learned (ms); save_at2: a
 * second save's key, the frame */
extern u32 nspire_save_call_ms;     /* (nspire_config.c's, linked here) */
static int save_at2 = -1;

/* An OS call of ms: the bus runs; with the interrupts on the USB
 * completions run in it (a window a ms). */
static void os_call(double ms, int irqs_on)
{
  uint32_t cpsr = 0;
  int began = irqs_on && nspire_audio_os_call_begin(&cpsr);
  for (double t = 0; t < ms; t += 1) {
    spend((ms - t < 1 ? ms - t : 1) * TICKS_PER_MS);
    bus_masked();
    if (began)
      fos_irq_window();
  }
  nspire_audio_os_call_end(began, cpsr);
}

/* a second key while the save is under way (save_second frames after the
 * first): queued (nspire_states.c now: begun once the first is written), or with --queue-old the rest of
 * the first finished at once with the interrupts masked (save_begin's states_save_finish before), then
 * the second begun; and the saves whose last part ran */
static unsigned save_second, save_queued, saves_done;
static int save_queue_old;
static void save_key(void)
{
  if (save_at < 0 || (frame_no != (unsigned)save_at && (save_at2 < 0 || frame_no != (unsigned)save_at2)))
    return;
  if (!save_old && save_at2 >= 0 && frame_no == (unsigned)save_at2 && save_next < save_nparts) {
    if (!save_queue_old) {
      save_queued++;
      return;
    }
    for (; save_next < save_nparts; save_next++)
      if (save_parts[save_next].os)
        os_call(save_parts[save_next].ms, 0);
      else {
        spend(save_parts[save_next].ms * TICKS_PER_MS);
        bus_masked();
      }
    saves_done++;
  }
  if (save_old) {
    double all = 0;
    for (unsigned i = 0; i < save_nparts; i++)
      all += save_parts[i].ms;
    if (save_key_ms > 2)
      nspire_audio_pause();                /* (a state's: 2b8d4d6 paused the sound; the game's save didn't) */
    os_call(save_key_ms + all, 0);
    if (save_key_ms > 2)
      nspire_reset_timing();
    save_next = save_nparts;
    return;
  }
  if (save_key_ms <= 2) {
    spend(save_key_ms * TICKS_PER_MS); /* (the game save's copy, update_backup) */
    bus_masked();
  }                                    /* (a state's take: its first part, SJ_TAKE) */
  save_next = 0;
}

/* after the save's last part: when owe and the save's state were last seen */
static double save_done_at = -1, owe_last_at, active_last_at;
static void save_frame_end(void)
{
  if (save_old || save_at < 0 || frame_no <= (unsigned)save_at)
    return;
  if (save_next >= save_nparts && save_queued) {
    save_queued--;
    save_next = 0;                         /* (the next one asked for: its take first) */
  }
  if (save_next >= save_nparts) {
    struct nspire_audio_stats st;
    (void)nspire_audio_save_frame(0, 0);   /* (save_job_frame: the bank given back) */
    nspire_audio_stats(&st);
    if (save_done_at < 0)
      save_done_at = owe_last_at = active_last_at = vt;
    if (st.owe)
      owe_last_at = vt;
    if (st.save_active)
      active_last_at = vt;
    return;
  }
  if (save_parts[save_next].os == NSPIRE_SAVE_CPU)
    nspire_audio_serve_ahead(16);      /* (save_job_frame: SAVE_PART_MS) */
  if (!nspire_audio_save_frame(1, save_parts[save_next].os))
    return;
  if (tracing())
    printf("    %.4f save part %u: %s %.1f ms, the ring %d ms\n", vt / NSPIRE_TICK_HZ, save_next,
           save_parts[save_next].os == NSPIRE_SAVE_OS ? "OS call" : save_parts[save_next].os ? "take" : "CPU",
           save_parts[save_next].ms, nspire_audio_ring_ms());
  if (save_parts[save_next].os)
    os_call(save_parts[save_next].ms, 1);   /* (an OS call, or the take: the interrupts on) */
  else {
    spend(save_parts[save_next].ms * TICKS_PER_MS);
    bus_masked();
  }
  save_next++;
  if (save_next == save_nparts)
    saves_done++;
}

/* (2026-11-17, final/speed8) ms of the frame in n lines, the lines' hook
 * (nspire_lines.c os_watch: the OS's tick for a pad when it comes) at each */
/* (2026-11-21, final/r19-pace) the OS's own sleeps outside the frame loop's
 * waits, at random lines (--case steady; the reference r18 log: 0-23 ms a second
 * of them, 2-44 sleeps a second in all): 'hiccups' a second on average, each
 * a sleep to the OS's next tick and its wake */
static double hiccups;
static uint32_t hic_rnd = 4242;
static void maybe_hiccup(void)
{
  if (hiccups <= 0 || nspire_os_idle)
    return;
  hic_rnd = hic_rnd * 1103515245u + 12345u;
  /* (a line is ~1/10000 of a second at 160 + 160 a frame) */
  if ((hic_rnd >> 8 & 0xFFFFF) < hiccups / (60.0 * 320) * 0x100000)
    (void)__wrap_nspire_os_sleep();
}

static void lines(double ms, int n)
{
  for (int i = 0; i < n; i++) {
    spend(ms / n * TICKS_PER_MS);
    maybe_hiccup();
    if (nspire_os_watching && (model_reg_now(), *nspire_os_watch_at.reg - 1u < nspire_os_watch_at.value))
      nspire_os_watch_line();
  }
}

/* (2026-11-21, final/r19-pace) Bluetooth headphones playing (--case steady's
 * "bt" set-ups; bt10's costs on the CX II): the glue's poll each
 * frame in front of its wait (nspire_bt_os_time: its window, then the run
 * loop), 0.35 ms, the mixer's write 0.21 ms, and 43 media packets a second
 * of ~0.5 ms each (their transfer and its completion) on the frames that
 * send one; the dongle's interrupts in the window. */
static int bt_on;
static uint32_t bt_rnd = 777;
static void bt_frame(void)
{
  if (!bt_on)
    return;
  bt_rnd = bt_rnd * 1103515245u + 12345u;
  spend((0.35 + 0.21 + ((bt_rnd >> 8 & 0xFFFF) < 0.72 * 65536 ? 0.5 : 0)) * TICKS_PER_MS);
  bus_masked();
}

static void game_frame(const struct load *L)
{
  double work;
  u32 out = 0;
  nspire_audio_poll();                 /* nspire_input_frame's keys_poll */
  if (pad_mode)
    pad_tick();                        /* and the pad glue's (nspire_usbpad_keys): a pad in */
  else
    hub_tick();                        /* or the hub with none */
  bus_masked();
  save_key();
  bt_frame();
  nspire_audio_frame_begin(nspire_pace == PACE_NORMAL ? (synchronize_flag ? 1 : 2) : 0);
  nspire_frameskip_begin_frame();      /* (its end: the frame's first point, nspire_frameskip_os_point) */
  bus_masked();
  work = L->game_ms + spread(L->game_spread);
  if (!skip_next_frame) {
    double d = L->draw_ms + spread(L->draw_spread), before, g = work;
    work += d;
    /* the emulation (the game and the lines drawn: half of d), run_frame's
     * point, then nspire_present_frame: the picture (the other half of d)
     * made, then nspire_frameskip_before_flip (its point; fast forward:
     * the LCD has taken the last one), then the flip; its time is the
     * picture's and the wait */
    lines(g * 0.7, 160);                   /* the game's lines 0-159 */
    lines(d / 2, 160);                     /* the lines drawn at VBlank */
    spend(g * 0.3 * TICKS_PER_MS);         /* the game's VBlank lines */
    nspire_frameskip_os_point(NSPIRE_OSP_PRESENT);
    spend(d / 2 * TICKS_PER_MS);
    before = vt;
    double prev_flip = last_flip;
    composed_frame_n = game_frames_n + 1;   /* (this frame's picture: counted at the frame's end) */
    model_buf_frame[model_screen_i] = composed_frame_n;   /* (made into nspire_screen) */
    int flip_now = nspire_frameskip_before_flip();
    if (vt - before > 1) { lcd_waits++; lcd_waited += vt - before; }
    if (show_log && lcd_cx2 && vt - before > 3 * TICKS_PER_MS)
      printf("  at %.3f s: the picture waited %.2f ms for the LCD (at %.2f ms of its refresh; the last one %.2f ms"
             " before, at %.2f), the frame's work %.1f ms\n", before / NSPIRE_TICK_HZ, (vt - before) / TICKS_PER_MS,
             (before - lcd_start(before)) / TICKS_PER_MS, (before - prev_flip) / TICKS_PER_MS,
             (prev_flip - lcd_start(prev_flip)) / TICKS_PER_MS, work);
    if (lcd_cx2 && last_flip >= 0 && lcd_start(vt) > last_flip) {
      /* the last picture was shown: the time from the one before it */
      if (last_shown_flip >= 0 && last_flip - last_shown_flip > flip_gap_max)
        flip_gap_max = last_flip - last_shown_flip;
      last_shown_flip = last_flip;
    }
    if (flip_now)
      nspire_flip();
    out = (u32)(d / 2 * TICKS_PER_MS + (vt - before));
    frames_drawn++;
  } else {
    lines(work * 0.7, 160);
    spend(work * 0.3 * TICKS_PER_MS);
    nspire_frameskip_os_point(NSPIRE_OSP_PRESENT);
  }
  cpu_ticks += 280896;
  render_gbc_sound();
  sound_flush_all_pops();
  nspire_frameskip_end_frame(out);
  game_frames_n++;
  if (bins_on)
    readout_note();
  {
    int b = bin_of(refresh_of(vt));
    if (b >= 0) {
      bin_frames[b]++;
      bin_drawn[b] += !skip_next_frame;
    }
  }
  bus_masked();
  unsigned ring_before = tracing() ? ring_now() : 0;
  double frame_end_at = vt;
  nspire_audio_frame_end();
  bus_masked();
  save_frame_end();                    /* nspire_host_main.c save_job_frame's place */
  frame_no++;
  {
    struct nspire_audio_stats st;
    nspire_audio_stats(&st);
    if (last_frame_end > 0 && (!st.rendering || st.priming))
      sound_off += frame_end_at - last_frame_end;
    last_frame_end = frame_end_at;
  }
  if (tracing()) {
    struct nspire_audio_stats s;
    nspire_audio_stats(&s);
    printf("    %.4f frame %s %.1f ms: ring %u -> %u, rate %+.2f%%; completions %u, windows %u (%u)%s%s;"
           " bank %u, left out %u\n",
           vt / NSPIRE_TICK_HZ, skip_next_frame ? "skipped" : "drawn", work, ring_before, s.buffered,
           (s.speed / 65536.0 - 1) * 100, s.completions, s.windows, s.windows_served, s.stalled ? ", stalled" : "",
           s.win_trusted ? "" : ", windows not trusted", s.bank, s.trims + s.dropped * 1000); if (s.owe) printf("      owe %u\n", s.owe);
    if (s.priming || !s.rendering)
      printf("      (sound off: %s)\n", s.priming ? "priming" : "not rendering");
#ifdef UA_HAS_QUEUE_MS
    {
      int qa = 0, qh = 0, qk = ua_queue_ms(&qa, &qh);
      printf("      queue: %s ahead %d head %d; the OS's wire left %u\n", qk ? "known" : "not known", qa, qh,
             fos_wire_left());
    }
#endif
  }
  note_delay();
  frames_run++;
  if (show_log) {
    static unsigned gaps_seen, silent_seen;
    struct ua_counters c;
    ua_counters(&c);
    if (fos_stats()->starved_frames != gaps_seen || c.underrun_pkts != silent_seen)
      printf("  at %.3f s: wire gaps %u, silent packets %u; %.1f ms since the last wake, frame %s %.1f ms\n",
             vt / NSPIRE_TICK_HZ, fos_stats()->starved_frames, c.underrun_pkts, (vt - last_wake) / TICKS_PER_MS,
             skip_next_frame ? "skipped" : "drawn", work);
    gaps_seen = fos_stats()->starved_frames;
    silent_seen = c.underrun_pkts;
  }
}

/* A menu: the sound paused, its keys read each refresh (get_gui_input's
 * keys_poll), back to the game with the timing anew. */
static void menu(double seconds)
{
  double until = vt + seconds * NSPIRE_TICK_HZ;
  nspire_audio_pause();
  while (vt < until) {
    nspire_audio_poll();
    bus_masked();
    spend(NSPIRE_TICK_HZ / 60.0);
    bus_masked();
  }
  nspire_reset_timing();
}

/* ---- a case ---- */
struct snap {
  double t;
  unsigned frames, drawn, lcd, sleeps, sleeps_busy;
  double slept_busy, win;
  struct ua_counters c;
  struct nspire_audio_stats s;
  unsigned wire_gaps;
};

static void snap(struct snap *s)
{
  s->t = vt;
  s->frames = frames_run;
  s->drawn = frames_drawn;
  s->lcd = lcd_pictures;
  s->sleeps = sleeps;
  s->sleeps_busy = sleeps_busy;
  s->slept_busy = slept_busy;
  s->win = win_spent;
  ua_counters(&s->c);
  nspire_audio_stats(&s->s);
  s->wire_gaps = fos_stats()->starved_frames;
}

struct result {
  double fps, drawn_fps, lcd_fps, os_ms_s, win_ms_s, delay_max, delay_mean, away_max, rate_lo, rate_hi;
  unsigned sleeps_s, windows_s, silent_pkts, silent_frames, wire_gaps, restarts, dropped, stalls;
};

static struct result diff(const struct snap *a, const struct snap *b)
{
  struct result r;
  double secs = (b->t - a->t) / NSPIRE_TICK_HZ;
  r.fps = (b->frames - a->frames) / secs;
  r.drawn_fps = (b->drawn - a->drawn) / secs;
  r.lcd_fps = (b->lcd - a->lcd) / secs;
  r.sleeps_s = (unsigned)((b->sleeps - a->sleeps) / secs + 0.5);
  /* the game's time the OS took: task sleeps outside the waits, and windows */
  r.win_ms_s = (b->win - a->win) / TICKS_PER_MS / secs;
  r.os_ms_s = (b->slept_busy - a->slept_busy) / TICKS_PER_MS / secs + r.win_ms_s;
  r.windows_s = (unsigned)((b->s.windows - a->s.windows) / secs + 0.5);
  r.dropped = b->s.dropped - a->s.dropped;
  r.stalls = b->s.stalls - a->s.stalls;
  r.silent_pkts = b->c.underrun_pkts - a->c.underrun_pkts;
  r.silent_frames = b->c.underrun_frames - a->c.underrun_frames;
  r.wire_gaps = b->wire_gaps - a->wire_gaps;
  r.restarts = b->c.starved - a->c.starved;
  r.delay_max = delay_max;
  r.delay_mean = delay_n ? delay_sum / delay_n : 0;
  r.away_max = away_max / TICKS_PER_MS;
  r.rate_lo = speed_max ? (speed_min / 65536.0 - 1) * 100 : 0;
  r.rate_hi = speed_max ? (speed_max / 65536.0 - 1) * 100 : 0;
  return r;
}

static void print(const char *name, const struct result *r)
{
  printf("pace %-10s game %5.2f fps, pictures %5.2f; OS %5.1f ms/s (%2u sleeps/s, %2u windows/s %4.1f ms), "
         "away <= %4.1f ms; silent packets %u (%u frames), wire gaps %u ms, restarts %u, dropped %u; "
         "delay max %.0f ms (mean %.0f); rate %+.2f..%+.2f%%\n", name, r->fps, r->drawn_fps, r->os_ms_s, r->sleeps_s,
         r->windows_s, r->win_ms_s, r->away_max, r->silent_pkts, r->silent_frames, r->wire_gaps, r->restarts,
         r->dropped, r->delay_max, r->delay_mean, r->rate_lo, r->rate_hi);
}

/* adapter 0: none plugged (the rule's cases without sound) */
static void start_with(unsigned frameskip_type, int adapter)
{
  nspire_audio_stop();
  fos_init();
  set_time(0);
  last_flip = last_flip_refresh = -1;
  lcd_pictures = lcd_lost = 0;
  flip_seen = 0;
  nspire_os_want(OS_WANT_PAD, 0);
  last_wake = last_service = 0;
  bus_ms = 0;
  sleeps = sleeps_busy = 0;
  slept_busy = win_spent = 0;
  rnd = 12345;
  jrnd = 99991;                 /* (each case its own wakes' jitter, the same for each rule) */
  wake_late256_t = 0;
  unit_usb_plugged = adapter;   /* without one the port is empty */
  nspire_audio_enabled = 1; nspire_audio_volume = 10;
  nspire_audio_mono = 0; nspire_audio_buffer = 0;   /* the defaults: Short */
  current_frameskip_type = frameskip_type;
  frameskip_value = 4;
  synchronize_flag = 1;
  nspire_pace = PACE_NORMAL;
  init_sound();
  for (unsigned i = 0; i < 4; i++) dma[i].direct_sound_channel = DMA_NO_DIRECT_SOUND;
  nspire_frameskip_reset();
  nspire_audio_start();
  tone();
  struct fos_spec s = {0x0d8c, 0x000c, cfg_cm108, sizeof cfg_cm108, 0};
  if (adapter)
    T_EQ(fos_plug(&s), 0);
}

static void start(unsigned frameskip_type)
{
  start_with(frameskip_type, 1);
}

static void measure_begin(struct snap *a)
{
  os_gap_max = 0;
  pad_gap_max = 0;
  pad_turns = 0;
  frames_run = frames_drawn = 0;
  delay_max = delay_sum = 0;
  away_max = 0;
  speed_min = 0xFFFFFFFFu;
  speed_max = 0;
  delay_n = 0;
  snap(a);
}

static void run_for(const struct load *L, double seconds)
{
  double until = vt + seconds * NSPIRE_TICK_HZ;
  while (vt < until)
    game_frame(L);
}

static const char *only;

static int wanted(const char *name) { return !only || !strcmp(only, name); }

/* (weak: faf629e's nspire_audio.c, PACE_SRC, has no log) */
int nspire_audio_log_pending(void) __attribute__((weak));
void nspire_audio_log_write(FILE *f) __attribute__((weak));

/* The log the perf file gets at the quit: once the audio is stopped, one
 * line a second the adapter streamed (the five minutes' ring not full in a
 * case), then nothing pending. */
static void finish(void)
{
  nspire_audio_stop();
  T_EQ(fos_stats()->violations, 0);
  if (old_code || !nspire_audio_log_write)
    return;
  FILE *f = tmpfile();
  char line[400];
  unsigned seconds = 0, heads = 0;
  T_TRUE(f != NULL);
  if (!f)
    return;
  T_TRUE(nspire_audio_log_pending());
  nspire_audio_log_write(f);
  T_EQ(nspire_audio_log_pending(), 0);
  rewind(f);
  while (fgets(line, sizeof(line), f)) {
    unsigned long tries, served;
    if (sscanf(line, "sound: IRQ windows %lu, %lu with", &tries, &served) == 2) {
      T_TRUE(tries >= 10);                 /* windows serving the queue, or once a second untrusted */
      if (irq_usb) T_TRUE(served >= tries * 9 / 10);
      else T_EQ(served, 0);                /* a sleep's case: the windows serve nothing */
    }
    if (!strncmp(line, "sound: ", 7) || !strncmp(line, "sound seconds: ", 15)) heads++;
    else if (!strncmp(line, "sound ", 6) && strstr(line, "; rate ")) seconds++;
    if (show_log) fputs(line, stdout);
  }
  fclose(f);
  T_EQ(heads, 3);
  T_TRUE(seconds >= 30 && seconds <= 300);
}

/* The CX II in Emerald's town: ~10 ms of work a frame, a picture drawn each
 * (frameskip off, the reference setting): 7 ms of game, 3-7 of drawing. */
static struct load town = { 7.0, 1.0, 3.0, 3.0 };      /* 10-14 ms */
/* The CX CAS, or a heavy scene: no spare time drawing every frame. */
static struct load heavy = { 14.0, 2.0, 9.0, 2.0 };    /* 23-27 ms drawn, 14-16 skipped */

/* ms a second of the game's time the OS may take: 30 (the brief), 33 where
 * frames of 14-16 ms need a window every other frame (30 a second at the
 * model's 0.9 ms, R11u's probes' mean) and no tick falls in the spare time
 * for the OS's task (one sleep in 2 s, ~6 ms) */
static double os_limit = 30;

static void check_full_speed(const char *name, const struct result *r)
{
  print(name, r);
  if (old_code)
    return;
  T_TRUE(r->fps >= 59.5);
  T_EQ(r->silent_pkts, 0);
  T_EQ(r->wire_gaps, 0);
  T_EQ(r->dropped, 0);             /* round 2 (R11u: 247157 frames dropped, the ring full) */
  T_TRUE(r->delay_max <= 150);
  /* the bus has (UA_NX - 1) x UA_NF ms at a completion, up to UA_NX x UA_NF
   * after the OS ran; speed18: served as late as the queue's clock allows,
   * so the longest away nears the latter less the OS's 4 frames (the wire
   * gaps above are the proof) */
  T_TRUE(r->away_max < UA_NX * UA_NF - 4);
  /* round 2 (R11u: "the sound pitches up and down"): at full speed the
   * factor stays within +-0.25%, the start after a menu or fast forward too */
  T_TRUE(r->rate_lo > -0.25 && r->rate_hi < 0.25);
  if (irq_usb)
    T_TRUE(r->os_ms_s < os_limit); /* round 2: the OS's time out of the game's (R11u: 140-195 ms a second) */
}

static void spare(const char *name, const struct load *L, double drawn_least)
{
  struct snap a, b;
  struct result r;
  start(no_frameskip);
  run_for(L, 2);
  T_EQ(ua_state(), UA_STREAMING);
  measure_begin(&a);
  run_for(L, MEASURE_S);
  snap(&b);
  r = diff(&a, &b);
  check_full_speed(name, &r);
  if (!old_code)
    T_TRUE(r.drawn_fps >= drawn_least);
  finish();
}

/* Walking in town (more to draw, frame blending on): 14-16 ms, 0.7-2.7 ms
 * to spare. faf629e's rule (a sleep each 16 ms) loses the game's speed here. */
static struct load moving = { 10.0, 1.0, 4.0, 1.0 };

static void case_spare(void)
{
  spare("a-town", &town, 59.5);      /* nothing skipped: the frames fit */
  /* 1.74 ms to spare a frame on average: round 1's sleeps (an OS tick every
   * 42 ms costing 0-10) drew 2 in 3 (40); windows of ~1 ms every other
   * frame fit, a late frame costs one picture (round 2) */
  os_limit = 33;
  spare("a-moving", &moving, irq_usb ? 58 : 39);
  os_limit = 30;
}

/* bt10: Output Bluetooth: the card idle (the driver's ua_set_idle, its
 * keep-alive a second) against the card streaming silence (the sound off:
 * what Output Bluetooth cost before, and what a menu costs), in town; then
 * Automatic again: the sound back with no silent packet once it plays.
 * (The headphones aren't in this model: their own OS time is the glue's.) */
#include "nspire_bt_glue.h"
static void case_idle(void)
{
  struct snap a, b;
  struct result r, rs;
  start(no_frameskip);
  nspire_audio_enabled = 0;
  run_for(&town, 2);
  measure_begin(&a);
  run_for(&town, 20);
  snap(&b);
  rs = diff(&a, &b);
  print("i-silence", &rs);
  printf("     the card streaming silence: %.1f completions/s\n", (b.c.completions - a.c.completions) / 20.0);
  run_for(&heavy, 2);
  measure_begin(&a);
  run_for(&heavy, 20);
  snap(&b);
  r = diff(&a, &b);
  print("i-silence-busy", &r);
  run_for(&town, 2);
  nspire_audio_enabled = 1;
#ifdef UA_HAS_IDLE
  nspire_audio_output = NSPIRE_OUTPUT_BT;
  run_for(&town, 2);
  T_TRUE(ua_idle());
  measure_begin(&a);
  run_for(&town, 20);
  snap(&b);
  r = diff(&a, &b);
  print("i-idle", &r);
  printf("     the card idle: %.1f completions/s, %.2f keep-alives/s; OS %.1f ms/s less than streaming silence\n",
         (b.c.completions - a.c.completions) / 20.0, (b.c.keepalives - a.c.keepalives) / 20.0,
         rs.os_ms_s - r.os_ms_s);
  if (!old_code) {
    T_TRUE(r.fps >= 59.5);
    T_EQ(r.windows_s, 0);
    T_TRUE(b.c.completions - a.c.completions <= 2 * 20);
    T_TRUE(b.c.keepalives - a.c.keepalives >= 20 / 3);   /* (one each ~1-2 s: the last linked frame stays recent) */
    T_TRUE(r.os_ms_s < rs.os_ms_s);
    T_EQ(b.s.stalls, a.s.stalls);
  }
  /* a game with no time to spare (frames back to back: no wait sleeps):
   * the keep-alive's completion waits for the OS's task sleep (TASK_MS):
   * one every 2-4 s, the last linked frame never 4 s old */
  run_for(&heavy, 2);
  measure_begin(&a);
  run_for(&heavy, 20);
  snap(&b);
  r = diff(&a, &b);
  print("i-idle-busy", &r);
  printf("     the card idle, no time to spare: %.2f keep-alives/s, game %.2f fps\n",
         (b.c.keepalives - a.c.keepalives) / 20.0, r.fps);
  if (!old_code) {
    T_EQ(r.windows_s, 0);
    T_TRUE(b.c.keepalives - a.c.keepalives >= 20 / 4);
    T_EQ(b.s.stalls, a.s.stalls);
  }
  nspire_audio_output = NSPIRE_OUTPUT_AUTO;
  run_for(&town, 1);
  T_EQ(ua_idle(), 0);
  measure_begin(&a);
  run_for(&town, 10);
  snap(&b);
  r = diff(&a, &b);
  check_full_speed("i-back", &r);
#endif
  nspire_audio_output = NSPIRE_OUTPUT_AUTO;
  finish();
}

/* speed18: Output Automatic with Bluetooth headphones
 * beside the card for minutes: the card's sound held back to theirs
 * (nspire_audio.c sync_with_headphones) and re-synced only when their delay
 * moves or a gap in the game's sound leaves the card ahead (rate_update's
 * settled fill). With the queue served late, in transfers of UA_NF ms, the
 * fill after the OS's turns moves in bigger steps: no re-sync may come of
 * that. The headphones here: they stream (the glue's frame says so), take
 * the mixer's frames, their delay steady (the library's ~120 ms measured
 * plus the sink's 150: 273, as sink_test's) or wobbling within the slack. */
static int hp_on;
static unsigned hp_delay = 273;
int __real_nspire_bt_glue_audio_frame(int mode, int usb_streaming);
int __wrap_nspire_bt_glue_audio_frame(int mode, int usb_streaming);
int __wrap_nspire_bt_glue_audio_frame(int mode, int usb_streaming)
{
  if (!hp_on)
    return __real_nspire_bt_glue_audio_frame(mode, usb_streaming);
  return mode && nspire_audio_output != NSPIRE_OUTPUT_USB && nspire_audio_enabled && nspire_audio_volume;
}
void __wrap_nspire_bt_glue_audio_write_mix(uint32_t *w1, unsigned n1, uint32_t *w2, unsigned n2, unsigned flags);
void __wrap_nspire_bt_glue_audio_write_mix(uint32_t *w1, unsigned n1, uint32_t *w2, unsigned n2, unsigned flags)
{
  unsigned i;
  if (flags & 1) {
    for (i = 0; i < n1; i++) w1[i] = 0;
    for (i = 0; i < n2; i++) w2[i] = 0;
  }
}
unsigned __wrap_nspire_bt_glue_audio_delay_ms(void);
unsigned __wrap_nspire_bt_glue_audio_delay_ms(void) { return hp_on ? hp_delay : 0; }

static void sync_run(const char *name, const struct load *L, int wobble)
{
  struct snap a, b;
  struct result r;
  struct nspire_audio_stats s0, s1;
  start(no_frameskip);
  nspire_audio_output = NSPIRE_OUTPUT_AUTO;
  hp_on = 1;
  nspire_bt_live = 1;
  hp_delay = 273;
  run_for(L, 4);
  measure_begin(&a);
  nspire_audio_stats(&s0);
  for (unsigned k = 0; k < 180; k++) {     /* three minutes */
    if (wobble)
      hp_delay = 273 + (k % 3 == 0 ? 20 : k % 3 == 1 ? -20 : 0);   /* (within SYNC_SLACK_MS 30) */
    run_for(L, 1);
  }
  snap(&b);
  nspire_audio_stats(&s1);
  r = diff(&a, &b);
  print(name, &r);
  printf("     %u re-syncs to the headphones (%u after a gap in the sound), the card %u ms behind them\n",
         s1.syncs - s0.syncs, s1.sync_gaps - s0.sync_gaps, s1.sync_ms);
  if (!old_code) {
    T_EQ(s1.syncs - s0.syncs, 0);
    T_EQ(s1.sync_gaps - s0.sync_gaps, 0);
    T_EQ(r.wire_gaps, 0);
    T_EQ(r.silent_pkts, 0);
    T_TRUE(r.fps >= 59.5);
  }
  hp_on = 0;
  nspire_bt_live = 0;
  finish();
}

static void case_sync(void)
{
  sync_run("s-town", &town, 0);
  sync_run("s-moving", &moving, 0);
  sync_run("s-wobble", &town, 1);
}

/* Round 2's fallback: windows deliver nothing (an OS whose completions need
 * a task's sleep): trust is lost after 3 misses, sleeps serve the queue. */
static void case_fallback(void)
{
  int was = irq_usb;
  irq_usb = 0;
  spare("f-sleeps", &town, 55);      /* round 1's sleeps, a picture left out where one comes late */
  irq_usb = was;
}

/* R11u seconds 130-135: the sound card pulled behind a kept adapter, the OS
 * no detach, no completion for 6 s (the ring full, every frame's sound
 * dropped, the rate at its +6%). A stall now: nothing produced, nothing
 * dropped, the game at full speed; it starts clean when the device takes
 * packets again. */
static void case_stall(void)
{
  struct snap a, b;
  struct result r;
  struct nspire_audio_stats s;
  start(no_frameskip);
  run_for(&town, 2);
  measure_begin(&a);
  run_for(&town, 10);
  fos_stall_device(1);
  run_for(&town, 6);
  nspire_audio_stats(&s);
  if (!old_code) {
    T_TRUE(s.stalled);
    T_EQ(s.rendering, 0);
  }
  fos_stall_device(0);
  run_for(&town, 2);
  snap(&b);
  r = diff(&a, &b);
  print("g-stall", &r);
  if (!old_code) {
    T_EQ(r.dropped, 0);
    T_EQ(r.stalls, 1);
    T_TRUE(r.fps >= 59.5);
    T_EQ(r.silent_pkts, 0);
  }
  nspire_audio_stats(&s);
  if (!old_code) {
    T_EQ(s.stalled, 0);
    T_TRUE(s.rendering);
  }
  measure_begin(&a);
  run_for(&town, MEASURE_S - 20);
  snap(&b);
  r = diff(&a, &b);
  check_full_speed("g-after", &r);
  finish();
}

static void case_no_spare(void)
{
  struct snap a, b;
  struct result r;
  start(no_frameskip);
  run_for(&heavy, 2);
  measure_begin(&a);
  run_for(&heavy, MEASURE_S);
  snap(&b);
  r = diff(&a, &b);
  print("b-no-spare", &r);
  if (!old_code) {
    /* Pictures skipped while sound plays (the setting is off): one in two
     * at most, so 20 ms a frame on average without the OS (50 a second
     * at best; faf629e: 33 drawing all, its sound in pieces); with the
     * OS's 10 ms tick about one sleep in two frames costing ~6.5 ms: 42.
     * The sound slower with the game (-29%), continuous. */
    T_TRUE(r.fps >= 42);
    T_TRUE(r.drawn_fps >= 20);
    T_EQ(r.silent_pkts, 0);
    T_EQ(r.wire_gaps, 0);
    T_TRUE(r.delay_max <= 150);
  }
  finish();
}

static void case_fast_forward(void)
{
  struct snap a, b;
  struct result r;
  start(no_frameskip);
  run_for(&town, 2);
  measure_begin(&a);
  run_for(&town, 10);
  synchronize_flag = 0;                 /* fast forward on */
  nspire_reset_timing();
  run_for(&town, 10);
  {
    struct nspire_audio_stats s;
    nspire_audio_stats(&s);
    if (!old_code) {
      T_TRUE(s.rendering);
      T_EQ(s.stalled, 0);
      T_TRUE(s.speed > 65536u * 11u / 10u);   /* resampler sped up with fast forward */
    }
  }
  synchronize_flag = 1;                 /* and off */
  nspire_reset_timing();
  struct snap m;
  snap(&m);
  r = diff(&a, &m);
  print("c-ff-on", &r);
  if (!old_code) {
    T_TRUE(r.fps >= 59.73 * 0.5 + 60 * 0.5);   /* fast forward faster than normal play */
    T_EQ(r.silent_pkts, 0);
    T_EQ(r.wire_gaps, 0);
    T_EQ(r.dropped, 0);
  }
  run_for(&town, 1);                    /* priming again */
  measure_begin(&a);
  run_for(&town, MEASURE_S - 21);
  snap(&b);
  r = diff(&a, &b);
  check_full_speed("c-ff-after", &r);
  {
    static const struct load fast_load = { 2.0, 0.5, 1.5, 0.5 };
    struct nspire_audio_stats s;
    synchronize_flag = 0;
    nspire_reset_timing();
    measure_begin(&a);
    run_for(&fast_load, 5);
    nspire_audio_stats(&s);
    snap(&b);
    r = diff(&a, &b);
    print("c-ff-fast", &r);
    if (!old_code) {
      T_TRUE(s.rendering);
      T_TRUE(s.speed > 65536u * 3u);
      T_EQ(r.silent_pkts, 0);
      T_EQ(r.wire_gaps, 0);
      T_EQ(r.dropped, 0);
    }
    synchronize_flag = 1;
    nspire_reset_timing();
  }
  finish();
}

static void case_menu(void)
{
  struct snap a, b;
  struct result r;
  start(no_frameskip);
  run_for(&town, 2);
  measure_begin(&a);
  run_for(&town, 20);
  menu(5);
  run_for(&town, 1);
  snap(&b);
  r = diff(&a, &b);
  print("d-menu", &r);
  if (!old_code) {
    T_EQ(r.silent_pkts, 0);
    T_EQ(r.wire_gaps, 0);
  }
  measure_begin(&a);
  run_for(&town, MEASURE_S - 26);
  snap(&b);
  r = diff(&a, &b);
  check_full_speed("d-after", &r);
  finish();
}

static void case_unplug(void)
{
  struct snap a, b;
  struct result r;
  start(no_frameskip);
  run_for(&town, 2);
  measure_begin(&a);
  run_for(&town, 10);
  fos_unplug();
  run_for(&town, 3);
  T_TRUE(ua_state() == UA_GONE || ua_state() == UA_WAITING);
  struct fos_spec s = {0x0d8c, 0x000c, cfg_cm108, sizeof cfg_cm108, 0};
  T_EQ(fos_plug(&s), 0);
  run_for(&town, 2);
  T_EQ(ua_state(), UA_STREAMING);
  snap(&b);
  r = diff(&a, &b);
  print("e-unplug", &r);
  if (!old_code)
    T_TRUE(r.fps >= 59.0);              /* the game near full speed through the pull (setup's sleeps: 16 ms) */
  measure_begin(&a);
  run_for(&town, MEASURE_S - 15);
  snap(&b);
  r = diff(&a, &b);
  check_full_speed("e-after", &r);
  finish();
}

/* ---- The rule (2026-11-15, final/pace) ----
 * If the game runs at 60 game frames per second or higher, 60 pictures are
 * always drawn (visual smoothness past 60; game speed below 60); on fast
 * forward the pictures are real, not skipped ones. Automatic frameskip (the default,
 * and what a stored Off becomes once: nspire_config.c), without the adapter
 * and with it streaming (rule (f): the "-s" lines), 20 s measured a case:
 *   rule-a  spare time: 60 game frames, 60 pictures;
 *   rule-b  a few percent too heavy: 60 game frames, pictures only as few
 *           below 60 as the frames' costs call for (within 3 a second);
 *   rule-c  far too heavy (the CX CAS): as fast as the floor allows (at most
 *           Frameskip value pictures left out in a row: c4 the default 4, c1
 *           the reference 1), the floor held;
 *   rule-d  fast forward with room: 60 pictures, never more, the game well
 *           over 60;
 *   rule-e  fast forward where 60 pictures leave under 60 game frames:
 *           pictures give way, the game at 60.
 * And everywhere: in fast forward a game over 61 has 60 pictures; in normal
 * play a game under full speed has only the floor's pictures.
 * --rule-off: the same with frameskip off (e451ab7's setting on the reference
 * calculators: the before; Off draws every picture, so b, c, e fail there by
 * design). */
#define RULE_S 20
static unsigned rule_type = 0;   /* auto_frameskip (NSPIRE_LIBRETRO's 0) */

/* Ms a frame of the game's time the OS took (windows, sleeps outside waits). */
static double os_per_frame(const struct result *r)
{
  return r->fps > 0 ? r->os_ms_s / r->fps : 0;
}

/* Pictures a second that hold the game at full speed with the fewest left
 * out, by the costs' means: a share x of them skipped where
 * (game + draw + os) - x draw = a GBA frame. */
static double rule_ideal(const struct load *L, const struct result *r)
{
  double g = L->game_ms + L->game_spread / 2 + os_per_frame(r), d = L->draw_ms + L->draw_spread / 2;
  double x = (g + d - 1000.0 / 59.7275) / d;
  return 59.7275 * (1 - (x < 0 ? 0 : x > 1 ? 1 : x));
}

static struct result rule_run(const char *name, const struct load *L, int adapter, int ff, unsigned value)
{
  struct snap a, b;
  struct result r;
  char label[32];
  start_with(rule_type, adapter);
  frameskip_value = value;
  nspire_frameskip_reset();
  run_for(L, 2);
  if (ff) {
    synchronize_flag = 0;              /* fast forward on, unlimited (nspire_ff_max's default) */
    nspire_reset_timing();
    run_for(L, 1);
  }
  if (adapter)
    T_EQ(ua_state(), UA_STREAMING);
  measure_begin(&a);
  run_for(L, RULE_S);
  snap(&b);
  r = diff(&a, &b);
  snprintf(label, sizeof(label), "%s%s", name, adapter ? "-s" : "");
  printf("pace %-10s game %5.2f fps, pictures %5.2f (LCD %5.2f); OS %5.1f ms/s; ideal %5.2f\n", label, r.fps,
         r.drawn_fps, r.lcd_fps, r.os_ms_s, rule_ideal(L, &r));
  synchronize_flag = 1;
  nspire_audio_stop();
  T_EQ(fos_stats()->violations, 0);
  if (old_code)
    return r;
  if (ff) {
    /* rule 2: never more than 60 pictures; 60 whenever the game is over 60 */
    T_TRUE(r.lcd_fps <= 60.5 && r.drawn_fps <= 60.5);
    if (r.fps > 61)
      T_TRUE(r.lcd_fps >= 58.5);
  } else if (r.fps < 59.5)
    T_TRUE(r.drawn_fps <= r.fps / (value + 1) + 1.5);   /* rule 1: below full speed, only the floor's pictures */
  return r;
}

/* The CX II in a town (10-14 ms drawn), a scene 3% too heavy (17.2 ms
 * drawn, 10.5 skipped), the CX CAS (23-27 drawn, 14-16 skipped), fast
 * forward's 18.5 ms drawn (9.5 skipped: 54 game frames drawing all). */
static struct load rule_heavier = { 10.0, 1.0, 6.4, 0.6 };
static struct load rule_ff_heavy = { 9.0, 1.0, 8.5, 1.0 };

static void case_rule(void)
{
  for (int adapter = 0; adapter <= 1; adapter++) {
    struct result r;
    double ideal;
    r = rule_run("rule-a", &town, adapter, 0, 4);
    if (!old_code) T_TRUE(r.fps >= 59.5 && r.drawn_fps >= 59.5);
    r = rule_run("rule-b", &rule_heavier, adapter, 0, 4);
    ideal = rule_ideal(&rule_heavier, &r);
    if (!old_code) {
      T_TRUE(r.fps >= 59.5);
      T_TRUE(r.drawn_fps < 59.5);                       /* some left out: the frames don't fit */
      T_TRUE(r.drawn_fps >= ideal - 3);                 /* only as many as needed (R11u's 45 for a few %) */
    }
    for (int k = 0; k < 2; k++) {
      /* the floor: one picture in value + 1, the game as fast as that goes */
      unsigned value = k ? 1 : 4;
      double n = value + 1, os, floor_fps;
      r = rule_run(value == 4 ? "rule-c4" : "rule-c1", &heavy, adapter, 0, value);
      os = os_per_frame(&r);
      floor_fps = 1000.0 * n / (n * (heavy.game_ms + heavy.game_spread / 2 + os) + heavy.draw_ms + heavy.draw_spread / 2);
      if (floor_fps > 59.7275) floor_fps = 59.7275;
      printf("pace %-10s the floor's speed %.2f fps, %.2f pictures\n", "", floor_fps, floor_fps / n);
      if (!old_code) {
        T_TRUE(r.fps >= floor_fps - 1.5);
        T_TRUE(r.drawn_fps >= r.fps / n - 0.5);       /* the floor held */
      }
    }
    r = rule_run("rule-d", &town, adapter, 1, 4);
    if (!old_code) T_TRUE(r.lcd_fps >= 58.5 && r.fps >= 90);
    r = rule_run("rule-e", &rule_ff_heavy, adapter, 1, 4);
    ideal = rule_ideal(&rule_ff_heavy, &r);
    if (!old_code) {
      T_TRUE(r.fps >= 59.5 && r.fps <= 62);
      T_TRUE(r.drawn_fps >= ideal - 3 && r.drawn_fps <= ideal + 3);
    }
  }
}

/* ---- The CX II's LCD at a GBA frame (2026-11-17, final/pace6) ----
 * The reference CX II (build 2b8d4d6, Emerald, sound through a C-Media adapter
 * on a hub): "i am not getting even 60 drawn with sound in"; its log 60/50-55
 * pictures the first 30 s, 55-58 after, 60/60 while the adapter took nothing.
 * Normal play with nspire.c lcd_tune's LCD (the hardware-like model above),
 * automatic frameskip at the reference 1, the windows at the log's 1.6 ms (22-30
 * a second, 35-48 ms), the OS's own time at a wake lcd_wake_ms. Each line:
 * game frames and pictures a second, the pictures the LCD showed and those it
 * never took, the game's time the OS took (sleeps outside waits and
 * windows), and the sleeps inside waits (the spare time's, the LCD wait's). */
static double lcd_win_ms = 1.6, lcd_wake_ms = 2.5;
static struct result lcd_run(const char *name, const struct load *L, int adapter, int hub, int ff)
{
  struct snap a, b;
  struct result r;
  char label[40];
  double was_win = win_ms, was_wake = os_wake_ms;
  unsigned idle_sleeps;
  win_ms = lcd_win_ms;
  os_wake_ms = lcd_wake_ms;
  lcd_cx2 = 1;
  if (getenv("PACE_LABEL"))
    printf("  run %s adapter %d hub %d pad %d ff %d\n", name, adapter, hub, pad_mode, ff);
  start_with(0 /* auto_frameskip */, adapter);
  rnd += steady_seed * 7919;           /* (--seed: other frame costs, other wakes) */
  jrnd += steady_seed * 104729;
  frameskip_value = 1;                 /* the reference: at most 1 in a row */
  nspire_frameskip_reset();
  hub_mode = hub;
  run_for(L, 2);
  if (ff) {
    synchronize_flag = 0;
    nspire_reset_timing();
    run_for(L, 1);
  }
  if (adapter)
    T_EQ(ua_state(), UA_STREAMING);
  measure_begin(&a);
  unsigned lost0 = lcd_lost, waits0 = lcd_waits;
  double waited0 = lcd_waited;
  run_for(L, RULE_S);
  snap(&b);
  r = diff(&a, &b);
  idle_sleeps = (b.sleeps - a.sleeps) - (b.sleeps_busy - a.sleeps_busy);
  snprintf(label, sizeof(label), "%s%s%s%s%s", name, adapter ? "-s" : "", hub == 1 ? "-hub" : hub == 2 ? "-hubq" : "",
           pad_mode ? "-pad" : "", ff ? "-ff" : "");
  printf("pace %-16s game %5.2f fps, pictures %5.2f, LCD showed %5.2f, lost %4.2f; OS %5.1f ms/s (%2u busy sleeps/s); "
         "%2.0f sleeps/s in waits; %2.0f LCD waits/s, %4.1f ms/s\n", label, r.fps, r.drawn_fps, r.lcd_fps, (lcd_lost - lost0) / ((b.t - a.t) / NSPIRE_TICK_HZ),
         r.os_ms_s, (unsigned)((b.sleeps_busy - a.sleeps_busy) / ((b.t - a.t) / NSPIRE_TICK_HZ) + 0.5),
         idle_sleeps / ((b.t - a.t) / NSPIRE_TICK_HZ), (lcd_waits - waits0) / ((b.t - a.t) / NSPIRE_TICK_HZ),
         (lcd_waited - waited0) / TICKS_PER_MS / ((b.t - a.t) / NSPIRE_TICK_HZ));
  synchronize_flag = 1;
  hub_mode = 0;
  nspire_audio_stop();
  T_EQ(fos_stats()->violations, 0);
  lcd_cx2 = 0;
  win_ms = was_win;
  os_wake_ms = was_wake;
  return r;
}

/* Emerald on the CX II: the town (10-14 ms a drawn frame), walking (14-16),
 * and between (12-16). */
static struct load lcd_mid = { 9.0, 1.0, 3.0, 3.0 };
static int lcd_check = 1;

static void case_lcd(void)
{
  const struct { const char *name; const struct load *L; double least; } loads[] = {
    { "lcd-town", &town, 59.5 }, { "lcd-mid", &lcd_mid, 59.0 }, { "lcd-moving", &moving, 0 } };
  for (unsigned i = 0; i < sizeof(loads) / sizeof(loads[0]); i++) {
    for (int setup = 0; setup < 4; setup++) {
      int adapter = setup > 0, hub = setup == 2 ? 2 : setup == 3 ? 1 : 0;
      struct result r = lcd_run(loads[i].name, loads[i].L, adapter, hub, 0);
      if (old_code || !lcd_check)
        continue;
      T_TRUE(r.fps >= 59.5);
      /* every picture drawn reaches the LCD (none replaced unseen but by
       * jitter: at most one a second) */
      T_TRUE(r.lcd_fps >= r.drawn_fps - 1.0);
      if (loads[i].least && hub != 1)
        T_TRUE(r.drawn_fps >= loads[i].least);
    }
  }
  /* Fast forward: a picture at each refresh (the LCD's 59.6-59.9), every
   * one the LCD shows, the game as fast as the rest allows: Emerald's town
   * as above, and the speed model's ff scene on the CX II (game 3.31 ms, a
   * drawn frame 8.72: tests/speed_table.sh). */
  static const struct load ff_scene = { 3.1, 0.4, 5.2, 0.8 };
  for (int setup = 0; setup < 3; setup++) {
    int adapter = setup > 0, hub = setup == 2 ? 2 : 0;
    struct result r = lcd_run("lcd-town", &town, adapter, hub, 1);
    if (!old_code && lcd_check)
      T_TRUE(r.lcd_fps >= 58.5 && r.lcd_fps >= r.drawn_fps - 0.5 && r.fps >= 80);
    r = lcd_run("lcd-ffscene", &ff_scene, adapter, hub, 1);
    if (!old_code && lcd_check)
      T_TRUE(r.lcd_fps >= 58.5 && r.lcd_fps >= r.drawn_fps - 0.5 && r.fps >= 150);
  }
}

/* ---- Steady pictures (2026-11-21, final/r19-pace) ----
 * The reference CX II on r18 (7baa00e, Emerald, a
 * USB sound card, 4 x 14 ms transfers): drawn frames were often
 * inconsistent when the game frames were above 60; they should be a
 * solid 60. Its sound seconds: of 246 at 59-61 game frames,
 * 119 drew every picture, 61 one fewer, 18 two, 25 more (58.93 a second for
 * 60.13); windows 23-28 a second at 1.57 ms, the OS's wake 1.23 ms after its
 * 10.28 ms tick, 0-23 ms a second of sleeps outside waits. Each run: the
 * LCD as lcd_tune leaves it, automatic frameskip at the reference 1, 20 s;
 * per second (60 refreshes) the new pictures the LCD showed (min / mean),
 * the pictures drawn, the refreshes that held the last picture (repeats) and
 * the pictures shown more than a game frame after the last one (jumps: a
 * frame's picture left out). Set-ups: no sound, the USB card, the card on a
 * hub (quiet), Bluetooth headphones alone, both. Loads: Emerald-like frames
 * from light to as heavy as fits, and one that doesn't fit (the rule's
 * "game speed first" below 60). */
static double steady_win_ms = 1.57, steady_wake_ms = 1.23, steady_tick_ms = 10.28, steady_lcd_ms = 16.743;
static double steady_hiccups = 1.5;
static int steady_check = 1;
struct steady_result { struct result r; struct bins b; double cost; unsigned ro_changes; char ro_text[64]; };
void pace_fps_overlay_reset(void);
static struct steady_result steady_run(const char *name, const struct load *L, int adapter, int hub, int bt, int ff)
{
  struct snap a, b;
  struct steady_result out;
  char label[48];
  double was_win = win_ms, was_wake = os_wake_ms, was_tick = os_tick_ms, was_lcd = lcd_period_ms;
  win_ms = steady_win_ms;
  lcd_period_ms = steady_lcd_ms;
  hiccups = steady_hiccups;
  hic_rnd = 4242 + steady_seed;
  bt_rnd = 777 + steady_seed;
  os_wake_ms = steady_wake_ms;
  os_tick_ms = steady_tick_ms;
  lcd_cx2 = 1;
  start_with(0 /* auto_frameskip */, adapter);
  rnd += steady_seed * 7919;
  jrnd += steady_seed * 104729;
  nspire_fps_overlay = 1;              /* (the readout: its changes counted) */
  pace_fps_overlay_reset();            /* (nspire_fps_overlay_reset, pace.mk's name: the last run's counts gone) */
  frameskip_value = 1;                 /* the reference: at most 1 in a row */
  nspire_frameskip_reset();
  hub_mode = hub;
  bt_on = hp_on = bt;
  nspire_bt_live = bt;
  run_for(L, 2);
  if (ff) {
    synchronize_flag = 0;
    nspire_reset_timing();
    run_for(L, 1);
  }
  measure_begin(&a);
  bins_begin();
  ro_changes = ro_frames = 0;
  stale_flips = 0;
  run_for(L, RULE_S);
  snap(&b);
  out.b = bins_take();
  out.ro_changes = ro_changes;
  snprintf(out.ro_text, sizeof(out.ro_text), "%s", nspire_fps_text);
  out.r = diff(&a, &b);
  /* a drawn frame's mean cost with the OS's time (windows, sleeps outside waits) and Bluetooth's */
  out.cost = L->game_ms + L->game_spread / 2 + L->draw_ms + L->draw_spread / 2 + os_per_frame(&out.r) +
             (bt ? 0.35 + 0.21 + 0.72 * 0.5 : 0);
  snprintf(label, sizeof(label), "%s%s%s%s%s", name, adapter ? "-s" : "", hub == 1 ? "-hub" : hub == 2 ? "-hubq" : "",
           bt ? "-bt" : "", ff ? "-ff" : "");
  printf("pace %-22s game %6.2f, drawn %5.2f (min %5.2f), shown %5.2f (min %5.2f) a second; repeats %3u (%2u at most"
         " in a second), jumps %3u; drawn frame %5.2f ms with the OS's; OS %5.1f ms/s, %2u windows/s; wire gaps %u,"
         " silent packets %u; seconds with 0/1/2/3+ left out %u/%u/%u/%u; readout \"%s\", %u changes; stale flips %u\n", label, out.r.fps, out.b.drawn_mean, out.b.drawn_min, out.b.shown_mean, out.b.shown_min,
         out.b.repeats, out.b.worst_repeats, out.b.jumps, out.cost, out.r.os_ms_s, out.r.windows_s, out.r.wire_gaps,
         out.r.silent_pkts, out.b.left[0], out.b.left[1], out.b.left[2], out.b.left[3], out.ro_text, out.ro_changes,
         stale_flips);
  /* every flip shows a newer picture than the last: none flipped twice */
  if (!old_code)
    T_EQ(stale_flips, 0);
  nspire_fps_overlay = 0;
  synchronize_flag = 1;
  hub_mode = 0;
  bt_on = hp_on = 0;
  nspire_bt_live = 0;
  nspire_audio_stop();
  T_EQ(fos_stats()->violations, 0);
  lcd_cx2 = 0;
  win_ms = was_win;
  os_wake_ms = was_wake;
  os_tick_ms = was_tick;
  lcd_period_ms = was_lcd;
  hiccups = 0;
  return out;
}

/* Fast forward switched on and off (the reference log: seconds 456-502, the
 * key pressed every second or two): normal play, then 'ff_s' of fast
 * forward, back, 'times' times; each second's pictures over the whole run
 * (the switches' seconds among them). */
static void steady_toggle(const char *name, const struct load *L, int adapter, double ff_s, double normal_s,
                          int times, struct bins *out, struct result *res)
{
  struct snap a, b;
  double was_win = win_ms, was_wake = os_wake_ms, was_tick = os_tick_ms, was_lcd = lcd_period_ms;
  win_ms = steady_win_ms;
  os_wake_ms = steady_wake_ms;
  os_tick_ms = steady_tick_ms;
  lcd_period_ms = steady_lcd_ms;
  hiccups = steady_hiccups;
  hic_rnd = 4242;
  lcd_cx2 = 1;
  start_with(0 /* auto_frameskip */, adapter);
  frameskip_value = 1;
  nspire_frameskip_reset();
  run_for(L, 2);
  measure_begin(&a);
  bins_begin();
  stale_flips = 0;
  for (int k = 0; k < times; k++) {
    run_for(L, normal_s);
    if (getenv("PACE_REPEATS")) printf("      at %.3f s: fast forward on\n", vt / NSPIRE_TICK_HZ);
    synchronize_flag = 0;
    nspire_reset_timing();             /* (nspire_host_main.c do_hotkeys' ACTION_FAST_FORWARD) */
    run_for(L, ff_s);
    if (getenv("PACE_REPEATS")) printf("      at %.3f s: fast forward off\n", vt / NSPIRE_TICK_HZ);
    synchronize_flag = 1;
    nspire_reset_timing();
  }
  run_for(L, normal_s);
  snap(&b);
  *out = bins_take();
  *res = diff(&a, &b);
  printf("pace %-22s game %6.2f, drawn %5.2f (min %5.2f), shown %5.2f (min %5.2f) a second; repeats %3u (%2u at most"
         " in a second), jumps %3u; wire gaps %u, silent packets %u; stale flips %u\n", name, res->fps, out->drawn_mean, out->drawn_min,
         out->shown_mean, out->shown_min, out->repeats, out->worst_repeats, out->jumps, res->wire_gaps,
         res->silent_pkts, stale_flips);
  if (!old_code)
    T_EQ(stale_flips, 0);
  nspire_audio_stop();
  T_EQ(fos_stats()->violations, 0);
  lcd_cx2 = 0;
  hiccups = 0;
  win_ms = was_win;
  os_wake_ms = was_wake;
  os_tick_ms = was_tick;
  lcd_period_ms = was_lcd;
}

/* Emerald on the CX II, light to heavy (ms: game + spread, drawing + spread) */
static struct load steady_loads[] = {
  { 7.0, 1.0, 3.0, 3.0 },    /* town, 10-14 */
  { 9.0, 1.0, 3.0, 3.0 },    /* 12-16 (lcd_mid) */
  { 10.0, 1.0, 4.0, 1.0 },   /* walking, 14-16 */
  { 9.5, 2.0, 3.5, 2.0 },    /* 13-17.5 */
  { 10.0, 2.0, 3.5, 1.5 },   /* 13.5-17 */
  { 11.0, 2.0, 4.0, 2.0 },   /* 15-19: doesn't fit */
};
static const char *const steady_names[] = { "st-town", "st-mid", "st-moving", "st-wide", "st-edge", "st-over" };
static int steady_load_only = -1;

static void case_steady(void)
{
  static const struct { int adapter, hub, bt; } setups[] = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 2, 0 }, { 1, 1, 0 },
                                                              { 0, 0, 1 }, { 1, 0, 1 } };
  const double budget = steady_lcd_ms;
  unsigned nl = sizeof(steady_loads) / sizeof(steady_loads[0]);
  for (unsigned i = 0; i < nl; i++) {
    if (steady_load_only >= 0 && (unsigned)steady_load_only != i)
      continue;
    for (unsigned k = 0; k < sizeof(setups) / sizeof(setups[0]); k++)
      for (int ff = 0; ff < 2; ff++) {
        struct steady_result o = steady_run(steady_names[i], &steady_loads[i], setups[k].adapter, setups[k].hub,
                                            setups[k].bt, ff);
        if (old_code || !steady_check)
          continue;
        T_EQ(o.r.wire_gaps, 0);
        if (setups[k].adapter)
          T_EQ(o.r.silent_pkts, 0);
        if (ff) {
          /* fast forward: a picture at every refresh, never more; with the
           * game over 75 every second; nearer 60 (a drawn frame of 14-16 ms
           * and an OS sleep of up to 11.5 ms in it, the refresh not enough)
           * a refresh holds the last picture now and then (a few in 20 s,
           * up to four in one second) */
          T_TRUE(o.b.shown_mean <= 60.5);
          if (o.r.fps >= 75)
            T_TRUE(o.b.shown_min >= 59.6);
          else if (o.r.fps >= 61)
            T_TRUE(o.b.shown_min >= 55.7 && o.b.shown_mean >= 59.2);
          continue;
        }
        T_TRUE(o.r.fps >= 59.5);
        /* the frames fit (a drawn frame with the OS's time a millisecond under a refresh): every refresh a new
         * picture, every second, and the readout "fps: 60" (one change at most: its first seconds); else
         * (st-over) only as many left out as the shortfall needs */
        if (o.cost < budget - 1.0) {
          T_TRUE(o.b.shown_min >= 59.6 && o.b.left[1] + o.b.left[2] + o.b.left[3] == 0 && o.b.jumps == 0);
          T_TRUE(o.ro_changes <= 1 && !strcmp(o.ro_text, "fps: 60"));
        } else if (o.cost > budget + 0.2) {
          double d = steady_loads[i].draw_ms + steady_loads[i].draw_spread / 2, x = (o.cost - budget) / d;
          T_TRUE(o.b.drawn_mean >= 59.73 * (1 - x) - 3);
        }
      }
  }
  /* fast forward switched on for half a second every 2.5 s, the town and walking, with the card and without */
  for (unsigned i = 0; i < 3; i++)
    for (int adapter = 0; adapter < 2; adapter++) {
      struct bins bb;
      struct result rr;
      char label[48];
      if (steady_load_only >= 0 && (unsigned)steady_load_only != i)
        continue;
      snprintf(label, sizeof(label), "%s%s-toggle", steady_names[i], adapter ? "-s" : "");
      steady_toggle(label, &steady_loads[i], adapter, 0.5, 2.0, 6, &bb, &rr);
      if (old_code || !steady_check)
        continue;
      T_EQ(rr.wire_gaps, 0);
      /* (a switch from fast forward: a refresh holds the last picture where
       * the motion changes, at most two in a second) */
      T_TRUE(bb.shown_min >= 57.7 && bb.shown_mean >= 59.0);
    }
}

/* Fast forward on the CX II with the card (st-wide: its pictures
 * are often held back for the LCD, nspire_frameskip.c ff_flush), and another screen 40 times: 'menu', as
 * open_menu (nspire_frameskip_screen_taken first, the sound paused, the menu's flips, a new count after
 * it), or unannounced flips of another screen between two frames (what the frame loop isn't told of).
 * A picture held back then is never flipped after the other screen drew into the buffers (no stale
 * flip), and, unannounced (no new count), the pictures counted drawn are the ones flipped. */
void nspire_frameskip_screen_taken(void) __attribute__((weak));   /* (PACE_SRC's older nspire_frameskip.c: none) */
static void ff_screens_run(const char *name, int menu_like, int n_flips)
{
  const struct load *L = &steady_loads[0];
  double was_win = win_ms, was_wake = os_wake_ms, was_tick = os_tick_ms, was_lcd = lcd_period_ms;
  unsigned pending = 0, flips0, drawn0;
  win_ms = steady_win_ms;
  lcd_period_ms = steady_lcd_ms;
  hiccups = steady_hiccups;
  hic_rnd = 4242 + steady_seed;
  os_wake_ms = steady_wake_ms;
  os_tick_ms = steady_tick_ms;
  lcd_cx2 = 1;
  start_with(0 /* auto_frameskip */, 1);
  frameskip_value = 1;
  nspire_frameskip_reset();
  run_for(L, 2);
  synchronize_flag = 0;
  nspire_reset_timing();
  run_for(L, 1);
  stale_flips = 0;
  flips0 = game_flips;
  drawn0 = nspire_perf_totals.drawn;
  for (int k = 0; k < 40; k++) {
    run_for(L, 0.2 + 0.0037 * k);        /* (the screens at many phases of the pictures) */
    pending += nspire_os_wake_hook != 0;  /* (a picture held back now: the hook is set while one is) */
    if (menu_like) {
      if (nspire_frameskip_screen_taken)
        nspire_frameskip_screen_taken();
      nspire_audio_pause();
    }
    for (int i = 0; i < n_flips; i++) {
      spend(0.5 * TICKS_PER_MS);
      other_flip();
    }
    if (menu_like) {
      spend(0.1 * NSPIRE_TICK_HZ);
      nspire_reset_timing();
    }
  }
  run_for(L, 0.5);
  printf("pace %-22s %u screens with a picture held back of 40; stale flips %u", name, pending, stale_flips);
  if (!menu_like)
    printf("; pictures flipped %u, counted drawn %u", game_flips - flips0, nspire_perf_totals.drawn - drawn0);
  printf("\n");
  if (!old_code) {
    T_TRUE(pending >= 5);                /* (the case it is about, often) */
    T_EQ(stale_flips, 0);
    /* (a held-back picture let go was counted drawn) the pictures counted are the ones
     * flipped (one may be held back at the end) */
    if (!menu_like)
      T_TRUE(nspire_perf_totals.drawn - drawn0 <= game_flips - flips0 + 1 &&
             game_flips - flips0 <= nspire_perf_totals.drawn - drawn0 + 1);
  }
  synchronize_flag = 1;
  nspire_audio_stop();
  T_EQ(fos_stats()->violations, 0);
  lcd_cx2 = 0;
  hiccups = 0;
  win_ms = was_win;
  os_wake_ms = was_wake;
  os_tick_ms = was_tick;
  lcd_period_ms = was_lcd;
}

static void case_ff_screens(void)
{
  ff_screens_run("ff-menu-3", 1, 3);
  ff_screens_run("ff-menu-6", 1, 6);
  ff_screens_run("ff-other-3", 0, 3);
  ff_screens_run("ff-other-1", 0, 1);
}

/* The readout fed whole real seconds of counts (nspire_frameskip_end_frame
 * at even times in each: a second's game frames, its first 'drawn' of them drawn), its text after each
 * frame: when it changed and to what. */
static double ro_seg_t0;
static unsigned ro_seg_changes;
static char ro_seg_text[64];
static double ro_seg_at;                   /* when it last changed (s from the segment's start) */
static void ro_begin(void)
{
  set_time(ceil(vt / NSPIRE_TICK_HZ + 1) * NSPIRE_TICK_HZ);   /* (a whole second of the clock) */
  synchronize_flag = 1;
  nspire_pace = PACE_NORMAL;
  nspire_fps_overlay = 1;
  pace_fps_overlay_reset();
  nspire_reset_timing();
  ro_seg_t0 = vt;
  ro_seg_changes = 0;
  ro_seg_at = 0;
  snprintf(ro_seg_text, sizeof(ro_seg_text), "%s", nspire_fps_text);
}
static void ro_second(unsigned game, unsigned drawn)
{
  double t0 = vt;
  for (unsigned i = 0; i < game; i++) {
    set_time(t0 + (i + 0.5) * NSPIRE_TICK_HZ / game);
    skip_next_frame = i >= drawn;
    nspire_frameskip_end_frame(0);
    if (strcmp(ro_seg_text, nspire_fps_text)) {
      if (getenv("PACE_READOUT"))
        printf("      at %.3f s: the readout \"%s\"\n", (vt - ro_seg_t0) / NSPIRE_TICK_HZ, nspire_fps_text);
      snprintf(ro_seg_text, sizeof(ro_seg_text), "%s", nspire_fps_text);
      ro_seg_changes++;
      ro_seg_at = (vt - ro_seg_t0) / NSPIRE_TICK_HZ;
    }
  }
  set_time(t0 + NSPIRE_TICK_HZ);
  skip_next_frame = 0;
}
/* n seconds at a rate (the whole seconds' counts as its frames fall in them), pictures 'dpics' fewer */
static void ro_rate(double rate, unsigned n, unsigned dpics)
{
  static double acc;
  for (unsigned s = 0; s < n; s++) {
    unsigned c = (unsigned)(floor(acc + rate) - floor(acc));
    acc += rate;
    ro_second(c, c > dpics ? c - dpics : 0);
  }
}
/* (the changes after the first number: "--" at the start, then "fps: 60") */
#define RO_START 2
static void ro_report(const char *name, double change_s)
{
  printf("pace %-22s readout \"%s\", %u changes, the last %.2f s after the change\n", name, ro_seg_text,
         ro_seg_changes, ro_seg_at - change_s);
}
static void case_readout(void)
{
  static const unsigned ref_secs[] = { 60, 60, 59, 60, 61 };   /* (the reference r18 log at a steady rate) */
  double c;
  start_with(0 /* auto_frameskip */, 0);
  /* steady: 59.73 a second (59 or 60 by where the seconds' edges fall), and the reference 59-61: "fps: 60",
   * changed once (from "--") */
  ro_begin();
  ro_rate(59.73, 30, 0);
  ro_report("ro-steady", 0);
  if (!old_code)
    T_TRUE(ro_seg_changes <= RO_START && !strcmp(ro_seg_text, "fps: 60"));
  ro_begin();
  for (unsigned k = 0; k < 30; k++)
    ro_second(ref_secs[k % 5], ref_secs[k % 5]);
  ro_report("ro-reference", 0);
  if (!old_code)
    T_TRUE(ro_seg_changes <= RO_START && !strcmp(ro_seg_text, "fps: 60"));
  /* a picture lost every 4 s, the game steady (a second's count then 58 by its edges): no change */
  ro_begin();
  {
    double acc = 0.3;
    for (unsigned k = 0; k < 30; k++) {
      unsigned n = (unsigned)(floor(acc + 59.73) - floor(acc));
      acc += 59.73;
      ro_second(n, n - (k % 4 == 1));
    }
  }
  ro_report("ro-lost-one", 0);
  if (!old_code)
    T_TRUE(ro_seg_at <= 5 && !strcmp(ro_seg_text, "fps: 60"));   /* (its first seconds aside) */
  /* the game 60 -> 58 (every picture): "fps: 58" within about a second, then no change */
  ro_begin();
  ro_rate(59.73, 12, 0);
  c = (vt - ro_seg_t0) / NSPIRE_TICK_HZ;
  ro_rate(58.0, 10, 0);
  ro_report("ro-game-58", c);
  if (!old_code)
    T_TRUE(!strcmp(ro_seg_text, "fps: 58") && ro_seg_at - c <= 1.2 && ro_seg_changes == RO_START + 1);
  /* a real 58.4 (58 or 59 a second): 58 within about a second, settled after */
  ro_begin();
  ro_rate(59.73, 12, 0);
  c = (vt - ro_seg_t0) / NSPIRE_TICK_HZ;
  ro_rate(58.4, 4, 0);
  if (!old_code)
    T_TRUE(!strcmp(ro_seg_text, "fps: 58") && ro_seg_changes == RO_START + 1);
  ro_rate(58.4, 16, 0);
  ro_report("ro-game-58.4", c);
  if (!old_code)
    T_TRUE(!strcmp(ro_seg_text, "fps: 58") && ro_seg_changes == RO_START + 1);
  /* and back to 60 within about a second */
  c = (vt - ro_seg_t0) / NSPIRE_TICK_HZ;
  ro_rate(59.73, 6, 0);
  ro_report("ro-game-back", c);
  if (!old_code)
    T_TRUE(!strcmp(ro_seg_text, "fps: 60") && ro_seg_at - c <= 1.2);
  /* pictures 60 -> 57 and 60 -> 58, the game at 60: said within about a second */
  ro_begin();
  ro_rate(59.73, 12, 0);
  c = (vt - ro_seg_t0) / NSPIRE_TICK_HZ;
  ro_rate(59.73, 10, 3);
  ro_report("ro-drawn-57", c);
  if (!old_code)
    T_TRUE(!strcmp(ro_seg_text, "fps: 60  drawn: 57") && ro_seg_at - c <= 1.2 && ro_seg_changes == RO_START + 1);
  ro_begin();
  ro_rate(59.73, 12, 0);
  c = (vt - ro_seg_t0) / NSPIRE_TICK_HZ;
  ro_rate(59.73, 10, 2);
  ro_report("ro-drawn-58", c);
  if (!old_code)
    T_TRUE(!strcmp(ro_seg_text, "fps: 60  drawn: 58") && ro_seg_at - c <= 1.2 && ro_seg_changes == RO_START + 1);
  nspire_fps_overlay = 0;
  nspire_audio_stop();
}

/* A controller in, on the hub, with the sound (2026-11-17, final/speed8;
 * pad_tick): the reference r15 log, 60 game frames and 38-55 pictures with
 * the pad in, 58-60 without. Each load with the adapter and the hub, and
 * the pad in or not; --pad-old: the glue's rule at the key read (r15's,
 * the before; report only). Each pad line: the OS's sleeps a second the
 * pad's rule took (at the key read, by the frame loop: at its points or as
 * the lines saw the tick come), and the longest from one of the OS's turns
 * to the next (a pad's reports' gap: r15's rule, 33 ms then up to a frame
 * to the key read and a tick, about 60 ms). Checked: the game at 60, the
 * pictures, that gap within r15's. */
/* (speed9a) one pad run: k 0 without the pad, 1 with r15's rule as shipped
 * (the glue's at the key read, the tick's phase from the last wake:
 * model_timer off), 2 with the frame loop's (the tick's timer read) */
static struct result pad_run(const char *name, const struct load *L, int adapter, int ff, int k, double secs)
{
  int was_old = pad_old, was_timer = model_timer;
  unsigned s0 = pad_sleeps, l0, p0 = nspire_frameskip_os_sleeps(&l0), l1;
  struct result r;
  pad_mode = k > 0;
  if (k == 1) {
    pad_old = 1;
    model_timer = 0;
  }
  r = lcd_run(name, L, adapter, 1, ff);
  pad_mode = 0;
  pad_old = was_old;
  model_timer = was_timer;
  u32 p1 = nspire_frameskip_os_sleeps(&l1);
  if (k > 0)
    printf("     %s: the pad's sleeps a second: %.1f by the glue's rule at the key read, %.1f by the frame loop "
           "(%.1f as the lines saw the tick); the longest without the OS %.1f ms; the pad's turns %.1f a second, "
           "the longest between them %.1f ms\n", k == 1 ? "r15's rule" : "the frame loop",
           (pad_sleeps - s0) / secs, (p1 - p0) / secs, (l1 - l0) / secs, os_gap_max / TICKS_PER_MS,
           pad_turns / (double)RULE_S, pad_gap_max / TICKS_PER_MS);
  return r;
}

/* Checked: the frame
 * loop's pictures (game frames in fast forward) at least r15's rule's as
 * shipped, with the wake late by the OS's own time (anchored), the USB
 * task's time charged (--usb-task, 0.43 ms: a sleep serves the pad's report
 * only if it begins that long before its tick: "the pad's turns"); the OS's
 * turns never further apart than R1's bound (33.3 ms + a frame + a tick) and
 * the pad's no more than two of them. tests/audio pace sweeps: --lcd-wake 0.1-2.5, --jitter, --tick, --usb8. */
static void case_pad(void)
{
  const struct { const char *name; const struct load *L; double least; } loads[] = {
    { "pad-town", &town, 59.5 }, { "pad-mid", &lcd_mid, 59.5 }, { "pad-moving", &moving, 59.0 } };
  /* Fast forward (2026-11-17: the reference bar, 120 game frames a second
   * and 60 pictures with all of it plugged in): the speed model's ff scene
   * on the CX II (as case_lcd's) with the pad, without and with the sound
   * adapter. */
  static const struct load ff_scene = { 3.1, 0.4, 5.2, 0.8 };
  double gap_frame = (USBPAD_TICK_FORCE + 548 + os_tick_ms * TICKS_PER_MS) / TICKS_PER_MS + 1;
  int check = !old_code && !pad_old && lcd_check;
  for (int adapter = 0; adapter < 2; adapter++) {
    struct result r0 = { 0 };
    for (int k = 0; k < 3; k++) {
      if (k == 1 && pad_old)
        continue;
      struct result r = pad_run("pad-ffscene", &ff_scene, adapter, 1, k, RULE_S + 3);
      if (k == 1)
        r0 = r;
      if (!check || k == 1)
        continue;
      if (!model_timer) {
        /* (--no-timer: where the timer isn't read, the frame loop leaves the
         * pad to the glue's rule: r15's, the same frames) */
        T_TRUE(k == 0 || (r.fps == r0.fps && r.drawn_fps == r0.drawn_fps && r.os_ms_s == r0.os_ms_s));
        continue;
      }
      T_TRUE(r.lcd_fps >= 58.5 && r.lcd_fps >= r.drawn_fps - 0.5 && (k == 0 ? r.fps >= 150 : r.fps >= r0.fps - 0.5));
      /* (a fast forward frame: its drawn one, 9 ms at most here) */
      if (k == 2 && !(pad_usb8 && adapter)) {
        double gap_ff = (USBPAD_TICK_FORCE + os_tick_ms * TICKS_PER_MS) / TICKS_PER_MS + 10;
        T_TRUE(os_gap_max / TICKS_PER_MS <= gap_ff);
        T_TRUE(!usb_task_ms || pad_gap_max / TICKS_PER_MS <= 2 * gap_ff);
      }
    }
  }
  for (unsigned i = 0; i < sizeof(loads) / sizeof(loads[0]); i++)
    for (int adapter = 1; adapter >= 0; adapter--) {
      /* (speed9a: and without the sound adapter: the hub and the pad alone;
       * each with the pad's ticks by r15's rule first, then the frame
       * loop's: at least as many pictures) */
      struct result r0 = { 0 };
      for (int k = 0; k < 3; k++) {
        if (k == 1 && pad_old)
          continue;                    /* (--pad-old: r15's rule only) */
        struct result r = pad_run(loads[i].name, loads[i].L, adapter, 0, k, RULE_S + 2);
        if (k == 1)
          r0 = r;
        if (k != 2 || !check)
          continue;
        if (!model_timer) {
          T_TRUE(r.fps == r0.fps && r.drawn_fps == r0.drawn_fps && r.os_ms_s == r0.os_ms_s);
          continue;
        }
        T_TRUE(r.fps >= 59.5);
        T_TRUE(r.drawn_fps >= r0.drawn_fps - 0.05 || r.drawn_fps >= loads[i].least);
        T_TRUE(r.lcd_fps >= r.drawn_fps - 1.0);
        T_EQ(r.silent_pkts, 0);
        if (!(pad_usb8 && adapter)) {
          T_TRUE(os_gap_max / TICKS_PER_MS <= gap_frame);
          T_TRUE(!usb_task_ms || pad_gap_max / TICKS_PER_MS <= 2 * gap_frame);
        }
      }
    }
}

/* The reference CX II saving in Emerald with the sound on (case_save): the
 * game 12-16 ms a frame (lcd_mid), the LCD as lcd_tune leaves it, windows
 * 1.6 ms. Each line: the game's frames and the pictures the LCD showed in
 * the 6 s around the save (4 before r19-snd), the longest time between two pictures shown (the
 * hitch: 16.7-33 ms is no hitch), the time the game's sound was off (paused
 * or priming: silence gpSP chose), the device's own gaps (the bus ran dry:
 * a click), silent packets (the ring ran dry while playing).
 * r19-snd: the kinds at normal speed, in fast forward (-ff) and with
 * Bluetooth headphones beside the card (-bt: the card held back to them);
 * the r19 kind is the reference 7baa00e log (copy 25.9, pack 56.0, write
 * 229.7, replace 65.2 ms). */
#define SAVE_STATE_KB 49                     /* the r19 log's 49081 bytes */
#define SAVE_CHUNK_KB 8                      /* nspire_states.c SJ_WRITE_BYTES */

/* the OS's parts of a file's write of kb KB that took write_ms in all, an
 * outlier (write_ms over the usual's) in its middle part; then the replace's
 * steps (3 for a new name, 6 over an old one), replace_ms in all */
static void save_os_parts_sealed(double kb, double write_ms, double replace_ms, int over, int seal)
{
  unsigned chunks = (unsigned)((kb + SAVE_CHUNK_KB - 1) / SAVE_CHUNK_KB), i, steps = over ? 6 : 3;
  double usual = 6 + chunks * (1 + 0.2 * SAVE_CHUNK_KB) + 3, scale = write_ms < usual ? write_ms / usual : 1;
  double extra = write_ms > usual ? write_ms - usual : 0;
  save_parts[save_nparts++] = (struct save_part){ 6 * scale, 1 };            /* the create */
  for (i = 0; i < chunks; i++)
    save_parts[save_nparts++] = (struct save_part){ (1 + 0.2 * SAVE_CHUNK_KB) * scale + (i == chunks / 2 ? extra : 0), 1 };
  save_parts[save_nparts++] = (struct save_part){ 3 * scale, 1 };            /* the close */
  if (seal)   /* (r19-fix: the game's save, written as .part, renamed .tmp after its close) */
    save_parts[save_nparts++] = (struct save_part){ replace_ms / steps, 1 };
  for (i = 0; i < steps; i++)
    save_parts[save_nparts++] = (struct save_part){ replace_ms / steps, 1 };
}

static void save_os_parts(double kb, double write_ms, double replace_ms, int over)
{
  save_os_parts_sealed(kb, write_ms, replace_ms, over, 0);
}

enum { SAVE_NORMAL, SAVE_FF, SAVE_BT };
static double none_lcd[3];           /* (the LCD's pictures a second in the 6 s without a save, each mode) */
static double save_lost[3][8];       /* (pictures lost a save, normal play: the table's) */
static void save_run(const char *name, const struct load *L, int old, double write_ms, double replace_ms,
                     double copy_ms, double pack, int mode, double learned, int twice)
{
  struct snap a, b, m1;
  struct result r, r1;
  double own = 0, lost;
  double was_win = win_ms, was_wake = os_wake_ms;
  unsigned i;
  win_ms = lcd_win_ms;
  os_wake_ms = lcd_wake_ms;
  lcd_cx2 = 1;
  save_nparts = 0;
  trace_run = !getenv("PACE_TRACE_RUN") || !strcmp(getenv("PACE_TRACE_RUN"), name);
  if (write_ms < 0) {
    save_key_ms = 0;                       /* (none: the same 4 s without a save, for the counts' own level) */
  } else if (replace_ms < 0) {
    /* the game's own save (nspire_backup.c): Emerald's 128 KB gzipped at
     * level 6 in 8 KB parts (tests/libbench.sh sram-save-gz: 46.6 ms CX II),
     * then the write, the rename and the other form's removal: their times
     * on the CX II never measured (the next log says them: "game's save");
     * taken as the states' (write_ms, 37.8, and a stat and a remove 15) */
    save_key_ms = 1.0;
    for (i = 0; i < 16; i++)
      save_parts[save_nparts++] = (struct save_part){ 46.6 / 16, 0 };
    if (old)
      save_parts[save_nparts++] = (struct save_part){ write_ms, 1 };
    else
      save_os_parts_sealed(5, write_ms, 37.8, 1, 1);
    save_parts[save_nparts++] = (struct save_part){ 15.0, 1 };
    if (old)
      save_parts[save_nparts - 1].ms += 37.8;
  } else {
    save_key_ms = copy_ms;
    if (!old)
      save_parts[save_nparts++] = (struct save_part){ copy_ms, NSPIRE_SAVE_TAKE };   /* SJ_TAKE */
    for (i = 0; i < 26; i++)               /* 416 KB in 16 KB steps (SJ_PACK_BYTES) */
      save_parts[save_nparts++] = (struct save_part){ pack / 26, 0 };
    save_parts[save_nparts++] = (struct save_part){ 19.9 - save_scale_ms, 0 };   /* the picture packed */
    if (old) {
      save_parts[save_nparts++] = (struct save_part){ write_ms, 1 };
      save_parts[save_nparts++] = (struct save_part){ replace_ms, 1 };
    } else
      save_os_parts(SAVE_STATE_KB, write_ms, replace_ms, replace_ms > 50);
  }
  save_os_longest = 0;
  for (i = 0; i < save_nparts; i++) {
    if (save_parts[i].os != NSPIRE_SAVE_CPU && save_parts[i].ms > save_os_longest)
      save_os_longest = save_parts[i].ms;
    if (save_parts[i].os != NSPIRE_SAVE_CPU)
      own += save_parts[i].ms;
  }
  /* (learned < 0: what this kind's calls take, learned by the saves before: the steady state) */
  {
    double l = 0;
    for (i = 0; i < save_nparts; i++)
      if (save_parts[i].os == NSPIRE_SAVE_OS && save_parts[i].ms > l)
        l = save_parts[i].ms;
    nspire_save_call_ms = (uint32_t)(learned < 0 ? l + 0.5 : learned);
  }
  start_with(0 /* auto_frameskip */, 1);
  frameskip_value = 1;
  nspire_frameskip_reset();
  if (mode == SAVE_FF)
    synchronize_flag = 0;
  if (mode == SAVE_BT) {
    nspire_audio_output = NSPIRE_OUTPUT_AUTO;
    hp_on = 1;
    nspire_bt_live = 1;
    hp_delay = 273;
  }
  save_old = old;
  save_at = -1;
  frame_no = 0;
  last_frame_end = 0;
  /* (r19-bank: 40 s first, the ring's fill settled at its target by the rate's slow term as a game that has
   * run a while has it: the reference log's ring 51-75 ms at its lowest; 3 s before, the fill still ~30) */
  run_for(L, old ? 3 : 40);
  measure_begin(&a);
  flip_gap_max = 0;
  last_shown_flip = -1;
  sound_off = 0;
  save_at = (int)frame_no + 30;
  save_at2 = save_second ? save_at + (int)save_second : -1;
  save_queued = saves_done = 0;
  if (twice) {
    /* (two saves: what the first one did (a first save ever: heard once), then the second one measured,
     * the calculator as its first save left it) */
    run_for(L, 10);
    snap(&m1);
    r1 = diff(&a, &m1);
    printf("pace %-19s the first save: silent packets %u, re-primes %u, dry calls %u; learned %u ms\n", name,
           r1.silent_pkts, m1.s.reprimes - a.s.reprimes, m1.s.os_call_dry - a.s.os_call_dry,
           (unsigned)nspire_save_call_ms);
    if (twice > 0)
      T_TRUE(m1.s.os_call_dry - a.s.os_call_dry <= 1);   /* (the one long call not learned yet: heard once) */
    measure_begin(&a);
    flip_gap_max = 0;
    last_shown_flip = -1;
    sound_off = 0;
    save_at2 = (int)frame_no + 30;
  }
  run_for(L, old ? 6 : save_second ? 16 : 10);   /* (r19-snd: 6 s; r19-bank: 10 s, the bank given back in them;
                                                  * r19-fix: two saves, 16 s) */
  snap(&b);
  r = diff(&a, &b);
  lost = (none_lcd[mode] - r.lcd_fps) * (old ? 6 : save_second ? 16 : 10);
  printf("pace %-19s game %6.2f fps, LCD showed %5.2f; longest between pictures %5.1f ms (OS call %5.1f);"
         " sound off %5.1f ms, wire gaps %u ms, silent packets %u, restarts %u, frames' sound left out %u,"
         " re-primes %u, dropped %u; rate %+.2f..%+.2f%%", name, r.fps,
         r.lcd_fps, flip_gap_max / TICKS_PER_MS, save_os_longest, sound_off / TICKS_PER_MS, r.wire_gaps, r.silent_pkts,
         r.restarts, b.s.trims - a.s.trims, b.s.reprimes - a.s.reprimes, r.dropped, r.rate_lo, r.rate_hi);
  if (!old)
    printf("; bank %u frames run ahead, %u given back, %u not made up; %u calls (%u short), %u dry, left %u;"
           " learned %u ms; pictures lost %.1f (the OS calls' own time %.1f refreshes); re-syncs %u (after a gap %u)",
           b.s.banked - a.s.banked, b.s.bank_held - a.s.bank_held, b.s.bank_held_back - a.s.bank_held_back,
           b.s.save_calls - a.s.save_calls, b.s.save_short - a.s.save_short, b.s.os_call_dry - a.s.os_call_dry,
           b.s.bank, (unsigned)nspire_save_call_ms, lost, own / 16.74, b.s.syncs - a.s.syncs,
           b.s.sync_gaps - a.s.sync_gaps);
  if (!old)
    printf("; owed %u, made up %u, let go %u", b.s.owed - a.s.owed, b.s.repaid - a.s.repaid,
           b.s.owe_dropped - a.s.owe_dropped);
  printf("\n");
  if (!old && write_ms < 0)
    none_lcd[mode] = r.lcd_fps;
  if (!old && mode == SAVE_NORMAL && twice < 0)
    save_lost[0][-twice - 1] = lost;
  if (!old_code && !old && lcd_check) {
    T_EQ(r.wire_gaps, 0);                  /* the device never ran dry: no click */
    T_EQ(r.silent_pkts, 0);                /* the ring never ran dry: nothing silent on the wire */
    T_TRUE(sound_off == 0);                /* no pause, no priming: the sound went on */
    T_EQ(b.s.reprimes - a.s.reprimes, 0);
    T_EQ(b.s.trims - a.s.trims, 0);        /* no frame's sound left out */
    T_EQ(r.dropped, 0);                    /* nor dropped (ring full) */
    T_EQ(b.s.os_call_dry - a.s.os_call_dry, 0);
    T_EQ(b.s.os_call_dips - a.s.os_call_dips, 0);   /* (nor a faded end: no fade at all) */
    T_EQ(b.s.bank, 0);                     /* the bank all given back within the 4 s */
    if (save_second)
      T_EQ(saves_done, 2);                 /* (r19-fix) both saves whole, the second after the first */
    if (twice == -1) {
      /* (calls all short: nothing run ahead, no picture lost but in the OS calls' own time) */
      T_EQ(b.s.banked - a.s.banked, 0);
      if (mode == SAVE_NORMAL)
        T_TRUE(lost <= own / 16.74 + 1);
    }
    if (mode != SAVE_FF) {
      T_TRUE(r.rate_lo >= -0.21 && r.rate_hi <= 0.21);   /* the pitch within the drift term's +-0.2% */
      /* no stall but the OS call's own: the picture back within three refreshes of it */
      T_TRUE(flip_gap_max / TICKS_PER_MS <= save_os_longest + 3 * 16.7 + 1);
    }
  }
  save_at = save_at2 = -1;
  hp_on = 0;
  nspire_bt_live = 0;
  nspire_audio_output = NSPIRE_OUTPUT_AUTO;
  synchronize_flag = 1;
  nspire_audio_stop();
  T_EQ(fos_stats()->violations, 0);
  lcd_cx2 = 0;
  win_ms = was_win;
  os_wake_ms = was_wake;
}

/* A save in a game that can't hold 60 even with pictures left out (a
 * drawn frame of 20-24 ms, 17-18 ms skipped: ~47 a second; and one just under: ~57): its lateness is the
 * game's own, not the save's. Owed at most what the save's own parts took, nothing owed after the save
 * (within its settling, 2 s, and a frame or so), the save's state over then (the slow-game mode and the
 * re-prime available again), no burst of frames run ahead after it, the sound as without a save. */
static unsigned slow_base_silent, slow_base_reprimes;
static void save_slow_run(const char *name, const struct load *L, const struct load *then, double write_ms,
                          double replace_ms)
{
  struct snap a, b;
  struct result r;
  double was_win = win_ms, was_wake = os_wake_ms, cost_ms = 0;
  unsigned i;
  uint32_t silent_none;
  if (save_only && strcmp(save_only, name))
    return;
  win_ms = lcd_win_ms;
  os_wake_ms = lcd_wake_ms;
  lcd_cx2 = 1;
  save_nparts = 0;
  if (write_ms >= 0) {
    save_parts[save_nparts++] = (struct save_part){ 12.7 + save_scale_ms, NSPIRE_SAVE_TAKE };
    for (i = 0; i < 26; i++)
      save_parts[save_nparts++] = (struct save_part){ 48.7 / 26, 0 };
    save_parts[save_nparts++] = (struct save_part){ 19.9 - save_scale_ms, 0 };
    save_os_parts(SAVE_STATE_KB, write_ms, replace_ms, replace_ms > 50);
  }
  /* (the save's own time as nspire_audio.c reckons it: its OS calls and the take as timed, a CPU part
   * CPU_PART_MS, 3 ms, at most) */
  for (i = 0; i < save_nparts; i++)
    cost_ms += save_parts[i].os ? save_parts[i].ms : 3.0;
  nspire_save_call_ms = 0;
  for (i = 0; i < save_nparts; i++)
    if (save_parts[i].os == NSPIRE_SAVE_OS && save_parts[i].ms > nspire_save_call_ms)
      nspire_save_call_ms = (uint32_t)(save_parts[i].ms + 0.5);
  start_with(0 /* auto_frameskip */, 1);
  frameskip_value = 1;
  nspire_frameskip_reset();
  save_old = 0;
  save_at = save_at2 = -1;
  frame_no = 0;
  last_frame_end = 0;
  run_for(L, 40);
  /* the same game 10 s without a save: its own silent packets */
  measure_begin(&a);
  run_for(L, 10);
  snap(&b);
  silent_none = diff(&a, &b).silent_pkts;
  measure_begin(&a);
  sound_off = 0;
  save_done_at = -1;
  save_at = write_ms >= 0 ? (int)frame_no + 30 : -1;
  if (then) {
    run_for(L, 1);                  /* (a game that gets slow a second after the key: then the save's frames) */
    for (i = 0; i < 14; i++) {
      run_for(then, 1);
      if (getenv("PACE_SLOW")) {
        struct nspire_audio_stats st;
        struct ua_counters c;
        nspire_audio_stats(&st);
        ua_counters(&c);
        printf("      at %.1f s: slow-game mode %d, rate %+.2f%%, ring %u, the save's state %d, owe %u, silent packets"
               " %u, re-primes %u\n", vt / NSPIRE_TICK_HZ, st.slow, (st.speed / 65536.0 - 1) * 100, st.buffered,
               st.save_active, st.owe, c.underrun_pkts, st.reprimes);
      }
    }
  } else
    run_for(L, 10);
  snap(&b);
  r = diff(&a, &b);
  {
    double after_owe = (owe_last_at - save_done_at) / NSPIRE_TICK_HZ, after_active = (active_last_at - save_done_at) / NSPIRE_TICK_HZ;
    unsigned owed = b.s.owed - a.s.owed, repaid = b.s.repaid - a.s.repaid, dropped = b.s.owe_dropped - a.s.owe_dropped;
    if (write_ms < 0) {
      /* (the same game without a save: what the save is held against) */
      slow_base_silent = r.silent_pkts;
      slow_base_reprimes = b.s.reprimes - a.s.reprimes;
      printf("pace %-19s game %6.2f fps, LCD showed %5.2f; silent packets %u, re-primes %u, sound off %.1f ms;"
             " slow-game mode %d; rate %+.2f..%+.2f%%\n", name, r.fps, r.lcd_fps, r.silent_pkts, slow_base_reprimes,
             sound_off / TICKS_PER_MS, b.s.slow, r.rate_lo, r.rate_hi);
      T_EQ(r.wire_gaps, 0);
      goto done;
    }
    printf("pace %-19s game %6.2f fps, LCD showed %5.2f; silent packets %u (without a save %u), frames' sound left out"
           " %u, re-primes %u, sound off %.1f ms; owed %u frames (the save's own time %.1f), made up %u, let go %u, owe"
           " at the end %u, the last %.2f s after the save; the save's state over %.2f s after it (at the end %d);"
           " slow-game mode %d; rate %+.2f..%+.2f%%\n", name, r.fps, r.lcd_fps, r.silent_pkts, silent_none,
           b.s.trims - a.s.trims, b.s.reprimes - a.s.reprimes, sound_off / TICKS_PER_MS, owed, cost_ms / 16.74, repaid,
           dropped, b.s.owe, after_owe, after_active, b.s.save_active, b.s.slow, r.rate_lo, r.rate_hi);
    if (!old_code) {
      T_TRUE(save_done_at >= 0);                    /* (the save ended in the 10 s) */
      T_TRUE(owed <= cost_ms / 16.74 + 2);          /* what the save cost, no more */
      T_TRUE(repaid <= owed);                       /* no burst beyond it */
      T_EQ(b.s.owe, 0);
      /* nothing owed once the save is over, the save's state over: its settling (120 frames) at most */
      T_TRUE(after_owe <= 120 / r.fps + 0.1);
      T_TRUE(after_active <= 120 / r.fps + 0.1 && !b.s.save_active);
      T_TRUE(b.s.slow);                             /* the slow-game mode (on again, or entered after the save) */
      T_EQ(r.wire_gaps, 0);
      /* (the sound: what the save's own time takes from the ring the slow game can't make up: a call
       * longer than the ring is heard, then the re-prime: once) */
      /* (a call longer than the ring, no bank to cover it: heard, primed again, for it and the call after) */
      T_TRUE(b.s.reprimes - a.s.reprimes <= slow_base_reprimes + (write_ms > 100 ? 2 : 1));
    }
    (void)silent_none;
  }
done:
  save_at = save_at2 = -1;
  synchronize_flag = 1;
  nspire_audio_stop();
  T_EQ(fos_stats()->violations, 0);
  lcd_cx2 = 0;
  win_ms = was_win;
  os_wake_ms = was_wake;
}

static void case_save_slow(void)
{
  static const struct load slow = { 17.0, 1.0, 3.0, 3.0 }, under = { 15.0, 1.0, 3.0, 2.0 };
  save_slow_run("save-slow-none", &slow, NULL, -1, -1);
  save_slow_run("save-slow-usual", &slow, NULL, 31.8, 37.8);
  save_slow_run("save-slow-r19", &slow, NULL, 229.7, 65.2);
  save_slow_run("save-under-none", &under, NULL, -1, -1);
  save_slow_run("save-under-usual", &under, NULL, 31.8, 37.8);
  save_slow_run("save-under-r19", &under, NULL, 229.7, 65.2);
  /* a game at full speed that gets slow a second after the save's key: the slow-game mode entered */
  save_slow_run("save-then-slow-none", &lcd_mid, &slow, -1, -1);
  save_slow_run("save-then-slow-usual", &lcd_mid, &slow, 31.8, 37.8);
  save_slow_run("save-then-slow-r19", &lcd_mid, &slow, 229.7, 65.2);
  save_slow_run("save-then-under-none", &lcd_mid, &under, -1, -1);
  save_slow_run("save-then-under-r19", &lcd_mid, &under, 229.7, 65.2);
}

static void case_save(void)
{
  /* (final/r19-bank) the kinds as their calculator has learned them (learned -1: its own longest call):
   * usual ... r19 as before; short: all calls under 25 ms, nothing learned; c60, c120, c230: the usual
   * save with one call of 60, 120, 230 ms, learned; first-r19: the first save ever on a calculator whose
   * write takes 229.7 (nothing learned: heard once, reported), then a save after it (covered) */
  static const struct { const char *name; double write, replace, copy, pack, learned; int twice; } kinds[] = {
    { "none", -1, -1, 0, 0, 0, 0 },
    { "usual", 31.8, 37.8, 0, 48.7, -1, 0 }, { "overwrite", 33.9, 71.2, 0, 48.7, -1, 0 },
    { "slow write", 118.9, 36.1, 0, 48.7, -1, 0 }, { "slowest", 205.1, 38.7, 0, 48.7, -1, 0 },
    { "r19", 229.7, 65.2, 25.9, 56.0, -1, 0 }, { "game", 31.8, -1, 0, 0, -1, 0 },
    { "short", 24.0, 36.0, 0, 48.7, 0, -1 }, { "c60", 24.6 + 60, 37.8, 0, 48.7, -1, -2 },
    { "c120", 24.6 + 120, 37.8, 0, 48.7, -1, -3 }, { "c230", 24.6 + 230, 37.8, 0, 48.7, -1, -4 },
    { "first-r19", 229.7, 65.2, 25.9, 56.0, 0, 1 } };
  static const char *const modes[] = { "", "-ff", "-bt" };
  for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++)
    for (int m = 0; m < 3; m++)
      for (int old = m || kinds[k].twice ? 0 : 1; old >= 0; old--) {
        char label[40];
        if (kinds[k].twice > 0 && m)
          continue;
        snprintf(label, sizeof(label), "save-%s%s%s", kinds[k].name, modes[m], old ? "-old" : "");
        for (char *c = label; *c; c++)
          if (*c == ' ') *c = '-';
        if (save_only && strcmp(save_only, label) && strncmp(label, "save-none", 9))
          continue;
        save_run(label, &lcd_mid, old, kinds[k].write, kinds[k].replace,
                 kinds[k].copy ? kinds[k].copy : 12.7 + save_scale_ms, kinds[k].pack, m, kinds[k].learned,
                 kinds[k].twice);
      }
  printf("pace pictures lost a save (normal play, learned): calls under 25 ms %.1f, 60 ms %.1f, 120 ms %.1f,"
         " 230 ms %.1f\n", save_lost[0][0], save_lost[0][1], save_lost[0][2], save_lost[0][3]);
  /* the key again 20 frames after the first (an autosave's too): queued */
  {
    static const struct { const char *name; double write, replace, copy, pack; } k2[] = {
      { "save-twice-usual", 31.8, 37.8, 0, 48.7 }, { "save-twice-r19", 229.7, 65.2, 25.9, 56.0 } };
    for (unsigned k = 0; k < 2; k++)
      for (int m = 0; m < 3; m++) {
        char label[48];
        snprintf(label, sizeof(label), "%s%s%s", k2[k].name, modes[m], save_queue_old ? "-masked" : "");
        if (save_only && strcmp(save_only, label))
          continue;
        save_second = 20;
        save_run(label, &lcd_mid, 0, k2[k].write, k2[k].replace, k2[k].copy ? k2[k].copy : 12.7 + save_scale_ms,
                 k2[k].pack, m, -1, 0);
        save_second = 0;
      }
  }
}

/* r19-snd: the menu's Save and the load key, the sound paused on purpose:
 * how it stops and comes back. The menu: the sound paused, 0.5 s of the
 * menu's polls, a state saved at once (its parts' times as the r19 log's,
 * 400 ms: with states_serve_os the queue served all through it), 0.5 s
 * more, the game again. The load key: paused, the r19 log's load (read
 * 96.2, unpack 21.6, apply 17.1) served, the game again. The device must
 * never run dry (a wire gap: the card's own click), and the sound be back
 * within the priming's time after the game is (sound off after it). */
static int menu_old;                       /* (--menu-old: the load and the menu's save masked, 7baa00e's) */
static void served(double ms)
{
  uint32_t cpsr = 0;
  int began = !menu_old && nspire_audio_serve_begin(&cpsr);
  for (double t = 0; t < ms; t += 1) {
    spend((ms - t < 1 ? ms - t : 1) * TICKS_PER_MS);
    bus_masked();
    if (began)
      fos_irq_window();
  }
  nspire_audio_serve_end(began, cpsr);
}

static void pause_run(const char *name, int load)
{
  struct snap a, b;
  struct result r;
  double was_win = win_ms, was_wake = os_wake_ms, back;
  win_ms = lcd_win_ms;
  os_wake_ms = lcd_wake_ms;
  lcd_cx2 = 1;
  start_with(0 /* auto_frameskip */, 1);
  frameskip_value = 1;
  nspire_frameskip_reset();
  frame_no = 0;
  last_frame_end = 0;
  run_for(&lcd_mid, 3);
  measure_begin(&a);
  nspire_audio_pause();
  if (load)
    served(96.2 + 21.6 + 17.1);
  else {
    menu(0.5);
    served(25.9 + 56.0 + 12.2 + 229.7 + 65.2);
    menu(0.5);
  }
  nspire_reset_timing();
  sound_off = 0;
  last_frame_end = 0;
  run_for(&lcd_mid, 2);
  back = sound_off / TICKS_PER_MS;
  snap(&b);
  r = diff(&a, &b);
  printf("pace %-19s wire gaps %u ms, silent packets %u, restarts %u; the sound back %5.1f ms after the game\n",
         name, r.wire_gaps, r.silent_pkts, r.restarts, back);
  if (!old_code && !menu_old) {
    T_EQ(r.wire_gaps, 0);                  /* the device kept in packets: no click of its own */
    T_EQ(r.silent_pkts, 0);
    T_TRUE(back <= 100);                   /* the ring's priming (60 ms) and a frame or two */
  }
  nspire_audio_stop();
  T_EQ(fos_stats()->violations, 0);
  lcd_cx2 = 0;
  win_ms = was_win;
  os_wake_ms = was_wake;
}

static void case_pause(void)
{
  pause_run(menu_old ? "menu-save-old" : "menu-save", 0);
  pause_run(menu_old ? "load-key-old" : "load-key", 1);
}

int main(int argc, char **argv)
{
  setvbuf(stdout, NULL, _IONBF, 0);
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--old")) old_code = 1;
    else if (!strcmp(argv[i], "--rule-off")) rule_type = no_frameskip;
    else if (!strcmp(argv[i], "--log")) show_log = 1;
    else if (!strcmp(argv[i], "--irq-usb")) irq_usb = 1;
    else if (!strcmp(argv[i], "--no-irq-usb")) irq_usb = 0;
    else if (!strcmp(argv[i], "--window") && i + 1 < argc) win_ms = atof(argv[++i]);
    else if (!strcmp(argv[i], "--fit")) { win_ms = 0.41; comp_ms = 0.21; lcd_win_ms = 0.41; }
    else if (!strcmp(argv[i], "--trace") && i + 2 < argc) {
      trace_from = atof(argv[++i]);
      trace_to = atof(argv[++i]);
    }
    else if (!strcmp(argv[i], "--case") && i + 1 < argc) only = argv[++i];
    else if (!strcmp(argv[i], "--lcd-period") && i + 1 < argc) lcd_period_ms = atof(argv[++i]);
    else if (!strcmp(argv[i], "--lcd-window") && i + 1 < argc) lcd_win_ms = atof(argv[++i]);
    else if (!strcmp(argv[i], "--lcd-wake") && i + 1 < argc) lcd_wake_ms = atof(argv[++i]);
    else if (!strcmp(argv[i], "--lcd-report")) lcd_check = steady_check = 0;
    else if (!strcmp(argv[i], "--steady-load") && i + 1 < argc) steady_load_only = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--hiccups") && i + 1 < argc) steady_hiccups = atof(argv[++i]);
    else if (!strcmp(argv[i], "--seed") && i + 1 < argc) steady_seed = (uint32_t)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--steady-lcd") && i + 1 < argc) steady_lcd_ms = atof(argv[++i]);
    else if (!strcmp(argv[i], "--steady") && i + 3 < argc) {   /* window ms, wake ms, tick ms */
      steady_win_ms = atof(argv[++i]); steady_wake_ms = atof(argv[++i]); steady_tick_ms = atof(argv[++i]);
    }
    else if (!strcmp(argv[i], "--hub-old")) hub_old = 1;
    else if (!strcmp(argv[i], "--pad-old")) pad_old = 1;
    else if (!strcmp(argv[i], "--no-timer")) model_timer = 0;
    else if (!strcmp(argv[i], "--oracle")) oracle = 1;
    else if (!strcmp(argv[i], "--jitter") && i + 1 < argc) wake_jitter_ms = atof(argv[++i]);
    else if (!strcmp(argv[i], "--usb-task") && i + 1 < argc) usb_task_ms = atof(argv[++i]);
    else if (!strcmp(argv[i], "--usb8")) pad_usb8 = 1;
    else if (!strcmp(argv[i], "--scale") && i + 1 < argc) save_scale_ms = atof(argv[++i]);
    else if (!strcmp(argv[i], "--save-only") && i + 1 < argc) save_only = argv[++i];
    else if (!strcmp(argv[i], "--menu-old")) menu_old = 1;
    else if (!strcmp(argv[i], "--queue-old")) save_queue_old = 1;
    else if (!strcmp(argv[i], "--mid") && i + 4 < argc) {   /* game spread draw spread (ms) */
      lcd_mid.game_ms = atof(argv[++i]); lcd_mid.game_spread = atof(argv[++i]);
      lcd_mid.draw_ms = atof(argv[++i]); lcd_mid.draw_spread = atof(argv[++i]);
    }
    else if (!strcmp(argv[i], "--tick") && i + 1 < argc) os_tick_ms = atof(argv[++i]);
    else if (!strcmp(argv[i], "--wake") && i + 1 < argc) os_wake_ms = atof(argv[++i]);
    else if (!strcmp(argv[i], "--town") && i + 4 < argc) {   /* game spread draw spread (ms) */
      town.game_ms = atof(argv[++i]); town.game_spread = atof(argv[++i]);
      town.draw_ms = atof(argv[++i]); town.draw_spread = atof(argv[++i]);
    }
  }
  corpus_fix();
  if (wanted("a")) T_RUN(case_spare);
  if (wanted("b")) T_RUN(case_no_spare);
  if (wanted("c")) T_RUN(case_fast_forward);
  if (wanted("d")) T_RUN(case_menu);
  if (wanted("e")) T_RUN(case_unplug);
  if (wanted("f")) T_RUN(case_fallback);
  if (wanted("g")) T_RUN(case_stall);
  if (wanted("rule")) T_RUN(case_rule);
  if (wanted("lcd")) T_RUN(case_lcd);
  if (wanted("pad")) T_RUN(case_pad);
  if (wanted("save")) T_RUN(case_save);
  if (wanted("save-slow")) T_RUN(case_save_slow);
  if (wanted("pause")) T_RUN(case_pause);
  if (wanted("idle")) T_RUN(case_idle);
  if (wanted("sync")) T_RUN(case_sync);
  if (wanted("steady")) T_RUN(case_steady);
  if (wanted("ff-screens")) T_RUN(case_ff_screens);
  if (wanted("readout")) T_RUN(case_readout);
  fos_fini();
  return T_DONE();
}
