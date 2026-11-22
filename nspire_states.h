/* Save state slots 0 to 999.
 *
 * A game's states are in the saves folder (nspire_paths.h), named the way
 * gpSP always named them: "<ROM file name without .tns><slot>.svs.tns", so
 * slot 12 of "Pokemon.gba.tns" is "Pokemon.gba12.svs.tns". A compressed ROM
 * ("Pokemon.gbz.tns") or a zip with "Pokemon.gba" in it uses the same names
 * as the plain one. Earlier builds kept them next to the ROM.
 *
 * States are written zlib-compressed behind a 32-byte header ("GPSZ") that
 * also counts saves, so the newest state is the one with the highest count;
 * with Compress saves off, the header says "stored" and the state follows
 * it as it is.
 * Older gpSP states (416 KB, uncompressed) still load. They count as older
 * than this build's, except while a game has none of this build's (it has
 * only states from older builds, or the first QoL build's, whose headers
 * lack the flag): then the newest is the most recently modified file. */
#ifndef NSPIRE_STATES_H
#define NSPIRE_STATES_H

#include <stddef.h>

#define STATE_SLOTS 1000

/* The name a game's own files are built from: the ROM's file name without
 * its last extension, with ".gbz" turned into ".gba" ("Pokemon.gba"); for a
 * zip, the ROM's name in it. */
void nspire_game_name(const char *rom_path, char *out, size_t size);
/* That name in the saves folder ("<saves folder>/Pokemon.gba"). */
void nspire_game_base(const char *rom_path, char *out, size_t size);
/* Before a game's files are touched (ZIP-6): finds out whether another game
 * of the same name owns them. If so, the game's base gets "~CODE" (its ROM's
 * game code) until another game is identified. */
void nspire_game_identify(const char *rom_path);
/* The game started: if no game owned its name, this one does now. */
void nspire_game_claim(void);
/* Where builds before the saves folder kept them: next to the ROM
 * ("dir/Pokemon.gba"). */
void nspire_game_old_base(const char *rom_path, char *out, size_t size);
/* Moves what earlier builds left in the game's folder (this game's files
 * and the other GBA games', and the folder's game order) into the saves
 * folder. Before a game's files are opened. */
void nspire_game_move_old_files(const char *rom_path);

/* Points the slot functions at a ROM and looks up which slots it has. */
void states_open_game(const char *rom_path);
void states_file_name(int slot, char *out, size_t size);

/* ---- The open game's states on a USB drive (2026-11-17, final/usbsave8) ----
 * The second place of nspire_places.h: "/gpsp/states/<name><slot>.svs" on the
 * drive, the calculator's names without ".tns", the same bytes. The slot
 * functions of this file (states_exists, states_save, states_load ...) stay
 * the calculator's. What is known of the drive's is kept in memory:
 * states_drive_exists, _next, _newest, _time and _picture (read_file 0)
 * answer from that and never touch the drive. states_drive_read, a picture
 * with read_file 1, _load, _save and states_place_copy read or write it: for
 * safe points only (the menus, a game's start and end), never a game's
 * frame. nspire_drive_saves.c calls them. */
/* The folder read (its names, sizes, times), the newest state's header and
 * picture: 1 known; 0 the drive can't be read. */
int states_drive_read(void);
/* 1 while what is kept is the open game's on the drive that is in. */
int states_drive_known(void);
int states_drive_exists(int slot);
int states_drive_count(void);
/* Where a new save goes there: after the highest slot (the lowest free one
 * once slot 1000 is used), -1: none free, or not known. */
int states_drive_next(void);
/* The newest there: the highest slot (-1 none), and its count (0 unknown). */
int states_drive_newest(void);
unsigned int states_drive_top(void);
long states_drive_time(int slot);
/* 1 its picture (in pic), 0 none, -1 not kept in memory (read_file 0). */
int states_drive_picture(int slot, unsigned short *pic, int read_file);
/* The game's state now into a free slot there: 1 done. 0: not written (the
 * slot is taken, the drive is full, gone, read-only: nspire_drive_error). */
int states_drive_save(int slot);
/* 1 loaded, 0 empty, -1 couldn't. */
int states_drive_load(int slot);
/* A slot's file copied to the other place's next free slot (*to_slot), the
 * same bytes: the calculator's to the drive (to_drive 1), or the drive's to
 * the calculator. STATES_COPY_SAME: the same state is there already, in
 * *to_slot; nothing written. Never over a state. */
enum { STATES_COPY_FAILED, STATES_COPY_DONE, STATES_COPY_SAME, STATES_COPY_OTHER };
int states_place_copy(int to_drive, int slot, int *to_slot);
/* save17: the drive's slot packed (compress 1) or stored (0), as
 * states_set_compressed does a calculator's slot; its age kept (the save
 * time in its header). 1 done, or it was that way already; 0 with the cause
 * in states_failure(STATES_SAVING). A safe point's call. */
int states_drive_set_compressed(int slot, int compress);
/* The newest of both places without clocks: a save in either place counts
 * from the highest count known of both. The calculator's highest count; and
 * a count the next saves must be above (the drive's, as kept from the runs
 * before: nspire_drive_saves.c). A game's start puts it back to 0. */
unsigned int states_newest_counter(void);
void states_counter_floor(unsigned int count);
/* The floor as it is (UI17: the Saves place opens another game's states and puts the open game's back, with
 * this). */
unsigned int states_counter_floor_now(void);
/* "usb:/gpsp/<folder>/<the game's name>" (with "~CODE" when another game owns
 * the name on the drive: "/gpsp/saves/<name>.id", as on the calculator), and
 * the name taken for this game once a file of it is written there. */
void nspire_game_drive_base(const char *rom_path, const char *folder, char *out, size_t size);
void nspire_game_drive_claim(void);

/* A state's picture of the game (newer states have one): STATE_PIC_W x
 * STATE_PIC_H in 5:6:5, 3/5 of the GBA's size. */
#define STATE_PIC_W 144
#define STATE_PIC_H 96
/* 1 if the slot's state has a picture (put in pic), 0 if not. */
int states_picture(int slot, unsigned short *pic);

/* Another game's states, for the game list, leaving the open game's as they
 * are: how many, the newest (-1: none), and its picture (1 if it has one). */
int states_peek(const char *rom_path, int *count, int *newest, unsigned short *pic);
/* The same, and when the newest was saved (its file's time, 0 if unknown). */
int states_peek_when(const char *rom_path, int *count, int *newest, unsigned short *pic, long *when);
/* When a slot of the open game was saved (save17: the time in its header, kept when it is packed or
 * stored; else, an older state's, its file's time), 0 if unknown. */
long states_time(int slot);

/* For the menus, which must not wait for files: what is kept of the games
 * looked up since gpSP started (their slots, the last pictures) answers at
 * once. states_peek_soon is states_peek_when (its result: 1 picture, 0 none)
 * and states_picture_soon states_picture, except that they return -1 when
 * the answer isn't kept and read_files is 0; with read_files 1 they read what
 * they must, but a game's slots are read one by one and states_yield (when
 * set) can stop that between two (it returns nonzero: a key went down): -1,
 * and the next call goes on where it stopped. */
int states_peek_soon(const char *rom_path, int *count, int *newest, unsigned short *pic, long *when,
                     int read_files);
int states_picture_soon(int slot, unsigned short *pic, int read_files);
/* states_time, the same way (-1: not known, and read_files 0). */
long states_time_soon(int slot, int read_files);
extern int (*states_yield)(void);
/* A game's start: as states_open_game, from what was kept of the game's
 * slots when all of them were read since gpSP started (and nothing moved
 * into the saves folder since). */
void states_open_game_kept(const char *rom_path);

int states_exists(int slot);
int states_count(void);
/* The most recently saved slot, or -1 if the game has none. */
int states_newest(void);
/* Where an auto-increment save goes: the slot after the highest one used, or
 * the lowest free slot once 999 is used. -1 when all 1000 are used. */
int states_next_new_slot(void);

/* 1 on success, 0 if the state couldn't be written (states_failure says why). */
int states_save(int slot);
/* The same over the game's frames (2026-11-17): begin takes the state and
 * its picture now (0: no memory), then states_save_step at each frame's
 * end does the next part (packing 16 KB of it, the picture, the OS's
 * write, its replace), BUSY until it says DONE or FAILED; in_game: the OS's
 * file calls with the sound's queue served (nspire_audio_os_call_begin);
 * next_is_file: the next part is an OS file call. states_save_finish does
 * the rest at once, as states_save does it (any other states_ call that
 * reads or changes slots does it first). */
enum { STATES_SAVE_IDLE, STATES_SAVE_BUSY, STATES_SAVE_DONE, STATES_SAVE_FAILED };
int states_save_begin(int slot);
/* (r19-fix) the same with a tag kept with its result (states_save_result_tag: the frames' message, 0 a
 * key's, 1 an autosave's) */
int states_save_begin_as(int slot, int tag);
int states_save_result_tag(void);
/* (r19-fix) a slot that has a state, or one being saved or asked for (no save finished for it) */
int states_exists_soon(int slot);
int states_save_step(int in_game);
int states_save_finish(void);
int states_save_busy(void);
int states_save_next_is_file(void);
/* (r19-snd) 1: the states' OS calls outside a game's frame with the sound's queue served (the menu's
 * own saves, loads and pictures, the load keys: nspire_host_main.c); 0 (the quit, a game's start and
 * its leaving, from the menu too: nspire_quit_game, nspire_start_game, nspire_leave_game; r19-fix): as
 * before */
extern int states_serve_os;
#ifdef NSPIRE_HOST
void states_drop_job_for_test(void);
#endif
/* (r19-snd) the next part is the state's take (at the frame's end the ring covers it) */
int states_save_next_is_take(void);
/* (r19-fix 3) The game is about to be changed under a save whose state is not taken
 * yet (taken at a frame's end after the key: a frame or a few, more with the waits):
 * STATES_CHANGE_KEEP (a restart): the game as it is now is what was asked for: taken now (the rest goes
 * on over the frames as before; with saves queued behind it, all of them now, the sound paused first);
 * STATES_CHANGE_BROKEN (the game jumped into nowhere): not taken but dropped, the slot keeps what it
 * had. A state taken already is the game before the change either way: its save goes on. */
enum { STATES_CHANGE_KEEP, STATES_CHANGE_BROKEN };
void states_save_before_change(int how);
/* Once, how a save begun by states_save_begin ended (DONE or FAILED, and
 * its slot), whoever finished it (IDLE: none since). */
int states_save_result(int *slot);
/* 1 on success, 0 if the slot is empty, -1 if the file isn't a state gpSP
 * can read (damaged, or from another emulator), or there was no memory for
 * it or its read failed (states_failure says which). */
int states_load(int slot);

/* ---- The memory saves and loads use, and why one failed (save17) ----
 * states_keep_buffers takes the memory a save and a load use (~1 MB: the
 * state's copy, its file, the packer's work) as each game's ROM loads
 * (states_drop_buffers before a ROM load, and at the quit), so a save
 * late in a long session needs no fresh memory; unless that would leave
 * less than 512 KB in one piece for everything else, even with the menus'
 * caches given up (states_drop_caches): then nothing is kept
 * (0); the ROM loader then retries after reducing its optional cache,
 * before mapping the new game. If it still cannot keep them, saves and
 * loads take their own as before, and the logs say so
 * loudly (states_log_buffers: the link log's line, gpsp_states_time.txt.tns).
 * The game's own save borrows the two big buffers while no state has them
 * (states_lend_buffers: 1, and their places; states_return_buffers). */
int states_keep_buffers(void);
/* r17: what the menus keep of the saves folder given up (the last 12
 * pictures, 27 KB each; the folder's names; other games' slots): the bytes
 * freed. A take that would leave less than 512 KB in one piece does it and
 * tries again. Read again when next needed. */
long states_drop_caches(void);
void states_drop_buffers(void);
void states_log_buffers(void);
int states_lend_buffers(unsigned char **a, unsigned long *a_size, unsigned char **b, unsigned long *b_size);
void states_return_buffers(void);
/* The memory free looked at (the largest block, up to 1 MB: one malloc and
 * its free at a time, so nothing else that allocates meanwhile goes short),
 * as what into gpsp_states_time.txt.tns's list of the last 16; at most
 * every 10 s unless now (saves and loads look each time, the menus every
 * 10 s). */
void states_memory_look(const char *what, int now);
/* Why the last save, load or copy (to the other place) failed: memory
 * (free: the largest block free then), the storage full (free: the bytes the
 * OS took before it refused, -1 unknown), a file call (step: "create",
 * "write", "close", "rename", "open", "read", "rollback"), the file damaged,
 * not a state this gpSP reads (another build's or emulator's), the USB
 * drive. err: errno then (0: none set); need: the bytes it needed. Each
 * failure's line is in the link log too ("states: save slot 38 failed: ...").
 * A save's, a load's and a copy's are kept apart. */
enum { STATES_WHY_NONE, STATES_WHY_MEMORY, STATES_WHY_FULL, STATES_WHY_FILE, STATES_WHY_DAMAGED, STATES_WHY_DRIVE,
       STATES_WHY_OTHER, STATES_WHY_FOREIGN };
enum { STATES_SAVING, STATES_LOADING, STATES_COPYING };
struct states_failure
{
  int why;
  const char *step;
  int err;
  unsigned long need;
  long free;
};
const struct states_failure *states_failure(int what);
/* The cause in a few plain words, for "Couldn't save to slot 3: ...": "not
 * enough memory", "storage full, 12 KB free", "file write failed" (25 at
 * most: a toast keeps to the band's one line, nspire_osd.c). */
const char *states_failure_words(int what);
/* The same for another file's failure (the game's own save, nspire_backup.c): the cause classified as a
 * save's is (memory: the largest block measured; a file call: full storage only on ENOSPC, or, not in a
 * game's frame, when the space measured where temp was is under need), and its words. */
void states_failure_memory(struct states_failure *f, const char *step, unsigned long need);
void states_failure_file(struct states_failure *f, const char *step, int err, long wrote, unsigned long need,
                         const char *temp, int in_game);
const char *states_failure_text(const struct states_failure *f);
/* The same for any state file (0: it can't be opened). */
int states_load_file(const char *path);

/* For the save manager: a state file's size and whether it's compressed
 * (0 if the slot is empty), packing or unpacking one (1 if it is that way
 * afterwards; its place among the newest stays), and deleting one. */
int states_file_info(int slot, long *bytes, int *compressed);
int states_set_compressed(int slot, int compress);
int states_delete(int slot);

/* The game's in-game save, the same way (nspire_backup.c): its file is the
 * plain .sav or the gzipped .sav.gz. Deleting it leaves the game's memory
 * alone; the file comes back when the game saves again. */
int nspire_backup_info(long *bytes, int *compressed);
int nspire_backup_set_compressed(int compressed);
int nspire_backup_delete(void);
void nspire_load_cartridge_backup(void);
/* The same for any game's save, by its ROM's path (UI17: the Saves place): the open game's through the
 * functions above, another's as a file. */
int nspire_backup_info_of(const char *rom_path, long *bytes, int *compressed);
int nspire_backup_set_compressed_of(const char *rom_path, int compressed);
int nspire_backup_delete_of(const char *rom_path);
/* R6 write-then-rename, shared with the game order file (UI-3). The new
 * contents are in "<tmp>"; safely replace 'name' while retaining the
 * previous file on failure (ok 0: tmp is dropped), 1 when done. */
int nspire_tmp_take_place(const char *tmp, const char *name, int ok);
/* At load: a "<name>.tmp" next to 'name' is dropped; one alone is taken
 * when whole(tmp), else dropped. */
void nspire_tmp_recover(const char *name, int (*whole)(const char *tmp));
/* The ROM manager gave the running game another file (its saves moved,
 * unless the new name had some): its save is written when it's left. */
void nspire_backup_renamed(void);
/* The game's save written over its frames (2026-11-17, nspire_backup.c):
 * update_backup starts it, nspire_backup_step does the next part at a
 * frame's end (next_is_file: an OS file call), nspire_backup_finish the
 * rest at once (update_backup_force and anything that reads or names the
 * save do it first). */
void nspire_backup_step(void);
int nspire_backup_next_is_file(void);
int nspire_backup_busy(void);
void nspire_backup_finish(void);
#ifdef NSPIRE_HOST
void nspire_backup_drop_job_for_test(void);   /* (a reset between two parts: the files as they are) */
#endif

/* The game's save on a USB drive (nspire_backup.c: its copy there, when it
 * is written, and which of the two is taken at a game's start; while
 * nspire_saves_on_drive, nspire_drive_saves.h).
 * nspire_backup_drive_load: at a game's start, its save loaded, before it runs.
 * nspire_backup_drive_sync: at a safe point.
 * nspire_backup_drive_note: once, what the player is to be told.
 * nspire_backup_copy_to_drive: the save manager's copy (STATES_COPY_*).
 * nspire_backup_drive_top: the highest count of the game's states on a drive,
 * as kept beside the calculator's save from the runs before (0 none); _set
 * keeps a higher one. */
enum { NSPIRE_SAVE_NOTE_NONE, NSPIRE_SAVE_NOTE_CALC_NEWER, NSPIRE_SAVE_NOTE_LEFT };
void nspire_backup_drive_load(void);
void nspire_backup_drive_sync(void);
int nspire_backup_drive_note(void);
int nspire_backup_drive_noted(void);   /* 1: there is one (not taken) */
int nspire_backup_copy_to_drive(int replace);
unsigned int nspire_backup_drive_top(void);
void nspire_backup_drive_top_set(unsigned int top);

/* How long the last saves and loads took, by part, to
 * gpsp_states_time.txt.tns in the saves folder (when gpSP closes). */
void states_timing_flush(void);
/* A menu's step and how long it took (ticks), for the same file: the paused
 * menu's opening, the file list's folder read. */
void states_menu_note(const char *what, unsigned ticks);
/* A line (printf style, under 160 bytes) for the timing file: a ROM load's steps. */
void states_timing_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* The game's state in memory, for the speed test: states_snapshot returns a
 * buffer (free() it) or NULL; states_restore puts it back. */
void *states_snapshot(void);
int states_restore(const void *snapshot);

#endif
