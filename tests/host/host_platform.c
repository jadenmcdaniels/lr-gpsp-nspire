/* The calculator-only pieces of gpSP's frontend, for gpsp-host: the ARM
 * assembly routines (the .S and .s files in nspire/) and the CPU clock (nspire_clock.c).
 * The scalers draw the same rows as notaz's ARM ones (nearest pixels instead
 * of blended ones), which is what the tests look at. */

extern unsigned host_hw_backlight(void);
#include "common.h"
#include "nspire_clock.h"
#include "nspire_hw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

uint32_t not_crt0_savedsp;

void enableAlignmentExceptions(void) {}
void clean_dcache(void) {}

void video_fill_u16_strip_arm(u16 *dst, u32 nh, u32 value)
{
  while (nh--)
    *dst++ = (u16)value;
}

/* src: 240x160 with 320 pixels a line (nspire_video_present.c's repack). */
static void scale(u16 *dst, const u16 *src, u32 top, u32 height)
{
  u32 x, y;
  for (y = 0; y < height; y++)
    for (x = 0; x < 320; x++)
      dst[(top + y) * 320 + x] = src[(y * 160 / height) * 320 + x * 240 / 320];
}

void upscale_aspect(u16 *dst, u16 *src)
{
  scale(dst, src, 13, 213);
}

void upscale_aspect_fast(u16 *dst, u16 *src)
{
  scale(dst, src, 0, 240);
}

void upscale_aspect_raw(u16 *dst, u16 *src)
{
  scale(dst, src, 13, 214);
}

/* ---- The CPU clock: there is none to change here ---- */

char nspire_clock_crashed_rom[512];

int nspire_clock_check_crash(void) { return 0; }
void nspire_clock_game(void) {}
void nspire_clock_normal(void) {}
void nspire_clock_left_game(void) {}

u32 nspire_cpu_mhz(void)
{
  return 0;
}

/* The PC's CX model times its first timer by the real-time clock only,
 * unless GPSP_HOST_CX_TIMER_HZ says what timing it by the CPU clock would
 * find. Another timer (any_rate) is timed as the calculator does: its
 * count over 0.2 s (of virtual time). */
void host_hw_spend(double seconds);

u32 nspire_cx_timer_hz(u32 value_reg, int any_rate)
{
  const char *hz = getenv("GPSP_HOST_CX_TIMER_HZ");
  u32 start, ticks;

  if (!any_rate)
    return hz ? (u32)strtoul(hz, NULL, 10) : 0;
  start = ~HW_READ(value_reg);
  host_hw_spend(0.2);
  ticks = ~HW_READ(value_reg) - start;
  return ticks * 5 >= 1000 && ticks * 5 <= 100000000 ? ticks * 5 : 0;
}

u32 nspire_cx_apb_hz(void)
{
  return 0;
}

/* R12: a CX II's timing of its first timer by the CPU clock: the model's
 * speed (host_hw.c host_hw_timer_hz; GPSP_HOST_CPU_TIMING_PPM: off by that). */
uint32_t host_hw_timer_hz(uint32_t value_reg);
extern int host_is_cx2;
u32 nspire_cx2_timer_hz(u32 value_reg, u32 top)
{
  (void)top;
  return host_is_cx2 ? host_hw_timer_hz(value_reg) : 0;
}

void nspire_clock_report(char *text, u32 size)
{
  snprintf(text, size, "clock: PC test build");
}

/* nspire_os.c on the PC: the OS's tick is every 10 ms of virtual time, and
 * a sleep passes the time to the next one (the calculator's TCC_Sleep(1)),
 * so the PC's frame rates count what sleeping costs. */
#include "nspire_os.h"

double host_hw_now(void);   /* virtual time, in seconds */
void host_hw_os_sleep_begin(void);
void host_hw_os_sleep_end(void);

/* LINK2: --os-tick MS (the CX CAS's measured 2.5 ms; the CX II's 10.25),
 * and --os-busy-fast MS: what each sleep costs on top while the fast
 * transport is up (the r6 logs' CX CAS: its sleeps then came back after
 * 50-82 ms, once 363; the CX II, never asked for one, unknown). */
double host_os_tick = 0.01;
double host_os_busy_fast;
#define HOST_OS_TICK host_os_tick
int link_cable_fast_up(void);

unsigned nspire_os_wanted;
static u32 host_sleeps;
static double host_last_wake;
/* PAD4: the OS's own time, in its ticks, as its tick handler counts them on
 * the calculator: gpSP runs with the interrupts masked, so the timer's
 * interrupts between two sleeps are one pending interrupt (one tick), and
 * the sleep itself is one more. A sleep right after a wake: 1; a sleep after
 * a tick or more of gpSP's own time: 2. host_usbpad.c's "pad slowhc ticks"
 * counts the OS's work in these. */
double host_os_ticks;

/* The time to the OS's next tick, in seconds. */
static double host_to_os_tick(void)
{
  double now = host_hw_now(), left = ((u64)(now / HOST_OS_TICK) + 1) * HOST_OS_TICK - now;

  /* now a hair below a tick: the float's rounding left nothing to spend and time stood still (S4's cycle looped on it) */
  return left < 1e-9 ? left + HOST_OS_TICK : left;
}

/* What nspire_os.c can
 * know of the tick, not the tick itself (the true one above is the sleeps'):
 *  - --os-timer 0|1 (1, the CX II's: nspire_os.c's probe passes): the tick's
 *    timer read, an SP804 of 21 counts a tick (load 0x14), the tick as the
 *    count reaches 0;
 *  - without it (r15's rule) the ticks foretold from the last wake, which
 *    comes --os-wake MS after the tick (and --os-jitter MS more at most, each
 *    wake its own): late by that, as on the calculator. 0 and 0 by default
 *    (the tests' timings as they were). */
int host_os_timer = 1;
double host_os_wake, host_os_jitter;
static u32 host_jitter_seed = 99991;
#define HOST_TT_LOAD 20u

static double host_jitter(void)
{
  host_jitter_seed = host_jitter_seed * 1103515245u + 12345u;
  return host_os_jitter * ((double)(host_jitter_seed >> 8 & 0xFFFF) / 65536.0);
}

/* the timer's count now: d counts to the tick (1..21; 21 reads 0) */
static u32 host_tt_count(void)
{
  double c = HOST_OS_TICK / (HOST_TT_LOAD + 1), left = host_to_os_tick();
  u32 d = (u32)ceil(left / c - 1e-9);

  if (d < 1)
    d = 1;
  if (d > HOST_TT_LOAD + 1)
    d = HOST_TT_LOAD + 1;
  return d;
}

/* The next tick as nspire_os.c foretells it (seconds from now), and whether known */
static double host_predicted_tick(int *known)
{
  double now = host_hw_now(), p = HOST_OS_TICK, since = now - host_last_wake;

  if (host_os_timer)
  {
    *known = 1;
    return (host_tt_count() - 0.5) * HOST_OS_TICK / (HOST_TT_LOAD + 1);
  }
  *known = since < 8 * p;
  return host_last_wake + (floor(since / p) + 1) * p - now;
}

int nspire_os_tick_timer(void)
{
  return host_os_timer;
}

/* the lines' inline check (nspire_lines.c os_watch): the host's count is
 * worked out on demand, so the word reads 1 (always near: the out-of-line
 * call checks by the bracket, as on the calculator) or never */
static const uint32_t host_tt_near = 1, host_tt_never = 0;
struct nspire_os_watch nspire_os_watch_at = { &host_tt_never, 0 };

void nspire_os_watch_arm(uint32_t counts)
{
  nspire_os_watch_at.reg = host_os_timer ? &host_tt_near : &host_tt_never;
  nspire_os_watch_at.value = host_os_timer && counts >= 2 ? counts : 0;
}

uint32_t nspire_os_tick_bracket(uint32_t *earliest, uint32_t *latest)
{
  double c = HOST_OS_TICK * NSPIRE_TICK_HZ / (HOST_TT_LOAD + 1);
  u32 d;

  if (!host_os_timer)
    return 0;
  d = host_tt_count();
  *earliest = (uint32_t)((d - 1) * c);
  *latest = (uint32_t)ceil(d * c);
  return d;
}

uint32_t nspire_os_tick_need(uint32_t need)
{
  u32 d;

  if (!host_os_timer)
    return 0;
  d = (u32)ceil(need / (HOST_OS_TICK * NSPIRE_TICK_HZ / (HOST_TT_LOAD + 1)) - 1e-9) + 1;
  return d <= HOST_TT_LOAD + 1 ? d : 0;
}

static u32 host_gap_longest;

uint32_t nspire_os_gap_longest(void)
{
  u32 g = host_gap_longest;

  host_gap_longest = 0;
  return g;
}

uint32_t nspire_os_wake_late(void)
{
  return host_os_timer ? (u32)((host_os_wake + host_os_jitter / 2) * NSPIRE_TICK_HZ) : 0;
}

void nspire_os_tick_rule_report(char *text, unsigned size)
{
  snprintf(text, size, "OS tick: %s (PC test build)",
           host_os_timer ? "its phase read from its timer" : "its phase from the last wake (r15's rule)");
}

int nspire_os_ready(void)
{
  return 1;
}

/* (r19-usb-exit) nspire_os.c's: the OS's sleep needs no look here */
uint32_t nspire_os_ready_cost(uint32_t *at)
{
  if (at)
    *at = 0;
  return 0;
}

/* (r19-usb-exit) nspire_os.c's hold: a sleep returns 0 at once, counted
 * (the report's os_sleeps_held) */
static int host_sleep_held;
static uint32_t host_sleeps_held;
void nspire_os_sleep_hold(int on)
{
  host_sleep_held = on;
}
uint32_t nspire_os_sleeps_held(void)
{
  return host_sleeps_held;
}

void host_link_os_ran(void);   /* host_link.c: --link-os's sends go */

/* R6: the OS's keys (nspire_os.h). A model of the calculator's OS: its
 * keypad interrupt (line 16 at the interrupt controller) is handled when
 * the OS runs (here: each OS sleep), and the OS queues an event for each
 * key that changed since it last looked; nobody takes them while gpSP
 * runs, and after exit the document browser would act on each. With
 * GPSP_HOST_OS_KEYS=poll the OS reads the keys at its ticks whatever the
 * interrupt (a path no calculator has shown: see test_os_keys_after_exit).
 * The report says what was left queued at exit (os_keys_left), how many
 * were queued in all, and the line's state.
 * EXIT-1: the hand-back (nspire_os_hand_back) is the last thing gpSP does:
 * the report says whether it ran, the line's state at the end, and
 * os_after_hand_back counts anything that let the OS run after it (an OS
 * sleep, a NavNet wait): it must be 0 (on the calculator the main task sat
 * in the OS after quitting: no key worked). There is no drain of the OS's
 * events any more (Ndless's get_event: see nspire_os.h). */
void host_hw_keypad(uint32_t words[4], int *on);
static int os_keypad_irq = 1, os_polls_keys = -1, os_pending, os_keypad_irq_start = -1;
static uint32_t os_known[4];
static int os_known_on;
static unsigned os_queue, os_queued, os_retaken;
static int handed_back, hand_backs;
static unsigned os_after_hand_back;

static void host_os_look_at(const uint32_t w[4], int on)
{
  int i;

  for (i = 0; i < 4; i++)
  {
    uint32_t changed = w[i] ^ os_known[i];
    for (; changed; changed &= changed - 1)
      os_queue++, os_queued++;
    os_known[i] = w[i];
  }
  if (on != os_known_on)
    os_queue++, os_queued++;
  os_known_on = on;
}

static void host_os_look_at_keys(void)
{
  uint32_t w[4];
  int on;

  host_hw_keypad(w, &on);
  host_os_look_at(w, on);
}

/* The OS ran: an OS sleep, or a NavNet write's wait (host_link.c). */
void host_os_runs(void)
{
  if (handed_back)
    os_after_hand_back++;
  if (os_polls_keys < 0)
  {
    const char *e = getenv("GPSP_HOST_OS_KEYS");
    os_polls_keys = e && !strcmp(e, "poll");
  }
  /* The interrupt's raised (and handled at once if the line's on). */
  if (os_keypad_irq || os_polls_keys)
    host_os_look_at_keys();
  else
    os_pending = 1;
}

static int os_taken;   /* gpSP turned the line off (nspire_os.c's keypad_taken) */
static unsigned os_line_turned_on;
/* The first-run hints (nspire_host_main.c) are off in the tests unless a
 * test asks (GPSP_HOST_HINTS=1): every test starts on a new calculator. */
int nspire_first_run_hints(void)
{
  const char *e = getenv("GPSP_HOST_HINTS");
  return e && !strcmp(e, "1");
}

int nspire_os_keypad_irq(int on)
{
  int was = os_keypad_irq;
  if (handed_back && !on)
    return was;
  os_taken = !on;
  os_keypad_irq = on;
  return was;
}

/* As nspire_os.c's: off again if the OS turned it on while gpSP has it. */
int nspire_os_keypad_keep_off(void)
{
  if (handed_back || !os_taken || !os_keypad_irq)
    return 0;
  os_keypad_irq = 0;
  os_retaken++;
  return 1;
}

/* EXIT-R6: the CPU takes interrupts (nspire_irqs_unmask: the pad glue's
 * stop() around the driver's shutdown): the OS's keypad handler runs if
 * its line is on, and queues what changed. */
void host_usbpad_irq_window(void) __attribute__((weak));   /* (2026-11-17) host_usbpad.c: the USB controller's interrupt too */
void host_os_irq_window(void)
{
  if (os_keypad_irq && !handed_back)
    host_os_look_at_keys();
  if (host_usbpad_irq_window)
    host_usbpad_irq_window();
}

/* EXIT-R6: GPSP_HOST_OS_LINE_ON=stop: the model's OS turns its keypad line
 * on in an OS slice that nothing checks after (here the link's shutdown at
 * quit, as NavNet's StopService was before EXIT-R5): what gpSP does next
 * must turn it off before it takes interrupts. */
void host_os_slice_at_stop(void)
{
  const char *e = getenv("GPSP_HOST_OS_LINE_ON");
  if (e && !strcmp(e, "stop") && !handed_back)
  {
    host_os_runs();
    os_keypad_irq = 1;
    os_line_turned_on++;
  }
}

uint32_t nspire_os_keypad_retaken(void)
{
  return os_retaken;
}

void nspire_os_hw_note_start(void)
{
  if (os_keypad_irq_start < 0)
    os_keypad_irq_start = os_keypad_irq;
}

void nspire_os_after_os_time(void)
{
}

uint32_t nspire_os_cursor_offs(void)
{
  return 0;
}

/* nspire_os.c's wording (EXIT-R3: its own code is tested in os_tests.c),
 * with the model's keypad line in the IRQ enable word and the other words
 * zero. */
static int hw_words(char *text, unsigned size, const char *name, int line)
{
  return snprintf(text, size, "%s: irq %08x (keypad line %s), keypad 00000000 00000000 status 00000000 enable 00000000, "
                  "gpio enable 00000000 status 00000000, cursor 00000000 xy 00000000", name,
                  0x00080000u | (line ? 0x10000u : 0), line ? "on" : "off");
}

void nspire_os_hand_back_text(char *text, unsigned size)
{
  int n = hw_words(text, size, "hardware at start", os_keypad_irq_start > 0);
  if (n > 0 && (unsigned)n < size)
    n += snprintf(text + n, size - n, "\n");
  if (n > 0 && (unsigned)n < size)
    n += hw_words(text + n, size - n, "hardware at exit, before the hand-back", os_keypad_irq);
  if (n > 0 && (unsigned)n < size)
    snprintf(text + n, size - n, "\nhand-back writes: keypad line on%s; the OS turned the keypad line on %u times, its "
             "busy cursor 0 times (turned off), moved the cursor back on the panel 0 times (not parked: a CX), %u "
             "sleeps refused after", os_keypad_irq_start > 0 ? "" : " (it was off at start: on all the same)",
             os_retaken, os_after_hand_back);
}

void nspire_os_after_hand_back_text(char *text, unsigned size)
{
  hw_words(text, size, "after the hand-back", os_keypad_irq);
}

/* QUIT6: nspire_os.c's wording, with the model's line 18 (the first timer's
 * interrupt AND its enable: host_hw.c), the OS's tick (19), its keypad (16)
 * and, on a CX II, line 18 enabled as every CX II OS keeps it. */
void nspire_os_irq_text(char *text, unsigned size, const char *when)
{
  uint32_t raw = host_hw_read(0x900C0010u) ? 1u << 18 : 0, masked = host_hw_read(0x900C0014u) ? 1u << 18 : 0;
  uint32_t enabled = 0x00080000u | (os_keypad_irq ? 0x10000u : 0) | (host_is_cx2 ? 1u << 18 : 0);

  masked &= enabled;
  snprintf(text, size, "interrupt lines %s: raised and enabled %08x%s, raised %08x, enabled %08x", when,
           (unsigned)masked, masked ? " (18: the 900c0000 timer block's)" : "", (unsigned)raw, (unsigned)enabled);
}

void nspire_os_hand_back(void)
{
  handed_back = 1;
  hand_backs++;
  os_keypad_irq = 1;
}

int nspire_os_handed_back(void)
{
  return handed_back;
}

/* At the report (after gpSP's exit): the OS gets the CPU back, and a
 * keypad interrupt raised while the line was off is handled now. */
void host_os_keys_report(FILE *f)
{
  /* Ndless, once gpSP has returned, waits with the CPU's interrupts off
   * until no key is down (ploaderhook.c wait_no_key_pressed), then they're
   * on: the OS's handler first sees the keys all up. */
  static const uint32_t up[4];
  if (os_keypad_irq && os_pending)
    host_os_look_at(up, 0);
  fprintf(f, "os_keys_left %u\n", os_queue);
  fprintf(f, "os_keys_queued %u\n", os_queued);
  fprintf(f, "os_keypad_irq_end %s\n", os_keypad_irq ? "on" : "off");
  fprintf(f, "hand_backs %d\n", hand_backs);
  fprintf(f, "os_keypad_retaken %u\n", os_retaken);
  fprintf(f, "os_line_turned_on %u\n", os_line_turned_on);
  fprintf(f, "os_after_hand_back %u\n", os_after_hand_back);
  fprintf(f, "os_sleeps_held %u\n", (unsigned)host_sleeps_held);
}

/* P2: the OS's time outside nspire_wait_until's idle wait (nspire_os_idle). */
int nspire_os_idle;
static double host_busy_slept;
static u32 host_busy_sleeps;

void (*nspire_os_wake_hook)(void);

void (*nspire_os_sleep_guard)(void);

int nspire_os_sleep(void)
{
  double left;

  if (nspire_os_sleep_guard)
    nspire_os_sleep_guard();   /* (r19-usb: as nspire_os.c's) */
  if (handed_back)
  {
    os_after_hand_back++;   /* refused, as on the calculator: and counted */
    return 0;
  }
  if (host_sleep_held)
  {
    host_sleeps_held++;
    return 0;
  }
  if ((host_hw_now() - host_last_wake) * NSPIRE_TICK_HZ > host_gap_longest)
    host_gap_longest = (u32)((host_hw_now() - host_last_wake) * NSPIRE_TICK_HZ);
  left = host_to_os_tick() + host_os_wake + host_jitter();
  host_os_ticks += 1 + ((u64)(host_hw_now() / HOST_OS_TICK) > (u64)(host_last_wake / HOST_OS_TICK) ? 1 : 0);
  if (host_os_busy_fast > 0 && link_cable_fast_up())
    left += host_os_busy_fast;
  host_os_runs();
  /* R12: the OS's USB ticks as it takes them (host_hw.c: its timer's
   * interrupts, or its look at the first timer) at the sleep's start and end */
  host_hw_os_sleep_begin();
  host_hw_spend(left);
  host_hw_os_sleep_end();
  host_last_wake = host_hw_now();
  host_sleeps++;
  if (!nspire_os_idle)
  {
    host_busy_slept += left;
    host_busy_sleeps++;
  }
  host_link_os_ran();
  if (nspire_os_wake_hook)
    nspire_os_wake_hook();     /* (nspire_os.c's) */
  return 1;
}

/* nspire_os.c's: only if the tick's phase is known (a wake in the last 8
 * ticks) and the tick at most max_wait away (0: 33 timer ticks, 1 ms). */
int nspire_os_sleep_near(uint32_t max_wait)
{
  int known;
  double next;

  if (!max_wait)
    max_wait = 33;
  next = host_predicted_tick(&known);
  if (!known || next * NSPIRE_TICK_HZ > max_wait)
    return 0;
  return nspire_os_sleep();
}

uint32_t nspire_os_near_wait(uint32_t since, uint32_t from, uint32_t until)
{
  uint32_t w = 33;

  if (until > from && since > from)
    w += (uint32_t)((u64)(HOST_OS_TICK * NSPIRE_TICK_HZ / 2) * ((since > until ? until : since) - from) / (until - from));
  return w;
}

uint32_t nspire_os_busy_slept(uint32_t *count)
{
  if (count)
    *count = host_busy_sleeps;
  return (u32)(host_busy_slept * NSPIRE_TICK_HZ);
}

int nspire_os_sleep_before(uint32_t deadline)
{
  int known;
  double t = host_predicted_tick(&known);
  u32 next = nspire_ticks() + (u32)((known ? (t > 0 ? t : 0) : HOST_OS_TICK) * NSPIRE_TICK_HZ);
  if ((s32)(deadline - next) < 66)
    return 0;
  return nspire_os_sleep();
}

uint32_t nspire_os_since_sleep(void)
{
  return (u32)((host_hw_now() - host_last_wake) * NSPIRE_TICK_HZ);
}

/* nspire_os.c's (2026-11-17, final/speed8: nspire_frameskip.c takes a
 * pad's tick as it comes): the next tick's distance, known 8 ticks after a
 * wake. */
int nspire_os_next_tick_in(uint32_t *ticks, uint32_t *period)
{
  int known;
  double t = host_predicted_tick(&known);

  *period = (uint32_t)(HOST_OS_TICK * NSPIRE_TICK_HZ + 0.5);
  *ticks = known ? (uint32_t)((t > 0 ? t : 0) * NSPIRE_TICK_HZ + 0.5) : *period;
  return known;
}

void nspire_os_report(char *text, unsigned size)
{
  snprintf(text, size, "OS sleep: PC test build, %lu sleeps", (unsigned long)host_sleeps);
}

uint32_t nspire_os_sleeps(void)
{
  return host_sleeps;
}

uint32_t nspire_os_long_sleeps;   /* (nspire_pictures.c: none here) */

uint32_t nspire_os_sleep_longest(void)
{
  return 0;
}

void nspire_os_diag_watch(char *text, unsigned size)
{
  snprintf(text, size, "wd: PC test build");
}

int nspire_os_watchdog_off(void)
{
  return 0;
}

uint32_t nspire_os_watchdog_offs(void)
{
  return 0;
}

/* R11w: the exit's backstop (counted: the report says whether it was off
 * again at the end) and the restart (host_hw.c: the report, then the
 * process ends as a reset would end it, no atexit). */
int host_os_backstop_arms, host_os_backstop_on;
uint32_t nspire_os_backstop_arm(uint32_t ms)
{
  (void)ms;
  host_os_backstop_arms++;
  host_os_backstop_on = 1;
  return 12000;
}

void nspire_os_backstop_disarm(void)
{
  host_os_backstop_on = 0;
}

int nspire_os_backstop_armed(void)
{
  return host_os_backstop_on;
}

void nspire_os_restart(void)
{
  void host_hw_restart(void);
  host_hw_restart();
}

/* The link log's diagnostics: no calculator's registers here (the USB
 * port is host_link.c's). */
void nspire_os_note_start(void) {}
void nspire_os_diag_system(char *text, unsigned size)
{
  snprintf(text, size, "calculator: PC test build");
}

void nspire_os_diag_usb(char *text, unsigned size)
{
  snprintf(text, size, "USB: PC socket");
}

void nspire_os_diag_screen(char *text, unsigned size)
{
  snprintf(text, size, "LCD: PC test build, backlight %03x", (unsigned)host_hw_backlight());
}
