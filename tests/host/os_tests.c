/* EXIT-R3: nspire_os.c's own exit code on the PC, against a register file
 * of its own (built with NSPIRE_OS_FAKE_REGS: every word() and put() comes
 * here). gpsp-host has stand-ins for these functions (host_platform.c), so
 * only this program runs the calculator's code: the hand-back's writes and
 * their order, line 16 at the interrupt controller, the keypad controller
 * written only where it differs from the start, the OS's busy cursor
 * turned off and counted, the CX II's cursor put off the panel and back
 * (EXIT-R5), the read back after the hand-back (EXIT-R4), the marker's
 * wording, and nothing after the hand-back. Each case runs in its own
 * process (nspire_os.c's state is static). */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "nspire.h"
#include "nspire_os.h"

int host_is_cx2;
unsigned nl_ndless_rev(void) { return 2020; }
static u32 ticks, tick_step = 100;
u32 nspire_ticks(void) { return ticks += tick_step; }
static int irq_windows;
void host_os_irq_window(void) { irq_windows++; }

/* ---- The register file ---- */
#define IRQ_ENABLE  0xDC000010u
#define IRQ_DISABLE 0xDC000014u
#define KPC_CONTROL 0x900E0000u
#define KPC_SIZE    0x900E0004u
#define KPC_STATUS  0x900E0008u
#define KPC_ENABLE  0x900E000Cu
#define GPIO_ENABLE 0x900E0040u
#define GPIO_STATUS 0x900E0044u
#define CURSOR      0xC0000C00u
#define CURSOR_XY   0xC0000C10u
#define CX_USB_GATE 0x900B0018u

static struct { u32 at, v; } regs[64];
static int nregs;
static struct { u32 at, v; } writes[64];
static int nwrites;

static u32 *fake_reg(u32 at)
{
  int i;
  for (i = 0; i < nregs; i++)
    if (regs[i].at == at)
      return &regs[i].v;
  regs[nregs].at = at;
  regs[nregs].v = 0;
  return &regs[nregs++].v;
}

static u32 wdog_step = 1000;
static int wdog_reads, wdog_skip_read;
static int vector_reads;   /* QUIT6: 0xDC000030 (reading it acknowledges an interrupt) */

/* (speed9a) the OS's tick's timer, an SP804 at 0x900D0000 (sim_on): 'load'
 * + 1 counts of 'cnt' timer ticks (the fake nspire_ticks), the tick as the
 * count reaches 0 (at_zero) or as it reloads after 0; its raw interrupt
 * latched from the probe's start (no OS to clear it); 'shake': each count's
 * length off by up to that many ticks, alternately (not steady); 'stuck': the
 * count never moves. */
static struct { int on, at_zero, stuck; u32 load, cnt, phase, shake, t0; } sim;
#define TT_LOAD  0x900D0000u
#define TT_VALUE 0x900D0004u
#define TT_CTRL  0x900D0008u
#define TT_RAW   0x900D0010u

static u32 sim_counts(u32 t)   /* counts since the timer's last reload at (t - phase) */
{
  u32 n = (t + sim.phase) / sim.cnt;
  if (sim.shake && ((t + sim.phase) % (2 * sim.cnt)) < sim.shake)
    n -= n ? 1 : 0;
  return sim.stuck ? 0 : n;
}

static u32 sim_value(u32 t)
{
  return sim.load - sim_counts(t) % (sim.load + 1);
}

static u32 sim_raw(u32 t)
{
  /* a tick between the start and t: a count reaching 0 (or, after it, the reload) */
  u32 a = sim_counts(sim.t0), b = sim_counts(t), off = sim.at_zero ? sim.load : sim.load + 1;
  return (b + (sim.load + 1) - off) / (sim.load + 1) > (a + (sim.load + 1) - off) / (sim.load + 1);
}

u32 os_test_reg_read(u32 at)
{
  if (sim.on && at == TT_VALUE)
    return sim_value(ticks);
  if (sim.on && at == TT_RAW)
    return sim_raw(ticks);
  if (at == 0xDC000030u)
    vector_reads++;
  /* R11w: the watchdog counts down while its control's bit 0 is set (wdog_step
   * counts a read, with optional freeze on read wdog_skip_read to test the
   * 10 ms fallback calibration path). */
  if (at == 0x90060004u && (*fake_reg(0x90060008u) & 1))
  {
    wdog_reads++;
    if (wdog_reads != wdog_skip_read)
      *fake_reg(at) -= wdog_step;
  }
  return *fake_reg(at);
}

/* (speed9a) the lines' inline check reads the tick timer's count through a
 * pointer (nspire_os.h nspire_os_watch_at): here a word sim_word() fills */
static u32 tt_value_word;
volatile const uint32_t *os_test_reg_ptr(u32 at)
{
  return at == TT_VALUE ? &tt_value_word : fake_reg(at);
}
static u32 sim_word(void)
{
  tt_value_word = sim_value(ticks);
  return *nspire_os_watch_at.reg;
}

/* The interrupt controller: a 1 written to 0xDC000010 enables that line, to
 * 0xDC000014 disables it (both read back through 0xDC000010). */
void os_test_reg_write(u32 at, u32 v)
{
  if (nwrites < 64)
  {
    writes[nwrites].at = at;
    writes[nwrites].v = v;
  }
  nwrites++;
  if (at == IRQ_ENABLE)
    *fake_reg(IRQ_ENABLE) |= v;
  else if (at == IRQ_DISABLE)
    *fake_reg(IRQ_ENABLE) &= ~v;
  else
    *fake_reg(at) = v;
}

static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static void set(u32 at, u32 v) { *fake_reg(at) = v; }

/* The words as the CX II's log had them (IRQ enable 080d0186 with the OS's
 * keypad line 16 on), a keypad controller, Ndless's cursor off, its place. */
static void calculator(int cx2)
{
  host_is_cx2 = cx2;
  set(IRQ_ENABLE, 0x080d0186u);
  set(KPC_CONTROL, 0x00001234u);
  set(KPC_SIZE, 0x00000808u);
  set(KPC_STATUS, 0x00000003u);
  set(KPC_ENABLE, 0x00000004u);
  set(GPIO_ENABLE, 0x00000800u);
  set(GPIO_STATUS, 0x00000001u);
  set(CURSOR, 0x00000002u);
  set(CURSOR_XY, 0x00500060u);
}

static int written(u32 at, int from)
{
  int i;
  for (i = from; i < nwrites && i < 64; i++)
    if (writes[i].at == at)
      return i;
  return -1;
}

static void case_cx2(void)
{
  char text[1600];
  int w;

  calculator(1);
  nspire_os_hw_note_start();
  CHECK(nwrites == 1 && writes[0].at == CURSOR_XY && writes[0].v == 0x01C001C0u,
        "cx2 start: the cursor put off the panel, nothing else written (%d writes, first %08x=%08x)", nwrites,
        (unsigned)writes[0].at, (unsigned)writes[0].v);
  nspire_os_keypad_irq(0);
  CHECK((*fake_reg(IRQ_ENABLE) & 0x10000u) == 0 && written(IRQ_DISABLE, 1) == 1, "cx2: line 16 off at start");

  /* An OS slice: it turned its line and busy cursor on, moved the cursor,
   * changed its keypad controller's control and GPIO enable. */
  set(IRQ_ENABLE, *fake_reg(IRQ_ENABLE) | 0x10000u);
  set(CURSOR, 0x00000003u);
  set(CURSOR_XY, 0x00100010u);
  set(KPC_CONTROL, 0x00009999u);
  set(GPIO_ENABLE, 0);
  nwrites = 0;
  nspire_os_after_os_time();
  CHECK((*fake_reg(IRQ_ENABLE) & 0x10000u) == 0, "cx2 after OS time: line 16 off again");
  CHECK(*fake_reg(CURSOR) == 0x00000002u, "cx2 after OS time: the cursor off, its other bits kept: %08x", (unsigned)*fake_reg(CURSOR));
  CHECK(*fake_reg(CURSOR_XY) == 0x01C001C0u, "cx2 after OS time: the cursor off the panel again: %08x",
        (unsigned)*fake_reg(CURSOR_XY));
  CHECK(nwrites == 3 && nspire_os_cursor_offs() == 1 && nspire_os_keypad_retaken() == 1,
        "cx2 after OS time: 3 writes, counted (%d, cursor %u, line %u)", nwrites, (unsigned)nspire_os_cursor_offs(),
        (unsigned)nspire_os_keypad_retaken());
  CHECK(*fake_reg(KPC_CONTROL) == 0x9999u, "cx2 after OS time: the keypad controller isn't touched while gpSP runs");
  nwrites = 0;
  nspire_os_after_os_time();
  CHECK(nwrites == 0, "cx2 after OS time, nothing changed: no write (%d)", nwrites);

  nspire_os_hand_back_text(text, sizeof(text));
  CHECK(!strcmp(text,
        "hardware at start: irq 080d0186 (keypad line on), keypad 00001234 00000808 status 00000003 enable 00000004, "
        "gpio enable 00000800 status 00000001, cursor 00000002 xy 00500060\n"
        "hardware at exit, before the hand-back: irq 080c0186 (keypad line off), keypad 00009999 00000808 status "
        "00000003 enable 00000004, gpio enable 00000000 status 00000001, cursor 00000002 xy 01c001c0\n"
        "hand-back writes: keypad line on, keypad controller as at start, cursor's place as at start; the OS turned "
        "the keypad line on 1 times, its busy cursor 1 times (turned off), moved the cursor back on the panel 1 "
        "times, 0 sleeps refused after"), "cx2: the marker's words:\n%s", text);

  nwrites = 0;
  nspire_os_hand_back();
  CHECK(written(KPC_CONTROL, 0) >= 0 && *fake_reg(KPC_CONTROL) == 0x1234u, "cx2 hand-back: keypad control as at start");
  CHECK(written(GPIO_ENABLE, 0) >= 0 && *fake_reg(GPIO_ENABLE) == 0x800u, "cx2 hand-back: GPIO enable as at start");
  CHECK(written(KPC_SIZE, 0) < 0 && written(KPC_ENABLE, 0) < 0, "cx2 hand-back: unchanged words not written");
  CHECK(written(KPC_STATUS, 0) < 0 && written(GPIO_STATUS, 0) < 0, "cx2 hand-back: status words never written");
  CHECK(written(CURSOR, 0) < 0, "cx2 hand-back: the cursor control was as at start: not written");
  w = written(CURSOR_XY, 0);
  CHECK(w >= 0 && *fake_reg(CURSOR_XY) == 0x00500060u, "cx2 hand-back: the cursor's place as at start: %08x",
        (unsigned)*fake_reg(CURSOR_XY));
  CHECK(nwrites == 4 && writes[3].at == IRQ_ENABLE && writes[3].v == 0x10000u,
        "cx2 hand-back: 4 writes, line 16 on the last (%d; last %08x=%08x)", nwrites, (unsigned)writes[3].at,
        (unsigned)writes[3].v);
  CHECK(*fake_reg(IRQ_ENABLE) == 0x080d0186u, "cx2 hand-back: the IRQ enable word as at start: %08x",
        (unsigned)*fake_reg(IRQ_ENABLE));
  CHECK(nspire_os_handed_back(), "cx2: handed back");

  /* After it: nothing masks, nothing writes, nothing sleeps into the OS. */
  set(CURSOR, 0x00000003u);
  nwrites = 0;
  nspire_os_keypad_irq(0);
  CHECK(!nspire_os_keypad_keep_off(), "cx2 after: keep_off does nothing");
  nspire_os_after_os_time();
  CHECK(nwrites == 0 && (*fake_reg(IRQ_ENABLE) & 0x10000u), "cx2 after the hand-back: no write (%d), line on", nwrites);
  CHECK(nspire_os_sleep() == 0, "cx2 after: a sleep refused");
  nspire_os_after_hand_back_text(text, sizeof(text));
  CHECK(!strcmp(text, "after the hand-back: irq 080d0186 (keypad line on), keypad 00001234 00000808 status 00000003 "
                      "enable 00000004, gpio enable 00000800 status 00000001, cursor 00000003 xy 00500060"),
        "cx2: the read back after the hand-back:\n%s", text);
  nspire_os_hand_back_text(text, sizeof(text));
  CHECK(strstr(text, ", 1 sleeps refused after") != NULL, "cx2: the refused sleep counted: %s", text);
}

/* An original CX: its cursor is off and never parked; its line off at start
 * (a moment of the OS's) comes back on all the same. */
static void case_cx(void)
{
  char text[1600];

  calculator(0);
  set(IRQ_ENABLE, 0x00080182u & ~0x10000u);
  nspire_os_hw_note_start();
  CHECK(nwrites == 0, "cx start: nothing written (%d)", nwrites);
  nspire_os_keypad_irq(0);
  nwrites = 0;
  nspire_os_after_os_time();
  CHECK(nwrites == 0, "cx after OS time, nothing on: no write (%d)", nwrites);
  nspire_os_hand_back_text(text, sizeof(text));
  CHECK(strstr(text, "hand-back writes: keypad line on (it was off at start: on all the same); ") &&
        strstr(text, "moved the cursor back on the panel 0 times (not parked: a CX), "), "cx: the marker's words:\n%s", text);
  nwrites = 0;
  nspire_os_hand_back();
  CHECK(nwrites == 1 && writes[0].at == IRQ_ENABLE && (*fake_reg(IRQ_ENABLE) & 0x10000u),
        "cx hand-back: line 16 on, nothing else to write (%d)", nwrites);
  /* A CX whose OS gated its USB controller off: can't tell (-1), gated. */
  set(CX_USB_GATE, 0x20);
  CHECK(nspire_usb_plugged() == -1 && nspire_usb_gated() == 1, "cx gated: plugged -1, gated 1");
}

static void run(const char *name, void (*fn)(void))
{
  pid_t pid;

  fflush(stdout);   /* (a child would print the parent's buffered lines again) */
  pid = fork();
  int status = 0;

  if (pid == 0)
  {
    fn();
    fflush(stdout);
    _exit(failures ? 1 : 0);
  }
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status))
  {
    printf("  FAIL os_tests %s\n", name);
    failures++;
  }
}

/* R11w (2026-11-15): the exit's watchdog backstop and the restart. */
#define WDOG_LOAD    0x90060000u
#define WDOG_VALUE   0x90060004u
#define WDOG_CONTROL 0x90060008u
#define WDOG_LOCK    0x90060C00u
static u32 last_write(u32 at)
{
  int i;
  u32 v = 0xDEADBEEFu;
  for (i = 0; i < nwrites && i < 64; i++)
    if (writes[i].at == at)
      v = writes[i].v;
  return v;
}

static void case_r11w(void)
{
  u32 rate;
  int k;

  calculator(1);
  set(WDOG_VALUE, 0x80000000u);
  rate = nspire_os_backstop_arm(12000);
  CHECK(rate > 0, "r11w: the backstop measured the watchdog's speed: %u", (unsigned)rate);
  CHECK(nspire_os_backstop_armed(), "r11w: armed");
  CHECK(last_write(WDOG_CONTROL) == 3, "r11w: interrupt and reset on: control %08x", (unsigned)last_write(WDOG_CONTROL));
  CHECK(last_write(WDOG_LOAD) == rate * 6000, "r11w: load for half of 12 s (the reset at the second time-out): %u",
        (unsigned)last_write(WDOG_LOAD));
  CHECK(last_write(WDOG_LOCK) == 0, "r11w: locked again");
  nwrites = 0;
  nspire_os_watchdog_off();   /* after an OS sleep: leaves the backstop alone */
  CHECK(written(WDOG_CONTROL, 0) < 0, "r11w: watchdog_off writes nothing while armed");
  nspire_os_backstop_disarm();
  CHECK(!nspire_os_backstop_armed() && last_write(WDOG_CONTROL) == 0, "r11w: disarmed: control 0");
  /* the restart: Ndless's ut_calc_reboot (the reset register, then the watchdog) */
  nwrites = 0;
  nspire_os_restart();
  k = written(0x900A0008u, 0);
  CHECK(k == 0 && writes[0].v == 2, "r11w: the restart writes 2 to 0x900A0008 first (%d)", k);
  CHECK(written(WDOG_LOCK, 1) > 0 && last_write(WDOG_LOAD) == 0x1000 && last_write(WDOG_CONTROL) == 3,
        "r11w: then the watchdog starved: unlocked, load 0x1000, control 3");
}

static void case_r13_wdog_fallback(void)
{
  u32 rate;

  calculator(1);
  set(WDOG_VALUE, 0x80000000u);
  wdog_step = 1000;
  wdog_reads = 0;
  wdog_skip_read = 2; /* 5 ms sample sees v1 == v0; 10 ms retry sees decrement */
  rate = nspire_os_backstop_arm(12000);
  CHECK(rate > 0 && wdog_reads == 3, "r13: 10 ms retry recovered watchdog rate %u (%d reads)",
        (unsigned)rate, wdog_reads);
  CHECK(nspire_os_backstop_armed() && last_write(WDOG_CONTROL) == 3 &&
        last_write(WDOG_LOAD) == rate * 6000,
        "r13: backstop armed after 10 ms retry: load %u", (unsigned)last_write(WDOG_LOAD));
  nspire_os_backstop_disarm();

  /* Slow watchdog (< 1 count/ms over 5-10 ms): floored to rate 1 rather than disabled. */
  set(WDOG_VALUE, 0x80000000u);
  wdog_step = 1;
  wdog_reads = 0;
  wdog_skip_read = 0;
  rate = nspire_os_backstop_arm(12000);
  CHECK(rate == 1 && last_write(WDOG_LOAD) == 6000 && nspire_os_backstop_armed(),
        "r13: slow watchdog floored to 1/ms (load %u)", (unsigned)last_write(WDOG_LOAD));
}

/* QUIT6: the interrupt lines at the hand-back, for the marker and the file
 * after it: the controller's IRQ status, raw lines and enables, read only
 * (no write, never 0x30, which acknowledges); line 18 (the 900c0000 timer
 * block, which the OS's handler only clears for its own timer) named. */
static void case_quit6_irq_lines(void)
{
  char text[300];

  calculator(1);
  set(0xDC000000u, 0x00040000u);
  set(0xDC000008u, 0x000c8000u);
  nwrites = 0;
  nspire_os_irq_text(text, sizeof(text), "before the put-backs");
  CHECK(!strcmp(text, "interrupt lines before the put-backs: raised and enabled 00040000 (18: the 900c0000 timer "
                      "block's), raised 000c8000, enabled 080d0186"), "quit6: the lines' words:\n%s", text);
  CHECK(nwrites == 0 && vector_reads == 0, "quit6: reads only, never 0x30 (%d writes, %d reads of 0x30)", nwrites,
        vector_reads);
  set(0xDC000000u, 0x00080000u);
  nspire_os_irq_text(text, sizeof(text), "after the hand-back");
  CHECK(!strcmp(text, "interrupt lines after the hand-back: raised and enabled 00080000, raised 000c8000, enabled "
                      "080d0186"), "quit6: the OS's tick alone raised, no line named:\n%s", text);
}

/* (speed9a) nspire_os.c's probe of the OS's tick's timer: read only, what it accepts and what it leaves to r15's rule. */
void nspire_os_test_tick(u32 tick256);

static void tick_timer(u32 load, u32 cnt, u32 control, int at_zero, u32 tick256)
{
  calculator(1);
  memset(&sim, 0, sizeof(sim));
  sim.on = 1;
  sim.load = load;
  sim.cnt = cnt;
  sim.at_zero = at_zero;
  sim.phase = 7 * cnt / 3;
  set(TT_LOAD, load);
  set(TT_CTRL, control);
  tick_step = 1;
  ticks = 1000;
  sim.t0 = ticks;
  nspire_os_test_tick(tick256);
}

static void case_tick_cx2(void)
{
  char text[400];
  uint32_t e, l, v, in, period;
  int known;

  /* the CX II's: load 0x14, control e6, a count 16 ticks (2048 Hz), 10.25 ms */
  tick_timer(20, 16, 0xe6, 1, 336 << 8);
  nwrites = 0;
  CHECK(nspire_os_tick_probe() == 1 && nspire_os_tick_timer(), "tick cx2: the timer read");
  CHECK(nwrites == 0, "tick cx2: nothing written (%d)", nwrites);
  nspire_os_tick_rule_report(text, sizeof(text));
  CHECK(strstr(text, "read from its timer") && strstr(text, "load 20") && strstr(text, "reaches 0") &&
        strstr(text, "a count 488 us"), "tick cx2: the log's line:\n%s", text);
  /* the bracket against the timer: every place in a tick */
  for (int k = 0; k < 21 * 16; k++)
  {
    u32 left;
    ticks = 5000 + k;
    left = 21 * 16 - (ticks + sim.phase + 16) % (21 * 16);   /* to the next count reaching 0 */
    CHECK(nspire_os_tick_bracket(&e, &l) && e < left + 1 && l + 1 >= left && l - e <= 17,
          "tick cx2: at %d the tick in %u, the bracket %u-%u", k, (unsigned)left, (unsigned)e, (unsigned)l);
    if (failures)
      break;
  }
  known = nspire_os_next_tick_in(&in, &period);   /* (no TCC_Sleep here: unknown, a whole tick) */
  CHECK(!known && period == 336, "tick cx2: no OS sleep, no tick foretold (%u of %u)", (unsigned)in, (unsigned)period);
  /* the USB task's 0.45 ms (15 ticks): from 2 counts to the tick (0.49-0.98 ms); 1 ms: 4 */
  CHECK(nspire_os_tick_need(15) == 2 && nspire_os_tick_need(33) == 4 && nspire_os_tick_need(400) == 0,
        "tick cx2: the counts for a need (%u %u %u)", (unsigned)nspire_os_tick_need(15),
        (unsigned)nspire_os_tick_need(33), (unsigned)nspire_os_tick_need(400));
  /* the lines' inline check: from d counts to the tick on, never raw 0 (just after it) */
  CHECK(nspire_os_watch_at.reg == os_test_reg_ptr(TT_VALUE), "tick cx2: the lines read the timer's count");
  for (u32 d = 2; d <= 4; d++)
  {
    nspire_os_watch_arm(d);
    for (int k = 0; k < 21 * 16; k++)
    {
      u32 counts, raw;
      ticks = 7000 + k;
      raw = sim_word();
      counts = nspire_os_tick_bracket(&e, &l);
      CHECK((raw - 1u < nspire_os_watch_at.value) == (counts <= d), "tick cx2: armed at %u, raw %u (counts %u)",
            (unsigned)d, (unsigned)raw, (unsigned)counts);
      if (failures)
        break;
    }
  }
  v = 0;
  (void)v;
  /* a CPU speed switch borrows the timer (nspire_clock.c): not read meanwhile */
  set(TT_CTRL, 0x82);
  CHECK(!nspire_os_tick_bracket(&e, &l), "tick cx2: another control word: not read");
  set(TT_CTRL, 0xe6);
  CHECK(nspire_os_tick_bracket(&e, &l), "tick cx2: as it was again: read");
}

static void case_tick_reload(void)
{
  char text[400];
  /* an SP804 whose interrupt comes as it reloads after 0 */
  tick_timer(20, 16, 0xe6, 0, 336 << 8);
  CHECK(nspire_os_tick_probe() == 1, "tick reload: the timer read");
  nspire_os_tick_rule_report(text, sizeof(text));
  CHECK(strstr(text, "as the count reloads") != NULL, "tick reload: %s", text);
  /* the inline check: counts 2..d (raw 1..d - 1); counts 1 (raw 0) is nearer than any need */
  nspire_os_watch_arm(3);
  for (int k = 0; k < 21 * 16; k++)
  {
    uint32_t e, l, counts, raw;
    ticks = 9000 + k;
    raw = sim_word();
    counts = nspire_os_tick_bracket(&e, &l);
    CHECK((raw - 1u < nspire_os_watch_at.value) == (counts >= 2 && counts <= 3),
          "tick reload: raw %u (counts %u)", (unsigned)raw, (unsigned)counts);
    if (failures)
      break;
  }
}

static void case_tick_cx(void)
{
  /* a CX's 2.98 ms tick on a 32768 Hz count (load 97): read if its wakes say so */
  tick_timer(97, 1, 0xe2, 1, 98 << 8);
  CHECK(nspire_os_tick_probe() == 1, "tick cx: the timer read (%s)", "load 97, a count a tick");
}

static void case_tick_refused(void)
{
  struct { const char *why; u32 load, cnt, control; int at_zero; u32 tick256, shake, stuck, raw_set; } c[] = {
    { "its control isn't on, periodic and interrupting", 20, 16, 0xa6, 1, 336 << 8, 0, 0, 0 },
    { "its control isn't on, periodic and interrupting", 20, 16, 0x66, 1, 336 << 8, 0, 0, 0 },
    { "its load is out of range", 2, 16, 0xe6, 1, 48 << 8, 0, 0, 0 },
    { "load + 1 of its counts isn't the tick the wakes measured", 20, 16, 0xe6, 1, 300 << 8, 0, 0, 0 },
    { "load + 1 of its counts isn't the tick the wakes measured", 97, 1, 0xe6, 1, 336 << 8, 0, 0, 0 },
    { "its interrupt didn't come within 1.5 ticks", 20, 16, 0xe6, 1, 336 << 8, 0, 1, 0 },
    { "its counts' steps aren't steady", 20, 16, 0xe6, 1, 336 << 8, 9, 0, 0 },
    { "its interrupt was raised at the start", 20, 16, 0xe6, 1, 336 << 8, 0, 0, 1 },
  };
  char text[400];
  for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++)
  {
    tick_timer(c[i].load, c[i].cnt, c[i].control, c[i].at_zero, c[i].tick256);
    sim.shake = c[i].shake;
    sim.stuck = c[i].stuck;
    if (c[i].raw_set)
      sim.t0 = 0;                      /* (a tick before the start, not cleared) */
    CHECK(nspire_os_tick_probe() == 0 && !nspire_os_tick_timer(), "tick refused %u: read anyway", i);
    nspire_os_watch_arm(2);
    CHECK(nspire_os_watch_at.reg != os_test_reg_ptr(TT_VALUE) && *nspire_os_watch_at.reg - 1u >= nspire_os_watch_at.value
          && !nspire_os_tick_need(15), "tick refused %u: the lines never call out", i);
    nspire_os_tick_rule_report(text, sizeof(text));
    CHECK(strstr(text, "r15's rule") && strstr(text, c[i].why), "tick refused %u: the log says\n%s\nnot\n%s", i,
          text, c[i].why);
  }
  /* not probed (no sleep yet): r15's rule, said so */
}

static void case_tick_unprobed(void)
{
  char text[400];
  uint32_t in, period;
  calculator(1);
  nspire_os_tick_rule_report(text, sizeof(text));
  CHECK(strstr(text, "not probed") && strstr(text, "r15's rule"), "tick unprobed: %s", text);
  nspire_os_watch_arm(2);
  CHECK(!nspire_os_tick_timer() && *nspire_os_watch_at.reg - 1u >= nspire_os_watch_at.value, "tick unprobed: not read");
  (void)nspire_os_next_tick_in(&in, &period);
}

/* (r19-usb-exit) The quit's hold (nspire_drive_stop_quiet: nothing ever on
 * the port's host end): a sleep returns 0 at once and is counted, before
 * anything else: no look for the OS's sleep (its scan of 16 MB of the
 * OS's code: ~0.5 s on a CX), no register touched; let go, a sleep is what
 * it was (here: no OS to sleep into, refused as before the hold). */
static void case_sleep_hold(void)
{
  int before;
  calculator(1);
  before = nregs;
  nspire_os_sleep_hold(1);
  CHECK(nspire_os_sleep() == 0 && nspire_os_sleep() == 0 && nspire_os_sleeps_held() == 2,
        "hold: two sleeps held, counted: %u", (unsigned)nspire_os_sleeps_held());
  CHECK(nregs == before && nspire_os_ready_cost(NULL) == 0 && nspire_os_sleeps() == 0,
        "hold: nothing looked at (%d registers, the look's cost %u, %u sleeps)", nregs - before,
        (unsigned)nspire_os_ready_cost(NULL), (unsigned)nspire_os_sleeps());
  nspire_os_sleep_hold(0);
  nspire_os_hand_back();
  CHECK(nspire_os_sleep() == 0 && nspire_os_sleeps_held() == 2, "hold let go: the sleep not held (refused after "
        "the hand-back, as ever): %u held", (unsigned)nspire_os_sleeps_held());
}

int main(void)
{
  run("sleep_hold", case_sleep_hold);
  run("tick_cx2", case_tick_cx2);
  run("tick_reload", case_tick_reload);
  run("tick_cx", case_tick_cx);
  run("tick_refused", case_tick_refused);
  run("tick_unprobed", case_tick_unprobed);
  run("quit6_irq_lines", case_quit6_irq_lines);
  run("cx2", case_cx2);
  run("cx", case_cx);
  run("r11w", case_r11w);
  run("r13_wdog", case_r13_wdog_fallback);
  printf("os_tests (nspire_os.c's exit code on a register file): %s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
