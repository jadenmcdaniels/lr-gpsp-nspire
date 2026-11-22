/* Saves on a USB drive (2026-11-17, final/usbsave8): the drive as the second
 * place of nspire_places.h, and what gpSP does by itself when Saving's
 * "Saves on" says USB drive. nspire_drive_saves.c; docs/usb-drive.txt "Saves
 * on the drive".
 *
 * The menus (final/ui7) show the place while its ready() says 1: the paused
 * menu's Save and Load have a USB DRIVE tab (save, load, copy through the
 * table), Settings, Saving has "Saves on". With Saves on: USB drive:
 *  - the game's own save has a copy on the drive (nspire_backup.c);
 *  - a state the game's frames save (the save key, autosave) is written to
 *    the calculator, as always, and copied to the drive at the next safe
 *    point; Save when I quit saves on the drive; Resume last save takes the
 *    newest state of both places;
 *  - without a drive that can be written all of it is the calculator's, as
 *    with Saves on: Calculator, said once.
 * Nothing here reads or writes the drive from a game's frame. */
#ifndef NSPIRE_DRIVE_SAVES_H
#define NSPIRE_DRIVE_SAVES_H

/* Sets nspire_drive_saves (nspire_places.h) to the drive's functions, unless
 * something else has (the PC's stand-in). Once, where drives are read. */
void nspire_drive_saves_start(void);
/* UI17: set while the Saves place has another game's states open (and that game identified): the drive's count
 * isn't kept beside the paused game's save then (keep_top). */
extern int nspire_saves_other_game;
/* 1 while Saves on says USB drive and a drive is in that takes gpSP's own
 * writes. Asks the drive nothing. */
int nspire_saves_on_drive(void);
/* A write gpSP made by itself (the save's copy) failed: the calculator is
 * used until the drive is plugged in anew (nspire_drive_error says why). */
void nspire_saves_drive_failed(void);
/* A game's start, its slots and its save loaded, before it runs: with Saves
 * on: USB drive the drive is opened and the game's states on it read. */
void nspire_saves_game_start(void);
/* Resume last save: the newest state of both places: its slot (-1 none), and
 * *on_drive 1 when it is the drive's (states_drive_load). */
int nspire_saves_resume(int *on_drive);
/* The game's frames saved a state in the calculator's slot. */
void nspire_saves_frame_saved(int slot);
/* A safe point (the menu about to open, a game being left): the states the
 * frames saved copied to the drive, the save's copy brought up to date.
 * may_open: a drive that is attached and not opened yet is opened (a wait). */
void nspire_saves_sync(int may_open);
/* Save when I quit: the state saved on the drive (1); 0: not (Saves on:
 * Calculator, no drive, or it failed): the calculator's slot is the caller's. */
int nspire_saves_exit_save(void);
/* Once: the one line the player is to be told (a toast), or NULL. */
const char *nspire_saves_line(void);
/* That line said as a toast once no other toast is up (the menu's "Saved to
 * slot 3" and the start's "Resumed from slot 2" stay their time first). At
 * the game's start, after the menu, and from the frames: a look at two
 * numbers while nothing waits, never the drive. */
void nspire_saves_say(void);

#endif
