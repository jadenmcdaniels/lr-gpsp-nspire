/* Saves on a USB drive: see nspire_drive_saves.h. */

#include "common.h"
#include "nspire.h"
#include "nspire_frontend.h"
#include "nspire_places.h"
#include "nspire_states.h"
#include "nspire_drive.h"
#include "nspire_drive_saves.h"

#include <stdio.h>
#include <string.h>

/* (2026-11-18, Hub 5) the pad glue's: a dead hub's power restarted, its drive coming back */
int nspire_usbhub_recovering(void) __attribute__((weak));
int nspire_usbhub_doubtful(void) __attribute__((weak));

/* (weak: the menus' nspire_devices.c has it; a build without the menus, the
 * PC's drive_tests, has this one) */
const struct nspire_place_ops *nspire_drive_saves __attribute__((weak));
void nspire_link_log(const char *fmt, ...) __attribute__((format(printf, 1, 2), weak));
int keys_down_now(void) __attribute__((weak));

static const struct nspire_place_ops drive_places;

/* The drive (its epoch: nspire_drive_epoch, which changes when it is pulled
 * out or put in) that failed a write or could not be read: not a place for
 * saves again as it is. And the one that was full for a write gpSP made by
 * itself (a copy of a state, the save's copy, Save when I quit): no more of
 * those; the menu's own Save there still tries and says so. */
static unsigned refused_epoch = ~0u, full_epoch = ~0u;
/* What the player is to be told, once: DRIVE_E_* (why the calculator has
 * the saves), NOTE_BACK (the drive has them again); the last one said and
 * the drive's epoch then (the same thing is not said twice of one drive). */
#define NOTE_BACK 100
static int note_pending, said_reason = -1;
static unsigned said_epoch;
/* The line that waits for the toast on screen to end (nspire_saves_say), and
 * whether it is one of the place's (a DRIVE_E_* reason). */
static const char *waiting;
static int waiting_place;
/* The calculator's slots the game's frames saved, to copy to the drive. */
#define PENDING 16
static int pending[PENDING], pending_n;

static int ours(void)
{
  return nspire_drive_saves == &drive_places;
}

/* always: also with Saves on: Calculator (else only what Saves on: USB drive
 * asked for is told: the menus say what happened on the drive's tab). */
static void note_of(int reason, int always)
{
  if ((nspire_saves_on != PLACE_DRIVE && !always) || (reason == said_reason && nspire_drive_epoch() == said_epoch))
    return;
  if (reason == NOTE_BACK && ((note_pending && note_pending != NOTE_BACK) || waiting_place))
  {
    /* back before the player was shown that it was away: neither is said */
    note_pending = 0;
    if (waiting_place)
      waiting = NULL;
    waiting_place = 0;
    said_reason = -1;
    return;
  }
  said_reason = reason;
  said_epoch = nspire_drive_epoch();
  note_pending = reason;
}

static void note(int reason)
{
  note_of(reason, 0);
}

/* In, opened (may_open: one that is attached is opened now, a wait of up to
 * seconds), not known to be write-protected, and not refused. */
static int usable(int may_open)
{
  return nspire_drive_usable(may_open) && refused_epoch != nspire_drive_epoch();
}

/* A write or a read of the drive failed: what that makes of it. by_itself:
 * gpSP's own write (nobody is looking at a screen that says so). */
static void failed(int by_itself)
{
  int why = nspire_drive_error();

  /* (a read that found the drive gone leaves no error of a write: what the drive is now) */
  if (why == DRIVE_E_NONE && !nspire_drive_usable(0))
    why = nspire_drive_error();
  if (nspire_link_log)
    nspire_link_log("saves: the USB drive %s", why == DRIVE_E_GONE ? "is gone" : why == DRIVE_E_PROTECTED ?
                    "is write-protected" : why == DRIVE_E_FULL ? "is full" : why == DRIVE_E_MEMORY ?
                    "needs memory to be opened" : "failed");
  if (why == DRIVE_E_NONE)
    return;   /* (not the drive: no memory for the state) */
  if (why == DRIVE_E_FAILED)
    refused_epoch = nspire_drive_epoch();
  else if (why == DRIVE_E_FULL && by_itself)
    full_epoch = nspire_drive_epoch();
  /* A drive that is write-protected or failed is no place for saves from here: its tab goes
   * from Save and Load. The menus' message for a save that did not work names a full drive or
   * one taken out: why this one's tab went is said back in the game, whatever Saves on is. */
  if (!by_itself && (why == DRIVE_E_PROTECTED || why == DRIVE_E_FAILED))
    note_of(why, 1);
  else if (by_itself || why != DRIVE_E_FULL)
    note(why);
}

/* The game's states on the drive known (read now when they are not: the
 * folder's names and the newest state's header and picture). 0: the drive
 * can't be read, and is no place for saves as it is. */
static int view(void)
{
  if (states_drive_known())
    return 1;
  if (states_drive_read())
    return 1;
  if (nspire_drive_usable(0))
  {
    /* in, and its folder can't be read */
    if (nspire_link_log)
      nspire_link_log("saves: the USB drive's saves can't be read");
    refused_epoch = nspire_drive_epoch();
    note(DRIVE_E_FAILED);
  }
  else
    note(DRIVE_E_GONE);
  return 0;
}

/* The highest count of the game's states on a drive, kept on the calculator
 * for the runs without the drive (a save there then counts from it). */
int nspire_saves_other_game;

static void keep_top(void)
{
  /* (only the paused game's: its mark file is named after the game identified, which is another's while the
   * Saves place shows another game) */
  if (nspire_saves_other_game)
    return;
  if (states_drive_top() > nspire_backup_drive_top())
    nspire_backup_drive_top_set(states_drive_top());
}

/* ---- The place's functions (nspire_places.h): the menus' ---- */

static int place_ready(void)
{
  return usable(1);
}

static int place_exists(int slot)
{
  return usable(0) && view() && states_drive_exists(slot);
}

static int place_next(void)
{
  return usable(0) && view() ? states_drive_next() : -1;
}

/* A picture that is kept in memory at once; one that is not: -1, and it is
 * read from the drive at a later ask, once it has been asked for a sixth of
 * a second and no key is down (one file a time, as the calculator's strip
 * reads its own once a slot has stayed picked). */
static int place_picture(int slot, u16 *pic)
{
  static struct { int slot; u32 since; } asked[4];
  static u32 read_at;
  static int any_read;
  u32 now = nspire_ticks();
  int i, k = -1, has;

  if (!usable(0) || !states_drive_known())
    return 0;
  has = states_drive_picture(slot, pic, 0);
  if (has >= 0)
    return has;
  for (i = 0; i < 4; i++)
    if (asked[i].slot == slot + 1)
      k = i;
  if (k < 0)
  {
    /* (the one asked longest ago makes room) */
    for (i = 0, k = 0; i < 4; i++)
      if (!asked[i].slot || (u32)(now - asked[i].since) > (u32)(now - asked[k].since))
        k = i;
    asked[k].slot = slot + 1;
    asked[k].since = now;
    return -1;
  }
  if ((u32)(now - asked[k].since) < NSPIRE_TICK_HZ / 6 || (keys_down_now && keys_down_now()) ||
      (any_read && (u32)(now - read_at) < NSPIRE_TICK_HZ / 30))
    return -1;
  asked[k].slot = 0;
  has = states_drive_picture(slot, pic, 1);
  read_at = nspire_ticks();
  any_read = 1;
  return has < 0 ? 0 : has;
}

static long place_time(int slot)
{
  return usable(0) && states_drive_known() ? states_drive_time(slot) : 0;
}

static int place_save(int slot)
{
  if (!usable(1) || !view() || states_drive_exists(slot))
    return 0;   /* (never over a state there: the menus save to next()) */
  if (!states_drive_save(slot))
  {
    failed(0);
    return 0;
  }
  keep_top();
  nspire_osd_message("Saved to slot %d (USB drive)", slot + 1);
  return 1;
}

static int place_load(int slot)
{
  if (!usable(1) || !view())
    return -1;
  return states_drive_load(slot);
}

static int place_copy(int from_place, int slot, int *to_slot)
{
  int result;

  if (!usable(1) || !view())
    return 0;
  result = states_place_copy(from_place == PLACE_CALCULATOR, slot, to_slot);
  if (result == STATES_COPY_FAILED && from_place == PLACE_CALCULATOR)
    failed(0);
  if (result == STATES_COPY_DONE && from_place == PLACE_CALCULATOR)
    keep_top();
  /* (the same state there already: it is in that slot) */
  return result == STATES_COPY_DONE || result == STATES_COPY_SAME;
}

/* UI17: the state's file on the drive deleted ("/gpsp/states/<the game's name><slot>.svs", nspire_states.h) */
static int place_remove(const char *rom_path, int slot)
{
  char base[600], path[620];

  /* (a state the drive that is in has, as read: never a file of that name on another drive) */
  if (!usable(1) || slot < 0 || slot >= STATE_SLOTS || !view() || !states_drive_exists(slot))
    return 0;
  nspire_game_drive_base(rom_path, "states", base, sizeof(base));
  snprintf(path, sizeof(path), "%s%d.svs", base, slot);
  if (nspire_drive_remove(path))
    return 1;
  failed(0);
  return 0;
}

static const struct nspire_place_ops drive_places =
{
  place_ready, place_exists, place_next, place_picture, place_time, place_save, place_load, place_copy, place_remove
};

void nspire_drive_saves_start(void)
{
  if (!nspire_drive_saves)
    nspire_drive_saves = &drive_places;
}

/* ---- What gpSP does by itself with Saves on: USB drive ---- */

int nspire_saves_on_drive(void)
{
  return ours() && nspire_saves_on == PLACE_DRIVE && usable(0) && full_epoch != nspire_drive_epoch();
}

void nspire_saves_drive_failed(void)
{
  failed(1);
}

void nspire_saves_game_start(void)
{
  pending_n = 0;
  if (!ours() || nspire_saves_on != PLACE_DRIVE)
    return;
  /* (kept from the runs before: a state saved on the calculator now, with or without the drive in,
   * counts above the drive's) */
  states_counter_floor(nspire_backup_drive_top());
  /* opened if it is only attached; its write-protect switch asked once */
  if (!nspire_drive_ready())
  {
    note(nspire_drive_error());
    return;
  }
  if (refused_epoch == nspire_drive_epoch())
  {
    note(DRIVE_E_FAILED);
    return;
  }
  if (view())
    keep_top();
}

int nspire_saves_resume(int *on_drive)
{
  int calc = states_newest(), drive;

  *on_drive = 0;
  if (!ours() || nspire_saves_on != PLACE_DRIVE || !usable(0) || !states_drive_known())
    return calc;
  /* the newest of the two by their counts (a save in either place counts from both: nspire_states.c) */
  drive = states_drive_newest();
  if (drive >= 0 && (calc < 0 || states_drive_top() > states_newest_counter()))
  {
    *on_drive = 1;
    return drive;
  }
  return calc;
}

void nspire_saves_frame_saved(int slot)
{
  int i;

  if (!ours() || nspire_saves_on != PLACE_DRIVE)
    return;
  for (i = 0; i < pending_n; i++)
    if (pending[i] == slot)
      return;
  if (pending_n == PENDING)
  {
    /* (more than it holds between two safe points: the oldest stays the calculator's alone) */
    memmove(pending, pending + 1, sizeof(pending[0]) * (PENDING - 1));
    pending_n--;
  }
  pending[pending_n++] = slot;
}

void nspire_saves_sync(int may_open)
{
  if (!ours())
    return;
  if (nspire_saves_on != PLACE_DRIVE)
    pending_n = 0;
  else if (!(may_open ? nspire_drive_ready() : nspire_drive_ready_now()))
    note(nspire_drive_error());
  else if (refused_epoch == nspire_drive_epoch())
    note(DRIVE_E_FAILED);
  else if (full_epoch == nspire_drive_epoch())
    note(DRIVE_E_FULL);
  else
  {
    /* the drive has the saves (again, when the player was told the calculator had them) */
    if (said_reason > 0 && said_reason != NOTE_BACK)
      note(NOTE_BACK);
    /* what the game's frames saved on the calculator: copied to the drive, the oldest first */
    while (pending_n && view())
    {
      int to = -1, result = states_place_copy(1, pending[0], &to);
      if (result == STATES_COPY_FAILED && states_exists(pending[0]))
      {
        failed(1);
        break;
      }
      memmove(pending, pending + 1, sizeof(pending[0]) * (size_t)(pending_n - 1));
      pending_n--;
    }
    if (states_drive_known())
      keep_top();
  }
  /* the save's copy on the drive (nspire_backup.c asks nspire_saves_on_drive) */
  nspire_backup_drive_sync();
}

int nspire_saves_exit_save(void)
{
  int slot;

  if (!nspire_saves_on_drive() || !nspire_drive_ready_now() || !view())
    return 0;
  slot = states_drive_next();
  if (slot < 0 || !states_drive_save(slot))
  {
    failed(1);
    return 0;
  }
  keep_top();
  return 1;
}

const char *nspire_saves_line(void)
{
  int place = note_pending, save;
  const char *line = NULL;

  note_pending = 0;
  /* (each one line of a toast: OSD_TEXT_MAX 63 characters, the band's line 53) */
  switch (place)
  {
  case DRIVE_E_GONE:
    line = (nspire_usbhub_recovering && nspire_usbhub_recovering()) || (nspire_usbhub_doubtful && nspire_usbhub_doubtful() == 1) ?
           "USB drive coming back: saves on the calculator" :
           "No USB drive: saves on the calculator";
    break;
  case DRIVE_E_PROTECTED: line = "USB drive read-only: saves on the calculator"; break;
  case DRIVE_E_FULL: line = "USB drive full: saves on the calculator"; break;
  case DRIVE_E_FAILED: line = "USB drive failed: saves on the calculator"; break;
  /* ms17: in, and not opened for lack of memory (tried again before each save or load that may open it) */
  case DRIVE_E_MEMORY: line = "USB drive needs memory: saved on the calculator"; break;
  default: break;
  }
  save = line ? NSPIRE_SAVE_NOTE_NONE : nspire_backup_drive_note();   /* (the save's stays for the next line) */
  if (!line && save == NSPIRE_SAVE_NOTE_CALC_NEWER)
    line = "Calculator's save was newer: drive updated";
  if (!line && save == NSPIRE_SAVE_NOTE_LEFT)
    line = "USB drive has another save: left as it is";
  if (!line && place == NOTE_BACK)
    line = "Saves are on the USB drive again";
  if (line && nspire_link_log)
    nspire_link_log("saves: said \"%s\"", line);
  return line;
}

void nspire_saves_say(void)
{
  const char *up;

  if (!waiting)
  {
    int place = note_pending;
    if (!place && !nspire_backup_drive_noted())
      return;   /* (what the frames find: nothing waits) */
    waiting = nspire_saves_line();
    waiting_place = waiting && place && place != NOTE_BACK;
    if (!waiting)
      return;
  }
  up = nspire_osd_visible();
  if (up && up[0])
    return;   /* the toast that is up stays its time ("Saved to slot 3", "Resumed from slot 2") */
  nspire_osd_message("%s", waiting);
  waiting = NULL;
  waiting_place = 0;
}
