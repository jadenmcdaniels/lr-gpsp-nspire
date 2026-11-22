/*
 * gpSP's load and save paths, for the library benchmark (tests/libbench.sh):
 * the frontend's own code (nspire_states.c, nspire_backup.c,
 * nspire_rom_convert.c, nspire_config.c, nspire_paths.c, nspire_zip.c,
 * nspire_gbz.c, the core's load_gamepak) built with the calculator's
 * flags and linked with the SDK's libz.a and the toolchain's newlib, over
 * libbench_os.c (the SDK's libsyscalls, and a stand-in for the OS's files
 * and heap). tests/libbench.py runs it under qemu-arm with the ARM926 model
 * (tools/calc_speed/arm926.c), which counts the cycles between
 * perf_frame_mark calls: each step of a path is one "frame".
 *
 *   libbench PATH ARGS...
 *
 * PATH (files are relative to the run folder libbench.py makes; the saves
 * folder is documents/zgpSP_files):
 *   rom ROM MB             load_gamepak with an MB ROM buffer (plain, .zip,
 *                          .gbz: what load_gamepak makes of it)
 *   state-save ROM BIOS STATE   the state loaded and two frames played (for
 *                          the picture), then states_save over slot 0
 *                          (which has the state): copy, pack, picture,
 *                          write, replace (its own timing parts)
 *   state-load ROM BIOS    states_load of slot 0: read, unpack, apply
 *   sram-save ROM BIOS STATE gz|plain [twice]   the game's save (the one in
 *                          the saves folder, loaded first), changed (marked
 *                          as the game's writes to it mark it), written as
 *                          gpSP does when a game is left
 *                          (update_backup_force): gzipped (Compress saves
 *                          on) or plain; twice: then left again with no
 *                          save in between (quit() after leaving)
 *   sram-load ROM          the save read at a game's start (the .sav.gz or
 *                          the .sav, whichever is there)
 *   rom-zip ROM            the ROM manager's C: the ROM zipped (level 6),
 *                          checked, renamed, the old one removed
 *   rom-unzip ROM          its U: a zip to a .gba, checked, renamed
 *   config ROM             the settings: read at start (from the program
 *                          file), written (copy + program file), a game's
 *                          own (settings and keys) read and written
 *   launch                 gpSP's start, up to the file list: the saves
 *                          folder, older files moved in, the settings
 *   start ROM BIOS         what a game's start does besides its ROM, save
 *                          and state: old files moved, its settings, the
 *                          BIOS, its states looked up
 *   list DIR               the file list: the folder read as gui.c does,
 *                          sorted, the game order, the picked game's
 *                          details (its states and newest picture)
 *   quit ROM BIOS STATE    Exit gpSP after a game (save on exit's state
 *                          save is state-save): the game's save written,
 *                          its settings closed, the settings saved, the
 *                          timings file, the last folder, the save again
 *   menu-list DIR ROM...   the file list as gui.c reads it: the folder, the
 *                          picked game's (the first ROM) newest state and
 *                          picture, the others'; then all again (the list
 *                          opened a second time)
 *   menu-paused ROM OTHER1 OTHER2   the paused menu in ROM: Load's strip, a
 *                          slot further, the menu opened again, Save's
 *                          strip, Resume's other games (OTHER1, OTHER2,
 *                          OTHER1 again), the save manager and two pictures
 *   menu-start ROM         a game's start: the loading screen's picture,
 *                          the game's states (as nspire_start_game)
 * After the last step it prints what it made, to check it worked, and the
 * stdio and OS calls of each step (libbench_os.c), through the OS
 * stand-in, which the model leaves out.
 */

#include "common.h"
#include "nspire.h"
#include "nspire_frontend.h"
#include "nspire_game_list.h"
#include "nspire_keys.h"
#include "nspire_link.h"
#include "nspire_gui_video.h"
#include "nspire_paths.h"
#include "nspire_rom_convert.h"
#include "nspire_states.h"
#include "nspire_os.h"
#include "nspire_gbz.h"
#include "nspire_ui.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <zlib.h>

extern u32 nspire_rom_buffer_size_choice, nspire_compress_saves;
extern u32 execute_cycles, resolution_width, resolution_height;
extern u8 *gamepak_buffers[];
extern u16 *gba_screen_pixels;
extern int os_count_step;
void os_count_print(void);
int os_printf(const char *format, ...);
void os_exit(int code) __attribute__((noreturn));
void init_gamepak_buffer(void);
void nspire_video_frame_start(void);
void nspire_load_cartridge_backup(void);
void update_backup_force(void);
s32 nspire_config_load(void);
s32 nspire_config_save(void);
void nspire_config_open_game(const char *rom_path);
void nspire_config_close_game(void);
void nspire_config_settings_for_changed(void);
void nspire_config_keys_for_changed(void);
extern u32 nspire_settings_for, nspire_keys_for;

/* The model counts the cycles between calls. */
void __attribute__((noinline)) perf_frame_mark(void)
{
  __asm__ volatile("" ::: "memory");
}

static int marks_on;

static void step(void)
{
  os_count_step++;
  perf_frame_mark();
}

u32 nspire_os_long_sleeps;   /* (nspire_pictures.c: the OS's long waits, none here) */

/* nspire_states.c times its parts with it: each part is a step here. */
u32 nspire_ticks(void)
{
  if (marks_on)
    step();
  return 0;
}

/* ---- What the frontend needs from the parts of gpSP not linked here ---- */

const char *get_documents_dir(void)
{
  return "documents/";
}

void nspire_jit_cache_sync(void *base, void *end)
{
  (void)base;
  (void)end;
}

/* This benchmark runs with the link disconnected. Keep current frontend
 * diagnostics callable without charging a transport or logging sink. */
int host_is_cx2 = 1;
uint32_t nspire_link_speed;
enum link_state nspire_link_state(void) { return LINK_OFF; }
void nspire_link_log(const char *fmt, ...) { (void)fmt; }
void nspire_link_log_toast(const char *text) { (void)text; }
int nspire_frameskip_before_flip(void) { return 1; }
void nspire_os_watch_line(void) {}   /* (nspire_frameskip.c: a pad's OS tick; nothing is watched here) */
static const uint32_t os_watch_never;   /* (nspire_lines.c's inline check: never near) */
struct nspire_os_watch nspire_os_watch_at = { &os_watch_never, 0 };
void nspire_fps_overlay_reset(void) {}
u32 nspire_render_ticks;
int nspire_perf_on;
unsigned nspire_sram_code;   /* nspire_sram.c: the On-chip memory setting (final/sram) */
void update_at_vblank(void) {}
void cfg_register_fileext(const char *ext, const char *prgm)
{
  (void)ext;
  (void)prgm;
}

static u32 old_key;

bool input_check_savestate(const u8 *src)
{
  const u8 *p = bson_find_key(src, "input");
  return (p && bson_contains_key(p, "prevkey", BSON_TYPE_INT32));
}

bool input_read_savestate(const u8 *src)
{
  const u8 *p = bson_find_key(src, "input");
  if (p)
    return bson_read_int32(p, "prevkey", &old_key);
  return false;
}

unsigned input_write_savestate(u8 *dst)
{
  u8 *wbptr1, *startp = dst;
  bson_start_document(dst, "input", wbptr1);
  bson_write_int32(dst, "prevkey", old_key);
  bson_finish_document(dst, wbptr1);
  return (unsigned int)(dst - startp);
}

/* The screen buffers, as perf_stubs.c's. */
#define SCREEN_PIXELS (320 * 240)
static u16 screens[3][SCREEN_PIXELS] __attribute__((aligned(32)));
static int drawing_screen = 1;
void *nspire_screen = screens[1];
void *nspire_displayed_screen = screens[0];
u16 nspire_landscape[SCREEN_PIXELS] __attribute__((aligned(32)));
int nspire_lcd_portrait = 1;

void nspire_flip(void)
{
  nspire_displayed_screen = nspire_screen;
  drawing_screen = (drawing_screen + 1) % 3;
  nspire_screen = screens[drawing_screen];
}

int nspire_screen_index(void)
{
  return drawing_screen;
}

/* Frontend globals nspire_config.c works on (nspire_video_ui.c,
 * nspire_host_main.c, nspire_input.c, nspire_clock.c on the calculator),
 * as tests/host/host_stubs.c has them. */
u32 update_backup_flag = 1;
u32 current_frameskip_type;
u32 frameskip_value = 4;
u32 random_skip;
u32 global_enable_analog;
u32 analog_sensitivity_level = 4;
char nspire_clock_crashed_rom[512];
u32 nspire_brightness;
uint32_t nspire_link_device;

/* ---- Setting a game up (not measured) ---- */

static int start_core(const char *rom, const char *bios, unsigned mb)
{
  nspire_rom_buffer_size_choice = mb;
  init_gamepak_buffer();
  init_sound();
  gba_screen_pixels = (u16 *)calloc(1, GBA_SCREEN_BUFFER_SIZE);
  if (bios && load_bios((char *)bios) != 0)
  {
    os_printf("can't read the BIOS %s\n", bios);
    return 0;
  }
  if (load_gamepak(NULL, rom, FEAT_AUTODETECT, FEAT_AUTODETECT, SERIAL_MODE_AUTO) != 0)
  {
    os_printf("can't load %s\n", rom);
    return 0;
  }
  reset_gba();
  return 1;
}

/* The state, then two frames drawn (a state's picture is the screen). */
static int play_state(const char *state)
{
  int frame;

  if (states_load_file(state) != 1)
  {
    os_printf("can't load the state %s\n", state);
    return 0;
  }
  resolution_width = 240;
  resolution_height = 160;
  for (frame = 0; frame < 2; frame++)
  {
    skip_next_frame = 0;
    nspire_video_frame_start();
    write_ioreg(REG_P1, 0x3FF);
    execute_arm_translate(execute_cycles);
  }
  return 1;
}

static u32 crc_of(const void *p, size_t n)
{
  return (u32)crc32(0, (const Bytef *)p, (uInt)n);
}

static long file_size(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

/* ---- The file list (gui.c's load_file reads a folder this way) ---- */

static const char *const wildcards[] = { ".gba.tns", ".gbz.tns", ".zip.tns", NULL };

static int list_folder(const char *dir_name, int details)
{
  static u16 picture[STATE_PIC_W * STATE_PIC_H];
  char **file_list, **dir_list, path[600];
  u32 num_files = 0, num_dirs = 0, files_room = 32, dirs_room = 32, i;
  struct stat file_info;
  struct dirent *current_file;
  DIR *current_dir;
  int states, newest;

  file_list = (char **)malloc(sizeof(char *) * 32);
  dir_list = (char **)malloc(sizeof(char *) * 32);
  memset(file_list, 0, sizeof(char *) * 32);
  memset(dir_list, 0, sizeof(char *) * 32);
  current_dir = opendir(dir_name);
  do
  {
    current_file = current_dir ? readdir(current_dir) : NULL;
    if (current_file)
    {
      const char *file_name = current_file->d_name;
      size_t file_name_length = strlen(file_name), ext_pos;

      /* (gui.c: a ".tns" is a file, no stat) */
      int known_file = file_name_length > 4 && !strcasecmp(file_name + file_name_length - 4, ".tns");
      snprintf(path, sizeof(path), "%s/%s", dir_name, file_name);
      if ((known_file || stat(path, &file_info) >= 0) && (file_name[0] != '.' || file_name[1] == '.'))
      {
        if (!known_file && S_ISDIR(file_info.st_mode))
        {
          dir_list[num_dirs] = (char *)malloc(file_name_length + 1);
          sprintf(dir_list[num_dirs], "%s", file_name);
          num_dirs++;
        }
        else if (file_name_length >= 8)
        {
          ext_pos = file_name[file_name_length - 8] == '.' ? file_name_length - 8 : 0;
          for (i = 0; wildcards[i] != NULL; i++)
            if (!strcasecmp(file_name + ext_pos, wildcards[i]))
            {
              file_list[num_files] = (char *)malloc(file_name_length + 1);
              sprintf(file_list[num_files], "%s", file_name);
              num_files++;
              break;
            }
        }
      }
      if (num_files == files_room)
      {
        file_list = (char **)realloc(file_list, sizeof(char *) * files_room * 2);
        memset(file_list + files_room, 0, sizeof(char *) * files_room);
        files_room *= 2;
      }
      if (num_dirs == dirs_room)
      {
        dir_list = (char **)realloc(dir_list, sizeof(char *) * dirs_room * 2);
        memset(dir_list + dirs_room, 0, sizeof(char *) * dirs_room);
        dirs_room *= 2;
      }
    }
  } while (current_file);
  if (current_dir)
    closedir(current_dir);
  game_list_sort(file_list, num_files);
  game_list_sort(dir_list, num_dirs);
  game_list_apply_order(file_list, num_files, dir_name);
  os_printf("list: %u games, %u folders\n", (unsigned)num_files, (unsigned)num_dirs);

  /* The picked game's details (gui.c's list_look_up), for each game in
   * turn as the cursor goes down the list: a step each. */
  for (i = 0; details && i < num_files; i++)
  {
    int has_picture;
    step();
    snprintf(path, sizeof(path), "%s/%s", dir_name, file_list[i]);
    has_picture = states_peek(path, &states, &newest, picture);
    if (has_picture)
      ui_scale(screens[0], 120, 80, picture, STATE_PIC_W, STATE_PIC_H, STATE_PIC_W);
    os_printf("details: %s: %d states, newest %d, picture %d, %ld bytes\n", file_list[i], states, newest,
           has_picture, file_size(path));
  }
  for (i = 0; i < num_files; i++)
    free(file_list[i]);
  for (i = 0; i < num_dirs; i++)
    free(dir_list[i]);
  free(file_list);
  free(dir_list);
  return 1;
}

/* ---- The menus' look-ups (gui.c's list and paused menu, the save
 * manager, the loading screen), the calls they make in the order they make
 * them; a step each for what one key (or opening a screen) costs. ---- */

static u16 menu_pic[STATE_PIC_W * STATE_PIC_H];

/* gui.c's list_look_up and the paused menu's other game: the game's newest
 * state, its picture and when */
static void menu_peek(const char *rom, int list)
{
  int count = 0, newest = -1, has;
  long when = 0;
  (void)list;   /* (list_look_up stats the ROM no more) */
  has = states_peek_when(rom, &count, &newest, menu_pic, &when);
  os_printf("peek %s: %d states, newest %d, picture %d\n", rom, count, newest, has);
}

/* gui.c's slot_pic: a slot of the strip */
static void menu_slot(int slot)
{
  if (states_exists(slot))
    os_printf("slot %d: picture %d\n", slot, states_picture(slot, menu_pic));
}

/* nspire_save_manager.c's refresh */
static int manager_slots[STATE_SLOTS + 1];

static int menu_manager_refresh(void)
{
  long bytes;
  int compressed, slot, n = 0;
  if (nspire_backup_info(&bytes, &compressed))
    manager_slots[n++] = -1;
  for (slot = 0; slot < STATE_SLOTS; slot++)
    if (states_file_info(slot, &bytes, &compressed))
      manager_slots[n++] = slot;
  return n;
}

static void menu_manager_picture(int slot)
{
  if (slot >= 0 && states_picture(slot, menu_pic))
    ui_scale(screens[0], 120, 80, menu_pic, STATE_PIC_W, STATE_PIC_H, STATE_PIC_W);
  states_time(slot);
}

static void usage(void)
{
  os_printf("usage: libbench PATH ARGS... (see libbench.c)\n");
  os_exit(2);
}

int main(int argc, char **argv)
{
  const char *path = argc > 1 ? argv[1] : "";
  char name[600], other[600];
  int ok = 1;

  nspire_compress_saves = 1;
  snprintf(nspire_program_path, sizeof(nspire_program_path), "documents/gba/_gpSP.tns");
  snprintf(main_path, sizeof(main_path), "documents/gba");
  if (!strcmp(path, "rom") && argc == 4)
  {
    nspire_rom_buffer_size_choice = (u32)atoi(argv[3]);
    init_gamepak_buffer();
    step();
    gbz_load_ahead = 256 * 1024;   /* (as nspire_start_game loads a game: fast17) */
    ok = load_gamepak(NULL, argv[2], FEAT_AUTODETECT, FEAT_AUTODETECT, SERIAL_MODE_AUTO) == 0;
    gbz_load_ahead_end();
    step();
    os_printf("rom: %u bytes, crc %08x, %s\n", (unsigned)gamepak_size,
           (unsigned)crc_of(gamepak_buffers[0], 1024 * 1024), gamepak_title);
  }
  else if (!strcmp(path, "state-save") && argc == 5)
  {
    ok = start_core(argv[2], argv[3], 16) && play_state(argv[4]);
    states_open_game(argv[2]);
    ok = ok && states_exists(0);
    marks_on = 1;
    ok = ok && states_save(0);
    marks_on = 0;
    states_file_name(0, name, sizeof(name));
    os_printf("state-save: %ld bytes\n", file_size(name));
  }
  else if (!strcmp(path, "state-load") && argc == 4)
  {
    ok = start_core(argv[2], argv[3], 16);
    states_open_game(argv[2]);
    marks_on = 1;
    ok = ok && states_load(0) == 1;
    marks_on = 0;
    os_printf("state-load: pc %08x\n", (unsigned)reg[REG_PC]);
  }
  else if (!strcmp(path, "sram-save") && (argc == 6 || (argc == 7 && !strcmp(argv[6], "twice"))))
  {
    /* the game's save (a real one, when libbench.py has it) loaded as a
     * game's start does, then the state played and the save changed */
    ok = start_core(argv[2], argv[3], 16);
    nspire_load_cartridge_backup();
    ok = ok && play_state(argv[4]);
    nspire_compress_saves = !strcmp(argv[5], "gz");
    /* (Through a pointer: inlined here, it took nspire_backup.c's variable
     * into this file's part of the link, which moved the buffers after it
     * and so the other paths' times.) */
    {
      void (*volatile mark)(void) = nspire_backup_mark_dirty;
      mark();
    }
    step();
    update_backup_force();
    step();
    if (argc == 7)
    {
      update_backup_force();
      step();
    }
    nspire_game_base(argv[2], name, sizeof(name));
    snprintf(other, sizeof(other), nspire_compress_saves ? "%s.sav.gz.tns" : "%s.sav.tns", name);
    os_printf("sram-save: type %d, %ld bytes, crc %08x\n", (int)backup_type, file_size(other),
              (unsigned)crc_of(gamepak_backup, 0x20000));
  }
  else if (!strcmp(path, "sram-load") && argc == 3)
  {
    ok = start_core(argv[2], NULL, 16);
    step();
    nspire_load_cartridge_backup();
    step();
    os_printf("sram-load: type %d, crc %08x\n", (int)backup_type, (unsigned)crc_of(gamepak_backup, 0x20000));
  }
  else if ((!strcmp(path, "rom-zip") || !strcmp(path, "rom-unzip")) && argc == 3)
  {
    const char *why;
    step();
    why = path[4] == 'z' ? rom_compress(argv[2], name, sizeof(name), NULL)
                         : rom_uncompress(argv[2], name, sizeof(name), NULL);
    step();
    ok = why == NULL;
    os_printf("%s: %s -> %s, %ld bytes\n", path, argv[2], why ? why : name, file_size(name));
  }
  else if (!strcmp(path, "config") && argc == 3)
  {
    /* As after a first run: the settings in the program file, the copy in
     * the saves folder, the game's own settings and keys in its file. */
    keys_set_defaults(key_bindings);
    nspire_config_save();
    nspire_config_open_game(argv[2]);
    nspire_settings_for = nspire_keys_for = 1;
    nspire_config_settings_for_changed();
    nspire_config_keys_for_changed();
    nspire_config_close_game();
    step();
    ok = nspire_config_load() == 0;
    step();
    ok = ok && nspire_config_save() == 0;
    step();
    nspire_config_open_game(argv[2]);
    step();
    nspire_config_close_game();
    step();
    nspire_game_base(argv[2], name, sizeof(name));
    snprintf(other, sizeof(other), "%s.settings.tns", name);
    os_printf("config: program file %ld bytes, game file %ld bytes\n", file_size(nspire_program_path),
              file_size(other));
  }
  else if (!strcmp(path, "launch") && argc == 2)
  {
    /* nspire_host_main.c's main, up to the file list: the saves folder,
     * older builds' files moved in, the file list's start, the settings. */
    step();
    nspire_save_dir_make();
    nspire_move_old_documents_files();
    step();
    nspire_move_old_files(main_path, 1);
    step();
    nspire_tidy_save_folder();
    step();
    nspire_file_list_start();
    step();
    keys_set_defaults(key_bindings);
    ok = nspire_config_load() == 0;
    step();
  }
  else if (!strcmp(path, "start") && argc == 4)
  {
    int newest;
    nspire_rom_buffer_size_choice = 16;
    init_gamepak_buffer();
    step();
    nspire_game_move_old_files(argv[2]);
    step();
    nspire_config_open_game(argv[2]);
    step();
    ok = load_bios(argv[3]) == 0;
    step();
    states_open_game(argv[2]);
    newest = states_newest();
    step();
    os_printf("start: %d states, newest %d\n", states_count(), newest);
  }
  else if (!strcmp(path, "list") && argc == 3)
  {
    step();
    ok = list_folder(argv[2], 1);
    step();
  }
  else if (!strcmp(path, "menu-list") && argc >= 4)
  {
    /* The list opened (its folder read), the picked game's details, then
     * the others' pictures one by one (gui.c's list_draw); the list left
     * and opened again, the same again. */
    int pass, i;
    for (pass = 0; pass < 2; pass++)
    {
      step();
      ok = ok && list_folder(argv[2], 0);
      step();
      menu_peek(argv[3], 1);
      step();
      for (i = 4; i < argc; i++)
        menu_peek(argv[i], 0);
    }
    step();
  }
  else if (!strcmp(path, "menu-paused") && argc == 5)
  {
    /* In the paused menu of ROM (its states looked up at its start, not
     * counted): Load's strip (the newest and its neighbours), a slot up, the
     * menu opened again, Save's strip; Resume's other games (OTHER1, OTHER2,
     * OTHER1 again); the save manager opened, its first state's picture, the
     * next one's. */
    int newest, n;
    states_open_game(argv[2]);
    newest = states_newest();
    step();
    menu_slot(newest - 1);
    menu_slot(newest);
    menu_slot(newest + 1);
    step();
    menu_slot(newest - 2);
    step();
    menu_slot(newest - 2);
    menu_slot(newest - 1);
    menu_slot(newest);
    step();
    menu_slot(newest);
    step();
    menu_peek(argv[3], 0);
    step();
    menu_peek(argv[4], 0);
    step();
    menu_peek(argv[3], 0);
    step();
    n = menu_manager_refresh();
    step();
    menu_manager_picture(n > 1 ? manager_slots[1] : -1);
    step();
    menu_manager_picture(n > 2 ? manager_slots[2] : -1);
    step();
    os_printf("menu-paused: newest %d, manager %d files\n", newest, n);
  }
  else if (!strcmp(path, "menu-start") && argc == 3)
  {
    /* A game's start (nspire_start_game): the loading screen's picture
     * (the newest state's), then the game's states looked up. */
    int count, newest;
    step();
    states_peek(argv[2], &count, &newest, menu_pic);
    step();
    states_open_game_kept(argv[2]);   /* (nspire_start_game) */
    newest = states_newest();
    step();
    os_printf("menu-start: %d states, newest %d\n", count, newest);
  }
  else if (!strcmp(path, "quit") && argc == 5)
  {
    /* nspire_quit_game (Exit gpSP, Save+Exit) after a game was played and
     * a state loaded twice, save on exit off (the state save is
     * state-save's): leaving the game (its save written, its settings
     * closed), the settings saved, then quit(): the timings file, the
     * last folder, the game's save written again. */
    FILE *f;
    keys_set_defaults(key_bindings);
    nspire_config_save();
    ok = start_core(argv[2], argv[3], 16);
    nspire_load_cartridge_backup();
    ok = ok && play_state(argv[4]);
    states_open_game(argv[2]);
    ok = ok && states_load(0) == 1 && states_load(0) == 1;
    nspire_config_open_game(argv[2]);
    step();
    update_backup_force();
    step();
    nspire_config_close_game();
    step();
    nspire_config_save();
    step();
    states_timing_flush();
    step();
    /* nspire_host_main.c's save_romdir */
    nspire_save_file(name, sizeof(name), "gpsp_romdir.tns");
    f = fopen(name, "wb");
    if (f)
    {
      other[0] = 0;
      getcwd(other, sizeof(other));
      if (other[0])
        fwrite(other, 1, strlen(other), f);
      fclose(f);
    }
    step();
    update_backup_force();
    step();
  }
  else
    usage();

  os_count_step = 0;
  os_count_print();
  os_printf("%s: %s\n", path, ok ? "ok" : "FAILED");
  os_exit(ok ? 0 : 1);
}
