/* Frame pacing, frameskip and frame timing (nspire_frameskip.h).
 *
 * gpSP used to wait for the LCD's vertical blank after every frame, drawn or
 * skipped, which tied the game to the LCD's refresh rate and kept automatic
 * frameskip from ever skipping. Now frames are paced with the 32768 Hz timer
 * at the GBA's 59.73 frames a second, and nspire_flip shows each one at the
 * LCD's next refresh without waiting.
 *
 * The rule (2026-11-15, final/pace: at 60 game frames a second or more, all
 * 60 pictures are drawn, none left out where none has to be: from 60 up the
 * picture's smoothness comes first, below 60 the game's speed), automatic
 * frameskip's, the default, in
 * normal play and fast forward alike, with or without a sound adapter:
 *  1. Below 60 game frames a second the game's speed comes first: a picture
 *     is left out when its frame starts half a frame or more behind the
 *     GBA's 59.73 a second (LATE_SKIP), one at a time, so only as many as
 *     that takes; never more than Frameskip value in a row (the floor: one
 *     picture in value + 1; while sound plays with frameskip off, one in 2:
 *     skip_for_sound). With the LCD at a GBA frame (the CX II, nspire.c
 *     lcd_tune) only where the picture would miss its refresh anyway or the
 *     frame is two behind (2026-11-21, final/r19-pace: lcd_picture_misses).
 *  2. From 60 up the pictures come first: 60 a second (normal play: every
 *     frame; fast forward: a picture at each LCD refresh, the game frames
 *     between them as many as the rest of the time holds), never more. Fast
 *     forward's picture is made a refresh ahead and flipped as the LCD takes
 *     the last one (ff_flush, final/r19-pace).
 *  3. Where 60 pictures would hold the game under 60, rule 1 again: fast
 *     forward keeps a clock at the GBA's speed too (ff_full_due) and leaves
 *     out a picture when the game falls half a frame behind it.
 * Off draws every picture (the game slows if it must); Manual draws one in
 * N + 1; without a timer (an older CX whose timers don't run) automatic
 * can't tell a late frame and goes by each real second's count (AUTO_LOW). */

#include "common.h"
#include "main.h"
#include "cpu.h"
#include "nspire.h"
#include "nspire_frameskip.h"
#include "nspire_frontend.h"
#include "nspire_paths.h"
#include "nspire_clock.h"
#include "nspire_sram.h"
#include "nspire_os.h"
#include "nspire_usbport.h"
#include "nspire_audio.h"
#include "nspire_bt_glue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 2026-11-15 (final/snd): while the game's sound goes to a USB adapter,
 * frameskip off is automatic frameskip (the Sound page says so): a frame
 * late is a gap in the sound or a slower one, a picture skipped is not
 * heard. Round 2 (the CX II's R11u log: automatic's levels drew 45 or 30
 * pictures for a shortfall of a few percent): a single picture is left out
 * when a frame starts a quarter of a frame behind its time, never two in a
 * row (begin_frame), so at most one in two (a game too slow even so runs as
 * fast as it can, its sound slower: nspire_audio.c). Manual frameskip and
 * automatic stay as set. Since final/pace that is automatic frameskip's own
 * rule (the top of this file) with a Frameskip value of 1, at half a frame
 * late; an Off is one chosen after the settings' one-time change of a stored
 * Off (nspire_config.c). */
static int skip_for_sound(void)
{
  return current_frameskip_type == no_frameskip && nspire_usbaudio_playing();
}

static u32 skip_type(void)
{
  return skip_for_sound() ? (u32)auto_frameskip : current_frameskip_type;
}

static u32 skip_value(void)
{
  return skip_for_sound() ? 1 : frameskip_value;
}

/* A GBA frame is 280896 cycles at 16.78 MHz. In 16.16 fixed-point ticks of
 * the 32768 Hz timer that is 280896 * 32768 / 2^24 * 2^16 = 280896 * 128. */
#define FRAME_PERIOD (280896u * 128u)
#define FRAME_TICKS  (FRAME_PERIOD >> 16)
/* Automatic frameskip (with a timer), the rule at the top: a picture is
 * left out when its frame starts LATE_SKIP or more behind its time, at most
 * auto_floor() in a row. One at a time, so the pictures left out are the
 * frames' shortfall's own: the pacing model's rule-b, 3% too heavy (17.2 ms
 * drawn, 10.5 skipped), draws 55.7 a second, the 55.7 its costs allow, not
 * the 45 of the levels it had from 2026-11-10 (all, 3 in 4, 2 in 3, 1 in 2
 * ... by the frames' measured costs: the CX II's R11u log read 45 and 30 for
 * a few percent). Half a frame, not round 2's quarter: a skip gains back the
 * drawing's time (a few ms), and from a quarter frame late that ran the
 * frame early and the rest was a wait, a picture lost (rule-b 54.2; the
 * sound's a-moving 59.26, now 59.73). Frames start up to about half a frame
 * behind their time then; the game's speed is the same. (Drawing whenever
 * the game wasn't more than a whole frame late, before 2026-11-10, let it
 * fall a frame behind before a skip, and its counter's "/60" said pictures;
 * the counter now says the game's frames: update_counters.) */
#define LATE_SKIP (FRAME_TICKS / 2)
static u32 frame_began;                /* this frame's start, after any wait (USB audio's work) */
static int frame_began_valid;

/* 2026-11-21 (final/r19-pace; on r18 the pictures drawn varied from second
 * to second with the game at 60 frames or more, where they should hold at a
 * steady 60). Normal play with the LCD at a GBA frame: a CX
 * II's log (r19's hardware log) had 60 game frames a second and 59,
 * 58 ... 47 pictures in half its seconds. The cause: rule 1's half a frame
 * (LATE_SKIP) left out pictures that would have been shown on time. The
 * pictures go out through three buffers: one the LCD shows, one flipped and
 * waiting for the refresh that takes it, one drawn into; a frame's picture
 * waits for the LCD to take the last one (nspire_frameskip_before_flip). So
 * once a picture has come a refresh late (it is shown at the next one, the
 * refresh before holding the last), every later one has a whole refresh in
 * hand: a frame that starts 8-18 ms behind its time, after an OS sleep of up
 * to 11.5 ms (its 10.28 ms tick and its own time) or a few windows, still has
 * its picture ready for the refresh it is due at, and the frames after it
 * catch up with their spare time. The half frame left that picture out
 * anyway: a jump in the motion, and the next refresh after a late frame held
 * the last picture too (the pacing model's steady case: 1-3 pictures a
 * second fewer at 14-16 ms frames, 2-4 ms to spare).
 * Now, with the LCD at a GBA frame and a timer, a picture is left out only
 * where it would miss its refresh anyway (lcd_picture_misses: the refresh
 * after the one that takes the last picture, by the LCD's place, against the
 * drawn frames' measured cost) and the frame is half a frame behind: the
 * picture lost either way, the game gets the time back (rule 1). A frame on
 * time whose picture would miss (a heavy frame, no time in hand yet) is
 * drawn: shown a refresh later, and the next ones have that refresh in hand.
 * The floor (Frameskip value in a row) is the same. Without the LCD's lock
 * (an older CX, a refresh not tuned): LATE_SKIP as before. */
static u32 np_cost, np_spread;     /* a drawn frame, its start to its flip (16ths of ticks; 0: not yet) */
static u32 np_flip_at;             /* the last picture's flip */
static int np_flip_valid;
static u32 np_lcd_ref;             /* a refresh's lines' start (ticks) */
static int np_lcd_known;
/* After a new count (a menu, a state, fast forward switched): the first
 * frame starts at once (it waited a frame before), the second at the
 * refreshes' place (np_snap: lcd_phase_lock's whole step), and the first
 * picture is flipped after a refresh has begun since its frame started
 * (np_start_deep; np_deep_from: that refresh's lines): from then on every
 * picture has a refresh in hand. Fast forward leaves its last picture
 * waiting for the LCD: the first frame starts before it is taken and that
 * is so by itself. Else the refresh at the switch holds the last picture,
 * where the motion changes anyway: before, the first frame late by a little
 * did, in play (pace_test --case steady's toggles: one a switch, 0.05-0.9 s
 * after it). */
static int np_start_deep, np_snap;
/* Frames run ahead (nspire_frameskip_run_ahead, a save's OS call to come):
 * LATE_SKIP alone leaves their pictures out until they are caught up, the
 * time that buys being the point. */
static int np_ahead;
static u32 np_deep_from;
#define NP_MARGIN (NSPIRE_TICK_HZ / 2000)     /* 0.5 ms more to spare */
#define NP_VB_GUARD (NSPIRE_TICK_HZ / 2000)   /* 0.5 ms: the LCD takes its address in the sync and back porch before the lines (7 of 344 lines, 0.34 ms) */


/* The most pictures left out in a row: the Frameskip value (the floor: one
 * picture in value + 1), 1 while sound plays with frameskip off. */
static u32 auto_floor(void)
{
  return skip_value() > 99 ? 99 : skip_value();
}
/* Without a timer (an older CX whose timers don't run), automatic
 * frameskip can't tell a late frame: it draws one in auto_level + 1, and
 * each real second moves auto_level (up to the Frameskip value) by the
 * game's frames that second: up below AUTO_LOW, down above AUTO_HIGH if
 * the level below didn't fall short (AUTO_LOW) when last tried. */
#define AUTO_LOW  58
#define AUTO_HIGH 61
static u32 auto_level, auto_counter;
static u8 auto_level_fps[100];   /* the game's frames a second at each level, last time (0: not tried) */
/* Fast forward (2026-11-12; fewer pictures would buy more speed, but the
 * pictures stay at 60 and the speed is that of game frames truly run, not
 * of pictures left out): never fewer pictures than normal play
 * draws, at even times, and the game as fast as the rest of the time allows
 * (paced to its top speed, nspire_ff_max, if one is set).
 *  - Slots: the pictures' times are LCD refreshes: each one with frameskip
 *    off or automatic (60 a second), else as many as normal play's
 *    frameskip draws (manual N: one in N + 1 refreshes; automatic without
 *    a timer: its level now). (Until final/pace automatic's slots were its
 *    level's too, 3 in 4 refreshes at "3 in 4", where 60 pictures might fit.)
 *  - Automatic (final/pace, the rule's 3): where 60 pictures leave the game
 *    under 60 frames a second, a picture is left out when the game falls
 *    LATE_SKIP behind a clock at the GBA's speed (ff_full_due, never ahead of
 *    now: time to spare isn't banked), at most auto_floor() in a row, as in
 *    normal play; the game then holds 60 (it ran at the drawn frames' speed:
 *    18.5 ms drawn, 54 a second).
 *  - Manual (and automatic without a timer): normal play's pattern is a
 *    floor: a frame it would skip is skipped here too (own counter), so fast
 *    forward is never slower than normal play; a calculator that can't keep
 *    normal speed plays as it does normally, unpaced. Its other frames are
 *    drawn unless the picture would come before the refresh ahead of its slot.
 *  - A slot each refresh (off, automatic): a frame is drawn when skipping it would make the next
 *    picture late for its slot, by the frames' measured costs (ff_cost_*)
 *    with FF_MARGIN, twice their spread and ff_late_margin to spare; all
 *    are drawn if a drawn frame takes a refresh. A picture waits until the
 *    LCD has taken the last one (nspire_frameskip_before_flip): one a slot,
 *    never two close together and then a gap. The game frames between
 *    pictures follow the costs, which change slowly: Emerald's town on a
 *    CX II by the PC estimate (1.73 frames a refresh) runs 2 2 2 1 2 2 1,
 *    not 1 to 4 at random.
 * 2026-11-10 to 11-12 pictures made way for the speed (15 a second at
 * max: CX II town 2.9x by the PC estimate), which looked choppy on the calculator;
 * before that 60 came first by a rule that took what came (1.9x). */
#define FF_MARGIN (NSPIRE_TICK_HZ / 500)                    /* ticks (2 ms) */
#define LCD_PERIOD nspire_lcd_period16()   /* 16.16 ticks a refresh: as measured (nspire.c lcd_tune), else 1/60 s */
/* The names of fast forward's top speeds (nspire_ff_max, in
 * nspire_config.c): 66 to 600 game frames a second in steps of 6, then max
 * (none). (Until final/pace 1.1x to 10.0x in tenths: the same steps, the
 * same index and stored word, and the same speed, fast_forward_limit.) */
const char *const nspire_ff_max_names[NSPIRE_FF_CHOICES] =
{
  "66 fps", "72 fps", "78 fps", "84 fps", "90 fps", "96 fps", "102 fps", "108 fps", "114 fps",
  "120 fps", "126 fps", "132 fps", "138 fps", "144 fps", "150 fps", "156 fps", "162 fps", "168 fps",
  "174 fps", "180 fps", "186 fps", "192 fps", "198 fps", "204 fps", "210 fps", "216 fps", "222 fps",
  "228 fps", "234 fps", "240 fps", "246 fps", "252 fps", "258 fps", "264 fps", "270 fps", "276 fps",
  "282 fps", "288 fps", "294 fps", "300 fps", "306 fps", "312 fps", "318 fps", "324 fps", "330 fps",
  "336 fps", "342 fps", "348 fps", "354 fps", "360 fps", "366 fps", "372 fps", "378 fps", "384 fps",
  "390 fps", "396 fps", "402 fps", "408 fps", "414 fps", "420 fps", "426 fps", "432 fps", "438 fps",
  "444 fps", "450 fps", "456 fps", "462 fps", "468 fps", "474 fps", "480 fps", "486 fps", "492 fps",
  "498 fps", "504 fps", "510 fps", "516 fps", "522 fps", "528 fps", "534 fps", "540 fps", "546 fps",
  "552 fps", "558 fps", "564 fps", "570 fps", "576 fps", "582 fps", "588 fps", "594 fps", "600 fps",
  "max"
};

/* Game frames a second at fast forward's top speed (0: none). */
static u32 ff_max_fps(void)
{
  return nspire_ff_max < NSPIRE_FF_UNLIMITED ? 6u * (nspire_ff_max + 11u) : 0;
}

enum nspire_pace_mode nspire_pace = PACE_NORMAL;

static u32 next_frame, next_frame_frac;
static u32 skipped_frames, manual_counter;
static u32 ff_next, ff_next_frac;
static int ff_started;
/* Fast forward's measured costs (16ths of a tick; 0: not yet): a drawn
 * frame, from its start after any wait to the next one's; what drawing it
 * adds (a drawn frame less a skipped one, learnt at each skipped frame:
 * the skipped frames' cost follows the drawn ones' when a scene changes
 * and none is skipped); the picture's own (nspire_present_frame), the
 * first guess at what drawing adds. And its pictures: the refresh the last
 * one is taken at, the next one's slot. */
static u32 ff_cost_drawn, ff_cost_draw_part, ff_cost_out;
static u32 ff_cost_spread;   /* how far frames fall from the averages (mean distance) */
/* More to spare after a picture that came late with frames skipped before
 * it (16ths of a tick): a skip that cost a picture makes the next ones
 * rarer, for a while. */
static u32 ff_late_margin, ff_skips;
static u32 ff_began, ff_waited;
static int ff_began_valid;
static u32 ff_lcd_ref;           /* a refresh's start (timer ticks) */
static int ff_lcd_known;
static u32 ff_shown_at, ff_slot, ff_slot_acc, ff_phase;
static int ff_slot_known;
/* Automatic's clock at the GBA's speed in fast forward (the rule's 3): when
 * this frame is due at 59.73 a second, never later than now. */
static u32 ff_full_due, ff_full_frac;
/* (final/r19-pace) fast forward's picture made and not flipped yet (ff_flush) */
static int ff_deferred;
static void *ff_defer_screen;
static u32 ff_defer_serial;   /* (r19-fix) nspire_flip_serial then: any flip since, and it is let go */
static void ff_flush(int wait);
static void ff_wake(void);
static int np_taken(void);

struct nspire_perf nspire_perf_totals;
static struct nspire_perf perf_window;   /* totals at the last counter update */
u32 nspire_render_ticks;
int nspire_perf_on;

/* The timer's count (half a second): game frames and pictures a second. */
static u32 fps_start, fps_frames, fps_drawn, fps_shown, fps_drawn_shown;
static int fps_started;

/* The counter counts frames against the real-time clock: the frames that
 * ended in the last whole second it showed. The timer paces the frames, and
 * counting by the timer too made a timer that runs slow, or not at all (an
 * older CX), show 60/60 whatever the game's real speed. Only if the clock
 * doesn't move (no change in RTC_QUIET_FRAMES frames) does the timer's
 * count show. */
#define RTC_QUIET_FRAMES 600
static u32 rtc_second;       /* the second being counted */
static int rtc_state;        /* 0: not started; 1: in the first (partial) second; 2: whole seconds */
static u32 rtc_frames;       /* pictures drawn in rtc_second */
static u32 rtc_game_frames;  /* all game frames ended in it */
static u32 rtc_quiet;        /* frames since the clock last changed */
static int rtc_shown_valid;  /* rtc_shown is a whole second's count */
static u32 rtc_shown, rtc_game_fps;   /* the last whole second's pictures, game frames */
/* (final/r19-pace; the readout's number is not to jump up and down) The
 * readout's numbers: a game at the LCD's 59.6-59.9 a second ends 59 or 60
 * frames in a whole second by where its edges fall (60 60 59 60 61 ... in
 * an r18 hardware log at a steady rate), and the readout said each. Now the
 * mean of the last RO_SECONDS whole seconds, rounded: shown when it is
 * within RO_NEAR tenths of that whole number, or RO_HOLD tenths or more from
 * what is shown (a mean between two numbers keeps the one shown); a second
 * RO_JUMP (over 61, a tenth) or more from what is shown is a real change (fast
 * forward switched, a heavy scene) and is shown at once, the mean starting
 * over from it (r19-fix: RO_JUMP 2, was 3). Still each count a real second's (the real-time clock's).
 * The pacing model (pace_test --case steady): the readout changed 11-20
 * times in 20 s of a steady game, now 0 in normal play. */
/* (r19-fix, core fix 8: a change of 2 read through the mean: a game at 58 said 60 for 3 s,
 * then 59 for 3 s; pictures down to 58 said all of them for 3 s. RO_JUMP 2: shown at the second's end.
 * The steady game's whole seconds are 59, 60 or 61 by where their edges fall: within 1, the mean's.
 * The pictures' change from all of them (or one fewer: not said) to fewer, or back, by the frames a
 * second left out (the game's count less the pictures', the same second's: no edges in it), 2 more or
 * fewer than shown; their number then the game's shown less those (not the second's own count, 1 off by
 * its edges). A picture lost in a second with its edges low (58 of 59) is no change; pictures that
 * swing by more each second (a game that doesn't fit) as before: the mean, 3 at once. Fast forward as
 * before. The game's change starts both over, the pictures' only theirs.
 * pace_test --case readout.) */
#define RO_SECONDS 8
#define RO_HOLD    8     /* tenths */
#define RO_NEAR    35    /* hundredths */
#define RO_JUMP    2
static u16 ro_game[RO_SECONDS], ro_drawn[RO_SECONDS];
static u32 ro_n, ro_dn, ro_game_shown, ro_drawn_shown;   /* (ro_dn: the pictures' own count of seconds) */

#define PERF_LOG_LINES 600
static char perf_log[PERF_LOG_LINES][128];
static int perf_log_count;
/* The game loop's stack where the counter's line was taken (layout line). */
static uintptr_t perf_log_sp;

extern u8 *gamepak_buffers[32];

/* Where Ndless put the program (its malloc decides) and the stack: the
 * cache sets the hot code, the GBA's memory, the dynarec's blocks, the
 * stack and the ROM buffer share depend on these addresses modulo the
 * caches' ways (4 KB instruction, 2 KB data). With the layout pinned,
 * the speed model runs the program's own placement of these against each
 * other; the base and the stack are what it can't know. */
static void layout_report(char *text, u32 size)
{
#ifdef HAVE_DYNAREC
  uintptr_t jit = (uintptr_t)rom_translation_cache;
#else
  uintptr_t jit = 0;   /* the PC tests' build: no dynarec */
#endif
  uintptr_t code = (uintptr_t)update_gba, data = (uintptr_t)reg;
  uintptr_t rom = (uintptr_t)gamepak_buffers[0];

  snprintf(text, size, "layout: update_gba %08x, reg %08x, jit %08x, stack %08x, rom %08x;"
           " in a 4 KB way %03x %03x %03x %03x %03x",
           (unsigned)code, (unsigned)data, (unsigned)jit, (unsigned)perf_log_sp, (unsigned)rom,
           (unsigned)(code & 0xfff), (unsigned)(data & 0xfff), (unsigned)(jit & 0xfff),
           (unsigned)(perf_log_sp & 0xfff), (unsigned)(rom & 0xfff));
}

/* The dynarec's PC tables and caches (core-17, R3/R4):
 * a lookup not found resumes or reads at a stale PC (never seen; anything
 * but 0 is a bug), and how often the 2 MB ROM cache filled (each flush
 * means translating again: a stutter). */
#ifdef HAVE_DYNAREC
u32 nspire_mem_pc_stats(u32 which);
#endif
static void dynarec_report(char *text, u32 size)
{
#ifdef HAVE_DYNAREC
  snprintf(text, size, "dynarec: pc lookups not found %u, cache flushes ROM %u RAM %u, ROM cache %u KB in use;"
           " RAM blocks copied %u, kept emptied %u",
           (unsigned)nspire_mem_pc_stats(3), (unsigned)nspire_mem_pc_stats(5),
           (unsigned)nspire_mem_pc_stats(4), (unsigned)(nspire_mem_pc_stats(6) >> 10),
           (unsigned)nspire_mem_pc_stats(7), (unsigned)nspire_mem_pc_stats(8));
#else
  snprintf(text, size, "dynarec: none");
#endif
}

/* The memory's speed at the game's clock (for the speed model's
 * calibration): the speed model's timings for an older CX (a line fill 90
 * cycles at 192 MHz = 469 ns, a store through the write buffer 5 = 26 ns;
 * CX II measured: 120 at 396 = 303 ns, 6 = 15 ns) are guesses, and the CX
 * CAS runs 1.5x slower than the model says. Once, at the Detailed counter's
 * first second (about 15 ms, taken out of that second's numbers): 8 loads a
 * loop, a line apart, over 32 KB (4x the data cache: every load a line
 * fill) against the same over 4 KB (all hits), then 8 stores a loop over
 * another 32 KB never read (the ARM926 doesn't allocate on a store: each
 * one goes through the write buffer) against stores to the cached 4 KB.
 * The buffer is malloc'd, word-aligned, only its own 64 KB touched. */
#define PROBE_HALF 32768u
static char mem_line[160];
static u32 probe_sink;

static __attribute__((noinline)) u32 probe_loads(const volatile u32 *b, u32 mask, u32 n)
{
  u32 sum = 0, off = 0;
  while (n--)
  {
    const volatile u32 *p = b + off;
    sum += p[0] + p[8] + p[16] + p[24] + p[32] + p[40] + p[48] + p[56];
    off = (off + 64) & mask;
  }
  return sum;
}

static __attribute__((noinline)) void probe_stores(volatile u32 *b, u32 mask, u32 n)
{
  u32 off = 0;
  while (n--)
  {
    volatile u32 *p = b + off;
    p[0] = n; p[8] = n; p[16] = n; p[24] = n; p[32] = n; p[40] = n; p[48] = n; p[56] = n;
    off = (off + 64) & mask;
  }
}

static u64 probe_ns(u32 ticks)
{
  return (u64)ticks * 1000000000ull / NSPIRE_TICK_HZ;
}

/* Returns the ticks it took (left out of the counter's second). */
static u32 mem_report(char *text, u32 size)
{
  const u32 n = 2048, near_mask = 4096 / 4 - 1, far_mask = PROBE_HALF / 4 - 1;
  u32 *buf = (u32 *)malloc(2 * PROBE_HALF);
  u32 start = nspire_ticks(), t0, hit_l, miss_l, hit_s, miss_s;

  if (!buf)
  {
    snprintf(text, size, "mem: not measured (no 64 KB free)");
    return nspire_ticks() - start;
  }
  probe_sink += probe_loads(buf, far_mask, n / 4);    /* the cache: nothing of the first half dirty */
  probe_sink += probe_loads(buf, near_mask, 64);      /* its first 4 KB in the cache */
  t0 = nspire_ticks(); probe_sink += probe_loads(buf, near_mask, 8 * n); hit_l = nspire_ticks() - t0;
  t0 = nspire_ticks(); probe_sink += probe_loads(buf, far_mask, n);      miss_l = nspire_ticks() - t0;
  probe_sink += probe_loads(buf, near_mask, 64);
  t0 = nspire_ticks(); probe_stores(buf, near_mask, 8 * n);                 hit_s = nspire_ticks() - t0;
  t0 = nspire_ticks(); probe_stores(buf + PROBE_HALF / 4, far_mask, 8 * n); miss_s = nspire_ticks() - t0;
  free(buf);
  if (!hit_l || !miss_l || !miss_s)
    snprintf(text, size, "mem: not timed (ticks %u %u %u %u)", (unsigned)hit_l, (unsigned)miss_l,
             (unsigned)hit_s, (unsigned)miss_s);
  else
  {
    /* ns a loop of 8, then a line fill / a store: the miss loop less the hit loop, over 8 */
    s64 load_loop = (s64)(probe_ns(miss_l) / n), load_hit = (s64)(probe_ns(hit_l) / (8 * n));
    s64 store_loop10 = (s64)(probe_ns(miss_s) * 10 / (8 * n)), store_hit10 = (s64)(probe_ns(hit_s) * 10 / (8 * n));
    s64 fill = (load_loop - load_hit) / 8, store10 = (store_loop10 - store_hit10) / 8;
    snprintf(text, size, "mem: line fill %d ns, store miss %d.%d ns (loops of 8: loads %d/%d ns, stores %d/%d ns;"
             " ticks %u %u %u %u)", (int)fill, (int)(store10 / 10), (int)(store10 < 0 ? -store10 % 10 : store10 % 10),
             (int)load_loop, (int)load_hit, (int)(store_loop10 / 10), (int)(store_hit10 / 10),
             (unsigned)hit_l, (unsigned)miss_l, (unsigned)hit_s, (unsigned)miss_s);
  }
  return nspire_ticks() - start;
}

/* P2: the OS's time outside the idle wait so far, into the totals. */
static void perf_note_os(void)
{
  uint32_t n;

  nspire_perf_totals.os = nspire_os_busy_slept(&n);
  nspire_perf_totals.os_sleeps = n;
}

void nspire_reset_timing(void)
{
  ff_flush(1);     /* (a fast forward picture waiting for the LCD: shown) */
  rtc_state = 0;   /* a new count; the last one stays shown until then */
  next_frame = nspire_ticks();
  next_frame_frac = 0;
  skipped_frames = 0;
  ff_started = 0;
  ff_waited = 0;
  fps_started = 0;
  frame_began_valid = 0;
  np_flip_valid = 0;
  np_start_deep = 1;
  np_deep_from = 0;
  np_snap = 2;
  np_ahead = 0;
  memset(&nspire_perf_totals, 0, sizeof(nspire_perf_totals));
  nspire_perf_totals.start = next_frame;
  perf_note_os();
  perf_window = nspire_perf_totals;
  nspire_render_ticks = 0;
  num_skipped_frames = 0;
  relative_frame_count = 0;
}

void nspire_frameskip_run_ahead(u32 frames)
{
  u64 t;

  if (nspire_pace != PACE_NORMAL || !synchronize_flag)
    return;
  t = (((u64)next_frame << 16) | next_frame_frac) - (u64)frames * FRAME_PERIOD;
  next_frame = (u32)(t >> 16);
  next_frame_frac = (u32)t & 0xFFFF;
  np_ahead = 1;   /* (final/r19-pace: these frames' pictures left out by LATE_SKIP, as before) */
}

/* (r19-snd) The reverse, for nspire_audio.c's save bank: the next frames due
 * frames later (the frame loop waits that much more: a picture shown a
 * refresh longer), the time an OS call took not made up by frames run back
 * to back, or the sound a save ran ahead given back. */
void nspire_frameskip_hold(u32 frames)
{
  u64 t;

  if (nspire_pace != PACE_NORMAL || !synchronize_flag)
    return;
  t = (((u64)next_frame << 16) | next_frame_frac) + (u64)frames * FRAME_PERIOD;
  next_frame = (u32)(t >> 16);
  next_frame_frac = (u32)t & 0xFFFF;
}

/* (r19-snd) Normal play, at a frame's end: how far the next frame is behind
 * its time now (ticks, 0 if it would wait or not paced): a run ahead not yet
 * made up. (next_frame is the frame's own time since its start.) */
/* (final/r19-bank) Normal play, at a frame's end: the time until the next
 * frame is due (ticks; 0 when it is due or late, or not paced): what a save's
 * CPU part may take without making the next frame late. */
u32 nspire_frameskip_spare(void)
{
  s32 early;

  if (nspire_pace != PACE_NORMAL || !synchronize_flag)
    return 0;
  early = (s32)((next_frame + (u32)((next_frame_frac + FRAME_PERIOD) >> 16)) - nspire_ticks());
  return early > 0 ? (u32)early : 0;
}

u32 nspire_frameskip_behind(void)
{
  s32 late;

  if (nspire_pace != PACE_NORMAL || !synchronize_flag)
    return 0;
  late = (s32)(nspire_ticks() - (next_frame + (u32)((next_frame_frac + FRAME_PERIOD) >> 16)));
  return late > 0 ? (u32)late : 0;
}

void nspire_frameskip_reset(void)
{
  nspire_reset_timing();
  /* A new game (or a setting changed): the levels' speeds start over. */
  auto_level = auto_counter = 0;
  memset(auto_level_fps, 0, sizeof(auto_level_fps));
  ff_cost_drawn = ff_cost_draw_part = ff_cost_out = ff_cost_spread = ff_late_margin = 0;
  np_cost = np_spread = 0;
  np_lcd_known = 0;
}

/* Called by the core's reset_gba. */
void nspire_fps_overlay_reset(void)
{
  fps_started = 0;
  fps_shown = 0;
  rtc_state = 0;
  rtc_shown_valid = 0;
  rtc_shown = rtc_game_fps = 0;
  fps_drawn_shown = 0;
  ro_n = ro_dn = ro_game_shown = ro_drawn_shown = 0;
}

struct nspire_perf_stats nspire_perf_since(const struct nspire_perf *since, u32 now)
{
  struct nspire_perf_stats s;
  u32 elapsed = now - since->start;
  u32 frames = nspire_perf_totals.frames - since->frames;
  u32 render = nspire_perf_totals.render - since->render;
  u32 output = nspire_perf_totals.output - since->output;
  u32 idle = nspire_perf_totals.idle - since->idle;
  u32 os = nspire_perf_totals.os - since->os;
  u32 cpu = elapsed > render + output + idle + os ? elapsed - render - output - idle - os : 0;
  u64 per_frame = (u64)NSPIRE_TICK_HZ * (frames ? frames : 1);

  s.game_fps = elapsed ? (u32)((u64)frames * NSPIRE_TICK_HZ * 10 / elapsed) : 0;
  s.drawn_fps = elapsed ? (u32)((u64)(nspire_perf_totals.drawn - since->drawn) * NSPIRE_TICK_HZ * 10 / elapsed) : 0;
  s.cpu_ms = (u32)(cpu * 10000ull / per_frame);
  s.render_ms = (u32)(render * 10000ull / per_frame);
  s.output_ms = (u32)(output * 10000ull / per_frame);
  s.idle_ms = (u32)(idle * 10000ull / per_frame);
  s.os_ms = (u32)(os * 10000ull / per_frame);
  s.os_sleeps = nspire_perf_totals.os_sleeps - since->os_sleeps;
  return s;
}

/* At most 9999.9 (no count or time a frame gets near it), so the
 * counter's line always fits its 80 bytes. */
#define TENTHS(v) (unsigned)((v) / 10 % 10000), (unsigned)((v) % 10)

void nspire_perf_log_flush(void)
{
  char path[600];
  FILE *f;
  int i;

  /* 2026-11-15: the USB audio's seconds too (only once it is stopped, at
   * the quit: nspire_audio_log_write), Detailed counter or not. While an
   * adapter is open nothing is written (as the link's log, R11t: a file
   * write stalls the USB callbacks 100-290 ms, more than the queue holds):
   * the lines stay for the next flush, the quit's after the audio's stop. */
  if (nspire_usbaudio_active())
    return;
  if (!perf_log_count && !nspire_audio_log_pending())
    return;
  /* A new file each time: appending has garbled files on the calculator. */
  nspire_save_file(path, sizeof(path), "gpsp_perf.txt.tns");
  f = fopen(path, "w");
  if (f)
  {
    char timer[120], clock[300], layout[160], dynarec[200], sram[160], ostick[260];
    nspire_log_buffered(f);   /* (r19-usb-exit: the sound's seconds, 43 KB on the CX, 16 KB a call) */
    nspire_timer_report(timer, sizeof(timer));
    nspire_os_tick_rule_report(ostick, sizeof(ostick));   /* (speed9a: which rule gives the OS tick's phase) */
    nspire_clock_report(clock, sizeof(clock));   /* which CPU and bus speed ran */
    layout_report(layout, sizeof(layout));
    dynarec_report(dynarec, sizeof(dynarec));
    nspire_sram_report(sram, sizeof(sram));
    fprintf(f, "%s (%s): every second, ms per frame\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n", gamepak_title, gamepak_code,
            timer, clock, layout, sram, mem_line[0] ? mem_line : "mem: not measured", dynarec, nspire_keys_line(), ostick);
    for (i = 0; i < perf_log_count; i++)
      fprintf(f, "%s\n", perf_log[i]);
    nspire_audio_log_write(f);
    fclose(f);
  }
  perf_log_count = 0;
}

/* Fast forward: the player's, or the speed test's. */
static int in_fast_forward(void)
{
  return nspire_pace == PACE_FAST_FORWARD || (nspire_pace == PACE_NORMAL && !synchronize_flag);
}

/* Whether automatic frameskip goes by the frames' lateness, the rule (it
 * needs the timer). */
static int auto_by_time(void)
{
  return skip_type() == auto_frameskip && nspire_timer_runs();
}

/* 16.16 timer ticks per frame at fast forward's top speed, or 0 without
 * one (the speed test's fast forward has none). The menu's number in the
 * readout's own measure, where 60 is the GBA's 59.73: "120 fps" runs
 * 120 x 59.73 / 60 = 119.5 a second, the 2.0x it was (the same speed for
 * the same stored value). Paced at 120.0 exactly, the game's frames locked
 * to the LCD's 60 refreshes and the frames between pictures went 1 and 3
 * where they go 2 (tests/host test_fast_forward_even's battle at 2.0x). */
static u32 fast_forward_limit(void)
{
  if (nspire_pace != PACE_NORMAL || !ff_max_fps())
    return 0;
  return (u32)((u64)FRAME_PERIOD * 60u / ff_max_fps());
}

/* Fast forward's top speed: ff_next is when this frame is due at it. An
 * early frame waits for it, a late one just goes (at most a frame's lag is
 * made up). */
static void fast_forward_pace(u32 limit)
{
  u32 now = nspire_ticks();
  s32 ahead;

  if (!limit)
    return;
  ff_next_frac += limit;
  ff_next += ff_next_frac >> 16;
  ff_next_frac &= 0xFFFF;
  ahead = (s32)(ff_next - now);
  if (ahead > 0)
  {
    nspire_wait_until(ff_next);
    nspire_perf_totals.idle += nspire_ticks() - now;
  }
  else if (ahead < -(s32)FRAME_TICKS)
    ff_next = now - FRAME_TICKS;
}

/* An average moved by 1/8 of a new value (the first one taken as it is). */
static void ff_average(u32 *cost, u32 value)
{
  *cost = *cost ? (u32)((s32)*cost + (((s32)value - (s32)*cost) >> 3)) : value;
}

/* The LCD's next refresh's lines from now (ticks): read, or by the refreshes
 * seen before (in the porches the LCD's place says nothing) */
static int np_next_lines(u32 now, u32 *next)
{
  u32 to = nspire_lcd_ticks_to_refresh(), period = LCD_PERIOD >> 16;
  s32 d;

  if (to)
  {
    np_lcd_ref = now + to;
    np_lcd_known = 1;
    *next = now + to;
    return 1;
  }
  if (!np_lcd_known || !period)
    return 0;
  d = (s32)(now - np_lcd_ref);
  if (d < 0)
    *next = np_lcd_ref;
  else
    *next = np_lcd_ref + (u32)(((u64)((u32)d / period) + 1) * period);
  return 1;
}

/* Whether a picture drawn by the frame starting now would come after the
 * refresh it is due at (the one after the refresh that takes the last
 * picture): by the drawn frames' cost, their spread and NP_MARGIN to spare. -1: not
 * known (no cost measured yet, the LCD's place not known). */
static int lcd_picture_misses(u32 now)
{
  u32 next, period = LCD_PERIOD >> 16, due, ready;
  int taken;

  if (!np_cost || !nspire_lcd_at_gba_rate() || !np_next_lines(now, &next))
    return -1;
  taken = np_taken();
  due = next + (taken ? 0 : period) - NP_VB_GUARD;
  ready = now + ((np_cost + np_spread) >> 4) + NP_MARGIN;
  return (s32)(ready - due) > 0;
}

/* Two frames behind: the game first, whatever the pictures (a frame behind
 * is what a refresh in hand allows; past that a game that can't hold its
 * time has nothing to catch up with). */
#define NP_LAG_SKIP (FRAME_TICKS * 2)

/* The game's sound has less in its ring than NP_RING_LOW: a frame behind is
 * that much less sound to play (the save's first parts in the pacing model
 * ran it dry at 10-13 ms; 12-25 hold it, 16 and over lose a picture now
 * and then to it in the steady cases with sound), so LATE_SKIP gets the
 * time back as before. */
#define NP_RING_LOW 12
static int np_ring_low(void)
{
  int ms = nspire_audio_ring_level_ms();
  return ms >= 0 && ms < NP_RING_LOW;
}

/* A drawn frame's cost into the averages (normal play). Each one at most
 * NP_CLAMP from the average: an OS sleep in a frame (up to a tick and the
 * OS's own time, 11.5 ms on the CX II) says nothing of the next frame's
 * cost, and in the average it would leave pictures out after it. */
#define NP_CLAMP (NSPIRE_TICK_HZ / 500)   /* 2 ms */
static void np_measure(u32 spent)
{
  if (spent > FRAME_TICKS * 4)
    spent = FRAME_TICKS * 4;   /* (a pause: a state saved, the menu) */
  spent <<= 4;
  if (np_cost)
  {
    if (spent > np_cost + (NP_CLAMP << 4))
      spent = np_cost + (NP_CLAMP << 4);
    else if (spent + (NP_CLAMP << 4) < np_cost)
      spent = np_cost - (NP_CLAMP << 4);
    ff_average(&np_spread, (spent > np_cost ? spent - np_cost : np_cost - spent) + 1);
  }
  ff_average(&np_cost, spent);
}

/* A skipped frame's cost by the averages: a drawn one less what drawing
 * adds (at first twice the picture's time: drawing the lines takes about
 * as long as the picture on both calculators), at least an eighth. */
static u32 ff_skipped_cost(void)
{
  u32 part = ff_cost_draw_part ? ff_cost_draw_part : ff_cost_out * 2;
  u32 cost = ff_cost_drawn > part ? ff_cost_drawn - part : 0;

  return (cost > ff_cost_drawn / 8 ? cost : ff_cost_drawn / 8) >> 4;
}

/* The last frame's cost into fast forward's averages, and how far it fell
 * from them. */
static void ff_measure(u32 now)
{
  u32 spent;

  if (!ff_began_valid)
    return;
  spent = now - ff_began - ff_waited;
  if (spent > FRAME_TICKS * 8)
    spent = FRAME_TICKS * 8;   /* a pause (a state saved...), not the game */
  spent <<= 4;
  if (ff_cost_drawn)
  {
    u32 expected = skip_next_frame ? ff_skipped_cost() << 4 : ff_cost_drawn;
    ff_average(&ff_cost_spread, (spent > expected ? spent - expected : expected - spent) + 1);
  }
  if (!skip_next_frame)
    ff_average(&ff_cost_drawn, spent);
  else if (ff_cost_drawn)
    ff_average(&ff_cost_draw_part, ff_cost_drawn > spent ? ff_cost_drawn - spent : 1);
}

/* The LCD's refreshes: one starts nspire_lcd_ticks_to_refresh() from now
 * (0 in the porches: nothing learnt), the others each LCD_PERIOD. */
static void ff_lcd_note(u32 now)
{
  u32 ticks = nspire_lcd_ticks_to_refresh();

  if (ticks)
  {
    ff_lcd_ref = now + ticks;
    ff_lcd_known = 1;
  }
}

/* The first refresh at or after t. */
static u32 ff_refresh_after(u32 t)
{
  s32 d = (s32)(t - ff_lcd_ref);
  s64 n;

  if (!ff_lcd_known)
    return t;
  if (d >= 0)
    n = (s64)((((u64)d << 16) + LCD_PERIOD - 1) / LCD_PERIOD);
  else
    n = -(s64)(((u64)(u32)-d << 16) / LCD_PERIOD);
  return n >= 0 ? ff_lcd_ref + (u32)(((u64)n * LCD_PERIOD) >> 16)
                : ff_lcd_ref - (u32)(((u64)-n * LCD_PERIOD) >> 16);
}

/* Normal play's frameskip as fast forward follows it: draws frames of each
 * cycle (the first ones), and so draws pictures in cycle refreshes. */
static void ff_pattern(u32 *cycle, u32 *draws)
{
  *cycle = *draws = 1;
  if (nspire_pace != PACE_NORMAL)
    return;   /* the speed test's: frameskip off */
  if (skip_type() == auto_frameskip)
  {
    /* with a timer a slot each refresh (the rule: ff_full_due does the rest) */
    if (!nspire_timer_runs())
      *cycle = (auto_level > skip_value() ? skip_value() : auto_level) + 1;
  }
  else if (skip_type() == manual_frameskip)
    *cycle = (skip_value() > 99 ? 99 : skip_value()) + 1;
}

static int ff_defers(void);
static u32 ff_target(void)
{
  /* (final/r19-pace) deferred pictures: made as the LCD takes the last one
   * (ff_shown_at), flipped at the first frame's start after it, with the
   * refresh up to the slot in hand for an OS sleep or a heavy frame */
  if (ff_defers())
    return ff_shown_at;
  return ff_slot - FF_MARGIN;
}

/* Fast forward: whether this frame is drawn (see FF_MARGIN). */
static int fast_forward_draw(u32 limit)
{
  u32 cycle, draws, phase, t, drawn;
  u32 now = nspire_ticks();

  ff_pattern(&cycle, &draws);
  phase = ff_phase < cycle ? ff_phase : 0;
  ff_phase = phase + 1 < cycle ? phase + 1 : 0;
  if (phase >= draws)
    return 0;   /* normal play skips it */
  if (ff_deferred)
  {
    /* (final/r19-pace) a picture made and waiting for the LCD: flipped if
     * the LCD has taken the last one, else this frame is skipped (a drawn
     * frame of a refresh or more waits for it) */
    ff_flush(0);
    if (ff_deferred)
    {
      if (ff_cost_drawn && (ff_cost_drawn >> 4) < (LCD_PERIOD >> 16))
        return 0;
      ff_flush(1);
    }
    now = nspire_ticks();
  }
  if (!ff_slot_known || !ff_cost_drawn)
    return 1;
  drawn = ff_cost_drawn >> 4;
  if (cycle > 1)
  {
    /* Frameskip on (an older CX): its frames are drawn unless the picture
     * would come before the refresh ahead of its slot. There a picture
     * takes longer than a slot's share of a refresh or two, so waiting for
     * the last moment would miss slots. */
    return (s32)(now + drawn + FF_MARGIN - (ff_slot - (LCD_PERIOD >> 16))) > 0;
  }
  /* Frameskip off: a drawn frame of a refresh or more draws them all (as
   * normal play: skipping one would leave a refresh without a picture).
   * Else if this one were skipped, the next (after its wait at the top
   * speed) would be drawn: if that would end too late for the slot, this
   * is the picture. Twice the frames' spread to spare, and FF_MARGIN.
   * (F3, 2026-11-12: a refresh less FF_MARGIN drew them all from
   * 14.7 ms on, where a skipped frame still fits between some pictures;
   * the test below already spares FF_MARGIN, and from there on twice
   * the spread more: with only the usual, the wild-battle wipe's uneven
   * frames lost 2 pictures a second for 1.8 game frames.) */
  if (drawn >= (LCD_PERIOD >> 16))
    return 1;
  t = now + ff_skipped_cost();
  if (limit)
  {
    u32 due = ff_next + (u32)(((u64)limit + ff_next_frac) >> 16);
    if ((s32)(due - t) > 0)
      t = due;
  }
  t += drawn + (ff_cost_spread >> 3) + (ff_late_margin >> 4);
  if (drawn + FF_MARGIN >= (LCD_PERIOD >> 16))
    t += ff_cost_spread >> 3;
  if ((s32)(t - ff_target()) > 0)
    return 1;
  ff_skips++;
  return 0;
}

/* nspire_frameskip.h: in fast forward a picture never replaces one the LCD
 * hasn't taken yet (it takes the newest at each refresh, so the other would
 * never be seen: two pictures close together, then a gap). */
static void fast_forward_shown(u32 now);

/* (final/r19-pace) Fast forward's picture made before the LCD has taken the
 * last one: flipped later, at a frame's start once it has (ff_flush), the
 * game frames between running meanwhile (they draw nothing into the screen
 * buffer). ff_defer_screen (above): the buffer it is in (nspire_screen
 * then): if that moved, something else flipped (a menu) and it is let go. */
/* the LCD has taken the last picture: its refresh has come, and the LCD
 * reads it (in the porches after that refresh's start it can't say, and the
 * time says it) */
static int ff_last_taken(void)
{
  if ((s32)(ff_shown_at + 1 - nspire_ticks()) > 0)
    return 0;
  return nspire_flip_taken() || !nspire_lcd_ticks_to_refresh();
}

/* Fast forward's wait for the LCD to take the last picture (idle time) */
static u32 ff_wait_taken(void)
{
  u32 start = nspire_ticks(), now = start;

  if ((s32)(ff_shown_at + 1 - now) > 0 && (s32)(ff_shown_at + 1 - now) < (s32)FRAME_TICKS * 2)
    nspire_wait_until(ff_shown_at + 1);
  /* The rest is idle too: while the link or a pad wants the OS, it gets
   * that time as in nspire_wait_until, up to a refresh after the one that
   * takes it, less FF_MARGIN (F4, 2026-11-12: a spin the OS never
   * had). */
  nspire_os_idle = 1;
  while (!nspire_flip_taken() && nspire_ticks() - start < FRAME_TICKS * 2)
  {
    if ((s32)(nspire_ticks() - ff_shown_at) > 0 && !nspire_lcd_ticks_to_refresh())
      break;
    if (nspire_os_wanted)
      nspire_os_sleep_before(ff_shown_at + (LCD_PERIOD >> 16) - FF_MARGIN);
  }
  nspire_os_idle = 0;
  now = nspire_ticks() - start;
  nspire_perf_totals.idle += now;
  return now;
}

/* Normal play: the LCD has taken the last picture (in the lines the LCD's
 * place says it; in the porches the time: flipped before the refresh that
 * has just been read began) */
static int np_taken(void)
{
  u32 next, period = LCD_PERIOD >> 16;
  int known;

  if (!np_flip_valid)
    return 1;
  known = np_next_lines(nspire_ticks(), &next);   /* (the refreshes' place, kept: the porches need it) */
  if (nspire_flip_taken())
    return 1;
  if (nspire_lcd_ticks_to_refresh() || !known)
    return 0;   /* (the lines of the picture before; or the porches with the refreshes not known yet: soon) */
  return (s32)(np_flip_at - (next - period - NP_VB_GUARD)) < 0;
}

/* Normal play's wait for the LCD to take the last picture (idle time; at
 * most a refresh and FF_MARGIN). 2026-11-17 (final/pace6; a CX II
 * with sound, 60/50-55): it slept in the OS up to its next tick (0-10 ms and
 * the OS's own time) whenever the OS was wanted (a sound adapter, a hub),
 * though the LCD takes the last picture at its next refresh's lines, often
 * a millisecond away: a sleep only when the OS's tick comes 2 ms before that
 * (none in the porches, where it's under 2 ms). */
static void np_wait_taken(void)
{
  u32 start = nspire_ticks();

  nspire_os_idle = 1;
  while (nspire_ticks() - start < (LCD_PERIOD >> 16) + FF_MARGIN)
  {
    /* (the LCD's place before the check: lines read after it are the
     * last picture's, and the next refresh is not the one to wait for) */
    u32 to = nspire_lcd_ticks_to_refresh();
    if (nspire_flip_taken())
      break;   /* (the LCD's place only: np_taken's guess in the porches lost pictures in gpsp-host's LCD) */
    if (nspire_os_wanted && to)
      nspire_os_sleep_before(nspire_ticks() + to);
  }
  nspire_os_idle = 0;
  nspire_perf_totals.idle += nspire_ticks() - start;
}

/* A deferred picture flipped: now if the LCD has taken the last one, else
 * (wait) once it has, else left for later (a frame's point, an OS sleep's
 * wake, the next frame's start). */
/* (r19-fix, core fix 8) a held-back picture counted drawn when it is flipped (end_frame leaves
 * it out): one let go never is */
static void ff_count_shown(void)
{
  nspire_perf_totals.drawn++;
  if (rtc_state)
    rtc_frames++;
  if (fps_started)
    fps_drawn++;
}

static void ff_drop(void)
{
  ff_deferred = 0;
  nspire_os_wake_hook = 0;
}

static void ff_flush(int wait)
{
  if (!ff_deferred)
    return;
  /* (r19-fix, core fix 6: by the buffer's pointer alone a menu that flipped three times, or
   * six, left the same pointer, and its own picture there was flipped as the game's) */
  if (nspire_flip_serial != ff_defer_serial || nspire_screen != ff_defer_screen)
  {
    ff_drop();   /* (flipped since: a menu's picture) */
    return;
  }
  if (!ff_last_taken())
  {
    if (!wait)
      return;
    /* (r19-fix, core fix 1) no flip from the wait's own sleeps: with the hook set, an OS
     * sleep in ff_wait_taken woke into ff_flush(0), which flipped it, and this flipped again: the
     * buffer after it (a picture two back) shown for a refresh. Flipped once, here, after the wait. */
    nspire_os_wake_hook = 0;
    (void)ff_wait_taken();
    if (!ff_deferred)
      return;
  }
  ff_deferred = 0;
  nspire_os_wake_hook = 0;
  nspire_flip();
  ff_count_shown();
  fast_forward_shown(nspire_ticks());
}

/* (an OS sleep woke: nspire_os_wake_hook) */
static void ff_wake(void)
{
  ff_flush(0);
}

/* nspire_frameskip.h (r19-fix, core fix 6): the menu and the other screens take it over */
void nspire_frameskip_screen_taken(void)
{
  ff_flush(0);
  if (ff_deferred)
    ff_drop();
}

/* Whether fast forward defers its pictures: the player's fast forward with a
 * picture at each refresh (frameskip automatic with the timer, or off), no
 * top speed set, the LCD at a GBA frame (the CX II) */
static int ff_defers(void)
{
  u32 cycle, draws;

  if (nspire_pace != PACE_NORMAL || synchronize_flag || !ff_slot_known || fast_forward_limit() ||
      !nspire_lcd_at_gba_rate())
    return 0;   /* (at a top speed the frames wait for their time anyway, the pictures evenly; an
                 * LCD not tuned (an older CX): as before, its fast forward from a heavy scene to a
                 * light one kept 56 pictures this way in gpsp-host's test_fast_forward_follows_scene) */
  ff_pattern(&cycle, &draws);
  return cycle == 1;
}

int nspire_frameskip_before_flip(void)
{
  u32 now, start;

  nspire_frameskip_os_point(NSPIRE_OSP_FLIP);
  if (!in_fast_forward() && nspire_pace == PACE_NORMAL && nspire_lcd_at_gba_rate())
  {
    /* 2026-11-16: normal play, the LCD at a GBA frame (nspire.c lcd_tune):
     * the pictures and the refreshes come at nearly the same rate, so where
     * a picture falls close to a refresh's start a frame's jitter put two in
     * one refresh, then none in the next (57.5 of 60 seen in the PC's model
     * of Emerald's town). The picture waits for the LCD to take the last one
     * (at most a refresh, idle time), which only happens there.
     * 2026-11-17 (final/pace6; a CX II with sound, 60/50-55): the
     * wait slept in the OS up to its next tick (0-10 ms and the OS's own
     * time) whenever the OS was wanted (a sound adapter, a hub), though the
     * LCD takes the last picture at its next refresh's lines, often a
     * millisecond away: a sleep now only when the OS's tick comes 2 ms
     * before that (none in the porches, where it's under 2 ms). And a picture
     * flipped in the porches was taken as the LCD read it, unseen by this
     * check (it only sees the lines being read): the frame's start looks too
     * (nspire_frameskip_begin_frame). The pacing model (tests/audio
     * pace_test --case lcd, the LCD at 16.74 ms): Emerald-like 12-16 ms
     * frames with sound 55.8 -> 59+ pictures. */
    start = nspire_ticks();
    if (frame_began_valid)
      np_measure(start - frame_began);   /* (final/r19-pace: lcd_picture_misses' cost) */
    if (np_start_deep && np_deep_from)
    {
      /* (the first picture after a new count: after the next refresh's start) */
      if ((s32)(np_deep_from - start) > 0 && (s32)(np_deep_from - start) <= (s32)(LCD_PERIOD >> 16))
      {
        nspire_wait_until(np_deep_from);
        nspire_perf_totals.idle += nspire_ticks() - start;
      }
      np_start_deep = 0;
    }
    np_wait_taken();
    np_flip_at = nspire_ticks();   /* (the flip comes now) */
    np_flip_valid = 1;
    return 1;
  }
  if (!in_fast_forward() || !ff_slot_known)
    return 1;
  /* nspire_flip_taken sees the LCD reading the last picture's lines; in
   * the porches it can't tell, and then the time says it: the refresh that
   * takes it has begun (waiting there for the lines would make this picture
   * miss the next refresh). */
  if (ff_defers() && !ff_last_taken())
  {
    ff_deferred = 1;
    ff_defer_screen = nspire_screen;
    ff_defer_serial = nspire_flip_serial;
    nspire_os_wake_hook = ff_wake;
    return 0;
  }
  (void)now;
  ff_waited += ff_wait_taken();
  return 1;
}

/* A picture was shown (nspire_flip): the refresh that takes it, and the
 * next one's slot. */
static void fast_forward_shown(u32 now)
{
  u32 cycle, draws, steps;

  ff_pattern(&cycle, &draws);
  ff_lcd_note(now);
  ff_shown_at = ff_refresh_after(now);
  if (ff_slot_known && ff_skips && (s32)(ff_shown_at - ff_slot) > (s32)(LCD_PERIOD >> 17))
    ff_late_margin = ff_late_margin + (FF_MARGIN << 4) < (FF_MARGIN << 6) ? ff_late_margin + (FF_MARGIN << 4)
                                                                         : FF_MARGIN << 6;
  else
    ff_late_margin -= ff_late_margin >> 5;
  ff_skips = 0;
  ff_slot_acc += (cycle << 16) / draws;
  steps = ff_slot_acc >> 16;
  ff_slot_acc &= 0xFFFF;
  ff_slot = ff_shown_at + (u32)(((u64)steps * LCD_PERIOD) >> 16);
  ff_slot_known = 1;
}

/* The speed readout (docs/ui/spec.txt), top left, from a whole second's
 * game frames and pictures: always the game's frames a second, never a
 * multiple (a multiple such as 3.5x says less than the frames a second
 * themselves):
 *   "fps: 60"            the game's frames a second, every picture drawn
 *                        (normal play, or fast forward's 60: "fps: 150");
 *   "fps: 60  drawn: 45" fewer pictures than that (60 in fast forward, the
 *                        game's frames in normal play), by more than one
 *                        (each number after its name and a colon, two
 *                        spaces between, no comma: since 2026-11-17,
 *                        "drawn: x", not "x drawn";
 *                        until then "fps: 60, 45 drawn");
 *   "fps: 52"            the game under full speed says it by itself;
 *   "fps: --"            not known yet.
 * Until final/pace "fps: 28/30" (pictures against automatic's level) and
 * "fps: 3.2x" in fast forward. The widest, "fps: 600  drawn: 59", is about
 * 100 px of the 200 the readout has (nspire_osd.c). Colours: green at full
 * speed with all the pictures (fast forward: at its top speed, or at least
 * 120 at max), amber with pictures left out or the game under full speed by
 * at most a quarter (fast forward: over normal speed), red below that. */
#define READOUT_FULL 60u   /* the GBA's 59.73 a second, as a whole second counts it */
static void readout(char *text, u32 size, u32 game, u32 drawn, u16 *color)
{
  u32 pictures = game < READOUT_FULL ? game : READOUT_FULL;
  int fewer = drawn + 1 < pictures;
  int which;

  if (fewer)
    snprintf(text, size, "fps: %u  drawn: %u", (unsigned)game, (unsigned)drawn);
  else
    snprintf(text, size, "fps: %u", (unsigned)game);
  if (in_fast_forward())
  {
    u32 top = ff_max_fps() ? ff_max_fps() : 2 * READOUT_FULL;
    which = game + 1 >= top && !fewer ? OSD_GOOD : game + 1 >= READOUT_FULL ? OSD_WARN : OSD_BAD;
  }
  else
    which = game + 1 >= READOUT_FULL ? (fewer ? OSD_WARN : OSD_GOOD) :
            game * 4 >= READOUT_FULL * 3 ? OSD_WARN : OSD_BAD;
  *color = nspire_osd_color(which);
}

/* A whole second's counts into the readout's (see RO_SECONDS) */
static u32 ro_mean100(const u16 *v, u32 n)
{
  u32 i, sum = 0;

  for (i = 0; i < n; i++)
    sum += v[i];
  return n ? sum * 100 / n : 0;
}

static u32 ro_held(u32 shown, u32 mean100, int first)
{
  u32 target = (mean100 + 50) / 100, off = mean100 > target * 100 ? mean100 - target * 100 : target * 100 - mean100;

  if (first || mean100 >= shown * 100 + RO_HOLD * 10 || mean100 + RO_HOLD * 10 <= shown * 100 || off <= RO_NEAR)
    return target;
  return shown;
}

static int ro_jump(u32 v, u32 shown, u32 low)
{
  u32 j = shown > READOUT_FULL + 1 && shown / 10 > low ? shown / 10 : low;   /* (fast forward's: a tenth) */
  return v + j <= shown || v >= shown + j;
}

static void readout_second(u32 game, u32 drawn)
{
  u32 n, slot, left = game > drawn ? game - drawn : 0;
  u32 left_shown = ro_game_shown > ro_drawn_shown ? ro_game_shown - ro_drawn_shown : 0;
  int first = !ro_n, dfirst = !ro_dn, by_left = 0, normal = !in_fast_forward();

  /* (fast forward's seconds swing more, unpaced: 3 as before, a tenth over 61) */
  if (!first && ro_jump(game, ro_game_shown, normal ? RO_JUMP : RO_JUMP + 1))
  {
    ro_n = ro_dn = 0;   /* a real change: from this second on */
    first = dfirst = 1;
  }
  else if (!dfirst && (ro_jump(drawn, ro_drawn_shown, RO_JUMP + 1) ||   /* (3, or fast forward's tenth: as before) */
                       (normal && ro_game_shown <= READOUT_FULL + 1 && (left_shown <= 1 || left <= 1) &&
                        (left >= left_shown + 2 || left + 2 <= left_shown))))
  {
    ro_dn = 0;
    dfirst = by_left = 1;
  }
  slot = ro_n % RO_SECONDS;
  ro_game[slot] = (u16)(game > 0xFFFF ? 0xFFFF : game);
  ro_n++;
  n = ro_n < RO_SECONDS ? ro_n : RO_SECONDS;
  ro_game_shown = ro_held(ro_game_shown, ro_mean100(ro_game, n), first);
  slot = ro_dn % RO_SECONDS;
  ro_drawn[slot] = (u16)(drawn > 0xFFFF ? 0xFFFF : drawn);
  ro_dn++;
  n = ro_dn < RO_SECONDS ? ro_dn : RO_SECONDS;
  ro_drawn_shown = ro_held(ro_drawn_shown, ro_mean100(ro_drawn, n), dfirst);
  if (by_left)
    ro_drawn_shown = ro_game_shown > left ? ro_game_shown - left : 0;
}

/* The counters' text, once a second (detailed) or twice (frames). */
static void update_counters(u32 now)
{
  u32 probe_ticks = 0;

  nspire_perf_on = nspire_fps_overlay == 2 || nspire_pace != PACE_NORMAL;
  if (!nspire_fps_overlay)
  {
    nspire_fps_text[0] = 0;
    nspire_perf_text[0] = 0;
  }

  if (now - perf_window.start >= NSPIRE_TICK_HZ)
  {
    struct nspire_perf_stats s = nspire_perf_since(&perf_window, now);
    /* The game's frames a real second, as the counter has them. */
    if (rtc_quiet < RTC_QUIET_FRAMES && rtc_shown_valid)
      s.game_fps = rtc_game_fps * 10;
    if (nspire_fps_overlay == 2)
    {
      /* the readout's pattern, a name, a colon and its number (ms a frame after the fps) */
      snprintf(nspire_perf_text, sizeof(nspire_perf_text),
               "fps: %u.%u  cpu: %u.%u gfx: %u.%u out: %u.%u idle: %u.%u", TENTHS(s.game_fps),
               TENTHS(s.cpu_ms), TENTHS(s.render_ms), TENTHS(s.output_ms), TENTHS(s.idle_ms));
      /* Fast forward's seconds too, marked "ff": the picture's cost a drawn
       * frame against the game's a frame settles where its time goes
       * (2026-11-11: the model says 116 game frames a second in Emerald's
       * town, the calculator showed 90-100). */
      if (!perf_log_sp)
      {
        volatile char here = 0;
        perf_log_sp = (uintptr_t)&here;
        /* The memory's speed at this clock, once (the header's "mem:" line). */
        if (!mem_line[0])
          probe_ticks = mem_report(mem_line, sizeof(mem_line)) + nspire_keys_probe();
      }
      if (perf_log_count < PERF_LOG_LINES)
        snprintf(perf_log[perf_log_count++], sizeof(perf_log[0]),
                 "%sgame %u.%u fps, drawn %u.%u; cpu %u.%u, gfx %u.%u, out %u.%u, idle %u.%u; os %u.%u ms (%u sleeps); port %s",
                 nspire_pace == PACE_NORMAL ? "" : "ff ",
                 TENTHS(s.game_fps), TENTHS(s.drawn_fps), TENTHS(s.cpu_ms), TENTHS(s.render_ms),
                 TENTHS(s.output_ms), TENTHS(s.idle_ms), TENTHS(s.os_ms), (unsigned)s.os_sleeps,
                 nspire_usbport_name(nspire_usbport_state()));
    }
    else
      nspire_perf_text[0] = 0;
    perf_window = nspire_perf_totals;
    perf_window.start = now + probe_ticks;   /* the probe's time in no second */
  }

  if (nspire_fps_overlay)
  {
    /* Real seconds' counts (above); the timer's only if the clock is still. */
    int by_rtc = rtc_quiet < RTC_QUIET_FRAMES;
    /* nspire_fps_full (the PC tests read it): full speed's game frames, 60
     * in normal play, fast forward's top speed (0 at max). Was the pictures
     * a second the readout's "/30" was against. */
    nspire_fps_full = in_fast_forward() ? ff_max_fps() : READOUT_FULL;
    if (by_rtc && !rtc_shown_valid)
    {
      snprintf(nspire_fps_text, sizeof(nspire_fps_text), "fps: --");
      nspire_fps_color = nspire_osd_color(OSD_WARN);
    }
    else
      readout(nspire_fps_text, sizeof(nspire_fps_text), by_rtc ? ro_game_shown : fps_shown,
              by_rtc ? ro_drawn_shown : fps_drawn_shown, &nspire_fps_color);
  }
}

/* For the log (nspire_host_main.c log_picture): the frameskip setting and
 * what it draws now. */
void nspire_frameskip_describe(char *text, u32 size)
{
  if (skip_for_sound())
    snprintf(text, size, "off, while sound plays a picture left out when a frame is late, never two in a row");
  else if (auto_by_time())
    snprintf(text, size, "automatic, a picture left out when a frame is late, at most %u in a row",
             (unsigned)auto_floor());
  else if (skip_type() == auto_frameskip)
    snprintf(text, size, "automatic (no timer), 1 of %u drawn", (unsigned)(auto_level + 1));
  else if (skip_type() == manual_frameskip)
    snprintf(text, size, "1 of %u drawn", (unsigned)(skip_value() + 1));
  else
    snprintf(text, size, "off");
}

/* 2026-11-17 (final/pace6): normal play with the LCD at a GBA frame
 * (nspire.c lcd_tune: within half a line of 280896 cycles, +-0.15%). The
 * frames' clock kept the GBA's 59.73 a second at whatever phase it started
 * in against the refreshes, which moved through them over seconds: where the
 * pictures fell near a refresh's start, each one late by a little missed it
 * and waited for the next (nspire_frameskip_before_flip), the game behind by
 * the wait, a picture left out to catch up. A CX II with sound
 * (60/50-58) had no time to spare for it. Now the frames start at a
 * refresh's first lines (LCD_LOCK_AT in), the clock moved toward that by at
 * most LCD_LOCK_STEP a frame (0.55%: in lock it follows the LCD's +-0.15%,
 * which the sound's rate follows as it follows any game's clock): a frame's
 * picture has the whole refresh to be ready by the next one's start. The
 * pacing model (tests/audio pace_test --case lcd, 20 s; Emerald-like frames
 * of 12-16 ms with sound, the LCD at 16.74 ms): 55.7 -> 59+ pictures. */
#define LCD_LOCK_AT   (NSPIRE_TICK_HZ / 2000)   /* 0.5 ms into the refresh's lines */
#define LCD_LOCK_STEP 3                          /* ticks a frame at most (92 us) */
static void lcd_phase_lock(void)
{
  u32 to, period;
  s32 e;

  if (!nspire_lcd_at_gba_rate())
    return;
  /* (only an LCD within 0.16% of a GBA frame: the sound's rate follows a
   * game's clock within its drift term's 0.2%) */
  if (LCD_PERIOD > FRAME_PERIOD + FRAME_PERIOD / 640 || LCD_PERIOD < FRAME_PERIOD - FRAME_PERIOD / 640)
    return;
  to = nspire_lcd_ticks_to_refresh();   /* the next refresh's lines start now + to (0: in the porches) */
  period = LCD_PERIOD >> 16;
  if (!to || !period)
    return;
  /* where this frame's time falls against the refreshes' lines, from the
   * target: within half a refresh either way */
  e = (s32)(next_frame - (nspire_ticks() + to + LCD_LOCK_AT));
  e %= (s32)period;
  if (e > (s32)period / 2)
    e -= (s32)period;
  else if (e < -(s32)period / 2)
    e += (s32)period;
  /* (final/r19-pace) the second frame after a new count (np_snap) at once: at
   * a random place against the refreshes, the frames' pictures crossed a
   * refresh's start for the 1.5 s the steps took, and the first heavy one
   * held a picture for a refresh (pace_test --case steady's toggles) */
  if (np_snap)
    np_snap = 0;
  else if (e > LCD_LOCK_STEP)
    e = LCD_LOCK_STEP;
  else if (e < -LCD_LOCK_STEP)
    e = -LCD_LOCK_STEP;
  next_frame -= (u32)e;
}

/* ---- A pad's OS time when the OS's tick comes (2026-11-17, final/speed8) ----
 * A controller's reports come only in the OS's time, so while one is in the
 * pad glue asks for an OS tick at least every USBPAD_TICK_FORCE (33 ms). It
 * took them at its key read, the frame's end: forced there once 33 ms had
 * passed, or near (the tick within 1-6 ms) from 1/60 s. A sleep lasts until
 * the OS's next tick (0-10.25 ms on a CX II) and the OS's own time after it,
 * and at the frame's end that wait to the tick is more than an Emerald frame
 * with sound has to spare: a CX II with the sound adapter and a
 * pad on a hub (r15's hardware log) played 60 game
 * frames with 38-55 pictures, 21-25 of the pad's sleeps a second outside
 * waits, 85-155 ms of them; 60/58-60 without the pad.
 * So, in a game's frames, the sleep starts just before the tick comes: the
 * picture's lines (nspire_lines.c: each line handed over at its HBlank and
 * each line drawn, the frame's emulation and drawing: it reaches the OS from
 * there, through update_gba's line handling and through every store that
 * makes waiting lines drawn first: the dynarec's palette, VRAM and OAM
 * handlers, write_io_register16, dma_transfer, enter_vblank) and the
 * frame's points (its start after the wait, the emulation done, the picture
 * made, the key read) take it.
 * speed9a (2026-11-17, on c0d37dc): only
 * where the tick's own timer is read (nspire_os_tick_timer). speed8 took
 * where the ticks fall from the last wake, which comes the OS's own time
 * after the tick (0.3-2.5 ms in the models; never measured), and slept
 * 0.2 ms before the tick so foretold: after it, mostly, waiting a whole
 * tick (the anchored model: 31 pictures moving, 230 ms/s of OS
 * time, where r15's rule had 59.5). Now:
 *  - the tick taken is the first at least 'force' (the glue's 33 ms) after
 *    the OS's last turn (a frame's points arm the lines for it);
 *  - the USB task runs only until the tick (gpSP's task, of a higher
 *    priority, has the CPU again after it), and a pad's report took 0.43 ms
 *    of it on the CX II (R11u): a sleep starts with the tick at least
 *    OSP_USB_NEED away and less than a timer count more (the CX II's counts
 *    of 0.49 ms: 0.49-0.98 ms before it). A tick nearer than that when
 *    seen is let go (the next one is taken);
 *  - while armed (the tick to take within a frame), a line looks at the
 *    timer's count inline (nspire_lines.c os_watch: two loads and a compare)
 *    and calls nspire_os_watch_line only from that count on;
 *  - the key read sleeps once 'force' and a tick have passed (that tick
 *    came where no line looked: the VBlank's lines, the picture's output,
 *    between the frames), unless the wait to come has a tick (normal play:
 *    the wait sleeps): the OS's turns never further apart than R1's bound,
 *    33.3 ms + a frame + a tick (r15's rule: forced at 'force', then its
 *    wait to the tick).
 * Where the timer isn't read (the start's probe: nspire_os.c), the glue's
 * own rule (r15's) stands. The pacing model: tests/audio/pace_test.c --case
 * pad (the wake anchored, with jitter, the USB task's time charged). */
#define OSP_USB_NEED (NSPIRE_TICK_HZ * 45 / 100000)   /* 0.45 ms (14 ticks): the USB task's time a report needs */
#define OSP_WAIT_ROOM  66                        /* nspire_os_sleep_before's room (nspire_os.c SLEEP_ROOM) */
#define OSP_LONG_TICK  (NSPIRE_TICK_HZ / 200)    /* 5 ms: a CX II's tick (10.25 ms); a CX's (2.98 ms) costs little to wait for */
static u32 osp_force, osp_pad_force, osp_need_at, osp_frame_at;
static int osp_need, osp_frames;
/* (speed9a) the sound's task time (nspire_audio.c: with a hub in, a tick at
 * least every NSPIRE_AUDIO_HUB_MS): its force and when it last asked */
static u32 osp_snd_force, osp_snd_at;
static int osp_snd;
static u32 osp_sleeps_n, osp_watch_sleeps_n, osp_missed_n;

/* A pad wants the OS (its key read within 4 frames), a game's frames run
 * (normal play or fast forward, a timer; their start seen within 3 frames:
 * not a menu, which reads the keys too), the OS's tick is a long one, and
 * its timer is read. */
static int osp_pad(u32 now)
{
  return osp_need && now - osp_need_at < FRAME_TICKS * 4;
}

static int osp_sound(u32 now)
{
  return osp_snd && now - osp_snd_at < FRAME_TICKS * 4;
}

static int osp_active(void)
{
  u32 now = nspire_ticks();
  if (!osp_frames || nspire_pace != PACE_NORMAL || !nspire_timer_runs() || now - osp_frame_at >= FRAME_TICKS * 3)
    return 0;
  /* the tick wanted soonest of those who want one: the pad's, the sound's */
  if (osp_pad(now))
    osp_force = osp_sound(now) && osp_snd_force < osp_pad_force ? osp_snd_force : osp_pad_force;
  else if (osp_sound(now))
    osp_force = osp_snd_force;
  else
    return 0;
  return 1;
}

static void osp_sleep(void)
{
  nspire_os_watching = 0;
  if (nspire_os_sleep())
    osp_sleeps_n++;
}

enum { OSP_UNKNOWN = -1, OSP_AHEAD, OSP_NOW };

/* Where the tick to take stands, by the timer's count: OSP_NOW, the next
 * tick is one to take and as near as the USB task's time allows; OSP_AHEAD,
 * one to take is further on (*in: the most it is off, timer ticks);
 * OSP_UNKNOWN, the timer can't be read. *next: the next tick's latest. */
static int osp_state(u32 since, u32 period, uint32_t *in, uint32_t *next)
{
  uint32_t earliest, latest, d, need;
  u32 at;

  need = nspire_os_tick_need(OSP_USB_NEED);
  d = nspire_os_tick_bracket(&earliest, &latest);
  if (!period || !need || !d)
    return OSP_UNKNOWN;
  *next = latest;
  at = since + latest;                 /* the next tick, as 'since' will read then */
  if (at < osp_force)
  {
    *in = latest + (osp_force - at + period - 1) / period * period;
    return OSP_AHEAD;
  }
  if (d == need)
  {
    *in = latest;
    return OSP_NOW;
  }
  /* further than that: this one; nearer: the USB task hasn't the time
   * before it, the one after */
  *in = d > need ? latest : latest + period;
  return OSP_AHEAD;
}

/* video.h: a line handed over (the count near a tick: nspire_lines.c's
 * inline check) or drawn while armed. */
void nspire_os_watch_line(void)
{
  uint32_t tick_in, period, in, next;

  if (*nspire_os_watch_at.reg - 1u >= nspire_os_watch_at.value)
    return;                            /* (a line drawn: the lines handed over check it inline) */
  if (!osp_active() || !nspire_os_next_tick_in(&tick_in, &period))
  {
    nspire_os_watching = 0;
    return;
  }
  if (osp_state(nspire_os_since_sleep(), period, &in, &next) == OSP_NOW)
  {
    osp_watch_sleeps_n++;
    osp_sleep();
  }
}

static void osp_point(int x)
{
  uint32_t now = nspire_ticks(), since = nspire_os_since_sleep(), tick_in, period, in = 0, next = 0;
  int st;

  st = nspire_os_next_tick_in(&tick_in, &period) ? osp_state(since, period, &in, &next) : OSP_UNKNOWN;
  if (st == OSP_UNKNOWN)
  {
    /* (the timer not read now: the glue's forced sleep) */
    nspire_os_watching = 0;
    if (x == NSPIRE_OSP_KEYS && osp_pad(now) && since >= osp_pad_force)
      osp_sleep();
    return;
  }
  if (x == NSPIRE_OSP_KEYS)
  {
    /* the frame's end in normal play: a tick in the wait to come is the
     * wait's (fast forward has none, but at its top speed) */
    if (synchronize_flag)
    {
      s32 wait = (s32)(next_frame + FRAME_TICKS - now);
      if (wait > 0 && (s32)(next + OSP_WAIT_ROOM) <= wait)
      {
        nspire_os_watching = 0;
        return;
      }
    }
    /* the pad's force and a tick passed (its tick came untaken): now (the
     * OS's turns never further apart than force + a tick + a frame). Not at
     * the frame's other points: a sleep there waits for its tick inside the
     * frame, and the picture comes later. (The sound's own limit forces its
     * sleep: nspire_audio_os_time.) */
    if (osp_pad(now) && since >= osp_pad_force + period)
    {
      osp_missed_n++;
      osp_sleep();
      return;
    }
  }
  if (st == OSP_NOW)
  {
    osp_sleep();
    return;
  }
  /* armed while the tick to take can come before the frame's next point
   * (a frame from now): the lines look from the count the USB task needs */
  nspire_os_watch_arm(nspire_os_tick_need(OSP_USB_NEED));
  nspire_os_watching = in < FRAME_TICKS + period;
}

/* nspire_frameskip.h */
void nspire_frameskip_os_point(int x)
{
  /* (final/r19-pace) a fast forward picture waiting for the LCD: flipped at
   * the frame's points too, once the LCD has taken the last one (the sooner
   * after that, the more of the refresh it has in hand) */
  if (ff_deferred && x != NSPIRE_OSP_FLIP)
    ff_flush(0);
  if (x == NSPIRE_OSP_START)
  {
    osp_frame_at = nspire_ticks();
    osp_frames = 1;
  }
  if (osp_active())
    osp_point(x);
  else
    nspire_os_watching = 0;
}

int nspire_frameskip_os_keys(u32 force)
{
  uint32_t tick_in, period;

  (void)nspire_os_next_tick_in(&tick_in, &period);
  osp_need = period >= OSP_LONG_TICK && nspire_os_tick_timer();
  osp_pad_force = force;
  osp_need_at = nspire_ticks();
  if (!osp_active())
  {
    nspire_os_watching = 0;
    return 0;
  }
  osp_point(NSPIRE_OSP_KEYS);
  return osp_need;   /* (0: the sound's task's ticks only; the glue's own rule for the pad) */
}

void nspire_frameskip_os_none(void)
{
  osp_need = 0;
  if (!osp_sound(nspire_ticks()))
    nspire_os_watching = 0;
}

/* (speed9a) nspire_frameskip.h: the sound's task wants a tick at least
 * every 'force' (0: no more). 1 if the frame loop takes it. */
int nspire_frameskip_os_task(u32 force)
{
  uint32_t tick_in, period;

  (void)nspire_os_next_tick_in(&tick_in, &period);
  osp_snd = force && period >= OSP_LONG_TICK && nspire_os_tick_timer();
  osp_snd_force = force;
  osp_snd_at = nspire_ticks();
  if (!osp_active())
  {
    if (!osp_pad(osp_snd_at))
      nspire_os_watching = 0;
    return 0;
  }
  return osp_snd;
}

u32 nspire_frameskip_os_sleeps(u32 *by_lines)
{
  if (by_lines)
    *by_lines = osp_watch_sleeps_n;
  return osp_sleeps_n;
}

void nspire_frameskip_begin_frame(void)
{
  int draw;
  s32 late;
  u32 now;

  if (nspire_pace != PACE_NORMAL || !synchronize_flag)
  {
    /* USB audio's OS time (no wait here: due now), before the frame's cost
     * is taken: a sleep it needs costs this frame. */
    nspire_audio_os_time(0, ff_began_valid ? ff_began + ff_waited : 0, 0);
    /* Bluetooth's (its interrupt window; the OS only while its dongle comes or goes) */
    if (!NSPIRE_BT_IDLE())
      nspire_bt_os_time(0, ff_began_valid ? ff_began + ff_waited : 0, 0);
    if (nspire_pace == PACE_UNCAPPED_DRAW_ALL)
      draw = 1;
    else if (nspire_pace == PACE_UNCAPPED_DRAW_NONE)
      draw = 0;
    else
    {
      u32 limit = fast_forward_limit();
      now = nspire_ticks();
      if (!ff_started)
      {
        ff_started = 1;
        ff_next = now;
        ff_next_frac = 0;
        ff_began_valid = 0;
        ff_slot_known = 0;
        ff_slot_acc = 0;
        ff_phase = 0;
        ff_skips = 0;
        ff_full_due = now;
        ff_full_frac = 0;
      }
      ff_measure(now);
      /* The rule's 3 (automatic): this frame's time at the GBA's speed, at
       * most a frame ahead of now (a skip's gain kept for the next frames,
       * not more: a game with time to spare banks nothing), and a pause is
       * no lateness. (F1, 2026-11-12, had normal play's level follow
       * the scene here; the clock follows it by itself.) */
      ff_full_frac += FRAME_PERIOD;
      ff_full_due += ff_full_frac >> 16;
      ff_full_frac &= 0xFFFF;
      late = (s32)(now - ff_full_due);
      if (late < -(s32)FRAME_TICKS || late > (s32)FRAME_TICKS * 6)
      {
        /* (too far behind: from now, still a frame behind, as normal play) */
        late = late < 0 ? -(s32)FRAME_TICKS : (s32)FRAME_TICKS;
        ff_full_due = now - (u32)late;
        ff_full_frac = 0;
      }
      fast_forward_pace(limit);
      draw = fast_forward_draw(limit);
      if (draw && auto_by_time() && nspire_pace == PACE_NORMAL && late >= (s32)LATE_SKIP &&
          skipped_frames < auto_floor())
        draw = 0;   /* 60 pictures would hold the game under 60: this one gives way */
      ff_began = nspire_ticks();
      ff_began_valid = 1;
      ff_waited = 0;
    }
    next_frame = nspire_ticks();
    next_frame_frac = 0;
    frame_began_valid = 0;
  }
  else
  {
    if (np_snap == 2 && nspire_lcd_at_gba_rate() && nspire_lcd_ticks_to_refresh() && !nspire_flip_taken())
    {
      /* (the first after a new count, the last picture still waiting for the
       * LCD: fast forward's: at once, so this one's picture comes after it
       * with no refresh between) */
      next_frame = nspire_ticks();
      next_frame_frac = 0;
      np_snap = 1;
    }
    else
    {
      if (np_snap == 2)
        np_snap = 1;   /* (the first a frame on, as before: its lock a whole step) */
      next_frame_frac += FRAME_PERIOD;
      next_frame += next_frame_frac >> 16;
      next_frame_frac &= 0xFFFF;
      lcd_phase_lock();
    }
    /* USB audio's OS time: out of the wait below when the OS's tick falls in
     * it (nspire_wait_until), else now if the queue of transfers needs it
     * before the next frame's end (nspire_audio.c); a sleep here comes
     * before the frame's cost is taken: what of it comes past the frame's
     * due time costs the frame (automatic frameskip sees it: the frame
     * starts late, a picture gives way), what comes before it is spare time
     * (a wait). */
    now = nspire_ticks();
    if (nspire_audio_os_time(next_frame, frame_began_valid ? frame_began : 0, 1))
      now = nspire_ticks();
    /* Bluetooth's, the same way (nspire_bt_glue.h: one function, placed
     * here); the clock read again after it whether or not it slept: its
     * poll may have blocked (an HCI command, a step of the firmware's read) */
    if (!NSPIRE_BT_IDLE())
    {
      nspire_bt_os_time(next_frame, frame_began_valid ? frame_began : 0, 1);
      now = nspire_ticks();
    }
    late = (s32)(now - next_frame);

    switch (skip_type())
    {
    case auto_frameskip:
      if (nspire_timer_runs())
      {
        /* The rule (the top of this file; 2026-11-15 round 2 of the USB
         * audio had it for frameskip off while sound plays, R11u's 60/45 and
         * 60/30 for a shortfall of a few percent): one picture at a time,
         * this one left out only when the frame starts half a frame or more
         * behind its time (LATE_SKIP; round 2's quarter), at most
         * auto_floor() in a row. */
        draw = late < (s32)LATE_SKIP || skipped_frames >= auto_floor();
        /* (final/r19-pace) with the LCD at a GBA frame, only a picture that
         * would miss its refresh anyway gives way (lcd_picture_misses): a
         * frame up to two behind whose picture is still on time is drawn
         * (the top of this file's 2026-11-21 note); not frames run ahead for
         * a save (np_ahead), nor with the sound's ring nearly dry */
        if (late < (s32)LATE_SKIP)
          np_ahead = 0;
        if (!draw && !np_ahead && late < (s32)NP_LAG_SKIP && !np_ring_low() &&
            lcd_picture_misses(now) == 0)
          draw = 1;
      }
      else
      {
        if (auto_level > skip_value())
          auto_level = skip_value();
        auto_counter = (auto_counter + 1) % (auto_level + 1);
        draw = auto_counter == 0;
      }
      break;
    case manual_frameskip:
      manual_counter = (manual_counter + 1) % (skip_value() + 1);
      if (random_skip)
        draw = manual_counter == rand_gen() % (skip_value() + 1);
      else
        draw = manual_counter == 0;
      break;
    default:
      draw = 1;
      break;
    }

    if (late < 0)
    {
      nspire_wait_until(next_frame);
      nspire_perf_totals.idle += nspire_ticks() - now;
    }
    else if (late > (s32)FRAME_TICKS * 6)
    {
      /* Too far behind to catch up: carry on from now, still a frame
       * behind (final/pace: from now itself the next frame was on time
       * whatever its cost, and drew a picture the floor's pattern hasn't: a
       * game too slow for full speed drew 1 in 4 at a floor of 1 in 5; from
       * LATE_SKIP behind, a skipped frame a little under a frame's time
       * still brought it under). A game that is light again makes up that
       * one frame at most. */
      next_frame = now - FRAME_TICKS;
      next_frame_frac = 0;
    }
    frame_began = nspire_ticks();
    if (np_start_deep && !np_deep_from)
    {
      /* the last picture still waiting for the LCD (fast forward's): in hand
       * already; else the refresh after this frame's start */
      if (nspire_lcd_ticks_to_refresh() && !nspire_flip_taken())
        np_start_deep = 0;
      else if (!np_next_lines(frame_began, &np_deep_from))
        np_start_deep = 0;
      else if (!np_deep_from)
        np_deep_from = 1;
    }
    frame_began_valid = 1;
    /* the LCD reading the last picture now: taken (nspire_frameskip_before_flip) */
    if (nspire_lcd_at_gba_rate())
      (void)nspire_flip_taken();
  }

  skipped_frames = draw ? 0 : skipped_frames + 1;
  skip_next_frame = !draw;
  nspire_frameskip_os_point(NSPIRE_OSP_START);
}

/* Automatic frameskip without a timer: a real second's game frames move
 * auto_level (see AUTO_LOW). In fast forward too (F1, 2026-11-12):
 * without a timer it is normal play's pattern, unpaced as normal play is,
 * so the same counts say the same; it kept the level it was switched on
 * at. */
static void auto_level_by_second(u32 game_fps)
{
  if (nspire_timer_runs() || skip_type() != auto_frameskip || nspire_pace != PACE_NORMAL)
    return;
  if (auto_level > skip_value())
    auto_level = skip_value();
  auto_level_fps[auto_level] = game_fps > 255 ? 255 : (u8)game_fps;
  if (game_fps < AUTO_LOW && auto_level < skip_value())
    auto_level++;
  else if (game_fps > AUTO_HIGH && auto_level > 0 &&
           (!auto_level_fps[auto_level - 1] || auto_level_fps[auto_level - 1] >= AUTO_LOW))
    auto_level--;
}

void nspire_frameskip_end_frame(u32 output_ticks)
{
  u32 now = nspire_ticks();
  /* (r19-fix) a picture held back for the LCD (fast forward's, ff_deferred) counts when flipped */
  int counted = !skip_next_frame && !ff_deferred;

  nspire_perf_totals.frames++;
  perf_note_os();
  nspire_perf_totals.render += nspire_render_ticks;
  nspire_render_ticks = 0;
  if (!skip_next_frame)
  {
    if (in_fast_forward())
    {
      /* the wait for the LCD (nspire_frameskip_before_flip) is idle time */
      output_ticks -= ff_waited < output_ticks ? ff_waited : output_ticks;
      ff_average(&ff_cost_out, output_ticks << 4);
      if (!ff_deferred)
        fast_forward_shown(now);
    }
    nspire_perf_totals.drawn += counted;
    nspire_perf_totals.output += output_ticks;
  }

  /* Game frames and pictures drawn in each real second (until final/pace
   * the pictures in normal play, the game frames in fast forward): a frame
   * counts in the second it ended in. */
  {
    u32 second = nspire_rtc_seconds();
    if (!rtc_state)
    {
      rtc_state = 1;
      rtc_second = second;
      rtc_frames = rtc_game_frames = rtc_quiet = 0;
    }
    else if (second != rtc_second)
    {
      if (rtc_state == 2 && second == rtc_second + 1)
      {
        rtc_shown = rtc_frames;
        rtc_game_fps = rtc_game_frames;
        readout_second(rtc_game_fps, rtc_shown);
        rtc_shown_valid = 1;
        auto_level_by_second(rtc_game_fps);
      }
      rtc_state = 2;
      rtc_second = second;
      rtc_frames = rtc_game_frames = rtc_quiet = 0;
    }
    else if (rtc_quiet < RTC_QUIET_FRAMES)
      rtc_quiet++;
    if (counted)
      rtc_frames++;
    rtc_game_frames++;
  }

  /* The same by the timer, over half a second (shown only if the real-time
   * clock doesn't move). */
  if (!fps_started)
  {
    fps_started = 1;
    fps_start = now;
    fps_frames = fps_drawn = 0;
  }
  else
  {
    fps_frames++;
    fps_drawn += counted;
    if (now - fps_start >= NSPIRE_TICK_HZ / 2)
    {
      u32 t = now - fps_start;
      fps_shown = (fps_frames * NSPIRE_TICK_HZ + t / 2) / t;
      fps_drawn_shown = (fps_drawn * NSPIRE_TICK_HZ + t / 2) / t;
      fps_start = now;
      fps_frames = fps_drawn = 0;
    }
  }
  update_counters(now);
}
