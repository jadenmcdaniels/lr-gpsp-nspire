/* Ndless entry: libretro gpSP core + legacy menu/GUI. */

#include "common.h"
#include "gba_memory.h"
#include "savestate.h"
#include "serial.h"
#include "cheats.h"
#include "gui.h"
#include "cpu.h"
#include "video.h"
#include "nspire.h"
#include "nspire_gui_video.h"
#include "nspire_frameskip.h"
#include "nspire_frontend.h"
#include "nspire_drive.h"
#include "nspire_drive_saves.h"
#include "nspire_places.h"
#include "nspire_audio.h"
#include "nspire_bt_glue.h"
#include "nspire_keys.h"
#include "nspire_states.h"
#include "nspire_clock.h"
#include "nspire_sram.h"
#include "nspire_brightness.h"
#include "nspire_paths.h"
#include "nspire_ui.h"
#include "nspire_ui_screens.h"
#include "nspire_link.h"
#include "nspire_os.h"
#include "nspire_exit.h"
#include "nspire_usbpad_glue.h"
#include "nspire_gbz.h"

#include <os.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void nspire_show_closing(void)
{
  static int shown;
  if (shown)
    return;
  shown = 1;
  ui_begin();
  ui_clip(0, UI_W);
  ui_rect(20, 84, UI_W - 40, 72, UI_RAISED);
  ui_text_center(UI_W / 2, 110, "Closing gpSP...", &ui_font_small_strong, UI_TEXT);
  ui_text_center(UI_W / 2, 134, "Saving and stopping USB", &ui_font_small_strong, UI_TEXT);
  ui_present();
}

/* The official BIOS file: in the saves folder, where gpSP moves it from
 * its own folder at start (nspire_paths.h), else still next to gpSP. */
static void bios_path(char *path, size_t size)
{
  FILE *f;
  nspire_save_file(path, size, "gba_bios.bin.tns");
  if ((f = fopen(path, "rb")) != NULL)
  {
    fclose(f);
    return;
  }
  snprintf(path, size, "%s/gba_bios.bin.tns", main_path);
}

#if defined(NSPIRE_LIBRETRO)
static void nspire_sync_bios_menu_from_disk(void)
{
  char path[600];
  FILE *fp;
  long sz;
  unsigned char b;

  bios_path(path, sizeof(path));

  fp = fopen(path, "rb");
  if (!fp)
  {
    nspire_bios_choice = 0;
    return;
  }
  if (fread(&b, 1, 1, fp) != 1)
  {
    fclose(fp);
    nspire_bios_choice = 0;
    return;
  }
  if (b != 0x18U)
  {
    fclose(fp);
    nspire_bios_choice = 0;
    return;
  }
  fseek(fp, 0, SEEK_END);
  sz = ftell(fp);
  fclose(fp);
  if (sz < 0x4000L)
  {
    nspire_bios_choice = 0;
    return;
  }
  nspire_bios_choice = 1;
}
#endif

extern void init_gamepak_buffer(void);
extern u32 nspire_input_frame(void);
extern void nspire_input_release_all(void);

extern uint32_t not_crt0_savedsp;
extern u32 quick_save_slot;

/* Automatic frameskip works now (nspire_frameskip.c), so it's the default:
 * every frame is drawn unless the game falls behind. */
u32 current_frameskip_type = auto_frameskip;
u32 frameskip_value = 4;
u32 random_skip = 0;

u8 *file_ext[] = {(u8 *)".gba.tns", (u8 *)".gbz.tns", (u8 *)".zip.tns", NULL};

u32 update_backup_flag = 1;
u32 synchronize_flag = 1;
s32 relative_frame_count = 0;
u32 clock_speed = 132;

int nspire_skip_state_load;
int nspire_start_slot = -1, nspire_start_slot_on_drive;
static int picture_log_pending;   /* log_picture at the next frame */
static int picture_later_pending;   /* and again (untimed) 1 s later: picture_later_at */
static void picture_at_quit(void);

void set_clock_speed(void) {}

void change_ext(u8 *src, u8 *buffer, u8 *extension)
{
  char *dot_position;
  strcpy((char *)buffer, (char *)src);
  dot_position = strrchr((char *)buffer, '.');

  if (dot_position)
    strcpy(dot_position, (char *)extension);
  else
  {
    strcat((char *)buffer, ".");
    strcat((char *)buffer, (char *)extension);
  }
}

u32 file_length(u8 *dummy, FILE *fp)
{
  u32 length;
  (void)dummy;
  fseek(fp, 0, SEEK_END);
  length = (u32)ftell(fp);
  fseek(fp, 0, SEEK_SET);
  return length;
}

static void ChangeWorkingDirectory(char *exe)
{
  char *s = strrchr(exe, '/');
  if (s != NULL)
  {
    *s = '\0';
    chdir(exe);
    *s = '/';
  }
}

/* The file list's last folder, kept in the saves folder. */
static void switch_to_romdir(void)
{
  char buff[600];
  int r;
  nspire_save_file(buff, sizeof(buff), "gpsp_romdir.tns");
  file_open(romdir_file, buff, read);

  if (file_check_valid(romdir_file))
  {
    r = file_read(romdir_file, buff, sizeof(buff) - 1);
    if (r > 0)
    {
      buff[r] = 0;
      while (r > 0 && isspace((unsigned char)buff[r - 1]))
        buff[--r] = 0;
      chdir(buff);
    }
    file_close(romdir_file);
  }
}

static void save_romdir(void)
{
  char path[512], dir[512], old[512];
  size_t len;
  FILE *f;

  /* getcwd is the OS's NU_Current_Dir here, which returns 0 when it
   * works: what it wrote is checked instead. */
  dir[0] = 0;
  getcwd(dir, sizeof(dir));
  if (!dir[0])
    return;
  nspire_save_file(path, sizeof(path), "gpsp_romdir.tns");
  /* EXIT-3: the same folder as the file says: no write (one of the CX's
   * slow writes at every quit). */
  len = 0;
  if ((f = fopen(path, "rb")) != NULL)
  {
    len = fread(old, 1, sizeof(old) - 1, f);
    fclose(f);
  }
  if (len == strlen(dir) && !memcmp(old, dir, len))
    return;
  file_open(romdir_file, path, write);
  if (file_check_valid(romdir_file))
  {
    file_write(romdir_file, dir, strlen(dir));
    file_close(romdir_file);
  }
}

/* A state file by name (the menu's "load savestate from file"). */
int load_state(u8 *filename)
{
  nspire_audio_pause();
  if (!filename)
    return 0;
  if (states_load_file((const char *)filename) != 1)
    return 0;
  instruction_count = 0;
  reg[OAM_UPDATED] = 1;
  nspire_autosave_restart();
  return 1;
}

/* ---- Save states ---- */

/* The slot picked with left/right on the menu's Save line while "a new slot each time" is on
 * (-1: none; it lasts until a save or the menu closes). */
static int save_pick = -1;

void nspire_save_pick(int slot)
{
  save_pick = slot;
}

int nspire_save_target(void)
{
  if (save_pick >= 0)
    return save_pick;
  return nspire_auto_increment ? states_next_new_slot() : (int)savestate_slot;
}

/* A state into slot (-1: none left), with a message when it's written: done
 * says what. 2026-11-17 (final/pace6; the sound cut out on saves and the
 * game froze for a split second while saving): it was
 * saved here at once with the sound paused (133-311 ms on the CX II, the
 * sound cut for that and its priming). Now the state and its picture are
 * taken here (~19 ms: the copy 12.7, the picture's scaling) and the rest
 * goes a part a frame (save_job_frame, nspire_states.c states_save_step),
 * the sound playing on. Linked, at once as before. (r19-snd: the take too
 * at a frame's end, the first whose ring covers it: a frame or a few later.) */
/* (r19-fix: each save's word kept with it, nspire_states.c states_save_begin_as's tag: saves wait for the
 * one under way now, the key's and an autosave's in their order) */
static const char *const save_job_words[2] = { "Saved", "Autosaved" };
void nspire_usbpad_message_posted(void) __attribute__((weak));   /* (builds without the pad glue) */

/* speed18: the sound's queue is served only as late as the frames allow
 * (nspire_audio.c serve_queue): before what keeps the CPU longer than a
 * frame's work it is asked to last (nspire_audio_serve_ahead): the state's
 * copy and picture at the key (~19 ms on the CX II), a save's part */
#define SAVE_PART_MS 16
static int save_state_to(int slot, const char *done)
{
  int started = 0;

  if (slot >= 0 && nspire_link_state() == LINK_LINKED)
  {
    /* linked: at once, as before (the other calculator waits it out) */
    nspire_audio_pause();
    nspire_clock_normal();
    if (states_save(slot))
    {
      savestate_slot = (u32)slot;
      save_pick = -1;
      nspire_saves_frame_saved(slot);
      nspire_osd_message("%s to slot %d", done, slot + 1);
      started = 1;
    }
    else
      nspire_osd_message("Couldn't save to slot %d: %s", slot + 1, states_failure_words(STATES_SAVING));
    nspire_clock_game();
    nspire_reset_timing();
  }
  else if (slot < 0)
    nspire_osd_message("All 1000 slots are used");
  else
  {
    if (!states_save_begin_as(slot, done == save_job_words[1]))
      nspire_osd_message("Couldn't save to slot %d: %s", slot + 1, states_failure_words(STATES_SAVING));
    else
    {
      save_pick = -1;
      started = 1;
      /* (Saves on: USB drive: the calculator's slot now, a part a frame as always; its copy to the
       * drive at the next safe point: nspire_drive_saves.h) */
      nspire_saves_frame_saved(slot);
    }
  }
  nspire_autosave_restart();
  return started;
}

/* At each frame's end, after its sound went into the ring: the next part of
 * a state being saved (its OS file calls at the calculator's own speed, as
 * the save was) or of the game's save (nspire_backup.c), and the message
 * once a state is written, whoever finished it (a menu, a load: the rest at
 * once). An OS file call is made at a frame's end where the ring and the
 * queue hold the longest OS call yet (nspire_audio_save_frame: the game run
 * ahead a frame at a time first, its time not made up after the call, the
 * rest given back once the save is done; r19-snd: pace6's 70 ms of ring and
 * 4 frames at once ran dry under a hardware log's 229.7 ms write). */
/* speed10: the OS runs in a save's file calls here (with the
 * sound, nspire_audio_os_call_begin: interrupts on), and whatever it might
 * draw would stay in the screen buffers' groups that nspire_pictures.c
 * keeps: after each, the groups are forgotten (each buffer's next picture
 * made whole). update_backup's own file calls (a save written at once, one
 * finished while linked) run with interrupts off, as before: the OS's
 * tasks don't run in them. */
static void save_job_frame(void)
{
  int slot, r;

  if (states_save_busy() && nspire_link_state() == LINK_LINKED)
  {
    /* a link came up meanwhile: the rest at once, as a linked save is */
    nspire_clock_normal();
    states_save_finish();
    nspire_clock_game();
    nspire_reuse_pictures_forget();
  }
  else if (states_save_busy())
  {
    int file = states_save_next_is_file(), take = states_save_next_is_take();
    if (!file && !take)
      nspire_audio_serve_ahead(SAVE_PART_MS);
    if (nspire_audio_save_frame(1, take ? NSPIRE_SAVE_TAKE : file ? NSPIRE_SAVE_OS : NSPIRE_SAVE_CPU))
    {
      if (file)
        nspire_clock_normal();
      states_save_step(1);
      if (file)
      {
        nspire_clock_game();
        nspire_reuse_pictures_forget();
      }
    }
  }
  else if (nspire_backup_busy())
  {
    int file = nspire_backup_next_is_file();
    if (!file)
      nspire_audio_serve_ahead(SAVE_PART_MS);
    if (nspire_audio_save_frame(1, file ? NSPIRE_SAVE_OS : NSPIRE_SAVE_CPU))
    {
      nspire_backup_step();
      if (file)
        nspire_reuse_pictures_forget();
    }
  }
  else
    (void)nspire_audio_save_frame(0, NSPIRE_SAVE_CPU);   /* (the bank given back) */
  r = states_save_result(&slot);
  if (r == STATES_SAVE_DONE)
  {
    savestate_slot = (u32)slot;
    nspire_osd_message("%s to slot %d", save_job_words[states_save_result_tag() == 1], slot + 1);
  }
  else if (r == STATES_SAVE_FAILED)
    nspire_osd_message("Couldn't save to slot %d: %s", slot + 1, states_failure_words(STATES_SAVING));
  if (r != STATES_SAVE_IDLE && nspire_usbpad_message_posted)
    nspire_usbpad_message_posted();   /* (a pad's toast waits its time, as after a key) */
}

int nspire_save_state_now(void)
{
  return save_state_to(nspire_save_target(), save_job_words[0]);
}

/* ---- Autosave ----
 *
 * Off, or every 30 s, 1 to 5 min, 10, 15 or 30 min, with the increment
 * setting or without it. A state each time
 * the game has run that long: to a new slot after the highest, so none is
 * overwritten, or to the picked slot. Counted in the real-time clock's
 * seconds of play, so an older CX without a timer counts them too: a step
 * of more than AUTOSAVE_STEP seconds between frames (the menu, a load, the
 * clock set) isn't play. Not while linked (a save would hold up the other
 * calculator's game): at the first frame after. Any save starts it over. */
#define AUTOSAVE_STEP 2
const u32 nspire_autosave_seconds[AUTOSAVE_CHOICES] = { 0, 30, 60, 120, 180, 240, 300, 600, 900, 1800 };
const char *const nspire_autosave_names[AUTOSAVE_CHOICES] =
  { "off", "every 30 s", "every minute", "every 2 min", "every 3 min", "every 4 min", "every 5 min",
    "every 10 min", "every 15 min", "every 30 min" };
static u32 autosave_played, autosave_second;
static int autosave_counting;

void nspire_autosave_restart(void)
{
  autosave_played = 0;
  autosave_counting = 0;
}

static void autosave_frame(void)
{
  u32 every = nspire_autosave_seconds[nspire_autosave % AUTOSAVE_CHOICES], now;

  if (!every || !gamepak_filename[0])
  {
    nspire_autosave_restart();
    return;
  }
  now = nspire_rtc_seconds();
  if (!autosave_counting)
  {
    autosave_counting = 1;
    autosave_second = now;
    return;
  }
  if (now != autosave_second)
  {
    if (now - autosave_second <= AUTOSAVE_STEP)
      autosave_played += now - autosave_second;
    autosave_second = now;
  }
  if (autosave_played < every || nspire_link_state() == LINK_LINKED)
    return;
  save_state_to(nspire_autosave_new_slot ? states_next_new_slot() : (int)savestate_slot, save_job_words[1]);
}

int nspire_load_state_slot(int slot)
{
  nspire_audio_pause();
  int result = states_load(slot);

  if (result == 1)
  {
    savestate_slot = (u32)slot;
    nspire_osd_message("Loaded slot %d", slot + 1);
    nspire_autosave_restart();
  }
  else if (result == 0)
    nspire_osd_message("Slot %d is empty", slot + 1);
  else
    nspire_osd_message("Couldn't load slot %d: %s", slot + 1, states_failure_words(STATES_LOADING));
  return result;
}

void save_state(u8 *filename, u16 *screen_capture)
{
  (void)filename;
  (void)screen_capture;
  nspire_save_state_now();
}

/* ---- Starting and leaving games ---- */

/* ---- fast17: the start's steps, timed ----
 * gpSP's start up to the game list, a game's start up to its loading screen
 * closing, and on to the first frame: each step's ms in a line of the exit
 * marker (gpsp_link_run.txt.tns, "start steps, ms ...", before the picture
 * lines), as the exit's steps are. */
#define START_STEPS 24
static struct { const char *name; u32 ms; } start_steps[START_STEPS];
static unsigned start_step_count;
static u32 start_mark, start_from;

static u32 start_ms(u32 ticks)
{
  return (u32)(((u64)ticks * 1000u) / NSPIRE_TICK_HZ);
}

static void start_begin(void)
{
  start_step_count = 0;
  start_mark = start_from = nspire_ticks();
}

static void start_step(const char *name)
{
  u32 now = nspire_ticks();
  if (start_step_count < START_STEPS)
  {
    start_steps[start_step_count].name = name;
    start_steps[start_step_count].ms = start_ms(now - start_mark);
    start_step_count++;
  }
  start_mark = now;
}

/* the line (kept for the exit marker: nspire_picture_log_text), and the
 * steps start over from here; game: a game's start, else gpSP's */
static char start_line_gpsp[600], start_line_game[600];

static void start_steps_log(int game)
{
  char *text = game ? start_line_game : start_line_gpsp;
  unsigned size = game ? sizeof(start_line_game) : sizeof(start_line_gpsp), i;
  int n = snprintf(text, size, "start steps, ms (%s):", game ? "the last game's start, to its first frame"
                                                             : "gpSP's start, to the game list");

  for (i = 0; i < start_step_count && n >= 0 && (unsigned)n < size; i++)
    n += snprintf(text + n, size - n, "%s %s %lu", i ? "," : "", start_steps[i].name,
                  (unsigned long)start_steps[i].ms);
  if (n >= 0 && (unsigned)n < size)
    snprintf(text + n, size - n, "; total %lu ms", (unsigned long)start_ms(start_mark - start_from));
  start_step_count = 0;
  start_from = start_mark;
}

static int first_frame_timed;   /* a game's start: its first frame adds the last step */

static int cancel_start(void);

/* EXIT-3: the game's save was written by nspire_leave_game and nothing ran
 * the game since: quit() doesn't write it again (a second write of the
 * same bytes, 200-700 ms on the CX CAS by its log). */
static int game_left_saved;
static void first_run_hints(void);

static int start_game(const char *rom_path);

/* (r19-fix, core fix 7) a game's start and its leaving (the quit, a game switch) with
 * states_serve_os 0, from the paused menu too: their saves and loads as the quit and the start always
 * ran them (the exit path's comments and steps take the OS as not running there; the sound is paused
 * for them anyway); the menu's own saves, loads and pictures stay served */
int nspire_start_game(const char *rom_path)
{
  int was = states_serve_os, r;
  states_serve_os = 0;
  r = start_game(rom_path);
  states_serve_os = was;
  return r;
}

static int start_game(const char *rom_path)
{
  nspire_frameskip_screen_taken();   /* (r19-fix: the loading screen's) */
  nspire_audio_pause();
  char full_path[512], gbz_path[512];
  int newest, loaded, resume, resume_on_drive = 0;
  /* (taken once: a start that fails or is cancelled doesn't leave it for the next one) */
  int start_slot = nspire_start_slot, start_on_drive = nspire_start_slot_on_drive;

  nspire_start_slot = -1;
  game_left_saved = 0;
  nspire_start_cancelled = 0;
  start_begin();

  /* The file browser gives a name in the folder it's in. The game's saves,
   * states and settings are named after the ROM's path, and browsing for
   * another game changes folders, so the path is made absolute. A path is
   * already whole when it starts with a slash either way or a drive ("A:"):
   * the OS gives its folder as "\documents\gba\", so the games gpSP joined
   * to it ("\documents\gba\/Game.gbz.tns": the paused menu's other games)
   * start with a backslash. 2026-11-17: those were taken for names in the
   * folder and joined again ("\documents\gba\/\documents\gba\/Pokemon ...":
   * the CX II's gpsp_states_time.txt.tns, FAILED), so picking another game
   * on the paused menu never started it. */
  if (rom_path[0] != '/' && rom_path[0] != '\\' && !(rom_path[0] && rom_path[1] == ':') &&
      !nspire_drive_path(rom_path))
  {
    char cwd[512];
    size_t len;
    cwd[0] = 0;
    getcwd(cwd, sizeof(cwd));
    len = strlen(cwd);
    snprintf(full_path, sizeof(full_path), "%s%s%s", cwd, len && cwd[len - 1] == '/' ? "" : "/",
             rom_path);
    rom_path = full_path;
  }

  nspire_drive_game_begin(rom_path);
  nspire_loading_start(rom_path);
  start_step("loading screen");
  nspire_game_identify(rom_path);
  start_step("game identified");
  if (!nspire_drive_path(rom_path))
    nspire_game_move_old_files(rom_path);
  start_step("old files moved");
  nspire_config_open_game(rom_path);
  start_step("game's settings");
  nspire_apply_bios();
  start_step("BIOS");
  if (nspire_loading_cancelled())
    return cancel_start();
  gbz_load_ahead = 256 * 1024;   /* fast17: a .gbz read 256 KB a read while it loads (nspire_gbz.h) */
  /* save17: the saves' kept memory given back for the ROM's load (a drive's ROM, a zip, grows the ROM
   * buffer to the whole game), taken again after it */
  states_drop_buffers();
  loaded = load_gamepak(NULL, rom_path, nspire_rtc_force_value(), FEAT_AUTODETECT,
                        SERIAL_MODE_AUTO) == 0;
  gbz_load_ahead_end();
  if (!loaded && nspire_rom_load_cancelled)
    return cancel_start();
  /* A zip too big for the memory free becomes a .gbz, which needs little,
   * and that loads. Its saves, states and settings are the zip's: both
   * are named after the ROM in the zip. */
  if (!loaded && nspire_rom_load_need_mb && !nspire_drive_path(rom_path))
  {
    int made = nspire_loading_zip_to_gbz(rom_path, gbz_path, sizeof(gbz_path));
    if (made < 0)
      return cancel_start();
    if (made)
    {
      rom_path = gbz_path;
      nspire_loading_start(rom_path);
      gbz_load_ahead = 256 * 1024;
      loaded = load_gamepak(NULL, rom_path, nspire_rtc_force_value(), FEAT_AUTODETECT,
                            SERIAL_MODE_AUTO) == 0;
      gbz_load_ahead_end();
      if (!loaded && nspire_rom_load_cancelled)
        return cancel_start();
    }
  }
  states_keep_buffers();
  states_log_buffers();
  if (!loaded)
  {
    nspire_config_close_game();
    nspire_loading_failed(nspire_drive_load_error(nspire_rom_load_error));
    return 0;
  }
  start_step("ROM loaded");
  nspire_loading_rom_note();
  if (nspire_loading_cancelled())
    return cancel_start();
  nspire_game_claim();
  nspire_link_apply_device();
  reset_gba();
  instruction_count = 0;
  reg[OAM_UPDATED] = 1;
  nspire_frameskip_reset();
  start_step("reset");
  nspire_load_cartridge_backup();
  start_step("game's save read");
  /* the game's slots: as the list or the paused menu read them when they
   * did (not ~15 ms a slot again on the CX II, ~50 on the CX) */
  states_open_game_kept(gamepak_filename);
  /* Saves on: USB drive (nspire_drive_saves.h; a safe point: the drive is read here): the game's
   * states on the drive looked up, its save there and the calculator's made one, before it runs */
  nspire_saves_game_start();
  nspire_backup_drive_load();
  newest = states_newest();
  savestate_slot = newest >= 0 ? (u32)newest : 0;
  nspire_osd_clear();
  synchronize_flag = 1;
  /* (the newest state of both places then: the drive's when it is the later one) */
  resume = nspire_saves_resume(&resume_on_drive);
  start_step("states looked up");
  /* UI17: a state picked in the Saves place is the one loaded, whatever Resume last save says */
  if (start_slot >= 0)
  {
    resume = start_slot;
    resume_on_drive = start_on_drive;
  }

  if ((nspire_load_on_start || start_slot >= 0) && !nspire_skip_state_load && resume >= 0)
  {
    char line[40];
    snprintf(line, sizeof(line), "Loading slot %d", resume + 1);
    nspire_loading_note(line);
    if (nspire_loading_cancelled())
      return cancel_start();
    if (resume_on_drive)
      loaded = states_drive_load(resume) == 1;
    else
      loaded = states_load(resume) == 1;
    if (loaded)
      nspire_osd_message(resume_on_drive ? "Resumed from slot %d (USB drive)" : "Resumed from slot %d", resume + 1);
    else
    {
      /* UI17: a picked slot that can't be loaded (its drive taken out, a damaged file): the calculator's newest;
       * what went wrong said for the picked one, and for that one too when it fails as well */
      char why[32];
      int fallback = start_slot >= 0 ? states_newest() : -1;
      snprintf(why, sizeof(why), "%s", resume_on_drive && !states_failure(STATES_LOADING)->why ? "USB drive" :
                                       states_failure_words(STATES_LOADING));
      if (fallback < 0 || (!resume_on_drive && fallback == resume))
        nspire_osd_message(resume_on_drive ? "Couldn't resume from slot %d (USB drive)" : "Couldn't resume from slot %d: %s",
                           resume + 1, why);
      else if (states_load(fallback) == 1)
        nspire_osd_message("Slot %d: %s. Resumed from slot %d.", resume + 1, why, fallback + 1);
      else
        nspire_osd_message("Slot %d: %s. Slot %d: %s.", resume + 1, why, fallback + 1,
                           states_failure_words(STATES_LOADING));
    }
    if (nspire_loading_cancelled())
      return cancel_start();
  }
  nspire_skip_state_load = 0;
  start_step("state loaded");
  nspire_autosave_restart();   /* a new game: its first autosave a whole wait away */
  nspire_loading_end();
  /* saves wanted on a USB drive and kept on the calculator, or the drive's save older: said once
   * (after "Resumed from slot 2", when that is up) */
  nspire_saves_say();
  first_run_hints();
  /* The key that started it (Enter, or A) doesn't press the game's buttons. */
  keys_mask_held();
  nspire_lcd_report_restart();   /* F3: the first line counts from here */
  nspire_reset_timing();
  picture_log_pending = 1;
  picture_later_pending = 0;
  nspire_drive_game_end(1);
  start_step("loading screen closed");
  first_frame_timed = 1;
  return 1;
}

int nspire_start_cancelled;

/* The very first game on a calculator: "The keys" (the keypad with the
 * basic keys marked) before the game, and a message over it saying where the
 * menu is. Once: gpsp_hints.txt.tns in the saves folder remembers (it is
 * written after the screen, so a crash before it shows it again). */
static void first_run_hints(void)
{
  char path[512];
  FILE *f;

  if (nspire_first_run_hints && !nspire_first_run_hints())
    return;
  nspire_save_file(path, sizeof(path), "gpsp_hints.txt.tns");
  f = fopen(path, "r");
  if (f)
  {
    fclose(f);
    return;
  }
  nspire_controls_hint();
  f = fopen(path, "w");
  if (f)
  {
    fputs("keys=1\n", f);
    fclose(f);
  }
  /* settles on hardware whether the marker can be written (else the screen comes at every start) */
  nspire_link_log("ui: first run: The keys shown, %s written", f ? "marker" : "no marker (could not be)");
  nspire_osd_message("Press menu for the game menu");
}

/* Q on the loading screen: as if the game was never started. Nothing is
 * written (no settings file, no save, no state). */
static int cancel_start(void)
{
  nspire_loading_end();
  nspire_config_drop_game();
  gamepak_filename[0] = 0;
  nspire_skip_state_load = 0;
  nspire_start_cancelled = 1;
  keys_wait_release();
  return 0;
}

/* EXIT-3: gpSP is quitting: the log is written once, last (nspire_exit.c). */
static int quitting;
/* The quit question's answer: 1 save, -1 don't, 0 as Save when I quit says. */
int nspire_exit_save;

static void leave_game(void);

void nspire_leave_game(void)
{
  int was = states_serve_os;
  states_serve_os = 0;   /* (r19-fix: as the quit always ran: nspire_start_game) */
  leave_game();
  states_serve_os = was;
}

static void leave_game(void)
{
  states_save_finish();   /* a state being saved is written first */
  nspire_audio_pause();
  nspire_link_disconnect();
  if (!quitting)
    nspire_link_log_flush();
  nspire_exit_step("link closed");
  if (!gamepak_filename[0])
    return;
  nspire_clock_normal();
  nspire_exit_step("clock normal");
  nspire_perf_log_flush();
  nspire_exit_step("perf log written");
  /* Saves on: USB drive (a safe point; a drive that is only attached is not opened for it here: no
   * wait on the way out): the states the frames saved copied to the drive, the save's copy there */
  if (nspire_saves_on == PLACE_DRIVE)
  {
    nspire_saves_sync(0);
    nspire_exit_step("USB drive's saves written");
  }
  if (nspire_exit_save > 0 || (nspire_exit_save == 0 && nspire_save_on_exit))
  {
    /* (on the drive when Saves on says so and it takes it; else, and when it fails, the calculator) */
    if (!nspire_saves_exit_save())
    {
      int slot = nspire_save_target();
      if (slot >= 0 && states_save(slot))
        savestate_slot = (u32)slot;
    }
    nspire_exit_step("state saved");
  }
  update_backup_force();
  nspire_exit_step("game's save written");
  nspire_config_close_game();
  nspire_exit_step("game's settings closed");
  game_left_saved = 1;
}

/* R11w (2026-11-15): every file first (the game's save, the state times, the
 * folder, the perf log, the pad log with its milestones and the driver's
 * block, the link log), then the USB shutdowns, each within the exit's
 * budget (nspire_exit.h EXIT_USB_MS). R11w hung inside them with its pad
 * log written and nothing after it: the keys dead, a reset. One that does
 * not finish in its time now restarts the calculator (a plain message for
 * two seconds first) instead of unloading code the OS can still call. The
 * one write after them: the perf log's sound seconds, which exist only once
 * the USB audio stopped (nspire_audio.c), and the clean marker at the very
 * end (nspire_exit.c). */
void quit(void)
{
  int untouched;

  nspire_frameskip_screen_taken();   /* (r19-fix: the closing screen's) */
  picture_at_quit();
  nspire_exit_begin();
  nspire_exit_step("quit");
  nspire_exit_trail("quit: the sound paused, the clock normal");
  nspire_audio_pause();
  nspire_clock_normal();
  nspire_clock_left_game();
  nspire_exit_step("clock normal");
  nspire_exit_trail("the code back from the on-chip SRAM");
  nspire_sram_stop();
  nspire_exit_step("code back from the on-chip SRAM");
  nspire_exit_trail("the perf log");
  nspire_perf_log_flush();   /* (unless a sound adapter is open: then after its stop) */
  nspire_exit_step("perf log written");
  nspire_exit_trail("a state under way finished, the state times");
  states_save_finish();   /* (save17: a state being saved is written now, before the drive's shutdown) */
  states_timing_flush();
  nspire_exit_step("state times written");
  nspire_exit_trail("the folder file");
  save_romdir();
  nspire_exit_step("folder written");
  if (!game_left_saved)
  {
    nspire_exit_trail("the game's save");
    update_backup_force();
    nspire_exit_step("game's save written");
  }
  nspire_exit_trail("the pad log");
  nspire_usbpad_exit_log();
  nspire_exit_step("pad log written");
  nspire_exit_trail("Bluetooth's key store");
  nspire_bt_glue_flush();   /* (Bluetooth's key store: no file is written in its stop) */
  nspire_exit_trail("the quitting screen, the USB shutdowns' clock and backstop");
  nspire_show_closing();
  nspire_exit_usb_begin();
  /* 2026-11-16: the OS's USB stopped before the exit (nspire_usbpad_glue.c
   * watch_stuck: the port enabled for each plug, nothing offered any more).
   * In that state the r13-hub run's calculator froze after a clean exit and
   * r12's quit hung: the calculator is restarted instead (the files are
   * written; this is the exit's own restart, as for a shutdown that timed
   * out). */
  if (nspire_usbpad_os_dead())
    nspire_exit_usb_timed_out("the OS's USB (it had stopped before the exit)");
  /* (r19-usb-exit) No device ever offered to the OS's drivers and the port
   * quiet (nspire_usbpad_exit_untouched: nothing plugged in, gated, or the
   * cable's plain end, the same for a second): the sound's and the drive's
   * shutdowns without their waits for callbacks that never ran (10 OS ticks
   * each; a CX's hardware run: 669 and 100 ms, its pad driver's 101). */
  untouched = nspire_usbpad_exit_untouched();
  nspire_exit_trail(untouched ? "USB sound's stop (nothing was ever offered: no wait)" : "USB sound's stop");
  if (untouched && nspire_audio_stop_quiet() == 0 && !nspire_usbaudio_owns_usb())
    nspire_exit_step("USB audio shut down (no wait)");
  else
  {
    if (nspire_audio_stop_within(nspire_exit_usb_left_ms()) != 0 ||
        nspire_usbaudio_owns_usb())
      nspire_exit_usb_timed_out("USB sound");
    nspire_exit_step("USB audio shut down");
  }
  /* Bluetooth: its links dropped, BTstack off, its key store written, the
   * dongle's pipes closed and its driver retired, within what is left (1.5 s
   * at most); a USB callback still inside gpSP at the end: the restart. */
  nspire_exit_trail("Bluetooth's stop (its links, BTstack, the dongle's pipes, its driver)");
  {
    unsigned left = nspire_exit_usb_left_ms() / 2;   /* (half: the pads, the link and the drive after it) */
    if (nspire_bt_glue_stop_within(left < 1500 ? left : 1500) != 0)
      nspire_exit_usb_timed_out("Bluetooth");
  }
  nspire_exit_step("Bluetooth shut down");
  nspire_exit_trail("the pad driver's stop");
  if (nspire_usbpad_stop_within(nspire_exit_usb_left_ms()) != 0)
    nspire_exit_usb_timed_out("pad driver");
  nspire_exit_step("pad driver shut down");
  nspire_exit_trail("the link's stop (NavNet StopService)");
  if (nspire_link_shutdown_within(nspire_exit_usb_left_ms()) != 0)
    nspire_exit_usb_timed_out("link (NavNet StopService)");
  nspire_os_keypad_keep_off();
  nspire_os_after_os_time();
  nspire_exit_step("link shut down (NavNet StopService)");
  nspire_exit_trail(nspire_drive_state() >= DRIVE_ATTACHED ? "the USB drive's stop (a drive in: unmounted, its pipes closed)" :
                                            "the USB drive's stop (no drive in)");
  if (untouched && nspire_drive_stop_quiet() == 0)
    nspire_exit_step("USB drive shut down (no wait)");
  else
  {
    if (nspire_drive_stop_within(nspire_exit_usb_left_ms()) != 0)
      nspire_exit_usb_timed_out("USB drive");
    nspire_os_keypad_keep_off();
    nspire_os_after_os_time();
    nspire_exit_step("USB drive shut down");
  }
  nspire_exit_trail("the USB shutdowns' end (the backstop off)");
  nspire_exit_usb_end();
  /* (r19-usb-exit) these were in "screen back" (nspire_restore's first
   * step): a CX II's quit had 384 ms there, the sound's seconds
   * (43 KB on the CX) and Bluetooth's log written after the USB stops */
  nspire_exit_trail("the perf log (the sound's seconds)");
  nspire_perf_log_flush();   /* the sound's seconds, once its adapter is shut */
  nspire_exit_step("perf log written (the sound's seconds)");
  nspire_exit_trail("Bluetooth's log");
  nspire_bt_glue_write_log();   /* gpsp_bt.txt.tns: Bluetooth's log, once it is stopped */
  nspire_exit_step("Bluetooth log written");
  nspire_exit_trail("memory and brightness given back");
  memory_term();
  states_drop_buffers();
  nspire_brightness_restore();
  nspire_exit_step("memory and brightness given back");
  /* R6's wait for the keys is in the hand-back now (nspire_exit.c): the
   * OS's keypad line stays off until then. */
  nspire_exit_trail("nspire_restore: the screen back, the OS's file list, then the exit handlers");
  nspire_restore();
  exit(0);
}

/* Leaves the game the way Save+Exit and "Exit gpSP" do. */
void nspire_quit_game(void)
{
  states_serve_os = 0;   /* (r19-fix: the quit's saves as it always ran them, from the menu too; never back) */
  nspire_frameskip_screen_taken();
  picture_at_quit();
  nspire_exit_begin();
  nspire_exit_step("quit chosen");
  nspire_exit_trail("quit chosen: the game left (its state and save), the settings");
  quitting = 1;
  nspire_leave_game();
  save_config_file();
  nspire_exit_step("settings written");
  quit();
}

void delay_us(u32 us_count)
{
  nspire_wait_until(nspire_ticks() + (u32)(((u64)us_count * NSPIRE_TICK_HZ) / 1000000));
}

void get_ticks_us(u64 *ticks_return)
{
  *ticks_return = (u64)nspire_ticks() * 1000000ULL / NSPIRE_TICK_HZ;
}

static void capture_initial_sp(void)
{
#if defined(__arm__)
  uintptr_t sp;
  __asm__ volatile("mov %0, sp" : "=r"(sp));
  not_crt0_savedsp = (uint32_t)sp;
#endif
}

/* The BIOS file couldn't be used: the screen says so, any key goes on. */
void nspire_bios_error_wait(const char *msg)
{
  struct ui_loading_view v;
  gui_action_type gui_action = CURSOR_NONE;

  memset(&v, 0, sizeof(v));
  v.what = "BIOS problem";
  v.title = "The GBA BIOS";
  v.line = msg;
  v.warn = 1;
  v.more = "Go on";
  ui_loading_draw(&v);
  while (gui_action == CURSOR_NONE)
  {
    gui_action = get_gui_input();
    delay_us(15000);
  }
}

int nspire_apply_bios(void)
{
  if (nspire_bios_choice == 0)
  {
    memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
    return 0;
  }

  {
    char path[600];

    bios_path(path, sizeof(path));
    if (load_bios(path) != 0 || bios_rom[0] != 0x18U)
    {
      memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
      nspire_bios_choice = 0;
    }
    return 0;
  }
}

static void core_bootstrap(void)
{
  nspire_rom_prepare_saves = states_keep_buffers;
  init_gamepak_buffer();
  init_sound();
  if (!gba_screen_pixels)
  {
    gba_screen_pixels = (u16 *)malloc(GBA_SCREEN_BUFFER_SIZE);
    if (!gba_screen_pixels)
      quit();
    memset(gba_screen_pixels, 0, GBA_SCREEN_BUFFER_SIZE);
  }
  selected_boot_mode = boot_game;
}

/* ---- Playing ---- */

static int in_speed_test;

/* The game jumped where no code can run (nspire_bad_jump, cpu.h): a damaged
 * ROM (an Advance Wars from a bad dump does it as it starts) or a game gone
 * wrong. The core stopped there instead of crashing the calculator. This
 * says so and closes the game without saving a state of it (it's in
 * pieces); then the file list comes up again, as after a game that doesn't
 * load (leaving it opens the menu, without a game). */
static void game_stopped(void)
{
  char why[96];
  u8 load_filename[512];
  int started = 0;

  nspire_frameskip_screen_taken();   /* (r19-fix: the stopped game's screen) */
  nspire_clock_normal();
  /* (r19-fix, core fix 3: a save whose state isn't taken yet is dropped, not taken of the game in
   * pieces; one taken before the jump is the game as it was and is written) */
  states_save_before_change(STATES_CHANGE_BROKEN);
  states_save_finish();
  nspire_perf_log_flush();
  nspire_input_release_all();
  video_resolution_large();
  if (nspire_rom_page_damaged)
    snprintf(why, sizeof(why), "A page of its .gbz doesn't read:\nthe file is damaged.");
  else
    snprintf(why, sizeof(why), "It jumped to %08X, where nothing\ncan run: its ROM is probably damaged.",
             (unsigned)nspire_bad_jump_pc);
  nspire_bad_jump = 0;
  nspire_rom_page_damaged = 0;
  nspire_game_stopped(why);
  update_backup_force();
  nspire_config_close_game();
  gamepak_filename[0] = 0;

  switch_to_romdir();
  while (!started)
  {
    if (load_file(file_ext, load_filename) == -1)
    {
      /* r19: the list is two tabs of the top row: left without a game picked (the arrows went to another
       * tab, or esc), the same row goes on in the menu (gui.c list_left: Settings, or Quit) */
      menu(copy_screen());
      break;
    }
    started = nspire_start_game((char *)load_filename);
  }
  if (started)
  {
    set_gba_resolution((video_scale_type)screen_scale);
    video_resolution_small();
  }
  nspire_clock_game();
  nspire_reset_timing();
}

/* 2026-11-11: a line about the picture's hardware at a game's first frame,
 * after each menu and when the game is quit (the CX CAS showed parts of the
 * picture drawn out of place after the menu; the PC model's pictures don't
 * change with it): the LCD (nspire_lcd_report: its refresh, registers, DMA
 * underflow, flips that found it on the next buffer), the clock, frameskip,
 * blending, size. Never per frame. Into the log when this run has one (a
 * first write would rotate the older logs), and the first, the last menu's
 * and the quit's into the exit marker (nspire_picture_log_text).
 * picture_log_pending (declared at the top): a game just started.
 * F3: and one more 1 s after the start and after each menu (not
 * timed: no busy wait), so that the menu's exit (the line right after it)
 * and the play after it (the line 1 s later) are told apart, and the play
 * after the menu can be set against the play after the start. */
#define PICTURE_LINE 560
static char picture_first[PICTURE_LINE], picture_first_later[PICTURE_LINE], picture_menu[PICTURE_LINE],
  picture_menu_later[PICTURE_LINE], picture_quit[PICTURE_LINE];
static unsigned picture_menus;
static u32 picture_later_at;
static char picture_later_when[40], *picture_later_keep;

static void log_picture_timed(const char *when, char *keep, u32 keep_size, int timed)
{
  char lcd[300], clock[200], skip[64], blend[120];

  nspire_lcd_report(lcd, sizeof(lcd), timed);
  nspire_clock_report(clock, sizeof(clock));
  nspire_frameskip_describe(skip, sizeof(skip));
  nspire_video_blend_report(blend, sizeof(blend));
  snprintf(keep, keep_size, "picture %s: %s; %s; frameskip %s, %s, size %u", when, lcd, clock, skip, blend,
           (unsigned)screen_scale);
  if (nspire_link_log_started())
    nspire_link_log("%s", keep);
}

static void log_picture(const char *when, char *keep, u32 keep_size)
{
  log_picture_timed(when, keep, keep_size, 1);
}

/* The untimed line 1 s after a start or a menu: picture_later_when says which. */
static void picture_later_start(const char *when, char *keep)
{
  snprintf(picture_later_when, sizeof(picture_later_when), "%s", when);
  picture_later_keep = keep;
  keep[0] = 0;
  picture_later_at = nspire_ticks();
  picture_later_pending = 1;
}

void nspire_picture_log_text(char *text, u32 size)
{
  const char *parts[7] = { start_line_gpsp, start_line_game, picture_first, picture_first_later, picture_menu,
                           picture_menu_later, picture_quit };
  u32 n = 0, i;

  if (size)
    text[0] = 0;
  for (i = 0; i < 7 && n < size; i++)
    if (parts[i][0])
      n += (u32)snprintf(text + n, size - n, "%s\n", parts[i]);
}

/* Before the quit's steps (they time from "quit chosen"), once. */
static void picture_at_quit(void)
{
  if (!picture_quit[0] && gamepak_filename[0] && picture_first[0])
    log_picture("at the quit", picture_quit, sizeof(picture_quit));
}

static void run_frame(void)
{
  u32 output_ticks = 0;

  /* Discard old PCM before frame pacing or logging can yield to USB. */
  nspire_audio_frame_begin(!in_speed_test && nspire_pace == PACE_NORMAL ? (synchronize_flag ? 1 : 2) : 0);

  if (picture_log_pending)
  {
    picture_log_pending = 0;
    if (first_frame_timed)
    {
      first_frame_timed = 0;
      start_step("to the first frame");
      start_steps_log(1);
    }
    log_picture("at the game's first frame", picture_first, sizeof(picture_first));
    picture_later_start("1 s after the game's start", picture_first_later);
    /* F5: the line's busy wait (up to 0.1 s, two LCD refreshes) is
     * not the game's first frame's time (automatic frameskip's costs, the
     * counter) */
    nspire_reset_timing();
  }
  else if (picture_later_pending && nspire_ticks() - picture_later_at >= NSPIRE_TICK_HZ)
  {
    picture_later_pending = 0;
    log_picture_timed(picture_later_when, picture_later_keep, PICTURE_LINE, 0);
  }
  rumble_frame_reset();
  nspire_link_frame_begin();
  nspire_frameskip_begin_frame();
  if (!skip_next_frame)
    nspire_video_frame_start();

#ifdef NSPIRE_NO_INTERPRETER
  execute_arm_translate(execute_cycles);   /* the calculator: the dynarec only */
#else
#ifdef HAVE_DYNAREC
  if (dynarec_enable)
    execute_arm_translate(execute_cycles);
  else
#endif
  {
    clear_gamepak_stickybits();
    execute_arm(execute_cycles);
  }
#endif
  if (nspire_bad_jump || nspire_rom_page_damaged)
  {
    /* The speed test stops and puts the game back as it was. */
    if (!in_speed_test)
      game_stopped();
    return;
  }

  process_cheats();
  /* (2026-11-17, final/speed8) the emulation done: one of the points where a
   * pad's OS tick may be taken (nspire_frameskip.c) */
  nspire_frameskip_os_point(NSPIRE_OSP_PRESENT);
  if (!skip_next_frame)
  {
    u32 start = nspire_ticks();
    nspire_present_frame();
    output_ticks = nspire_ticks() - start;
  }
  nspire_frameskip_end_frame(output_ticks);
  update_backup();

  switch (serial_mode)
  {
  case SERIAL_MODE_RFU:
    rfu_frame_update();
    break;
  case SERIAL_MODE_SERIAL_POKE:
    serialpoke_frame_update();
    break;
  case SERIAL_MODE_SERIAL_MSIO:
    serialmsio_frame_update();
    break;
  default:
    break;
  }
  nspire_audio_frame_end();
  save_job_frame();
  nspire_link_frame_end();
}

/* nspire_link.h: the other GBA hasn't come for a while (it's slower, in
 * its menu, or gone): a screen saying so, drawn once and again only when
 * the reason changes. */
unsigned nspire_link_wait_screens;   /* waits that showed the screen (the PC's report) */

void nspire_link_waiting(unsigned waited_ms, int other_in_menu)
{
  static int shown = -1;

  if (waited_ms > LINK_WAIT_SHOW_MS + 50 && shown == other_in_menu)
    return;
  if (waited_ms < LINK_WAIT_SHOW_MS + 20)   /* a wait's first call (20 ms slices) */
    nspire_link_wait_screens++;
  shown = other_in_menu;
  nspire_frameskip_screen_taken();   /* (r19-fix: the wait's screen) */
  ui_link_wait_draw(other_in_menu);
  nspire_present_forget_bands();
}

/* LNK-1: the keypad as it is now (the frame's keys were read before the
 * wait), and only an esc that went down since the wait began: one held from
 * the game (down at the frame's read, or at the wait's first slice) has to
 * be let go first. */
int nspire_link_wait_cancelled(unsigned waited_ms)
{
  static int esc_was_up;
  int down = key_down_now(KEY_ESC);

  if (waited_ms <= 20)
    esc_was_up = !key_down(KEY_ESC) && !down;
  if (!down)
    esc_was_up = 1;
  return down && esc_was_up;
}

static void open_menu(void)
{
  nspire_frameskip_screen_taken();   /* (r19-fix: a fast forward picture waiting: shown now or let go) */
  nspire_clock_normal();
  /* r19-snd: the sound stopped first (faded: nspire_audio.c soft_stop), then a state being saved
   * finished, its OS calls and the menu's with the queue served (silence on the wire, no gap) */
  nspire_audio_pause();
  states_serve_os = 1;
  states_save_finish();   /* (a state being saved: the menu's lists and files see it) */
  nspire_perf_log_flush();
  nspire_input_release_all();
  nspire_link_menu(1);
  /* Saves on: USB drive: the menu is a safe point (nspire_drive_saves.h): before it opens, the
   * states the frames saved are copied to the drive and the save's copy there is brought up to
   * date (nothing to do: no drive is touched) */
  {
    u16 *screen = copy_screen();
    if (nspire_saves_on == PLACE_DRIVE)
      nspire_saves_sync(1);
    /* Called directly, on this stack: the frame has ended, so the dynarec
     * isn't using it. */
    menu(screen);
  }
  states_serve_os = 0;
  keys_wait_release();
  nspire_link_menu(0);
  /* what there is to say of the saves' place, once: now, or when the menu's toast has had its time */
  nspire_saves_say();
  nspire_clock_game();
  {
    char when[40];
    snprintf(when, sizeof(when), "after the menu (%u)", ++picture_menus);
    log_picture(when, picture_menu, sizeof(picture_menu));
    snprintf(when, sizeof(when), "1 s after the menu (%u)", picture_menus);
    picture_later_start(when, picture_menu_later);
  }
  nspire_reset_timing();
}

static void change_slot(int delta)
{
  savestate_slot = (savestate_slot + STATE_SLOTS + delta) % STATE_SLOTS;
  /* (r19-fix: a slot being saved counts as saved; nothing finished for it: states_exists finished a save
   * under way at once, masked) */
  nspire_osd_message(states_exists_soon((int)savestate_slot) ? "Slot %d (saved)" : "Slot %d (empty)",
                     (int)savestate_slot + 1);
}

#define HOTKEY(action) (hotkeys & (1u << (action)))

/* The hotkeys pressed this frame (nspire_input_frame). */
static void do_hotkeys(u32 hotkeys)
{
  if (HOTKEY(ACTION_MENU))
  {
    open_menu();
    return;
  }
  if (HOTKEY(ACTION_QUIT))
    nspire_quit_game();
  if (HOTKEY(ACTION_SAVE_STATE))
    nspire_save_state_now();   /* (a frame's time now: no new count) */
  if (HOTKEY(ACTION_LOAD_STATE))
  {
    states_serve_os = 1;   /* (r19-snd: the load's OS calls with the queue served) */
    nspire_load_state_slot((int)savestate_slot);
    states_serve_os = 0;
    nspire_reset_timing();
  }
  if (HOTKEY(ACTION_LOAD_NEWEST))
  {
    states_serve_os = 1;
    if (states_newest() >= 0)
      nspire_load_state_slot(states_newest());
    else
      nspire_osd_message("No saves yet");
    states_serve_os = 0;
    nspire_reset_timing();
  }
  if (HOTKEY(ACTION_NEXT_SLOT))
    change_slot(1);
  if (HOTKEY(ACTION_PREVIOUS_SLOT))
    change_slot(-1);
  if (HOTKEY(ACTION_SHOW_FPS))
  {
    nspire_fps_overlay = nspire_fps_overlay ? 0 : 1;
    nspire_osd_message(nspire_fps_overlay ? "FPS counter on" : "FPS counter off");
  }
  if (HOTKEY(ACTION_FAST_FORWARD))
  {
    synchronize_flag ^= 1;
    nspire_osd_message(synchronize_flag ? "Fast forward off" : "Fast forward on");
    nspire_reset_timing();
  }
  if (HOTKEY(ACTION_RESTART))
  {
    /* (r19-fix, core fix 3: a save keyed a frame or a few before has its state taken now, not
     * the restarted game's at a frame's end after) */
    states_save_before_change(STATES_CHANGE_KEEP);
    nspire_apply_bios();
    reset_gba();
    nspire_osd_message("Game restarted");
    nspire_reset_timing();
  }
  if (HOTKEY(ACTION_BRIGHTER))
    nspire_brightness_step(1);
  if (HOTKEY(ACTION_DIMMER))
    nspire_brightness_step(-1);
}

static void emulation_loop(void)
{
  nspire_reset_timing();
  while (1)
  {
    u32 hotkeys = nspire_input_frame();
    if (hotkeys)
      do_hotkeys(hotkeys);
    autosave_frame();
    nspire_saves_say();   /* (a line that waited for a toast to end; nothing of the drive) */
    run_frame();
  }
}

/* ---- Speed test ---- */

struct speed_test_part
{
  const char *name;
  enum nspire_pace_mode pace;
  u32 seconds;
};

static const struct speed_test_part speed_test_parts[] =
{
  { "Normal play, frameskip off", PACE_NORMAL, 10 },
  { "No limit, every frame drawn", PACE_UNCAPPED_DRAW_ALL, 5 },
  { "No limit, nothing drawn", PACE_UNCAPPED_DRAW_NONE, 5 },
  { "Fast forward, frameskip off", PACE_FAST_FORWARD, 5 },
};

#define SPEED_TEST_PARTS (int)(sizeof(speed_test_parts) / sizeof(speed_test_parts[0]))
#define TENTHS(v) (unsigned)((v) / 10), (unsigned)((v) % 10)

/* Runs the game from where it is in four ways and reports frames per
 * second and where each frame's time goes; the game is put back as it was.
 * Called from the menu. */
void nspire_speed_test(void)
{
  struct nspire_perf_stats stats[SPEED_TEST_PARTS];
  void *snapshot;
  u32 saved_type = current_frameskip_type;
  u32 saved_value = frameskip_value, saved_fps = nspire_fps_overlay, saved_sync = synchronize_flag;
  u32 mhz;
  int done = 0, stopped = 0, i;
  const unsigned char *own = nspire_gui_own_keys;
  char path[600], line[300];
  FILE *f;

  if (!gamepak_filename[0])
    return;
  snapshot = states_snapshot();
  if (!snapshot)
  {
    nspire_osd_message("Not enough memory for the speed test");
    return;
  }

  nspire_gui_own_keys = NULL;
  nspire_frameskip_screen_taken();
  video_resolution_small();
  nspire_clock_game();
  mhz = nspire_cpu_mhz();
  keys_wait_release();
  synchronize_flag = 1;
  in_speed_test = 1;
  for (i = 0; i < SPEED_TEST_PARTS && !stopped; i++)
  {
    const struct speed_test_part *part = &speed_test_parts[i];
    struct nspire_perf base;
    u32 length = part->seconds * NSPIRE_TICK_HZ;

    states_restore(snapshot);
    current_frameskip_type = no_frameskip;
    nspire_fps_overlay = 2;
    nspire_pace = part->pace;
    nspire_osd_message("Speed test %d/%d (esc stops it)", i + 1, SPEED_TEST_PARTS);
    nspire_reset_timing();
    nspire_perf_on = 1;
    base = nspire_perf_totals;
    while (nspire_ticks() - base.start < length &&
           nspire_perf_totals.frames - base.frames < part->seconds * 400)
    {
      nspire_input_release_all();
      keys_poll();
      if (key_pressed(KEY_ESC) || nspire_gui_button_pressed(ACTION_B))
      {
        stopped = 1;
        break;
      }
      run_frame();
      if (nspire_bad_jump || nspire_rom_page_damaged)
      {
        stopped = 1;
        break;
      }
    }
    stats[i] = nspire_perf_since(&base, nspire_ticks());
    done = i + 1;
  }

  in_speed_test = 0;
  nspire_bad_jump = 0;
  nspire_rom_page_damaged = 0;
  nspire_pace = PACE_NORMAL;
  current_frameskip_type = saved_type;
  frameskip_value = saved_value;
  nspire_fps_overlay = saved_fps;
  synchronize_flag = saved_sync;
  states_restore(snapshot);
  free(snapshot);
  nspire_osd_clear();
  nspire_clock_normal();
  video_resolution_large();

  /* A new file each run: appending has garbled files on the calculator. */
  nspire_save_file(path, sizeof(path), "gpsp_results.txt.tns");
  f = fopen(path, "w");
  if (f)
  {
    fprintf(f, "== %s (%s), cpu %u MHz, %s LCD, scaling %u\n", gamepak_title, gamepak_code,
            (unsigned)mhz, nspire_lcd_portrait ? "portrait" : "landscape", (unsigned)screen_scale);
    nspire_clock_report(line, sizeof(line));
    fprintf(f, "%s\n", line);
    nspire_brightness_report(line, sizeof(line));
    fprintf(f, "%s\n", line);
    nspire_timer_report(line, sizeof(line));
    fprintf(f, "%s\n", line);
    for (i = 0; i < done; i++)
      fprintf(f, "%s:\n  game %u.%u fps, drawn %u.%u; per frame: cpu %u.%u, gfx %u.%u, out %u.%u, idle %u.%u ms\n",
              speed_test_parts[i].name, TENTHS(stats[i].game_fps), TENTHS(stats[i].drawn_fps),
              TENTHS(stats[i].cpu_ms), TENTHS(stats[i].render_ms), TENTHS(stats[i].output_ms),
              TENTHS(stats[i].idle_ms));
    fclose(f);
  }

  /* Results, until a key is pressed. */
  {
    struct ui_speed_view view;
    memset(&view, 0, sizeof(view));
    view.mhz = (unsigned)mhz;
    view.stopped = stopped;
    for (i = 0; i < done && i < 4; i++)
    {
      struct ui_speed_part *p = &view.parts[view.count++];
      p->name = speed_test_parts[i].name;
      p->fast_forward = speed_test_parts[i].pace == PACE_FAST_FORWARD;
      p->game_fps = stats[i].game_fps;
      p->drawn_fps = stats[i].drawn_fps;
      p->cpu_ms = stats[i].cpu_ms;
      p->gfx_ms = stats[i].render_ms;
      p->out_ms = stats[i].output_ms;
    }
    keys_wait_release();
    ui_speed_draw(&view);
    do
    {
      update_at_vblank();
      keys_poll();
    } while (!any_key_down() && !nspire_gui_button_down(ACTION_A) && !nspire_gui_button_down(ACTION_B));
  }
  keys_wait_release();
  nspire_gui_own_keys = own;
  nspire_reset_timing();
}

/* ---- Start ---- */

/* ROMs opened from the calculator's documents start this gpSP: Ndless keeps
 * file associations in ndless.cfg.tns as "ext.gba=<program name without
 * .tns>" and finds the program by that name, so the name gpSP was started
 * as is registered. An association naming another program is left alone
 * (Ndless's rule); one naming a gpSP like this one (a name starting with
 * gpsp_ or _gpsp, such as gpsp_qol, this build's name before _gpSP) is
 * pointed at this one, so renaming the program keeps ROMs opening. */
static const char *const rom_types[] = { "gba", "gbz" };
#define NUM_ROM_TYPES (sizeof(rom_types) / sizeof(rom_types[0]))

static int gpsp_program_name(const char *value, size_t len)
{
  return len >= 5 && (!strncasecmp(value, "gpsp_", 5) || !strncasecmp(value, "_gpsp", 5));
}

/* The file is short: read it whole, change the lines, write it back. */
static void retarget_rom_types(const char *name)
{
  static char text[8192], out[8192 + 256];
  char path[600];
  size_t len, out_len = 0, name_len = strlen(name), i;
  int changed = 0;
  char *line;
  FILE *f;

  snprintf(path, sizeof(path), "%sndless/ndless.cfg.tns", get_documents_dir());
  f = fopen(path, "rb");
  if (!f)
    return;
  len = fread(text, 1, sizeof(text) - 1, f);
  if (!feof(f))   /* longer than any ndless.cfg.tns: leave it */
  {
    fclose(f);
    return;
  }
  fclose(f);
  text[len] = 0;

  for (line = text; *line; )
  {
    char *end = strchr(line, '\n');
    size_t line_len = end ? (size_t)(end - line + 1) : strlen(line);
    const char *value = NULL;
    size_t value_len;

    for (i = 0; i < NUM_ROM_TYPES; i++)
    {
      char key[16];
      snprintf(key, sizeof(key), "ext.%s=", rom_types[i]);
      if (!strncmp(line, key, strlen(key)))
        value = line + strlen(key);
    }
    value_len = value ? strcspn(value, "\r\n") : 0;
    if (value && gpsp_program_name(value, value_len) &&
        !(value_len == name_len && !strncmp(value, name, name_len)) &&
        out_len + (size_t)(value - line) + name_len + 1 < sizeof(out))
    {
      memcpy(out + out_len, line, value - line);
      out_len += value - line;
      memcpy(out + out_len, name, name_len);
      out_len += name_len;
      out[out_len++] = '\n';
      changed = 1;
    }
    else if (out_len + line_len < sizeof(out))
    {
      memcpy(out + out_len, line, line_len);
      out_len += line_len;
    }
    line += line_len;
  }

  if (changed && (f = fopen(path, "wb")) != NULL)
  {
    fwrite(out, 1, out_len, f);
    fclose(f);
  }
}

static void register_rom_types(const char *exe_path)
{
  const char *slash = strrchr(exe_path, '/');
  char name[64];
  size_t len, i;

  snprintf(name, sizeof(name), "%s", slash ? slash + 1 : exe_path);
  len = strlen(name);
  if (len > 4 && !strcasecmp(name + len - 4, ".tns"))
    name[len - 4] = 0;
  if (!name[0])
    return;
  retarget_rom_types(name);
  /* The types that have no program yet. */
  for (i = 0; i < NUM_ROM_TYPES; i++)
    cfg_register_fileext(rom_types[i], name);
}

int main(int argc, char *argv[])
{
  u8 load_filename[512];
  int started;

  /* Ndless: is_classic is (hwtype() < 1) from os.h — skip on classic models. */
  if (is_classic)
    return 0;

  capture_initial_sp();
  /* R6: the OS's keypad interrupt is gpSP's until it exits; EXIT-1: the
   * hand-back at exit, last (nspire_exit.h). */
  nspire_exit_start();
  nspire_os_note_start();   /* R4: a CX II's power controller words as gpSP starts */
  /* fast17: the screen and the timer first, so the start's steps are timed
   * from here (none of the files below needs them otherwise) */
  nspire_init();
  start_begin();

  ChangeWorkingDirectory(argv[0]);
  getcwd(main_path, sizeof(main_path));
  /* The saves folder, made on the first start (also on a new calculator,
   * wherever gpSP is), and what earlier builds left in My Documents
   * (gpSP_saves, zgpSP_saves), next to gpSP and the BIOS moved in, before
   * the settings are read
   * (nspire_paths.h). */
  snprintf(nspire_program_path, sizeof(nspire_program_path), "%s", argv[0]);
  nspire_save_dir_make();
  /* (r19-usb) the last run's exit trail, if it never reached its hand-back */
  nspire_exit_trail_check();
  nspire_move_old_documents_files();
  nspire_move_old_files(main_path, 1);
  nspire_tidy_save_folder();
  start_step("saves folder, old files moved");
  nspire_file_list_start();
  start_step("saves folder's names read");
  keys_set_defaults(key_bindings);
  /* Settings found elsewhere (the saves folder's copy, an older build's
   * file) go into the program file now, not at the next save. */
  if (load_config_file() == 0 && !nspire_config_in_program())
    save_config_file();
#if defined(NSPIRE_LIBRETRO)
  nspire_sync_bios_menu_from_disk();
#endif
  start_step("settings read");

  gamepak_filename[0] = 0;

  nspire_brightness_init();
  nspire_brightness_apply();
  /* R3: a freeze put the speed to normal and took the marker away: saved
   * now (word 61 = 1000), or a reset before the next save would read auto
   * again and raise a CX once more. */
  if (nspire_clock_check_crash())
    save_config_file();
  /* On-chip memory (nspire_sram.c): a run with it on that didn't end turns
   * it off, saved now as the clock's crash is; else on if it is set. */
  if (nspire_sram_start())
    save_config_file();
  start_step("brightness, clock and SRAM markers");
  register_rom_types(argv[0]);
  start_step("file types registered");
  core_bootstrap();
  start_step("ROM buffer, sound");
  init_video();
  video_resolution_large();
  start_step("video");

  nspire_apply_bios();
  start_step("BIOS");

  init_input();
  start_step("keys, pads, Bluetooth, drive, sound");
  start_steps_log(0);

  /* A game that doesn't load says why, then the file list comes up again
   * (leaving it opens the menu, without a game). */
  started = argc > 1 && nspire_start_game(argv[1]);
  if (!started)
    switch_to_romdir();
  while (!started)
  {
    if (load_file(file_ext, load_filename) == -1)
    {
      /* r19: the list is two tabs of the top row: left without a game picked (the arrows went to another
       * tab, or esc), the same row goes on in the menu (gui.c list_left: Settings, or Quit) */
      menu(copy_screen());
      break;
    }
    started = nspire_start_game((char *)load_filename);
  }
  if (started)
  {
    set_gba_resolution((video_scale_type)screen_scale);
    video_resolution_small();
  }

  nspire_clock_game();
  emulation_loop();
  return 0;
}
