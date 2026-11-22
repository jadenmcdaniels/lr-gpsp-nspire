/* The CPU speed on the CX II and the older CX, taken from PocketSNES for
 * the Nspire.
 *
 * CX II:
 * The power controller (0x90140000) holds the clock multiplier of the 12 MHz
 * base clock in bits 24-29 of 0x90140030 (the OS sets 33, 396 MHz, or 24,
 * 288 MHz while USB is plugged in); memory speeds up with it. Writing the
 * register alone changes nothing. The OS's own switch puts normal memory to
 * sleep (self-refresh), then for each multiplier step writes the register,
 * writes 0x10000100 to power controller register 0x20 and waits for its
 * interrupt, then wakes the memory. nspire/clock_switch_nspire.S does the
 * same from the on-chip SRAM, because normal memory sleeps meanwhile; that
 * is also why the LCD reads the OS's own buffer (on-chip memory) during a
 * switch. Every switch is checked by timing a loop against the 32 kHz timer.
 *
 * The older CX (132 MHz) has its clock setting in one register of its power
 * controller (0x900B0000), as PocketSNES's e487fb2 found it: see "The older
 * CX's clock" below.
 *
 * Too high a speed can freeze the calculator, so while a game runs at a
 * raised speed, a marker file (gpsp_clock.tns, next to gpSP) names the speed
 * and the game; leaving cleanly removes it, and if it is still there at the
 * next start that speed is put back to normal. */

#include "common.h"
#include "nspire.h"
#include "nspire_clock.h"
#include "nspire_paths.h"
#include "nspire_frontend.h"

#include <stdio.h>
#include <string.h>

#ifndef NSPIRE_CLOCK_FAKE_HW
#define CLOCK_CONTROL  (*(volatile u32 *)0x90140030)
#endif
#define PMU_COMMAND    (*(volatile u32 *)0x90140020)   /* only read here */
#define MEMC_STATE     (*(volatile u32 *)0x90120004)   /* bit 10: normal memory sleeps */
#define CLOCK_MULTIPLIER(value) ((int)(((value) >> 24) & 0x3F))
#define LCD_BASE       (*(volatile u32 *)0xC0000010)
#define LCD_CURRENT    (*(volatile u32 *)0xC000002C)   /* where the LCD reads now */

/* The second SP804 dual timer, at 32768 Hz: the switch gives up on a step
 * that doesn't finish when it runs out. It belongs to the OS (and Ndless's
 * msleep), so it is put back afterwards. */
#define T2_LOAD    (*(volatile u32 *)0x900D0000)
#define T2_CONTROL (*(volatile u32 *)0x900D0008)
#define T2_CLEAR   (*(volatile u32 *)0x900D000C)
#define T2_SPEED   (*(volatile u32 *)0x900D0080)

/* Where the switch runs in the SRAM: past the OS's code (0xA4000000) and
 * the MMU's page table (0xA4004000-0xA4007FFF), in 1 KB that was zero in
 * every dump. What's there is put back after each switch. */
#define SWITCH_SRAM       ((u8 *)0xA4035000)
#define SWITCH_STEP_TICKS 1092   /* a step may take 1/30 s */
extern const u8 clock_switch_code[], clock_switch_code_end[];

/* The OS's LCD buffer is on-chip memory there. */
#define ON_CHIP_LCD_START 0xA8000000u
#define ON_CHIP_LCD_END   0xA8040000u

/* fast17: not a .tns (the OS's file list never shows it: its removal after
 * a crash doesn't make the exit read that list again, 6.6 s on the CX II);
 * r16 and older named it gpsp_clock.tns: still read, and removed, once. */
#define CLOCK_MARKER "gpsp_clock.run"
#define CLOCK_MARKER_OLD "gpsp_clock.tns"

/* CX II: of the 12 MHz clock (408-504 MHz). Older CX: of 6 MHz (168-264).
 * The last is auto (CPU_SPEED_AUTO): normal on a CX II, 192 MHz on a CX. */
static const int multipliers[CPU_SPEEDS] = { 0, 34, 35, 36, 37, 38, 39, 40, 41, 42, 0 };
static const int cx_multipliers[CPU_SPEEDS] = { 0, 28, 30, 32, 34, 36, 38, 40, 42, 44, 32 };

char nspire_clock_crashed_rom[512];
static int crashed;               /* a raised speed froze the calculator last time */

static u32 os_clock_control;      /* the OS's setting, put back after a raised one */
static int clock_raised;
static u32 clock_mhz;             /* measured after the last switch, 0 if none */
static int failed_multiplier;     /* a raise that didn't happen isn't tried again */
static int shown_multiplier = -1; /* the speed last shown in a message */
static int marker_written;
static int marker_multiplier;     /* what the marker holds */
static u32 marker_game;           /* path_hash of the game it names */
static int lower_failed;          /* the OS's speed wasn't put back (lower_clock) */

static struct
{
  u32 before, target, after, result, ticks, mhz;
  const char *refused;   /* why it wasn't tried, or NULL */
} last_switch;

#ifndef NSPIRE_CLOCK_FAKE_HW
static u32 interrupts_off(void)
{
  u32 old, off;
  __asm__ volatile ("mrs %0, cpsr\n\torr %1, %0, #0xC0\n\tmsr cpsr_c, %1"
                    : "=r" (old), "=r" (off) : : "memory");
  return old;
}

static void interrupts_restore(u32 old)
{
  __asm__ volatile ("msr cpsr_c, %0" : : "r" (old) : "memory");
}

/* The CPU clock in MHz (rounded), from a loop of 4 * 'loops' cycles timed
 * with the 32 kHz timer: on the ARM926EJ-S, SUBS takes 1 cycle and a taken
 * branch 3, and the loop runs from the instruction cache, so memory speed
 * doesn't come into it. */
static u32 measure_mhz(u32 loops)
{
  u64 cycles = (u64)loops * 4;
  u32 cpsr = interrupts_off();
  u32 start = nspire_ticks(), ticks;
  __asm__ volatile ("1: subs %0, %0, #1\n\tbne 1b" : "+r" (loops) : : "cc");
  ticks = nspire_ticks() - start;
  interrupts_restore(cpsr);
  if (ticks == 0)
    return 0;
  return (u32)((cycles * NSPIRE_TICK_HZ + ticks * 500000ull) / ticks / 1000000);
}
#endif

#define QUICK_LOOPS 4000000   /* 16 million cycles: about 40 ms, to within 0.1% */

/* Within 2% of 12 MHz (CX II) or 6 MHz (older CX) times the multiplier. */
static int mhz_near(u32 mhz, u32 want)
{
  return mhz * 50 >= want * 49 && mhz * 50 <= want * 51;
}

static int clock_matches(u32 mhz, int multiplier)
{
  return mhz_near(mhz, (u32)multiplier * (is_cx2 ? 12 : 6));
}

/* The PC's unit test (tests/host/frontend_tests.c) has its own switches. */
#ifdef NSPIRE_CLOCK_FAKE_HW
static u32 switch_clock(u32 value);
#else
static void sync_code(void)
{
  __asm__ volatile ("mcr p15, 0, %0, c7, c10, 4\n\t"   /* drain the write buffer */
                    "mcr p15, 0, %0, c7, c5, 0"        /* invalidate the instruction cache */
                    : : "r" (0) : "memory");
}

/* Points the LCD at the OS's buffer, with what is on the screen copied there
 * first, and waits until it reads from there. Returns 0 if it can't (then
 * normal memory mustn't sleep). */
static int lcd_to_on_chip(void)
{
  u32 os = (u32)os_screen, since;
  int polls;

  if (os < ON_CHIP_LCD_START || os >= ON_CHIP_LCD_END)
    return 0;
  if (LCD_BASE != os)
  {
    /* The OS's buffer turns pixels as they are written into it: a landscape
     * picture shows upright. gpSP's buffers on this LCD hold turned
     * pictures, so the one on screen is turned back as it is copied. */
    const u16 *shown = (const u16 *)LCD_BASE;
    volatile u16 *dst = (volatile u16 *)os;
    u32 x, p, line;
    if (nspire_lcd_portrait)
    {
      for (x = 0; x < 320; x++)
        for (p = 0; p < 240; p++)
          dst[(239 - p) * 320 + x] = shown[x * 240 + p];
      /* Reading the buffer brought it into the data cache; frames written
       * into it later must go to memory, where the LCD reads them. */
      for (line = (u32)shown & ~31u; line < (u32)shown + 320 * 240 * 2; line += 32)
        __asm__ volatile ("mcr p15, 0, %0, c7, c14, 1" : : "r" (line));
    }
    __asm__ volatile ("mcr p15, 0, %0, c7, c10, 4" : : "r" (0) : "memory");
    LCD_BASE = os;
  }
  since = nspire_ticks();
  for (polls = 0; polls < 4000000; polls++)
  {
    u32 reading = LCD_CURRENT;
    if (reading >= os && reading < os + 320 * 240 * 2)
      return 1;
    if (nspire_ticks() - since > NSPIRE_TICK_HZ / 10)
      break;
  }
  return 0;
}

/* Switches the clock register to 'value' (only its multiplier may differ
 * from the register's now) the OS's way. Returns the measured clock in MHz,
 * 0 if it can't be measured. */
static u32 switch_clock(u32 value)
{
  static u8 saved[1024];
  u32 size = (u32)(clock_switch_code_end - clock_switch_code);
  u32 now = CLOCK_CONTROL, lcd = LCD_BASE, mhz;
  int running;

  if (now == value)
    return measure_mhz(QUICK_LOOPS);
  last_switch.before = now;
  last_switch.target = value;
  last_switch.result = last_switch.ticks = 0;
  last_switch.refused = NULL;
  if (size > sizeof(saved))
    last_switch.refused = "switch code too big";
  else if ((now & ~(0x3Fu << 24)) != (value & ~(0x3Fu << 24)) || (now & 0xFF) != 0x03)
    last_switch.refused = "clock register in an unknown mode";
  else if ((PMU_COMMAND & ~0x100u) != 0x10000000)
    last_switch.refused = "power controller in an unknown state";
  else if (MEMC_STATE & 0x400)
    last_switch.refused = "memory asleep";
  else if (!lcd_to_on_chip())
    last_switch.refused = "LCD reads normal memory";

  if (!last_switch.refused)
  {
    u32 cpsr = interrupts_off();
    u32 control = T2_CONTROL, load = T2_LOAD, speed = T2_SPEED, start;
    T2_CONTROL = 0;
    T2_SPEED = 0xA;   /* 32768 Hz, like the first timer */
    memcpy(saved, SWITCH_SRAM, size);
    memcpy(SWITCH_SRAM, clock_switch_code, size);
    sync_code();
    start = nspire_ticks();
    last_switch.result = ((u32 (*)(u32, u32))SWITCH_SRAM)(value, SWITCH_STEP_TICKS);
    last_switch.ticks = nspire_ticks() - start;
    memcpy(SWITCH_SRAM, saved, size);
    sync_code();
    T2_CONTROL = 0;
    T2_CLEAR = 1;
    T2_SPEED = speed;
    T2_CONTROL = control & 0x7F;
    T2_LOAD = load;
    T2_CONTROL = control;
    interrupts_restore(cpsr);
  }
  LCD_BASE = lcd;

  mhz = measure_mhz(QUICK_LOOPS);
  /* A step that didn't happen leaves the register ahead of the clock: it is
   * set to the clock the CPU runs at, so the OS's own switches (when it
   * idles) start from the truth. */
  running = (int)((mhz + 6) / 12);
  if (mhz && !clock_matches(mhz, CLOCK_MULTIPLIER(CLOCK_CONTROL)) && clock_matches(mhz, running))
    CLOCK_CONTROL = (CLOCK_CONTROL & ~(0x3Fu << 24)) | (u32)running << 24;
  last_switch.after = CLOCK_CONTROL;
  last_switch.mhz = mhz;
  /* R12: the timer counts at the OS's clock select now, which may follow
   * the CPU's: timed again (nspire.c; logged) */
  if (!last_switch.refused)
    nspire_timer_recheck();
  return mhz;
}

u32 nspire_cx2_timer_hz(u32 value_reg, u32 top)
{
  u32 mhz = (u32)CLOCK_MULTIPLIER(CLOCK_CONTROL) * 12, loops, cpsr, v0, v1, counts;

  if (!is_cx2 || mhz < 24 || mhz > 600)
    return 0;
  /* 4 cycles a turn (SUBS 1, a taken branch 3, from the instruction cache,
   * as measure_mhz): 20 ms at the clock the register says */
  loops = mhz * 5000;
  cpsr = interrupts_off();
  v0 = *(volatile u32 *)value_reg;
  __asm__ volatile ("1: subs %0, %0, #1\n\tbne 1b" : "+r" (loops) : : "cc");
  v1 = *(volatile u32 *)value_reg;
  interrupts_restore(cpsr);
  counts = v0 >= v1 ? v0 - v1 : v0 + 1 + (top - v1);
  return counts && counts < 0xFFFFFFFFu / 50 ? counts * 50 : 0;
}
#endif

/* ---- The older CX's clock ----
 *
 * Its power controller (0x900B0000) has the clock setting in one register,
 * decoded as Linux's clk-nspire.c does: the base clock is 6 MHz times bits
 * 15-20 (48 MHz if bit 8 is set), the CPU's is the base divided by bits 1-7
 * times bits 21-22, the memory bus's (AHB) is the CPU's divided by bits
 * 12-14 plus 1, and the APB's is half that. Writing 4 to 0x900B000C starts a
 * switch to the value written to 0x900B0000; it happens while the CPU waits
 * for an interrupt, and the power controller flags it done (0x900B0014 bit
 * 1). 0x900B0024 reads the setting in use. nLaunchy (by Nover's author)
 * writes 0x0030A002: base 198 MHz, CPU 198, bus 66, the calculator's own
 * bus speed; others reported flash corruption with the bus at 72 MHz. So a
 * raised speed is the base clock itself with the bus divided to at most
 * 66 MHz. The switch waits as Ndless's msleep does on a CX, with only the
 * power controller's and the second timer's interrupts (15, and 19 as a
 * timeout) able to wake the CPU. As in PocketSNES, not yet run on a CX. */
#define CX_CLOCK_LOAD  (*(volatile u32 *)0x900B0000)
#define CX_CLOCK_APPLY (*(volatile u32 *)0x900B000C)
#define CX_POWER_FLAGS (*(volatile u32 *)0x900B0014)
#ifndef NSPIRE_CLOCK_FAKE_HW
#define CX_CLOCK_NOW   (*(volatile u32 *)0x900B0024)
#endif
#define VIC_ENABLED    (*(volatile u32 *)0xDC000010)
#define VIC_DISABLE    (*(volatile u32 *)0xDC000014)
#define T2_RAW         (*(volatile u32 *)0x900D0010)
#define CX_FIELDS      (0xFEu | 0x100u | (7u << 12) | (0x3Fu << 15) | (3u << 21))
#define CX_BUS_MAX_MHZ 66
#define CX_TIMEOUT_TICKS 3277   /* 0.1 s */

static u32 cx_cpu_mhz(u32 value)
{
  u32 base = (value & 0x100) ? 48 : 6 * ((value >> 15) & 0x3F);
  u32 ratio = ((value >> 1) & 0x7F) * ((value >> 21) & 3);
  return ratio ? base / ratio : 0;
}

static u32 cx_bus_mhz(u32 value)
{
  return cx_cpu_mhz(value) / (((value >> 12) & 7) + 1);
}

/* The setting the OS left: an older CX (not a CX II, not a monochrome
 * model) at 60 to 300 MHz with the bus at most at 66 MHz, or 0 if it's
 * something else (then the clock is left alone). */
static u32 cx_os_setting(void)
{
  u32 value, mhz;

  if (is_cx2 || is_classic)
    return 0;
  value = clock_raised ? os_clock_control : CX_CLOCK_NOW;
  mhz = cx_cpu_mhz(value);
  if ((value & 0x100) || mhz < 60 || mhz > 300 || cx_bus_mhz(value) > CX_BUS_MAX_MHZ)
    return 0;
  return value;
}

/* The setting for 6 MHz times 'multiplier', from the OS's. */
static u32 cx_setting(u32 os, int multiplier)
{
  u32 mhz = (u32)multiplier * 6;
  u32 divider = (mhz + CX_BUS_MAX_MHZ - 1) / CX_BUS_MAX_MHZ;
  return (os & ~CX_FIELDS) | (u32)multiplier << 15 | 1u << 21 | (divider - 1) << 12 | 1u << 1;
}

#ifdef NSPIRE_CLOCK_FAKE_HW
static u32 cx_switch_clock(u32 value);
#else
static u32 cx_switch_clock(u32 value)
{
  u32 now = CX_CLOCK_NOW, mhz;

  if (now == value)
    return measure_mhz(QUICK_LOOPS);
  last_switch.before = now;
  last_switch.target = value;
  last_switch.result = last_switch.ticks = 0;
  last_switch.refused = NULL;
  if (!cx_cpu_mhz(value) || cx_bus_mhz(value) > CX_BUS_MAX_MHZ || (value & 0x100))
    last_switch.refused = "setting out of range";
  else if (CX_POWER_FLAGS & 2)
    last_switch.refused = "power controller busy";

  if (!last_switch.refused)
  {
    u32 cpsr = interrupts_off();
    u32 enabled = VIC_ENABLED;
    u32 control = T2_CONTROL, load = T2_LOAD, speed = T2_SPEED, start, waits = 0;
    __asm__ volatile ("mcr p15, 0, %0, c7, c10, 4" : : "r" (0) : "memory");   /* drain the write buffer */
    /* The timeout: the second timer counts down once. */
    T2_CONTROL = 0;
    T2_CLEAR = 1;
    T2_SPEED = 0xA;
    T2_LOAD = CX_TIMEOUT_TICKS;
    T2_CONTROL = 0x63;   /* one-shot, 32-bit, interrupt, not running yet */
    VIC_DISABLE = 0xFFFFFFFF;
    VIC_ENABLED = (1u << 15) | (1u << 19);
    start = nspire_ticks();
    T2_CONTROL = 0xE3;
    CX_CLOCK_LOAD = value;
    CX_CLOCK_APPLY = 4;
    while (!(CX_POWER_FLAGS & 2) && !(T2_RAW & 1) && waits < 64)
    {
      __asm__ volatile ("mcr p15, 0, %0, c7, c0, 4" : : "r" (0) : "memory");   /* wait for interrupt */
      waits++;
    }
    last_switch.ticks = nspire_ticks() - start;
    last_switch.result = waits | (CX_POWER_FLAGS & 2 ? 0x100 : 0) | (T2_RAW & 1 ? 0x200 : 0);
    CX_POWER_FLAGS = 2;   /* done: acknowledged */
    T2_CONTROL = 0;
    T2_CLEAR = 1;
    T2_SPEED = speed;
    T2_CONTROL = control & 0x7F;
    T2_LOAD = load;
    T2_CONTROL = control;
    VIC_DISABLE = 0xFFFFFFFF;
    VIC_ENABLED = enabled;
    interrupts_restore(cpsr);
    /* If the timer's speed follows the bus, it changed: time it again
     * (nspire_tickrate.h; it carries on from where it is). */
    nspire_timer_recheck();
  }
  mhz = measure_mhz(QUICK_LOOPS);
  last_switch.after = CX_CLOCK_NOW;
  last_switch.mhz = mhz;
  return mhz;
}

u32 nspire_cx_apb_hz(void)
{
  u32 value = CX_CLOCK_NOW, bus = cx_bus_mhz(value);

  if (is_cx2 || is_classic || (value & 0x100) || !bus || bus > CX_BUS_MAX_MHZ)
    return 0;
  return bus * 1000000u / 2;
}

u32 nspire_cx_timer_hz(u32 value_reg, int any_rate)
{
  static const u32 rates[] = { 2048, 4096, 8192, 16384, 32768, 65536, 131072 };
  u32 value = CX_CLOCK_NOW, mhz = cx_cpu_mhz(value), loops, cpsr, start, ticks, hz, i;

  if (is_cx2 || is_classic || (value & 0x100) || mhz < 60 || mhz > 300)
    return 0;
  /* 4 cycles a turn (SUBS 1, a taken branch 3, from the instruction cache):
   * 0.2 s at the clock the register says. */
  loops = mhz * 50000;
  cpsr = interrupts_off();
  start = ~*(volatile u32 *)value_reg;   /* raw: the timer counts down */
  __asm__ volatile ("1: subs %0, %0, #1\n\tbne 1b" : "+r" (loops) : : "cc");
  ticks = ~*(volatile u32 *)value_reg - start;
  interrupts_restore(cpsr);
  hz = ticks * 5;
  /* The first timer's 32 kHz selects: only a speed near a power of two of
   * 32768 (the crystal divided) is taken, anything else means the register
   * wasn't read right. Another timer's clock isn't known (the fast timer's
   * may be the bus's): any speed a free-running count can have. */
  if (any_rate)
    return hz >= 1000 && hz <= 100000000 ? hz : 0;
  for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
    if (hz * 100 >= rates[i] * 97 && hz * 100 <= rates[i] * 103)
      return hz;
  return 0;
}

#endif
/* Puts the OS's setting back with 'sw' (switch_clock or cx_switch_clock;
 * 'reg' its register), tried twice. The speed counts as raised (and the
 * freeze marker stays) until the CPU is measured at 'want_mhz', or, if it
 * can't be measured, until a switch went through: a refused or cut-off
 * lowering (switch_clock's checks, a step timed out) mustn't leave gpSP
 * with the CPU raised and no marker. */
static void lower_clock(u32 (*sw)(u32), u32 want_mhz, const volatile u32 *reg)
{
  int tries;

  for (tries = 0; tries < 2 && clock_raised; tries++)
  {
    int switching = *reg != os_clock_control;
    clock_mhz = sw(os_clock_control);
    if (clock_mhz ? mhz_near(clock_mhz, want_mhz) :
        !switching || (!last_switch.refused && !(last_switch.result & 0xA00) && *reg == os_clock_control))
      clock_raised = 0;
  }
  lower_failed = clock_raised;
}

static u32 cx_set_multiplier(int multiplier)
{
  u32 os = cx_os_setting(), value;

  if (!os || multiplier < 0 || multiplier > 46)   /* 276 MHz */
    return 0;
  if (!multiplier)
  {
    lower_clock(cx_switch_clock, cx_cpu_mhz(os_clock_control), &CX_CLOCK_NOW);
    return clock_mhz ? clock_mhz : cx_cpu_mhz(CX_CLOCK_NOW);
  }
  if (!clock_raised)
    os_clock_control = os;
  value = cx_setting(os, multiplier);
  if (!clock_raised || value != CX_CLOCK_NOW)
    clock_mhz = cx_switch_clock(value);
  clock_raised = 1;
  return clock_mhz ? clock_mhz : cx_cpu_mhz(CX_CLOCK_NOW);
}

static u32 clock_now_mhz(void)
{
  return clock_mhz ? clock_mhz : (u32)CLOCK_MULTIPLIER(CLOCK_CONTROL) * 12;
}

/* 0 puts back the calculator's own speed. Returns the clock in MHz. */
static u32 set_multiplier(int multiplier)
{
  u32 value;

  if (!is_cx2)
    return cx_set_multiplier(multiplier);
  if (multiplier < 0 || multiplier > 63)
    return 0;
  if (!multiplier)
  {
    lower_clock(switch_clock, (u32)CLOCK_MULTIPLIER(os_clock_control) * 12, &CLOCK_CONTROL);
    return clock_now_mhz();
  }
  if (!clock_raised)
    os_clock_control = CLOCK_CONTROL;
  value = (os_clock_control & ~(0x3Fu << 24)) | (u32)multiplier << 24;
  if (!clock_raised || value != CLOCK_CONTROL)
    clock_mhz = switch_clock(value);
  clock_raised = 1;
  return clock_now_mhz();
}

static void marker_path(char *path, u32 size)
{
  nspire_save_file(path, size, CLOCK_MARKER);
}

static void remove_marker(void)
{
  char path[600];

  if (!marker_written)
    return;
  marker_path(path, sizeof(path));
  remove(path);
  marker_written = 0;
}

/* FNV-1a: which game the marker names. */
static u32 path_hash(const char *path)
{
  u32 hash = 2166136261u;

  while (*path)
    hash = (hash ^ (u8)*path++) * 16777619u;
  return hash;
}

int nspire_clock_check_crash(void)
{
  char path[600], line[sizeof(nspire_clock_crashed_rom)];
  FILE *f;

  /* fast17: the saves folder's names read at start say if it's there: no
   * file open at every start (~5-50 ms on the CX II); an older build's
   * name when only that is there */
  f = NULL;
  if (nspire_save_folder_had(CLOCK_MARKER))
  {
    marker_path(path, sizeof(path));
    f = fopen(path, "r");
  }
  if (!f && nspire_save_folder_had(CLOCK_MARKER_OLD))
  {
    nspire_save_file(path, sizeof(path), CLOCK_MARKER_OLD);
    f = fopen(path, "r");
  }
  if (!f)
    return 0;
  line[0] = 0;
  if (fgets(line, sizeof(line), f) && fgets(line, sizeof(line), f))
    line[strcspn(line, "\r\n")] = 0;
  else
    line[0] = 0;
  fclose(f);
  remove(path);
  snprintf(nspire_clock_crashed_rom, sizeof(nspire_clock_crashed_rom), "%s", line);
  nspire_cpu_speed = 0;   /* normal, not auto: saved as chosen (nspire_config.c) */
  crashed = 1;
  return 1;
}

void nspire_clock_game(void)
{
  int multiplier = nspire_cpu_speed < CPU_SPEEDS ?
                   (is_cx2 ? multipliers : cx_multipliers)[nspire_cpu_speed] : 0;
  u32 mhz;

  if (crashed)
  {
    crashed = 0;
    nspire_osd_message("A raised CPU speed froze it last time: back to normal");
    shown_multiplier = 0;
  }
  if (!is_cx2 && !cx_os_setting())
    return;   /* an older CX whose clock setting isn't what's expected */
  if (multiplier != failed_multiplier)
    failed_multiplier = 0;
  if (multiplier && multiplier == failed_multiplier)
    multiplier = 0;

  /* The marker names this game and speed: rewritten when either changed
   * (another game from the menu, the speed changed in it). */
  if (multiplier && (!marker_written || multiplier != marker_multiplier || path_hash(gamepak_filename) != marker_game))
  {
    char path[600];
    FILE *f;
    marker_path(path, sizeof(path));
    f = fopen(path, "w");
    if (!f)
      multiplier = 0;   /* no marker, no raised speed */
    else
    {
      fprintf(f, "%d\n%s\n", multiplier, gamepak_filename);
      fclose(f);
      marker_written = 1;
      marker_multiplier = multiplier;
      marker_game = path_hash(gamepak_filename);
    }
  }

  mhz = set_multiplier(multiplier);
  if (multiplier && mhz && !clock_matches(mhz, multiplier))
  {
    /* The clock didn't get there: back to normal, and say so. */
    failed_multiplier = multiplier;
    mhz = set_multiplier(0);
    nspire_osd_message("CPU speed not raised (still %u MHz)", (unsigned)mhz);
  }
  else if (multiplier != shown_multiplier && (multiplier || shown_multiplier > 0) && mhz)
    nspire_osd_message(multiplier ? "CPU %u MHz" : "CPU %u MHz (normal)", (unsigned)mhz);
  shown_multiplier = multiplier;
  if (!clock_raised)
    remove_marker();   /* a game at normal speed, or the raise didn't happen */
}

void nspire_clock_normal(void)
{
  set_multiplier(0);
}

void nspire_clock_left_game(void)
{
  set_multiplier(0);
  if (!clock_raised)
    remove_marker();
  else if (marker_written)
  {
    /* Still raised: the marker stays (the next start puts the speed back
     * to normal), with why after its two lines. */
    char path[600], line[300];
    FILE *f;
    marker_path(path, sizeof(path));
    f = fopen(path, "a");
    if (f)
    {
      nspire_clock_report(line, sizeof(line));
      fprintf(f, "%s\n", line);
      fclose(f);
    }
  }
}

#ifndef NSPIRE_CLOCK_FAKE_HW

u32 nspire_cpu_mhz(void)
{
  return measure_mhz(20000000);   /* 80 million cycles */
}
#endif

static void clock_report(char *text, u32 size);

void nspire_clock_report(char *text, u32 size)
{
  u32 len;

  clock_report(text, size);
  len = (u32)strlen(text);
  if (lower_failed)
    snprintf(text + len, size - len, "; the OS's speed not put back");
}

static void clock_report(char *text, u32 size)
{
  u32 len;

  if (!is_cx2)
  {
    u32 now = CX_CLOCK_NOW;
    snprintf(text, size, "CX clock register %08lx (cpu %lu MHz, bus %lu MHz)%s", (unsigned long)now,
             (unsigned long)cx_cpu_mhz(now), (unsigned long)cx_bus_mhz(now),
             cx_os_setting() ? "" : ", not set here (unknown setting)");
    if (!last_switch.target)
      return;
    len = (u32)strlen(text);
    if (last_switch.refused)
      snprintf(text + len, size - len, "; last switch %08lx -> %08lx not tried: %s",
               (unsigned long)last_switch.before, (unsigned long)last_switch.target, last_switch.refused);
    else
      snprintf(text + len, size - len, "; last switch %08lx -> %08lx: %lu waits%s%s, register %08lx, %lu MHz",
               (unsigned long)last_switch.before, (unsigned long)last_switch.target,
               (unsigned long)(last_switch.result & 0xFF),
               last_switch.result & 0x100 ? ", done" : ", no done flag",
               last_switch.result & 0x200 ? ", timed out" : "",
               (unsigned long)last_switch.after, (unsigned long)last_switch.mhz);
    return;
  }
  snprintf(text, size, "clock register %08lx (x%d)", (unsigned long)CLOCK_CONTROL,
           CLOCK_MULTIPLIER(CLOCK_CONTROL));
  if (!last_switch.target)
    return;
  len = (u32)strlen(text);
  if (last_switch.refused)
    snprintf(text + len, size - len, "; last switch %08lx -> %08lx not tried: %s, %lu MHz",
             (unsigned long)last_switch.before, (unsigned long)last_switch.target,
             last_switch.refused, (unsigned long)last_switch.mhz);
  else
    snprintf(text + len, size - len,
             "; last switch %08lx -> %08lx: %lu steps%s%s%s%s, register %08lx, %lu MHz",
             (unsigned long)last_switch.before, (unsigned long)last_switch.target,
             (unsigned long)(last_switch.result & 0xFF),
             last_switch.result & 0x100 ? ", memory didn't sleep" : "",
             last_switch.result & 0x200 ? ", a step timed out" : "",
             last_switch.result & 0x400 ? ", memory didn't wake" : "",
             last_switch.result & 0x800 ? ", power interrupt stuck" : "",
             (unsigned long)last_switch.after, (unsigned long)last_switch.mhz);
}
