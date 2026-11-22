/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public Licens e as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef _WIN32_WCE

#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#include <time.h>

#endif

#if !defined(GP2X_BUILD) && !defined(NSPIRE_BUILD)
#include "gp2x/cpuctrl.h"
#endif

#if defined(NSPIRE_LIBRETRO)
/* Root common.h pulls memory.h and collides with libretro gba_memory.h (MEMORY_H). */
#include "libretro-gpsp/common.h"
#else
#include "common.h"
#endif
#include "gui.h"

#if defined(NSPIRE_LIBRETRO)
#include "gba_memory.h"
#include "serial.h"
#include "nspire_frameskip.h"
#include "savestate.h"
#include "nspire_frontend.h"
#include "nspire_keys.h"
#include "nspire_states.h"
#include "nspire_clock.h"
#include "nspire_sram.h"
#include "nspire_brightness.h"
#include "nspire_game_list.h"
#include "nspire_ui_screens.h"
#include "nspire_dialog.h"
#include "nspire_link.h"
#include "nspire_usbpad_glue.h"
#include "nspire_drive.h"
#include "nspire_audio.h"
#include "nspire_bt_glue.h"
#include "nspire_cheats.h"
#include "nspire_places.h"
#include "nspire_bluetooth.h"
#include "nspire.h"

#if defined(NSPIRE_LIBRETRO)
/* Advanced > Link speed: nspire_link_speed.c owns it (the fast link); this weak one is for builds without that file. */
__attribute__((weak)) uint32_t nspire_link_speed = 0;
#endif
#endif

#if defined(NSPIRE_LIBRETRO)
/* Top byte of word [26] global / [15] per-game: new frameskip encoding (auto/manual/off). */
#define GPSP_CFG_FS_V2_MARK 0xFF000000u
#define GPSP_GAME_FS_V2_MARK 0xFF000000u

static void nspire_clamp_rom_buffer_size(void)
{
  if (nspire_rom_buffer_size_choice < 2)
    nspire_rom_buffer_size_choice = 2;
  else if (nspire_rom_buffer_size_choice > 32)
    nspire_rom_buffer_size_choice = 32;
}

/* save17: what a failed save or load says, by its true cause (states_failure; the link log has its step,
 * errno and sizes). r16 said "The calculator is full" to every failed save. */
static const char *state_failure_text(int saving)
{
  static char text[160];
  const struct states_failure *f = states_failure(saving ? STATES_SAVING : STATES_LOADING);
  switch (f->why)
  {
  case STATES_WHY_MEMORY:
    return saving ? "There isn't enough free memory for it now. Close open documents, or quit gpSP and start it "
                    "again, then try again." :
                    "There isn't enough free memory to load it now. Quit gpSP and start it again, then load it.";
  case STATES_WHY_FULL:
    if (f->free < 0)
      return "The calculator is full. Delete a file or a game, then try again.";
    snprintf(text, sizeof(text), "The calculator is full: %ld" "\xa0" "KB free, it needs %lu" "\xa0" "KB. Delete a file "
             "or a game, then try again.", f->free / 1024, (f->need + 1023) / 1024);
    return text;
  case STATES_WHY_FOREIGN:
    return "It isn't a state this gpSP can read (another version's?). Pick another slot. The game goes on as it was.";
  case STATES_WHY_FILE:
    snprintf(text, sizeof(text), "The file %s failed. %s", f->step ? f->step : "write",
             saving ? "Try again, or pick another slot." : "Pick another slot. The game goes on as it was.");
    return text;
  default:
    return saving ? "Try again, or pick another slot." : "Pick another slot. The game goes on as it was.";
  }
}
#endif

#define MAX_PATH 1024

// Blatantly stolen and trimmed from MZX (megazeux.sourceforge.net)

#define FILE_LIST_ROWS UI_LIST_ROWS

#ifdef PSP_BUILD
  u8 *clock_speed_options[] =
  {
    "33MHz", "66MHz", "100MHz", "133MHz", "166MHz", "200MHz", "233MHz",
    "266MHz", "300MHz", "333MHz"
  };
  #define menu_get_clock_speed() \
    clock_speed = (clock_speed_number + 1) * 33
  #define get_clock_speed_number() \
    clock_speed_number = (clock_speed / 33) - 1
#elif defined(WIZ_BUILD)
  u8 *clock_speed_options[] =
  {
    "300MHz", "333MHz", "366MHz", "400MHz", "433MHz",
    "466MHz", "500MHz", "533MHz", "566MHz", "600MHz",
    "633MHz", "666MHz", "700MHz", "733MHz", "766MHz",
    "800MHz", "833MHz", "866MHz", "900MHz"
  };
  #define menu_get_clock_speed() \
    clock_speed = 300 + (clock_speed_number * 3333) / 100
  #define get_clock_speed_number() \
    clock_speed_number = (clock_speed - 300) / 33
#elif defined(GP2X_BUILD)
  u8 *clock_speed_options[] =
  {
    "150MHz", "160MHz", "170MHz", "180MHz", "190MHz",
    "200MHz", "210MHz", "220MHz", "230MHz", "240MHz",
    "250MHz", "260MHz", "270MHz", "280MHz", "290MHz"
  };
  #define menu_get_clock_speed() \
    clock_speed = 150 + clock_speed_number * 10
  #define get_clock_speed_number() \
    clock_speed_number = (clock_speed - 150) / 10
#elif defined(NSPIRE_BUILD)
  u8 *clock_speed_options[] =
  {
    "120MHz", "126MHz", "132MHz", "138MHz", "144MHz",
	"150MHz", "156MHz", "162MHz", "168MHz", "174MHz",
	"180MHz", "186MHz", "192MHz", "198MHz", "204MHz",
	"210MHz", "216MHz", "222MHz", "228MHz", "234MHz",
	"240MHz", "246MHz", "252MHz", "258MHz", "264MHz",
	"270MHz", "276MHz", "282MHz", "288MHz", "294MHz",
	"300MHz"
  };
  #define menu_get_clock_speed() \
    clock_speed = 120 + clock_speed_number * 6
  #define get_clock_speed_number() \
    clock_speed_number = (clock_speed - 120) / 6
#else
  u8 *clock_speed_options[] =
  {
    "0"
  };
  #define menu_get_clock_speed() 0
  #define get_clock_speed_number() 0
#endif


int sort_function(const void *dest_str_ptr, const void *src_str_ptr)
{
  char *dest_str = *((char **)dest_str_ptr);
  char *src_str = *((char **)src_str_ptr);

  if(src_str[0] == '.')
    return 1;

  if(dest_str[0] == '.')
    return -1;

  return strcasecmp(dest_str, src_str);
}

#if defined(NSPIRE_LIBRETRO)
/* What the game list shows beside the names is the picked game's: its
 * latest save's picture, when that was, how many saves. Looked up once the
 * list has rested on a game for a sixth of a second (holding an arrow
 * doesn't wait for files); the names themselves need no files. */
static struct ui_list_details list_details;
static char list_details_file[256];
static u32 list_moved_at;   /* the last time the pick or the first line changed */
static int list_redraw;     /* a folder was just opened: draw it whatever */
static int list_version;    /* changes when something looked up changes what is drawn */

/* The grid's pictures: each game on the screen's latest save's, looked up
 * one a draw once the list rests (as the picked game's details are). */
static u16 grid_pics[UI_LIST_ROWS][144 * 96];
static char grid_file[UI_LIST_MAX][256];
static s8 grid_state[UI_LIST_MAX];   /* 0: not looked up; 1: has a picture; -1: none */
static char grid_facts[UI_LIST_MAX][24];   /* "4 saves" (UI17: each tile's) */
static char grid_age[UI_LIST_MAX][12];     /* and when the last one was: "2 h" */
/* UI17: the games a line each (v), not the store's grid; kept while gpSP runs */
static int list_lines;

static void list_cache_clear(void)
{
  int r;
  for(r = 0; r < UI_LIST_MAX; r++)
    grid_file[r][0] = 0;
  list_details_file[0] = 0;
  list_details.ready = 0;
  list_version++;
}

static void list_path(char *path, size_t size, const char *dir, const char *file)
{
  size_t len = strlen(dir);
  snprintf(path, size, "%s%s%s", dir, len && dir[len - 1] == '/' ? "" : "/", file);
}

/* The picked game: its latest save's picture, "2 days ago", how many saves.
 * 0 while that isn't known: what was looked up before shows at once, files
 * are read only with read_files (and a key stops that: states_yield). */
static int list_look_up(const char *dir, const char *file, int read_files)
{
  char path[MAX_PATH + 256];
  long when = 0;
  int has;

  list_path(path, sizeof(path), dir, file);
  has = states_peek_soon(path, &list_details.states, &list_details.newest, list_details.picture, &when,
   read_files);
  if(has < 0)
    return 0;
  list_details.has_picture = has;
  list_details.bytes = 0;   /* (not shown: no stat of the ROM at each pick) */
  list_details.facts[0] = 0;
  if(!list_details.states)
    snprintf(list_details.facts, sizeof(list_details.facts), "Not played yet");
  else
    ui_when(when, (long)time(NULL), 0, list_details.facts, sizeof(list_details.facts));
  list_details.ready = 1;
  list_version++;
  return 1;
}

/* Draws the list when something on it changed. */
/* r19: the top row's words, as the games' grid and the saves' draw them: the menu's (menu() names them before it
 * opens either), or the ones there are without a game: SETTINGS in the left corner, GAMES in the middle, QUIT in
 * the right corner (with a game paused SAVES before GAMES). GAMES is word nav_games, SAVES nav_saves (-1: none). */
static char nav_text[8][16] = { "Settings", "Games", "Quit" };
static const char *nav_names[8] = { nav_text[0], nav_text[1], nav_text[2] };
static int nav_count = 3, nav_games = 1, nav_saves = -1;

static void list_draw(u8 **files, u32 num_files, const char *const *dirs, u32 num_dirs,
 const char *dir_name, s32 file_picked, s32 file_first, int moving, const char *status, int nav_focus)
{
  static struct ui_list_view last;
  static int last_ready, last_version, last_pick = -1, last_first = -1;
  static char last_status[80];
  static u32 scroll_from;     /* when the picked name started to wait for its scroll */
  static int scrolling_for = -1;
  struct ui_list_view v;
  const char *folder, *end;
  char folder_name[64];
  int r;
  u32 now = nspire_ticks();

  memset(&v, 0, sizeof(v));
  /* the folder's own name: "/documents/gba/" -> "gba" */
  end = dir_name + strlen(dir_name);
  while(end > dir_name && end[-1] == '/')
    end--;
  folder = end;
  while(folder > dir_name && folder[-1] != '/')
    folder--;
  snprintf(folder_name, sizeof(folder_name), "%.*s", (int)(end - folder), folder);
  if(!strcasecmp(folder_name, "documents") || !folder_name[0])
    snprintf(folder_name, sizeof(folder_name), "My Documents");
  v.folder = folder_name;
  v.tabs = nav_names;
  v.tab_count = nav_count;
  v.tab_on = nav_games;
  v.nav_focus = nav_focus;
  v.path = dir_name;
  v.games = (int)num_files;
  v.items = (int)(num_files + num_dirs);
  v.first = file_first;
  v.picked_row = v.marked_row = -1;
  if(file_picked != last_pick || file_first != last_first)
  {
    last_pick = file_picked;
    last_first = file_first;
    list_moved_at = now;
  }
  v.lines = list_lines;
  for(r = 0; r < (list_lines ? UI_LIST_LINES : UI_LIST_ROWS) && file_first + r < (s32)(num_files + num_dirs); r++)
  {
    const char *file;

    if(file_first + r >= (s32)num_files)
    {
      /* r19: a folder, after the games (dirs' first is the way up: shown last): its name, no picture */
      v.rows[r].file = dirs[((u32)(file_first + r) - num_files + 1) % num_dirs];
      v.rows[r].folder = 1;
      v.rows[r].looked_up = 1;
      grid_file[r][0] = 0;
      grid_state[r] = 0;
      if(file_first + r == file_picked)
      {
        if(!nav_focus)
          v.picked_row = r;
        else
          v.marked_row = r;
      }
      continue;
    }
    file = (const char *)files[file_first + r];
    v.rows[r].file = file;
    if(strcmp(grid_file[r], file))
    {
      snprintf(grid_file[r], sizeof(grid_file[r]), "%s", file);
      grid_state[r] = 0;
      grid_facts[r][0] = grid_age[r][0] = 0;
    }
    v.rows[r].picture = grid_state[r] == 1 && !list_lines ? grid_pics[r] : NULL;
    v.rows[r].looked_up = grid_state[r] != 0;
    memcpy(v.rows[r].facts, grid_facts[r], sizeof(v.rows[r].facts));
    memcpy(v.rows[r].age, grid_age[r], sizeof(v.rows[r].age));
    if(file_first + r == file_picked)
    {
      if(!nav_focus)
        v.picked_row = r;
      else
        v.marked_row = r;
    }
  }
  v.moving = moving;
  v.dir_picked = -1;
  v.status = status;
  v.details = &list_details;
  v.resume = gamepak_filename[0] != 0;
  v.has_usb = nspire_drive_enabled();

  {
    /* What was looked up before (since gpSP started) shows at once; files are read once the pick has rested a
     * sixth of a second, the picked game first, then the other tiles' pictures, one a draw. (r19: the tiles are
     * looked up whatever is picked: a folder too.) */
    int picked_game = file_picked < (s32)num_files;
    int rested = now - list_moved_at >= NSPIRE_TICK_HZ / 6, read = 0;

    if(picked_game)
    {
      const char *file = (const char *)files[file_picked];
      if(strcmp(file, list_details_file))
      {
        snprintf(list_details_file, sizeof(list_details_file), "%s", file);
        list_details.ready = 0;
      }
      else if(!moving && !list_details.ready && !list_look_up(dir_name, file, 0) && rested)
      {
        list_look_up(dir_name, file, 1);
        read = 1;
      }
    }
    /* (while a game is moved, only what is kept already: the tile it passes shows its picture, nothing is read) */
    if(moving || !picked_game || list_details.ready || read)
      for(r = 0; r < UI_LIST_MAX && v.rows[r].file; r++)
        if(!grid_state[r] && !v.rows[r].folder)
        {
          char path[MAX_PATH + 256];
          int count, newest, has;
          long when;
          /* (the lines show no pictures: one place for the one being read) */
          u16 *pic = list_lines ? grid_pics[0] : grid_pics[r];
          list_path(path, sizeof(path), dir_name, grid_file[r]);
          has = states_peek_soon(path, &count, &newest, pic, &when, 0);
          if(has < 0 && rested && !read && !moving)
          {
            has = states_peek_soon(path, &count, &newest, pic, &when, 1);
            read = 1;
          }
          if(has < 0)
            continue;
          grid_state[r] = list_lines ? -2 : has ? 1 : -1;
          /* its facts: how many saves, and when the last one was ("4 saves", "2 h") */
          grid_age[r][0] = 0;
          if(!count)
            snprintf(grid_facts[r], sizeof(grid_facts[r]), "Not played");
          else
          {
            snprintf(grid_facts[r], sizeof(grid_facts[r]), "%d save%s", count, count == 1 ? "" : "s");
            ui_when(when, (long)time(NULL), 1, grid_age[r], sizeof(grid_age[r]));
          }
          memcpy(v.rows[r].facts, grid_facts[r], sizeof(v.rows[r].facts));
          memcpy(v.rows[r].age, grid_age[r], sizeof(v.rows[r].age));
          v.rows[r].picture = grid_state[r] == 1 ? grid_pics[r] : NULL;
          v.rows[r].looked_up = 1;
          list_version++;
        }

    /* A name too long for its place scrolls (ui_line_text): counted from when the item was picked; 0 while
     * the last drawing's picked name fit (ui_list_scrolls), so a list at rest isn't drawn again. */
    if(!moving && v.picked_row >= 0)
    {
      if(scrolling_for != file_picked)
      {
        scrolling_for = file_picked;
        scroll_from = now;
      }
      v.picked_ms = ui_list_scrolls ? (unsigned)((now - scroll_from) * 30 / NSPIRE_TICK_HZ) * 1000 / 30 : 0;
    }
    else
      scrolling_for = -1;
  }

  if(!list_redraw && !memcmp(&v, &last, sizeof(v)) && last_ready == list_details.ready &&
   last_version == list_version && !strcmp(status, last_status))
    return;
  ui_list_draw(&v);
  list_redraw = 0;
  last = v;
  last_ready = list_details.ready;
  last_version = list_version;
  snprintf(last_status, sizeof(last_status), "%s", status);
}
#endif

int nspire_list_left_by_esc;
/* r19: the games' grid is the top row's GAMES (the row every top-level screen has). How it opens next: 0 in the
 * grid, its first item picked (the start); 1 with the arrows on the row, on GAMES. How it was left when it
 * returns -1: nspire_list_nav_exit -1 (left of GAMES: the word before it), 1 (right of it: the word after), or 0
 * with nspire_list_left_by_esc 1 (esc or the menu key on the row). */
int nspire_list_nav;
int nspire_list_nav_exit;

s32 load_file(u8 **wildcards, u8 *result)
{
  nspire_audio_pause();
  DIR *current_dir;
  struct dirent *current_file;
  struct stat file_info;
  u8 current_dir_name[MAX_PATH];
  u32 total_filenames_allocated;
  u32 total_dirnames_allocated;
  u8 **file_list;
  u8 **dir_list;
  u32 num_files;
  u32 num_dirs;
  u8 *file_name;
  u32 file_name_length;
  u32 ext_pos = -1;
  u32 chosen_file, chosen_dir;
  u32 dialog_result = 1;
  s32 return_value = 1;
  s32 current_file_selection;
  s32 current_file_scroll_value;
  u32 current_file_number, current_dir_number;
  u32 repeat;
  u32 i;
  int known_file = 0;
  u32 list_read_at = 0;
  gui_action_type gui_action;
#if defined(NSPIRE_LIBRETRO)
  s32 moving = -1;               /* the game being moved (tab), or -1 */
  s32 moving_from = 0;
  u8 **order_before = NULL;      /* the list before the move, for esc */
  char list_status[60] = "";     /* what the last key did, until the next */
  u8 *usb_folder = NULL;        /* identity, even if a real folder has this name */
  int nav_focus = nspire_list_nav != 0;               /* r19: the arrows are on the top row */
  int start_column = -1;
  s32 num_items = 0;                                  /* the games and, after them, the folders */
  /* The number keys don't start games (they are for moving around, not
   * for loading one):
   * here as in every menu, 8 2 5 4 6 move like the arrows
   * (get_gui_input) and the others do nothing. */
  /* a key stops the look-ups of files (they go on later) */
  states_yield = keys_down_now;
  nspire_list_nav = 0;
  nspire_list_nav_exit = 0;
  nspire_list_left_by_esc = 0;
  /* (r19: without a game the row has no SAVES, whatever the menu last named: after a start that was
   * cancelled or a game that stopped the grid still drew it) */
  if(!gamepak_filename[0])
  {
    snprintf(nav_text[0], sizeof(nav_text[0]), "Settings");
    snprintf(nav_text[1], sizeof(nav_text[1]), "Games");
    snprintf(nav_text[2], sizeof(nav_text[2]), "Quit");
    nav_names[0] = nav_text[0];
    nav_names[1] = nav_text[1];
    nav_names[2] = nav_text[2];
    nav_count = 3;
    nav_games = 1;
    nav_saves = -1;
  }
#endif

  while(return_value == 1)
  {
    current_file_selection = 0;
    current_file_scroll_value = 0;

    total_filenames_allocated = 32;
    total_dirnames_allocated = 32;
    num_files = 0;
    num_dirs = 0;
    current_dir = NULL;
#if defined(NSPIRE_LIBRETRO)
    usb_folder = NULL;
#endif
    file_list = (u8 **)calloc(32, sizeof(u8 *));
    dir_list = (u8 **)calloc(32, sizeof(u8 *));
    if(!file_list || !dir_list)
      goto list_failed;
    chosen_file = 0;
    chosen_dir = 0;

    /* Ndless returns an OS status here, while POSIX returns the buffer. */
    current_dir_name[0] = '\0';
    getcwd(current_dir_name, MAX_PATH);
    if(!current_dir_name[0])
      goto list_failed;
#if defined(NSPIRE_LIBRETRO)
    list_redraw = 1;
    list_cache_clear();   /* a new pass: look the games up again */
#endif

#if defined(NSPIRE_LIBRETRO)
    list_read_at = nspire_ticks();
#endif
    current_dir = opendir(current_dir_name);

    do
    {
	  if(current_dir)
        current_file = readdir(current_dir);
      else
        current_file = NULL;

      if(current_file)
      {
        file_name = current_file->d_name;
        file_name_length = strlen(file_name);
#if defined(NSPIRE_LIBRETRO)
        /* A ".tns" is a file (the OS's documents are): no stat for it, a
         * file system lookup each (~5 ms on the CX II, ~17 on the CX). */
        known_file = file_name_length > 4 && !strcasecmp(file_name + file_name_length - 4, ".tns");
#endif

        if((known_file || stat(file_name, &file_info) >= 0) &&
         ((file_name[0] != '.') || (file_name[1] == '.')))
        {
          if(!known_file && S_ISDIR(file_info.st_mode))
          {
            dir_list[num_dirs] =
             (u8 *)malloc(file_name_length + 1);
            if(!dir_list[num_dirs])
              goto list_failed;
            sprintf(dir_list[num_dirs], "%s", file_name);

            num_dirs++;
          }
          else
          {
			// Must match one of the wildcards, also ignore the .
#ifdef NSPIRE_BUILD
			if(file_name_length >= 8)
			{
			  if(file_name[file_name_length - 8] == '.')
			    ext_pos = file_name_length - 8;
#else
            if(file_name_length >= 4)
            {
              if(file_name[file_name_length - 4] == '.')
                ext_pos = file_name_length - 4;
              else

              if(file_name[file_name_length - 3] == '.')
                ext_pos = file_name_length - 3;
#endif
              else
                ext_pos = 0;

              for(i = 0; wildcards[i] != NULL; i++)
              {
                if(!strcasecmp((file_name + ext_pos),
                 wildcards[i]))
                {
                  file_list[num_files] =
                   (u8 *)malloc(file_name_length + 1);
                  if(!file_list[num_files])
                    goto list_failed;
                  sprintf(file_list[num_files], "%s", file_name);

                  num_files++;
                  break;
                }
              }
            }
          }
        }

        if(num_files == total_filenames_allocated)
        {
          u8 **grown;
          if(total_filenames_allocated >= 65536)
            goto list_failed;
          grown = (u8 **)realloc(file_list, sizeof(u8 *) * total_filenames_allocated * 2);
          if(!grown)
            goto list_failed;
          file_list = grown;
          memset(file_list + total_filenames_allocated, 0,
           sizeof(u8 *) * total_filenames_allocated);
          total_filenames_allocated *= 2;
        }

        if(num_dirs == total_dirnames_allocated)
        {
          u8 **grown;
          if(total_dirnames_allocated >= 65536)
            goto list_failed;
          grown = (u8 **)realloc(dir_list, sizeof(u8 *) * total_dirnames_allocated * 2);
          if(!grown)
            goto list_failed;
          dir_list = grown;
          memset(dir_list + total_dirnames_allocated, 0,
           sizeof(u8 *) * total_dirnames_allocated);
          total_dirnames_allocated *= 2;
        }
      }
    } while(current_file);
#if defined(NSPIRE_LIBRETRO)
    if(nspire_drive_enabled())
    {
      /* The scan grows at capacity, so one extra entry fits here. */
      usb_folder = (u8 *)strdup(DRIVE_LABEL);
      if(!usb_folder)
        goto list_failed;
      dir_list[num_dirs++] = usb_folder;
    }
#endif
#ifndef NSPIRE_BUILD
    qsort((void *)file_list, num_files, sizeof(u8 *), sort_function);
    qsort((void *)dir_list, num_dirs, sizeof(u8 *), sort_function);
#endif
    if(current_dir)
      closedir(current_dir);
    current_dir = NULL;
#if defined(NSPIRE_LIBRETRO)
    game_list_sort((char **)file_list, num_files);
    game_list_sort((char **)dir_list, num_dirs);
    /* the way up is always the first folder, whether the file system lists
     * ".." or not */
    if(!num_dirs || strcmp((char *)dir_list[0], ".."))
    {
      u32 d;
      for(d = 0; d < num_dirs && strcmp((char *)dir_list[d], ".."); d++)
        ;
      if(d == num_dirs)
      {
        if(num_dirs == total_dirnames_allocated)
        {
          u8 **grown;
          if(total_dirnames_allocated >= 65536)
            goto list_failed;
          grown = (u8 **)realloc(dir_list, sizeof(u8 *) * total_dirnames_allocated * 2);
          if(!grown)
            goto list_failed;
          dir_list = grown;
          memset(dir_list + total_dirnames_allocated, 0, sizeof(u8 *) * total_dirnames_allocated);
          total_dirnames_allocated *= 2;
        }
        dir_list[num_dirs] = (u8 *)malloc(3);
        if(!dir_list[num_dirs])
          goto list_failed;
        strcpy((char *)dir_list[num_dirs], "..");
        num_dirs++;
      }
      for(; d > 0; d--)
      {
        u8 *t = dir_list[d];
        dir_list[d] = dir_list[d - 1];
        dir_list[d - 1] = t;
      }
    }
    game_list_apply_order((char **)file_list, num_files, (char *)current_dir_name);
    moving = -1;
    order_before = (u8 **)malloc(sizeof(u8 *) * (num_files ? num_files : 1));
    states_menu_note("list: folder read, sorted, ordered", nspire_ticks() - list_read_at);
#endif

    repeat = 1;

    /* r19: the grid's items are the games and, after them, the folders (this folder's, the USB drive when
     * there is one, the way up last): no list or tab of folders (folders need no menu of their own: they are
     * items like the games). A folder just opened starts on its first item. */
    num_items = (s32)(num_files + num_dirs);
    (void)start_column;

  {
    while(repeat)
    {
      /* the grid's rows of three, or a line an item (v) */
      s32 cols = list_lines ? 1 : 3, page = list_lines ? UI_LIST_LINES : FILE_LIST_ROWS;
      int on_game = current_file_selection < (s32)num_files;
      /* the folder an item past the games is (dir_list's first is the way up: shown last) */
      #define ITEM_DIR(k) (((u32)(k) - num_files + 1) % num_dirs)

      update_at_vblank();
      list_draw(file_list, num_files, (const char *const *)dir_list, num_dirs,
       (char *)current_dir_name, current_file_selection, current_file_scroll_value,
       moving >= 0, list_status, nav_focus);

      {
        /* UI17: s (the picked game's saves) and v (the grid or the lines) are the list's (u when USB drive ready) */
        static const unsigned char list_keys[] = { KEY_S, KEY_V, 0 };
        static const unsigned char list_keys_usb[] = { KEY_S, KEY_V, KEY_U, 0 };
        nspire_gui_own_keys = nav_focus ? NULL : nspire_drive_enabled() ? list_keys_usb : list_keys;
        gui_action = get_gui_input();
        nspire_gui_own_keys = NULL;
      }

      {
        int tab = !nav_focus && on_game && key_pressed(KEY_TAB);
        int usb_key = !nav_focus && nspire_drive_enabled() && key_pressed(KEY_U);
        int view_key = !nav_focus && key_pressed(KEY_V), saves_key = !nav_focus && on_game && key_pressed(KEY_S);

        if(gui_action != CURSOR_NONE || tab || usb_key || view_key || saves_key)
          list_status[0] = 0;

        if(moving >= 0)
        {
          /* Arrows carry the game among the games, enter or tab puts it
           * there, esc (or menu) puts it back. */
          if(tab)
            gui_action = CURSOR_SELECT;
          switch(gui_action)
          {
            case CURSOR_LEFT:
            case CURSOR_RIGHT:
            case CURSOR_UP:
            case CURSOR_DOWN:
            {
              s32 delta = gui_action == CURSOR_LEFT ? -1 :
                          gui_action == CURSOR_RIGHT ? 1 :
                          gui_action == CURSOR_UP ? -cols : cols;
              s32 target = (s32)current_file_selection + delta;
              if(target >= 0 && target < (s32)num_files)
              {
                game_list_move((char **)file_list, num_files,
                 current_file_selection, target);
                current_file_selection = target;
                while(current_file_selection < current_file_scroll_value)
                  current_file_scroll_value -= cols;
                while(current_file_selection >= current_file_scroll_value + page)
                  current_file_scroll_value += cols;
                if(current_file_scroll_value < 0)
                  current_file_scroll_value = 0;
              }
              gui_action = CURSOR_NONE;
              break;
            }

            case CURSOR_SELECT:
              if(!game_list_save_order((char **)file_list, num_files,
               (char *)current_dir_name))
                snprintf(list_status, sizeof(list_status), "Couldn't save the order.");
              moving = -1;
              gui_action = CURSOR_NONE;
              break;

            case CURSOR_BACK:
            case CURSOR_EXIT:
              memcpy(file_list, order_before, sizeof(u8 *) * num_files);
              current_file_selection = moving_from;
              /* Keep it on the screen, the grid's rows where they always are (a multiple of 3 first, or the
               * games would change columns). */
              if(current_file_selection < current_file_scroll_value)
                current_file_scroll_value = current_file_selection / cols * cols;
              else if(current_file_selection >= current_file_scroll_value + page)
                current_file_scroll_value = current_file_selection / cols * cols - (page - cols);
              moving = -1;
              gui_action = CURSOR_NONE;
              break;

            default:
              gui_action = CURSOR_NONE;
              break;
          }
        }
        else if(view_key)
        {
          /* UI17: the grid, or a line an item; the picked one stays picked, on the screen */
          list_lines = !list_lines;
          cols = list_lines ? 1 : 3;
          page = list_lines ? UI_LIST_LINES : FILE_LIST_ROWS;
          current_file_scroll_value = list_lines ? current_file_selection - page / 2 :
           current_file_selection / cols * cols;
          if(current_file_scroll_value + page > num_items)
            current_file_scroll_value = (num_items - 1) / cols * cols + cols - page;
          if(current_file_scroll_value < 0)
            current_file_scroll_value = 0;
          list_cache_clear();
          list_redraw = 1;
          gui_action = CURSOR_NONE;
        }
        else if(saves_key)
        {
          /* UI17: the picked game's Saves place; a state picked there starts the game from it */
          char path[MAX_PATH + 256];
          int slot = -1, on_drive = 0;
          list_path(path, sizeof(path), (char *)current_dir_name, (char *)file_list[current_file_selection]);
          if(nspire_saves_place(path, &slot, &on_drive))
          {
            repeat = 0;
            return_value = 0;
            strcpy(result, file_list[current_file_selection]);
            nspire_skip_state_load = 0;
            nspire_start_slot = slot;
            nspire_start_slot_on_drive = on_drive;
          }
          list_cache_clear();   /* (its saves may have changed) */
          list_redraw = 1;
          gui_action = CURSOR_NONE;
        }
        else if(usb_key)
        {
          repeat = 0;
          if(nspire_drive_browser((char *)result, 512))
            return_value = 0;
          gui_action = CURSOR_NONE;
        }
        else if(tab && num_files > 1 && order_before)
        {
          if(key_down(KEY_SHIFT))
          {
            game_list_forget_order((char *)current_dir_name);
            game_list_sort((char **)file_list, num_files);
            snprintf(list_status, sizeof(list_status), "Sorted A to Z.");
          }
          else
          {
            memcpy(order_before, file_list, sizeof(u8 *) * num_files);
            moving = moving_from = current_file_selection;
          }
        }
      }

      /* r19: the arrows on the top row: left and right along it (GAMES is this grid; past it the grid is left
       * for the word before or after), down or enter into the items, esc or the menu key out (to the game, or
       * to Quit: menu()) */
      if(nav_focus)
      {
        switch(gui_action)
        {
          case CURSOR_LEFT:
          case CURSOR_RIGHT:
            nspire_list_nav_exit = gui_action == CURSOR_LEFT ? -1 : 1;
            return_value = -1;
            repeat = 0;
            break;

          case CURSOR_DOWN:
          case CURSOR_SELECT:
            if(num_items)
              nav_focus = 0;
            break;

          case CURSOR_BACK:
          case CURSOR_EXIT:
            nspire_list_left_by_esc = 1;
            return_value = -1;
            repeat = 0;
            break;

          default:
            break;
        }
        continue;
      }

      switch(gui_action)
      {
        case CURSOR_DOWN:
          if(num_items == 0)
            break;
          /* the item under it; when the row under it is shorter, that row's last item; from the last row,
           * the first row's item in the same column */
          if((current_file_selection / cols + 1) * cols < num_items)
          {
            current_file_selection += cols;
            if(current_file_selection > num_items - 1)
              current_file_selection = num_items - 1;
            if(current_file_selection >= current_file_scroll_value + page)
              current_file_scroll_value += cols;
          }
          else
          {
            current_file_selection %= cols;
            current_file_scroll_value = 0;
          }
          break;

        case CURSOR_R:
          if(num_items == 0)
            break;
          current_file_selection += page;
          if(current_file_selection > num_items - 1)
            current_file_selection = num_items - 1;
          current_file_scroll_value = (current_file_selection / cols) * cols;
          /* the screen full where the list ends */
          if(current_file_scroll_value + page > num_items)
            current_file_scroll_value = (num_items - 1) / cols * cols + cols - page;
          if(current_file_scroll_value < 0)
            current_file_scroll_value = 0;
          break;

        case CURSOR_UP:
          if(current_file_selection >= cols)
          {
            current_file_selection -= cols;
            if(current_file_selection < current_file_scroll_value)
              current_file_scroll_value -= cols;
            if(current_file_scroll_value < 0)
              current_file_scroll_value = 0;
          }
          else
            nav_focus = 1;   /* up from the first row: the top row */
          break;

        case CURSOR_L:
          if(num_items == 0)
            break;
          current_file_selection -= page;
          if(current_file_selection < 0)
            current_file_selection = 0;
          current_file_scroll_value = (current_file_selection / cols) * cols;
          break;

        case CURSOR_RIGHT:
          if(current_file_selection % cols < cols - 1 && current_file_selection + 1 < num_items)
            current_file_selection++;
          break;

        case CURSOR_LEFT:
          if(current_file_selection % cols > 0)
            current_file_selection--;
          break;

        case CURSOR_SELECT:
          if(num_items == 0)
            break;
          if(!on_game)
          {
            /* a folder: opened (the USB drive: its browser) */
            u8 *dir = dir_list[ITEM_DIR(current_file_selection)];
            repeat = 0;
            if(dir == usb_folder)
            {
              /* All load_file callers provide a 512-byte path buffer. */
              if(nspire_drive_browser((char *)result, 512))
                return_value = 0;
              break;
            }
            chdir(dir);
          }
          else
          {
            repeat = 0;
            return_value = 0;
            strcpy(result, file_list[current_file_selection]);
            nspire_skip_state_load = key_down(KEY_SHIFT);
          }
          break;

        case CURSOR_BACK:
        case CURSOR_EXIT:
          /* r19: esc (and the menu key) go back a step: to the top row */
          nav_focus = 1;
          break;

        default:
          break;
      }
      if(moving >= 0)
        moving = current_file_selection;
      #undef ITEM_DIR
    }
  }

    goto list_cleanup;
list_failed:
    return_value = -1;
    if(current_dir)
      closedir(current_dir);
list_cleanup:
    for(i = 0; i < num_files; i++)
    {
      free(file_list[i]);
    }
    free(file_list);
#if defined(NSPIRE_LIBRETRO)
    free(order_before);
    order_before = NULL;
#endif

    for(i = 0; i < num_dirs; i++)
    {
      free(dir_list[i]);
    }
    free(dir_list);
  }

  return return_value;
}

typedef enum
{
  NUMBER_SELECTION_OPTION = 0x01,
  STRING_SELECTION_OPTION = 0x02,
  SUBMENU_OPTION          = 0x04,
  ACTION_OPTION           = 0x08
} menu_option_type_enum;

struct _menu_type
{
  void (* init_function)();
  void (* passive_function)();
  struct _menu_option_type *options;
  u32 num_options;
};

struct _menu_option_type
{
  void (* action_function)();
  void (* passive_function)();
  struct _menu_type *sub_menu;
  char *display_string;
  void *options;
  u32 *current_option;
  u32 num_options;
  char *help_string;
  u32 line_number;
  menu_option_type_enum option_type;
};

typedef struct _menu_option_type menu_option_type;
typedef struct _menu_type menu_type;

#define make_menu(name, init_function, passive_function)                      \
  menu_type name##_menu =                                                     \
  {                                                                           \
    init_function,                                                            \
    passive_function,                                                         \
    name##_options,                                                           \
    sizeof(name##_options) / sizeof(menu_option_type)                         \
  }                                                                           \

#define MENU_SLOTS STATE_SLOTS

#define action_option(action_function, passive_function, display_string,      \
 help_string, line_number)                                                    \
{                                                                             \
  action_function,                                                            \
  passive_function,                                                           \
  NULL,                                                                       \
  display_string,                                                             \
  NULL,                                                                       \
  NULL,                                                                       \
  0,                                                                          \
  help_string,                                                                \
  line_number,                                                                \
  ACTION_OPTION                                                               \
}                                                                             \

#define submenu_option(sub_menu, display_string, help_string, line_number)    \
{                                                                             \
  NULL,                                                                       \
  NULL,                                                                       \
  sub_menu,                                                                   \
  display_string,                                                             \
  NULL,                                                                       \
  NULL,                                                                       \
  sizeof(sub_menu) / sizeof(menu_option_type),                                \
  help_string,                                                                \
  line_number,                                                                \
  SUBMENU_OPTION                                                              \
}                                                                             \

#define selection_option(passive_function, display_string, options,           \
 option_ptr, num_options, help_string, line_number, type)                     \
{                                                                             \
  NULL,                                                                       \
  passive_function,                                                           \
  NULL,                                                                       \
  display_string,                                                             \
  options,                                                                    \
  option_ptr,                                                                 \
  num_options,                                                                \
  help_string,                                                                \
  line_number,                                                                \
  type                                                                        \
}                                                                             \

#define action_selection_option(action_function, passive_function,            \
 display_string, options, option_ptr, num_options, help_string, line_number,  \
 type)                                                                        \
{                                                                             \
  action_function,                                                            \
  passive_function,                                                           \
  NULL,                                                                       \
  display_string,                                                             \
  options,                                                                    \
  option_ptr,                                                                 \
  num_options,                                                                \
  help_string,                                                                \
  line_number,                                                                \
  type | ACTION_OPTION                                                        \
}                                                                             \


#define string_selection_option(passive_function, display_string, options,    \
 option_ptr, num_options, help_string, line_number)                           \
  selection_option(passive_function, display_string ": %s", options,          \
   option_ptr, num_options, help_string, line_number, STRING_SELECTION_OPTION)\

#define numeric_selection_option(passive_function, display_string,            \
 option_ptr, num_options, help_string, line_number)                           \
  selection_option(passive_function, display_string ": %d", NULL, option_ptr, \
   num_options, help_string, line_number, NUMBER_SELECTION_OPTION)            \

#define string_selection_(action_function, passive_function,     \
 display_string, options, option_ptr, num_options, help_string, line_number)  \
  action_selection_option(action_function, passive_function,                  \
   display_string ": %s",  options, option_ptr, num_options, help_string,     \
   line_number, STRING_SELECTION_OPTION)                                      \

#define numeric_selection_action_option(action_function, passive_function,    \
 display_string, option_ptr, num_options, help_string, line_number)           \
  action_selection_option(action_function, passive_function,                  \
   display_string ": %d",  NULL, option_ptr, num_options, help_string,        \
   line_number, NUMBER_SELECTION_OPTION)                                      \

#define numeric_selection_action_hide_option(action_function,                 \
 passive_function, display_string, option_ptr, num_options, help_string,      \
 line_number)                                                                 \
  action_selection_option(action_function, passive_function,                  \
   display_string, NULL, option_ptr, num_options, help_string,                \
   line_number, NUMBER_SELECTION_OPTION)                                      \

#define GAMEPAD_MENU_WIDTH 15

#ifdef PSP_BUILD

u32 gamepad_config_line_to_button[] =
 { 8, 6, 7, 9, 1, 2, 3, 0, 4, 5, 11, 10 };

#endif

#ifdef GP2X_BUILD

u32 gamepad_config_line_to_button[] =
 { 0, 2, 1, 3, 8, 9, 10, 11, 6, 7, 4, 5, 14 };

#endif

#ifdef NSPIRE_BUILD

u32 gamepad_config_line_to_button[] =
 { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };

/* Key names, by gpSP's key numbers (nspire_keys.c). */
extern char *nspire_key_strings[];
 
#endif

u8 *scale_options[] =
{
#ifdef WIZ_BUILD
  "unscaled 3:2", "scaled 3:2 (slower)",
  "unscaled 3:2 (anti-tear)", "scaled 3:2 (anti-tear)"
#elif defined(NSPIRE_LIBRETRO)
  "Small (240x160)", "3:2, smooth", "Full screen, smooth", "3:2, sharp"
#elif NSPIRE_BUILD
  "unscaled 3:2 (small)", "scaled/blended 3:2 (slowest)", "fullscreen/blended (slower)", "scaled/unblended 3:2 (fast)"
#else
  "unscaled 3:2", "scaled 3:2", "fullscreen", "scaled 3:2 (software)"
#ifdef PSP_BUILD
  " 16:9"
#endif
#endif
};

#if defined(NSPIRE_LIBRETRO)

/* gpSP's settings files: see nspire_config.c. */
s32 load_game_config_file()
{
  return 0;   /* a game's own settings open with it (nspire_start_game) */
}

s32 load_config_file()
{
  return nspire_config_load();
}

s32 save_game_config_file()
{
  return 0;   /* written with save_config_file */
}

s32 save_config_file()
{
  return nspire_config_save();
}

#else
s32 load_game_config_file()
{
  u8 game_config_filename[512];
  u32 file_loaded = 0;
  u32 i;
#ifdef NSPIRE_BUILD
  change_ext(gamepak_filename, game_config_filename, ".cfg.tns");
#else
  change_ext(gamepak_filename, game_config_filename, ".cfg");
#endif

  file_open(game_config_file, game_config_filename, read);

  if(file_check_valid(game_config_file))
  {
    u32 file_size = file_length(game_config_filename, game_config_file);

#if defined(NSPIRE_LIBRETRO)
    if (file_size == 64)
    {
      u32 file_options[16];

      file_read_array(game_config_file, file_options);
      {
        u32 r0 = file_options[0];
        u32 r15 = file_options[15];
        if ((r15 & GPSP_GAME_FS_V2_MARK) == GPSP_GAME_FS_V2_MARK)
          current_frameskip_type = (frameskip_type)(r0 % 3);
        else
        {
          static const frameskip_type om[] = {
            no_frameskip, auto_frameskip, auto_frameskip, manual_frameskip
          };
          current_frameskip_type = om[r0 % 4];
        }
      }
      frameskip_value = file_options[1];
      random_skip = file_options[2] % 2;
      clock_speed = file_options[3];

      if (clock_speed > 300)
        clock_speed = 132;
      if (clock_speed < 33)
        clock_speed = 33;

      if (frameskip_value > 99)
        frameskip_value = 99;
      if (frameskip_value < 1 && current_frameskip_type == auto_frameskip)
        frameskip_value = 4;

      for (i = 0; i < 10; i++)
      {
        cheats[i].cheat_active = file_options[4 + i] % 2;
        cheats[i].cheat_name[0] = 0;
      }

      quick_save_slot = file_options[14];

      file_loaded = 1;
    }
    else if (file_size == 60)
    {
      u32 file_options[15];

      file_read_array(game_config_file, file_options);
      {
        u32 old_type = file_options[0] % 3;
        if (old_type == 2)
          current_frameskip_type = no_frameskip;
        else if (old_type == 1)
          current_frameskip_type = manual_frameskip;
        else
          current_frameskip_type = auto_frameskip;
      }
      frameskip_value = file_options[1];
      if (frameskip_value > 99)
        frameskip_value = 99;
      if (frameskip_value < 1 && current_frameskip_type == auto_frameskip)
        frameskip_value = 4;

      random_skip = file_options[2] % 2;
      clock_speed = file_options[3];

      if (clock_speed > 300)
        clock_speed = 132;
      if (clock_speed < 33)
        clock_speed = 33;

      for (i = 0; i < 10; i++)
      {
        cheats[i].cheat_active = file_options[4 + i] % 2;
        cheats[i].cheat_name[0] = 0;
      }

      quick_save_slot = file_options[14];

      file_loaded = 1;
    }
#else
    /* Sanity check: File size must be the right size */
    if(file_size == 60)
    {
      u32 file_options[file_size / 4];

      file_read_array(game_config_file, file_options);
      current_frameskip_type = file_options[0] % 3;
      frameskip_value = file_options[1];
      random_skip = file_options[2] % 2;
      clock_speed = file_options[3];

#ifdef WIZ_BUILD
      if(clock_speed > 900)
        clock_speed = 533;
#elif defined(GP2X_BUILD)
      if(clock_speed >= 300)
        clock_speed = 200;
#elif defined(PSP_BUILD)
      if(clock_speed > 333)
        clock_speed = 333;
#elif defined(NSPIRE_BUILD)
	  if(clock_speed > 300)
	    clock_speed = 132;
#endif

      if(clock_speed < 33)
        clock_speed = 33;

      if(frameskip_value < 0)
        frameskip_value = 0;

      if(frameskip_value > 99)
        frameskip_value = 99;

      for(i = 0; i < 10; i++)
      {
        cheats[i].cheat_active = file_options[4 + i] % 2;
        cheats[i].cheat_name[0] = 0;
      }
      
	  quick_save_slot = file_options[14];
	  
      file_loaded = 1;
    }
#endif
	file_close(game_config_file);
  }

  if(file_loaded)
    return 0;

#if defined(NSPIRE_LIBRETRO)
  current_frameskip_type = manual_frameskip;
  frameskip_value = 2;
  random_skip = 0;
#else
  current_frameskip_type = auto_frameskip;
  frameskip_value = 4;
#ifdef WIZ_BUILD
  frameskip_value = 1;
#endif
  random_skip = 0;
#endif
  clock_speed = default_clock_speed;

  for(i = 0; i < 10; i++)
  {
    cheats[i].cheat_active = 0;
    cheats[i].cheat_name[0] = 0;
  }

  return -1;
}

s32 load_config_file()
{
  u8 config_path[512];

  #if (defined(PSP_BUILD) || defined(ARM_ARCH)) && !defined(_WIN32_WCE)
    sprintf(config_path, "%s/%s", main_path, GPSP_CONFIG_FILENAME);
  #else
    sprintf(config_path, "%s\\%s", main_path, GPSP_CONFIG_FILENAME);
  #endif

  file_open(config_file, config_path, read);

  if(file_check_valid(config_file))
  {
    u32 file_size = file_length(config_path, config_file);

    // Sanity check: file sizes by feature growth:
    // 96 = +BIOS, 112 = +frameskip, 124 = +emulator options, 128 = +ROM buffer size,
    // 132 = +LCD cache mode, 136 = +FPS overlay, 140 = +GBA blend skip, 144 = +dynarec SMC policy,
    // 148 = legacy (extra u32 for removed RAM block reuse option; ignored), 152 = +video renderer
    // (removed since: its word is read as nothing and written as 0).
    if(file_size == 92
#if defined(NSPIRE_LIBRETRO)
     || file_size == 96
     || file_size == 112
     || file_size == 124
     || file_size == 128
     || file_size == 132
     || file_size == 136
     || file_size == 140
     || file_size == 144
     || file_size == 148
     || file_size == 152
#endif
     )
    {
      u32 words = file_size / 4;
#if defined(NSPIRE_LIBRETRO)
      u32 file_options[38];
#else
      u32 file_options[28];
#endif
      u32 i;
      s32 menu_button = -1;

      memset(file_options, 0, sizeof(file_options));
      file_read(config_file, file_options, words * sizeof(u32));

      screen_scale = file_options[0] %
        (sizeof(scale_options) / sizeof(scale_options[0]));
      screen_filter = file_options[1] % 2;
      global_enable_audio = file_options[2] % 2;

#ifdef PSP_BUILD
      audio_buffer_size_number = file_options[3] % 10;
#else
      audio_buffer_size_number = file_options[3] % 11;
#endif

      update_backup_flag = file_options[4] % 2;
      global_enable_analog = file_options[5] % 2;
      analog_sensitivity_level = file_options[6] % 8;

#ifdef PSP_BUILD
    scePowerSetClockFrequency(clock_speed, clock_speed, clock_speed / 2);
#endif

      // Sanity check: Make sure there's a MENU or FRAMESKIP
      // key, if not assign to triangle

#ifndef PC_BUILD
#ifdef NSPIRE_BUILD
	  for(i = 0; i < 16; i++)
	  {
	    gamepad_config_map[i] = file_options[7 + i];
		if(gamepad_config_map[i] > 128 || nspire_key_strings[gamepad_config_map[i]] == NULL)
			gamepad_config_map[i] = 0;
	  }
	  if(!gamepad_config_map[10]) //KEY_MAP_MENU
		gamepad_config_map[10] = 102; //KEY_MENU
#else
      for(i = 0; i < 16; i++)
      {
        gamepad_config_map[i] = file_options[7 + i] %
         (BUTTON_ID_NONE + 1);

        if(gamepad_config_map[i] == BUTTON_ID_MENU)
        {
          menu_button = i;
        }
      }

      if(menu_button == -1)
      {
        gamepad_config_map[0] = BUTTON_ID_MENU;
      }
#endif
#endif

#if defined(NSPIRE_LIBRETRO)
      if (words >= 24)
        nspire_bios_choice = file_options[23] % 2;
      else
        nspire_bios_choice = 1;

      if (words >= 28)
      {
        u32 r24 = file_options[24];
        u32 r26 = file_options[26];
        if ((r26 & GPSP_CFG_FS_V2_MARK) == GPSP_CFG_FS_V2_MARK)
          current_frameskip_type = (frameskip_type)(r24 % 3);
        else
        {
          static const frameskip_type om[] = {
            no_frameskip, auto_frameskip, auto_frameskip, manual_frameskip
          };
          current_frameskip_type = om[r24 % 4];
        }
        frameskip_value = file_options[25];
        if (frameskip_value > 99)
          frameskip_value = 99;
        if (frameskip_value < 1 && current_frameskip_type == auto_frameskip)
          frameskip_value = 4;
        random_skip = file_options[27] % 2;
      }

      if (words >= 31)
      {
        nspire_sprlim_choice = file_options[28] % 2;
        nspire_rtc_choice = file_options[29] % 3;
        nspire_frame_mix_choice = file_options[30] % 2;
      }
      if (words >= 32)
        nspire_rom_buffer_size_choice = file_options[31];
      else
        nspire_rom_buffer_size_choice = 8;
      if (words >= 33)
        nspire_lcd_cache_clean_full = file_options[32] % 2;
      else
        nspire_lcd_cache_clean_full = 0;
      if (words >= 34)
        nspire_fps_overlay = file_options[33] % 2;
      else
        nspire_fps_overlay = 0;
      if (words >= 35)
        nspire_gba_blend_off = file_options[34] % 2;
      else
        nspire_gba_blend_off = 0;
      if (words >= 36)
        nspire_dynarec_ram_policy = file_options[35] % 2;
      else
        nspire_dynarec_ram_policy = 0;
      /* word 36 was the renderer (1 = legacy), which ignored blending Off */
      if (words >= 37 && file_options[36] == 1)
        nspire_gba_blend_off = 0;
      nspire_clamp_rom_buffer_size();
      nspire_emulator_options_apply();
#endif

      file_close(config_file);
    }

    return 0;
  }

  return -1;
}

s32 save_game_config_file()
{
  u8 game_config_filename[512];
  u32 i;

#ifdef NSPIRE_BUILD
  change_ext(gamepak_filename, game_config_filename, ".cfg.tns");
#else
  change_ext(gamepak_filename, game_config_filename, ".cfg");
#endif

  file_open(game_config_file, game_config_filename, write);

  if(file_check_valid(game_config_file))
  {
#if defined(NSPIRE_LIBRETRO)
    u32 file_options[16];

    file_options[0] = (u32)current_frameskip_type % 3;
    file_options[1] = frameskip_value;
    file_options[2] = random_skip;
    file_options[3] = clock_speed;

    for(i = 0; i < 10; i++)
    {
      file_options[4 + i] = cheats[i].cheat_active;
    }

    file_options[14] = quick_save_slot;
    file_options[15] = GPSP_GAME_FS_V2_MARK;

    file_write(game_config_file, file_options, sizeof(file_options));
#else
    u32 file_options[15];

    file_options[0] = current_frameskip_type;
    file_options[1] = frameskip_value;
    file_options[2] = random_skip;
    file_options[3] = clock_speed;

    for(i = 0; i < 10; i++)
    {
      file_options[4 + i] = cheats[i].cheat_active;
    }
	
	file_options[14] = quick_save_slot;
	
    file_write_array(game_config_file, file_options);
#endif

    file_close(game_config_file);

    return 0;
  }

  return -1;
}

s32 save_config_file()
{
  u8 config_path[512];

  #if (defined(PSP_BUILD) || defined(ARM_ARCH)) && !defined(_WIN32_WCE)
    sprintf(config_path, "%s/%s", main_path, GPSP_CONFIG_FILENAME);
  #else
    sprintf(config_path, "%s\\%s", main_path, GPSP_CONFIG_FILENAME);
  #endif

  save_game_config_file();
  
  file_open(config_file, config_path, write);

  if(file_check_valid(config_file))
  {
#if defined(NSPIRE_LIBRETRO)
    u32 file_options[37];
#else
    u32 file_options[23];
#endif
    u32 i;

    file_options[0] = screen_scale;
    file_options[1] = screen_filter;
    file_options[2] = global_enable_audio;
    file_options[3] = audio_buffer_size_number;
    file_options[4] = update_backup_flag;
    file_options[5] = global_enable_analog;
    file_options[6] = analog_sensitivity_level;

#ifndef PC_BUILD
    for(i = 0; i < 16; i++)
    {
      file_options[7 + i] = gamepad_config_map[i];
    }
#endif

#if defined(NSPIRE_LIBRETRO)
    nspire_clamp_rom_buffer_size();
    file_options[23] = nspire_bios_choice % 2;
    file_options[24] = (u32)current_frameskip_type % 3;
    file_options[25] = frameskip_value;
    file_options[26] = GPSP_CFG_FS_V2_MARK;
    file_options[27] = random_skip % 2;
    file_options[28] = nspire_sprlim_choice % 2;
    file_options[29] = nspire_rtc_choice % 3;
    file_options[30] = nspire_frame_mix_choice % 2;
    file_options[31] = nspire_rom_buffer_size_choice;
    file_options[32] = nspire_lcd_cache_clean_full % 2;
    file_options[33] = nspire_fps_overlay % 2;
    file_options[34] = nspire_gba_blend_off % 2;
    file_options[35] = nspire_dynarec_ram_policy % 2;
    file_options[36] = 0;   /* reserved: the video renderer choice (removed) */
    file_write(config_file, file_options, 37 * sizeof(u32));
#else
    file_write_array(config_file, file_options);
#endif

    file_close(config_file);

    return 0;
  }

  return -1;
}
#endif

u32 savestate_slot = 0;

u32 menu(u16 *original_screen)
{
  nspire_audio_pause();
  u32 clock_speed_number;
  gui_action_type gui_action;
  u32 i;
  u32 repeat = 1;
  u32 return_value = 0;
  u32 first_load = 0;
  u32 opened_at = nspire_ticks();   /* for the timing file: the menu's opening, to its first picture */

  menu_type *current_menu;
  menu_option_type *current_option;
  u32 current_option_num;

  auto void choose_menu();
#if defined(NSPIRE_LIBRETRO)
  auto void nav_publish();
  auto void list_left();
  auto void load_picked(int slot, int on_drive);
  auto void saves_place_after();
  /* r19: the paused game's saves (the top row's SAVES) open with the arrows on the row (1), or in its tiles (0:
   * as the menu opens, on Resume; 2: on the tile they were last left on) */
  int saves_nav = 0;
  /* r16: where the menu's saves and loads go (nspire_places.h): the calculator, or a USB drive: the place the
   * saves' tiles last showed */
  int place = PLACE_CALCULATOR;
#endif

#ifdef NSPIRE_BUILD
  /* On the settings pages enter picks a line to change (then left/right
   * change it, enter keeps it, esc puts it back) */
  int editing = 0;
  int menu_redraw = 1;         /* the menu is drawn again (what it shows may have changed) */
  u32 edit_from = 0;           /* the value when it was picked, for esc */
  char menu_status[80] = "";   /* what the last action did */
  u8 *autosave_options[AUTOSAVE_CHOICES];

  for (i = 0; i < AUTOSAVE_CHOICES; i++)
    autosave_options[i] = (u8 *)nspire_autosave_names[i];
#endif

  void menu_update_clock()
  {
    get_clock_speed_number();
    if (clock_speed_number < 0 || clock_speed_number >=
     sizeof(clock_speed_options) / sizeof(clock_speed_options[0]))
    {
      clock_speed = default_clock_speed;
      get_clock_speed_number();
    }
  }

  void menu_exit()
  {
    if(!first_load)
     repeat = 0;
  }

  /* A message over the menu: what went wrong and what to do, one OK. */
  void tell(const char *title, const char *text)
  {
    struct ui_dialog_view d;
    memset(&d, 0, sizeof(d));
    d.title = title;
    d.text = text;
    d.first = "OK";
    d.warn = 1;
    nspire_link_log("menu: told \"%s\": %s", title, text);   /* (the tests read what the menus said) */
    nspire_ask(&d, NULL, NULL);
    menu_redraw = 1;
  }

  /* A question over the menu, the safe answer first: 1 when the second
   * (a red one if it destroys something) was picked. */
  int ask(const char *title, const char *text, const char *first, const char *second)
  {
    struct ui_dialog_view d;
    int answer;
    memset(&d, 0, sizeof(d));
    d.title = title;
    d.text = text;
    d.first = first;
    d.second = second;
    d.destructive = 1;
    answer = nspire_ask(&d, NULL, NULL);
    menu_redraw = 1;
    return answer == 1;
  }

  /* Quit asks nothing (no extra step on the way out). A game
   * that is open is saved first when Save when I quit is on (it is, unless
   * the player turned it off), and left as it is when it is off. */
  void nspire_show_closing(void);

  void menu_quit()
  {
    nspire_show_closing();
    if(!first_load)
      nspire_exit_save = 0;   /* as Save when I quit says (nspire_leave_game) */
    menu_get_clock_speed();
#if defined(NSPIRE_LIBRETRO)
    if(!first_load)
      nspire_quit_game();
#endif
    save_config_file();
    quit();
  }

  /* r19: Games and Folders are the list (load_file), under the same row of tabs: opened with the arrows on
   * the row (nspire_list_nav), left for the tab before or after, or by esc (list_left). */
  void menu_load()
  {
    u8 *file_ext[] = { ".gba.tns", ".gbz.tns", ".zip.tns", NULL };
    u8 load_filename[512];

    nav_publish();
    nspire_list_nav = 1;
    if(load_file(file_ext, load_filename) != -1)
    {
      int started;
#if defined(NSPIRE_LIBRETRO)
      /* UI17: a state of the paused game itself picked in the store's Saves: loaded in place, as Load does (no
       * leaving: Save when I quit would write over the picked slot first) */
      if(!first_load && nspire_start_slot >= 0)
      {
        char cwd[MAX_PATH], path[MAX_PATH + 256];
        const char *f = (const char *)load_filename;
        cwd[0] = 0;
        /* (a whole path already: a drive's "usb:/...", or one from the root, as the start keeps it) */
        if(nspire_drive_path(f) || f[0] == '/' || f[0] == '\\' || (f[0] && f[1] == ':'))
          snprintf(path, sizeof(path), "%s", f);
        else
        {
          getcwd(cwd, sizeof(cwd));
          list_path(path, sizeof(path), cwd, f);
        }
        if(nspire_same_rom_path(path, (const char *)gamepak_filename))
        {
          int slot = nspire_start_slot, on_drive = nspire_start_slot_on_drive;
          nspire_start_slot = -1;
          place = on_drive ? PLACE_DRIVE : PLACE_CALCULATOR;
          load_picked(slot, on_drive);
          if(repeat)
            saves_place_after();
          return;
        }
      }
#endif
      if(!first_load)
        nspire_leave_game();
      started = nspire_start_game((char *)load_filename);
      /* Q cancelled it: the list again (the last game is closed) */
      while(!started && nspire_start_cancelled)
      {
        gamepak_filename[0] = 0;
        first_load = 1;
        if(load_file(file_ext, load_filename) == -1)
          break;
        started = nspire_start_game((char *)load_filename);
      }
      if(!started)
      {
        /* It said why (or the list was left after a cancel). The last game
         * is closed: the menu is without one. */
        gamepak_filename[0] = 0;
        first_load = 1;
        ui_backdrop_clear();
        choose_menu(current_menu);
        if(nspire_list_nav_exit || nspire_list_left_by_esc)
          list_left();
        return;
      }
      first_load = 0;
      return_value = 1;
      repeat = 0;
      menu_update_clock();
    }
    else
    {
#if defined(NSPIRE_LIBRETRO)
      if(!first_load)
        saves_place_after();   /* (the store's Saves may have changed the paused game's) */
#endif
      list_left();
    }
  }

  void menu_restart()
  {
    if(!first_load)
    {
      if(!ask("Restart the game?", "It goes back to its first screen. Anything you haven't saved is lost.",
       "Keep playing", "Restart"))
        return;
      nspire_apply_bios();
      reset_gba();
      nspire_frameskip_reset();
      instruction_count = 0;
      reg[OAM_UPDATED] = 1;
      nspire_osd_message("Game restarted");
      return_value = 1;
      repeat = 0;
    }
  }

  void place_save()
  {
    int slot = nspire_drive_saves->next ? nspire_drive_saves->next() : -1;
    if(slot < 0)
      tell("Couldn't save", "The USB drive has no free slot. Delete some saves on it, then try again.");
    else if(nspire_drive_saves->save(slot) == 1)
      repeat = 0;
    else if(nspire_usbhub_recovering() || nspire_usbhub_doubtful() == 1)
      tell("Couldn't save", "The USB hub is restarting: the drive comes back in a few seconds. Try again then.");
    else
      tell("Couldn't save", "The USB drive is full or was taken out. Try again.");
  }
  void menu_load_state()
  {
    int result;

    if(first_load)
      return;
    result = states_load((int)savestate_slot);
    if(result == 1)
    {
      nspire_osd_message("Loaded slot %d", (int)savestate_slot + 1);
      nspire_frameskip_reset();
      return_value = 1;
      repeat = 0;
    }
    else if(result == 0)
      snprintf(menu_status, sizeof(menu_status), "Slot %d is empty.", (int)savestate_slot + 1);
    else
      tell("That save can't be loaded", state_failure_text(0));
  }

  void load_game_save()
  {
    long sb = 0;
    int sc = 0;
    if(first_load || !nspire_backup_info(&sb, &sc))
    {
      snprintf(menu_status, sizeof(menu_status), "No in-game save found.");
      return;
    }
    nspire_apply_bios();
    reset_gba();
    nspire_load_cartridge_backup();
    nspire_frameskip_reset();
    instruction_count = 0;
    reg[OAM_UPDATED] = 1;
    nspire_osd_message("Loaded in-game save");
    return_value = 1;
    repeat = 0;
  }

  void save_slot_now(int slot, int ask_replace, int allow_overwrite_msg)
  {
    int existed;
    if(first_load)
      return;
    if(place == PLACE_DRIVE)
    {
      int next = nspire_drive_saves->next ? nspire_drive_saves->next() : -1;
      if(slot != next)
        snprintf(menu_status, sizeof(menu_status), "USB drive saves to New only.");
      else
        place_save();
      return;
    }
    if(slot < 0)
    {
      tell("Couldn't save", "All 1000 slots are used. Delete some saves (M: All saves), then try again.");
      return;
    }
    existed = states_exists(slot);
    if(ask_replace && existed)
    {
      char title[32], text[96];
      snprintf(title, sizeof(title), "Replace slot %d?", slot + 1);
      snprintf(text, sizeof(text), "The save in slot %d is lost. You can't undo this.", slot + 1);
      if(!ask(title, text, "No, keep it", "Replace"))
        return;
    }
    if(states_save(slot))
    {
      savestate_slot = (u32)slot;
      nspire_save_pick(-1);
      nspire_osd_message(allow_overwrite_msg && existed ? "Saved over slot %d" : "Saved to slot %d", slot + 1);
      repeat = 0;
    }
    else
      tell("Couldn't save", state_failure_text(1));
  }

  /* UI17: the paused game's Saves place (m on Save and Load; System's Manage saves): a state picked there is
   * loaded as Load loads it, and the game goes on from it */
  /* A state picked in a Saves place loaded into the paused game, as Load loads it; a drive's by its own number
   * (the calculator's picked slot, which Save and the game's keys go by, isn't set to a drive's number) */
  void load_picked(int slot, int on_drive)
  {
    nspire_start_picture_has = -1;   /* (no start, no loading screen) */
    free(nspire_start_picture);
    nspire_start_picture = NULL;
    if(!on_drive)
    {
      savestate_slot = (u32)slot;
      menu_load_state();
      return;
    }
    if(nspire_drive_saves && nspire_drive_saves->load(slot) == 1)
    {
      nspire_osd_message("Loaded slot %d (USB drive)", slot + 1);
      nspire_frameskip_reset();
      return_value = 1;
      repeat = 0;
    }
    else
      tell("That save can't be loaded", "The USB drive was taken out, or the save is damaged. The game goes on.");
  }

  /* Back from a Saves place without a load: the picked slot as the saves are now */
  void saves_place_after()
  {
    if(place == PLACE_CALCULATOR && !states_exists((int)savestate_slot) && states_newest() >= 0)
      savestate_slot = (u32)states_newest();
    menu_redraw = 1;
  }

  /* r19: the top row's SAVES: the paused game's saves (nspire_save_manager.c nspire_saves_tab: Resume, Save, its
   * own save, each state), drawn under the same row; it takes the arrows until they leave its word, something
   * is picked or esc is pressed. What is picked is done here, as the menu always did it: a save through
   * save_slot_now (its questions and messages), a load through load_picked. */
  auto void nav_to(u32 n);
  auto u32 tab_of(void (*action)());
  auto u32 next_visible(u32 n, int step);
  auto void tabs_esc();
  auto int option_hidden(menu_option_type *o);
  void menu_saves()
  {
    int slot = (int)savestate_slot, on_drive = 0, how;
    u32 at = current_option_num;

    if(first_load)
      return;
    nav_publish();
    how = nspire_saves_tab(nav_names, nav_count, nav_saves, saves_nav, &slot, &on_drive);
    saves_nav = 1;
    menu_status[0] = 0;
    switch(how)
    {
      case SAVES_TAB_RESUME:
        menu_exit();
        break;

      case SAVES_TAB_SAVE:
        place = on_drive ? PLACE_DRIVE : PLACE_CALCULATOR;
        if(slot < 0)
          slot = place == PLACE_DRIVE ? (nspire_drive_saves->next ? nspire_drive_saves->next() : -1) :
           nspire_save_target();
        save_slot_now(slot, 1, 1);
        saves_nav = 2;   /* (not saved: its tiles again, on the one it was on, what happened said in their band) */
        break;

      case SAVES_TAB_LOAD:
        /* (r19: the place it is loaded from is the menu's place from here on: with Saves on the USB
         * drive, a calculator state loaded left the picked slot on the newest state at the menu's end, and
         * the game's save key then wrote over that one) */
        place = on_drive ? PLACE_DRIVE : PLACE_CALCULATOR;
        load_picked(slot, on_drive);
        saves_nav = 2;
        break;

      case SAVES_TAB_LOAD_GAME_SAVE:
        /* the game starts again from its own save: asked first, as Restart is (what was played since that
         * save is lost; and del, the GBA's A as the keys come, is enter here) */
        if(ask("Restart from the in-game save?", "The game starts again from its own save. Anything not "
         "saved since is lost.", "Keep playing", "Restart"))
          load_game_save();
        saves_nav = 2;
        break;

      case SAVES_TAB_LEFT:
      case SAVES_TAB_RIGHT:
        nav_to(next_visible(at, how == SAVES_TAB_LEFT ? -1 : 1));
        break;

      default:
        nav_to(at);
        tabs_esc();
        break;
    }
    if(repeat && menu_status[0])
    {
      nspire_saves_status(menu_status);
      menu_status[0] = 0;
    }
    saves_place_after();
  }

  void menu_rom_manager()
  {
    nspire_rom_manager();
  }

  /* More > Saves: the paused game's saves as a page (nspire_saves_place, the one GAMES' S opens for any game);
   * a state picked there is loaded. Without a game: where the saves of a game are. */
  void menu_save_manager()
  {
    int slot = -1, on_drive = 0;
    if(first_load)
    {
      snprintf(menu_status, sizeof(menu_status), "In Games, press S on a game.");
      return;
    }
    if(nspire_saves_place(gamepak_filename, &slot, &on_drive))
    {
      place = on_drive ? PLACE_DRIVE : PLACE_CALCULATOR;
      load_picked(slot, on_drive);
    }
  }

  /* r19: Settings > Cheats: the codes of the game's cheat file, a line each with its switch (nspire_cheats.c) */
  void menu_cheats()
  {
    /* (a file with none of the engine's codes: said, where its tile is, instead of an empty page) */
    if(!nspire_cheats_count())
      snprintf(menu_status, sizeof(menu_status), "CodeBreaker codes only: none here.");
    else
      nspire_cheats_page();
  }

  /* Controls: the keys on a drawing of the keypad (nspire_controls.c). */
  void menu_controls()
  {
    nspire_controls_screen();
  }

  /* PAD3: the USB port, the OS and the controller driver, live (nspire_padpage.c). */
  void menu_controller_status()
  {
    nspire_controller_page();
  }

  /* r16: Bluetooth's page (nspire_devices.c), listed while the build has it (nspire_bt) */
  void menu_bluetooth()
  {
    nspire_bluetooth_page();
  }

  /* Links, or unlinks; back to the game either way (it goes on while the
   * other GBA is looked for). */
  void menu_link()
  {
    if(first_load)
      return;
    if(nspire_link_state() == LINK_OFF)
      nspire_link_connect();
    else
      nspire_link_disconnect();
    if(nspire_link_state() != LINK_OFF)
      repeat = 0;
  }

  /* The pages with nothing to prepare. */
  void submenu_none()
  {
  }

  u8 *enable_disable_options[] = { "disabled", "enabled" };
  /* r19: the settings a player needs, each with a name and values that say what they are without the manual
   * (labels a beginner understands, where a name like Less flicker said too little, and no setting that
   * isn't needed). What went from the pages (its value kept as the
   * files have it): Skip up to and Skip pattern (manual frame skip's), Sprite limit, Compress saves, In-game
   * save's time, ROM cache, Sound's switch (volume 0 is off) and Channels. Speed test and LCD cache moved to
   * System, Blending to Speed, Cartridge clock to System, Link (the paused menu's choice) to its page.
   * A setting with up to CYCLE_MAX values is changed by enter alone, a press a value (until r19 only off/on
   * was: the others took enter, an arrow, enter); one with more (a range) between chevrons, by the arrows. */
  #define CYCLE_MAX 4
  u8 *frameskip_options[] = { "Auto", "Manual", "Off" };
  u8 *brightness_options[] = { "System", "1/10", "2/10", "3/10", "4/10", "5/10",
   "6/10", "7/10", "8/10", "9/10", "10/10" };
  /* (r19: every value of a tile fits a third of the width, so its page is three across: "Wireless adapter"
   * was longer) */
  u8 *link_device_options[] = { "Auto", "Wireless", "Cable", "Cable, exact" };
  u8 *off_on_options[] = { "off", "on" };
  u8 *slot_options[] = { "Same slot", "New slot" };
  u8 *saves_on_options[] = { (u8 *)"Calculator", (u8 *)"USB drive" };

  /* r19: the picture's size is one setting of three, named for what the player sees (the old names Full, Fit
   * and Small said too little): Original (the GBA's own pixels, a border round them), Fit (as wide as the screen,
   * the GBA's shape kept: bars above and below), Stretch (the whole screen, a little taller than it should be).
   * Fit is SMOOTHED (scaled_aspect), always: the sharp Fit (scaled_raw) is the one that went. (The first cut
   * made it the sharp one, on the idea that few players use a smoothed picture; it is the other way round:
   * on the calculator the sharp one looked wrong, and few players turn the smoothing off.) */
  u8 *picture_options[] = { "Original", "Fit", "Stretch" };
  static const char *const picture_help[] = { "The GBA's own pixels, with a border.",
                                              "Fills the width. Bars above and below.",
                                              "Fills the screen. A little taller." };
  u32 picture_pick = 0;

  void picture_sync()
  {
    picture_pick = screen_scale == unscaled ? 0 : screen_scale == fullscreen ? 2 : 1;
  }

  void picture_apply()
  {
    screen_scale = picture_pick == 0 ? unscaled : picture_pick == 2 ? fullscreen : scaled_aspect;
  }

  /* the volume's steps (until r19 the Sound page's, nspire_audio_page) */
  static char volume_text[NSPIRE_VOLUME_MAX + 1][8];
  u8 *volume_options[NSPIRE_VOLUME_MAX + 1];

  for(i = 0; i <= NSPIRE_VOLUME_MAX; i++)
  {
    snprintf(volume_text[i], sizeof(volume_text[i]), "%u/%u", (unsigned)i, (unsigned)NSPIRE_VOLUME_MAX);
    volume_options[i] = (u8 *)volume_text[i];
  }

  u8 *show_speed_options[] = { "Off", "On", "Detailed" };
  u8 *settings_for_plain[] = { "All games", "This game" };

  /* ---- Settings (docs/ui/r19.txt). r19, after nine pages of a few tiles each (too many menus, cluttered;
   * the design is one screen, the settings as tiles on it): ONE screen, under the top
   * row's SETTINGS: what a player changes, each a tile with its value, and the screens that are lists (Keys,
   * Bluetooth, Cheats). Everything else is one page behind its last tile, MORE. The last string of a setting is
   * the one sentence the band shows while it is picked: only where its name and value can't say what it
   * does. ---- */

  void submenu_apply()
  {
    nspire_emulator_options_apply();
  }

  menu_option_type about_options[] =
  {
    submenu_option(NULL, "Back", "Return to the main menu.", 13)
  };

  make_menu(about, submenu_none, NULL);

  /* More: one screenful (eleven tiles at most: nothing scrolls). What is left once every choice of speed is
   * made for the player (a choice that only decided speed has no tile and is fixed to the fast value): where
   * saves go, the link's device, a game's own settings and
   * keys, an older CX's LCD fix, and the things to do (restart, the games' manager, the speed test, About).
   * Gone with their tiles, the value fixed (nspire_config_fast, nspire_stubs.c): Frame skip (Auto), Flicker fix
   * (off), Compatibility (Fast), Transparency (on), Fast forward (no limit), the smoothed Fit. Gone with their
   * tiles, the value as the files have it (a new file: the default): Sound delay, Sound output, BT quality,
   * Turbo rate, CPU speed, Fast memory, Game clock, BIOS, Auto resume, Save on quit, Ask to delete, Link speed
   * (the three that turn themselves down after a freeze still do). */
  menu_option_type more_options[] =
  {
    /* (r19: back as a tile after a time fixed to Auto. Auto or Off; Manual, from an
     * older file, shows until it is changed and is never offered) */
    string_selection_option(NULL, "Frame skip", frameskip_options,
     (u32 *)(&current_frameskip_type), 3,
     "Auto: skips pictures when it runs slow.", 0),
    string_selection_option(NULL, "Save to", slot_options,
     &nspire_auto_increment, 2, "Where the save key puts a save.", 0),
    string_selection_option(NULL, "Autosave to", slot_options,
     &nspire_autosave_new_slot, 2, "", 1),
    /* r16: where Save puts a state (a USB drive: nspire_places.h); shown while a drive can hold them */
    string_selection_option(NULL, "Saves on", saves_on_options,
     &nspire_saves_on, 2, "", 2),
    string_selection_option(nspire_link_device_changed, "Link device", link_device_options,
     (u32 *)&nspire_link_device, LINK_DEVICES,
     "Auto picks what the game uses.", 3),
    string_selection_option(nspire_config_settings_for_changed, "Settings for",
     settings_for_plain, &nspire_settings_for, 2, "", 4),
    string_selection_option(nspire_config_keys_for_changed, "Keys for",
     settings_for_plain, &nspire_keys_for, 2, "", 5),
    string_selection_option(NULL, "LCD fix",
     off_on_options, &nspire_lcd_cache_clean_full, 2,
     "Try it if the picture is garbled.", 6),
    action_option(menu_restart, NULL, "Game|Restart", "", 7),
    /* (r19: the save manager was hard to find. It is S on a game in GAMES and the paused
     * game's SAVES; neither says "manage". This tile opens the paused game's saves as a page: mark, delete,
     * pack, copy to the drive.) */
    action_option(menu_save_manager, NULL, "Saves|Manage", "", 8),
    action_option(menu_rom_manager, NULL, "Games|Manage", "", 8),
    submenu_option(&about_menu, "About", "", 10),
  };

  make_menu(more, submenu_apply, NULL);

  /* Settings: the one screen, eleven tiles at most (four rows of three: nothing scrolls). Volume first (0 is
   * off). Bluetooth where the build has it (nspire_bluetooth.h), Link with a game, Cheats with codes. */
  menu_option_type settings_options[] =
  {
    string_selection_option(NULL, "Volume", volume_options,
     (u32 *)&nspire_audio_volume, NSPIRE_VOLUME_MAX + 1, "", 0),
    string_selection_option(nspire_brightness_apply, "Brightness",
     brightness_options, &nspire_brightness, BRIGHTNESS_LEVELS + 1, "", 1),
    string_selection_option(picture_apply, "Screen size", picture_options,
     &picture_pick, 3, "", 2),
    string_selection_option(NULL, "FPS counter", show_speed_options,
     &nspire_fps_overlay, 3, "", 3),
    string_selection_option(NULL, "Autosave", autosave_options,
     &nspire_autosave, AUTOSAVE_CHOICES, "", 4),
    action_option(menu_controls, NULL, "Keys", "", 5),
    /* (the USB port's messages send the player here: "USB problem: Settings > Controller") */
    action_option(menu_controller_status, NULL, "Controller", "", 6),
    action_option(menu_bluetooth, NULL, "Bluetooth", "", 7),
    action_option(menu_link, NULL, "Link|Off", "", 8),   /* (its band: link_help_now, as the link stands) */
    action_option(menu_cheats, NULL, "Cheats", "", 9),
    submenu_option(&more_menu, "More", "", 10),
  };

  make_menu(settings, submenu_apply, NULL);

  /* r19: the top row: a word in each corner (Settings, Quit) and the selector between them:
   * Games, and with a game paused Saves before it (its saves: Resume, Save and each state as tiles). It
   * replaces a row of seven tabs, which looked cluttered; folders have no tab or menu of their own
   * (the folders are items of the games' grid). */
  menu_option_type main_options[] =
  {
    submenu_option(&settings_menu, "Settings", "", 0),
    action_option(menu_saves, NULL, "Saves", "", 1),
    action_option(menu_load, NULL, "Games", "", 2),
    action_option(menu_quit, NULL, "Quit", "", 3)
  };

  make_menu(main, submenu_none, NULL);

#if defined(NSPIRE_LIBRETRO)
  /* ---- The new look (nspire_ui_screens.c) ---- */

  static struct ui_menu_view view, last_view;
  static char last_status[80];

  /* The pages: their names (what the PC tests see, "More:3"); -1 for the top row. Settings is the one under the
   * top row itself (drawn with the row, not a path). */
  static const char *const page_titles[] = { "Settings", "More", "About" };
  int page_of(menu_type *m)
  {
    return m == &settings_menu ? 0 : m == &more_menu ? 1 : m == &about_menu ? 2 : -1;
  }
  /* The page a page is opened from (esc goes back to it): About from More, the others from Settings. */
  menu_type *page_parent(menu_type *m)
  {
    return m == &about_menu ? &more_menu : &settings_menu;
  }

  /* The tile of Settings the arrows were last on (settings_options' index), kept while the menu is open */
  static u32 settings_pick;

  /* The top row with SETTINGS picked, the arrows on the row (its tiles stay under it, none picked). */
  void settings_to_row()
  {
    u32 n;
    if(current_menu == &settings_menu)
      settings_pick = current_option_num;
    for(n = 0; n < main_menu.num_options; n++)
      if(main_menu.options[n].sub_menu == &settings_menu)
        nav_to(n);
  }

  /* Down or enter on SETTINGS: the arrows into its tiles, on the one they were last on. */
  void settings_open()
  {
    choose_menu(&settings_menu);
    if(settings_pick < settings_menu.num_options && !option_hidden(settings_options + settings_pick))
    {
      current_option_num = settings_pick;
      current_option = settings_options + settings_pick;
    }
  }

  /* Back from a page: to the tile that opened it. */
  void back_to(menu_type *menu, menu_type *opened)
  {
    u32 n;
    choose_menu(menu);
    for(n = 0; n < menu->num_options; n++)
      if(menu->options[n].sub_menu == opened)
      {
        current_option_num = n;
        current_option = menu->options + n;
      }
  }

  /* Lines the pages leave out: "Back" (esc goes back) and cheats that
   * aren't loaded. */
  int option_hidden(menu_option_type *o)
  {
    if((o->option_type & SUBMENU_OPTION) && !o->sub_menu)
      return 1;
    /* without a game (one that didn't start): nothing to restart, no saves of its own */
    if(first_load && (o->action_function == menu_restart || o->action_function == menu_saves))
      return 1;
    /* the main menu without a game (one that didn't start): no game to go
     * back to, save, load, trade in or cheat; and "Choose a game" only then */
    /* Cheats: a game whose cheat file was found (nspire_cheats.c; one with no code the engine runs too: its
     * tile says so, or the player is left wondering why the file does nothing) */
    if(o->action_function == menu_cheats && (first_load || !(nspire_cheats_count() || nspire_cheats_skipped())))
      return 1;
    /* r16: what other work brings, shown once it is there: Bluetooth's page, the drive's saves */
    if(o->action_function == menu_bluetooth && !nspire_bt)
      return 1;
    if(o->current_option == &nspire_saves_on && !nspire_places_ready())
      return 1;
    /* without a game: nothing to link */
    if(first_load && o->action_function == menu_link)
      return 1;
    /* (r19) where the autosave goes, only while there is one; the older CX's LCD fix, only there */
    if(o->current_option == &nspire_autosave_new_slot && !nspire_autosave)
      return 1;
    if(o->current_option == &nspire_lcd_cache_clean_full && is_cx2)
      return 1;
    return 0;
  }

  /* The next line up or down that's shown (n itself if none is). */
  u32 next_visible(u32 n, int step)
  {
    u32 tries, count = current_menu->num_options;
    for(tries = 0; tries < count; tries++)
    {
      n = (n + count + step) % count;
      if(!option_hidden(current_menu->options + n))
        return n;
    }
    return n;
  }

  /* "Display scaling: %s" -> "Display scaling"; values start upper case. */
  void row_label(const char *display, char *out, size_t size)
  {
    const char *colon = strstr(display, ": %"), *bar = strchr(display, '|');
    const char *end = colon ? colon : bar ? bar : display + strlen(display);
    snprintf(out, size, "%.*s", (int)(end - display), display);
  }

  /* r19: the top row as the list draws it (list_draw): this menu's tabs, Games' place among them. */
  void nav_publish()
  {
    u32 n;
    nav_count = 0;
    nav_games = 0;
    nav_saves = -1;
    for(n = 0; n < main_menu.num_options && nav_count < 8; n++)
    {
      menu_option_type *o = main_menu.options + n;
      if(option_hidden(o))
        continue;
      if(o->action_function == menu_load)
        nav_games = nav_count;
      if(o->action_function == menu_saves)
        nav_saves = nav_count;
      row_label((const char *)o->display_string, nav_text[nav_count], sizeof(nav_text[0]));
      nav_names[nav_count] = nav_text[nav_count];
      nav_count++;
    }
  }

  /* The top row's tab n (main_options' index) picked, the arrows on the row. */
  void nav_to(u32 n)
  {
    current_menu = &main_menu;
    current_option_num = n;
    current_option = main_menu.options + n;
    editing = 0;
    menu_redraw = 1;
  }

  u32 tab_of(void (*action)())
  {
    u32 n;
    for(n = 0; n < main_menu.num_options; n++)
      if(main_menu.options[n].action_function == action)
        return n;
    return 0;
  }

  /* r19: the list (Games, Folders) was left without a game picked: by an arrow past its two tabs, to the tab
   * before or after them; by esc, as esc on any tab (tabs_esc). */
  auto void tabs_esc();
  void list_left()
  {
    current_menu = &main_menu;
    if(nspire_list_nav_exit < 0)
    {
      current_option_num = tab_of(menu_load);
      nav_to(next_visible(current_option_num, -1));
    }
    else if(nspire_list_nav_exit > 0)
    {
      current_option_num = tab_of(menu_load);
      nav_to(next_visible(current_option_num, 1));
    }
    else
    {
      nav_to(tab_of(menu_load));
      tabs_esc();
    }
    nspire_list_nav_exit = 0;
    nspire_list_left_by_esc = 0;
  }

  /* esc (or the menu key) with the arrows on the tabs: back to the game; without a game there is nothing to go
   * back to: the block goes to Quit (enter there leaves gpSP; no key leaves it by itself). */
  void tabs_esc()
  {
    if(first_load)
      nav_to(tab_of(menu_quit));
    else
      menu_exit();
  }

  /* A two-way line (off/on): enter flips it. Blending's is the other way
   * round ("On", "Off (fast)": its variable says it is off). */
  int option_is_switch(menu_option_type *o)
  {
    const char *const *names = (const char *const *)o->options;
    return (o->option_type & STRING_SELECTION_OPTION) && o->num_options == 2 &&
     ((!strcmp(names[0], "off") && !strcmp(names[1], "on")) ||
      (!strcmp(names[0], "disabled") && !strcmp(names[1], "enabled")) ||
      (!strcmp(names[0], "no") && !strcmp(names[1], "yes")) ||
      (!strcmp(names[0], "On") && !strncmp(names[1], "Off", 3)));
  }

  /* Whether a switch is on: its second value, or its first when that is "On". */
  int switch_is_on(menu_option_type *o)
  {
    const char *const *names = (const char *const *)o->options;
    return (*o->current_option == 1) != !strcmp(names[0], "On");
  }

  /* A line's value as it is now; row_value_of: as it is with the value n (ui18: every value a tile can show is
   * measured, tiles_fit) */
  auto void row_value_of(menu_option_type *o, struct ui_row *row, u32 n);
  void row_value(menu_option_type *o, struct ui_row *row)
  {
    row_value_of(o, row, o->current_option ? *o->current_option : 0);
  }

  void row_value_of(menu_option_type *o, struct ui_row *row, u32 n)
  {
    const char *bar = strchr((const char *)o->display_string, '|');
    /* a thing to do or a page: what enter does, after the name's '|' */
    if(bar)
      snprintf(row->value, sizeof(row->value), "%s", bar + 1);
    /* Cheats: how many of its codes are on */
    if(o->action_function == menu_cheats)
    {
      if(nspire_cheats_count())
        snprintf(row->value, sizeof(row->value), "%d on", nspire_cheats_on_count());
      else
        snprintf(row->value, sizeof(row->value), "None usable");
    }
    /* r19: Link's tile says the link's state now (enter links or unlinks) */
    if(o->action_function == menu_link)
    {
      enum link_state ls = nspire_link_state();
      const char *st = nspire_link_status(), *player = st ? strstr(st, "player ") : NULL;
      /* (a third of the width at most: the band says the rest, link_help_now) */
      snprintf(row->value, sizeof(row->value), "%s", ls == LINK_LINKED ? "Linked" : ls == LINK_LOOKING ? "Waiting" :
       nspire_usbpad_name() ? "Controller in" : nspire_link_swap_hint() ? "Swap ends" :
       nspire_link_cable_present() ? "Ready" : "No cable");
      /* (linked: which player this one is) */
      if(ls == LINK_LINKED && player && player[7] >= '1' && player[7] <= '4')
        snprintf(row->value, sizeof(row->value), "Player %c", player[7]);
    }
    if(o->option_type & NUMBER_SELECTION_OPTION)
    {
      row->kind = UI_ROW_PICK;
      if(o->current_option == &frameskip_value)
      {
        /* what the number does depends on the frame skip: the most in a row, or so many of each n + 1 */
        if(!n)
          snprintf(row->value, sizeof(row->value), "None");
        else if(current_frameskip_type == auto_frameskip)
          snprintf(row->value, sizeof(row->value), "%u in a row", n);
        else if(current_frameskip_type == manual_frameskip)
          snprintf(row->value, sizeof(row->value), "%u of every %u", n, n + 1);
        else
          snprintf(row->value, sizeof(row->value), "%u (skip off)", n);
      }
      else if(o->current_option == &nspire_rom_buffer_size_choice)
        snprintf(row->value, sizeof(row->value), "%u MB", n < 2 ? 2 : n);
      else
        snprintf(row->value, sizeof(row->value), "%u", n);
    }
    else if(o->option_type & STRING_SELECTION_OPTION)
    {
      const char *const *names = (const char *const *)o->options;
      u32 picked = n < o->num_options ? n : 0;
      if(option_is_switch(o))
      {
        row->kind = UI_ROW_SWITCH;
        row->on = (picked == 1) != !strcmp(names[0], "On");   /* (switch_is_on, for the value n) */
        return;
      }
      row->kind = UI_ROW_PICK;
      if(o->current_option == &nspire_autosave)
      {
        /* "every 2 min" -> "2 min", "every minute" -> "1 min" */
        const char *t = names[picked];
        if(!strncmp(t, "every ", 6))
          t += 6;
        snprintf(row->value, sizeof(row->value), "%s", !strcmp(t, "minute") ? "1 min" : t);
      }
      else
        snprintf(row->value, sizeof(row->value), "%s", names[picked]);
      row->value[0] = (char)toupper((unsigned char)row->value[0]);
    }
    else if(o->option_type & SUBMENU_OPTION)
      row->kind = UI_ROW_SUBMENU;
    else if(o->action_function == menu_controls || o->action_function == menu_cheats ||
     o->action_function == menu_controller_status ||
     o->action_function == menu_rom_manager || o->action_function == menu_bluetooth)
      row->kind = UI_ROW_SUBMENU;   /* a screen of its own: the same mark as a page */
    else
      row->kind = UI_ROW_ACTION;
  }

  /* What is under the picked word of the row, when menu() draws it: Quit says what it does to the game (SETTINGS:
   * its tiles, draw_menu_ui; SAVES and GAMES draw themselves). */
  void main_picture(menu_option_type *o)
  {
    view.help = NULL;
    if(!first_load && o->action_function == menu_quit)
      view.help = nspire_save_on_exit ? "The game is saved first." : "Save on quit is off: save first.";
  }

  /* ui18: how many columns each of a page's tiles fits (3, 2 or 1: a row of its own) with every value it can
   * show, measured once a page (fit_for: the page they are for; choose_menu forgets it) */
  static u8 option_fit[UI_MAX_ROWS];
  static menu_type *fit_for;
  static u32 view_option[UI_MAX_ROWS];   /* the option each of view.rows is */
  int option_tile_fit(menu_option_type *o)
  {
    struct ui_row r;
    char label[48];
    u32 n, count = (o->option_type & STRING_SELECTION_OPTION) ? o->num_options : 1;
    int fit = 3, f;
    row_label((const char *)o->display_string, label, sizeof(label));
    if(o->current_option == &frameskip_value)
    {
      /* its words depend on the frame skip: each form at its longest */
      static const char *const forms[] = { "99 in a row", "99 of every 100", "99 (skip off)" };
      for(n = 0; n < 3; n++)
      {
        f = ui_tile_fit(label, forms[n], 1);
        fit = f < fit ? f : fit;
      }
      return fit;
    }
    for(n = 0; n < count; n++)
    {
      memset(&r, 0, sizeof(r));
      /* a number's longest is its largest */
      row_value_of(o, &r, (o->option_type & NUMBER_SELECTION_OPTION) ? o->num_options - 1 : n);
      /* (the chevrons' room: only a value changed between them, a range) */
      f = ui_tile_fit(label, r.kind == UI_ROW_SWITCH ? (r.on ? "On" : "Off") : r.value,
                      r.kind == UI_ROW_PICK && count > CYCLE_MAX);
      fit = f < fit ? f : fit;
    }
    return fit;
  }

  /* r19: Settings > Link's band, as the link stands: what enter does now, what to do first, or why the link
   * stopped (one line of 40 characters: until r19 a sentence of twice that, the same whatever the tile said). */
  const char *link_help_now()
  {
    enum link_state ls = nspire_link_state();
    const char *st = nspire_link_status();
    if(ls == LINK_LINKED)
      return "Enter unlinks.";
    if(ls == LINK_LOOKING)
      return "Waiting: pick Link on the other one.";
    if(nspire_usbpad_name())
      return "A controller is in the port.";
    if(nspire_link_swap_hint())
      return "Turn the cable round: swap its ends.";
    if(st && strcmp(st, "Not linked") && strlen(st) <= 38)
      return st;
    if(nspire_link_cable_present())
      return "Enter links. Pick Link on both.";
    return "Plug in the cable, pick Link on both.";
  }

  /* The band of the sound's tiles: what plays the sound now. Bluetooth headphones taking it say so (the adapter's
   * state otherwise); both playing (Output Auto), both named, the headphones' name cut to the band. (Until r19
   * the Sound page's.) */
  const char *sound_help_now()
  {
    static char text[64];
    const char *bt = nspire_bt_glue_audio_summary(), *name = nspire_bt_glue_headphones_name();
    if(name && nspire_audio_output == NSPIRE_OUTPUT_AUTO && nspire_usbaudio_streaming())
      snprintf(text, sizeof(text), "USB + Bluetooth: %.21s", name);
    else
      snprintf(text, sizeof(text), "%.63s", bt && nspire_audio_output != NSPIRE_OUTPUT_USB ? bt : nspire_audio_summary());
    return text;
  }

  /* Draws the menu when something on it changed. */
  void draw_menu_ui()
  {
    static char title[160], tags[96], last_help[64];
    /* the top row with SETTINGS picked: its tiles are under it already, none picked (down or enter: into them) */
    int on_row = current_menu == &main_menu && current_option->sub_menu == &settings_menu;
    menu_type *shown = on_row ? &settings_menu : current_menu;
    int page = page_of(shown);
    u32 n, last_line = 0;

    picture_sync();
    view.page = page < 0 ? -1 : 0;
    view.tabs = page < 0 ? NULL : &page_titles[page];
    view.tab_count = 0;
    view.count = 0;
    view.picked = 0;
    view.status = menu_status;
    view.editing = editing;
    view.nav_focus = 1;   /* (the top row's own screen, Quit's: the arrows are on the row) */
    view.nav = NULL;
    view.nav_count = view.nav_on = 0;
    if(shown == &settings_menu)
    {
      /* Settings: the screen under the top row, drawn with the row (SETTINGS its first word) */
      if(!first_load)
        nspire_cheats_look();
      nav_publish();
      view.nav = nav_names;
      view.nav_count = nav_count;
      view.nav_focus = on_row;
      /* (a file from before r19 with the Sound switch off: the volume is 0) */
      if(!nspire_audio_enabled)
      {
        nspire_audio_volume = 0;
        nspire_audio_enabled = 1;
      }
    }
    view.about = current_menu == &about_menu;
    view.no_game = first_load;
    view.picture = NULL;
    view.pic_empty = NULL;
    view.pic_head[0] = view.pic_sub[0] = 0;
    view.help = on_row ? "" : (const char *)current_option->help_string;
    if(current_option->action_function == menu_link)
      view.help = link_help_now();
    if(!on_row && current_option->current_option == (u32 *)&nspire_audio_volume)
      view.help = sound_help_now();
    if(!on_row && current_option->current_option == &picture_pick)
      view.help = picture_help[picture_pick % 3];
    if(first_load)
      snprintf(title, sizeof(title), "gpSP");
    else
      ui_game_title(gamepak_filename, title, sizeof(title), tags, sizeof(tags));
    view.title = view.page >= 0 ? view.tabs[0] : title;
    /* a page says where it is, as a path: "SETTINGS / PICTURE", "SETTINGS / SYSTEM / ABOUT" */
    view.from = page <= 0 ? NULL : current_menu == &about_menu ? "Settings/More" : "Settings";

    memset(view.rows, 0, sizeof(view.rows));
    for(n = 0; n < shown->num_options && view.count < UI_MAX_ROWS; n++)
    {
      menu_option_type *o = shown->options + n;
      struct ui_row *row = &view.rows[view.count];

      if(option_hidden(o))
        continue;
      if(o == current_option)
        view.picked = view.count;
      view_option[view.count] = n;
      if(view.count && o->line_number > last_line + 1 && view.page < 0)
        row->gap_before = 1;
      last_line = o->line_number;

      if(view.page < 0)
      {
        /* the top row's tabs: their names */
        row_label((const char *)o->display_string, row->label, sizeof(row->label));
        row->kind = UI_ROW_ACTION;
      }
      else
      {
        row_label((const char *)o->display_string, row->label, sizeof(row->label));
        row_value(o, row);
      }
      view.count++;
    }

    /* ui18: a page's tiles: three columns when every one fits a third of the width with every value it can
     * show, else two; one that doesn't fit half takes a row of its own */
    view.cols = 0;
    if(view.page >= 0)
    {
      if(fit_for != shown)
      {
        fit_for = shown;
        for(n = 0; n < shown->num_options && n < UI_MAX_ROWS; n++)
          option_fit[n] = (u8)option_tile_fit(shown->options + n);
      }
      view.cols = 3;
      for(n = 0; n < (u32)view.count; n++)
        if(option_fit[view_option[n]] < 3)
          view.cols = 2;
      for(n = 0; n < (u32)view.count; n++)
        view.rows[n].tile = option_fit[view_option[n]] < 2 ? UI_TILE_WIDE : 0;
    }

    if(on_row)
      view.picked = -1;
    if(view.page < 0)
      main_picture(current_option);

    /* A picked cell whose name or value is too long for it scrolls in it, counted from when it (or its value)
     * last changed, in 30ths of a second (how far it moves at a time). Nothing else on a page moves: while no
     * cell is too long the time stays 0 and an unchanged page isn't drawn again. */
    {
      static menu_option_type *scroll_option;
      static u32 scroll_value, scroll_at;
      static int scroll_editing;
      u32 now = nspire_ticks(), value = current_option->current_option ? *current_option->current_option : 0;
      if(scroll_option != current_option || scroll_value != value || scroll_editing != editing || menu_redraw)
      {
        scroll_option = current_option;
        scroll_value = value;
        scroll_editing = editing;
        scroll_at = now;
      }
      view.picked_ms = ui_menu_scrolls ?
       (unsigned)((now - scroll_at) * 30 / NSPIRE_TICK_HZ) * 1000 / 30 : 0;
    }

    if(!menu_redraw && !memcmp(&view, &last_view, sizeof(view)) && !strcmp(menu_status, last_status) &&
     !strcmp(view.help ? view.help : "", last_help))
      return;
    ui_menu_draw(&view);
    last_view = view;
    snprintf(last_status, sizeof(last_status), "%s", menu_status);
    snprintf(last_help, sizeof(last_help), "%.63s", view.help ? view.help : "");
    menu_redraw = 0;
  }
#endif

  void choose_menu(menu_type *new_menu)
  {
    if(new_menu == NULL)
      new_menu = &main_menu;

    current_menu = new_menu;
    current_option = new_menu->options;
    current_option_num = 0;
    editing = 0;
#if defined(NSPIRE_LIBRETRO)
    fit_for = NULL;   /* (ui18: its tiles measured again: values can differ from one opening to the next) */
#endif
    if(option_hidden(current_option))
    {
      current_option_num = next_visible(0, 1);
      current_option = new_menu->options + current_option_num;
    }
    menu_redraw = 1;
    if(current_menu->init_function)
     current_menu->init_function();
  }

#if defined(NSPIRE_LIBRETRO)
  /* The menu's options call the functions above through trampolines, code
   * GCC wrote onto this frame of the stack as data on the way in. The menu
   * opens from more than one stack depth, so they must reach memory and
   * the instruction cache can't keep older ones. */
  nspire_sync_written_code();
#endif

  menu_update_clock();
  video_resolution_large();

  if(gamepak_filename[0] == 0)
    first_load = 1;
  /* The paused game as it stands: Resume's picture (ui_backdrop_picture). */
  if(first_load)
    ui_backdrop_clear();
  else
    ui_backdrop_make(original_screen, 240);

  choose_menu(&main_menu);
  nspire_save_pick(-1);
  states_yield = keys_down_now;   /* a key stops the look-ups of files */
  /* r16: saves go where Saving's "Saves on" says, while that place can hold them */
  place = nspire_saves_on == PLACE_DRIVE && nspire_places_ready() ? PLACE_DRIVE : PLACE_CALCULATOR;
  settings_pick = 0;   /* Settings' tiles: the arrows go to the first (Volume) */
  if(first_load)
  {
    /* r19: opened from the games' grid without a game (an arrow left GAMES, or esc on the row): the word the
     * arrows went to; else GAMES, the arrows on the row */
    if(nspire_list_nav_exit || nspire_list_left_by_esc)
      list_left();
    else
      nav_to(tab_of(menu_load));
  }
  else
  {
    /* a game paused: its saves, the arrows on their first tile, Resume (enter goes back to the game) */
    nav_to(tab_of(menu_saves));
    saves_nav = 0;
  }

  while(repeat)
    {
      /* r19: SAVES and GAMES draw themselves under the same row and take the arrows until they leave their word,
       * something is picked or esc is pressed (nspire_saves_tab, load_file) */
      if(current_menu == &main_menu && (current_option->action_function == menu_load ||
       current_option->action_function == menu_saves))
      {
        current_option->action_function();
        continue;
      }
      current_menu->init_function();
      /* (r19: the picked tile isn't shown any more, Saving's Saves on with the drive pulled: the next
       * one that is; enter changed the hidden one) */
      if(page_of(current_menu) >= 0 && current_menu != &about_menu && option_hidden(current_option))
      {
        current_option_num = next_visible(current_option_num, 1);
        current_option = current_menu->options + current_option_num;
        editing = 0;
      }
      draw_menu_ui();
      if(opened_at)
      {
        states_menu_note("paused menu opened (its picture, the first draw)", nspire_ticks() - opened_at);
        states_memory_look("the paused menu", 0);   /* (save17: the memory free, every 10 s at most) */
        opened_at = 0;
      }
      update_at_vblank();

      gui_action = get_gui_input();
      if(gui_action != CURSOR_NONE)
        menu_status[0] = 0;
      /* esc goes back, like the menu key. */
      if(gui_action == CURSOR_BACK)
        gui_action = CURSOR_EXIT;

      if(page_of(current_menu) >= 0)
      {
        int pick = (current_option->option_type & (NUMBER_SELECTION_OPTION |
         STRING_SELECTION_OPTION)) != 0;

        if(!editing && gui_action == CURSOR_SELECT && pick)
        {
          /* r19: a setting of a few values: enter is the next one (a switch flips); a range is changed
           * between chevrons, by the arrows */
          if((current_option->option_type & STRING_SELECTION_OPTION) && current_option->num_options <= CYCLE_MAX)
          {
            u32 count = current_option->num_options, to = (*current_option->current_option + 1) % count;
            /* (manual frame skip isn't offered: a file that has it keeps it until it is changed) */
            if(current_option->current_option == (u32 *)&current_frameskip_type && to == manual_frameskip)
              to = (to + 1) % count;
            *current_option->current_option = to;
            if(current_option->passive_function)
              current_option->passive_function();
            continue;
          }
          editing = 1;
          edit_from = *current_option->current_option;
          continue;
        }
        if(editing)
        {
          if(gui_action == CURSOR_EXIT)
          {
            /* esc: a value goes back to what it was */
            if(*current_option->current_option != edit_from)
            {
              *current_option->current_option = edit_from;
              if(current_option->passive_function)
                current_option->passive_function();
            }
            editing = 0;
            continue;
          }
          if(gui_action == CURSOR_SELECT)
          {
            editing = 0;
            continue;
          }
          /* up/down keep the value and move on */
          if(gui_action == CURSOR_UP || gui_action == CURSOR_DOWN)
            editing = 0;
        }
      }

      if(current_menu == &main_menu)
      {
        int on_settings = current_option->sub_menu == &settings_menu;

        /* the arrows on the top row: left and right along it; down or enter into SETTINGS' tiles (they are under
         * it already); enter on Quit quits; esc back to the game (without one: to Quit) */
        switch(gui_action)
        {
          case CURSOR_RIGHT:
          case CURSOR_LEFT:
            current_option_num = next_visible(current_option_num, gui_action == CURSOR_RIGHT ? 1 : -1);
            current_option = current_menu->options + current_option_num;
            break;

          case CURSOR_DOWN:
            if(on_settings)
              settings_open();
            break;

          case CURSOR_EXIT:
            tabs_esc();
            break;

          case CURSOR_SELECT:
            menu_redraw = 1;
            if(on_settings)
              settings_open();
            else if(current_option->option_type & ACTION_OPTION)
              current_option->action_function();
            break;

          default:
            break;
        }
        continue;
      }

    switch(gui_action)
    {
      /* ui18: a page is tiles: the arrows move among them (nspire_ui_screens.h, ui_tile_step); enter starts
       * changing the picked value (a switch flips), left and right then change it, enter keeps it, esc puts it
       * back (r16: a list, up and down moved, left and right changed the value at once) */
      case CURSOR_DOWN:
      case CURSOR_UP:
      case CURSOR_RIGHT:
      case CURSOR_LEFT:
        if(!editing || gui_action == CURSOR_UP || gui_action == CURSOR_DOWN)
        {
          u8 flags[UI_MAX_ROWS];
          int k, to;
          for(k = 0; k < view.count; k++)
            flags[k] = view.rows[k].tile;
          /* Settings, the screen under the top row: up from its first row of tiles is the row */
          if(current_menu == &settings_menu && gui_action == CURSOR_UP && view.picked >= 0 &&
           view.picked < view.count)
          {
            u8 row[UI_MAX_ROWS], col[UI_MAX_ROWS], span[UI_MAX_ROWS];
            ui_tiles_place(flags, view.count, view.cols, row, col, span);
            if(row[view.picked] == 0)
            {
              settings_to_row();
              break;
            }
          }
          to = ui_tile_step(flags, view.count, view.cols, view.picked,
                            gui_action == CURSOR_LEFT ? -1 : gui_action == CURSOR_RIGHT ? 1 : 0,
                            gui_action == CURSOR_UP ? -1 : gui_action == CURSOR_DOWN ? 1 : 0);
          if(to >= 0 && to < view.count)
          {
            current_option_num = view_option[to];
            current_option = current_menu->options + current_option_num;
          }
          break;
        }
        if(current_option->option_type & (NUMBER_SELECTION_OPTION |
         STRING_SELECTION_OPTION))
        {
          u32 count = current_option->num_options;
          u32 step = 1;
          /* Long lists (the save slots) move 10 at a time while held. */
          if(count >= 100 && gui_repeat_count() > 20)
            step = 10;
          /* r19: a range stops at its ends (its first value doesn't come after its last) */
          u32 now = *current_option->current_option;
          if(gui_action == CURSOR_RIGHT)
            now = now + step < count ? now + step : count - 1;
          else
            now = now >= step ? now - step : 0;
          *(current_option->current_option) = now;

          if(current_option->passive_function)
            current_option->passive_function();
        }
        break;

      case CURSOR_EXIT:
        /* back from a page: to the tile that opened it (About from More, More and Cheats from Settings);
         * from Settings' tiles: to the top row */
        if(current_menu == &settings_menu)
        {
          settings_to_row();
          break;
        }
        if(page_of(current_menu) >= 0)
        {
          back_to(page_parent(current_menu), current_menu);
          break;
        }
        choose_menu(&main_menu);
        break;

      case CURSOR_SELECT:
        /* what the line does may change any of it (a save, another
         * screen in between) */
        /* (r19: a page whose only line isn't shown, About: enter does nothing; it left for the top row) */
        if(option_hidden(current_option))
          break;
        menu_redraw = 1;
        if(current_option->option_type & ACTION_OPTION)
          current_option->action_function();

        if(current_option->option_type & SUBMENU_OPTION)
          choose_menu(current_option->sub_menu);
        break;

      default:
        break;
    }
  }

  ui_shown[0] = 0;
  nspire_save_pick(-1);
  /* (usbsave8) left on the drive's tab: the picked slot is the calculator's again, as tab makes
   * it. The game's keys (load, save without New slot per save) go by it, and they are the
   * calculator's: a drive's slot number there would load or replace another state. */
  if(place == PLACE_DRIVE)
    savestate_slot = states_newest() >= 0 ? (u32)states_newest() : 0;
  set_gba_resolution(screen_scale);
  video_resolution_small();
  menu_get_clock_speed();
  set_clock_speed();
  save_config_file();

  return return_value;
}
