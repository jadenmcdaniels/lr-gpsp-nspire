#ifndef NSPIRE_AUDIO_H
#define NSPIRE_AUDIO_H
#include <stdint.h>
#include <stdio.h>

/* Settings also live in each game's settings file. Volume is tenths. */
/* r19: the volume in 21 steps, 0 (silent) to NSPIRE_VOLUME_MAX, said "14/20" (until r19 11 steps, "70%") */
#define NSPIRE_VOLUME_MAX 20u
#define NSPIRE_VOLUME_DEFAULT 14u
extern unsigned int nspire_audio_enabled, nspire_audio_volume;
extern unsigned int nspire_audio_mono, nspire_audio_buffer;
void nspire_audio_start(void);
void nspire_audio_stop(void);
int nspire_audio_stop_within(unsigned ms);
/* (r19-usb-exit) The quit's stop with nothing ever offered to the OS's USB
 * drivers (nspire_usbpad_exit_untouched): no wait for a callback that never
 * ran; 0 done, -2 not so (a card): then nspire_audio_stop_within. */
int nspire_audio_stop_quiet(void);
void nspire_audio_poll(void);
/* mode: 0 = pause/silence (menus, speed test), 1 = normal speed, 2 = fast forward. */
void nspire_audio_frame_begin(int mode);
void nspire_audio_frame_end(void);
void nspire_audio_pause(void);
/* 2026-11-15: the frame loop's OS time while the adapter streams
 * (nspire_frameskip.c, at a frame's end before its wait): 'due' when the
 * next frame may start (timer ticks; now or earlier: at once), 'began' when
 * the frame just done started after its wait (0: not known), 'paced' 0 when
 * frames go back to back (fast forward; 'due' is then not read). Sleeps in
 * the OS now if the queue of transfers would run dry before another frame's
 * end, or if now costs the game the least; nothing when a wait
 * (nspire_wait_until) will give the OS a tick out of the spare time. 1 if
 * it slept. Reads no timer when the audio wants no OS time (the PC tests'
 * virtual time). See nspire_audio.c. */
int nspire_audio_os_time(uint32_t due, uint32_t began, int paced);
const char *nspire_audio_summary(void);
int nspire_usbaudio_active(void);
int nspire_usbaudio_owns_usb(void);
/* The game's sound is going to the adapter (not paused, muted or off):
 * automatic frameskip stands in for frameskip off meanwhile. */
int nspire_usbaudio_playing(void);
/* bt10: a USB sound adapter streams (it plays the game's sound in a game unless Output is Bluetooth) */
int nspire_usbaudio_streaming(void);
/* The adapter streams and the sound's own OS sleeps give the OS's task its
 * time, a USB hub's every NSPIRE_AUDIO_HUB_MS at the most when the OS's
 * explore flag is known (else NSPIRE_AUDIO_HUB_FALLBACK_MS, or while
 * Bluetooth wants the task; the pad glue's hub tick leaves it to them). */
#define NSPIRE_AUDIO_HUB_MS 1000
#define NSPIRE_AUDIO_HUB_FALLBACK_MS 250
int nspire_audio_serves_os(void);
/* Around an OS file call in a game (a state's or the game's save written):
 * the CPU's interrupts on for it while the adapter streams, so the sound
 * plays on through it; begin says whether it turned them on (cpsr: as they
 * were), end puts them back. See nspire_audio.c. */
int nspire_audio_os_call_begin(uint32_t *cpsr);
void nspire_audio_os_call_end(int began, uint32_t cpsr);
/* (r19-snd) the same for a stretch outside a game's frame (the menu's save, a load key's load): the
 * calls in it make none of their own */
int nspire_audio_serve_begin(uint32_t *cpsr);
void nspire_audio_serve_end(int began, uint32_t cpsr);
/* The game's sound in the ring (ms) once the queue is served (a window),
 * -1 when none plays. */
/* (final/r19-pace) the ring as it is (ms; -1 as above), nothing served */
int nspire_audio_ring_level_ms(void);
int nspire_audio_ring_ms(void);
/* speed18: the caller is about to keep the CPU ms more than a frame's work
 * (a state's copy at the save key, a save's part at a frame's end): the
 * queue is served now if it would not last that long (nspire_audio.c
 * serve_queue: it is served as late as the frames allow otherwise). */
void nspire_audio_serve_ahead(unsigned ms);
/* (r19-snd) At each frame's end while a save is under way (busy) and once
 * after (busy 0): the game run ahead a frame at a time until the ring holds
 * the longest OS call yet, the bank given back after; 1 if the save's next
 * part may run now: next NSPIRE_SAVE_CPU (a CPU part: always), _OS (an OS
 * call), _TAKE (the state's copy, ~26 ms with the interrupts on). See
 * nspire_audio.c "A save's OS calls". */
enum { NSPIRE_SAVE_CPU, NSPIRE_SAVE_OS, NSPIRE_SAVE_TAKE };
int nspire_audio_save_frame(int busy, int next);
/* (final/r19-bank) the most this save banked (ms; read once, then 0): the timing file's save line */
uint32_t nspire_audio_save_bank_ms(void);

struct nspire_audio_stats {
  uint32_t rate, buffered, dropped, completions, underruns;
  int rendering, priming;
  uint32_t speed;                       /* the rate's factor, Q16 (65536: the game's own) */
  uint32_t sleeps_forced, sleeps_near;  /* the sleeps nspire_audio itself made */
  uint32_t windows, windows_served, window_ticks;  /* IRQ windows, with completions, their time */
  int stalled, slow, win_trusted;       /* the adapter takes nothing; the slow-game mode; windows trusted */
  uint32_t stalls, trims, reprimes;
  /* bt10 */
  uint32_t sync_ms, syncs, sync_gaps;   /* held back for the headphones: ms; re-syncs; of them after a sound gap */
  int card_idle;                        /* the card idle (Output Bluetooth) */
  uint32_t keepalives;                  /* the driver's silent transfers while idle */
  /* r19-snd: a save's bank */
  uint32_t bank, banked, bank_held, bank_held_back;   /* frames run ahead now; in all; given back; not made up */
  uint32_t save_calls, save_short, os_call_dry;       /* OS calls gated; of them short of the need; that ran dry */
  uint32_t os_call_longest_ms;                        /* the longest OS call */
  uint32_t os_call_dips, owe;                         /* calls that faded their end; frames still to make up */
  /* (r19-fix) frames owed in all, made up in all, let go (the save over, the game simply slow); a save
   * under way or settling (its bank, its 2 s after: no slow-game mode, no re-prime by the fill) */
  uint32_t owed, repaid, owe_dropped;
  int save_active;
};
void nspire_audio_stats(struct nspire_audio_stats *s);
/* The seconds kept while the adapter streamed (one line each, the last
 * five minutes), for the perf log at exit: only once the audio is stopped. */
int nspire_audio_log_pending(void);
void nspire_audio_log_write(FILE *f);
#endif
