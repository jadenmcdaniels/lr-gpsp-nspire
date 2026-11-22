/* Saves in two places (r16, the menus' side; docs/ui/spec.txt, the widget spec):
 * the calculator's zgpSP_files, and a USB drive on the port (or the hub).
 *
 * What the menus do with it (gui.c, nspire_ui_screens.c), while a drive can
 * hold saves (nspire_places_ready):
 *   - Settings, Saving: a cell "SAVES ON" / "Calculator" or "USB drive"
 *     (nspire_saves_on): where Save puts a state, and where the paused
 *     menu's Save and Load open.
 *   - The paused menu's Save and Load: the places as tabs over the strip
 *     ("[tab] CALCULATOR  USB DRIVE"); tab switches them; the strip shows that
 *     place's slots; enter saves or loads there. On Load, "[c] COPY TO USB
 *     DRIVE" (or "COPY TO CALCULATOR") at the right: c copies the picked slot
 *     to the other place's next free slot.
 * The USB drive code fills nspire_drive_saves with its functions; while
 * it is NULL nothing of this shows and the menus are as before. On the PC,
 * GPSP_HOST_DRIVE_SAVES=1 gives a stand-in with three made-up slots
 * (tests/host/ui_wired.py, scene "drive_saves"). */
#ifndef NSPIRE_PLACES_H
#define NSPIRE_PLACES_H

#include "common.h"

enum { PLACE_CALCULATOR, PLACE_DRIVE, PLACES };

/* "Calculator", "USB drive": the tabs' and the setting's words */
extern const char *const nspire_place_names[PLACES];

/* Saving, "Saves on": PLACE_CALCULATOR or PLACE_DRIVE (not in the settings
 * file yet: the USB drive code keeps it where it keeps its own) */
extern u32 nspire_saves_on;

/* A place's saves, slots numbered from 0 as the calculator's are. */
struct nspire_place_ops
{
  int (*ready)(void);                       /* 1: the drive is in and can hold saves now */
  int (*exists)(int slot);                  /* 1: a state is in slot */
  int (*next)(void);                        /* the slot a new save goes to, -1 when none is free */
  int (*picture)(int slot, u16 *pic);       /* its picture (STATE_PIC_W x STATE_PIC_H): 1, 0 none, -1 not read
                                               yet (the menu asks again; it must not block) */
  long (*time)(int slot);                   /* when it was saved (UTC seconds), 0 not known */
  int (*save)(int slot);                    /* the paused game's state into slot: 1 done */
  int (*load)(int slot);                    /* 1 loaded, 0 empty, -1 couldn't */
  int (*copy)(int from_place, int slot, int *to_slot);   /* slot of from_place to the other place's next free
                                                            slot (*to_slot): 1 done */
  /* UI17 (the Saves place's drive tab): a state of the game whose ROM is rom_path deleted there: 1 done. NULL:
   * not offered. What is kept of the place's slots is then stale: the caller opens the game's states again. */
  int (*remove)(const char *rom_path, int slot);
};

/* NULL: no saves on a drive in this build */
extern const struct nspire_place_ops *nspire_drive_saves;

/* 1 while a drive can hold saves (nspire_drive_saves set, and ready) */
int nspire_places_ready(void);

#endif
