/* Compressing and uncompressing ROMs on the calculator (the ROM manager,
 * nspire_rom_manager.c, and a zip too big to unpack: nspire_loading.c): a
 * .gba or a .zip becomes a .gbz of it (nspire_gbz.h), which gpSP reads
 * page by page with little memory; a .zip or a .gbz becomes a .gba. The new
 * file is written next to the old one under a temporary name, read back
 * and checked, and only then does the old one go. The game's saves keep
 * working: they're named after the ROM (inside a zip, its file name there:
 * nspire_states.h). */
#ifndef NSPIRE_ROM_CONVERT_H
#define NSPIRE_ROM_CONVERT_H

#include <stddef.h>

enum { ROM_OTHER, ROM_PLAIN, ROM_ZIP, ROM_GBZ };

/* What a ROM file is, from its first bytes (and a plain one's name). */
int rom_kind(const char *path);

/* Called now and then with the bytes done of the whole job (writing, then
 * checking). Nonzero stops the job: it then ends as rom_convert_cancelled,
 * with nothing changed. */
typedef int (*rom_progress_fn)(unsigned long done, unsigned long total);
extern const char rom_convert_cancelled[];
/* Why a job failed when a write came back short: the storage is full. */
extern const char rom_convert_no_room[];

/* Set by rom_compress: it made the .gbz but kept the zip, which holds more
 * than the ROM (another game, a patch, a save). */
extern int rom_convert_kept_original;

/* "dir/Game.gba.tns" or "dir/Game.zip.tns" with "Game.gba" in it ->
 * "dir/Game.gbz.tns", named as tools/gbz.py names it. NULL when it worked
 * (new_path gets the new file), else why not; then nothing changed. */
const char *rom_compress(const char *path, char *new_path, size_t size, rom_progress_fn progress);
/* "dir/Game.zip.tns" (or .gbz.tns) -> "dir/Game.gba.tns". The same. */
const char *rom_uncompress(const char *path, char *new_path, size_t size, rom_progress_fn progress);

/* The .gbz a zip would become (gbz_path), when that's for gpSP to make on
 * its own: 1 when the ROM in the zip is a .gba (as tools/gbz.py names its
 * .gbz, so the saves carry over) and no file has the .gbz's name yet; -1
 * when the .gbz is there already; 0 when the ROM isn't a .gba. */
int rom_zip_gbz_name(const char *path, char *gbz_path, size_t size);

#endif
