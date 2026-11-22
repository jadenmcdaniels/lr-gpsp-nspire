#ifndef NSPIRE_DRIVE_H
#define NSPIRE_DRIVE_H

#include <stdint.h>
#include <stdio.h>

#define DRIVE_PREFIX "usb:/"
#define DRIVE_LABEL "USB drive"
/* DRIVE_NOMEM (ms17): in, and not opened for lack of memory (the driver's
 * "Not enough memory for the USB drive"): a save or load that may open it
 * first gives up the menus' caches and tries the opening again, once a
 * drive; the drive's page's Select tries again too. */
enum drive_state { DRIVE_OFF, DRIVE_NONE, DRIVE_ATTACHED, DRIVE_READY, DRIVE_ERROR, DRIVE_NOMEM };

int nspire_drive_enabled(void);
int nspire_drive_path(const char *path);
void nspire_drive_start(void);
void nspire_drive_stop(void);
/* R11w (2026-11-15): the drive driver stopped within ms (the exit's USB
 * budget): 0 done, -1 a callback still inside gpSP then (not stopped: the
 * caller restarts the calculator). nspire_drive_stop is this with the exit's
 * budget, restarting itself if it must (atexit). */
int nspire_drive_stop_within(unsigned ms);
/* (r19-usb-exit) The quit's stop with nothing ever offered to the OS's USB
 * drivers (nspire_usbpad_exit_untouched): no wait for a callback that never
 * ran; 0 done, -2 not so (a drive, a callback): then the usual stop. */
int nspire_drive_stop_quiet(void);
/* Main-loop notification only; no USB transfer or filesystem read here. */
void nspire_drive_poll(void);
void nspire_drive_frame(void);
/* Menus only: gives the OS time and mounts a newly attached drive. */
unsigned nspire_drive_menu_tick(void);
/* Menus only: a drive that is in and could not be opened is tried again. */
void nspire_drive_retry(void);
enum drive_state nspire_drive_state(void);
const char *nspire_drive_summary(void);
unsigned nspire_drive_epoch(void);
int nspire_drive_is_game(const char *name);
/* Allocated names only. Both arrays and each name belong to the caller. */
int nspire_drive_list(const char *dir, char ***games, unsigned *n_games,
                      char ***dirs, unsigned *n_dirs);
FILE *nspire_drive_fopen(const char *path, const char *mode);
void nspire_drive_game_begin(const char *path);
void nspire_drive_game_end(int loaded);
const char *nspire_drive_load_error(const char *core_reason);
/* A separate view of the drive's folders; returns its full usb:/ path. */
int nspire_drive_browser(char *result, unsigned size);

/* ---- Saves and states on the drive (2026-11-17, final/usbsave8) ----
 * Whole small files under "usb:/...", read and written at safe points only
 * (the menus, a game's start and end): each call blocks until the drive has
 * answered, tens of ms to seconds. Nothing is written to a drive by anything
 * else in gpSP. */
/* A drive is in, opened and takes writes (asked of it once: its write-protect
 * switch). Opens one that is attached and not opened yet (its first commands:
 * up to a few seconds); _now: does not (a drive that is only attached is not
 * ready). nspire_drive_usable: in, opened (may_open: as _ready) and not known
 * to be write-protected, with nothing asked of the drive: for every draw. */
int nspire_drive_ready(void);
int nspire_drive_ready_now(void);
int nspire_drive_usable(int may_open);
/* Why it is not ready, or why the last put, remove or rename failed. */
enum { DRIVE_E_NONE = 0, DRIVE_E_GONE, DRIVE_E_PROTECTED, DRIVE_E_FULL, DRIVE_E_FAILED, DRIVE_E_MEMORY };
int nspire_drive_error(void);
/* 1: the file is on the drive, whole (written under another name, synced,
 * then renamed: nspire-usbms's usbms_put; its folders made). 0: not, and the
 * file that was there is as it was. */
int nspire_drive_put(const char *path, const void *data, unsigned long size);
/* What a put that was cut short (the drive pulled out) left of the file and
 * its "<path>.tmp", made one file again: the new one when it was written
 * whole, else the old one (usbms_settle; a put does it first). discard: the
 * .tmp dropped, the file of the name as it is (usbms_discard): for a .tmp the
 * caller read and found not whole. A .tmp may share its clusters with the
 * file: it is never removed by its own name. 1: done, or nothing was cut. */
int nspire_drive_settle(const char *path);
int nspire_drive_discard(const char *path);
/* The file's bytes (malloc'd; *size of them), NULL when it is not there,
 * can't be read or is over limit bytes; errno then: ENOENT (not there, or
 * no drive), EFBIG (over limit), ENOMEM (no memory for it), EIO (a read
 * failed). get_into: into buf (cap bytes): 1, or 0 with the same errno. */
void *nspire_drive_get(const char *path, unsigned long limit, unsigned long *size);
int nspire_drive_get_into(const char *path, void *buf, unsigned long cap, unsigned long *size);
int nspire_drive_remove(const char *path);                    /* 1 removed */
int nspire_drive_rename(const char *from, const char *to);    /* 1 renamed (to must not exist) */
/* 1 when the file is there: its size and when it was written (seconds, 0 unknown). */
int nspire_drive_stat(const char *path, unsigned long *size, unsigned long *mtime);
/* Every file in a folder: its name, size and when it was written (seconds,
 * 0 unknown); each returns 0 to stop. 0, or -1 when the folder can't be read
 * (none: no names, 0). */
/* ms17: the bytes the drive's driver holds now (its read cache and transfer
 * memory while a drive is open, its write buffer through a burst of writes,
 * open files; nspire-usbms usbms_mem_held), for the memory log. open_room:
 * what opening a drive takes, while one could still be opened (the driver
 * started, no drive open): the saves' keep leaves room for it. trim: the
 * write buffer given back now (memory wanted). */
unsigned long nspire_drive_mem_held(void);
unsigned long nspire_drive_open_room(void);
void nspire_drive_trim(void);
int nspire_drive_dir(const char *dir,
                     int (*each)(const char *name, unsigned long size, unsigned long mtime, void *arg), void *arg);

#endif
