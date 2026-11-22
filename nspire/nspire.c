#include "common.h"
#include "nspire_hw.h"
#include "nspire_paths.h"
#include "nspire_tickrate.h"
#include "nspire_clock.h"
#include "nspire_os.h"
#include "nspire_exit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Three screen buffers take turns: one is shown, one may still be waiting to
 * be shown at the LCD's next refresh, and the next frame is drawn into the
 * third (nspire_screen). */
#define SCREEN_PIXELS (320 * 240)
#define SCREENS 3
static u16 screens[SCREENS][SCREEN_PIXELS] __attribute__ ((aligned (32)));
static int drawing_screen;
/* Whether the LCD has started reading the buffer nspire_flip gave it last. */
static int flip_taken = 1;
/* Flips since the last nspire_lcd_report, those that found the LCD reading
 * the buffer to draw into next, and those that waited for it in vain. */
static u32 flips, flips_waited, flips_timed_out;

void* nspire_displayed_screen;
void* nspire_screen;
u32 nspire_flip_serial;
u16 nspire_landscape[SCREEN_PIXELS] __attribute__ ((aligned (32)));
u16* os_screen;
u32 LCDTiming0;
u32 LCDTiming1;
u32 LCDControl;

int nspire_lcd_portrait;

/* 2026-11-16 (in fast forward the CX II drew 53 or 54 frames a second,
 * where 60 were expected): the CX II's LCD refreshed every
 * 18.5 ms, 54 times a second, under the timings Ndless leaves it with
 * (install.c moves TIMING1 from the OS's 0x03780D3F, 447 lines, 23.0 ms,
 * to 0x0720013F, 360 lines, for a column the OS cut off; the logs' "LCD
 * refresh" lines). A line takes 51.4 us in both, so no more than 54
 * pictures a second could be seen: fast forward waits for each refresh (54
 * pictures), and normal play's 60 left 6 a second unseen. While gpSP runs,
 * the LCD runs at the GBA's own 59.73 a second, by its front porches only
 * (the time after a line's pixels and after the last line, which the panel
 * doesn't count; the back porches and sync widths, which place the picture,
 * stay Ndless's): the horizontal front porch to 21 clocks (the timings
 * Ndless's lcd_compat.c found to work on this panel), then the vertical
 * front porch as long as gives a GBA frame by the line's time as measured.
 * Measured again after; a refresh more than 3% off a GBA frame (or not
 * seen) puts the timings back as they were. nspire_restore puts them back at
 * the quit. A refresh already within 1% of it (the PC's model, 60 a second)
 * is left alone. */
#define LCD_TUNE_HFP 0x14u                 /* the horizontal front porch field: 21 clocks */
#define LCD_VFP_MIN 2u                     /* the vertical front porch's least, in lines */
#define LCD_GBA_PERIOD (280896u * 128u)    /* a GBA frame in 16.16 ticks (nspire_frameskip.c FRAME_PERIOD) */
static u32 lcd_period16;                   /* a refresh, measured (16.16 ticks); 0: not measured, 1/60 s */
static u32 lcd_lines = 447;                /* lines a refresh, the porches' included */
static int lcd_tuned;                      /* the timings were changed (nspire_restore puts them back) */
static char lcd_note[160];                 /* what was done, for nspire_lcd_report */

/* gpSP counts time with an SP804 timer running free, set up for 32768 Hz
 * (as in PocketSNES and nSDL); its settings are put back on exit. On the
 * CX II that's the first timer, which counts 32768 a second there. An
 * older CX's first timer counted slower (games ran slow with the FPS
 * counter at 60/60), so there the timer is timed against the CPU clock at
 * start and the real-time clock as gpSP runs, and its count corrected
 * (nspire_tickrate.h). On a CX CAS the first timer didn't run at
 * all, with any clock select (its gpsp_timer.txt.tns, 2026-11-06): no game
 * was held to its speed, and the link's waits all ended at once. So an
 * older CX tries the timers its OS leaves alone, in turn, until one runs.
 *
 * R12 (2026-11-15): on the CX II gpSP no longer writes the block's clock
 * select at all, nor takes the first timer if the OS runs it (before
 * R12, R11t's 32 kHz write slowed the OS's 44-tick USB wait to 44 s). It
 * reads both blocks' timers first (logged), then: "borrow" (the OS runs
 * 900C0000 as a 32-bit count gpSP can follow: its count is followed, nothing
 * written in the block), "own" (the OS doesn't run it: gpSP runs it free
 * from the top, at whatever clock the OS selected, undivided as the OS may
 * read it), or, only if neither can, "legacy" (R11t's way, the block put
 * back as the OS had it for each come-up's burst). In the first two the
 * count's speed is timed against the CPU clock at start and refined by the
 * real-time clock (nspire_tickrate.h tick_base). */
#define TIMER_LOAD(base)    (base)
#define TIMER_VALUE(base)   ((base) + 4)
#define TIMER_CONTROL(base) ((base) + 8)
/* QUIT6 (2026-11-17: the calculator froze on a quit and needed a
 * reset). An SP804 sets its raw interrupt (bit 0 at +0x10)
 * whenever its count reaches zero, whatever its control's bit 5 (interrupt
 * enable) says; only a write to +0x0C clears it; the block's one line (18
 * for 0x900C0000, which the OS keeps enabled for its USB timer at
 * 0x900C0020) is either timer's raw interrupt AND its bit 5 (Firebird
 * misc.c timer_cx_int_check). gpSP runs 0x900C0000 free from 0xFFFFFFFF at
 * 12 MHz with bit 5 off: at 357.9 s the count passes zero and the raw
 * interrupt is set. The exit put the OS's control back (0x20 on every CX II:
 * the reset value, interrupt enabled) with it still set: line 18 raised for
 * good, which the OS's handler (its own timer's) never clears; once Ndless
 * gives the OS its interrupts back the CPU takes that interrupt forever.
 * Every CX II run that reached the hand-back after more than 357.9 s froze
 * there (r13-hub 594 s, another CX II's 577 s), none of the seven shorter
 * (36-351 s; QUIT6's hardware logs). The put-back now clears what gpSP's run
 * raised (unless the OS had it raised when gpSP took the timer). */
#define TIMER_INTCLR(base)  ((base) + 0x0C)
#define TIMER_RIS(base)     ((base) + 0x10)
#define TIMER_MIS(base)     ((base) + 0x14)
/* A dual timer's clock select: ONE register for both of its timers.
 *
 * R11t (2026-11-15): gpSP wrote the whole word (0xA) and so set the other
 * timer's bits too; on a CX II that other timer is the OS's (0x900C0020:
 * control 0xE2, enabled, periodic, its interrupt on, and the block's
 * interrupt line 18 on in every CX II log; off, 0x42, once nothing was on
 * the USB port: an R9F CX II's link log at 173 s).
 * Now each timer changes only its own field and puts only that back.
 *
 * The fields: bits 1:0 the timer at +0x00's, bits 3:2 the one at +0x20's.
 * Hackspire's timers page (tested on a CX CAS HW-AA, one timer of a block):
 * bit 0 a ~10 MHz clock, bit 1 32 kHz (over bit 0), neither 33 MHz. That
 * bits 3:2 are the +0x20 timer's is inferred, not documented: 0xA, the
 * value every Nspire program writes for "32 kHz", is that page's 32 kHz in
 * both fields, and the page found no use for bit 3 on the timer it timed.
 * Firebird models no clock select at all (reads 0, writes ignored). Every
 * other bit is unknown and kept as the OS has it. */
#define TIMER_CLOCK(base)   (((base) & ~0xFFu) + 0x80)
#define TIMER_HALF(base)    (((base) & 0x20u) ? 2u : 0u)       /* the field's shift */
#define TIMER_FIELD(base)   (3u << TIMER_HALF(base))           /* its bits in the select */
#define TIMER_OTHER(base)   ((base) ^ 0x20u)                   /* the block's other timer */
#define CLOCK_32K   0x2u          /* a field's value: Hackspire's 32 kHz */
#define FIRST_TIMER 0x900C0000u
#define KEEP_CLOCK  0xFFFFFFFFu   /* the clock select left as the OS has it */

struct timer_choice
{
	u32 base, clock;   /* clock: the timer's own 2-bit field's value, or KEEP_CLOCK */
	int if_idle;   /* only if the OS isn't running it (its control's bit 7) */
	const char *name;
};

/* An older CX's, in order: the first timer with Hackspire's 32 kHz select
 * (its field 2, tested on a CX CAS; 1 is a ~10 MHz clock), then the OS's
 * own; the fast timer (0x90010000), which no CX OS runs (Firebird, which
 * runs them, stops if one starts it); the second dual timer's other half
 * (0x900D0020): the OS's tick is its first (control 0xE6 on a CX II), so
 * its select is left as it is. Ndless's msleep uses that tick and puts it
 * back. (Before R11t the first timer was tried with the whole word 0xA too:
 * its own field of that is 2, the try before it.) */
static const struct timer_choice cx_timers[] = {
	{ FIRST_TIMER, CLOCK_32K, 0, "first timer" },
	{ FIRST_TIMER, KEEP_CLOCK, 0, "first timer" },
	{ 0x90010000u, KEEP_CLOCK, 1, "fast timer" },
	{ 0x90010000u, CLOCK_32K, 1, "fast timer" },
	{ 0x900D0020u, KEEP_CLOCK, 1, "second timer's other half" },
};
/* A CX II's legacy fallback only (R12: when the OS runs the first timer in
 * a way that can't be followed, or it doesn't count at the OS's select):
 * the first timer at 32 kHz (seen: 32768 a second against the real-time
 * clock, PocketSNES FINDINGS.md), and if its own field alone doesn't start
 * it, as the OS has it (the real-time clock check then times it,
 * nspire_tickrate.h). The block goes back to the OS for each come-up's
 * burst (nspire_timer_lend). */
static const struct timer_choice cx2_timers[] = {
	{ FIRST_TIMER, CLOCK_32K, 0, "first timer" },
	{ FIRST_TIMER, KEEP_CLOCK, 0, "first timer" },
};

static const struct timer_choice *timer_used;   /* NULL: none runs */
static u32 timer_base = FIRST_TIMER;
static u32 timer_prescale;   /* control bits 2-3: 0, 4 (by 16) or 8 (by 256) */
static u32 saved_timer_load, saved_timer_control, saved_timer_clock;
static u32 saved_timer_ris;   /* QUIT6: its raw interrupt when gpSP took it (bit 0) */
static int timer_works;
/* QUIT6: the exit's put-back of gpSP's timer, for the logs: the raw
 * interrupt before it, and the raw and masked one after it. */
static struct { int done; u32 base, raw_before, raw_after, masked_after; } irq_exit;

/* R11t: the clock select as the OS had it and what gpSP did to it, for the
 * logs (nspire_timer_clock_text): at start, the last write made (the timer
 * used, or the last tried), the other timer's count over OTHER_READS reads
 * before and after it; at exit, the put-back. */
#define OTHER_READS 5000      /* ~1-5 ms each way on a calculator: ~40-160 counts at 32 kHz */
static struct
{
	u32 base;                    /* the timer whose field was written; 0: none */
	u32 os, wrote, after;        /* the select: the OS's, gpSP's word, read back */
	int written;                 /* 0: its field already held the value (nothing written) */
	u32 other_control, other_load;
	u32 moves_before, moves_after;
	int other_runs;              /* the other timer ran (control bit 7): counted */
	int exit_done;               /* the put-back ran */
	u32 exit_before, exit_wrote, exit_after;
} clock_log;
/* An older CX: what each timer tried did, for its log. */
static char timer_tried[300];
/* An older CX: its timer's speed by the CPU clock (0 if it couldn't be
 * timed), and the real-time clock and the timer at start, for its log. */
static u32 cx_timer_hz, cx_rtc_start, cx_raw_start;
static u32 fallback_ticks;

/* ---- R12: the CX II's time base (see above) ---- */
enum tb_mode { TB_CX, TB_OWN, TB_BORROW, TB_LEGACY };
static enum tb_mode tb_mode = TB_CX;
static char tb_why[200];
#define TB_BLOCKS 2
static const u32 tb_block[TB_BLOCKS] = { 0x900C0000u, 0x900D0000u };
#define TIMER_BGLOAD(base) ((base) + 0x18)
/* Both blocks' timers as found, before any write (a CX II's only), their
 * selects, and the speed each counted at (0: not running). */
static struct { u32 load, value, control, bgload, ris; } tb_seen[TB_BLOCKS][2];
static u32 tb_select_seen[TB_BLOCKS], tb_hz_seen[TB_BLOCKS][2];
static int tb_seen_read;
static u32 tb_cpu_hz;          /* the first timer by the CPU clock at start */
static struct tick_follow tb_follow;
static int tb_periodic;        /* followed: it reloads from its load (the OS's periodic) */
static u32 tb_rtc_start, tb_rtc_watched;
static u32 tb_os_changed;      /* own: the OS set the first timer up itself (when, s + 1) */
static u32 tb_os_stopped;      /* borrowed: the OS stopped it and gpSP took it (when, s + 1) */
static int tb_taken;           /* gpSP runs the first timer (own): put back at exit */
static u32 tb_switches, tb_switch_hz;   /* CPU clock switches, the CPU loop's timing after the last */
static int tb_r11t;            /* the PC model only: R11t's rule, to show what it did */
#define TB_OWN_CONTROL 0x82u   /* enabled, free running, 32-bit, no interrupt, undivided */
#define TB_MIN_PERIOD_S 4      /* a periodic count followed: longer than twice the real-time clock's +-1 s */
static struct { int done; u32 control, load, select; } tb_exit;
/* Legacy: the block lent back to the OS for a come-up's burst. */
static int lent;
static u32 lent_ticks, lent_rtc, lent_sleeps, lent_last, lends;
#define LENT_TICKS_PER_SLEEP 336   /* a CX II OS sleep: its tick, 10.25 ms */

/* The CX II LCD scans 320 lines of 240 pixels, 16-bit 5:6:5 (PL111 timing 2
 * gives the clocks per line, timing 1 the lines). Not in Ndless's
 * compatibility mode, which needs the build marked --240x320-support true. */
static int nspire_lcd_is_portrait(void)
{
	u32 clocks_per_line = ((HW_READ(HW_LCD_TIMING2) >> 16) & 0x3FF) + 1;
	u32 lines = (HW_READ(HW_LCD_TIMING1) & 0x3FF) + 1;
	u32 bpp = (HW_READ(HW_LCD_CONTROL) >> 1) & 7;
	return clocks_per_line == 240 && lines == 320 && bpp == 6;
}

/* Writes back and drops a range's cached lines. */
static void flush_dcache_range(void *start, u32 size)
{
	uintptr_t addr = (uintptr_t)start & ~(uintptr_t)31, end = (uintptr_t)start + size;
	for (; addr < end; addr += 32)
		HW_FLUSH_DCACHE_LINE(addr);
	HW_DRAIN_WRITE_BUFFER();
}

/* Adds what trying t did to the older CX's log line. */
static void tried(const struct timer_choice *t, const char *what)
{
	size_t n = strlen(timer_tried);
	char select[16];

	if (t->clock == KEEP_CLOCK)
		snprintf(select, sizeof(select), "the OS's");
	else
		snprintf(select, sizeof(select), "%lx", (unsigned long)t->clock);
	if (n < sizeof(timer_tried))
		snprintf(timer_tried + n, sizeof(timer_tried) - n, "%s%s (select %s): %s", n ? "; " : "", t->name,
			 select, what);
}

/* How far the block's other timer counts in OTHER_READS reads of it (its
 * value register: reading it changes nothing). Periodic or free running,
 * 16 or 32 bits: a step back up is a reload from its load. Only reads. */
static u32 other_moves(u32 other, u32 control, u32 load)
{
	u32 mask = (control & 2) ? 0xFFFFFFFFu : 0xFFFFu, prev, cur, moves = 0, i;

	load &= mask;
	prev = HW_READ(TIMER_VALUE(other)) & mask;
	for (i = 0; i < OTHER_READS; i++)
	{
		cur = HW_READ(TIMER_VALUE(other)) & mask;
		if (cur <= prev)
			moves += prev - cur;
		else   /* reloaded: down to 0, then from its load (free running: its top) */
			moves += prev + 1 + (((control & 0x40) ? load : mask) - cur);
		prev = cur;
	}
	return moves;
}

/* The select with the field of the timer at 'base' set to 'field' (its
 * other bits, the other timer's and the unknown ones, as in 'word'). */
static u32 select_with(u32 word, u32 base, u32 field)
{
	return (word & ~TIMER_FIELD(base)) | ((field << TIMER_HALF(base)) & TIMER_FIELD(base));
}

/* Writes the field of the timer at 'base' as timer_try's caller asked,
 * nothing else: the other timer's bits stay as they are now. Logged. */
static void clock_set(u32 base, u32 field)
{
	u32 other = TIMER_OTHER(base), now = HW_READ(TIMER_CLOCK(base));
	u32 want = field == KEEP_CLOCK ? now : select_with(now, base, field);

	clock_log.base = base;
	clock_log.os = now;
	clock_log.wrote = want;
	clock_log.written = want != now;
	clock_log.other_control = HW_READ(TIMER_CONTROL(other));
	clock_log.other_load = HW_READ(TIMER_LOAD(other));
	clock_log.other_runs = (clock_log.other_control & 0x80) != 0;
	clock_log.moves_before = clock_log.moves_after = 0;
	if (clock_log.written && clock_log.other_runs)
		clock_log.moves_before = other_moves(other, clock_log.other_control, clock_log.other_load);
	if (clock_log.written)
		HW_WRITE(TIMER_CLOCK(base), want);
	clock_log.after = HW_READ(TIMER_CLOCK(base));
	if (clock_log.written && clock_log.other_runs)
		clock_log.moves_after = other_moves(other, clock_log.other_control, clock_log.other_load);
}

/* Puts the field of the timer at 'base' back as the OS had it (in
 * saved_timer_clock); the other timer's bits as they are now (the OS may
 * have changed them since). */
static void clock_put_back(u32 base, int at_exit)
{
	u32 now = HW_READ(TIMER_CLOCK(base)), want = (now & ~TIMER_FIELD(base)) | (saved_timer_clock & TIMER_FIELD(base));

	if (want != now)
		HW_WRITE(TIMER_CLOCK(base), want);
	if (!at_exit)
		return;
	clock_log.exit_done = 1;
	clock_log.exit_before = now;
	clock_log.exit_wrote = want;
	clock_log.exit_after = HW_READ(TIMER_CLOCK(base));
}

/* QUIT6: with the count stopped, the raw interrupt gpSP's run raised is
 * cleared before the OS's control (its interrupt enable) goes back; one the
 * OS had when gpSP took the timer stays. */
static void timer_clear_raised(u32 base)
{
	if (!(saved_timer_ris & 1))
		HW_WRITE(TIMER_INTCLR(base), 1);
}

/* Puts back the timer at 'base' as it was before timer_try, its own field
 * of the clock select too if it was changed (at_exit: logged as the exit's). */
static void timer_put_back(u32 base, int clock_changed, int at_exit)
{
	HW_WRITE(TIMER_CONTROL(base), 0);
	timer_clear_raised(base);
	if (clock_changed)
		clock_put_back(base, at_exit);
	HW_WRITE(TIMER_LOAD(base), saved_timer_load);
	HW_WRITE(TIMER_CONTROL(base), saved_timer_control);
}

/* Starts the timer free running (32-bit, no interrupt, divided by
 * timer_prescale) from its top. */
static void timer_run(u32 base)
{
	HW_WRITE(TIMER_CONTROL(base), 0);
	HW_WRITE(TIMER_LOAD(base), 0xFFFFFFFF);
	HW_WRITE(TIMER_CONTROL(base), 0x82 | timer_prescale);
}

/* Starts timer t: 1 if it moves (then it's the one used); else it's put
 * back as it was, 0. */
static int timer_try(const struct timer_choice *t)
{
	u32 control = HW_READ(TIMER_CONTROL(t->base)), first, i;

	if (t->if_idle && (control & 0x80))
	{
		tried(t, "the OS runs it");
		return 0;
	}
	saved_timer_load = HW_READ(TIMER_LOAD(t->base));
	saved_timer_control = control;
	saved_timer_clock = HW_READ(TIMER_CLOCK(t->base));
	saved_timer_ris = HW_READ(TIMER_RIS(t->base));
	HW_WRITE(TIMER_CONTROL(t->base), 0);
	/* R11t: its own field only (the block's other timer may be the OS's) */
	clock_set(t->base, t->clock);
	timer_prescale = 0;
	timer_run(t->base);
	/* 40000 reads take 5 to 20 ms: a 32 kHz timer moves a hundred times or
	 * more, a 1 kHz one a few. (More only made gpSP start late on a CX
	 * with timers that don't run: six tries.) */
	first = HW_READ(TIMER_VALUE(t->base));
	for (i = 0; i < 40000; i++)
		if (HW_READ(TIMER_VALUE(t->base)) != first)
		{
			timer_used = t;
			timer_base = t->base;
			tried(t, "runs");
			return 1;
		}
	timer_put_back(t->base, t->clock != KEEP_CLOCK, 0);
	tried(t, "doesn't run");
	return 0;
}

static u32 fast_timer_hz(u32 measured);
static void cx2_timer_init(void);

static void timer_init(void)
{
	u32 i;

	timer_tried[0] = 0;
	if (is_cx2)
	{
		cx2_timer_init();
		return;
	}
	for (i = 0; i < sizeof(cx_timers) / sizeof(cx_timers[0]) && !timer_works; i++)
		timer_works = timer_try(&cx_timers[i]);
	tick_rate_start(NSPIRE_TICK_HZ);
	/* An older CX: timed against its CPU clock right away (0.2 s), in case
	 * its real-time clock doesn't time it (the counter stayed at 60/60 on
	 * the CX CAS with the real-time clock check alone). */
	if (!is_cx2)
		cx_rtc_start = HW_READ(HW_RTC_SECONDS);
	if (timer_works && !is_cx2)
	{
		int any_rate = timer_base != FIRST_TIMER;
		cx_timer_hz = nspire_cx_timer_hz(TIMER_VALUE(timer_base), any_rate);
		/* A fast count (the fast timer's may be the bus clock's) divided down
		 * to near 32768, where the real-time clock check times it too (to 64
		 * times off) and a tick isn't cut short by the scale's rounding. */
		if (cx_timer_hz > 16 * NSPIRE_TICK_HZ)
		{
			timer_prescale = cx_timer_hz > 256 * NSPIRE_TICK_HZ ? 8 : 4;
			timer_run(timer_base);
			cx_timer_hz = nspire_cx_timer_hz(TIMER_VALUE(timer_base), any_rate);
		}
		cx_timer_hz = fast_timer_hz(cx_timer_hz);
		cx_raw_start = ~HW_READ(TIMER_VALUE(timer_base));
		tick_rate_assume(cx_raw_start, cx_timer_hz);
	}
	nspire_raw_timer = timer_works ? (volatile const u32 *)(uintptr_t)TIMER_VALUE(timer_base) : NULL;
}

/* An older CX's fast timer counts the peripheral bus's clock (its log on
 * the CX CAS, 2026-11-06: 116948 a second by the real-time clock divided by
 * 256, the bus at 60 MHz: 30 MHz / 256 = 117187), which its clock register
 * gives exactly; timing it by the CPU found 85955 there. So that, where
 * the CPU's timing is within a factor of two of it. */
static u32 fast_timer_hz(u32 measured)
{
	u32 apb = nspire_cx_apb_hz(), want;

	if ((timer_base & ~0x20u) != 0x90010000u || !apb)
		return measured;
	want = apb / (timer_prescale == 8 ? 256 : timer_prescale == 4 ? 16 : 1);
	return measured && measured < want * 2 && measured * 2 > want ? want : measured;
}

/* ---- R12: the CX II's time base ---- */

static int tb_follows(void)
{
	return tb_mode == TB_OWN || tb_mode == TB_BORROW;
}

static const char *tb_mode_name(void)
{
	static const char *const names[] = { "older CX", "own", "borrowed", "legacy" };
	return names[tb_mode];
}

static void tb_watch(u32 rtc);

/* The first timer's count, followed (own or borrowed): counting up since
 * the start, across its wraps, rewrites not made into time
 * (nspire_tickrate.h tick_follow); once a second, a look at what the OS did
 * to it (tb_watch). */
static u64 tb_raw(u32 rtc)
{
	u32 v = HW_READ(TIMER_VALUE(FIRST_TIMER)), top = 0xFFFFFFFFu;
	u64 total;

	if (tb_periodic && v > tb_follow.prev)
		top = HW_READ(TIMER_LOAD(FIRST_TIMER));   /* past 0: on from its load, as the OS has it now */
	total = tick_follow_step(&tb_follow, v, top, rtc);
	if (rtc != tb_rtc_watched)
	{
		tb_rtc_watched = rtc;
		tb_watch(rtc);
	}
	return total;
}

/* The count gpSP times things by, raw (its low 32 bits). */
static u32 tb_ref_raw(void)
{
	if (tb_follows())
		return (u32)tb_raw(HW_READ(HW_RTC_SECONDS));
	return ~HW_READ(TIMER_VALUE(FIRST_TIMER));
}

static u32 tb_ref_hz(void)
{
	return tb_follows() ? tick_base_hz() : NSPIRE_TICK_HZ;
}

/* How fast the timer at 'base' counts (a second), over about 'ms' of
 * gpSP's count; 0 if it doesn't run. Periodic or free running, 16 or 32
 * bits: a step back up is a reload. Only reads. */
static u32 tb_counted_hz(u32 base, u32 ms)
{
	u32 control = HW_READ(TIMER_CONTROL(base)), mask, top, prev, cur, i, t0, t1, want, hz = tb_ref_hz();
	u64 moves = 0;

	if (!(control & 0x80) || !hz)
		return 0;
	mask = (control & 2) ? 0xFFFFFFFFu : 0xFFFFu;
	top = (control & 0x40) ? HW_READ(TIMER_LOAD(base)) & mask : mask;
	want = (u32)((u64)hz * ms / 1000);
	t0 = tb_ref_raw();
	prev = HW_READ(TIMER_VALUE(base)) & mask;
	for (i = 0; i < 2000000; i++)
	{
		cur = HW_READ(TIMER_VALUE(base)) & mask;
		moves += cur <= prev ? prev - cur : (u64)prev + 1 + (top >= cur ? top - cur : 0);
		prev = cur;
		if ((i & 7) == 7 && tb_ref_raw() - t0 >= want)
			break;
	}
	t1 = tb_ref_raw();
	return t1 != t0 ? (u32)(moves * hz / (u32)(t1 - t0)) : 0;
}

/* Both blocks' timers, before any write: only reads. */
static void tb_read_blocks(void)
{
	u32 i, j;

	for (i = 0; i < TB_BLOCKS; i++)
	{
		for (j = 0; j < 2; j++)
		{
			u32 base = tb_block[i] + 0x20 * j;
			tb_seen[i][j].load = HW_READ(TIMER_LOAD(base));
			tb_seen[i][j].value = HW_READ(TIMER_VALUE(base));
			tb_seen[i][j].control = HW_READ(TIMER_CONTROL(base));
			tb_seen[i][j].bgload = HW_READ(TIMER_BGLOAD(base));
			tb_seen[i][j].ris = HW_READ(TIMER_RIS(base)) & 1;
		}
		tb_select_seen[i] = HW_READ(TIMER_CLOCK(tb_block[i]));
	}
	tb_seen_read = 1;
}

/* Once a second while the count is followed: what the OS did to the first
 * timer. Own: set up by the OS for itself (its control or load not
 * gpSP's): followed as the OS's from then (borrowed), nothing put back at
 * exit. Borrowed: stopped by the OS (its count still across a second's
 * change, its control saying so): gpSP's from
 * then (own), put back at exit as the OS left it. */
static void tb_watch(u32 rtc)
{
	u32 c = HW_READ(TIMER_CONTROL(FIRST_TIMER));

	if (tb_mode == TB_OWN)
	{
		if (c == (TB_OWN_CONTROL | timer_prescale) && HW_READ(TIMER_LOAD(FIRST_TIMER)) == 0xFFFFFFFFu)
			return;
		tb_os_changed = rtc - tb_rtc_start + 1;
		tb_mode = TB_BORROW;
		tb_taken = 0;
		tb_periodic = (c & 0x40) != 0;
		nspire_raw_timer = NULL;
		return;
	}
	if (tb_mode != TB_BORROW)
		return;
	tb_periodic = (c & 0x40) != 0;
	if (!(c & 0x80) && tb_follow.still >= 1)
	{
		saved_timer_load = HW_READ(TIMER_LOAD(FIRST_TIMER));
		saved_timer_control = c;
		saved_timer_ris = HW_READ(TIMER_RIS(FIRST_TIMER));
		timer_prescale = 0;
		timer_run(FIRST_TIMER);
		tick_follow_restart(&tb_follow, HW_READ(TIMER_VALUE(FIRST_TIMER)), rtc);
		tb_os_stopped = rtc - tb_rtc_start + 1;
		tb_mode = TB_OWN;
		tb_taken = 1;
		tb_periodic = 0;
		nspire_raw_timer = (volatile const u32 *)(uintptr_t)TIMER_VALUE(FIRST_TIMER);
	}
}

static void cx2_timer_init(void)
{
	u32 c, i, j;

	tb_read_blocks();
	c = tb_seen[0][0].control;
#ifdef NSPIRE_HOST
	{
		/* the PC model only: R11t's rule (the select written, nothing lent),
		 * to show the 44 s it gave (tests/host pad_tests test_r12_*) */
		const char *e = getenv("GPSP_HOST_TIMER");
		tb_r11t = e && !strcmp(e, "r11t");
	}
#endif
	if (tb_r11t)
		snprintf(tb_why, sizeof(tb_why), "R11t's rule, the PC model's GPSP_HOST_TIMER=r11t: the select written, nothing lent");
	else if (c & 0x80)
	{
		u32 top = (c & 0x40) ? tb_seen[0][0].load : 0xFFFFFFFFu, hz = 0;

		if (c & 1)
			snprintf(tb_why, sizeof(tb_why), "the OS runs 900c0000 one-shot (control %02lx): nothing to follow", (unsigned long)c);
		else if (!(c & 2))
			snprintf(tb_why, sizeof(tb_why), "the OS runs 900c0000 16-bit (control %02lx): too short to follow",
			         (unsigned long)c);
		else if (!(hz = nspire_cx2_timer_hz(TIMER_VALUE(FIRST_TIMER), top)))
			snprintf(tb_why, sizeof(tb_why), "the OS runs 900c0000 (control %02lx) but it didn't count in 20 ms by the CPU "
			         "clock", (unsigned long)c);
		else if ((c & 0x40) && (u64)top + 1 < (u64)hz * TB_MIN_PERIOD_S)
			snprintf(tb_why, sizeof(tb_why), "the OS runs 900c0000 periodic (control %02lx), every %lu ms (load %08lx at "
			         "%lu/s): under %u s, too short to follow", (unsigned long)c,
			         (unsigned long)(((u64)top + 1) * 1000 / hz), (unsigned long)top, (unsigned long)hz, TB_MIN_PERIOD_S);
		else
		{
			tb_mode = TB_BORROW;
			tb_periodic = (c & 0x40) != 0;
			tb_cpu_hz = hz;
			snprintf(tb_why, sizeof(tb_why), "the OS runs 900c0000 (control %02lx, %s): gpSP follows its count and writes "
			         "nothing in the block", (unsigned long)c, tb_periodic ? "periodic" : "free running");
		}
		if (tb_mode != TB_BORROW)
			tb_cpu_hz = hz;
	}
	else
	{
		/* not the OS's: gpSP's, free from the top, at the OS's clock, undivided */
		saved_timer_load = tb_seen[0][0].load;
		saved_timer_control = c;
		saved_timer_ris = tb_seen[0][0].ris;
		timer_prescale = 0;
		timer_run(FIRST_TIMER);
		tb_cpu_hz = nspire_cx2_timer_hz(TIMER_VALUE(FIRST_TIMER), 0xFFFFFFFFu);
		if (tb_cpu_hz)
		{
			tb_mode = TB_OWN;
			tb_taken = 1;
			snprintf(tb_why, sizeof(tb_why), "the OS doesn't run 900c0000 (control %02lx): gpSP runs it free from the top "
			         "at the OS's clock (select not written, undivided: the OS may read it)", (unsigned long)c);
		}
		else
		{
			HW_WRITE(TIMER_CONTROL(FIRST_TIMER), 0);
			timer_clear_raised(FIRST_TIMER);
			HW_WRITE(TIMER_LOAD(FIRST_TIMER), saved_timer_load);
			HW_WRITE(TIMER_CONTROL(FIRST_TIMER), saved_timer_control);
			snprintf(tb_why, sizeof(tb_why), "900c0000 didn't count at the OS's clock select %08lx in 20 ms by the CPU clock",
			         (unsigned long)tb_select_seen[0]);
		}
	}
	if (tb_follows())
	{
		u32 rtc = HW_READ(HW_RTC_SECONDS);

		tb_rtc_start = tb_rtc_watched = rtc;
		tick_follow_start(&tb_follow, HW_READ(TIMER_VALUE(FIRST_TIMER)), tb_cpu_hz, rtc);
		tick_base_start(0, tb_cpu_hz, 0);
		timer_works = 1;
		timer_base = FIRST_TIMER;
		/* (the link's NavNet task times its writes by it: a periodic count's
		 * wraps would spoil that, so none then) */
		nspire_raw_timer = tb_periodic ? NULL : (volatile const u32 *)(uintptr_t)TIMER_VALUE(FIRST_TIMER);
		clock_log.base = FIRST_TIMER;
		clock_log.os = clock_log.wrote = clock_log.after = tb_select_seen[0];
	}
	else
	{
		/* legacy: R11t's (the first timer's own field of the select at 32 kHz) */
		tb_mode = TB_LEGACY;
		for (i = 0; i < sizeof(cx2_timers) / sizeof(cx2_timers[0]) && !timer_works; i++)
			timer_works = timer_try(&cx2_timers[i]);
		tick_rate_start(NSPIRE_TICK_HZ);
		nspire_raw_timer = timer_works ? (volatile const u32 *)(uintptr_t)TIMER_VALUE(timer_base) : NULL;
	}
	/* The other timers' speeds (gpSP's count times them; only reads) */
	for (i = 0; i < TB_BLOCKS && timer_works; i++)
		for (j = 0; j < 2; j++)
			if (i || j)
				tb_hz_seen[i][j] = tb_counted_hz(tb_block[i] + 0x20 * j, 2);
	tb_hz_seen[0][0] = tb_cpu_hz;
}

/* Legacy, lent: gpSP's time by the OS's sleeps (its tick each), within the
 * real-time clock's seconds (+-1), never back. */
static u32 lent_now(void)
{
	u32 s = nspire_os_sleeps() - lent_sleeps, r = HW_READ(HW_RTC_SECONDS) - lent_rtc;
	u32 t = s * LENT_TICKS_PER_SLEEP, lo = r ? (r - 1) * NSPIRE_TICK_HZ : 0, hi = (r + 1) * NSPIRE_TICK_HZ;

	t = t < lo ? lo : t > hi ? hi : t;
	if (t < lent_last)
		t = lent_last;
	lent_last = t;
	return lent_ticks + t;
}

void nspire_timer_lend(void)
{
	if (tb_mode != TB_LEGACY || tb_r11t || !timer_works || !timer_used || lent)
		return;
	lent_ticks = nspire_ticks();
	lent_rtc = HW_READ(HW_RTC_SECONDS);
	lent_sleeps = nspire_os_sleeps();
	lent_last = 0;
	/* the block as the OS had it: its select, the first timer's load and control */
	timer_put_back(FIRST_TIMER, timer_used->clock != KEEP_CLOCK, 0);
	nspire_raw_timer = NULL;
	lent = 1;
	lends++;
}

void nspire_timer_retake(void)
{
	u32 t;

	if (!lent)
		return;
	t = lent_now();
	lent = 0;
	/* as the OS has it now (put back at exit), then gpSP's as at start */
	saved_timer_load = HW_READ(TIMER_LOAD(FIRST_TIMER));
	saved_timer_control = HW_READ(TIMER_CONTROL(FIRST_TIMER));
	saved_timer_clock = HW_READ(TIMER_CLOCK(FIRST_TIMER));
	saved_timer_ris = HW_READ(TIMER_RIS(FIRST_TIMER));
	HW_WRITE(TIMER_CONTROL(FIRST_TIMER), 0);
	if (timer_used->clock != KEEP_CLOCK && select_with(saved_timer_clock, FIRST_TIMER, timer_used->clock) != saved_timer_clock)
		HW_WRITE(TIMER_CLOCK(FIRST_TIMER), select_with(saved_timer_clock, FIRST_TIMER, timer_used->clock));
	timer_run(FIRST_TIMER);
	tick_rate_resume(~HW_READ(TIMER_VALUE(FIRST_TIMER)), t);
	nspire_raw_timer = (volatile const u32 *)(uintptr_t)TIMER_VALUE(FIRST_TIMER);
}

void nspire_timer_usb_probe(uint32_t w[6])
{
	u32 other = FIRST_TIMER + 0x20;

	w[0] = HW_READ(TIMER_CONTROL(other));
	w[1] = HW_READ(TIMER_LOAD(other));
	w[2] = is_cx2 && timer_works && !lent ? tb_counted_hz(other, 5) / 100 : 0;
	w[3] = HW_READ(TIMER_CLOCK(FIRST_TIMER));
	w[4] = HW_READ(TIMER_CONTROL(FIRST_TIMER));
	w[5] = is_cx2 && timer_works && !lent ? tb_ref_hz() / 100 : 0;
}

volatile const u32 *nspire_raw_timer;

u32 nspire_raw_timer_us(u32 counts)
{
	u32 hz = !is_cx2 ? cx_timer_hz : tb_follows() ? tick_base_hz() : NSPIRE_TICK_HZ;

	return hz ? (u32)((u64)counts * 1000000 / hz) : 0;
}

/* QUIT6: the raw interrupt of gpSP's timer before the put-back (the exit's
 * marker says it, written before) and what the put-back left (the file after
 * the hand-back says it). */
static void irq_exit_note(u32 base, int after)
{
	irq_exit.base = base;
	if (!after)
	{
		irq_exit.raw_before = HW_READ(TIMER_RIS(base)) & 1;
		return;
	}
	irq_exit.done = 1;
	irq_exit.raw_after = HW_READ(TIMER_RIS(base)) & 1;
	irq_exit.masked_after = HW_READ(TIMER_MIS(base)) & 1;
}

static void timer_restore(void)
{
	nspire_raw_timer = NULL;
	if (tb_follows())
	{
		/* own: the first timer as the OS had it (when gpSP took it); borrowed:
		 * nothing (nothing was written) */
		if (tb_taken)
		{
			irq_exit_note(FIRST_TIMER, 0);
			HW_WRITE(TIMER_CONTROL(FIRST_TIMER), 0);
			timer_clear_raised(FIRST_TIMER);
			HW_WRITE(TIMER_LOAD(FIRST_TIMER), saved_timer_load);
			HW_WRITE(TIMER_CONTROL(FIRST_TIMER), saved_timer_control);
			irq_exit_note(FIRST_TIMER, 1);
		}
		tb_exit.done = 1;
		tb_exit.control = HW_READ(TIMER_CONTROL(FIRST_TIMER));
		tb_exit.load = HW_READ(TIMER_LOAD(FIRST_TIMER));
		tb_exit.select = HW_READ(TIMER_CLOCK(FIRST_TIMER));
		return;
	}
	if (lent)
		return;   /* the OS's already */
	if (timer_used)
	{
		irq_exit_note(timer_base, 0);
		timer_put_back(timer_base, timer_used->clock != KEEP_CLOCK, 1);
		irq_exit_note(timer_base, 1);
	}
}

/* QUIT6: gpSP's timer's interrupt, for the logs: before the exit's put-back
 * (the marker, written before it: whether its count passed zero, so the
 * put-back has an interrupt to clear) and after it (the file after the
 * hand-back: what the OS gets); "" when gpSP runs no timer of its own. */
static int timer_irq_text(char *text, u32 size)
{
	u32 base = tb_follows() ? FIRST_TIMER : timer_base;
	int mine = tb_follows() ? tb_taken : timer_used && !lent;

	if (!size)
		return 0;
	text[0] = 0;
	if (irq_exit.done)
		return snprintf(text, size, "; its interrupt: raw %lu before the put-back (%s), raw %lu masked %lu after (%s)",
		                (unsigned long)irq_exit.raw_before,
		                !irq_exit.raw_before ? "its count never passed zero" :
		                saved_timer_ris & 1 ? "the OS had it raised when gpSP took it" : "its count had passed zero",
		                (unsigned long)irq_exit.raw_after, (unsigned long)irq_exit.masked_after,
		                irq_exit.masked_after ? "LINE RAISED: the OS takes an interrupt it doesn't clear" :
		                "the OS's line quiet");
	if (!mine)
		return 0;
	return snprintf(text, size, "; %08lx's interrupt now: raw %lu (%s), the OS had %lu when gpSP took it: the put-back %s",
	                (unsigned long)base, (unsigned long)(HW_READ(TIMER_RIS(base)) & 1),
	                HW_READ(TIMER_RIS(base)) & 1 ? "its count passed zero" : "its count hasn't passed zero",
	                (unsigned long)(saved_timer_ris & 1),
	                saved_timer_ris & 1 ? "leaves it as the OS had it" : "clears it before the OS's control goes back");
}

/* R11t: the clock select's line for the logs (the link log's and pad log's
 * header, the exit marker, the after-the-hand-back file): what the OS had,
 * what gpSP wrote (its own field only), and the OS's other timer's count
 * before and after (unchanged: the write left it alone); after the exit's
 * put-back, that too. Register reads only (the exit's after-file calls it
 * after the hand-back). */
int nspire_timer_clock_text(char *text, u32 size)
{
	int n;

	if (!size)
		return clock_log.base != 0;
	if (!clock_log.base)
	{
		snprintf(text, size, "timer clock select: not touched yet (no timer tried)");
		return 0;
	}
	if (is_cx2 && (tb_follows() || tb_exit.done))
	{
		/* R12: nothing written to it on a CX II (own or borrowed) */
		n = snprintf(text, size, "timer clock select 900c0080 (one word for 900c0000 and 900c0020): the OS had %08lx; "
		             "gpSP writes nothing to it on a CX II (R12: time base %s), reads %08lx", (unsigned long)clock_log.os,
		             tb_mode_name(), (unsigned long)(tb_exit.done ? tb_exit.select : HW_READ(TIMER_CLOCK(FIRST_TIMER))));
		if (n >= 0 && (u32)n < size && tb_exit.done)
			n += snprintf(text + n, size - n, "; at exit 900c0000 %s: control %02lx, load %08lx (the OS had control %02lx, "
			              "load %08lx at start)", tb_taken ? "put back as the OS had it" : "untouched (the OS's)",
			              (unsigned long)tb_exit.control, (unsigned long)tb_exit.load, (unsigned long)tb_seen[0][0].control,
			              (unsigned long)tb_seen[0][0].load);
		else if (n >= 0 && (u32)n < size)
			n += snprintf(text + n, size - n, "; 900c0000 %s", tb_taken ? "gpSP's (put back at exit as the OS had it)" :
			              "the OS's (nothing to put back)");
		if (n >= 0 && (u32)n < size)
			timer_irq_text(text + n, size - n);
		return 1;
	}
	n = snprintf(text, size, "timer clock select %08lx (one word for %08lx and %08lx; gpSP's field %lx): the OS had "
	             "%08lx; gpSP %s %08lx, reads %08lx",
	             (unsigned long)TIMER_CLOCK(clock_log.base), (unsigned long)(clock_log.base & ~0x20u),
	             (unsigned long)(clock_log.base | 0x20u), (unsigned long)TIMER_FIELD(clock_log.base),
	             (unsigned long)clock_log.os, clock_log.written ? "wrote" : "wrote nothing, keeping",
	             (unsigned long)clock_log.wrote, (unsigned long)clock_log.after);
	if (n >= 0 && (u32)n < size)
	{
		u32 other = TIMER_OTHER(clock_log.base);

		if (!clock_log.other_runs)
			n += snprintf(text + n, size - n, "; the other timer %08lx: control %02lx, not running", (unsigned long)other,
			              (unsigned long)clock_log.other_control);
		else if (!clock_log.written)
			n += snprintf(text + n, size - n, "; the other timer %08lx: control %02lx, load %08lx, runs (not counted: "
			              "nothing written)", (unsigned long)other, (unsigned long)clock_log.other_control,
			              (unsigned long)clock_log.other_load);
		else
			n += snprintf(text + n, size - n, "; the other timer %08lx: control %02lx, load %08lx, counted %lu in %u reads "
			              "before the write, %lu after (%s)", (unsigned long)other,
			              (unsigned long)clock_log.other_control, (unsigned long)clock_log.other_load,
			              (unsigned long)clock_log.moves_before, OTHER_READS, (unsigned long)clock_log.moves_after,
			              clock_log.moves_after * 4 < clock_log.moves_before * 3 ||
			              clock_log.moves_after * 3 > clock_log.moves_before * 4 + 3 ? "CHANGED: the write reached it" :
			              "unchanged");
	}
	if (n >= 0 && (u32)n < size)
	{
		if (clock_log.exit_done)
			n += snprintf(text + n, size - n, "; at exit it read %08lx, gpSP wrote %08lx%s, reads %08lx",
			              (unsigned long)clock_log.exit_before, (unsigned long)clock_log.exit_wrote,
			              clock_log.exit_wrote == clock_log.exit_before ? " (nothing: unchanged)" : "",
			              (unsigned long)clock_log.exit_after);
		else if (timer_used && timer_used->clock != KEEP_CLOCK)
			n += snprintf(text + n, size - n, "; now %08lx, to put back at exit %08lx",
			              (unsigned long)HW_READ(TIMER_CLOCK(clock_log.base)),
			              (unsigned long)((HW_READ(TIMER_CLOCK(clock_log.base)) & ~TIMER_FIELD(clock_log.base)) |
			                              (saved_timer_clock & TIMER_FIELD(clock_log.base))));
	}
	if (n >= 0 && (u32)n < size)
		timer_irq_text(text + n, size - n);
	return 1;
}

int nspire_timer_runs(void)
{
	return timer_works;
}

u32 nspire_ticks(void)
{
	u32 raw;

	if (!timer_works)
		return fallback_ticks;
	if (tb_follows())
	{
		/* R12: the count followed (the real-time clock read every time: the
		 * follower's guard and the speed's refinement) */
		u32 rtc = HW_READ(HW_RTC_SECONDS);
		return tick_base_ticks(tb_raw(rtc), rtc);
	}
	if (lent)
		return lent_now();
	raw = ~HW_READ(TIMER_VALUE(timer_base));   /* the timer counts down */
	return tick_rate_ticks(raw, tick_rate_checked() ? 0 : HW_READ(HW_RTC_SECONDS));
}

/* R12: the timer's log lines after the clock select's (nspire.h). */
int nspire_timer_header_text(int line, char *text, u32 size)
{
	int n = 0;

	if (!size)
		return 0;
	text[0] = 0;
	if (line == 0)
		return nspire_timer_clock_text(text, size);
	if (!is_cx2 || !tb_seen_read || line > 1 + TB_BLOCKS)
		return 0;
	if (line == 1)
	{
		char base[300];

		if (tb_follows())
			tick_base_report(base, sizeof(base));
		else if (timer_works)
			tick_rate_report(base, sizeof(base));
		else
			snprintf(base, sizeof(base), "no timer runs");
		n = snprintf(text, size, "time base: %s (%s); %s", tb_mode_name(), tb_why, base);
		if (n >= 0 && (u32)n < size && tb_follows())
			n += snprintf(text + n, size - n, "; follower: %lu jump(s)%s, %lu lost wrap(s)", (unsigned long)tb_follow.jumps,
			              "", (unsigned long)tb_follow.lost_wraps);
		if (n >= 0 && (u32)n < size && tb_follow.jumps)
			n += snprintf(text + n, size - n, " (the last %08lx -> %08lx)", (unsigned long)tb_follow.jump_from,
			              (unsigned long)tb_follow.jump_to);
		if (n >= 0 && (u32)n < size && tb_os_changed)
			n += snprintf(text + n, size - n, "; the OS set 900c0000 up itself %lu s in: followed as its since",
			              (unsigned long)(tb_os_changed - 1));
		if (n >= 0 && (u32)n < size && tb_os_stopped)
			n += snprintf(text + n, size - n, "; the OS stopped 900c0000 %lu s in: gpSP's since",
			              (unsigned long)(tb_os_stopped - 1));
		if (n >= 0 && (u32)n < size && tb_switches)
			n += snprintf(text + n, size - n, "; %lu CPU clock switch(es), after the last %lu/s by the CPU clock",
			              (unsigned long)tb_switches, (unsigned long)tb_switch_hz);
		if (n >= 0 && (u32)n < size && tb_mode == TB_LEGACY)
			n += snprintf(text + n, size - n, "; lent to the OS for %lu come-up burst(s)", (unsigned long)lends);
		return 1;
	}
	{
		u32 b = (u32)line - 2, j;

		n = snprintf(text, size, "timers at start, read before any write: select %08lx at %08lx", (unsigned long)tb_select_seen[b],
		             (unsigned long)TIMER_CLOCK(tb_block[b]));
		for (j = 0; j < 2 && n >= 0 && (u32)n < size; j++)
			n += snprintf(text + n, size - n, "; %08lx: load %08lx value %08lx control %02lx background load %08lx, %s%lu "
			              "counts/s, raw interrupt %lu", (unsigned long)(tb_block[b] + 0x20 * j),
			              (unsigned long)tb_seen[b][j].load, (unsigned long)tb_seen[b][j].value,
			              (unsigned long)tb_seen[b][j].control, (unsigned long)tb_seen[b][j].bgload,
			              b == 0 && j == 0 ? "by the CPU clock " : "", (unsigned long)tb_hz_seen[b][j],
			              (unsigned long)tb_seen[b][j].ris);
		return 1;
	}
}

/* An older CX's timer lines, for the clean-exit marker (gpsp_link_run.txt.tns,
 * nspire_exit.c; EXIT-3: they were a file of their own, gpsp_timer.txt.tns,
 * a write more at every exit): what its timer did against the CPU clock
 * and the real-time clock. Nothing on a CX II. */
void nspire_timer_log_text(char *text, u32 size)
{
	char line[520];
	u32 raw, rtc;
	int n = 0;

	if (!size)
		return;
	text[0] = 0;
#define ADD(...) do { if (n >= 0 && (u32)n < size) n += snprintf(text + n, size - n, __VA_ARGS__); } while (0)
	/* R11t: both models: the clock select before the exit's put-back; R12:
	 * a CX II's time base lines after it */
	nspire_timer_clock_text(line, sizeof(line));
	ADD("%s\n", line);
	if (is_cx2)
	{
		int i;

		for (i = 1; nspire_timer_header_text(i, line, sizeof(line)); i++)
			ADD("%s\n", line);
		return;
	}
	raw = timer_works ? ~HW_READ(TIMER_VALUE(timer_base)) : 0;
	rtc = HW_READ(HW_RTC_SECONDS);
	nspire_timer_report(line, sizeof(line));
	ADD("%s\n", line);
	ADD("timers tried: %s\n", timer_tried);
	if (timer_used)
		ADD("timer used: %s at %08lx, divided by %u\n", timer_used->name, (unsigned long)timer_base,
		    timer_prescale == 8 ? 256 : timer_prescale == 4 ? 16 : 1);
	/* The power controller's clock gates (Firebird's names), which may be
	 * why a timer doesn't run: only read. */
	ADD("power controller: disable %08lx, disable2 %08lx\n", (unsigned long)HW_READ(HW_CX_CLOCK_GATES),
	    (unsigned long)HW_READ(HW_CX_CLOCK_GATES2));
	ADD("by the CPU clock: %lu ticks/s (0: not timed)\n", (unsigned long)cx_timer_hz);
	ADD("while gpSP ran: real-time clock %lu -> %lu (%lu s), timer %lu raw ticks (%lu a second)\n",
	    (unsigned long)cx_rtc_start, (unsigned long)rtc, (unsigned long)(rtc - cx_rtc_start),
	    (unsigned long)(raw - cx_raw_start),
	    (unsigned long)(rtc != cx_rtc_start ? (raw - cx_raw_start) / (rtc - cx_rtc_start) : 0));
	nspire_clock_report(line, sizeof(line));
	ADD("%s\n", line);
#undef ADD
}

/* EXIT-1: the OS's timer settings back (nspire_exit.c, right before the
 * hand-back: the exit's steps are timed by it until then). */
void nspire_timer_hand_back(void)
{
	timer_restore();
}

u32 nspire_rtc_seconds(void)
{
	return HW_READ(HW_RTC_SECONDS);
}

void nspire_timer_recheck(void)
{
	u32 hz;

	if (!timer_works)
		return;
	if (tb_follows())
	{
		/* R12: a CX II's count at the OS's clock, which may follow the CPU's:
		 * the CPU loop's timing now (logged), and the real-time clock's
		 * refinement from here on (it takes a changed speed once two stretches
		 * agree on it) */
		tb_switches++;
		tb_switch_hz = nspire_cx2_timer_hz(TIMER_VALUE(FIRST_TIMER), tb_periodic ? HW_READ(TIMER_LOAD(FIRST_TIMER)) :
		                                   0xFFFFFFFFu);
		tick_base_recheck();
		return;
	}
	if (lent)
		return;
	tick_rate_recheck(~HW_READ(TIMER_VALUE(timer_base)));
	/* An older CX's other timers: one on the bus's clock changed speed
	 * with it, so it's timed by the CPU clock again (0.2 s). */
	if (!is_cx2 && timer_base != FIRST_TIMER && (hz = fast_timer_hz(nspire_cx_timer_hz(TIMER_VALUE(timer_base), 1))))
	{
		cx_timer_hz = hz;
		tick_rate_assume(~HW_READ(TIMER_VALUE(timer_base)), hz);
	}
}

void nspire_timer_report(char *text, u32 size)
{
	if (!timer_works)
		snprintf(text, size, "timer: doesn't run, so games aren't held to their speed");
	else if (tb_follows())
	{
		int n = snprintf(text, size, "timer: %s 900c0000, ", tb_mode_name());
		if (n >= 0 && (u32)n < size)
			tick_base_report(text + n, size - n);
	}
	else
		tick_rate_report(text, size);
}

void nspire_wait_until(u32 tick)
{
	if (!timer_works)
	{
		/* Without a timer, time only moves by waiting: games run unthrottled. */
		if ((s32)(tick - fallback_ticks) > 0)
			fallback_ticks = tick;
		return;
	}
	/* Never more than a second, whatever was asked. */
	if ((s32)(tick - nspire_ticks()) > (s32)NSPIRE_TICK_HZ)
		return;
	/* While the link wants it, the OS's tasks get the spare time: a sleep
	 * whenever the OS's tick comes before the frame is due (nspire_os.h). */
	/* P2: sleeps here are spare time, counted apart (nspire_os_busy_slept). */
	nspire_os_idle = 1;
	while ((s32)(tick - nspire_ticks()) > 0)
		if (nspire_os_wanted)
			nspire_os_sleep_before(tick);
	nspire_os_idle = 0;
}

/* Lines a refresh by PL111 timing 1: the lines shown, the sync's, the front
 * and the back porch's. */
static u32 lcd_lines_of(u32 t1)
{
	return (t1 & 0x3FF) + 1 + (((t1 >> 10) & 0x3F) + 1) + ((t1 >> 16) & 0xFF) + (t1 >> 24);
}

/* A refresh's time (16.16 ticks), the mean of n, from the LCD coming back
 * to its buffer's first lines (as nspire_lcd_report times it); 0 when n
 * weren't seen in a quarter of a second. */
static u32 lcd_measure(int n)
{
	uintptr_t base = HW_LCD_BASE_GET(), last = 0;
	u32 since = nspire_ticks(), first = 0;
	int found = 0;
	long polls;

	for (polls = 0; polls < 8000000L && nspire_ticks() - since < NSPIRE_TICK_HZ / 4; polls++)
	{
		uintptr_t off = HW_LCD_CURRENT_GET() - base;
		if (polls && off < sizeof(screens[0]) / 4 && last >= sizeof(screens[0]) / 4)
		{
			u32 now = nspire_ticks();
			if (!found++)
				first = now;
			else if (found > n)
				return (u32)(((u64)(now - first) << 16) / (u32)n);
		}
		last = off;
	}
	return 0;
}

/* Within pct percent of a GBA frame. */
static int lcd_near_gba(u32 period16, u32 pct)
{
	return period16 && (u64)period16 * 100 >= (u64)LCD_GBA_PERIOD * (100 - pct) &&
	       (u64)period16 * 100 <= (u64)LCD_GBA_PERIOD * (100 + pct);
}

#define LCD_MS10(p) ((unsigned long)(((u64)(p) * 10000 / NSPIRE_TICK_HZ) >> 16))   /* 16.16 ticks -> tenths of ms */

/* The refresh made a GBA frame (see lcd_period16). */
static void lcd_tune(void)
{
	u32 t0 = LCDTiming0, t1 = LCDTiming1, p0, p1, p2, line16, fixed, want, vfp;

	lcd_lines = lcd_lines_of(t1);
	p0 = lcd_measure(4);
	if (!p0)
	{
		snprintf(lcd_note, sizeof(lcd_note), "; refresh not seen at the start: timings kept");
		return;
	}
	if (lcd_near_gba(p0, 1))
	{
		snprintf(lcd_note, sizeof(lcd_note), "; refresh %lu.%lu ms at the start: kept", LCD_MS10(p0) / 10,
		         LCD_MS10(p0) % 10);
		return;   /* the PC's model; and a calculator already there */
	}
	/* the lines a little shorter */
	if (((t0 >> 16) & 0xFF) > LCD_TUNE_HFP)
	{
		t0 = (t0 & ~(0xFFu << 16)) | (LCD_TUNE_HFP << 16);
		HW_WRITE(HW_LCD_TIMING0, t0);
		lcd_measure(1);   /* the refresh it changed in */
		p1 = lcd_measure(4);
	}
	else
		p1 = p0;
	/* as many lines as make a GBA frame at that line's time */
	line16 = p1 / lcd_lines;
	fixed = lcd_lines - ((t1 >> 16) & 0xFF);
	want = line16 ? (LCD_GBA_PERIOD + line16 / 2) / line16 : 0;
	vfp = want >= fixed + LCD_VFP_MIN ? want - fixed : LCD_VFP_MIN;
	if (vfp > 255)
		vfp = 255;
	if (p1)
	{
		t1 = (t1 & ~(0xFFu << 16)) | (vfp << 16);
		HW_WRITE(HW_LCD_TIMING1, t1);
		lcd_measure(1);
	}
	p2 = p1 ? lcd_measure(4) : 0;
	if (!lcd_near_gba(p2, 3))
	{
		HW_WRITE(HW_LCD_TIMING0, LCDTiming0);
		HW_WRITE(HW_LCD_TIMING1, LCDTiming1);
		lcd_period16 = p0;   /* the refresh as it is, for the pictures' times */
		snprintf(lcd_note, sizeof(lcd_note), "; refresh %lu.%lu ms, %lu.%lu ms tuned: timings put back",
		         LCD_MS10(p0) / 10, LCD_MS10(p0) % 10, LCD_MS10(p2) / 10, LCD_MS10(p2) % 10);
		return;
	}
	lcd_tuned = 1;
	lcd_period16 = p2;
	lcd_lines = lcd_lines_of(t1);
	/* QUIT6: and the timings found (the line's words are the tuned ones; the
	 * 23.0 ms of a CX II whose OS set its own 03780d3f again after a sleep,
	 * 18.5 ms under Ndless's 0720013f), which the quit puts back */
	snprintf(lcd_note, sizeof(lcd_note), "; refresh %lu.%lu ms tuned to %lu.%lu ms (%lu lines; found %08lx %08lx, put "
	         "back at the quit)", LCD_MS10(p0) / 10, LCD_MS10(p0) % 10, LCD_MS10(p2) / 10, LCD_MS10(p2) % 10,
	         (unsigned long)lcd_lines, (unsigned long)LCDTiming0, (unsigned long)LCDTiming1);
}

void nspire_init()
{
	int i;

	os_screen = (u16*)HW_LCD_BASE_GET();
	LCDTiming0 = HW_READ(HW_LCD_TIMING0);
	LCDTiming1 = HW_READ(HW_LCD_TIMING1);
	LCDControl = HW_READ(HW_LCD_CONTROL);
	timer_init();

	/* The CPU never reads the screen buffers on a portrait LCD, and its data
	 * cache only fills on reads, so they are kept out of the cache and only
	 * the write buffer needs draining before the LCD reads them. */
	memset(screens, 0, sizeof(screens));
	for (i = 0; i < SCREENS; i++)
		flush_dcache_range(screens[i], sizeof(screens[i]));
	drawing_screen = 1;
	nspire_displayed_screen = screens[0];
	nspire_screen = screens[1];

	nspire_lcd_portrait = nspire_lcd_is_portrait();
	if (nspire_lcd_portrait)
	{
		/* Keep the OS's portrait 5:6:5 format and the placing of its timings;
		 * the buffer address changes, and the refresh is made a GBA frame
		 * (lcd_tune). Frames are written turned (nspire_turn.c). */
		set_display_buffer(nspire_displayed_screen);
		lcd_tune();
		return;
	}
	HW_WRITE(HW_LCD_CONTROL, 0);
	HW_WRITE(HW_LCD_TIMING0, 0x251A004C);
	HW_WRITE(HW_LCD_TIMING1, 0x110300EF);
	set_display_buffer(nspire_displayed_screen);
	u32 newControl = (LCDControl & ~0x90E) | 0x008;
	HW_WRITE(HW_LCD_CONTROL, newControl);
	HW_WRITE(HW_LCD_CONTROL, newControl | 0x800);
}

void nspire_restore()
{
	if (nspire_lcd_portrait)
	{
		set_display_buffer(os_screen);
		if (lcd_tuned)
		{
			HW_WRITE(HW_LCD_TIMING0, LCDTiming0);
			HW_WRITE(HW_LCD_TIMING1, LCDTiming1);
			lcd_tuned = 0;
		}
	}
	else
	{
		HW_WRITE(HW_LCD_CONTROL, 0);
		set_display_buffer(os_screen);
		HW_WRITE(HW_LCD_TIMING0, LCDTiming0);
		HW_WRITE(HW_LCD_TIMING1, LCDTiming1);
		HW_WRITE(HW_LCD_CONTROL, LCDControl & ~0x800);
		HW_WRITE(HW_LCD_CONTROL, LCDControl);
	}
	enableAlignmentExceptions();
	nspire_exit_step("screen back");
	/* The OS's file list doesn't see files gpSP moves or removes: without
	 * this it kept showing files and folders that were gone. Reading them
	 * all again takes many seconds, so only then (nspire_paths.h). */
	/* fast17: the names gpSP removed or renamed away alone, not the whole
	 * saves folder read again; the step says why (nspire_paths.h) */
	if (nspire_file_list_changed())
	{
		nspire_exit_step(nspire_file_list_why());
		refresh_osscr();
		nspire_exit_step("OS file list read again");
	}
	else
		nspire_exit_step(nspire_file_list_why());
	/* (8 bytes: nspire_restore keeps r16's size, so the game's unpinned code
	 * after it, nspire_flip and the renderer's objects, stays where it was) */
	__asm__ volatile("nop\n\tnop");
	/* EXIT-1: the timer goes back with the hand-back (nspire_exit.c), last. */
}

void set_display_buffer(void* buffer)
{
	HW_LCD_BASE_SET(buffer);
}

#if defined(NSPIRE_LIBRETRO)
extern u32 nspire_lcd_cache_clean_full;
#endif

/* Shows the frame drawn into nspire_screen and moves nspire_screen on to the
 * next buffer. The LCD controller takes a new address at its next refresh, so
 * it always shows whole frames; and it never reads the buffer drawn into
 * next, unless frames come faster than it refreshes, which is waited out. */
void nspire_flip(void)
{
	uintptr_t start, end;
	u32 since;
	int polls;

	if (nspire_lcd_portrait)
		HW_DRAIN_WRITE_BUFFER();
	else
		nspire_lcd_fb_cache_clean_for_display();
	set_display_buffer(nspire_screen);
	nspire_displayed_screen = nspire_screen;
	flip_taken = 0;
	drawing_screen = (drawing_screen + 1) % SCREENS;
	nspire_screen = screens[drawing_screen];

	start = (uintptr_t)nspire_screen;
	end = start + sizeof(screens[0]);
	since = nspire_ticks();
	flips++;
	nspire_flip_serial++;
	for (polls = 0; polls < 1000000; polls++)
	{
		uintptr_t reading = HW_LCD_CURRENT_GET();
		if (reading < start || reading >= end)
			break;
		if (!polls)
			flips_waited++;
		if (nspire_ticks() - since > NSPIRE_TICK_HZ / 25)
		{
			flips_timed_out++;   /* drawn into while the LCD reads it: torn */
			break;
		}
	}
}

/* 2026-11-11 (the CX CAS: "after the menu ... bits of the screen flash in
 * another place"; the PC model draws the same pictures with and without
 * the menu): what only the calculator can say, one line at a game's start
 * and after each menu (nspire_host_main.c), never per frame. The LCD's
 * refresh, timed now (at most 0.1 s; nothing is flipped meanwhile), its
 * registers, whether its DMA ran dry (PL111 raw interrupt bit 1, FIFO
 * underflow: lines shifted; cleared here, so each line says "since the
 * last"), and of the flips since the last line, how many found the LCD
 * still reading the buffer drawn into next (waited) and how many waited
 * the whole 1/25 s (drawn into while shown: tearing). */
void nspire_lcd_report(char *text, u32 size, int timed)
{
	uintptr_t base = HW_LCD_BASE_GET(), last = 0;
	u32 since = nspire_ticks(), starts[2] = { 0, 0 }, raw;
	int found = 0, polls;

	for (polls = 0; timed && polls < 2000000 && found < 2 && nspire_ticks() - since < NSPIRE_TICK_HZ / 10; polls++)
	{
		uintptr_t off = HW_LCD_CURRENT_GET() - base;
		/* a refresh starts: the LCD back at the buffer's first lines */
		if (polls && off < sizeof(screens[0]) / 4 && last >= sizeof(screens[0]) / 4)
			starts[found++] = nspire_ticks();
		last = off;
	}
	raw = HW_READ(HW_LCD_RAW_IRQ);
	HW_WRITE(HW_LCD_IRQ_CLEAR, 2);
	if (found == 2)
		snprintf(text, size, "LCD refresh %lu.%lu ms", (unsigned long)((starts[1] - starts[0]) * 1000u / NSPIRE_TICK_HZ),
		         (unsigned long)((starts[1] - starts[0]) * 10000u / NSPIRE_TICK_HZ % 10));
	else if (timed)
		snprintf(text, size, "LCD refresh not seen in 0.1 s");
	else
		snprintf(text, size, "LCD refresh not timed");
	snprintf(text + strlen(text), size - strlen(text),
	         ", control %08lx, timing %08lx %08lx %08lx, interrupts raw %02lx mask %02lx (DMA ran dry: %s); "
	         "%lu flips: %lu found the LCD on the next buffer, %lu drawn into while shown",
	         (unsigned long)HW_READ(HW_LCD_CONTROL), (unsigned long)HW_READ(HW_LCD_TIMING0),
	         (unsigned long)HW_READ(HW_LCD_TIMING1), (unsigned long)HW_READ(HW_LCD_TIMING2),
	         (unsigned long)(raw & 0x1F), (unsigned long)(HW_READ(HW_LCD_IRQ_MASK) & 0x1F),
	         raw & 2 ? "yes, since the last line" : "no", (unsigned long)flips, (unsigned long)flips_waited,
	         (unsigned long)flips_timed_out);
	snprintf(text + strlen(text), size - strlen(text), "%s", lcd_note);
	flips = flips_waited = flips_timed_out = 0;
}

/* F3 (2026-11-11): a game starts the lines' count. The underflow
 * bit is only cleared by nspire_lcd_report, so the first line said "since
 * boot" (the OS's time and the game list's included). */
void nspire_lcd_report_restart(void)
{
	HW_WRITE(HW_LCD_IRQ_CLEAR, 2);
	flips = flips_waited = flips_timed_out = 0;
}

int nspire_screen_index(void)
{
	return drawing_screen;
}

/* Whether the LCD has taken the frame nspire_flip showed last: it takes a
 * new address at the start of a refresh, then reads that buffer's lines (in
 * the porches its address is just past the end, so this only notices while
 * the lines are read). A frame drawn before then would replace it unseen.
 * The buffers lie one after the other, so a buffer's first byte is also
 * the address in the porches of the one before it: that address says
 * nothing (2026-11-12: it read as taken, and as a refresh just begun). */
int nspire_flip_taken(void)
{
	if (!flip_taken)
	{
		uintptr_t reading = HW_LCD_CURRENT_GET(), start = (uintptr_t)nspire_displayed_screen;
		flip_taken = reading > start && reading < start + sizeof(screens[0]);
	}
	return flip_taken;
}

/* About how long (timer ticks) until the LCD's next refresh starts. It
 * reads a buffer's 320 lines, then waits out the porches: lcd_lines' time
 * in all (447 by the CX II OS's timings, about 60 times a second, until
 * measured: lcd_tune). In the porches (its address just past a buffer)
 * it's under the porches' time: 0. */
u32 nspire_lcd_ticks_to_refresh(void)
{
	uintptr_t reading = HW_LCD_CURRENT_GET();
	int i;

	for (i = 0; i < SCREENS; i++)
	{
		uintptr_t start = (uintptr_t)screens[i];
		if (reading > start && reading < start + sizeof(screens[i]))
		{
			u32 refresh = lcd_period16 ? lcd_period16 >> 16 : NSPIRE_TICK_HZ / 60;
			return refresh - (u32)((u64)refresh * (reading - start) * 320 / (sizeof(screens[i]) * lcd_lines));
		}
	}
	return 0;
}

int nspire_lcd_at_gba_rate(void)
{
	return lcd_tuned;
}

u32 nspire_lcd_period16(void)
{
	return lcd_period16 ? lcd_period16 : (u32)(((u64)NSPIRE_TICK_HZ << 16) / 60);
}

/* Waits for the LCD's next vertical compare interrupt (menus run at the LCD's
 * pace). Frames are shown by nspire_flip. */
void update_at_vblank(void)
{
	u32 since = nspire_ticks();
	HW_WRITE(HW_LCD_IRQ_CLEAR, 4);
	while ((HW_READ(HW_LCD_RAW_IRQ) & 4) == 0)
	{
		if (nspire_ticks() - since > NSPIRE_TICK_HZ / 20)
			break;
	}
	HW_WRITE(HW_LCD_IRQ_CLEAR, 4);
}

void nspire_lcd_fb_cache_clean_for_display(void)
{
#if defined(NSPIRE_LIBRETRO)
	if (nspire_lcd_cache_clean_full)
		clean_dcache();
	else
#endif
		warm_cache_op_range(WOP_D_CLEAN, nspire_screen, NSPIRE_LCD_FB_SIZE_BYTES);
}

//#define VERY_SAFE_CACHE

int warm_cache_op_range(int op, void *addr, unsigned long size)
{
	void *end_addr = addr + size;
	switch (op)
	{
	case WOP_D_CLEAN:
#ifndef VERY_SAFE_CACHE
		addr = (void*)((uintptr_t)addr & ~(uintptr_t)0x1F);
		while(addr < end_addr)
		{
			HW_CLEAN_DCACHE_LINE(addr);
			addr += 32;
		}
		HW_DRAIN_WRITE_BUFFER();
#else
		clean_dcache();
#endif
		return 0;
	case WOP_I_INVALIDATE:
#ifndef VERY_SAFE_CACHE
		addr = (void*)((uintptr_t)addr & ~(uintptr_t)0x1F);
		while(addr < end_addr)
		{
			HW_INVALIDATE_ICACHE_LINE(addr);
			addr += 32;
		}
#else
		HW_INVALIDATE_ICACHE();
#endif
		return 0;
	}
	return -1;
}

/* Code written as data, anywhere (gui.c's trampolines on the stack): the
 * whole data cache is written back and the instruction cache emptied. */
void nspire_sync_written_code(void)
{
	clean_dcache();
	HW_INVALIDATE_ICACHE();
}

void nspire_jit_cache_sync(void *base, void *end)
{
	unsigned long size;
	char *b;
	char *e;

	if (!base || !end || end <= base)
		return;
	b = (char *)base;
	e = (char *)end;
	size = (unsigned long)(e - b);
	warm_cache_op_range(WOP_D_CLEAN, b, size);
	warm_cache_op_range(WOP_I_INVALIDATE, b, size);
}

unsigned months[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

struct tm* localtime(const time_t* input_time)
{
	unsigned int time = *input_time;

	static struct tm tm_result;
	tm_result.tm_isdst = 0;

	tm_result.tm_sec = time % 60;
	time /= 60;

	tm_result.tm_min = time % 60;
	time /= 60;

	tm_result.tm_hour = time % 24;
	time /= 24;

	tm_result.tm_wday = (time + 4) % 7;

	unsigned leap = 0;
	tm_result.tm_year = 70 + (time / (365*4 + 1))*4;
	time %= (365*4 + 1);
	if (time >= (365*3 + 1))
	{
		time -= (365*3 + 1);
		tm_result.tm_year += 3;
	}
	else if (time >= 365*2)
	{
		time -= 365*2;
		tm_result.tm_year += 2;
		leap = 1;
	}
	else if (time >= 365)
	{
		time -= 365;
		tm_result.tm_year++;
	}
	tm_result.tm_yday = time;
	if (leap && time >= 31 + 29 - 1)
	{
		if (time == 31 + 29 - 1)
		{
			tm_result.tm_mon = 1;
			tm_result.tm_mday = 29;
			return &tm_result;
		}
		time--;
	}

	tm_result.tm_mon = 0;
	while(time >= months[tm_result.tm_mon])
		time -= months[tm_result.tm_mon++];
	tm_result.tm_mday = time + 1;
	return &tm_result;
}
