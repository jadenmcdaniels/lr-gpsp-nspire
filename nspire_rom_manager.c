/* The games manager (Settings > More > Games: Manage; "ROM manager" in the code), next to the save manager: the
 * games in the folder of the game that's running (without one, the file
 * list's folder), their sizes, compressed or not. enter marks games (A:
 * all); C compresses the marked ones, or else the one picked, into .gbz
 * files (what gpSP reads with the least memory: a zip is unpacked whole),
 * U uncompresses them back into .gba files (nspire_rom_convert.h). Saves
 * keep working either way. The game that's running can be done too when its
 * whole ROM is in memory (else it still reads pieces of its file). */

#include "common.h"
#include "nspire.h"
#include "nspire_gui_video.h"
#include "nspire_frontend.h"
#include "nspire_game_list.h"
#include "nspire_keys.h"
#include "nspire_paths.h"
#include "nspire_rom_convert.h"
#include "nspire_states.h"
#include "nspire_ui_screens.h"
#include "nspire_dialog.h"

#include <dirent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define ROWS UI_MANAGER_ROWS
#define MAX_ROMS 256

struct rom_entry
{
  char name[256];   /* the file's name, "Game.zip.tns" */
  long bytes;
  int kind;
};

static struct rom_entry *roms;
static unsigned char marked[MAX_ROMS];
static int rom_count, selected, top;
static char folder[512], status[128];
static int last_percent;
static int working_percent = -1;   /* a game being done, or -1 */

static int is_rom_name(const char *name)
{
  static const char *const exts[] = { ".gba.tns", ".gbz.tns", ".zip.tns" };
  size_t len = strlen(name), i;
  for (i = 0; i < 3; i++)
    if (len > 8 && !strcasecmp(name + len - 8, exts[i]))
      return 1;
  return 0;
}

static void rom_path(const char *name, char *path, size_t size)
{
  size_t len = strlen(folder);
  snprintf(path, size, "%s%s%s", folder, len && folder[len - 1] == '/' ? "" : "/", name);
}

/* A reset or a flat battery in the middle of a conversion leaves its
 * "<name>.gbz.tns.tmp" or ".gba.tns.tmp" (up to 32 MB, hidden from every
 * list). No conversion runs when the manager opens: they go (ZIP-5). The
 * state and save code's own .tmp files are theirs, and stay. */
static void remove_leftovers(void)
{
  static const char *const tails[] = { ".gbz.tns.tmp", ".gba.tns.tmp" };
  struct dirent *entry;
  DIR *dir = opendir(folder);
  char path[800];

  while (dir && (entry = readdir(dir)) != NULL)
  {
    size_t len = strlen(entry->d_name), i;
    for (i = 0; i < 2; i++)
      if (len > 12 && !strcasecmp(entry->d_name + len - 12, tails[i]))
      {
        rom_path(entry->d_name, path, sizeof(path));
        remove(path);
      }
  }
  if (dir)
    closedir(dir);
}

/* The folder's games, in the file list's order. */
static void refresh(void)
{
  static char *names[MAX_ROMS];
  struct rom_entry *sorted;
  struct dirent *entry;
  DIR *dir;
  int i, j;

  rom_count = 0;
  dir = opendir(folder);
  while (dir && (entry = readdir(dir)) != NULL && rom_count < MAX_ROMS)
    if (is_rom_name(entry->d_name))
      snprintf(roms[rom_count++].name, sizeof(roms[0].name), "%s", entry->d_name);
  if (dir)
    closedir(dir);

  for (i = 0; i < rom_count; i++)
    names[i] = roms[i].name;
  game_list_sort(names, (unsigned)rom_count);
  game_list_apply_order(names, (unsigned)rom_count, folder);
  sorted = (struct rom_entry *)malloc(sizeof(*sorted) * (rom_count ? rom_count : 1));
  if (sorted)
  {
    for (i = 0; i < rom_count; i++)
      sorted[i] = *(struct rom_entry *)(names[i] - offsetof(struct rom_entry, name));
    memcpy(roms, sorted, sizeof(*sorted) * rom_count);
    free(sorted);
  }

  for (i = 0; i < rom_count; i++)
  {
    char path[800];
    struct stat st;
    rom_path(roms[i].name, path, sizeof(path));
    roms[i].bytes = stat(path, &st) == 0 ? (long)st.st_size : 0;
    roms[i].kind = rom_kind(path);
  }
  for (j = 0; j < MAX_ROMS; j++)
    marked[j] = 0;
  if (selected >= rom_count)
    selected = rom_count ? rom_count - 1 : 0;
}

static void size_text(long bytes, char *text, size_t size)
{
  if (bytes < 1024 * 1024)
    snprintf(text, size, "%ld KB", (bytes + 1023) / 1024);
  else
  {
    long tenths = (long)(((long long)bytes * 10 + 524288) / 1048576);
    snprintf(text, size, "%ld.%ld MB", tenths / 10, tenths % 10);
  }
}

static int marked_count(void)
{
  int i, n = 0;
  for (i = 0; i < rom_count; i++)
    n += marked[i];
  return n;
}

static const char *kind_text(int kind)
{
  return kind == ROM_ZIP ? "Zip" : kind == ROM_GBZ ? "GBZ" : kind == ROM_PLAIN ? "GBA" : "?";
}

/* The picked game's newest state's picture and how many states it has,
 * looked up once it has stayed picked for a sixth of a second. */
static u16 picture[UI_TILE_W * UI_TILE_H];
static char picture_for[256];
static int picture_has, picture_states;
static int waiting = -1;
static u32 picked_at;

static int look_up(int i)
{
  static u16 big[STATE_PIC_W * STATE_PIC_H];
  char path[800];
  int newest, states, has;

  if (!strcmp(picture_for, roms[i].name))
    return 1;
  if (i != waiting)
  {
    waiting = i;
    picked_at = nspire_ticks();
    return 0;
  }
  rom_path(roms[i].name, path, sizeof(path));
  /* one looked up before at once; files read once the line has rested */
  has = states_peek_soon(path, &states, &newest, big, NULL, nspire_ticks() - picked_at >= NSPIRE_TICK_HZ / 6);
  if (has < 0)
    return 0;
  picture_has = has;
  picture_states = states;
  if (picture_has)
    ui_scale(picture, UI_TILE_W, UI_TILE_H, big, STATE_PIC_W, STATE_PIC_H, STATE_PIC_W);
  snprintf(picture_for, sizeof(picture_for), "%s", roms[i].name);
  return 1;
}

static int scroll_for = -1;   /* the picked line whose name scrolls, and since when */
static u32 scroll_at;

static void draw(void)
{
  static const char *const actions[] = { "C", "Make it a .gbz", "U", "Make it a .gba", NULL };
  static struct ui_manager_view v;
  char bytes[24], tags[96];
  long total = 0;
  int i, row, marked_n = marked_count();

  memset(&v, 0, sizeof(v));
  v.title = "Settings/More/Games";
  if (selected != scroll_for)
  {
    scroll_for = selected;
    scroll_at = nspire_ticks();
  }
  v.picked_ms = (unsigned)((u64)(nspire_ticks() - scroll_at) * 1000 / NSPIRE_TICK_HZ);   /* u64: 32 bits are 131 s of ticks * 1000 */
  v.name = "roms";
  for (i = 0; i < rom_count; i++)
    total += roms[i].bytes;
  size_text(total, bytes, sizeof(bytes));
  snprintf(v.subtitle, sizeof(v.subtitle), "%d %s, %s", rom_count, rom_count == 1 ? "game" : "games", bytes);
  v.count = rom_count;
  v.first = top;
  v.picked = selected;
  v.marked = marked_n;
  v.empty = "No games in this folder.";
  for (row = 0; row < ROWS && top + row < rom_count; row++)
  {
    ui_game_title(roms[top + row].name, v.rows[row].label, sizeof(v.rows[row].label), tags, sizeof(tags));
    size_text(roms[top + row].bytes, v.rows[row].size, sizeof(v.rows[row].size));
    v.rows[row].marked = marked[top + row];
  }

  if (marked_n)
  {
    long sum = 0;
    for (i = 0; i < rom_count; i++)
      if (marked[i])
        sum += roms[i].bytes;
    v.pic_empty = "";
    snprintf(v.head, sizeof(v.head), "%d marked", marked_n);
    size_text(sum, v.facts[0], sizeof(v.facts[0]));
  }
  else if (rom_count)
  {
    const struct rom_entry *r = &roms[selected];
    int ready = look_up(selected);
    v.picture = ready && picture_has ? picture : NULL;
    v.pic_empty = ready ? "No saved state" : "";
    ui_game_title(r->name, v.head, sizeof(v.head), tags, sizeof(tags));
    size_text(r->bytes, bytes, sizeof(bytes));
    snprintf(v.facts[0], sizeof(v.facts[0]), "%s %s", kind_text(r->kind), bytes);   /* "GBA 1 KB . 5 saves": 18 fit the panel */
    if (ready)
    {
      snprintf(v.facts[1], sizeof(v.facts[1]), "%d %s", picture_states, picture_states == 1 ? "save" : "saves");
    }
  }
  v.actions = actions;
  v.status = status;
  v.percent = working_percent;
  ui_manager_draw(&v);
}


/* The status line while a game is done: "Compressing 2/5: Name 37%". */
static const char *working_verb;
static int working_number, working_count;
static char working_name[40];

static int progress(unsigned long done, unsigned long total)
{
  int percent = total ? (int)((unsigned long long)done * 100 / total) : 0;
  if (percent == last_percent)
    return 0;
  last_percent = percent;
  snprintf(status, sizeof(status), "%s %d/%d: %s %d%%", working_verb, working_number, working_count,
           working_name, percent);
  working_percent = percent;
  draw();
  return 0;
}

static void act(int compress)
{
  static char names[MAX_ROMS][256];
  int targets = marked_count(), count = 0, done = 0, skipped = 0, failed = 0, kept_zips = 0, i;
  long before = 0, after = 0;
  const char *first_why = NULL;
  char was[24], now[24], keep[256];

  if (!rom_count)
    return;
  /* The game picked stays picked, under its new name. */
  snprintf(keep, sizeof(keep), "%s", roms[selected].name);
  /* What's to do: the marked games (or the one picked) that aren't that
   * way already. */
  for (i = 0; i < rom_count; i++)
  {
    if (targets ? !marked[i] : i != selected)
      continue;
    if (compress ? roms[i].kind == ROM_GBZ : roms[i].kind == ROM_PLAIN)
      skipped++;
    else
      snprintf(names[count++], sizeof(names[0]), "%s", roms[i].name);
  }
  if (!count)
  {
    snprintf(status, sizeof(status), compress ? "Nothing to compress: %s already."
                                              : "Nothing to uncompress: %s already.",
             skipped == 1 ? (compress ? "it's a .gbz" : "it's a .gba") : (compress ? "they're .gbz" : "they're .gba"));
    return;
  }

  working_verb = compress ? "Compressing" : "Uncompressing";
  working_count = count;
  for (i = 0; i < count; i++)
  {
    char path[800], new_path[512];
    const char *why, *new_name;
    struct stat st;
    int running;

    rom_path(names[i], path, sizeof(path));
    working_number = i + 1;
    snprintf(working_name, sizeof(working_name), "%.24s", names[i]);
    last_percent = -1;
    progress(0, 1);

    /* The running game's file can go only if it isn't read any more. */
    {
      char game[600];
      nspire_normalize_path(game, sizeof(game), gamepak_filename);
      running = gamepak_filename[0] && !strcmp(path, game);
    }
    if (running && !nspire_rom_file_release())
      why = "It's running and still reads its file.";
    else
    {
      long bytes = stat(path, &st) == 0 ? (long)st.st_size : 0;
      why = compress ? rom_compress(path, new_path, sizeof(new_path), progress)
                     : rom_uncompress(path, new_path, sizeof(new_path), progress);
      if (!why)
      {
        kept_zips += rom_convert_kept_original;
        before += bytes;
        after += stat(new_path, &st) == 0 ? (long)st.st_size : 0;
        new_name = strrchr(new_path, '/') ? strrchr(new_path, '/') + 1 : new_path;
        game_list_rename(folder, names[i], new_name);
        if (!strcmp(keep, names[i]))
          snprintf(keep, sizeof(keep), "%.255s", new_name);
        if (running)
        {
          snprintf(gamepak_filename, sizeof(gamepak_filename), "%s", new_path);
          states_open_game(gamepak_filename);
          nspire_backup_renamed();
        }
      }
    }
    if (why)
    {
      failed++;
      if (!first_why)
        first_why = why;
    }
    else
      done++;
  }

  working_percent = -1;
  picture_for[0] = 0;   /* new names */
  refresh();
  for (i = 0; i < rom_count; i++)
    if (!strcmp(roms[i].name, keep))
      selected = i;
  size_text(before, was, sizeof(was));
  size_text(after, now, sizeof(now));
  if (failed)
    snprintf(status, sizeof(status), "%s %d; %d not: %s", compress ? "Compressed" : "Uncompressed", done,
             failed, first_why);
  else
    snprintf(status, sizeof(status), "%s %d: %s before, %s now.%s", compress ? "Compressed" : "Uncompressed",
             done, was, now, kept_zips ? " The zip stays: more in it." : "");
}

/* C, U and A act on the ROMs: the keys bound to A and B (Enter and Esc
 * in menus) leave these alone here. */
static const unsigned char own_keys[] = { KEY_C, KEY_U, KEY_A, 0 };

void nspire_rom_manager(void)
{
  const char *slash;

  if (!strncmp(gamepak_filename, "usb:/", 5))
  {
    struct ui_dialog_view d = {0};
    d.title = "USB game";
    d.text = "USB games are read-only. Copy the game to the calculator to compress or change it.";
    d.first = "Back";
    nspire_ask(&d, NULL, NULL);
    return;
  }

  roms = (struct rom_entry *)malloc(sizeof(*roms) * MAX_ROMS);
  if (!roms)
    return;
  /* The folder in the form "/documents/gba" (the OS gives "\documents\gba\"). */
  if (gamepak_filename[0])
  {
    char game[512];
    nspire_normalize_path(game, sizeof(game), gamepak_filename);
    slash = strrchr(game, '/');
    snprintf(folder, sizeof(folder), "%.*s", !slash ? 1 : slash == game ? 1 : (int)(slash - game),
             slash ? game : ".");
  }
  else
  {
    char cwd[512];
    cwd[0] = 0;
    getcwd(cwd, sizeof(cwd));
    nspire_normalize_path(folder, sizeof(folder), cwd[0] ? cwd : ".");
  }
  selected = top = 0;
  status[0] = 0;
  picture_for[0] = 0;
  waiting = -1;
  scroll_for = -1;
  remove_leftovers();
  refresh();
  keys_wait_release();
  nspire_gui_own_keys = own_keys;
  for (;;)
  {
    gui_action_type action;

    if (selected < top)
      top = selected;
    if (selected >= top + ROWS)
      top = selected - ROWS + 1;
    draw();
    update_at_vblank();

    action = get_gui_input();
    if (action == CURSOR_BACK || action == CURSOR_EXIT)
      break;
    if (action != CURSOR_NONE)
      status[0] = 0;
    if (action == CURSOR_UP && rom_count)
      selected = (selected + rom_count - 1) % rom_count;
    else if (action == CURSOR_DOWN && rom_count)
      selected = (selected + 1) % rom_count;
    else if (action == CURSOR_SELECT && rom_count)
    {
      marked[selected] = !marked[selected];
      if (selected + 1 < rom_count)
        selected++;
    }
    else if (key_pressed(KEY_C))
      act(1);
    else if (key_pressed(KEY_U))
      act(0);
    else if (key_pressed(KEY_A) && rom_count)
    {
      int all = marked_count() == rom_count;
      memset(marked, all ? 0 : 1, sizeof(marked));
      status[0] = 0;
    }
  }
  nspire_gui_own_keys = NULL;
  keys_wait_release();
  free(roms);
  roms = NULL;
}
