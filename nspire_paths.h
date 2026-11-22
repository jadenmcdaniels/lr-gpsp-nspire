/* Where gpSP keeps its files, as PocketSNES does: one "zgpSP_files" folder
 * in My Documents holds every game's in-game save, save states and own
 * settings, and gpSP's own files (a copy of the settings for all games, the
 * file list's last folder and game orders, the speed test's and the detailed
 * counter's logs, the CPU speed's crash marker, the GBA BIOS). gpSP's folder
 * keeps only the program and the games. The settings for all games
 * are also kept inside the program file itself (nspire_selfstore.h), so they
 * go wherever _gpSP.tns goes. The folder isn't in the ROM folder, so
 * _gpSP.tns is the first thing there (a folder is listed above every file).
 * The OS lists folders A to Z, ignoring case, so the z puts the saves
 * folder below the game folders (PocketSNES's is zPocketSNES_files).
 * gpSP makes the folder when it starts, wherever it is (no gba folder
 * needed), so a new calculator gets it too.
 *
 * Earlier builds kept all of these next to the ROMs and next to gpSP, and
 * builds after that in My Documents/gpSP_saves, then zgpSP_saves:
 * nspire_move_old_files and nspire_move_old_documents_files move them in.
 * The GBA BIOS goes in the folder too, from wherever it is in My Documents
 * or the folders in it (nspire_tidy_save_folder). */
#ifndef NSPIRE_PATHS_H
#define NSPIRE_PATHS_H

#include <stddef.h>

#define SAVE_FOLDER "zgpSP_files"
/* The folder's names before: gpSP_saves, then zgpSP_saves. */
#define OLD_SAVE_FOLDERS { "zgpSP_saves", "gpSP_saves" }
/* The settings for all games (nspire_config.c), and the name it had. */
#define CONFIG_FILE     "gpsp.cfg.tns"
#define OLD_CONFIG_FILE "gpsp_qol.cfg.tns"
#define BIOS_FILE       "gba_bios.bin.tns"

/* The program file gpSP runs from (argv[0]); "" when unknown. */
extern char nspire_program_path[512];

/* "<My Documents>/zgpSP_files/<name>" (the folder itself for name ""). */
void nspire_save_file(char *out, size_t size, const char *name);
/* (r19-usb-exit) A log written a line at a time (fprintf, fputs) reaches the
 * OS a buffer at a time: Ndless's newlib takes BUFSIZ (1 KB: its _fstat
 * gives no block size), and each buffer is one OS fwrite (libsyscalls
 * _write). The CX II's evening quit wrote its 36 KB pad log in 153 ms, 4.2
 * ms a KB, where a state's 8 KB parts take 0.75 ms a KB (gpsp_states_time):
 * the calls cost, not the bytes, and a CX's file calls ~3.4 times a CX II's.
 * The logs written at the quit (the pad log, the perf log with the sound's
 * seconds, the state times) take a 16 KB buffer (newlib's, freed at the
 * close; without the memory for it, newlib's own fallback). f: a FILE *. */
#define NSPIRE_LOG_BUFFER 16384
void nspire_log_buffered(void *f);
/* Makes the saves folder if it isn't there. */
void nspire_save_dir_make(void);

/* A path in the form "/documents/gba/Game.zip.tns": the OS's own form of
 * the current folder ("\documents\gba\") turned to /, no // or / at the
 * end. Files are moved and renamed by these. */
void nspire_normalize_path(char *out, size_t size, const char *path);

/* The file with the order of the games in the ROM folder dir (the file
 * list's): "gpsp_order_gba.txt.tns" for My Documents/gba,
 * "gpsp_order_gba_rpg.txt.tns" for gba/rpg, "gpsp_order.txt.tns" for My
 * Documents itself. */
void nspire_order_file(char *out, size_t size, const char *dir);

/* Moves a game's files (in-game save, plain or gzipped, states, own
 * settings) named after from_base to the same names after to_base. A file
 * to_base already has stays, and so does the one that would replace it. */
void nspire_move_game_files(const char *from_base, const char *to_base);

/* Moves into the saves folder what earlier builds kept in the ROM folder
 * dir: the GBA games' in-game saves, states and own settings, and the
 * folder's game order (gpsp_order.txt.tns); with with_program_files, dir is
 * gpSP's own folder and its files there move in too: settings files
 * (*.cfg.tns, older builds' too), the last folder, logs, the BIOS. A file
 * the saves folder already has wins: the old one stays where it is. Cheap
 * when there's nothing to move. */
void nspire_move_old_files(const char *dir, int with_program_files);

/* Moves into the saves folder what earlier builds left in My Documents:
 * all of My Documents/zgpSP_saves and gpSP_saves (the folder's names
 * before; each goes once it's empty: a file the folder already has wins
 * and stays there) and gpSP's settings files in My Documents itself (older builds
 * wrote "..cfg.tns" in whatever folder their file list had open). */
void nspire_move_old_documents_files(void);

/* After the moves: gpsp_qol.cfg.tns (the settings file's name before)
 * becomes gpsp.cfg.tns, over an older build's gpsp.cfg.tns (an older
 * format, read only when there's nothing newer); and with no
 * gba_bios.bin.tns in the folder, one in My Documents or a folder in it
 * moves in. */
void nspire_tidy_save_folder(void);

/* The OS's list of the files in My Documents is a copy it keeps, and it
 * doesn't see files a program adds, removes or renames. Reading them all
 * again (refresh_osscr at exit) takes many seconds with games and states,
 * so it's done only when the list would show something that isn't there:
 * a .tns name in the saves folder at start (nspire_file_list_start) gone at
 * exit, or a change elsewhere (a move, a new folder, the ROM manager, a .tns
 * removed outside the saves folder: nspire_file_list_touched). Files only
 * added to the saves folder (a new state, a log) don't count: the list shows
 * them after the next refresh. fast17: the exit doesn't read the folder
 * again; every remove() and rename() gpSP makes tells nspire_file_gone
 * (the linker's --wrap on the calculator: nspire_fswrap.c; host_hw.c's
 * wrappers on the PC), and the exit looks only at the names that went (a
 * stat each). nspire_file_list_why: the exit's step line after
 * nspire_file_list_changed ("file list looked at (changed: why)"). */
void nspire_file_list_start(void);
void nspire_file_list_touched(void);
void nspire_file_gone(const char *path);
int nspire_file_list_changed(void);
const char *nspire_file_list_why(void);
/* 1 if the saves folder had a file of this name (any case) at start (or
 * that isn't known), 0 if not: a look-up in what the start read, no file
 * call. */
int nspire_save_folder_had(const char *name);
/* Counts nspire_file_list_touched: what the menus keep about the files
 * (nspire_states.c) is read again when it changes. */
extern unsigned nspire_save_folder_moves;

/* Replace a completed temporary file without losing the previous file if
 * rename fails. .rollback is reserved for the previous complete version;
 * recover it before opening a file after an interrupted replacement. */
int nspire_file_replace(const char *tmp, const char *path);
/* (r19-snd) nspire_file_replace a step at a time (one or two OS calls each,
 * the same calls in the same order): begin (0: names it can't take), then
 * step until it isn't NSPIRE_REPLACE_BUSY: 1 replaced, 0 not (errno as the
 * one-call version leaves it). */
#define NSPIRE_REPLACE_BUSY (-1)
struct nspire_replace
{
  int step;
  const char *tmp, *path;
  char old[640];
};
int nspire_file_replace_begin(struct nspire_replace *r, const char *tmp, const char *path);
int nspire_file_replace_step(struct nspire_replace *r);
int nspire_file_recover(const char *path);
/* Explicitly delete the file and its temporary/recovery versions. */
int nspire_file_delete(const char *path);

#endif
