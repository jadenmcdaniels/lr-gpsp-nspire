/* The Saves place (UI17; until then the save manager, Settings > System >
 * Manage saves): one game's states and its own save, as the game list shows
 * the games, a grid of tiles (nspire_ui_screens.c ui_saves_draw), the
 * newest state first. Reached from the game list (s, the picked game), from
 * the paused menu's Save and Load (s, the paused game) and from System.
 *
 *   enter    loads the picked state (back to the game; from the game list,
 *            the game starts there)
 *   m, a     mark it (or unmark it: the picked one only, the cursor stays;
 *            ui18), mark all (or none): many at once. m held while the
 *            arrows move: each state the cursor comes to gets what the first
 *            got (marked, or unmarked): a run of them at once
 *            (the game's own save is never marked: m, a and a sweep pass it)
 *   c, u     compress, uncompress the marked ones, or the picked one
 *   del      deletes them, after a question (Settings > Saving > Confirm
 *            delete Off: states without it; the game's own save always asks)
 *   t        copies them to the USB drive (on the drive's tab: to the
 *            calculator): a state to the other place's next free slot, the
 *            game's own save to its file there (another save there is
 *            replaced only after a question)
 *   tab      the calculator's saves or the USB drive's, while a drive can
 *            hold saves (nspire_places.h)
 *
 * Another game than the open one: its states are opened here (the states
 * API acts on the open game's) and the paused game's are put back on the
 * way out, its counter floor with them; its own save is a file
 * (nspire_backup_*_of). Its states copy to the drive while a game is
 * paused too (the drive's highest count kept beside the paused game's save
 * can only rise by it: its next saves count higher, which they are); its
 * own save copies only while it is open (nspire_backup_copy_to_drive is the
 * open game's).
 *
 * On the drive's tab: enter loads, t copies to the calculator, del deletes
 * (nspire_place_ops remove; the game's states are then read again). Packing
 * there is not offered (UI17). */

#include "common.h"
#include "nspire.h"
#include "nspire_gui_video.h"
#include "nspire_frontend.h"
#include "nspire_keys.h"
#include "nspire_states.h"
#include "nspire_dialog.h"
#include "nspire_drive.h"
#include "nspire_places.h"
#include "nspire_drive_saves.h"
#include "nspire_ui_screens.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TILES UI_SAVES_TILES
#define GAME_SAVE -1   /* the game's own save's "slot" */
/* r19: the paused game's saves are the top row's SAVES (nspire_saves_tab): two tiles before the saves that are
 * things to do, not saves (never marked, deleted, packed or copied) */
#define RESUME_TILE -2 /* the game as it stands: enter goes back to it */
#define NEW_TILE -3    /* enter saves (to a new slot, or as Settings > Saving > Save to says) */
#define IS_STATE(slot) ((slot) >= 0)
#define IS_SAVE(slot) ((slot) >= GAME_SAVE)
#define PICS 8         /* pictures kept: the screen's and a row more */

struct save_file
{
  int slot;
  long bytes;          /* -1: not known (the drive's) */
  int compressed;
};

/* (r19: as SAVES two tiles that aren't saves come first: with 1000 states, or 999 and the game's own
 * save, the tables of STATE_SLOTS + 1 were written past their end every time the paused menu opened) */
#define FILES_MAX (STATE_SLOTS + 3)   /* Resume, Save, the game's own save, every state */
static struct save_file files[FILES_MAX];
static unsigned char marked[FILES_MAX];
static int file_count, selected, top, place;
static char status[100];
static const char *game;       /* the game's ROM path */
static char title[160];        /* its name */
static int open_game;          /* it is the paused game */
static unsigned drive_seen;    /* the drive (its epoch) the drive's tab was read from */
static int no_memory;          /* a pack, unpack or picture found no memory */
static int sweep = -1;         /* ui18: m held: the mark each tile the cursor comes to gets (1, 0), -1 none */
static int redraw = 1;         /* the place is drawn whatever it showed last (another screen was over it) */
/* r19: drawn as the top row's SAVES: the row's words, which one this is, whether the arrows are on the row */
static const char *const *tab_names;
static int tab_count, tab_on, tab_nav;

/* Whether a drive can hold saves: asked twice a second at most (r19: the place is the paused menu's first
 * screen and is looked at every frame; a drive that needs memory was tried again each time and filled the log). */
static int drive_in(void)
{
  static u32 asked_at;
  static int ready, asked;
  u32 now = nspire_ticks();
  if (!asked || now - asked_at >= NSPIRE_TICK_HZ / 2)
  {
    ready = nspire_places_ready();
    asked_at = now;
    asked = 1;
  }
  return ready;
}

/* The other place can take a copy: a drive in. */
static int can_copy(void)
{
  return drive_in();
}

/* Deleting here: the calculator's, or the drive's when it can. */
static int can_delete(void)
{
  return place == PLACE_CALCULATOR || (drive_in() && nspire_drive_saves->remove);
}

/* The game's states read again (a drive's state deleted behind what is kept of them), its counter floor kept. */
static void reopen(void)
{
  unsigned int floor = states_counter_floor_now();
  states_open_game(game);
  states_counter_floor(floor);
}

static void refresh(void)
{
  int slot, k, newest;

  file_count = 0;
  if (tab_names)
  {
    files[file_count].bytes = -1;
    files[file_count++].slot = RESUME_TILE;
    files[file_count].bytes = -1;
    files[file_count++].slot = NEW_TILE;
  }
  if (place == PLACE_CALCULATOR &&
      nspire_backup_info_of(game, &files[file_count].bytes, &files[file_count].compressed))
    files[file_count++].slot = GAME_SAVE;
  /* the newest first (the state with the highest count, as Resume last save takes it: after slot 1000 new saves
   * fill the gaps, and New slot per save off rewrites the picked one), then the others from the highest slot */
  newest = place == PLACE_CALCULATOR ? states_newest() : states_drive_known() ? states_drive_newest() : -1;
  drive_seen = nspire_drive_epoch();
  for (k = -1; k < STATE_SLOTS; k++)
  {
    struct save_file *f;
    if (file_count >= FILES_MAX)
      break;
    f = &files[file_count];
    slot = k < 0 ? newest : STATE_SLOTS - 1 - k;
    if (slot < 0 || (k >= 0 && slot == newest))
      continue;
    if (place == PLACE_CALCULATOR ? states_file_info(slot, &f->bytes, &f->compressed)
                                  : nspire_drive_saves->exists(slot))
    {
      if (place == PLACE_DRIVE)
      {
        f->bytes = -1;
        f->compressed = 0;
      }
      f->slot = slot;
      file_count++;
    }
  }
  memset(marked, 0, sizeof(marked));
  if (selected >= file_count)
    selected = file_count ? file_count - 1 : 0;
}

static int marked_count(void)
{
  int i, n = 0;
  for (i = 0; i < file_count; i++)
    n += marked[i];
  return n;
}

static void size_text(long bytes, char *text, size_t size)
{
  if (bytes < 0)
    snprintf(text, size, "%s", "");
  else if (bytes < 1024)
    snprintf(text, size, "%ld B", bytes);
  else if (bytes < 10L * 1024 * 1024)
    snprintf(text, size, "%ld KB", (bytes + 1023) / 1024);
  else
    snprintf(text, size, "%ld MB", (bytes + 512L * 1024) / (1024 * 1024));
}

/* ---- The tiles' pictures: looked up once the screen rests, one file a draw ---- */

/* (taken from the heap while the place is open, given back when it is left: the CX II is short of memory. None:
 * the tiles show no pictures.) */
static struct pic_slot
{
  int key;               /* place * STATE_SLOTS + slot + 1; 0 none */
  int has;
  u16 pic[UI_TILE_W * UI_TILE_H];
} *pics;
static u16 *big;         /* a state's picture as it is read (STATE_PIC_W x STATE_PIC_H) */
static int pics_next;
static u32 moved_at;

static void pics_forget(void)
{
  int k;
  for (k = 0; pics && k < PICS; k++)
    pics[k].key = 0;
}

/* The pictures' memory taken (entering, after a pack or a copy) and given back (leaving, before one). They are
 * copies, scaled: nothing points into what states_drop_caches may free. */
static void pics_release(void)
{
  free(pics);
  free(big);
  pics = NULL;
  big = NULL;
}

static void pics_take(void)
{
  pics_release();
  pics = (struct pic_slot *)malloc(sizeof(*pics) * PICS);
  big = (u16 *)malloc(STATE_PIC_W * STATE_PIC_H * sizeof(u16));
  pics_forget();
}

/* The picture of slot (NULL: none, or not looked up yet); *read: a file was read this draw (one a draw). */
static const u16 *picture_of(int slot, int *read)
{
  int key = place * STATE_SLOTS + slot + 1, k, has;
  int rested = nspire_ticks() - moved_at >= NSPIRE_TICK_HZ / 6;

  if (!pics || !big)
    return NULL;
  for (k = 0; k < PICS; k++)
    if (pics[k].key == key)
      return pics[k].has ? pics[k].pic : NULL;
  if (place == PLACE_DRIVE)
    has = *read ? -1 : nspire_drive_saves->picture(slot, big);   /* (it waits for the screen to rest itself) */
  else
  {
    has = states_picture_soon(slot, big, 0);
    if (has < 0 && rested && !*read)
    {
      has = states_picture_soon(slot, big, 1);
      *read = 1;
    }
  }
  if (has < 0)
    return NULL;
  k = pics_next++ % PICS;
  /* (not one on the screen now, when another will do) */
  pics[k].key = key;
  pics[k].has = has;
  if (has)
    ui_scale(pics[k].pic, UI_TILE_W, UI_TILE_H, big, STATE_PIC_W, STATE_PIC_H, STATE_PIC_W);
  return has ? pics[k].pic : NULL;
}

/* ---- The screen ---- */

static void tile_of(const struct save_file *f, struct ui_save_tile *t, int *read)
{
  char size[16];
  memset(t, 0, sizeof(*t));
  size_text(f->bytes, size, sizeof(size));
  if (f->slot == RESUME_TILE)
  {
    t->action = UI_SAVE_RESUME;
    snprintf(t->name, sizeof(t->name), "Resume");
    snprintf(t->band, sizeof(t->band), "%.47s", title);
    return;
  }
  if (f->slot == NEW_TILE)
  {
    int to = place == PLACE_DRIVE ? (nspire_drive_saves->next ? nspire_drive_saves->next() : -1) : nspire_save_target();
    t->action = UI_SAVE_NEW;
    snprintf(t->name, sizeof(t->name), "Save");
    if (to < 0)
      snprintf(t->band, sizeof(t->band), "No free slot");
    else
      snprintf(t->band, sizeof(t->band), place == PLACE_DRIVE ? "To slot %d, USB drive" : "To slot %d", to + 1);
    return;
  }
  if (f->slot == GAME_SAVE)
  {
    t->game_save = 1;
    snprintf(t->name, sizeof(t->name), "In-game save");
    snprintf(t->band, sizeof(t->band), "The game's own save%s%s", size[0] ? " " UI_DOT " " : "", size);
  }
  else
  {
    long when;
    char ago[24] = "";
    snprintf(t->name, sizeof(t->name), "Slot %d", f->slot + 1);
    t->picture = picture_of(f->slot, read);
    if (place == PLACE_DRIVE)
      when = nspire_drive_saves->time ? nspire_drive_saves->time(f->slot) : 0;
    else
    {
      when = states_time_soon(f->slot, 0);
      if (when < 0 && !*read && nspire_ticks() - moved_at >= NSPIRE_TICK_HZ / 6)
      {
        when = states_time_soon(f->slot, 1);
        *read = 1;
      }
    }
    if (when >= 0)
    {
      ui_when(when, (long)time(NULL), 1, t->when, sizeof(t->when));
      ui_when(when, (long)time(NULL), 0, ago, sizeof(ago));
    }
    /* r19: what the band says of it: when it was saved, how big it is, where */
    snprintf(t->band, sizeof(t->band), "%s%s%s%s", ago, ago[0] && size[0] ? " " UI_DOT " " : "", size,
             place == PLACE_DRIVE ? (ago[0] || size[0] ? " " UI_DOT " USB" : "USB drive") : "");
  }
  if (place == PLACE_CALCULATOR)   /* (the drive's: the tab says where they are; their sizes aren't read) */
    snprintf(t->facts, sizeof(t->facts), "%s%s", size, f->compressed ? " packed" : "");
}

/* The states shown (the tiles but the game's own save). */
static int states_shown(void)
{
  int i, n = 0;
  for (i = 0; i < file_count; i++)
    n += IS_STATE(files[i].slot);
  return n;
}

static void draw(void)
{
  static struct ui_saves_view v;
  static const char *keys[20];
  char bytes[24];
  long total = 0;
  int i, n = 0, read = 0, marked_n = marked_count();
  const struct save_file *f = file_count ? &files[selected] : NULL;

  memset(&v, 0, sizeof(v));
  memset(keys, 0, sizeof(keys));
  v.game = title;
  v.count = file_count;
  v.first = top;
  v.picked = selected;
  v.empty = place == PLACE_DRIVE ? "No saves of this game on the USB drive." : "No saves for this game yet.";
  v.tabs = tab_names;
  v.tab_count = tab_count;
  v.tab_on = tab_on;
  v.nav_focus = tab_names && tab_nav;
  if (drive_in())
  {
    v.places[0] = nspire_place_names[PLACE_CALCULATOR];
    v.places[1] = nspire_place_names[PLACE_DRIVE];
    v.place_count = 2;
    v.place = place;
  }
  for (i = 0; i < file_count; i++)
    if (!marked_n || marked[i])
      total += files[i].bytes > 0 ? files[i].bytes : 0;
  size_text(total, bytes, sizeof(bytes));
  if (marked_n)
    snprintf(v.note, sizeof(v.note), "%d marked", marked_n);
  else if (file_count)
    snprintf(v.note, sizeof(v.note), "%s", bytes);
  for (i = 0; i < TILES && top + i < file_count; i++)
  {
    tile_of(&files[top + i], &v.tiles[i], &read);
    v.tiles[i].marked = marked[top + i];
  }

  /* the band's right: what the keys do to the picked one (or the marked ones), the most used first, two (three
   * with a drive in); what doesn't fit beside its facts is left out (ui_band). (r19: enter isn't said, it does what enter
   * does to every item; the place's key comes third, before the ones for many saves.) */
  if (f && IS_STATE(f->slot) && tab_names)
  {
    keys[n++] = "S";
    keys[n++] = "Save";   /* (over it, after a question) */
  }
  if (f && IS_SAVE(f->slot) && can_delete())
  {
    keys[n++] = "D";
    keys[n++] = "Delete";
  }
  if (drive_in())
  {
    keys[n++] = "tab";
    keys[n++] = place == PLACE_DRIVE ? "Calculator" : "USB drive";
  }
  if (f && IS_STATE(f->slot))
  {
    keys[n++] = "M";
    keys[n++] = marked[selected] ? "Unmark" : "Mark";
  }
  if (f && IS_SAVE(f->slot))
  {
    /* (a drive's: whether it is packed isn't read: c packs, u unpacks) */
    int packed = marked_n || place == PLACE_DRIVE ? 0 : f->compressed;
    keys[n++] = packed ? "U" : "C";
    keys[n++] = packed ? "Unpack" : "Pack";
  }
  if (f && IS_SAVE(f->slot) && can_copy())
  {
    keys[n++] = "T";
    keys[n++] = "Copy";   /* (to the other place) */
  }
  /* (two said, three with a drive in: its key is the third) */
  if (n > (drive_in() ? 6 : 4))
    n = drive_in() ? 6 : 4;
  if (marked_n)
  {
    keys[n++] = "A";
    keys[n++] = marked_n == states_shown() ? "None" : "All";
  }
  keys[n] = NULL;
  v.keys = keys;
  v.status = status;
  /* drawn when something on it changed (r19: it is the paused menu's first screen; until then every frame) */
  {
    static struct ui_saves_view shown;
    static char shown_status[sizeof(status)];
    static const char *shown_keys[20];
    if (!redraw && !memcmp(&v, &shown, sizeof(v)) && !strcmp(status, shown_status) &&
        !memcmp(keys, shown_keys, sizeof(shown_keys)))
      return;
    shown = v;
    snprintf(shown_status, sizeof(shown_status), "%s", status);
    memcpy(shown_keys, keys, sizeof(shown_keys));
    redraw = 0;
  }
  ui_saves_draw(&v);
}

/* The marked files, or the one picked, are the ones acted on. The game's own save is never marked (m and a
 * pass it): it is deleted, packed or copied only picked alone, after its own question. */
static int is_target(int i, int targets)
{
  return targets ? marked[i] : i == selected;
}

static void draw_ground(void *unused)
{
  (void)unused;
  redraw = 1;
  draw();
}

/* A question over the place, the safe answer first: 1 for the second. */
static int ask(const char *title, const char *text, const char *second)
{
  struct ui_dialog_view d;
  int answer;
  memset(&d, 0, sizeof(d));
  d.title = title;
  d.text = text;
  d.first = "No, keep it";
  d.second = second;
  d.destructive = 1;
  /* (ui18: no wait for the keys here: the question shows at once and takes no answer until the key that asked
   * it is let go, nspire_ask) */
  status[0] = 0;
  answer = nspire_ask(&d, draw_ground, NULL);
  keys_wait_release();
  redraw = 1;
  return answer == 1;
}

/* Deleting asks first: 1 to delete. ui18: Settings > Saving > Confirm delete Off: states go without the question;
 * the game's own save always asks (its progress, and its copy on the drive with it). */
static int confirm_delete(int targets)
{
  char title_text[48];
  int i;

  if (!nspire_confirm_delete)
  {
    /* (the game's own save is never marked: it is a target only picked alone) */
    for (i = 0; i < file_count && !(is_target(i, targets) && files[i].slot == GAME_SAVE); i++)
      ;
    if (i == file_count)
      return 1;
  }
  if (targets > 1)
  {
    snprintf(title_text, sizeof(title_text), "Delete these %d saves?", targets);
    if (ask(title_text, "They are gone for good. You can't undo this.", "Delete"))
      return 1;
  }
  else
  {
    for (i = 0; !is_target(i, targets); i++)
      ;
    if (files[i].slot == GAME_SAVE)
    {
      /* the game's own save is its progress: named, and its copy on the USB drive said when it goes too
       * (nspire_backup_delete deletes that one with the paused game's) */
      char what[96], fit[64];
      snprintf(what, sizeof(what), "Delete %s's own save?", ui_fit(title, &ui_font_small_strong, 200, fit, sizeof(fit)));
      if (ask(what, open_game && nspire_saves_on_drive() ?
                    "Its progress is gone for good, and its copy on the USB drive too. You can't undo this." :
                    "Its progress is gone for good. You can't undo this.", "Delete"))
        return 1;
    }
    else
    {
      snprintf(title_text, sizeof(title_text), "Delete slot %d?", files[i].slot + 1);
      if (ask(title_text, "It is gone for good. You can't undo this.", "Delete"))
        return 1;
    }
  }
  snprintf(status, sizeof(status), "Nothing deleted.");
  return 0;
}

/* After a pack or unpack failed: whether there was memory for it (a state and its packed form at once). */
static int enough_memory(void)
{
  void *p = malloc(2 * GBA_STATE_MEM_SIZE);
  free(p);
  return p != NULL;
}

static void act(int what)   /* 'c' compress, 'u' uncompress, 'd' delete */
{
  static int slots[FILES_MAX];
  int targets = marked_count(), count = 0, done = 0, failed = 0, i, k;
  long before = 0, after = 0;
  char was[24], now[24], why[40] = "";
  const char *verb;

  no_memory = 0;
  sweep = -1;
  if (!file_count || (!targets && !IS_SAVE(files[selected].slot)))
    return;
  if ((what == 'd' && !can_delete()) || (what == 'd' && !confirm_delete(targets)))
    return;
  /* (the tiles' pictures given back while it works: a pack needs its memory, save17's 512 KB margin) */
  pics_release();
  if (what != 'd')
  {
    snprintf(status, sizeof(status), what == 'c' ? "Packing..." : "Unpacking...");
    draw();
    update_at_vblank();
  }
  for (i = 0; i < file_count; i++)
  {
    int ok, slot = files[i].slot;
    if (!is_target(i, targets))
      continue;
    before += files[i].bytes;
    if (what == 'd' && place == PLACE_DRIVE)
      /* (the drive that was read, and the state still there: never another drive's file of that name) */
      ok = drive_seen == nspire_drive_epoch() && nspire_drive_saves->exists(slot) &&
           nspire_drive_saves->remove(game, slot);
    else if (what == 'd')
      ok = slot == GAME_SAVE ? nspire_backup_delete_of(game) : states_delete(slot);
    else if (place == PLACE_DRIVE)
      /* save17's: the drive's state packed or stored, its age kept */
      ok = drive_seen == nspire_drive_epoch() && nspire_drive_saves->exists(slot) &&
           states_drive_set_compressed(slot, what == 'c');
    else
      /* a state through the states API (its age kept: the save time in its header, save17); the game's own
       * save is a gzip file (nspire_backup.c) */
      ok = slot == GAME_SAVE ? nspire_backup_set_compressed_of(game, what == 'c')
                             : states_set_compressed(slot, what == 'c');
    if (ok)
      done++;
    else
    {
      failed++;
      if (what != 'd' && slot != GAME_SAVE)
        snprintf(why, sizeof(why), "%s", states_failure_words(STATES_SAVING));
      else if (what != 'd' && !enough_memory())
        no_memory = 1;
    }
    slots[count++] = slot;
  }
  if (place == PLACE_DRIVE)
    reopen();
  refresh();
  pics_take();     /* (again; they changed) */
  for (i = 0; i < file_count; i++)
    for (k = 0; k < count; k++)
      if (files[i].slot == slots[k])
        after += files[i].bytes;

  size_text(before, was, sizeof(was));
  size_text(after, now, sizeof(now));
  verb = what == 'c' ? "Packed" : what == 'u' ? "Unpacked" : "Deleted";
  if (failed && why[0])
    snprintf(status, sizeof(status), "%s %d; %d couldn't be: %s", verb, done, failed, why);
  else if (failed && no_memory)
    snprintf(status, sizeof(status), "%s %d; %d couldn't be (not enough memory)", verb, done, failed);
  else if (failed)
    snprintf(status, sizeof(status), "%s %d; %d couldn't be (space?)", verb, done, failed);
  else if (place == PLACE_DRIVE)
    snprintf(status, sizeof(status), "%s %d on the USB drive.", verb, done);   /* (their sizes aren't read) */
  else if (what == 'd')
    snprintf(status, sizeof(status), "Deleted %d (%s).", done, was);
  else
    snprintf(status, sizeof(status), "%s %d: %s before, %s now.", verb, done, was, now);
}

/* t: the marked files, or the picked one, copied to the other place. */
static void copy_act(void)
{
  int targets = marked_count(), done = 0, failed = 0, asked = 0, skipped = 0, to = -1, last = -1, i;
  const char *there = place == PLACE_DRIVE ? "calculator" : "USB drive";

  sweep = -1;
  if (!file_count || !can_copy() || (!targets && !IS_SAVE(files[selected].slot)))
    return;
  /* (the drive's own tab: its states were read from a drive that has since changed. From the calculator's
   * nothing of the drive's is acted on: r19, T there said "look again" until the drive's tab had been shown) */
  if (place == PLACE_DRIVE && drive_seen != nspire_drive_epoch())
  {
    snprintf(status, sizeof(status), "The USB drive changed: look again.");
    return;
  }
  snprintf(status, sizeof(status), "Copying...");
  draw();
  update_at_vblank();
  pics_release();   /* (a copy needs memory: given back while it works) */
  for (i = 0; i < file_count; i++)
  {
    int r;
    if (!is_target(i, targets))
      continue;
    if (files[i].slot != GAME_SAVE)
    {
      /* a state: to the other place's next free slot (or the slot that has it already); one of the drive's
       * still there */
      r = (place == PLACE_CALCULATOR || nspire_drive_saves->exists(files[i].slot)) &&
          nspire_drive_saves->copy(place, files[i].slot, &to) == 1 ? STATES_COPY_DONE : STATES_COPY_FAILED;
      if (r == STATES_COPY_DONE)
        last = to;
    }
    else if (!open_game)
    {
      skipped++;   /* (another game's own save: only the open game's copies, nspire_backup_copy_to_drive) */
      continue;
    }
    else
    {
      r = nspire_backup_copy_to_drive(0);
      if (r == STATES_COPY_OTHER)
      {
        /* another save of the game is on the drive: replaced only when the player says so */
        asked = 1;
        r = ask("Replace the save on the USB drive?", "The game's own save there is lost. You can't undo this.",
                "Replace") ? nspire_backup_copy_to_drive(1) : STATES_COPY_OTHER;
      }
    }
    done += r == STATES_COPY_DONE || r == STATES_COPY_SAME;
    failed += r == STATES_COPY_FAILED;
  }
  pics_take();
  if (place == PLACE_DRIVE)
    refresh();   /* (the calculator's slots aren't shown: the drive's tab is) */
  if (failed)
    snprintf(status, sizeof(status), "Copied %d; %d couldn't be (space?)", done, failed);
  else if (!done && skipped)
    snprintf(status, sizeof(status), "Copies while its game is open.");
  else if (!done)
    snprintf(status, sizeof(status), asked ? "Nothing copied." : "Nothing to copy.");
  else if (done == 1 && last >= 0)
    snprintf(status, sizeof(status), "Copied to slot %d on the %s.", last + 1, there);
  else
    snprintf(status, sizeof(status), "Copied %d to the %s.", done, there);
}

/* Two paths of one ROM file: the same, whatever their separators ("\documents\gba\/x" and "/documents/gba/x"). */
int nspire_same_rom_path(const char *a, const char *b)
{
  for (;;)
  {
    int sa = *a == '/' || *a == '\\', sb = *b == '/' || *b == '\\';
    if (sa != sb)
      return 0;
    if (sa)
    {
      while (*a == '/' || *a == '\\')
        a++;
      while (*b == '/' || *b == '\\')
        b++;
      continue;
    }
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
      return 0;
    if (!*a)
      return 1;
    a++;
    b++;
  }
}

/* r19: said in the band when the place opens next (what the menu's save or load just did: "Slot 3 is empty"). */
void nspire_saves_status(const char *text)
{
  snprintf(status, sizeof(status), "%s", text ? text : "");
}

/* The place's loop. tabs NULL: a page of its own (the games' grid's S, System's Saves): returns SAVES_TAB_LOAD
 * when a state was picked (*slot, *on_drive), else SAVES_TAB_ESC. tabs: the paused game's saves as the top
 * row's SAVES (nspire_saves_tab). */
static int place_loop(const char *rom_path, int *slot, int *on_drive, const char *const *tabs, int count, int on,
                      int nav)
{
  char tags[96];
  unsigned int floor = 0;
  int result = SAVES_TAB_ESC;
  const unsigned char *menu_keys = nspire_gui_own_keys;
  /* The keys this place keeps for itself (not a button's in a menu): the letters, tab; as SAVES, s too. d
   * deletes. (r19: del is no longer kept: with the keys as they come it is the GBA's A, which is enter in every
   * menu: on the paused menu's first screen it would have asked to delete the save the player meant to load.
   * A del that isn't a button's key still deletes: it comes as CURSOR_CLEAR.) */
  static const unsigned char own_keys[] = { KEY_D, KEY_C, KEY_U, KEY_A, KEY_T, KEY_M, KEY_TAB, 0 };
  static const unsigned char tab_keys[] = { KEY_D, KEY_C, KEY_U, KEY_A, KEY_T, KEY_M, KEY_TAB, KEY_S, 0 };
  /* as SAVES: the tile picked when it was last left (a question, a save that didn't happen: back on that tile) */
  static int last_selected, last_place;
  char kept_status[sizeof(status)];

  tab_names = tabs;
  tab_count = count;
  tab_on = on;
  tab_nav = nav == 1;
  game = rom_path && rom_path[0] ? rom_path : gamepak_filename;
  /* the paused game only when it is this very file: another of the same name (another folder, another
   * region) has its own saves ("~CODE", nspire_game_identify) */
  open_game = gamepak_filename[0] && nspire_same_rom_path(game, gamepak_filename);
  ui_game_title(game, title, sizeof(title), tags, sizeof(tags));
  if (!open_game)
  {
    floor = states_counter_floor_now();
    /* (the paused game's writes under way finished first, under its own names) */
    nspire_backup_finish();
    states_save_finish();
    nspire_game_identify(game);   /* (its files' names: the ones its start will use) */
    nspire_saves_other_game = 1;
    states_open_game_kept(game);
  }
  pics_take();
  selected = top = 0;
  /* (as SAVES: what the menu's last save or load said, nspire_saves_status, is shown once) */
  snprintf(kept_status, sizeof(kept_status), "%s", tabs ? status : "");
  status[0] = 0;
  no_memory = 0;
  place = nspire_saves_on == PLACE_DRIVE && drive_in() ? PLACE_DRIVE : PLACE_CALCULATOR;
  if (tabs && nav == 2 && (last_place == PLACE_CALCULATOR || drive_in()))
    place = last_place;
  pics_forget();
  refresh();
  if (tabs && nav == 2)
    selected = last_selected < file_count ? last_selected : file_count ? file_count - 1 : 0;
  snprintf(status, sizeof(status), "%s", kept_status);
  /* a page of its own: the newest state picked (picking one to load is what it is for most), not the game's
   * own save; as SAVES: Resume, its first tile */
  if (!tabs && file_count > 1 && files[0].slot == GAME_SAVE)
    selected = 1;
  /* the paused game's: the slot the paused menu has picked, picked here too */
  if (!tabs && open_game && slot && *slot >= 0 && place == PLACE_CALCULATOR)
  {
    int i;
    for (i = 0; i < file_count; i++)
      if (files[i].slot == *slot)
        selected = i;
  }
  moved_at = nspire_ticks();
  redraw = 1;
  /* (as SAVES no key is waited for: a held arrow goes on along the top row through this word, and the menu
   * opens with a key of the game's still held; the letters here act on a press, not on a key held) */
  if (!tabs)
    keys_wait_release();
  sweep = -1;
  for (;;)
  {
    gui_action_type action;
    int was = selected;

    /* the drive's tab of a drive taken out or swapped: read again, never shown as the one before */
    if (place == PLACE_DRIVE && (!drive_in() || drive_seen != nspire_drive_epoch()))
    {
      if (!drive_in())
        place = PLACE_CALCULATOR;
      reopen();
      selected = top = 0;
      refresh();
      pics_forget();
    }
    if (selected < top)
      top = selected / 3 * 3;
    if (selected >= top + TILES)
      top = selected / 3 * 3 - (TILES - 3);
    draw();
    update_at_vblank();

    nspire_gui_own_keys = tab_names && tab_nav ? NULL : tab_names ? tab_keys : own_keys;
    action = get_gui_input();
    nspire_gui_own_keys = menu_keys;
    if (action != CURSOR_NONE)
      status[0] = 0;
    if (tab_names && tab_nav)
    {
      /* r19: the arrows on the top row: left and right leave for the word before or after, down or enter goes
       * into the tiles, esc or the menu key back to the game */
      if (action == CURSOR_LEFT || action == CURSOR_RIGHT)
      {
        result = action == CURSOR_LEFT ? SAVES_TAB_LEFT : SAVES_TAB_RIGHT;
        break;
      }
      if (action == CURSOR_BACK || action == CURSOR_EXIT)
        break;
      if ((action == CURSOR_DOWN || action == CURSOR_SELECT) && file_count)
      {
        tab_nav = 0;
        moved_at = nspire_ticks();
      }
      continue;
    }
    /* esc or the menu key: a page of its own is left; as SAVES (the paused menu's first screen), back to the
     * game at once, from its tiles as from the row */
    if (action == CURSOR_BACK || action == CURSOR_EXIT)
      break;
    /* ui18: m held: a sweep, its mark (what the first tile got: 1 marked, 0 unmarked) for every tile the cursor
     * comes to; let go, no sweep */
    if (!key_down(KEY_M))
      sweep = -1;
    if (action == CURSOR_RIGHT && selected + 1 < file_count && selected % 3 < 2)
      selected++;
    else if (action == CURSOR_LEFT && selected % 3 > 0)
      selected--;
    else if (action == CURSOR_DOWN && selected / 3 < (file_count - 1) / 3)
      selected = selected + 3 < file_count ? selected + 3 : file_count - 1;
    else if (action == CURSOR_UP && selected >= 3)
      selected -= 3;
    else if (action == CURSOR_UP && tab_names)
    {
      tab_nav = 1;   /* up from the first row: the top row */
      sweep = -1;
    }
    else if (action == CURSOR_SELECT && file_count && files[selected].slot == RESUME_TILE)
    {
      result = SAVES_TAB_RESUME;
      break;
    }
    else if (file_count && files[selected].slot == NEW_TILE && (action == CURSOR_SELECT || key_pressed(KEY_S)))
    {
      /* Save: to the slot Settings > Saving > Save to gives (a new one, or the same one) */
      if (slot)
        *slot = -1;
      if (on_drive)
        *on_drive = place == PLACE_DRIVE;
      result = SAVES_TAB_SAVE;
      break;
    }
    else if (action == CURSOR_SELECT && file_count && files[selected].slot == GAME_SAVE && tab_names && open_game)
    {
      result = SAVES_TAB_LOAD_GAME_SAVE;
      break;
    }
    else if (action == CURSOR_SELECT && file_count && IS_STATE(files[selected].slot))
    {
      if (slot)
        *slot = files[selected].slot;
      if (on_drive)
        *on_drive = place == PLACE_DRIVE;
      /* its picture for the loading screen, when the game starts from it (the store: gui.c); taken from the
       * heap until the loading screen has it (none: the screen shows none) */
      free(nspire_start_picture);
      nspire_start_picture = (u16 *)malloc(STATE_PIC_W * STATE_PIC_H * sizeof(u16));
      nspire_start_picture_has = -1;
      if (nspire_start_picture)
      {
        int has = place == PLACE_DRIVE ? nspire_drive_saves->picture(files[selected].slot, nspire_start_picture)
                                       : states_picture(files[selected].slot, nspire_start_picture);
        nspire_start_picture_has = has > 0;
      }
      result = SAVES_TAB_LOAD;
      break;
    }
    else if (tab_names && key_pressed(KEY_S) && file_count && IS_STATE(files[selected].slot))
    {
      /* s on a state: saved over it (the menu asks first); on Resume: as Save */
      if (slot)
        *slot = files[selected].slot;
      if (on_drive)
        *on_drive = place == PLACE_DRIVE;
      result = SAVES_TAB_SAVE;
      break;
    }
    else if (tab_names && key_pressed(KEY_S) && file_count && files[selected].slot == RESUME_TILE)
    {
      if (slot)
        *slot = -1;
      if (on_drive)
        *on_drive = place == PLACE_DRIVE;
      result = SAVES_TAB_SAVE;
      break;
    }
    else if (action == CURSOR_CLEAR || key_pressed(KEY_D))
      act('d');
    else if (key_pressed(KEY_C))
      act('c');
    else if (key_pressed(KEY_U))
      act('u');
    else if (key_pressed(KEY_T))
      copy_act();
    else if (key_pressed(KEY_M) && file_count)
    {
      /* ui18 (marking no longer goes on to the next): the picked one only, the cursor stays;
       * held, the arrows sweep (the game's own save is never marked: a sweep started on it marks) */
      if (IS_STATE(files[selected].slot))
        marked[selected] = !marked[selected];
      sweep = IS_STATE(files[selected].slot) ? marked[selected] : 1;
    }
    else if (key_pressed(KEY_A) && file_count)
    {
      /* all the states (never the game's own save), or none */
      int i, all = marked_count() == states_shown();
      sweep = -1;
      for (i = 0; i < file_count; i++)
        marked[i] = !all && IS_STATE(files[i].slot);
    }
    else if (key_pressed(KEY_TAB) && drive_in())
    {
      place = !place;
      selected = top = 0;
      sweep = -1;
      refresh();
    }
    if (selected != was && sweep >= 0 && IS_STATE(files[selected].slot))
      marked[selected] = (unsigned char)sweep;
    if (selected != was || action != CURSOR_NONE)
      moved_at = nspire_ticks();
  }
  nspire_gui_own_keys = menu_keys;
  last_selected = selected;
  last_place = place;
  if (!tabs)
    keys_wait_release();
  pics_release();
  nspire_saves_other_game = 0;
  if (!open_game && gamepak_filename[0])
  {
    /* the paused game's names and states again, as they were */
    nspire_game_identify(gamepak_filename);
    states_open_game_kept(gamepak_filename);
    states_counter_floor(floor);
  }
  tab_names = NULL;
  status[0] = 0;
  ui_shown[0] = 0;
  return result;
}

int nspire_saves_place(const char *rom_path, int *slot, int *on_drive)
{
  return place_loop(rom_path, slot, on_drive, NULL, 0, 0, 0) == SAVES_TAB_LOAD;
}

int nspire_saves_tab(const char *const *tabs, int count, int on, int nav, int *slot, int *on_drive)
{
  return place_loop(NULL, slot, on_drive, tabs, count, on, nav);
}
