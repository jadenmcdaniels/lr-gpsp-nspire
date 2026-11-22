#ifndef NSPIRE_FRAMESKIP_H
#define NSPIRE_FRAMESKIP_H

/* Frame pacing and frameskip (nspire_frameskip.c). Frames are paced with
 * the 32768 Hz timer at the GBA's own rate (59.73 a second); the LCD shows
 * each frame at its next refresh (nspire_flip). */

enum nspire_pace_mode
{
  PACE_NORMAL,              /* real speed, frameskip as set */
  PACE_UNCAPPED_DRAW_ALL,   /* speed test: as fast as it goes, every frame drawn */
  PACE_UNCAPPED_DRAW_NONE,  /* speed test: as fast as it goes, nothing drawn */
  PACE_FAST_FORWARD         /* speed test: fast forward with frameskip off */
};
extern enum nspire_pace_mode nspire_pace;

/* Fast forward's top speed (Advanced's Fast forward limit): index i is
 * 6 x (i + 11) game frames a second (66 to 600; until final/pace shown as
 * (i + 11) / 10 times normal speed, the same index), NSPIRE_FF_UNLIMITED
 * none ("max"). */
#define NSPIRE_FF_CHOICES 91
#define NSPIRE_FF_UNLIMITED 90
extern u32 nspire_ff_max;
extern const char *const nspire_ff_max_names[NSPIRE_FF_CHOICES];

void nspire_frameskip_reset(void);
/* Before a frame: decides whether it is drawn (sets skip_next_frame) and
 * waits until it is due. */
void nspire_frameskip_begin_frame(void);
/* After a frame: counts it, and the time it took to show if drawn. */
void nspire_frameskip_end_frame(u32 output_ticks);
/* The setting and what it draws now, for the log. */
void nspire_frameskip_describe(char *text, u32 size);
/* Just before a drawn frame is shown (nspire_flip): waits until the LCD has
 * taken the last picture (fast forward; normal play with the LCD at a GBA
 * frame). 0: don't flip: fast forward flips it itself once the LCD has the
 * last one, at a later frame's start (final/r19-pace), the frames between
 * drawing nothing into the screen buffer. */
int nspire_frameskip_before_flip(void);
/* Normal play: the next frames due frames earlier (they run at once, a
 * picture left out where the rule says, and the game's sound gets that much
 * ahead in the ring): before an OS file call in a game (nspire_host_main.c
 * save_job_frame), whose time the ring then plays through. */
void nspire_frameskip_run_ahead(u32 frames);
/* (r19-snd) The reverse: the next frames due frames later (nspire_audio.c's
 * save bank: an OS call's time not made up, the bank given back), and how
 * far the frame clock is behind now (ticks). */
void nspire_frameskip_hold(u32 frames);
u32 nspire_frameskip_behind(void);
/* (final/r19-bank) at a frame's end: the time until the next frame is due (ticks, 0 if late) */
u32 nspire_frameskip_spare(void);

/* 2026-11-17 (final/speed8): a pad's OS ticks taken as they come
 * (nspire_frameskip.c, "A pad's OS time when the OS's tick comes"). The
 * frame's points: its start after its wait (nspire_frameskip_begin_frame),
 * the emulation done (run_frame, before the picture), the picture made
 * (nspire_frameskip_before_flip), the key read (the pad glue); between them
 * the picture's lines (nspire_lines.c, video.h nspire_os_watch_fire). */
enum { NSPIRE_OSP_START, NSPIRE_OSP_PRESENT, NSPIRE_OSP_FLIP, NSPIRE_OSP_KEYS };
/* (r19-fix) Something other than the game's frames takes the screen over (the
 * menu, the loading screen, the link's wait screen, the speed test, the
 * quit): a fast forward picture still waiting for the LCD is flipped now if
 * the LCD has taken the last one, else let go (never flipped after the other
 * screen drew into the buffers, and not counted as drawn). */
void nspire_frameskip_screen_taken(void);
void nspire_frameskip_os_point(int where);
/* The pad glue at its key read, a pad in and wanting the OS: an OS tick at
 * least every 'force' timer ticks. 1: the frame loop takes them (normal
 * play), this key read's point taken; 0: the glue's own rule. */
int nspire_frameskip_os_keys(u32 force);
/* No pad wants the OS now. */
void nspire_frameskip_os_none(void);
/* (2026-11-17, final/speed9a) The sound's task (nspire_audio.c, a hub in):
 * an OS tick at least every 'force' timer ticks, taken as it comes as the
 * pad's are (0: none wanted). 1: the frame loop takes it; 0: the caller's
 * own sleeps (no timer read, menus, a short tick). */
int nspire_frameskip_os_task(u32 force);
/* The sleeps the frame loop took for a pad so far (*by_lines: the lines'). */
u32 nspire_frameskip_os_sleeps(u32 *by_lines);

/* Frame timing totals since 'start', for the counters and the speed test. */
struct nspire_perf
{
  u32 start, frames, drawn, render, output, idle;
  u32 os, os_sleeps;   /* P2: nspire_os_busy_slept's, at the last frame's end */
};
/* Per frame, in tenths: frames per second and milliseconds. */
struct nspire_perf_stats
{
  u32 game_fps, drawn_fps, cpu_ms, render_ms, output_ms, idle_ms;
  u32 os_ms, os_sleeps;   /* P2: the OS's time outside the idle wait (taken out of cpu), its sleeps */
};
extern struct nspire_perf nspire_perf_totals;
struct nspire_perf_stats nspire_perf_since(const struct nspire_perf *since, u32 now);

/* The detailed counter's lines are kept and written to gpsp_perf.txt.tns
 * (next to the program) when the menu opens or the game is left. */
void nspire_perf_log_flush(void);
/* OUT-04 (nspire_input.c): times 16 touchpad scans once, for the header's
 * "keys:" line; returns the ticks it took (left out of the counter's second). */
u32 nspire_keys_probe(void);
const char *nspire_keys_line(void);

#endif
