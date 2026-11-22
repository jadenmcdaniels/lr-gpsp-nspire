/* Letting the calculator's OS run its own tasks: nspire_os.h.
 *
 * TCC_Sleep is Nucleus's: the calling task leaves the run queue for some
 * OS ticks and the OS runs everything else meanwhile. Ndless has no
 * syscall for it, so it's found by its code: 16 instructions (64 bytes)
 * starting push {r4, lr}; sub sp, sp, #8; mov r4, r0, whose FNV-1a hash,
 * with a branch out of the function or a BL kept to its condition and
 * opcode (the relocated part), is SLEEP_HASH; its literal at +0x18 is the
 * address of Nucleus's current-task pointer. These facts are ND Screen
 * Mirror's (GigaZelensky/Nspire-ScreenMirror-Ndless, vendor/ndvideo
 * native_usb_ui: "IRQs alone do not let lower-priority USB tasks run. The
 * app must leave the runnable queue briefly", on CX II OS 6.2 and the
 * original CX), the code here is gpSP's. It's used only if exactly one
 * place in the OS's code matches and the current task looks like a
 * Nucleus task ("TASK" at +0x0c, nothing at +0x38, as ND Screen Mirror
 * checks before each sleep).
 *
 * The OS's tick is timed at the start and kept up to date from the sleeps'
 * wakes (in 1/256 timer ticks, a running average). Where the next one comes
 * (2026-11-17, final/speed9a): read from the tick's own
 * timer when the start's probe found it to be one it can read (the SP804
 * at 0x900D0000: its count down to the tick, a count 0.49 ms on a CX II);
 * else, r15's rule, from the last wake (which comes the OS's own time after
 * the tick, so it is late by that: known for 8 ticks after a wake, none
 * after): a sleep before a deadline is only made when that tick comes in
 * time, and where the ticks fall isn't known, only when a whole tick fits.
 *
 * The diagnostics (nspire_os_diag_*, nspire_usb_*) only read: Ndless's OS
 * id word (0x10000020, resources/utils.c), the model word at 0x900A0000
 * (0x202 on a CX II, ND Screen Mirror), the USB controller's registers (a
 * CX II's FOTG210: its layout in ND Screen Mirror's usb_registers.h and
 * nnse_abort.h, after U-Boot's fotg210.h; a CX's ChipIdea controller: ND
 * Screen Mirror's usb_registers.h and Hackspire's CX page, the bits the
 * standard ChipIdea OTGSC and PORTSC ones), the PL111 LCD's, the interrupt
 * controller's mask and the timers' control words (what ND Screen Mirror
 * logs).
 *
 * One write (R3): the SP805 watchdog is turned off again (as Ndless does
 * when it starts a program: resources/ploaderhook.c's ut_disable_watchdog,
 * "seems to be sometimes reenabled by the OS") after each OS sleep and at
 * each linked frame, if the OS turned it on meanwhile. */

#include <os.h>
#include <libndls.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "nspire.h"
#include "nspire_os.h"
#include "nspire_link.h"

#define OS_CODE_START 0x10000000u
#define OS_CODE_END   0x11000000u
#define SLEEP_WORDS   16
#define SLEEP_HASH    0x13e2fde1u
#define TASK_MAGIC    0x5441534bu   /* "TASK" */
/* A sleep before a deadline only if the OS's tick comes this long before it
 * (timer ticks: 2 ms), for the tasks it wakes to run and gpSP's to come back. */
#define SLEEP_ROOM    66
/* The last wake says where the OS's ticks fall for this many ticks after it
 * (the tick's length is known to a fraction of a timer tick, and the wake
 * comes a little after the OS's tick). */
#define PHASE_TICKS   8
/* The OS's tick: the first timer at 0x900D0000 (SP804: its control word,
 * enable and interrupt enable) and its interrupt at the controller. */
#define IRQ_ENABLE      0xDC000010u
#define OS_TICK_IRQ     (1u << 19)
#define OS_TICK_CONTROL 0x900D0008u
#define OS_TICK_ON      0xA0u
/* (speed9a) the same timer's load, count and raw interrupt (SP804) */
#define OS_TICK_LOAD    0x900D0000u
#define OS_TICK_VALUE   0x900D0004u
#define OS_TICK_RAW     0x900D0010u
#define OS_TICK_PERIODIC 0x40u
#define LCD_CURSOR_XY  0xC0000C10u   /* PL111 ClcdCrsrXY: X from bit 0, Y from bit 16 */
/* EXIT-R5: where the CX II's cursor waits while gpSP runs: past the
 * panel's 240 pixels a line and 320 lines on both axes, whatever the
 * cursor's size (32 or 64). 2026-11-11: 448 (0x1C0), not 1023 (0x3FF):
 * a 64-pixel cursor at 1023 reaches 1086, and a controller that compares
 * positions in 10 bits (the most a PL111 panel needs) would wrap it onto
 * the panel's first lines and pixels; 448 + 63 = 511 needs only 9. Not
 * known to have shown (the CX II's flashing top row was the text band,
 * nspire_blend.c). */
#define CURSOR_PARKED  0x01C001C0u

unsigned nspire_os_wanted;
uint32_t nspire_os_task_var;

static int state;   /* 0: not looked for; 1: found; -1: not */
static const char *why = "not looked for";
static u32 sleep_at, task_var, matches;
static void (*tcc_sleep)(unsigned ticks);
/* The OS's tick in 1/256 timer ticks, and when a sleep last came back (just
 * past one of its ticks). */
static u32 os_tick256 = (NSPIRE_TICK_HZ / 100) << 8, last_wake, sleeps;
u32 nspire_os_long_sleeps;   /* (below: nspire_os_sleep) */
/* P2: timer ticks slept in all (each sleep's wait, its call to its return),
 * and those inside nspire_wait_until's idle wait (nspire_os_idle set): the
 * rest is what the OS took from a game with no spare time. */
static u32 slept_ticks, idle_slept_ticks, idle_sleeps;
int nspire_os_idle;
/* R3: the longest sleep since nspire_os_sleep_longest last asked (ticks). */
static u32 sleep_longest;
/* R3: times the watchdog was found on and turned off. */
static u32 watchdog_offs;
/* R6: gpSP has the OS's keypad interrupt off; times something turned it on. */
static int keypad_taken;
static u32 keypad_retaken;
/* EXIT-1: after nspire_os_hand_back the OS has its keys and screen again;
 * nothing of gpSP's sleeps into it or masks anything any more. EXIT-2: the
 * OS's busy cursor found on and turned off. */
static int handed_back;
static u32 cursor_offs, sleeps_refused;
/* EXIT-R5: the CX II's cursor put off the panel at start; times the OS
 * moved it back on (found away from there after its time). */
static int cursor_parked;
static u32 cursor_moves;
/* (speed9a) The tick's phase from its timer: 1 when the start's probe
 * (tick_probe) found the timer counting down to the tick as an SP804 does,
 * at a speed that makes the tick the wakes measured; then tt_load and
 * tt_control are what it had (checked at each read: a CPU speed switch
 * borrows the timer, nspire_clock.c), tt_count256 a count (1/256 timer
 * ticks), tt_at_zero 1 if the tick comes as the count reaches 0 (else as
 * it reloads after 0). tt_why: why not, for the logs. */
static int tt_rule;
static const char *tt_why = "not probed";
static u32 tt_load, tt_control, tt_count256, tt_at_zero, tt_probed;
/* (speed9a) the longest from a wake to the next sleep since last asked */
static u32 gap_longest;
/* (speed9a) with the timer read: how late a wake comes after its tick (the
 * OS's own time then), 1/256 timer ticks, a running average (an eighth) */
static u32 wake_late256;
/* the lines' check (nspire_os_watch_arm) */
static const uint32_t watch_never = 0;
struct nspire_os_watch nspire_os_watch_at = { &watch_never, 0 };

#ifdef NSPIRE_OS_FAKE_REGS
/* EXIT-R3: tests/host/os_tests.c builds this file against a register file
 * of its own: every read and write of a hardware word goes through these. */
u32 os_test_reg_read(u32 at);
void os_test_reg_write(u32 at, u32 value);
volatile const uint32_t *os_test_reg_ptr(u32 at);
static u32 word(u32 at)
{
  return os_test_reg_read(at);
}
static void put(u32 at, u32 value)
{
  os_test_reg_write(at, value);
}
#else
static u32 word(u32 at)
{
  return *(volatile u32 *)at;
}
static void put(u32 at, u32 value)
{
  *(volatile u32 *)at = value;
}
#endif

static int sleep_body(u32 at)
{
  u32 hash = 2166136261u, i;

  for (i = 0; i < SLEEP_WORDS; i++)
  {
    u32 op = word(at + i * 4);
    if ((op & 0x0e000000u) == 0x0a000000u)
    {
      u32 target = at + i * 4 + 8 + (u32)((s32)(op << 8) >> 6);
      if ((op & 0x01000000u) || target < at || target >= at + SLEEP_WORDS * 4)
        op &= 0xff000000u;
    }
    hash = (hash ^ op) * 16777619u;
  }
  return hash == SLEEP_HASH;
}

/* What an LDR rX, [pc, #n] at pc loads (0 if it isn't one). */
static u32 pc_literal(u32 pc)
{
  u32 op = word(pc);
  if ((op & 0xffff0000u) == 0xe59f0000u)
    return word(pc + 8 + (op & 0xfff));
  if ((op & 0xffff0000u) == 0xe51f0000u)
    return word(pc + 8 - (op & 0xfff));
  return 0;
}

static int in_ram(u32 at, u32 bytes)
{
  return !(at & 3) && at >= 0x10000000u && at < 0x14000000u && 0x14000000u - at >= bytes;
}

static int is_task(u32 task)
{
  return in_ram(task, 0x44) && word(task + 0x0c) == TASK_MAGIC;
}

static int task_ok(void)
{
  u32 task;
  if (!in_ram(task_var, 4))
    return 0;
  task = word(task_var);
  return is_task(task) && word(task + 0x38) == 0;
}

/* One OS tick's sleep, with the interrupts on for it (the OS's tick wakes
 * the task, and its tasks run on them), then as they were. gpSP runs with
 * the IRQ mask set (both calculators' logs, 2026-11-08: "interrupts are
 * off", so no sleep ever happened and the link never came up); ND Screen
 * Mirror too turns them on just around its sleep (native_critical_leave(0)
 * before TCC_Sleep(1)). */
static void sleep_tick(void)
{
  u32 cpsr;

  /* EXIT-1: after the hand-back nothing of gpSP's lets the OS run (Ndless
   * waits for the keys with interrupts off, then the OS has them). */
  if (handed_back)
  {
    sleeps_refused++;
    return;
  }
  nspire_os_keypad_keep_off();   /* R6: the OS never gets gpSP's keys */
  cpsr = nspire_irqs_unmask();
  tcc_sleep(1);
  nspire_irqs_restore(cpsr);
  nspire_os_after_os_time();   /* EXIT-2: its busy cursor off again */
}

/* (r19-usb-exit) The first look's own time: the scan of 16 MB of the OS's
 * code for TCC_Sleep, three sleeps and the tick's probe. It comes at the
 * run's first OS sleep, wherever that is: in a run that never slept (the
 * list and the quit, a game with nothing plugged in) the quit's first USB
 * shutdown paid it (the CX's "USB audio shut down 669", of which 10 sleeps
 * are ~100 ms). The exit's steps say it when it falls in one. */
static u32 ready_cost, ready_at;
static int os_ready_find(void);

int nspire_os_ready(void)
{
  u32 t0;
  int r;

  if (state)
    return state > 0;
  t0 = nspire_ticks();
  r = os_ready_find();
  ready_at = nspire_ticks();
  ready_cost = ready_at - t0;
  return r;
}

uint32_t nspire_os_ready_cost(uint32_t *at)
{
  if (at)
    *at = ready_at;
  return ready_cost;
}

static int os_ready_find(void)
{
  u32 at, t0;

  state = -1;
  for (at = OS_CODE_START; at < OS_CODE_END; at += 4)
    if (word(at) == 0xe92d4010u && word(at + 4) == 0xe24dd008u && word(at + 8) == 0xe1a04000u &&
        sleep_body(at))
    {
      if (!matches++)
        sleep_at = at;
    }
  if (matches != 1)
  {
    why = matches ? "TCC_Sleep's code is in more than one place" : "TCC_Sleep's code isn't in this OS";
    return 0;
  }
  task_var = pc_literal(sleep_at + 0x18);
  if (!task_ok())
  {
    why = "the current task doesn't look like a Nucleus task";
    return 0;
  }
  /* The OS's tick, which wakes a sleep: its timer (the first one at
   * 0x900D0000: enabled, its interrupt on) and its interrupt at the
   * controller (19). Both calculators' logs have them (IRQ enable 080d0186
   * and 00090182, timer control e6); without them a sleep would never
   * come back, and the three below have no time limit. */
  if (!(word(IRQ_ENABLE) & OS_TICK_IRQ) || (word(OS_TICK_CONTROL) & OS_TICK_ON) != OS_TICK_ON)
  {
    why = "the OS's tick is off (its timer or its interrupt)";
    return 0;
  }
  tcc_sleep = (void (*)(unsigned))(uintptr_t)sleep_at;
  nspire_os_task_var = task_var;
  state = 1;
  why = "found";
  /* The OS's tick: from just past one, two more. */
  sleep_tick();
  t0 = nspire_ticks();
  sleep_tick();
  sleep_tick();
  last_wake = nspire_ticks();
  if (last_wake - t0 >= 2 * (NSPIRE_TICK_HZ / 1000) && last_wake - t0 <= 2 * (NSPIRE_TICK_HZ / 10))
    os_tick256 = (last_wake - t0) << 7;   /* half of two ticks, in 1/256 */
  sleeps = 3;
  (void)nspire_os_tick_probe();
  return 1;
}

/* ---- The tick's phase from its timer (2026-11-17, final/speed9a) ----
 * r15's rule took where the ticks fall from the last wake. A wake comes the
 * OS's own time after the tick (its handlers, the tasks it wakes first), so
 * every tick foretold that way was late by that time (1-2.5 ms, the
 * models'), and the frame loop's watch (nspire_frameskip.c), which sleeps
 * just before the tick, slept just after it: a whole tick's wait. The tick's
 * timer says it exactly: an SP804 counting down to the tick (the CX II's
 * logs: 0x900D0000 load 0x14, control e6: on, periodic, its interrupt on,
 * the clock divided by 16: 21 counts of 0.49 ms, 10.25 ms). Read only.
 *
 * The probe, once, after the start's sleeps (just after a wake, the OS
 * having taken its tick: its raw interrupt clear), with the interrupts off
 * as gpSP's frames have them: the timer must be on, periodic and
 * interrupting, its load 3 to 0xFFFF and its count not above it; then the
 * count is watched until the tick's raw interrupt comes (1.5 ticks at
 * most): the count must step down one at a time, 4 steps at least, at a
 * steady speed (each step within an eighth of their mean), and (load + 1)
 * steps must make the tick the wakes measured (within 1/16); the count seen
 * as the interrupt came says whether the tick comes as the count reaches 0
 * or as it reloads; and the interrupt must come where the first count said
 * (within a count). Anything else: r15's rule, and the logs say why. The
 * CX's OS 4.5 (a 2.98 ms tick) goes by the same probe: whatever its timer
 * is, it is read only if it passes. */
#define TT_PROBE_READS 200000u

int nspire_os_tick_probe(void)
{
  u32 load, control, v, last, t, t_first = 0, t_edge0 = 0, t_edge = 0, edges = 0;
  u32 v_first, t_start, limit, i, irq_v = 0, before_irq = 0, tick_at = 0;
  u32 step_min = 0xFFFFFFFFu, step_max = 0;
  int at_zero;

  tt_rule = 0;
  tt_probed = 1;
  nspire_os_watch_at.reg = &watch_never;
  nspire_os_watch_at.value = 0;
  control = word(OS_TICK_CONTROL);
  load = word(OS_TICK_LOAD);
  if ((control & (OS_TICK_ON | OS_TICK_PERIODIC)) != (OS_TICK_ON | OS_TICK_PERIODIC))
  {
    tt_why = "its control isn't on, periodic and interrupting";
    return 0;
  }
  if (load < 3 || load > 0xFFFF)
  {
    tt_why = "its load is out of range";
    return 0;
  }
  if (word(OS_TICK_RAW) & 1)
  {
    tt_why = "its interrupt was raised at the start";
    return 0;
  }
  t_start = nspire_ticks();
  limit = (os_tick256 * 3 / 2) >> 8;
  v_first = last = word(OS_TICK_VALUE);
  t_first = t_start;
  if (v_first > load)
  {
    tt_why = "its count is above its load";
    return 0;
  }
  for (i = 0; i < TT_PROBE_READS; i++)
  {
    u32 raw = word(OS_TICK_RAW) & 1;
    v = word(OS_TICK_VALUE);
    t = nspire_ticks();
    if (raw)
    {
      irq_v = v;
      before_irq = last;
      tick_at = t;
      break;
    }
    if (v != last)
    {
      if (v != last - 1 && !(last == 0 && v == load))
      {
        tt_why = "its count doesn't step down by one";
        return 0;
      }
      if (v != load)                      /* (a step: the reload's isn't timed) */
      {
        if (edges)
        {
          u32 d = t - t_edge;
          if (d < step_min) step_min = d;
          if (d > step_max) step_max = d;
        }
        else
          t_edge0 = t;
        t_edge = t;
        edges++;
      }
      last = v;
    }
    if (t - t_start > limit)
      break;
  }
  if (!tick_at)
  {
    tt_why = "its interrupt didn't come within 1.5 ticks";
    return 0;
  }
  if (edges < 5)
  {
    tt_why = "too few of its counts seen before the tick";
    return 0;
  }
  {
    /* the mean step (1/256 timer ticks), its spread, the tick it makes */
    u32 mean256 = (u32)(((u64)(t_edge - t_edge0) << 8) / (edges - 1));
    u32 tick256 = mean256 * (load + 1), lo = os_tick256 - os_tick256 / 16, hi = os_tick256 + os_tick256 / 16;
    if (((u64)step_max << 8) > (u64)mean256 + mean256 / 8 + 256 ||
        ((u64)step_min << 8) + mean256 / 8 + 256 < (u64)mean256)
    {
      tt_why = "its counts' steps aren't steady";
      return 0;
    }
    if (tick256 < lo || tick256 > hi)
    {
      tt_why = "load + 1 of its counts isn't the tick the wakes measured";
      return 0;
    }
    /* how the tick comes: as the count reaches 0 (0 seen with the interrupt
     * raised), or as it reloads (0 seen before it) */
    if (irq_v == 0 && before_irq == 1)
      at_zero = 1;
    else if (before_irq == 0 && irq_v == load)
      at_zero = 0;
    else
    {
      tt_why = "the count where its interrupt came is neither 0 nor the reload";
      return 0;
    }
    /* the interrupt where the first count said: within its bracket, a count of slack */
    {
      u32 d = at_zero ? v_first : v_first + 1;
      u64 earliest = (u64)(d - 1) * mean256, latest = (u64)d * mean256;
      u64 seen = (u64)(tick_at - t_first) << 8;
      if (seen + mean256 < earliest || seen > latest + 2 * mean256)
      {
        tt_why = "its interrupt didn't come where its count said";
        return 0;
      }
    }
    tt_load = load;
    tt_control = control;
    tt_count256 = mean256;
    tt_at_zero = (u32)at_zero;
    os_tick256 = tick256;
    tt_rule = 1;
    tt_why = "read from its timer";
#ifdef NSPIRE_OS_FAKE_REGS
    nspire_os_watch_at.reg = os_test_reg_ptr(OS_TICK_VALUE);
#else
    nspire_os_watch_at.reg = (volatile const uint32_t *)OS_TICK_VALUE;
#endif
  }
  return 1;
}

/* (speed9a) Counts until the tick by the timer, or 0 if it can't be read
 * now (not the timer's rule, or the timer isn't as the probe found it). The
 * tick comes within ((d - 1) c, d c] of now, c a count. */
static u32 tt_counts(void)
{
  u32 v;
  if (!tt_rule || word(OS_TICK_CONTROL) != tt_control || word(OS_TICK_LOAD) != tt_load)
    return 0;
  v = word(OS_TICK_VALUE);
  if (v > tt_load)
    return 0;
  if (!tt_at_zero)
    return v + 1;
  return v ? v : tt_load + 1;
}

int nspire_os_tick_timer(void)
{
  return tt_rule;
}

/* The tick's distance by the timer: the latest it can be (timer ticks,
 * rounded up) in *latest and the earliest in *earliest; the counts to it
 * (d: within ((d - 1) c, d c]) if read, else 0. */
uint32_t nspire_os_tick_bracket(uint32_t *earliest, uint32_t *latest)
{
  u32 d = tt_counts();
  if (!d)
    return 0;
  *earliest = (u32)(((u64)(d - 1) * tt_count256) >> 8);
  *latest = (u32)(((u64)d * tt_count256 + 255) >> 8);
  return d;
}

/* The counts to the tick at which it is at least 'need' timer ticks away
 * and less than a count more: ceil(need / c) + 1 (the tick within
 * ((d - 1) c, d c]); 0 if the timer isn't read or a tick isn't that long. */
uint32_t nspire_os_tick_need(uint32_t need)
{
  u32 d;
  if (!tt_rule || !tt_count256)
    return 0;
  d = (u32)((((u64)need << 8) + tt_count256 - 1) / tt_count256) + 1;
  return d <= tt_load + 1 ? d : 0;
}

/* The lines' check (nspire_lines.c os_watch, while nspire_os_watching):
 * a line calls nspire_os_watch_line only when *reg - 1 < value, reg the
 * tick timer's count (with the timer's rule; else a word that never is).
 * The tick as the count reaches 0: counts 1..d are raw 1..d (raw 0, just
 * after the tick, isn't); as it reloads: counts 2..d are raw 1..d - 1. */

void nspire_os_watch_arm(uint32_t counts)
{
  if (!tt_rule || counts < 2)
  {
    nspire_os_watch_at.value = 0;
    return;
  }
  nspire_os_watch_at.value = tt_at_zero ? counts : counts - 1;
}

void nspire_os_tick_rule_report(char *text, unsigned size)
{
  if (!tt_probed)
    snprintf(text, size, "OS tick: not probed (no OS sleep yet); its phase from the last wake (r15's rule)");
  else if (tt_rule)
    snprintf(text, size, "OS tick: its phase read from its timer (900d0000: load %lu, control %02lx, a count %lu us, "
             "the tick as the count %s; %lu us a tick); a wake %lu us after its tick (the OS's own time, averaged)",
             (unsigned long)tt_load, (unsigned long)tt_control,
             (unsigned long)((u64)tt_count256 * 1000000 / 256 / NSPIRE_TICK_HZ),
             tt_at_zero ? "reaches 0" : "reloads", (unsigned long)((u64)os_tick256 * 1000000 / 256 / NSPIRE_TICK_HZ),
             (unsigned long)((u64)wake_late256 * 1000000 / 256 / NSPIRE_TICK_HZ));
  else
    snprintf(text, size, "OS tick: its phase from the last wake (r15's rule): the timer not read (%s; 900d0000: load "
             "%lx, value %lx, control %lx)", tt_why, (unsigned long)word(OS_TICK_LOAD),
             (unsigned long)word(OS_TICK_VALUE), (unsigned long)word(OS_TICK_CONTROL));
}

#ifdef NSPIRE_OS_FAKE_REGS
/* tests/host/os_tests.c: the tick as the start's sleeps would have timed it */
void nspire_os_test_tick(u32 tick256)
{
  os_tick256 = tick256;
}
#endif

uint32_t nspire_os_wake_late(void)
{
  return (wake_late256 + 128) >> 8;
}

uint32_t nspire_os_gap_longest(void)
{
  u32 g = gap_longest;
  gap_longest = 0;
  return g;
}

void (*nspire_os_wake_hook)(void);

/* (r19-usb-exit) A quit with nothing ever on the port's host end: the USB
 * drive driver's shutdown waits 10 OS ticks for callback epilogues of a
 * driver no callback ever entered (~100 ms on either calculator). While
 * held, a sleep returns 0 at once (counted): the caller's loops then end
 * (each is bounded by its count) without the OS's time. */
static int sleep_held;
static u32 sleeps_held;

void nspire_os_sleep_hold(int on)
{
  sleep_held = on;
}

uint32_t nspire_os_sleeps_held(void)
{
  return sleeps_held;
}

void (*nspire_os_sleep_guard)(void);

int nspire_os_sleep(void)
{
  u32 before = last_wake, since, n;

  if (nspire_os_sleep_guard)
    nspire_os_sleep_guard();   /* (r19-usb: the exit's USB shutdowns' deadline, before anything) */
  if (handed_back)
  {
    sleeps_refused++;
    return 0;
  }
  if (sleep_held)
  {
    sleeps_held++;
    return 0;
  }
  if (!nspire_os_ready() || !task_ok())
    return 0;
  uint32_t earliest, latest;
  int timed;

  n = nspire_ticks();
  if (n - before > gap_longest)
    gap_longest = n - before;      /* (speed9a: here, not at the key read) */
  timed = nspire_os_tick_bracket(&earliest, &latest);
  sleep_tick();
  last_wake = nspire_ticks();
  if (timed)
  {
    /* the tick it waited for: the middle of the count's bracket */
    u32 tick = n + (earliest + latest) / 2, late = last_wake - tick;
    if ((s32)late >= 0 && late < (os_tick256 >> 9))
      wake_late256 = wake_late256 ? wake_late256 - wake_late256 / 8 + (late << 5) : late << 8;
  }
  /* R3: the CX II resets as if by its reset button only while linked, when
   * the OS runs: a watchdog the OS turned on in its time is off again
   * before gpSP's masked frames go on (nspire_os_watchdog_off). */
  nspire_os_watchdog_off();
  if (last_wake - n > sleep_longest)
    sleep_longest = last_wake - n;
  /* speed10 (nspire_pictures.c): a wait of more than a tick and a half
   * (one's is up to a tick): time the OS might have drawn in */
  if (last_wake - n > (os_tick256 * 3) >> 9)
    nspire_os_long_sleeps++;
  slept_ticks += last_wake - n;
  if (nspire_os_idle)
  {
    idle_slept_ticks += last_wake - n;
    idle_sleeps++;
  }
  sleeps++;
  /* Two wakes a few ticks apart are a whole number of ticks apart: the
   * tick's length from them, into the running average (an eighth). */
  since = last_wake - before;
  if (since && since < PHASE_TICKS * (os_tick256 >> 8))
  {
    n = (u32)((((u64)since << 8) + os_tick256 / 2) / os_tick256);
    if (n >= 1 && n <= PHASE_TICKS)
      os_tick256 = (os_tick256 * 7 + (u32)(((u64)since << 8) / n)) / 8;
  }
  if (nspire_os_wake_hook)
    nspire_os_wake_hook();
  return 1;
}

/* The OS's next tick (timer ticks) after 'now', from the last wake; 0 in
 * *known if where the ticks fall isn't known (then the latest it can be). */
static u32 next_tick(u32 now, int *known)
{
  u32 since = now - last_wake, n, d = tt_counts();

  if (d)
  {
    /* (speed9a) the timer: the middle of its count's bracket */
    *known = 1;
    return now + (u32)(((u64)(2 * d - 1) * tt_count256) >> 9);
  }
  if (since >= PHASE_TICKS * (os_tick256 >> 8))
  {
    *known = 0;
    return now + (os_tick256 + 255) / 256;
  }
  *known = 1;
  n = (u32)(((u64)since << 8) / os_tick256) + 1;
  return last_wake + (u32)(((u64)n * os_tick256) >> 8);
}

int nspire_os_sleep_before(uint32_t deadline)
{
  u32 next;
  int known;

  if (state <= 0 && !nspire_os_ready())
    return 0;
  next = next_tick(nspire_ticks(), &known);
  if ((s32)(deadline - next) < SLEEP_ROOM)
    return 0;
  return nspire_os_sleep();
}

/* The near wait's least: a tenth of the tick, at least 16 timer ticks
 * (CX II 10.25 ms: 33, 1 ms; CX 2.98 ms: 9, so 16, 0.5 ms). */
static u32 near_least(void)
{
  u32 w = (os_tick256 >> 8) / 10;

  return w < 16 ? 16 : w;
}

uint32_t nspire_os_near_wait(uint32_t since, uint32_t from, uint32_t until)
{
  u32 w = near_least();

  if (until > from && since > from)
  {
    if (since > until)
      since = until;
    w += (u32)((u64)(os_tick256 >> 9) * (since - from) / (until - from));
  }
  return w;
}

int nspire_os_sleep_near(uint32_t max_wait)
{
  u32 now, next;
  int known;

  if (state <= 0 && !nspire_os_ready())
    return 0;
  if (!max_wait)
    max_wait = near_least();
  now = nspire_ticks();
  next = next_tick(now, &known);
  if (!known || (s32)(next - now) > (s32)max_wait)
    return 0;
  return nspire_os_sleep();
}

/* 2026-11-15 (USB audio's OS time, nspire_audio.c): how far the next tick
 * is, and whether that's known, without sleeping or looking for TCC_Sleep. */
int nspire_os_next_tick_in(uint32_t *ticks, uint32_t *period)
{
  u32 now = nspire_ticks(), next;
  int known;

  *period = (os_tick256 + 128) / 256;
  if (state <= 0)
  {
    *ticks = (os_tick256 + 255) / 256;
    return 0;
  }
  next = next_tick(now, &known);
  *ticks = next - now;
  return known;
}

uint32_t nspire_os_busy_slept(uint32_t *count)
{
  if (count)
    *count = sleeps - idle_sleeps;
  return slept_ticks - idle_slept_ticks;
}

uint32_t nspire_os_since_sleep(void)
{
  return nspire_ticks() - last_wake;
}

uint32_t nspire_os_sleeps(void)
{
  return sleeps;
}

uint32_t nspire_os_sleep_longest(void)
{
  u32 v = sleep_longest;
  sleep_longest = 0;
  return v;
}

void nspire_os_report(char *text, unsigned size)
{
  unsigned n;

  if (state <= 0)
  {
    snprintf(text, size, "OS sleep: none (%s; %lu matches)", why, (unsigned long)matches);
    return;
  }
  snprintf(text, size, "OS sleep: TCC_Sleep at %08lx, current task at %08lx, OS tick %lu us, %lu sleeps; "
           "keypad interrupt %s, turned on by the OS %lu times; busy cursor turned off %lu times, ",
           (unsigned long)sleep_at, (unsigned long)task_var,
           (unsigned long)((u64)os_tick256 * 1000000 / 256 / NSPIRE_TICK_HZ), (unsigned long)sleeps,
           word(IRQ_ENABLE) & (1u << 16) ? "on" : "off (gpSP has the keys)", (unsigned long)keypad_retaken,
           (unsigned long)cursor_offs);
  n = (unsigned)strlen(text);
  if (n < size)
  {
    if (cursor_parked)
      snprintf(text + n, size - n, "parked off the panel, moved back %lu times (xy %08lx)",
               (unsigned long)cursor_moves, (unsigned long)word(LCD_CURSOR_XY));
    else
      snprintf(text + n, size - n, "not parked");
  }
  n = (unsigned)strlen(text);
  if (n + 2 < size)
  {
    /* (speed9a) which rule gives the tick's phase */
    snprintf(text + n, size - n, "; ");
    nspire_os_tick_rule_report(text + n + 2, size - n - 2);
  }
}

int nspire_os_task_stack(uint32_t task, uint32_t words[3])
{
  if (!is_task(task))
    return 0;
  words[0] = word(task + 0x24);
  words[1] = word(task + 0x28);
  words[2] = word(task + 0x2c);
  return 1;
}

/* ---- The diagnostics: read only ---- */

#define MODEL_WORD     0x900A0000u
#define OS_ID_WORD     0x10000020u
#define USB_BASE       0xB0000000u
/* A CX II's FOTG210. */
#define FOTG_CAP       (USB_BASE + 0x00)   /* (& 0xFFFF00FF) == 0x01000010 */
#define FOTG_USBCMD    (USB_BASE + 0x10)   /* bit 0: run */
#define FOTG_PORTSC    (USB_BASE + 0x30)   /* bit 0: connected, bit 2: enabled */
#define FOTG_OTGCSR    (USB_BASE + 0x80)
#define FOTG_INT_MASK  (USB_BASE + 0xC4)
#define OTGCSR_DEV_B   (1u << 21)   /* the ID pin: the device end (1), the host's with the adapter (0) */
#define OTGCSR_ROLE_D  (1u << 20)   /* acting as the device */
#define OTGCSR_A_VBUS  (1u << 19)
#define OTGCSR_A_SESS  (1u << 18)
#define OTGCSR_B_SESS  (1u << 17)
#define OTGCSR_B_END   (1u << 16)
/* A CX's ChipIdea controller. */
#define CI_ID          (USB_BASE + 0x000)   /* low 16 bits 0xFA05 */
#define CI_CAP         (USB_BASE + 0x100)   /* (& 0xFFFF00FF) == 0x01000040 */
#define CI_USBCMD      (USB_BASE + 0x140)
#define CI_USBSTS      (USB_BASE + 0x144)
#define CI_PORTSC      (USB_BASE + 0x184)   /* 0 connected, 2 enabled, 24 forced full speed, 27:26 speed */
#define CI_OTGSC       (USB_BASE + 0x1A4)
#define CI_USBMODE     (USB_BASE + 0x1A8)   /* bits 1:0: 2 device, 3 host */
#define OTGSC_IDPU     (1u << 5)    /* the ID pin's pull-up */
#define OTGSC_ID       (1u << 8)    /* 0: this calculator has the adapter (the host's end) */
#define OTGSC_AVV      (1u << 9)
#define OTGSC_ASV      (1u << 10)
#define OTGSC_BSV      (1u << 11)
#define OTGSC_BSE      (1u << 12)
/* A CX's bus access to the USB controller is off with bit 5 here, and its
 * registers read stale (Hackspire). */
#define CX_USB_GATE    0x900B0018u
#define LCD_CURSOR     0xC0000C00u   /* PL111: bit 0, the hardware cursor (the OS's busy one) */
/* The backlight: a CX II's (0 brightest, 225 darkest), a CX's (its LCD
 * contrast, higher brighter): nspire_brightness.c. The OS dims it after a
 * while without a key. */
#define BACKLIGHT_CX2  0x90130014u
#define BACKLIGHT_CX   0x900F0020u
/* The SP805 watchdog (Hackspire: 0x90060000 on the CX and the CX II): its
 * load, count, control (bit 0 counting with its interrupt, bit 1 reset on
 * the second time-out), interrupt clear (a write reloads the count), raw
 * interrupt, and lock (1 locked; 0x1ACCE551 unlocks). */
#define WDOG_LOAD      0x90060000u
#define WDOG_VALUE     0x90060004u
#define WDOG_CONTROL   0x90060008u
#define WDOG_INTCLR    0x9006000Cu
#define WDOG_RIS       0x90060010u
#define WDOG_LOCK      0x90060C00u
#define WDOG_KEY       0x1ACCE551u

/* Ndless's OS ids (resources/utils.c: the word at 0x10000020) of the CX
 * models' OSes. */
static const struct
{
  u32 id;
  const char *name;
} os_ids[] =
{
  { 0x102f0a10u, "3.1.0 CX" }, { 0x102f11a0u, "3.1.0 CAS CX" }, { 0x10375620u, "3.6.0 CX" },
  { 0x10376090u, "3.6.0 CAS CX" }, { 0x1037c760u, "3.9.0 CX" }, { 0x1037ccc0u, "3.9.0 CAS CX" },
  { 0x1037d160u, "3.9.1 CX" }, { 0x1037d6c0u, "3.9.1 CAS CX" }, { 0x10386e70u, "4.0.0.235 CX" },
  { 0x103873b0u, "4.0.0.235 CAS CX" }, { 0x1038c290u, "4.0.3.93 CX" }, { 0x1038c7d0u, "4.0.3.93 CAS CX" },
  { 0x1039c7a0u, "4.2.0.532 CX" }, { 0x1039cd20u, "4.2.0.532 CAS CX" }, { 0x103a3100u, "4.3.0.702 CX" },
  { 0x103a3690u, "4.3.0.702 CAS CX" }, { 0x103a9f50u, "4.4.0.532 CX" }, { 0x103aa4e0u, "4.4.0.532 CAS CX" },
  { 0x103b1860u, "4.5.0.1180 CX" }, { 0x103b1e60u, "4.5.0.1180 CAS CX" }, { 0x103b27d0u, "4.5.1.12 CX" },
  { 0x103b2ef0u, "4.5.1.12 CAS CX" }, { 0x103b3020u, "4.5.3.14 CX" }, { 0x103b3740u, "4.5.3.14 CAS CX" },
  { 0x103b3a10u, "4.5.4.48 CX" }, { 0x103b4130u, "4.5.4.48 CAS CX" }, { 0x103b18e0u, "4.5.5 CX" },
  { 0x103b1f80u, "4.5.5 CAS CX" }, { 0x1040e4d0u, "5.2.0.771 CX II" }, { 0x1040eae0u, "5.2.0.771 CX II-T" },
  { 0x1040f3b0u, "5.2.0.771 CAS CX II" }, { 0x10416cc0u, "5.3.0.564 CX II" },
  { 0x10417460u, "5.3.0.564 CX II-T" }, { 0x10417da0u, "5.3.0.564 CAS CX II" },
  { 0x10429e10u, "6.2.0.333 CX II" }, { 0x1042a580u, "6.2.0.333 CX II-T" }, { 0x1042ae10u, "6.2.0.333 CAS CX II" },
  { 0x10429ec0u, "6.4.0.74 CX II" }, { 0x1042a600u, "6.4.0.74 CX II-T" }, { 0x1042aee0u, "6.4.0.74 CAS CX II" },
};

/* R4: the CX II power controller's words (Firebird's model), read only. */
static const u32 pmu_offsets[] = { 0x00, 0x04, 0x08, 0x0C, 0x20, 0x24, 0x30, 0x50, 0x60,
                                   0x800, 0x804, 0x808, 0x80C, 0x810, 0x814, 0x818, 0x81C };
static char pmu_at_start[200];

static void pmu_words(char *out, unsigned size)
{
  unsigned i, n = 0;
  out[0] = 0;
  for (i = 0; i < sizeof(pmu_offsets) / sizeof(pmu_offsets[0]) && n < size; i++)
    n += (unsigned)snprintf(out + n, size - n, "%s%lx", i ? " " : "", (unsigned long)word(0x90140000u + pmu_offsets[i]));
}

/* At gpSP's start (main): the words the run after a reset is started in. */
void nspire_os_note_start(void)
{
  if (is_cx2 && !pmu_at_start[0])
    pmu_words(pmu_at_start, sizeof(pmu_at_start));
}

void nspire_os_diag_system(char *text, unsigned size)
{
  u32 id = word(OS_ID_WORD), i;
  const char *name = "not one Ndless lists for a CX";

  for (i = 0; i < sizeof(os_ids) / sizeof(os_ids[0]); i++)
    if (os_ids[i].id == id)
      name = os_ids[i].name;
  snprintf(text, size, "calculator: OS %s (id %08lx), Ndless r%u, %s (model word %08lx)", name,
           (unsigned long)id, (unsigned)nl_ndless_rev(), is_cx2 ? "CX II" : "CX", (unsigned long)word(MODEL_WORD));
  /* R3: a CX II's power controller (0x90140000 in Firebird's model, its
   * wake/reset reason among the first words), read only, so the run after a
   * reset says why the machine restarted; and the watchdog. R4: as gpSP
   * started (nspire_os_note_start) and now, every word Firebird's model has
   * (0x00-0x0C, 0x20 disable, 0x24 interrupt state, 0x30 clocks, 0x50, 0x60,
   * 0x800-0x81C). */
  if (is_cx2)
  {
    unsigned n = (unsigned)strlen(text);
    char now[200];
    pmu_words(now, sizeof(now));
    if (!pmu_at_start[0])
      nspire_os_note_start();
    if (n < size)
      snprintf(text + n, size - n, "; PMU at start %s; now %s; watchdog control %lx", pmu_at_start, now,
               (unsigned long)word(WDOG_CONTROL));
  }
}

/* Which controller the port has, if its registers read as one: 2 a CX II's
 * FOTG210, 1 a CX's ChipIdea, 0 neither (or its reads are stale). */
static int model_cx2(void)
{
  static int cx2 = -1;   /* Ndless's is_cx2 is a syscall: once */

  if (cx2 < 0)
    cx2 = is_cx2;
  return cx2;
}

static int usb_kind(void)
{
  if (model_cx2())
    return (word(FOTG_CAP) & 0xFFFF00FFu) == 0x01000010u ? 2 : 0;
  if (word(CX_USB_GATE) & 0x20)
    return 0;
  return (word(CI_ID) & 0xFFFF) == 0xFA05 && (word(CI_CAP) & 0xFFFF00FFu) == 0x01000040u ? 1 : 0;
}

int nspire_usb_plugged(void)
{
  u32 v;

  switch (usb_kind())
  {
  case 2:
    v = word(FOTG_OTGCSR);
    if (!v || v == 0xFFFFFFFFu)
      return -1;
    /* ND Screen Mirror's "detached": the device end, acting as one, no B
     * session, and the session ended. */
    return (v & (OTGCSR_DEV_B | OTGCSR_ROLE_D | OTGCSR_B_SESS | OTGCSR_B_END)) ==
           (OTGCSR_DEV_B | OTGCSR_ROLE_D | OTGCSR_B_END) ? 0 : 1;
  case 1:
    v = word(CI_OTGSC);
    if (!v || v == 0xFFFFFFFFu || !(v & OTGSC_IDPU))
      return -1;   /* without the pull-up the ID pin reads nothing */
    /* The device end and no session either way. (Not B session end too, as
     * on a CX II: Hackspire's OTGSC at rest is 0x120, the ID pin and its
     * pull-up only.) */
    return (v & (OTGSC_ID | OTGSC_AVV | OTGSC_ASV | OTGSC_BSV)) == OTGSC_ID ? 0 : 1;
  }
  return -1;
}

int nspire_usb_gated(void)
{
  return !model_cx2() && (word(CX_USB_GATE) & 0x20) != 0;
}

uint32_t nspire_usb_bits(void)
{
  switch (usb_kind())
  {
  case 2:
    return 0x20000000u | (word(FOTG_OTGCSR) >> 16 & 0x3F) | (word(FOTG_PORTSC) & 5) << 8;
  case 1:
    return 0x10000000u | (word(CI_OTGSC) >> 8 & 0x1F) | (word(CI_PORTSC) & 5) << 8 | (word(CI_USBMODE) & 3) << 12;
  }
  return 0;
}

/* PAD3: the words the pad log and the Controller page show (nspire_os.h).
 * Reads only: OTGISR, USBSTS and the global status are write-1-to-clear
 * and stay as they are for the OS's handler. */
int nspire_usb_raw(uint32_t w[NSPIRE_USB_RAW])
{
  int kind = usb_kind();
  unsigned i;

  for (i = 0; i < NSPIRE_USB_RAW; i++)
    w[i] = 0;
  if (kind == 2)
  {
    w[0] = word(FOTG_OTGCSR);
    w[1] = word(USB_BASE + 0x84);    /* OTGISR */
    w[2] = word(FOTG_USBCMD);
    w[3] = word(USB_BASE + 0x14);    /* USBSTS */
    w[4] = word(FOTG_PORTSC);
    w[5] = word(USB_BASE + 0xC0);    /* the global interrupt status */
  }
  else if (kind == 1)
  {
    w[0] = word(CI_OTGSC);
    w[1] = word(CI_USBMODE);
    w[2] = word(CI_USBCMD);
    w[3] = word(CI_USBSTS);
    w[4] = word(CI_PORTSC);
    w[5] = word(USB_BASE + 0x148);   /* USBINTR */
  }
  return kind;
}

/* The controller's kind for nspire_usbcycle.c (S4's power cycle). */
int nspire_usb_kind(void)
{
  return usb_kind();
}

/* Whether the host controller's run bit is on (the OS starts it only when
 * it has set the port up as the host): 1 running, 0 stopped, -1 can't tell. */
int nspire_usb_running(void)
{
  switch (usb_kind())
  {
  case 2:
    return word(FOTG_USBCMD) & 1;
  case 1:
    return word(CI_USBCMD) & 1;
  }
  return -1;
}

void nspire_os_diag_usb(char *text, unsigned size)
{
  u32 v, port;

  switch (usb_kind())
  {
  case 2:
    v = word(FOTG_OTGCSR);
    port = word(FOTG_PORTSC);
    snprintf(text, size, "USB (FOTG210): OTGCSR %08lx (%s end, acting as %s%s%s%s%s), USBCMD %08lx (%s), "
             "PORTSC %08lx (%s, %s), int mask %08lx", (unsigned long)v, v & OTGCSR_DEV_B ? "the device's" :
             "the host's (the adapter)", v & OTGCSR_ROLE_D ? "the device" : "the host",
             v & OTGCSR_A_VBUS ? ", A VBUS valid" : "", v & OTGCSR_A_SESS ? ", A session" : "",
             v & OTGCSR_B_SESS ? ", B session" : "", v & OTGCSR_B_END ? ", B session ended" : "",
             (unsigned long)word(FOTG_USBCMD), word(FOTG_USBCMD) & 1 ? "running" : "stopped",
             (unsigned long)port, port & 1 ? "connected" : "not connected", port & 4 ? "enabled" : "not enabled",
             (unsigned long)word(FOTG_INT_MASK));
    return;
  case 1:
    v = word(CI_OTGSC);
    port = word(CI_PORTSC);
    snprintf(text, size, "USB (ChipIdea): OTGSC %08lx (%s end%s%s%s%s%s), USBMODE %08lx (%s), USBCMD %08lx, "
             "USBSTS %08lx, PORTSC1 %08lx (%s, %s%s, speed %lu)", (unsigned long)v,
             v & OTGSC_ID ? "the device's" : "the host's (the adapter)", v & OTGSC_IDPU ? "" : ", no ID pull-up",
             v & OTGSC_AVV ? ", A VBUS valid" : "", v & OTGSC_ASV ? ", A session" : "",
             v & OTGSC_BSV ? ", B session" : "", v & OTGSC_BSE ? ", B session ended" : "",
             (unsigned long)word(CI_USBMODE), (word(CI_USBMODE) & 3) == 3 ? "host" :
             (word(CI_USBMODE) & 3) == 2 ? "device" : "idle", (unsigned long)word(CI_USBCMD),
             (unsigned long)word(CI_USBSTS), (unsigned long)port, port & 1 ? "connected" : "not connected",
             port & 4 ? "enabled" : "not enabled", port & (1u << 24) ? ", forced full speed" : "",
             (unsigned long)(port >> 26 & 3));
    return;
  }
  if (is_cx2)
    snprintf(text, size, "USB: the controller doesn't read as a FOTG210 (capability %08lx)",
             (unsigned long)word(FOTG_CAP));
  else
    snprintf(text, size, "USB: can't tell (bus access %s, id %08lx, capability %08lx)",
             word(CX_USB_GATE) & 0x20 ? "off: its reads are stale" : "on", (unsigned long)word(CI_ID),
             (unsigned long)word(CI_CAP));
}

void nspire_os_diag_screen(char *text, unsigned size)
{
  snprintf(text, size, "LCD base %08lx, control %08lx, cursor %s, backlight %03lx; IRQ enable %08lx; timers %08lx "
           "%08lx %08lx %08lx; clock %lu s", (unsigned long)word(0xC0000010u), (unsigned long)word(0xC0000018u),
           word(LCD_CURSOR) & 1 ? "on" : "off", (unsigned long)word(model_cx2() ? BACKLIGHT_CX2 : BACKLIGHT_CX),
           (unsigned long)word(IRQ_ENABLE), (unsigned long)word(0x900C0008u),
           (unsigned long)word(0x900C0028u), (unsigned long)word(0x900C0080u), (unsigned long)word(0x900D0008u),
           (unsigned long)word(0x90090000u));
}

void nspire_os_diag_watch(char *text, unsigned size)
{
  snprintf(text, size, "wd %lx/%lx/%lx%s%s, off %lu; bl %03lx; cur %lu; irq %08lx", (unsigned long)word(WDOG_CONTROL),
           (unsigned long)word(WDOG_LOAD), (unsigned long)word(WDOG_VALUE), word(WDOG_RIS) & 1 ? " due" : "",
           word(WDOG_LOCK) & 1 ? " locked" : "", (unsigned long)watchdog_offs, (unsigned long)word(model_cx2() ? BACKLIGHT_CX2 : BACKLIGHT_CX),
           (unsigned long)(word(LCD_CURSOR) & 1), (unsigned long)word(IRQ_ENABLE));
}

/* R11w: the exit's backstop is armed (nspire_os_backstop_arm) */
static int backstop;

int nspire_os_watchdog_off(void)
{
  /* R6: called after OS time (sleeps, NavNet calls, each linked frame): the
   * keypad's interrupt off again too, if the OS turned it on; EXIT-2: and
   * its busy cursor. */
  nspire_os_after_os_time();
  if (backstop)
    return 0;   /* R11w: the exit's own (it resets a hung USB shutdown) */
  if (!(word(WDOG_CONTROL) & 3))
    return 0;
  put(WDOG_LOCK, WDOG_KEY);
  put(WDOG_CONTROL, 0);
  put(WDOG_LOCK, 0);
  watchdog_offs++;
  return 1;
}

uint32_t nspire_os_watchdog_offs(void)
{
  return watchdog_offs;
}

/* ---- R11w (2026-11-15): the exit's backstop and the restart ---- */

static void irqs_off_for_good(void)
{
#if defined(__arm__) && !defined(NSPIRE_OS_FAKE_REGS)
  u32 cpsr;
  __asm__ volatile("mrs %0, cpsr\n\torr %0, %0, #0xC0\n\tmsr cpsr_c, %0" : "=r"(cpsr) : : "memory");
#endif
}

uint32_t nspire_os_backstop_arm(uint32_t ms)
{
  u32 v0, v1, t0, rate, spin;

  put(WDOG_LOCK, WDOG_KEY);
  put(WDOG_CONTROL, 0);
  put(WDOG_LOAD, 0xFFFFFFFFu);
  put(WDOG_INTCLR, 1);              /* (a write reloads the count) */
  put(WDOG_CONTROL, 1);             /* counting, no reset yet: its speed first */
  v0 = word(WDOG_VALUE);
  t0 = nspire_ticks();
  /* (r19-usb: bounded by a count too: a timer that doesn't run never ends
   * a wait on its ticks, and nothing then would) */
  for (spin = 0; nspire_ticks() - t0 < NSPIRE_TICK_HZ / 200 && spin < 4000000u; spin++)   /* ~5 ms by gpSP's timer */
    ;
  v1 = word(WDOG_VALUE);
  rate = (u32)((u64)(v0 - v1) * NSPIRE_TICK_HZ / (nspire_ticks() - t0 ? nspire_ticks() - t0 : 1) / 1000);
  if (!rate || v1 >= v0)
  {
    /* Retry over ~10 ms if the watchdog clock is slow or hasn't ticked in 5 ms. */
    for (spin = 0; nspire_ticks() - t0 < NSPIRE_TICK_HZ / 100 && spin < 8000000u; spin++)
      ;
    v1 = word(WDOG_VALUE);
    if (v0 > v1)
    {
      u32 dt = nspire_ticks() - t0;
      rate = (u32)((u64)(v0 - v1) * NSPIRE_TICK_HZ / (dt ? dt : 1) / 1000);
      if (!rate)
        rate = 1;
    }
  }
  if (!rate || v1 >= v0)
  {
    put(WDOG_CONTROL, 0);           /* it doesn't count: no backstop */
    put(WDOG_LOCK, 0);
    return 0;
  }
  put(WDOG_LOAD, (u32)((u64)rate * ms / 2 > 0xFFFFFFF0u ? 0xFFFFFFF0u : (u64)rate * ms / 2));
  put(WDOG_INTCLR, 1);
  put(WDOG_CONTROL, 3);             /* interrupt at the first time-out, reset at the second */
  put(WDOG_LOCK, 0);
  backstop = 1;
  return rate;
}

void nspire_os_backstop_disarm(void)
{
  if (!backstop)
    return;
  backstop = 0;
  put(WDOG_LOCK, WDOG_KEY);
  put(WDOG_CONTROL, 0);
  put(WDOG_INTCLR, 1);
  put(WDOG_LOCK, 0);
}

int nspire_os_backstop_armed(void)
{
  return backstop;
}

void nspire_os_restart(void)
{
  unsigned spin;

  irqs_off_for_good();
  put(0x900A0008u, 2);              /* the CPU reset (Ndless ut_calc_reboot) */
  for (spin = 0; spin < 100000; spin++)
    (void)word(WDOG_VALUE);
  /* still here: the watchdog, starved (its fallback there too) */
  put(WDOG_LOCK, WDOG_KEY);
  put(WDOG_LOAD, 0x1000);
  put(WDOG_CONTROL, 3);
#ifndef NSPIRE_OS_FAKE_REGS
  for (;;)
    ;
#endif
}

/* R6: the OS's keypad interrupt (nspire_os.h). 0xDC000010 reads the
 * enabled lines and a 1 written there enables one; 0xDC000014 disables
 * (Ndless's msleep, PocketSNES's clock switch). */
#define IRQ_DISABLE 0xDC000014u
#define KEYPAD_IRQ  (1u << 16)
/* The keypad controller (Firebird's keypad.cpp): control (scan mode and
 * timing), size, interrupt status (a 1 written clears; never written
 * here: what's pending is the OS's to see), interrupt enable, the GPIO
 * (touchpad) interrupt enable and status. gpSP only reads its key words
 * (0x10-0x1C). */
#define KPC_CONTROL     0x900E0000u
#define KPC_SIZE        0x900E0004u
#define KPC_INT_STATUS  0x900E0008u
#define KPC_INT_ENABLE  0x900E000Cu
#define KPC_GPIO_ENABLE 0x900E0040u
#define KPC_GPIO_STATUS 0x900E0044u

/* EXIT-1: the words gpSP may change or the OS may leave changed while
 * gpSP has the machine, as read at start (nspire_os_hw_note_start). */
enum { HW_IRQ, HW_KPC_CONTROL, HW_KPC_SIZE, HW_KPC_STATUS, HW_KPC_ENABLE, HW_GPIO_ENABLE, HW_GPIO_STATUS, HW_CURSOR,
       HW_CURSOR_XY, HW_WORDS };
static const u32 hw_at[HW_WORDS] = { IRQ_ENABLE, KPC_CONTROL, KPC_SIZE, KPC_INT_STATUS, KPC_INT_ENABLE, KPC_GPIO_ENABLE,
                                     KPC_GPIO_STATUS, LCD_CURSOR, LCD_CURSOR_XY };
static u32 hw_start[HW_WORDS];
static int hw_noted;

static void hw_read(u32 words[HW_WORDS])
{
  int i;
  for (i = 0; i < HW_WORDS; i++)
    words[i] = word(hw_at[i]);
}

/* EXIT-R5: on a CX II, the OS's hardware cursor (its pointer, and its busy
 * clock: "cursor on" at every sample of its linked log) is put where the
 * panel doesn't show it, as well as turned off after the OS's time: if the
 * OS turns it on again in each of its slices (a sleep, a NavNet call that
 * blocks for seconds), the clock showed for the whole slice. With its
 * keypad line off the OS gets no touchpad events, so it has no reason to
 * move its pointer; if it does, after_os_time puts it back and counts it.
 * The words as at start come back in the hand-back. */
void nspire_os_hw_note_start(void)
{
  if (hw_noted)
    return;
  hw_read(hw_start);
  hw_noted = 1;
  if (is_cx2)
  {
    put(LCD_CURSOR_XY, CURSOR_PARKED);
    cursor_parked = 1;
  }
}

int nspire_os_keypad_irq(int on)
{
  int was = (word(IRQ_ENABLE) & KEYPAD_IRQ) != 0;

  if (handed_back && !on)
    return was;   /* never masked again after the hand-back */
  keypad_taken = !on;
  if (!on)
    put(IRQ_DISABLE, KEYPAD_IRQ);
  else if (!was)
    put(IRQ_ENABLE, KEYPAD_IRQ);
  return was;
}

int nspire_os_keypad_keep_off(void)
{
  if (handed_back || !keypad_taken || !(word(IRQ_ENABLE) & KEYPAD_IRQ))
    return 0;
  put(IRQ_DISABLE, KEYPAD_IRQ);
  keypad_retaken++;
  return 1;
}

uint32_t nspire_os_keypad_retaken(void)
{
  return keypad_retaken;
}

/* EXIT-2: the OS draws its busy cursor (a turning clock, the LCD's
 * hardware cursor: 0xC0000C00 bit 0) when its main task, which gpSP runs
 * in, gets time but pumps no events, which the seamless code gives it far
 * more often than before (the CX II's log: "cursor on" while linked). Ndless
 * turns it off for a CX II program and puts it back after; the OS turned
 * it on again in its time. So off again after each place the OS ran: one
 * read, a write only when found on; on a CX II one more read, the
 * cursor's place (EXIT-R5). Called each linked frame too (nspire_link.c,
 * through nspire_os_watchdog_off): two or three uncached reads. */
void nspire_os_after_os_time(void)
{
  if (handed_back)
    return;
  nspire_os_keypad_keep_off();
  if (hw_noted && (word(LCD_CURSOR) & 1))
  {
    put(LCD_CURSOR, word(LCD_CURSOR) & ~1u);
    cursor_offs++;
  }
  if (cursor_parked && word(LCD_CURSOR_XY) != CURSOR_PARKED)
  {
    put(LCD_CURSOR_XY, CURSOR_PARKED);
    cursor_moves++;
  }
}

uint32_t nspire_os_cursor_offs(void)
{
  return cursor_offs;
}

static int hw_put(char *text, unsigned size, const char *name, const u32 w[HW_WORDS])
{
  return snprintf(text, size, "%s: irq %08lx (keypad line %s), keypad %08lx %08lx status %08lx enable %08lx, "
                  "gpio enable %08lx status %08lx, cursor %08lx xy %08lx", name, (unsigned long)w[HW_IRQ],
                  w[HW_IRQ] & KEYPAD_IRQ ? "on" : "off", (unsigned long)w[HW_KPC_CONTROL],
                  (unsigned long)w[HW_KPC_SIZE], (unsigned long)w[HW_KPC_STATUS], (unsigned long)w[HW_KPC_ENABLE],
                  (unsigned long)w[HW_GPIO_ENABLE], (unsigned long)w[HW_GPIO_STATUS], (unsigned long)w[HW_CURSOR],
                  (unsigned long)w[HW_CURSOR_XY]);
}

void nspire_os_hand_back_text(char *text, unsigned size)
{
  u32 now[HW_WORDS];
  int n;

  hw_read(now);
  n = hw_put(text, size, "hardware at start", hw_start);
  if (n > 0 && (unsigned)n < size)
    n += snprintf(text + n, size - n, "\n");
  if (n > 0 && (unsigned)n < size)
    n += hw_put(text + n, size - n, "hardware at exit, before the hand-back", now);
  if (n > 0 && (unsigned)n < size)
    snprintf(text + n, size - n, "\nhand-back writes: keypad line on%s%s%s%s; the OS turned the keypad line on %lu "
             "times, its busy cursor %lu times (turned off), moved the cursor back on the panel %lu times%s, "
             "%lu sleeps refused after",
             hw_start[HW_IRQ] & KEYPAD_IRQ ? "" : " (it was off at start: on all the same)",
             now[HW_KPC_CONTROL] != hw_start[HW_KPC_CONTROL] || now[HW_KPC_SIZE] != hw_start[HW_KPC_SIZE] ||
             now[HW_KPC_ENABLE] != hw_start[HW_KPC_ENABLE] || now[HW_GPIO_ENABLE] != hw_start[HW_GPIO_ENABLE] ?
             ", keypad controller as at start" : "",
             now[HW_CURSOR] != hw_start[HW_CURSOR] ? ", cursor as at start" : "",
             now[HW_CURSOR_XY] != hw_start[HW_CURSOR_XY] ? ", cursor's place as at start" : "",
             (unsigned long)keypad_retaken, (unsigned long)cursor_offs, (unsigned long)cursor_moves,
             cursor_parked ? "" : " (not parked: a CX)", (unsigned long)sleeps_refused);
}

/* EXIT-R4: the same words read back right after the hand-back's writes,
 * for a file written after it (nspire_exit.c): whether they took. */
void nspire_os_after_hand_back_text(char *text, unsigned size)
{
  u32 now[HW_WORDS];

  hw_read(now);
  hw_put(text, size, "after the hand-back", now);
}

/* QUIT6 (nspire_os.h): the controller's IRQ status (raised, enabled, not a
 * FIQ: what the CPU takes once its I bit clears) and raw lines; reads only
 * (0x30, which acknowledges, is never read). Line 18 is the timer block at
 * 0x900C0000 (gpSP's 0x900C0000 and the OS's 0x900C0020), 19 the OS's tick,
 * 16 the keypad. */
#define IRQ_STATUS 0xDC000000u
#define IRQ_RAW    0xDC000008u
void nspire_os_irq_text(char *text, unsigned size, const char *when)
{
  u32 status = word(IRQ_STATUS), raw = word(IRQ_RAW), enabled = word(IRQ_ENABLE);

  snprintf(text, size, "interrupt lines %s: raised and enabled %08lx%s, raised %08lx, enabled %08lx", when,
           (unsigned long)status, status & (1u << 18) ? " (18: the 900c0000 timer block's)" : "", (unsigned long)raw,
           (unsigned long)enabled);
}

/* EXIT-1 (nspire_os.h): register writes only, nothing that lets the OS run;
 * the interrupt status words are left as they are (pending: the OS's
 * handler sees the keys as they are once the CPU takes interrupts again,
 * after Ndless's own wait for no key down). */
void nspire_os_hand_back(void)
{
  static const int restore[] = { HW_KPC_CONTROL, HW_KPC_SIZE, HW_KPC_ENABLE, HW_GPIO_ENABLE };
  unsigned i;

  handed_back = 1;
  keypad_taken = 0;
  if (hw_noted)
  {
    for (i = 0; i < sizeof(restore) / sizeof(restore[0]); i++)
      if (word(hw_at[restore[i]]) != hw_start[restore[i]])
        put(hw_at[restore[i]], hw_start[restore[i]]);
    /* The cursor's place first, then whether it shows (EXIT-R5). */
    if (word(LCD_CURSOR_XY) != hw_start[HW_CURSOR_XY])
      put(LCD_CURSOR_XY, hw_start[HW_CURSOR_XY]);
    if (word(LCD_CURSOR) != hw_start[HW_CURSOR])
      put(LCD_CURSOR, hw_start[HW_CURSOR]);
  }
  /* On whatever it was at start: "off" there can only be a moment of the
   * OS's (it reads its keys through it), and off for good is a dead keypad. */
  put(IRQ_ENABLE, KEYPAD_IRQ);
}

int nspire_os_handed_back(void)
{
  return handed_back;
}
