/* The speed of the timer behind nspire_ticks(), checked against the
 * real-time clock (from PocketSNES's tickrate.cpp, issue #13 there).
 *
 * The first SP804 timer is set up to count 32768 times a second, and on the
 * CX II it does. On an older CX it doesn't: games ran slow with the FPS
 * counter at 60/60, which is what a timer counting slower than it should
 * does, as frames are paced and counted by it. So while gpSP runs the timer
 * is timed against the real-time clock's seconds, and if it is more than 5%
 * off, its count is turned into ticks at the meant speed from then on. */
#ifndef NSPIRE_TICKRATE_H
#define NSPIRE_TICKRATE_H

#include <stddef.h>
#include <stdint.h>

/* Starts over, for a timer meant to count 'hz' times a second. */
void tick_rate_start(uint32_t hz);

/* A timer reading 'raw' (counting up) taken when the real-time clock read
 * 'seconds', as ticks at the meant speed. Every reading of the timer comes
 * here: the ones just before and after the clock's second changes time it,
 * over two stretches of 2 seconds or more that have to agree. Until the
 * timer is found to be off the reading comes back as it is; once it is, the
 * ticks carry on from where they were, without a jump. */
uint32_t tick_rate_ticks(uint32_t raw, uint32_t seconds);

/* Checks the timer's speed again from the reading 'raw' on (an older CX's
 * clock switch may change it), carrying on from the ticks it gives now. */
void tick_rate_recheck(uint32_t raw);

/* Takes the timer to count 'hz' a second from the reading 'raw' on (timed
 * some other way: an older CX's CPU clock), carrying on from the ticks it
 * gives now: corrected if that is more than 5% off, the readings as they
 * are if not. The real-time clock check carries on and has the last word. */
void tick_rate_assume(uint32_t raw, uint32_t hz);

/* Done checking (the real-time clock needn't be read any more). */
int tick_rate_checked(void);

/* The timer's speed by the real-time clock (0 until checked), and whether
 * its count is being corrected. */
uint32_t tick_rate_measured(void);
int tick_rate_corrected(void);

/* A line about it, for the speed test results and the perf log. */
void tick_rate_report(char *text, size_t size);

/* R12: the readings start again at 'raw' (a timer started over: the CX II's
 * timer taken back after a come-up's burst), the ticks going on from
 * 'ticks'; the speed found so far kept, the real-time clock's check from
 * here. */
void tick_rate_resume(uint32_t raw, uint32_t ticks);

/* ---- R12 (2026-11-15): the CX II's time base, a count whose speed is not
 * known in advance ----
 *
 * On the CX II gpSP no longer writes the timer block's clock select (it is
 * shared with the OS's USB timer: R12), so its count goes at
 * whatever clock the OS chose: ~10 MHz, 12 MHz, 32.768 MHz, maybe one that
 * follows the CPU's. tick_base turns such a count (64-bit, counting up) into
 * ticks at 32768 a second: from the first reading at the speed given at the
 * start (the CPU clock's timing), then refined against the real-time
 * clock's seconds over ever longer stretches from one sharp start (2, 4,
 * 8 ... 1024 s; each needs the readings around both ends' second changes
 * within 1/1000 of it together, so a refinement is within 0.05%: with
 * readings 1 ms apart from 2 s on, 16 ms apart from 32 s on), the ticks
 * carrying on from where they are
 * at each change (no jump), never back. */
void tick_base_start(uint64_t raw, uint32_t hz, uint32_t ticks);
uint32_t tick_base_ticks(uint64_t raw, uint32_t seconds);
/* The speed may have changed (a CPU clock switch): timed again from here on
 * (the speed in use kept until then; a stretch more than 10% off it is
 * taken once a second one agrees with it within 0.5%). */
void tick_base_recheck(void);
/* Done refining: the real-time clock needn't be read for it any more. */
int tick_base_checked(void);
/* The speed in use, in counts a second x 65536, and rounded. */
uint64_t tick_base_hz_q16(void);
uint32_t tick_base_hz(void);
/* How many refinements and disagreements so far. */
uint32_t tick_base_refinements(void);
uint32_t tick_base_disagreements(void);
/* A line about it (the speed at start and now, each refinement), for the
 * logs and the speed test results. */
void tick_base_report(char *text, size_t size);

/* Following a down-counter gpSP may not own (the OS's, "borrowed"), or its
 * own one the OS might change: each reading becomes a count since the start
 * (64-bit, counting up), across its wraps (from 'top': a periodic timer's
 * load, a free-running one's 0xFFFFFFFF). A step that would move the count by
 * more than the real time that can have passed since the last reading (the
 * real-time clock's seconds between them, plus one, at the speed given,
 * plus a quarter) is a jump (the counter rewritten): counted, and it moves
 * the count by nothing. Readings a second or more apart get the whole
 * periods that went by unseen added (counted as lost wraps). */
struct tick_follow
{
  uint32_t prev;            /* the last reading */
  uint32_t last_seconds;    /* the real-time clock then */
  uint64_t total;           /* counts since the start */
  uint32_t hz;              /* about how fast it counts */
  uint32_t jumps, lost_wraps;
  uint32_t still;           /* seconds the reading hasn't moved */
  uint32_t jump_from, jump_to;   /* the last jump's readings */
};
void tick_follow_start(struct tick_follow *f, uint32_t value, uint32_t hz, uint32_t seconds);
/* Starts again from 'value' (another timer, or the same one restarted),
 * the count going on from where it is. */
void tick_follow_restart(struct tick_follow *f, uint32_t value, uint32_t seconds);
uint64_t tick_follow_step(struct tick_follow *f, uint32_t value, uint32_t top, uint32_t seconds);

#endif
