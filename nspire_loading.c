/* The loading screen: starting a game reads its ROM into memory first (all
 * of it when the ROM buffer is big enough, 1 MB at a time), which takes a
 * while for big games; then its newest state is loaded. This shows the
 * game, a bar for the ROM, and what comes after it; or why it didn't load.
 * Q cancels: the keys are read after each 1 MB and between the steps. */

#include "common.h"
#include "nspire.h"
#include "nspire_gui_video.h"
#include "nspire_frontend.h"
#include "nspire_game_list.h"
#include "nspire_keys.h"
#include "nspire_paths.h"
#include "nspire_rom_convert.h"
#include "nspire_states.h"
#include "nspire_ui.h"
#include "nspire_ui_screens.h"
#include "nspire_gbz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define COLOR_TEXT  UI_TEXT2
#define COLOR_ERROR UI_WARN

static char title[160], tags[96];
static u16 picture[STATE_PIC_W * STATE_PIC_H];
static int has_picture;
/* UI17: the picture of the state the next start loads (a slot picked in the Saves place), and whether it is
 * set (-1: not; the newest state's is shown, as before). Taken once. */
u16 *nspire_start_picture;   /* (from the heap: given back once shown) */
int nspire_start_picture_has = -1;
static u32 bar_done, bar_total;

extern int (*nspire_rom_progress)(u32 done, u32 total);
extern int (*nspire_rom_poll)(void);
static int cancelled;
static int q_held, esc_held, b_now_held, b_held;   /* held since start, not a new press */
static u32 full_poll_at;
static struct { u32 poll_ticks, polls, full_polls, draw_ticks, draws; } load_stats;
/* The bar is a zip being made into a .gbz: 1 while it's written, 2 while
 * it's read back and checked. */
static int converting;

/* The game (the picture of the state it resumes from, if it does), its
 * name, the bar, and a line under it; or, without the bar, why it stopped
 * and a key to go on (ui_loading_draw). */
static char convert_tmp[560];   /* the .gbz being written (its size so far is "Saved 4.2 MB") */
static char fact_text[40];

static void draw_screen(const char *what, int bar, const char *line, u16 line_color, const char *more)
{
  struct ui_loading_view v;

  memset(&v, 0, sizeof(v));
  v.what = what;
  v.title = title;
  v.tags = tags;
  v.picture = has_picture ? picture : NULL;
  v.bar = bar;
  v.done = bar_done;
  v.total = bar_total;
  v.line = line;
  v.fact = converting == 1 && fact_text[0] ? fact_text : NULL;   /* not while it is checked: the line says Checking N% */
  v.warn = line_color == COLOR_ERROR && !bar;
  v.more = more;
  v.cancel = bar;
  snprintf(ui_shown, sizeof(ui_shown), "%s",
           !bar ? "stopped" : converting == 2 ? "checking" : converting ? "converting" : "loading");
  ui_loading_draw(&v);
}

static void draw(const char *line, u16 line_color, const char *more)
{
  draw_screen("Starting", 1, line, line_color, more);
}

static int check_hold_key(unsigned key, int *held)
{
  if (!key_down(key))
  {
    *held = 0;
    return 0;
  }
  return !*held || key_pressed(key);
}

int nspire_loading_cancelled(void)
{
  if (!cancelled)
  {
    keys_poll();
    if (check_hold_key(KEY_Q, &q_held) || check_hold_key(KEY_ESC, &esc_held))
      cancelled = 1;
    if (!nspire_gui_button_down(ACTION_B))
      b_held = 0;
    else if (!b_held || nspire_gui_button_pressed(ACTION_B))
      cancelled = 1;
  }
  return cancelled;
}

/* fast17: the ROM load's own polls (gba_memory.c, each 128 KB) read Q, esc
 * and B from the keypad itself (key_down_now: a few register reads); the
 * whole of keys_poll (the pads, Bluetooth's menu pass with its OS sleep,
 * the drive, the sound) at most each FULL_POLL_MS, and at each 1 MB's redraw.
 * r16's CX II took 5.6 s for Emerald's .gbz where r15 took 3.4 s:
 * Bluetooth's menu pass came with r16, an OS sleep each 40 ms of the load. */
#define FULL_POLL_MS 200

static void full_poll(void)
{
  full_poll_at = nspire_ticks();
  load_stats.full_polls++;
  nspire_loading_cancelled();
}

static int rom_poll(void)
{
  u32 t = nspire_ticks();

  if (!cancelled)
  {
    if (!key_down_now(KEY_Q))
      q_held = 0;
    else if (!q_held)
      cancelled = 1;
    if (!key_down_now(KEY_ESC))
      esc_held = 0;
    else if (!esc_held)
      cancelled = 1;
    if (!nspire_gui_button_down_now(ACTION_B))
      b_now_held = 0;
    else if (!b_now_held)
      cancelled = 1;
  }
  if (!cancelled && t - full_poll_at >= FULL_POLL_MS * (NSPIRE_TICK_HZ / 1000))
    full_poll();
  load_stats.polls++;
  load_stats.poll_ticks += nspire_ticks() - t;
  return cancelled;
}

static int rom_progress(u32 done, u32 total)
{
  char line[60];
  u32 t = nspire_ticks();
  full_poll();
  if (cancelled)
    return 1;
  bar_done = done;
  bar_total = total;
  snprintf(line, sizeof(line), "%u of %u MB", (unsigned)done, (unsigned)total);
  draw(line, COLOR_TEXT, NULL);
  load_stats.draws++;
  load_stats.draw_ticks += nspire_ticks() - t;
  return 0;
}

static unsigned ms10(u32 ticks)
{
  return (unsigned)(((u64)ticks * 10000u) / NSPIRE_TICK_HZ);
}

void nspire_loading_rom_note(void)
{
  gbz_load_ahead_end();
  states_timing_note("ROM load parts, ms: reads %u.%u (%lu, %lu KB), inflate %u.%u, key polls %u.%u (%lu, %lu whole), "
                     "bar drawn %u.%u (%lu)",
                     ms10(gbz_stats.read_ticks) / 10, ms10(gbz_stats.read_ticks) % 10, (unsigned long)gbz_stats.reads,
                     (unsigned long)(gbz_stats.bytes / 1024), ms10(gbz_stats.inflate_ticks) / 10,
                     ms10(gbz_stats.inflate_ticks) % 10, ms10(load_stats.poll_ticks) / 10,
                     ms10(load_stats.poll_ticks) % 10, (unsigned long)load_stats.polls,
                     (unsigned long)load_stats.full_polls, ms10(load_stats.draw_ticks) / 10,
                     ms10(load_stats.draw_ticks) % 10, (unsigned long)load_stats.draws);
}

void nspire_loading_start(const char *rom_path)
{
  int count, newest;

  ui_game_title(rom_path, title, sizeof(title), tags, sizeof(tags));
  /* the moment it carries on from */
  if (nspire_start_picture_has >= 0)
  {
    has_picture = nspire_start_picture_has && nspire_start_picture && !nspire_skip_state_load;
    if (has_picture)
      memcpy(picture, nspire_start_picture, sizeof(picture));
    nspire_start_picture_has = -1;
    free(nspire_start_picture);
    nspire_start_picture = NULL;
  }
  else
    has_picture = nspire_load_on_start && !nspire_skip_state_load &&
                  states_peek(rom_path, &count, &newest, picture);
  bar_done = bar_total = 0;
  /* Q, esc and B count from here: one held from before isn't a press */
  keys_poll();
  cancelled = 0;
  q_held = key_down_now(KEY_Q);
  esc_held = key_down_now(KEY_ESC);
  b_now_held = nspire_gui_button_down_now(ACTION_B);
  b_held = nspire_gui_button_down(ACTION_B);
  full_poll_at = nspire_ticks();
  memset(&load_stats, 0, sizeof(load_stats));
  memset(&gbz_stats, 0, sizeof(gbz_stats));
  nspire_rom_progress = rom_progress;
  nspire_rom_poll = rom_poll;
  draw("Reading the game...", COLOR_TEXT, NULL);
}

void nspire_loading_note(const char *text)
{
  draw(text, COLOR_TEXT, NULL);
}

/* ---- A zip too big to unpack becomes a .gbz ---- */

static int convert_percent;

static int convert_progress(unsigned long done, unsigned long total)
{
  char line[100];
  int percent = total ? (int)((unsigned long long)done * 100 / total) : 0;

  if (nspire_loading_cancelled())
    return 1;
  /* A redraw takes a while on the calculator: one for each percent. */
  if (percent == convert_percent)
    return 0;
  convert_percent = percent;
  bar_done = done;
  bar_total = total;
  /* The first half of the job writes the .gbz, the second reads it back. */
  converting = done <= total / 2 ? 1 : 2;
  if (converting == 1)
    snprintf(line, sizeof(line), "%d%%", percent);
  else
    snprintf(line, sizeof(line), "Checking " UI_NBSP "%d%%", percent);
  {
    struct stat st;
    long mb10 = stat(convert_tmp, &st) == 0 ? (long)(st.st_size * 10 / (1024 * 1024)) : 0;
    if (mb10)
      snprintf(fact_text, sizeof(fact_text), "Saved %ld.%ld" UI_NBSP "MB", mb10 / 10, mb10 % 10);
  }
  draw_screen("Getting it ready, once", 1, line, COLOR_TEXT, NULL);
  return 0;
}

int nspire_loading_zip_to_gbz(const char *zip_path, char *gbz_path, size_t size)
{
  static char why_text[220];
  char normal[512], dir[512];
  const char *why, *slash;
  unsigned need = (unsigned)nspire_rom_load_need_mb, free_mb = (unsigned)nspire_rom_load_free_mb;
  int can = rom_zip_gbz_name(zip_path, gbz_path, size);
  struct stat st;

  if (can == 0)
    return 0;   /* not a .gba in it: gba_memory.c's message says what to do */
  if (can < 0)
  {
    snprintf(why_text, sizeof(why_text), "Not enough memory to unpack this zip: it needs %u" UI_NBSP "MB, "
             "%u" UI_NBSP "MB is free. Close open documents, or play its .gbz, next to it: it needs much less.",
             need, free_mb);
    nspire_rom_load_error = why_text;
    return 0;
  }

  converting = 1;
  convert_percent = -1;
  fact_text[0] = 0;
  snprintf(convert_tmp, sizeof(convert_tmp), "%s.tmp", gbz_path);
  convert_progress(0, 1);
  why = rom_compress(zip_path, gbz_path, size, convert_progress);
  converting = 0;
  bar_done = bar_total = 0;
  if (why == rom_convert_cancelled)
    return -1;
  if (why == rom_convert_no_room)
  {
    /* A .gbz takes about as much storage as the zip. */
    long zip_mb = stat(zip_path, &st) == 0 ? (long)((st.st_size + (1 << 20) - 1) >> 20) : 0;
    snprintf(why_text, sizeof(why_text), "This zip doesn't fit in memory (%u" UI_NBSP "MB, %u" UI_NBSP "MB free), "
             "and a .gbz of it needs about %ld" UI_NBSP "MB of storage: not that much is free. The zip is as it was.",
             need, free_mb, zip_mb);
  }
  else if (why)
    snprintf(why_text, sizeof(why_text), "This zip doesn't fit in memory (%u" UI_NBSP "MB, %u" UI_NBSP "MB free), and no "
             ".gbz could be made of it: %s It's as it was.", need, free_mb, why);
  if (why)
  {
    nspire_rom_load_error = why_text;
    return 0;
  }

  /* The game keeps its place in the folder's order, under the new name. */
  nspire_normalize_path(normal, sizeof(normal), zip_path);
  slash = strrchr(normal, '/');
  snprintf(dir, sizeof(dir), "%.*s", !slash ? 1 : slash == normal ? 1 : (int)(slash - normal),
           slash ? normal : ".");
  game_list_rename(dir, slash ? slash + 1 : normal, strrchr(gbz_path, '/') ? strrchr(gbz_path, '/') + 1 : gbz_path);
  return 1;
}

void nspire_loading_end(void)
{
  gbz_load_ahead_end();
  nspire_rom_progress = NULL;
  nspire_rom_poll = NULL;
}

static void wait_key_over(const char *why)
{
  const unsigned char *own = nspire_gui_own_keys;
  nspire_gui_own_keys = NULL;
  keys_wait_release();
  do
  {
    draw_screen("Couldn't start it", 0, why, COLOR_ERROR, "Back to games");
    update_at_vblank();
    keys_poll();
  } while (!any_key_down() && !nspire_gui_button_down(ACTION_A) && !nspire_gui_button_down(ACTION_B));
  keys_wait_release();
  nspire_gui_own_keys = own;
}

void nspire_game_stopped(const char *why)
{
  wait_key_over(why);
}

void nspire_loading_failed(const char *why)
{
  gbz_load_ahead_end();
  nspire_rom_progress = NULL;
  nspire_rom_poll = NULL;
  bar_total = 0;
  wait_key_over(why);
}
