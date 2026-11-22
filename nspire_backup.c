/* Cartridge save (SRAM / Flash / EEPROM) persistence for NSPIRE_LIBRETRO.
 *
 * An in-game save is the cartridge's memory as it is (the size says the
 * type), in the saves folder (nspire_paths.h): "<ROM name>.sav.tns", what
 * other gpSP builds and PC emulators read,
 * or with Compress saves on, gzipped: "<ROM name>.sav.gz.tns" (a PC unpacks
 * it like any .gz), as PocketSNES does. Writing one form removes the other;
 * loading takes the newer of the two. A compressed ROM (.gbz.tns) or a zip
 * uses the same files as the plain ROM. */

#include "common.h"
#include "gba_memory.h"
#include "streams/file_stream.h"
#include "nspire_states.h"
#include "nspire_frontend.h"
#include "nspire_link.h"
#include "nspire_paths.h"
#include "nspire_audio.h"
#include "nspire.h"
#include "nspire_drive.h"
#include "nspire_drive_saves.h"
#include "nspire_dialog.h"
#include "nspire_keys.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

char backup_filename[512];

void nspire_backup_finish(void);   /* (below: a save being written over frames) */
static u32 backup_dirties;          /* nspire_backup_mark_dirty's, so far */
/* (the save's copy on a USB drive, at the end of this file) */
static int calc_loaded;             /* the calculator had a save of the open game, and it loaded */
static u32 dirties_at_load;
static void drive_forget(void);
static void drive_deleted(void);

static void backup_path(char *out)
{
  char base[500];   /* room for ".sav.tns" in the 512 of backup_filename */
  nspire_game_base(gamepak_filename, base, sizeof(base));
  snprintf(out, 512, "%s.sav.tns", base);
}

static void packed_backup_path(char *out)
{
  char base[496];
  nspire_game_base(gamepak_filename, base, sizeof(base));
  snprintf(out, 512, "%s.sav.gz.tns", base);
}

/* R6: a save is written to "<its name>.tmp" (no .tns: the OS's file list
 * doesn't show it) and takes its name only once it's whole: written in
 * place, a reset or a full storage mid-write (the CX II reset while linked
 * in R1-R4, at the Cable Club, which saves first) left half a save and no
 * other, and the game's progress was gone. A reset between the old one's
 * removal and the rename leaves only the new one's .tmp, whole: it's taken
 * at the next load (recover_tmp); one in its write leaves the old file and
 * a partial .tmp, which is dropped. */
static void tmp_path(char *out, size_t size, const char *name)
{
  snprintf(out, size, "%s.tmp", name);
}

/* (r19-fix 4) A save is written to "<its name>.part" and renamed "<its name>.tmp" only
 * once it is closed, whole: since r19-snd the write goes 8 KB a frame, and a .tmp cut after 1, 4 or 8 parts
 * of a 128 KB save is 8, 32 or 64 KB, a save's size: recover_tmp took it for whole and gave it the save's
 * name, newer than the .sav.gz it was to replace. A .tmp is now always whole; a .part never is: removed at
 * the next load, never read. (An older build's .tmp is judged as before, and a plain one shorter than the
 * .sav.gz's save is dropped: r19's cut at a part, with the save it was to replace beside it.) */
static void part_path(char *out, size_t size, const char *name)
{
  snprintf(out, size, "%s.part", name);
}

static int exists(const char *name);
/* the closed .part given the .tmp's name (one there before it: a leftover, replaced) */
static int part_sealed(const char *part, const char *tmp)
{
  if (exists(tmp))
    remove(tmp);
  return rename(part, tmp) == 0;
}

static void bk_file(const char *step, int err, long wrote, unsigned long need, const char *temp);
/* the same for a save written at once: a rename that fails kept as the cause, the .part dropped */
static int sealed_or_dropped(const char *part, const char *tmp)
{
  errno = 0;
  if (part_sealed(part, tmp))
    return 1;
  bk_file("rename", errno, -1, 0, NULL);
  remove(part);
  return 0;
}

static int exists(const char *name)
{
  struct stat st;
  return stat(name, &st) == 0;
}

static int valid_save_size(long sz)
{
  return sz == 0x200 || sz == 0x2000 || sz == 0x8000 || sz == 0x10000 ||
         sz == 0x20000;
}

/* A .tmp is whole when it's a save's size, or a gzip file whose last four
 * bytes (the size it unpacks to) are one; a partial one is dropped (SAV-4). */
static int tmp_is_whole(const char *tmp)
{
  FILE *f = fopen(tmp, "rb");
  long size = -1;
  u8 head[2], tail[4];
  int whole = 0;

  if (!f)
    return 0;
  if (fseek(f, 0, SEEK_END) == 0)
    size = ftell(f);
  if (valid_save_size(size))
    whole = 1;
  else if (size > 18 && size < 0x40000 && fseek(f, 0, SEEK_SET) == 0 &&
           fread(head, 1, 2, f) == 2 && head[0] == 0x1F && head[1] == 0x8B &&
           fseek(f, size - 4, SEEK_SET) == 0 && fread(tail, 1, 4, f) == 4)
    whole = valid_save_size((long)tail[0] | (long)tail[1] << 8 |
                            (long)tail[2] << 16 | (long)tail[3] << 24);
  fclose(f);
  return whole;
}

/* Also used by the game order file (nspire_game_list.c, UI-3): 'whole' tells
 * a finished .tmp from a cut one. */
void nspire_tmp_recover(const char *name, int (*whole)(const char *tmp))
{
  char tmp[1110];

  if (!nspire_file_recover(name))
    return;
  snprintf(tmp, sizeof(tmp), "%s.tmp", name);
  if (!exists(tmp))
    return;
  if (exists(name) || !whole(tmp))
    remove(tmp);
  else
    rename(tmp, name);
}

static void recover_tmp(const char *name)
{
  char part[530];
  part_path(part, sizeof(part), name);
  if (exists(part))
    remove(part);   /* (a write cut short: never whole) */
  nspire_tmp_recover(name, tmp_is_whole);
}

/* (r19-fix) an older build's plain .tmp, a save's size, shorter than the save the .sav.gz beside it holds:
 * cut at one of r19's 8 KB parts: dropped */
static void drop_short_tmp(const char *plain, const char *packed)
{
  char tmp[520];
  struct stat st;
  FILE *f;
  u8 tail[4];
  long isize = 0;

  tmp_path(tmp, sizeof(tmp), plain);
  if (stat(tmp, &st) != 0 || !exists(packed))
    return;
  f = fopen(packed, "rb");
  if (f && fseek(f, -4, SEEK_END) == 0 && fread(tail, 1, 4, f) == 4)
    isize = (long)tail[0] | (long)tail[1] << 8 | (long)tail[2] << 16 | (long)tail[3] << 24;
  if (f)
    fclose(f);
  if (valid_save_size(isize) && (long)st.st_size < isize)
    remove(tmp);
}

/* The written .tmp takes the save's name (ok), or is dropped. */
int nspire_tmp_take_place(const char *tmp, const char *name, int ok)
{
  if (!ok)
  {
    remove(tmp);
    return 0;
  }
  return nspire_file_replace(tmp, name);
}
#define take_place(tmp, name, ok) ((u32)nspire_tmp_take_place(tmp, name, (int)(ok)))

/* ---- Why the game's save wasn't written (save17) ----
 * r16 said "Couldn't write the game's save (storage full?)" for every
 * failure. Now the cause, classified as a state's is (nspire_states.c
 * states_failure_*: memory with the largest block measured; full storage
 * only when the OS says ENOSPC or, outside a game's frame, the space
 * measured is under the file's size; else the file call that failed), in
 * the toast ("Couldn't write game save: storage full, 12 KB free": the
 * band's one line) and a link log line with its step, errno and sizes.
 * (weak: builds without nspire_states.c say only that it failed) */
#pragma weak states_failure_memory
#pragma weak states_failure_file
#pragma weak states_failure_text
void nspire_link_log(const char *fmt, ...) __attribute__((format(printf, 1, 2), weak));
static struct states_failure bk_fail;
static int bk_in_game;   /* (a write now is in a game's frame: no storage measured) */

static void bk_memory(const char *step, unsigned long need)
{
  if (states_failure_memory)
    states_failure_memory(&bk_fail, step, need);
  else
  {
    memset(&bk_fail, 0, sizeof(bk_fail));
    bk_fail.why = STATES_WHY_MEMORY;
    bk_fail.step = step;
  }
}

static void bk_file(const char *step, int err, long wrote, unsigned long need, const char *temp)
{
  if (states_failure_file)
    states_failure_file(&bk_fail, step, err, wrote, need, temp, bk_in_game);
  else
  {
    memset(&bk_fail, 0, sizeof(bk_fail));
    bk_fail.why = STATES_WHY_FILE;
    bk_fail.step = step;
    bk_fail.err = err;
  }
}

static void bk_say(void)
{
  const char *words = states_failure_text ? states_failure_text(&bk_fail) : "it failed";
  nspire_osd_message("Couldn't write game save: %s", words);
  if (nspire_link_log)
    nspire_link_log("game save: not written: %s, at %s; errno %d; needs %lu bytes; %s %ld", words,
                    bk_fail.step ? bk_fail.step : "-", bk_fail.err, bk_fail.need,
                    bk_fail.why == STATES_WHY_MEMORY ? "the largest free block" :
                    bk_fail.why == STATES_WHY_FULL ? "storage free" : "bytes the OS took", bk_fail.free);
}

/* take_place, a rename that fails kept as the cause */
static u32 take_place_why(const char *tmp, const char *name, int ok)
{
  if (ok)
  {
    errno = 0;
    if (nspire_file_replace(tmp, name))
      return 1;
    memset(&bk_fail, 0, sizeof(bk_fail));
    bk_fail.why = STATES_WHY_FILE;
    bk_fail.step = "rename";
    bk_fail.err = errno;
    bk_fail.free = -1;
  }
  remove(tmp);
  return 0;
}

/* gzip (windowBits 31) or gunzip (47: gzip or zlib) from one buffer into
 * another; the bytes out, or 0. */
static u32 gzip_buffer(int pack, const u8 *in, u32 in_size, u8 *out, u32 out_size)
{
  z_stream z;
  int ok;

  memset(&z, 0, sizeof(z));
  ok = (pack ? deflateInit2(&z, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY)
             : inflateInit2(&z, 47)) == Z_OK;
  if (!ok)
    return 0;
  z.next_in = (Bytef *)in;
  z.avail_in = in_size;
  z.next_out = out;
  z.avail_out = out_size;
  ok = (pack ? deflate(&z, Z_FINISH) : inflate(&z, Z_FINISH)) == Z_STREAM_END;
  if (pack)
    deflateEnd(&z);
  else
    inflateEnd(&z);
  return ok ? (u32)z.total_out : 0;
}

static int file_time(const char *path, u32 *time)
{
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  /* st_mtim: Ndless's os.h undefines the st_mtime shorthand. */
  *time = (u32)st.st_mtim.tv_sec;
  return 1;
}

#define WRITE_BACKUP_DELAY 10

static u32 backup_update_count = WRITE_BACKUP_DELAY + 1;
/* SAV-1: a save file exists but didn't load (odd size, short read, damaged
 * gzip): the memory is blank and must not replace it. Nothing is written
 * until the game really saves; then the file is kept as "<name>.bad". */
static int backup_unreadable;

static void keep_unreadable(const char *name)
{
  char bad[530];
  int i;

  if (!exists(name))
    return;
  for (i = 0; i < 10; i++)
  {
    if (i)
      snprintf(bad, sizeof(bad), "%s.bad%d", name, i);
    else
      snprintf(bad, sizeof(bad), "%s.bad", name);
    if (!exists(bad))
    {
      if (rename(name, bad) != 0)
        backup_unreadable = 2;   /* kept where it is, and still blocks writes */
      return;
    }
  }
  backup_unreadable = 2;
}

/* What the save file holds, so that leaving the game writes it only when
 * it isn't the cartridge memory as it is:
 * - 0: not known, or the game wrote to its save memory since
 *   (nspire_backup_mark_dirty, from gba_memory.c's writes) or the running
 *   game got another file name (nspire_backup_renamed);
 * - BACKUP_FILE_DELETED: the save manager deleted it; it isn't written back
 *   from memory when the game is left, only once the game saves again;
 * - else the memory as it is, its size | 1 if gzipped: loaded (the only
 *   form there) or written since. Leaving writes nothing while that is the
 *   size and the form Compress saves asks for (a state can change the
 *   type).
 * (One variable, the one the deleted flag was: the game's write handlers
 * clear it as they did that flag, with the same code.) */
static u32 backup_file;
#define BACKUP_FILE_DELETED 1

static u32 nspire_backup_file_size(void);

#ifdef NSPIRE_BACKUP_CHECK
/* The PC tests (tests/host): the bytes last loaded or written, compared
 * whenever leaving writes nothing; a difference is a write to the save
 * memory that wasn't marked (core_tests checks the count, host_tests.py
 * fails on anything on stderr). */
static u8 backup_synced_bytes[sizeof(gamepak_backup)];
int nspire_backup_check_failures;
#endif

/* backup_file for the memory as it is, in a form (with no size, 0 or 1:
 * nothing is written either way). */
static u32 backup_file_now(int compressed)
{
  return nspire_backup_file_size() | (compressed != 0);
}

/* The save file holds the memory as it is, in that form. */
static void backup_file_synced(int compressed)
{
  backup_file = backup_file_now(compressed);
#ifdef NSPIRE_BACKUP_CHECK
  memcpy(backup_synced_bytes, gamepak_backup, nspire_backup_file_size());
#endif
}

void nspire_backup_mark_dirty(void)
{
  if (backup_unreadable)
  {
    char plain[512], packed[512];

    backup_unreadable = 1;
    backup_path(plain);
    packed_backup_path(packed);
    keep_unreadable(plain);
    keep_unreadable(packed);
    if (backup_unreadable == 2)
      return;   /* couldn't be set aside: nothing is written over it */
    backup_unreadable = 0;
  }
  backup_update_count = WRITE_BACKUP_DELAY;
  backup_file = 0;
  backup_dirties++;
}

void nspire_backup_renamed(void)
{
  nspire_backup_finish();
  if (backup_file != BACKUP_FILE_DELETED)
    backup_file = 0;
}

/* Sets the type from a save's size, as load_backup does; 0 for a size no
 * cartridge has. */
static int backup_type_from_size(u32 sz);

/* A gzipped save into the cartridge memory. The file is read with open and
 * read, as libretro-common's unbuffered files are: one read of the OS
 * straight into packed, not 1 KB at a time through stdio's buffer. */
static u32 load_packed_backup(const char *name)
{
  int fd = open(name, O_RDONLY);
  off_t size, got = 0;
  ssize_t n;
  u8 *packed;
  u32 sz = 0;

  if (fd < 0)
    return 0;
  if ((size = lseek(fd, 0, SEEK_END)) > 0 && size < 0x40000 &&
      lseek(fd, 0, SEEK_SET) == 0 && (packed = (u8 *)malloc((size_t)size)) != NULL)
  {
    while (got < size && (n = read(fd, packed + got, (size_t)(size - got))) > 0)
      got += n;
    if (got == size)
      sz = gzip_buffer(0, packed, (u32)size, gamepak_backup, sizeof(gamepak_backup));
    free(packed);
  }
  close(fd);
  if (!sz || !backup_type_from_size(sz))
  {
    memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
    return 0;
  }
  return 1;
}

void nspire_load_cartridge_backup(void)
{
  char packed[512];
  u32 plain_time = 0, packed_time = 0;
  int has_plain, has_packed, loaded, loaded_compressed = 0, bad_packed = 0;

  nspire_backup_finish();   /* (the last game's, under its own name) */

  backup_file = 0;
  calc_loaded = 0;
  dirties_at_load = backup_dirties;
  drive_forget();
  if (!gamepak_filename[0])
    return;
  backup_unreadable = 0;
  backup_path(backup_filename);
  packed_backup_path(packed);
  if (!nspire_file_recover(backup_filename) || !nspire_file_recover(packed))
  {
    backup_unreadable = 2;
    return;
  }
  drop_short_tmp(backup_filename, packed);
  recover_tmp(backup_filename);
  recover_tmp(packed);
  has_plain = file_time(backup_filename, &plain_time);
  has_packed = file_time(packed, &packed_time);
  if (has_packed && (!has_plain || packed_time >= plain_time))
  {
    loaded = load_packed_backup(packed);
    loaded_compressed = loaded != 0;
    bad_packed = !loaded;
    if (!loaded && has_plain)
      loaded = load_backup(backup_filename);
  }
  else
    loaded = load_backup(backup_filename);
  backup_unreadable = bad_packed || ((has_plain || has_packed) && !loaded);
  if (loaded && !backup_unreadable && has_plain != has_packed)
    backup_file_synced(loaded_compressed);
  backup_update_count = WRITE_BACKUP_DELAY + 1;
  /* (for the drive's copy, below: whether the calculator has a save of this game) */
  calc_loaded = loaded && !backup_unreadable;
  dirties_at_load = backup_dirties;
  drive_forget();
}

/* (noinline: one copy, not one in each caller) */
static __attribute__((noinline)) u32 nspire_backup_file_size(void)
{
  switch (backup_type)
  {
    case BACKUP_SRAM:
      return 0x8000;

    case BACKUP_FLASH:
      if (flash_bank_cnt == FLASH_SIZE_128KB)
        return 0x20000;
      return 0x10000;

    case BACKUP_EEPROM:
      if (eeprom_size == EEPROM_512_BYTE)
        return 0x200;
      return 0x2000;

    default:
      break;
  }
  return 0;
}

u32 load_backup(char *name)
{
  RFILE *fd;
  int64_t sz64;
  u32 sz;

  if (!nspire_file_recover(name))
  {
    backup_unreadable = 2;
    return 0;
  }
  fd = filestream_open(name, RETRO_VFS_FILE_ACCESS_READ,
                       RETRO_VFS_FILE_ACCESS_HINT_NONE);
  if (!fd)
  {
    memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
    return 0;
  }

  sz64 = filestream_get_size(fd);
  if (sz64 <= 0 || sz64 > (int64_t)sizeof(gamepak_backup))
  {
    filestream_close(fd);
    memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
    return 0;
  }

  sz = (u32)sz64;
  if (filestream_read(fd, gamepak_backup, sz) != (int64_t)sz)
  {
    filestream_close(fd);
    memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
    return 0;
  }
  filestream_close(fd);

  if (!backup_type_from_size(sz))
  {
    memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
    return 0;
  }
  return 1;
}

static int backup_type_from_size(u32 sz)
{
  switch (sz)
  {
    case 0x200:
      backup_type = BACKUP_EEPROM;
      eeprom_size = EEPROM_512_BYTE;
      break;

    case 0x2000:
      backup_type = BACKUP_EEPROM;
      eeprom_size = EEPROM_8_KBYTE;
      break;

    case 0x8000:
      backup_type = BACKUP_SRAM;
      break;

    case 0x10000:
      backup_type = BACKUP_FLASH;
      flash_bank_cnt = FLASH_SIZE_64KB;
      flash_device_id = FLASH_DEVICE_MACRONIX_64KB;
      break;

    case 0x20000:
      backup_type = BACKUP_FLASH;
      flash_bank_cnt = FLASH_SIZE_128KB;
      flash_device_id = FLASH_DEVICE_MACRONIX_128KB;
      break;

    default:
      return 0;
  }

  backup_type_reset = backup_type;
  flash_mode = FLASH_BASE_MODE;
  flash_command_position = 0;
  flash_bank_num = 0;
  eeprom_mode = EEPROM_BASE_MODE;
  eeprom_address = 0;
  eeprom_counter = 0;
  return 1;
}

/* The save gzipped into name. */
static u32 save_packed_backup(const char *name)
{
  u32 size = nspire_backup_file_size(), bound, packed_size;
  u8 *packed;
  FILE *f;
  int ok;

  if (!size || (backup_type != BACKUP_SRAM && backup_type != BACKUP_FLASH &&
                backup_type != BACKUP_EEPROM))
    return 0;
  bound = size + size / 8 + 64;
  packed = (u8 *)malloc(bound);
  if (!packed)
  {
    bk_memory("its packed copy", bound);
    return 0;
  }
  packed_size = gzip_buffer(1, gamepak_backup, size, packed, bound);
  if (!packed_size)
  {
    free(packed);
    bk_memory("its packing", 268 * 1024);   /* (deflate's level 6, 15-bit window) */
    return 0;
  }
  errno = 0;
  f = fopen(name, "wb");
  ok = 0;
  if (!f)
    bk_file("create", errno, -1, packed_size, name);
  else
  {
    long wrote;
    setvbuf(f, NULL, _IONBF, 0);
    wrote = (long)fwrite(packed, 1, packed_size, f);
    ok = (u32)wrote == packed_size;
    if (!ok)
    {
      int err = errno;
      fclose(f);
      f = NULL;
      remove(name);
      bk_file("write", err, wrote, packed_size, name);
    }
  }
  if (f && fclose(f) != 0)
  {
    ok = 0;
    bk_file("close", errno, -1, packed_size, NULL);
  }
  free(packed);
  return ok ? 1 : 0;
}

/* R4: the save gzipped into name while linked, with no heap: deflate's
 * memory from a static arena (a 1 KB window, memLevel 2: ~12 KB in all)
 * and 4 KB written at a time. The usual way takes ~400 KB from the heap
 * for a 128 KB save (deflate's 15-bit window ~268 KB and the whole packed
 * copy) on the game's frame; linked, the OS's USB and NavNet work (the
 * CX II as host has its service task too) shares that heap, and the CX II
 * reset at the Cable Club, which saves just before "Please wait", and
 * never while linked elsewhere. Smaller files are a little larger. */
static u8 linked_arena[24 * 1024] __attribute__((aligned(8)));
static u32 linked_arena_used;
static u8 linked_out[4096];

static voidpf arena_alloc(voidpf opaque, uInt items, uInt size)
{
  u32 n = ((u32)items * size + 7) & ~7u;
  (void)opaque;
  if (n > sizeof(linked_arena) - linked_arena_used)
    return Z_NULL;
  linked_arena_used += n;
  return linked_arena + linked_arena_used - n;
}

static void arena_free(voidpf opaque, voidpf address)
{
  (void)opaque;
  (void)address;
}

static u32 save_packed_linked(const char *name, u32 *written)
{
  u32 size = nspire_backup_file_size();
  z_stream z;
  FILE *f;
  int ok, ret;

  *written = 0;
  if (!size || (backup_type != BACKUP_SRAM && backup_type != BACKUP_FLASH &&
                backup_type != BACKUP_EEPROM))
    return 0;
  memset(&z, 0, sizeof(z));
  linked_arena_used = 0;
  z.zalloc = arena_alloc;
  z.zfree = arena_free;
  if (deflateInit2(&z, 6, Z_DEFLATED, 16 + 10, 2, Z_DEFAULT_STRATEGY) != Z_OK)
  {
    bk_memory("its packing", sizeof(linked_arena));
    return 0;
  }
  errno = 0;
  f = fopen(name, "wb");
  ok = f != NULL;
  if (!f)
    bk_file("create", errno, -1, size, name);
  z.next_in = gamepak_backup;
  z.avail_in = size;
  do
  {
    z.next_out = linked_out;
    z.avail_out = sizeof(linked_out);
    ret = deflate(&z, Z_FINISH);
    if (ret == Z_STREAM_ERROR)
      ok = 0;
    if (ok && fwrite(linked_out, 1, sizeof(linked_out) - z.avail_out, f) != sizeof(linked_out) - z.avail_out)
    {
      ok = 0;
      bk_file("write", errno, -1, size, NULL);   /* (linked: no storage measured, the other calculator waits) */
    }
  } while (ok && ret != Z_STREAM_END);
  *written = (u32)z.total_out;
  deflateEnd(&z);
  if (f && fclose(f) != 0 && ok)
  {
    ok = 0;
    bk_file("close", errno, -1, size, NULL);
  }
  return ok ? 1 : 0;
}

void nspire_link_game_save(int end, unsigned bytes) __attribute__((weak));

/* The game's save in the form Compress saves asks for (or compressed as
 * given), the other form removed once it's written. */
static u32 write_backup_as(int compressed)
{
  char plain[512], packed[512], tmp[520], part[530];
  u32 ok;

  if (backup_unreadable)
    return 0;
  /* save17: a game with no save memory (none detected yet, or none at all) has nothing to write: no
   * file, and no failure (r16 tried at each quit with Compress saves on, and said "Couldn't write the
   * game's save") */
  if (!nspire_backup_file_size())
    return 0;
  memset(&bk_fail, 0, sizeof(bk_fail));
  backup_path(plain);
  packed_backup_path(packed);
  tmp_path(tmp, sizeof(tmp), compressed ? packed : plain);
  part_path(part, sizeof(part), compressed ? packed : plain);
  /* (r19-fix: written as the .part, the .tmp's name once closed: see part_path) */
  if (nspire_link_state() == LINK_LINKED)
  {
    u32 bytes = 0;
    if (nspire_link_game_save)
      nspire_link_game_save(0, 0);
    ok = compressed ? save_packed_linked(part, &bytes) : save_backup(part);
    if (!ok)
      remove(part);   /* (r19: what a write that failed left, now: not at this game's next load) */
    ok = take_place_why(tmp, compressed ? packed : plain, ok && sealed_or_dropped(part, tmp));
    if (nspire_link_game_save)
      nspire_link_game_save(1, compressed ? bytes : nspire_backup_file_size());
  }
  else
  {
    ok = compressed ? save_packed_backup(part) : save_backup(part);
    if (!ok)
      remove(part);
    ok = take_place_why(tmp, compressed ? packed : plain, ok && sealed_or_dropped(part, tmp));
  }
  if (ok)
  {
    nspire_file_delete(compressed ? plain : packed);
    part_path(part, sizeof(part), compressed ? plain : packed);
    if (exists(part))
      remove(part);   /* (r19-fix: a cut write of the other form) */
    backup_file_synced(compressed);
  }
  else
  {
    backup_file = 0;
    bk_say();
  }
  return ok;
}

u32 save_backup(char *name)
{
  RFILE *fd;
  u32 backup_size;
  int64_t w;

  if (backup_type != BACKUP_SRAM && backup_type != BACKUP_FLASH &&
      backup_type != BACKUP_EEPROM)
    return 0;

  backup_size = nspire_backup_file_size();
  if (!backup_size)
    return 0;

  errno = 0;
  fd = filestream_open(name, RETRO_VFS_FILE_ACCESS_WRITE,
                       RETRO_VFS_FILE_ACCESS_HINT_NONE);
  if (!fd)
  {
    bk_file("create", errno, -1, backup_size, name);
    return 0;
  }

  w = filestream_write(fd, gamepak_backup, (int64_t)backup_size);
  if (w != (int64_t)backup_size)
  {
    int err = errno;
    filestream_close(fd);
    remove(name);
    bk_file("write", err, w < 0 ? 0 : (long)w, backup_size, name);
    return 0;
  }
  /* (a close that fails is a write that didn't land) */
  if (filestream_close(fd) != 0)
  {
    bk_file("close", errno, -1, backup_size, NULL);
    return 0;
  }
  return 1;
}

/* (weak: builds without nspire_audio.c, tests/arm's libbench and the PC's
 * unit builds, have none: the calls as they were) */
int nspire_audio_os_call_begin(uint32_t *cpsr) __attribute__((weak));
void nspire_audio_os_call_end(int began, uint32_t cpsr) __attribute__((weak));
uint32_t nspire_audio_save_bank_ms(void) __attribute__((weak));

/* ---- The game's save written over its frames (2026-11-17, final/pace6) ----
 * The sound used to cut out on saves and the game froze for a split
 * second while saving. When the game has saved (WRITE_BACKUP_DELAY frames
 * after its last write to the save memory) the file was written there and
 * then: Emerald's 128 KB gzipped at level 6, the OS's write, its rename,
 * the other form's removal, the sound's queue unserved all along. Now the
 * memory is copied (the save as it was, whatever the game writes next) and
 * the rest goes a part a frame (update_backup): 16 KB of it deflated, then
 * each OS call in a frame of its own, with the sound's queue served through
 * it (nspire_audio_os_call_begin). The file's bytes are the same. While
 * linked it is written at once as before (save_packed_linked: no heap).
 * Anything else that writes, reads or names the save finishes it first. */
enum { BJ_IDLE, BJ_PACK, BJ_WRITE, BJ_PLACE, BJ_TIDY, BJ_CREATE, BJ_CLOSE, BJ_SEAL };
#define BJ_PACK_BYTES 8192   /* (zlib 6: ~3 ms of the CX II's a step, tests/libbench.sh's 46.6 ms for 128 KB) */
/* (r19-snd) the write an OS call per part, as a state's (nspire_states.c SJ_WRITE_BYTES): the file's
 * create, 8 KB at a time, its close, the replace's steps (nspire_file_replace_step) */
#define BJ_WRITE_BYTES 8192
static struct
{
  int step, compressed, ok, zlib_on, lent;
  u8 *mem, *out;
  u8 *arena;                  /* (lent: zlib's memory, in the states' file buffer) */
  unsigned long arena_size, arena_used;
  u32 size, out_size, out_cap, done, dirty_at;
  u32 t[4], longest, steps;   /* ticks: packing, writing, renaming, the other form's removal */
  z_stream z;
  FILE *f;                    /* (r19-snd: being written, BJ_CREATE to BJ_CLOSE) */
  u32 written;
  int placing;                /* (the replace begun: bj.rp) */
  struct nspire_replace rp;
  char plain[512], packed[512], tmp[520], part[530];
} bj;

/* save17: the game's save borrows the states' kept buffers while no state has them
 * (nspire_states.c states_lend_buffers): the memory and the packed copy in one, zlib's ~260 KB in the
 * other. No fresh ~0.55 MB late in a session; otherwise its own, as before. (weak: builds without
 * nspire_states.c) */
#pragma weak states_lend_buffers
/* (r19-snd: the menu's (states_serve_os) calls with the queue served too; weak: builds without nspire_states.c) */
#pragma weak states_serve_os
static int serve_os(void)
{
  return &states_serve_os != NULL && states_serve_os;
}
#pragma weak states_return_buffers

static voidpf bj_alloc(voidpf opaque, uInt items, uInt size)
{
  unsigned long n = ((unsigned long)items * size + 7) & ~7ul;
  (void)opaque;
  if (n > bj.arena_size - bj.arena_used)
    return Z_NULL;
  bj.arena_used += n;
  return bj.arena + bj.arena_used - n;
}

static void bj_free(voidpf opaque, voidpf address)
{
  (void)opaque;
  (void)address;
}

static void backup_job_free(void)
{
  if (bj.f)
  {
    fclose(bj.f);               /* (never open here: a failure closed it) */
    bj.f = NULL;
  }
  if (bj.zlib_on)
    deflateEnd(&bj.z);
  bj.zlib_on = 0;
  if (bj.lent)
    states_return_buffers();
  else
  {
    free(bj.mem);
    free(bj.out);
  }
  bj.lent = 0;
  bj.mem = bj.out = NULL;
  bj.step = BJ_IDLE;
}

/* 1 if it started; 0: write_backup_as at once (an unusual save, no memory) */
static int backup_job_start(int compressed)
{
  u32 size = nspire_backup_file_size();

  if (backup_unreadable || !size || nspire_link_state() == LINK_LINKED ||
      (backup_type != BACKUP_SRAM && backup_type != BACKUP_FLASH && backup_type != BACKUP_EEPROM))
    return 0;
  memset(&bj, 0, sizeof(bj));
  bj.compressed = compressed;
  bj.size = size;
  bj.dirty_at = backup_dirties;
  bj.out_cap = compressed ? size + size / 8 + 64 : 0;
  {
    unsigned char *a, *b;
    unsigned long a_size, b_size;
    if (states_lend_buffers && states_lend_buffers(&a, &a_size, &b, &b_size))
    {
      if (((size + 7) & ~7u) + bj.out_cap <= a_size)
      {
        bj.lent = 1;
        bj.mem = a;
        bj.out = compressed ? a + ((size + 7) & ~7u) : NULL;
        bj.arena = b;
        bj.arena_size = b_size;
        bj.arena_used = 0;
      }
      else
        states_return_buffers();
    }
  }
  if (!bj.lent)
    bj.mem = (u8 *)malloc(size);
  if (!bj.mem)
    return 0;
  memcpy(bj.mem, gamepak_backup, size);
  backup_path(bj.plain);
  packed_backup_path(bj.packed);
  tmp_path(bj.tmp, sizeof(bj.tmp), compressed ? bj.packed : bj.plain);
  part_path(bj.part, sizeof(bj.part), compressed ? bj.packed : bj.plain);
  bj.ok = 1;
  if (compressed)
  {
    if (!bj.lent)
      bj.out = (u8 *)malloc(bj.out_cap);
    else
    {
      bj.z.zalloc = bj_alloc;
      bj.z.zfree = bj_free;
    }
    if (!bj.out || deflateInit2(&bj.z, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    {
      backup_job_free();
      return 0;
    }
    bj.zlib_on = 1;
    bj.z.next_out = bj.out;
    bj.z.avail_out = bj.out_cap;
    bj.step = BJ_PACK;
  }
  else
    bj.step = BJ_CREATE;
  return 1;
}

/* (r19-snd) A file call of the game's save failed: the file closed, the cause kept (save17's words; the
 * .tmp dropped at BJ_PLACE) */
static void backup_write_failed(const char *step, int err, long wrote, int in_game)
{
  u32 want = bj.compressed ? bj.out_size : bj.size;
  if (bj.f)
  {
    fclose(bj.f);
    bj.f = NULL;
  }
  remove(bj.part);
  bj.ok = 0;
  bk_in_game = in_game;
  bk_file(step, err, wrote, want, strcmp(step, "close") ? bj.part : NULL);
  bk_in_game = 0;
}

/* The next part; in_game: the OS's calls with the sound's queue served. */
static void backup_job_run(int in_game)
{
  int began = 0;
  uint32_t cpsr = 0;

  switch (bj.step)
  {
  case BJ_PACK:
  {
    u32 n = bj.size - bj.done < BJ_PACK_BYTES ? bj.size - bj.done : BJ_PACK_BYTES;
    int last = bj.done + n == bj.size, r;
    bj.z.next_in = bj.mem + bj.done;
    bj.z.avail_in = n;
    r = deflate(&bj.z, last ? Z_FINISH : Z_NO_FLUSH);
    bj.done += n - bj.z.avail_in;
    if (last && r == Z_STREAM_END)
    {
      bj.out_size = (u32)bj.z.total_out;
      deflateEnd(&bj.z);
      bj.zlib_on = 0;
      bj.step = BJ_CREATE;
    }
    else if (r != Z_OK && (r != Z_BUF_ERROR || last))
    {
      bj.ok = 0;
      memset(&bk_fail, 0, sizeof(bk_fail));
      if (r == Z_MEM_ERROR)
        bk_memory("its packing", 268 * 1024);
      else
      {
        bk_fail.why = STATES_WHY_OTHER;
        bk_fail.step = "packing";
        bk_fail.free = -1;
      }
      bj.step = BJ_PLACE;   /* (nothing written: the .tmp, if any, dropped) */
    }
    return;
  }
  case BJ_CREATE:
    if ((in_game || serve_os()) && nspire_audio_os_call_begin)
      began = nspire_audio_os_call_begin(&cpsr);
    memset(&bk_fail, 0, sizeof(bk_fail));
    errno = 0;
    bj.f = fopen(bj.part, "wb");
    if (bj.f)
      setvbuf(bj.f, NULL, _IONBF, 0);   /* (unbuffered: a part's write is what reached the file) */
    else
      backup_write_failed("create", errno, -1, in_game);
    if (began)
      nspire_audio_os_call_end(began, cpsr);
    bj.written = 0;
    bj.step = bj.f ? BJ_WRITE : BJ_PLACE;
    return;
  case BJ_WRITE:
    if ((in_game || serve_os()) && nspire_audio_os_call_begin)
      began = nspire_audio_os_call_begin(&cpsr);
    {
      /* save17: its step, errno and what the OS took (unbuffered: the write's count) kept as the cause */
      u32 want = bj.compressed ? bj.out_size : bj.size, n = want - bj.written;
      long wrote;
      if (in_game && n > BJ_WRITE_BYTES)
        n = BJ_WRITE_BYTES;
      errno = 0;
      wrote = (long)fwrite((bj.compressed ? bj.out : bj.mem) + bj.written, 1, n, bj.f);
      if ((u32)wrote == n)
        bj.written += n;
      else
        backup_write_failed("write", errno, (long)bj.written + (wrote > 0 ? wrote : 0), in_game);
      if (bj.ok && bj.written == want)
        bj.step = BJ_CLOSE;
      else if (!bj.ok)
        bj.step = BJ_PLACE;
    }
    if (began)
      nspire_audio_os_call_end(began, cpsr);
    return;
  case BJ_CLOSE:
    if ((in_game || serve_os()) && nspire_audio_os_call_begin)
      began = nspire_audio_os_call_begin(&cpsr);
    errno = 0;
    {
      int r = fclose(bj.f);
      bj.f = NULL;
      if (r != 0)
        backup_write_failed("close", errno, -1, in_game);
    }
    if (began)
      nspire_audio_os_call_end(began, cpsr);
    bj.step = bj.ok ? BJ_SEAL : BJ_PLACE;
    return;
  case BJ_SEAL:
    /* (r19-fix) closed, whole: the .part takes the .tmp's name (an OS call of its own) */
    if ((in_game || serve_os()) && nspire_audio_os_call_begin)
      began = nspire_audio_os_call_begin(&cpsr);
    errno = 0;
    if (!part_sealed(bj.part, bj.tmp))
      backup_write_failed("rename", errno, -1, in_game);
    if (began)
      nspire_audio_os_call_end(began, cpsr);
    bj.step = BJ_PLACE;
    return;
  case BJ_PLACE:
    if ((in_game || serve_os()) && nspire_audio_os_call_begin)
      began = nspire_audio_os_call_begin(&cpsr);
    {
      /* take_place_why's, a step at a time in a game's frame */
      int r = NSPIRE_REPLACE_BUSY;
      const char *name = bj.compressed ? bj.packed : bj.plain;
      if (!bj.ok)
        r = 0;
      else if (!bj.placing)
      {
        bj.placing = 1;
        if (!nspire_file_replace_begin(&bj.rp, bj.tmp, name))
          r = 0;
      }
      if (r == NSPIRE_REPLACE_BUSY)
      {
        errno = 0;
        r = nspire_file_replace_step(&bj.rp);
        while (r == NSPIRE_REPLACE_BUSY && !in_game)
          r = nspire_file_replace_step(&bj.rp);
      }
      if (r == NSPIRE_REPLACE_BUSY)
      {
        if (began)
          nspire_audio_os_call_end(began, cpsr);
        return;
      }
      if (r == 0 && bj.ok)
      {
        memset(&bk_fail, 0, sizeof(bk_fail));
        bk_fail.why = STATES_WHY_FILE;
        bk_fail.step = "rename";
        bk_fail.err = errno;
        bk_fail.free = -1;
      }
      if (r == 0)
        remove(bj.tmp);
      bj.ok = r == 1;
      bj.placing = 0;
    }
    if (began)
      nspire_audio_os_call_end(began, cpsr);
    if (!bj.ok)
    {
      backup_file = 0;
      bk_say();
      backup_job_free();
      return;
    }
    bj.step = BJ_TIDY;
    return;
  case BJ_TIDY:
    if ((in_game || serve_os()) && nspire_audio_os_call_begin)
      began = nspire_audio_os_call_begin(&cpsr);
    nspire_file_delete(bj.compressed ? bj.plain : bj.packed);
    {
      /* (r19-fix: and a cut write of the other form) */
      char other[530];
      part_path(other, sizeof(other), bj.compressed ? bj.plain : bj.packed);
      if (exists(other))
        remove(other);
    }
    if (began)
      nspire_audio_os_call_end(began, cpsr);
    /* the file holds the memory as it is, unless the game wrote to it since */
    if (bj.dirty_at == backup_dirties)
    {
      backup_file = bj.size | (bj.compressed != 0);
#ifdef NSPIRE_BACKUP_CHECK
      memcpy(backup_synced_bytes, bj.mem, bj.size);
#endif
    }
    bj.step = BJ_IDLE;   /* (freed by backup_job_step, after the note) */
    return;
  default:
    return;
  }
}

/* The parts' times into gpsp_states_time.txt.tns: what the next hardware
 * log says of them (the game's save's OS calls were never timed). */
#define BJ_MS(t) (unsigned)((t) * 1000u / NSPIRE_TICK_HZ), (unsigned)((t) * 10000u / NSPIRE_TICK_HZ % 10)
static void backup_job_step(int in_game)
{
  int part = bj.step == BJ_PACK ? 0 : bj.step == BJ_CREATE || bj.step == BJ_WRITE || bj.step == BJ_CLOSE ||
             bj.step == BJ_SEAL ? 1 :
             bj.step == BJ_PLACE ? 2 : 3;
  u32 t0 = nspire_ticks(), dt;

  backup_job_run(in_game);
  dt = nspire_ticks() - t0;
  bj.t[part] += dt;
  bj.steps++;
  if (dt > bj.longest)
    bj.longest = dt;
  if (bj.step == BJ_IDLE && (bj.mem || bj.out))
  {
    if (bj.ok)
      states_timing_note("game's save, %u bytes%s, %u parts (the longest %u.%u ms): pack %u.%u ms, write %u.%u,"
                         " rename %u.%u, the other form removed %u.%u; bank %u ms",
                         (unsigned)(bj.compressed ? bj.out_size : bj.size), bj.compressed ? " gzipped" : "",
                         (unsigned)bj.steps, BJ_MS(bj.longest), BJ_MS(bj.t[0]), BJ_MS(bj.t[1]), BJ_MS(bj.t[2]),
                         BJ_MS(bj.t[3]), (unsigned)(nspire_audio_save_bank_ms ? nspire_audio_save_bank_ms() : 0));
    backup_job_free();
  }
}

#ifdef NSPIRE_HOST
void nspire_backup_drop_job_for_test(void)
{
  if (bj.step)
    backup_job_free();
}
#endif

/* A save being written, written now (as it was before: no interrupts on). */
void nspire_backup_finish(void)
{
  while (bj.step)
    backup_job_step(0);
}

int nspire_backup_busy(void)
{
  return bj.step != BJ_IDLE;
}

/* At a frame's end (nspire_host_main.c save_job_frame): the next part. */
void nspire_backup_step(void)
{
  if (bj.step)
    backup_job_step(1);
}

int nspire_backup_next_is_file(void)
{
  return bj.step == BJ_CREATE || bj.step == BJ_WRITE || bj.step == BJ_CLOSE || bj.step == BJ_PLACE ||
         bj.step == BJ_TIDY || bj.step == BJ_SEAL;
}

void update_backup(void)
{
  /* (linked: no heap held over frames, R4; the rest at once) */
  if (bj.step && nspire_link_state() == LINK_LINKED)
    nspire_backup_finish();
  if (!update_backup_flag || !gamepak_filename[0])
    return;

  if (backup_update_count != WRITE_BACKUP_DELAY + 1 && backup_update_count)
    backup_update_count--;

  if (backup_update_count == 0)
  {
    /* (r19-fix 5) the last one still being written (its parts behind a state's: one job a
     * frame): this one waits for it, due at each frame until then, the memory copied when it starts; was
     * finished at once here, the interrupts masked (its OS calls back to back, the sound's queue unserved) */
    if (bj.step)
      return;
    if (!backup_job_start(nspire_compress_saves))
    {
      bk_in_game = 1;
      write_backup_as(nspire_compress_saves);
      bk_in_game = 0;
    }
    backup_update_count = WRITE_BACKUP_DELAY + 1;
  }
}

/* (noinline: one copy, not one in each of its three callers) */
__attribute__((noinline)) void update_backup_force(void)
{
  nspire_backup_finish();
  if (!gamepak_filename[0] || backup_file == BACKUP_FILE_DELETED || !nspire_backup_file_size())
    return;   /* (save17: no save memory, nothing to write) */
  if (backup_file != backup_file_now(nspire_compress_saves))
    write_backup_as(nspire_compress_saves);
#ifdef NSPIRE_BACKUP_CHECK
  else if (memcmp(backup_synced_bytes, gamepak_backup, nspire_backup_file_size()))
  {
    nspire_backup_check_failures++;
    fprintf(stderr, "nspire_backup: the save memory changed unmarked, and leaving didn't write it\n");
  }
#endif
  backup_update_count = WRITE_BACKUP_DELAY + 1;
}

/* ---- For the save manager ---- */

int nspire_backup_info(long *bytes, int *compressed)
{
  char plain[512], packed[512];
  u32 plain_time = 0, packed_time = 0;
  int has_plain, has_packed;
  struct stat st;

  nspire_backup_finish();

  backup_path(plain);
  packed_backup_path(packed);
  has_plain = file_time(plain, &plain_time);
  has_packed = file_time(packed, &packed_time);
  if (!has_plain && !has_packed)
    return 0;
  *compressed = has_packed && (!has_plain || packed_time >= plain_time);
  *bytes = stat(*compressed ? packed : plain, &st) == 0 ? (long)st.st_size : 0;
  return 1;
}

int nspire_backup_set_compressed(int compressed)
{
  long bytes;
  int now;

  nspire_backup_finish();

  if (!nspire_backup_info(&bytes, &now))
    return 0;
  if (now == compressed)
    return 1;
  /* The game's memory has the newest save (it was loaded, and written to
   * since): written out in the other form. */
  return write_backup_as(compressed) != 0;
}

int nspire_backup_delete(void)
{
  char plain[512], packed[512];
  int removed = 0;

  nspire_backup_finish();

  backup_path(plain);
  packed_backup_path(packed);
  removed |= nspire_file_delete(plain);
  removed |= nspire_file_delete(packed);
  backup_file = BACKUP_FILE_DELETED;
  backup_unreadable = 0;
  backup_update_count = WRITE_BACKUP_DELAY + 1;
  drive_deleted();   /* (the drive's copy too, while the game's saves are there) */
  return removed;
}

/* ---- Any game's save, for the Saves place (UI17) ----
 * The game list's Saves shows a game that isn't open: its save's file is
 * looked at, packed, unpacked or deleted as a file (the open game's goes
 * through the functions above: its memory has the newest save). */

/* The open game: the same ROM file (separators and case aside), not one of the same name (another folder, another
 * region: its own files, "~CODE"). */
static int is_open_game(const char *rom_path)
{
  const char *a = rom_path, *b = gamepak_filename;
  if (!b[0])
    return 0;
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

static void paths_of(const char *rom_path, char *plain, char *packed)
{
  char base[496];
  nspire_game_base(rom_path, base, sizeof(base));
  snprintf(plain, 512, "%s.sav.tns", base);
  snprintf(packed, 512, "%s.sav.gz.tns", base);
}

int nspire_backup_info_of(const char *rom_path, long *bytes, int *compressed)
{
  char plain[512], packed[512];
  u32 plain_time = 0, packed_time = 0;
  int has_plain, has_packed;
  struct stat st;

  if (is_open_game(rom_path))
    return nspire_backup_info(bytes, compressed);
  paths_of(rom_path, plain, packed);
  has_plain = file_time(plain, &plain_time);
  has_packed = file_time(packed, &packed_time);
  if (!has_plain && !has_packed)
    return 0;
  *compressed = has_packed && (!has_plain || packed_time >= plain_time);
  *bytes = stat(*compressed ? packed : plain, &st) == 0 ? (long)st.st_size : 0;
  return 1;
}

int nspire_backup_delete_of(const char *rom_path)
{
  char plain[512], packed[512];
  int removed = 0;

  if (is_open_game(rom_path))
    return nspire_backup_delete();
  paths_of(rom_path, plain, packed);
  removed |= nspire_file_delete(plain);
  removed |= nspire_file_delete(packed);
  return removed;
}

int nspire_backup_set_compressed_of(const char *rom_path, int compressed)
{
  char plain[512], packed[512], tmp[520];
  const char *from, *to;
  long bytes, size = -1;
  int now, ok = 0;
  u8 *in = NULL, *out = NULL;
  u32 out_size = 0, n = 0;
  FILE *f;

  if (is_open_game(rom_path))
    return nspire_backup_set_compressed(compressed);
  if (!nspire_backup_info_of(rom_path, &bytes, &now))
    return 0;
  if (now == compressed)
    return 1;
  paths_of(rom_path, plain, packed);
  from = now ? packed : plain;
  to = compressed ? packed : plain;
  f = fopen(from, "rb");
  if (f && fseek(f, 0, SEEK_END) == 0)
    size = ftell(f);
  if (f && size > 0 && size < 0x40000 && fseek(f, 0, SEEK_SET) == 0 && (in = (u8 *)malloc((size_t)size)) != NULL)
    ok = fread(in, 1, (size_t)size, f) == (size_t)size;
  if (f)
    fclose(f);
  if (ok)
  {
    out_size = compressed ? (u32)size + (u32)size / 8 + 64 : 0x20000;
    out = (u8 *)malloc(out_size);
    n = out ? gzip_buffer(compressed, in, (u32)size, out, out_size) : 0;
    ok = n && (compressed || valid_save_size((long)n));
  }
  free(in);
  if (ok)
  {
    tmp_path(tmp, sizeof(tmp), to);
    f = fopen(tmp, "wb");
    ok = f && fwrite(out, 1, n, f) == n;
    if (f && fclose(f) != 0)
      ok = 0;
    ok = nspire_tmp_take_place(tmp, to, ok);
    if (ok)
      nspire_file_delete(from);
  }
  free(out);
  return ok;
}

/* ---- The save's copy on a USB drive (2026-11-17, final/usbsave8) ----
 *
 * With Saves on: USB drive (nspire_places.h) and a drive in that takes
 * writes (nspire_saves_on_drive, nspire_drive_saves.c), the game's save has
 * a copy on the drive: "/gpsp/saves/<the game's name>.sav", plain, whatever
 * Compress saves says (what a PC emulator reads; a "<name>.sav.gz" there is
 * read too).
 *
 * The calculator's file stays the one the game works on: written as before,
 * a part a frame after the game has saved. The drive's copy is written at
 * safe points only (a USB write takes tens of ms to seconds): when the
 * menu opens, when the game is left, at quit (nspire_backup_drive_sync),
 * and only when the save changed. So between the game's save and the next
 * safe point the drive's copy is the older one: a calculator that crashes
 * or a drive pulled out in between leaves it so.
 *
 * Which of the two is the newer is not asked of their clocks (a
 * calculator's is often unset, a PC's is another). Each time both hold the
 * same bytes, their sum is kept beside the calculator's file
 * ("<name>.sav.usb.tns", 16 bytes: a mark, the sum, the size, and the
 * highest count of the game's states on a drive: nspire_backup_drive_top).
 * At a game's start
 * (nspire_backup_drive_load), when the two differ:
 *  - the calculator's is still that sum: the drive's was changed elsewhere
 *    (another calculator, a PC): it is taken, and written to the
 *    calculator;
 *  - the drive's is still that sum: the calculator's is the newer (the
 *    game saved and no safe point followed): it is kept and written to the
 *    drive, and the player is told;
 *  - neither, or no sum yet: two saves nobody can order. The player picks
 *    one (a question); with no answer the calculator's is played and the
 *    drive's is left as it is, with no copy made there this time.
 * A drive's file that is no save (an odd size) is never replaced. */
#define DRIVE_MARK "GPSU"
static int drive_mirror;            /* the drive's copy is this game's: kept up to date */
static u32 drive_sum, drive_size;   /* what it holds (with drive_mirror) */
static u32 synced_dirties, synced_size;
static unsigned drive_seen;         /* the drive (its epoch) it was compared with */
static int drive_note_pending;

#pragma weak nspire_drive_epoch
#pragma weak nspire_drive_put
#pragma weak nspire_drive_get
#pragma weak nspire_drive_remove
#pragma weak nspire_drive_rename
#pragma weak nspire_drive_settle
#pragma weak nspire_drive_discard
#pragma weak nspire_drive_stat
#pragma weak nspire_drive_error
#pragma weak nspire_ask
#pragma weak keys_wait_release
#pragma weak nspire_saves_on_drive
#pragma weak nspire_saves_drive_failed
#pragma weak nspire_game_drive_base
#pragma weak nspire_game_drive_claim

static void drive_forget(void)
{
  drive_mirror = 0;
  drive_seen = ~0u;
  drive_note_pending = 0;
}

static void drive_paths(char *plain, char *packed, char *mark)
{
  char base[480];
  if (plain || packed)
  {
    nspire_game_drive_base(gamepak_filename, "saves", base, sizeof(base));
    if (plain)
      snprintf(plain, 512, "%s.sav", base);
    if (packed)
      snprintf(packed, 512, "%s.sav.gz", base);
  }
  if (mark)
  {
    nspire_game_base(gamepak_filename, base, sizeof(base));
    snprintf(mark, 512, "%s.sav.usb.tns", base);
  }
}

static u32 save_sum(const u8 *data, u32 size)
{
  return (u32)crc32(crc32(0, NULL, 0), data, size);
}

/* The file beside the calculator's save: the sum the calculator's save and
 * the drive's last had in common (size 0: none known), and the highest count
 * of the game's states written to a drive (0: none). */
struct drive_mark
{
  u32 magic, sum, size, top;
};

static int mark_load(struct drive_mark *m)
{
  char path[512];
  FILE *f;
  size_t got;

  memset(m, 0, sizeof(*m));
  drive_paths(NULL, NULL, path);
  f = fopen(path, "rb");
  if (!f)
    return 0;
  got = fread(m, 4, 4, f);
  fclose(f);
  if (got < 3 || memcmp(&m->magic, DRIVE_MARK, 4))
  {
    memset(m, 0, sizeof(*m));
    return 0;
  }
  if (got < 4)
    m->top = 0;
  return 1;
}

static void mark_store(const struct drive_mark *want)
{
  char path[512];
  struct drive_mark m = *want, had;
  FILE *f;

  memcpy(&m.magic, DRIVE_MARK, 4);
  if (mark_load(&had) && !memcmp(&had, &m, sizeof(m)))
    return;
  drive_paths(NULL, NULL, path);
  f = fopen(path, "wb");
  if (f)
  {
    fwrite(&m, 4, 4, f);
    fclose(f);
  }
}

static int mark_read(u32 *sum, u32 *size)
{
  struct drive_mark m;
  int ok = mark_load(&m) && m.size != 0;
  *sum = m.sum;
  *size = m.size;
  return ok;
}

static void mark_write(u32 sum, u32 size)
{
  struct drive_mark m;
  mark_load(&m);
  m.sum = sum;
  m.size = size;
  mark_store(&m);
}

/* (the calculator's file is not the drive's bytes: no sum to go by next time) */
static void mark_remove(void)
{
  struct drive_mark m;
  if (!mark_load(&m))
    return;
  if (!m.top)
  {
    char path[512];
    drive_paths(NULL, NULL, path);
    remove(path);
    return;
  }
  m.sum = m.size = 0;
  mark_store(&m);
}

unsigned int nspire_backup_drive_top(void)
{
  struct drive_mark m;
  if (!gamepak_filename[0])
    return 0;
  mark_load(&m);
  return m.top;
}

void nspire_backup_drive_top_set(unsigned int top)
{
  struct drive_mark m;
  if (!gamepak_filename[0])
    return;
  mark_load(&m);
  if (top <= m.top)
    return;
  m.top = top;
  mark_store(&m);
}

/* The game has a save on the calculator's side (its memory): one that
 * loaded, or one the game wrote since. */
static u32 calc_save_size(void)
{
  if (backup_unreadable || backup_file == BACKUP_FILE_DELETED ||
      (!calc_loaded && backup_dirties == dirties_at_load))
    return 0;
  return nspire_backup_file_size();
}

/* The drive's save, unpacked (malloc'd, *size bytes): 1; 0: none there;
 * -1: a file is there that is no save (left alone). */
static int drive_save_read(u8 **data, u32 *size)
{
  char plain[512], packed[512], tmp[520];
  unsigned long n = 0;
  u8 *d;

  *data = NULL;
  *size = 0;
  drive_paths(plain, packed, NULL);
  /* what a write of it that was cut short left (the drive pulled out), made one file again by the
   * drive's driver first: a .tmp of a save's size is the new save and takes the name; any other
   * is dropped and the save of the name stays */
  snprintf(tmp, sizeof(tmp), "%s.tmp", plain);
  if (nspire_drive_stat(tmp, &n, NULL))
  {
    if (valid_save_size((long)n))
      nspire_drive_settle(plain);
    else
      nspire_drive_discard(plain);
  }
  d = (u8 *)nspire_drive_get(plain, sizeof(gamepak_backup), &n);
  if (!d && !nspire_drive_stat(plain, NULL, NULL))
  {
    /* (a drive that can't be written, its .tmp the whole new save: read as it is) */
    d = (u8 *)nspire_drive_get(tmp, sizeof(gamepak_backup), &n);
    if (!d || !valid_save_size((long)n))
    {
      free(d);
      d = NULL;
    }
  }
  if (d)
  {
    if (!valid_save_size((long)n))
    {
      free(d);
      return -1;
    }
    *data = d;
    *size = (u32)n;
    return 1;
  }
  if (nspire_drive_stat(plain, NULL, NULL))
    return -1;   /* there, and not read (too big for a save, or the drive failed) */
  d = (u8 *)nspire_drive_get(packed, 0x40000, &n);
  if (d)
  {
    u8 *raw = (u8 *)malloc(sizeof(gamepak_backup));
    u32 got = raw ? gzip_buffer(0, d, (u32)n, raw, sizeof(gamepak_backup)) : 0;
    free(d);
    if (!valid_save_size((long)got))
    {
      free(raw);
      return -1;
    }
    *data = raw;
    *size = got;
    return 1;
  }
  return nspire_drive_stat(packed, NULL, NULL) ? -1 : 0;
}

/* The game's save as it is in memory written to the drive: 1 done. */
static int drive_save_write(u32 size)
{
  char plain[512], packed[512];
  int calc_whole;

  /* the calculator's file first: the sum kept beside it says both are these bytes */
  update_backup_force();
  calc_whole = !backup_unreadable && backup_file == backup_file_now(nspire_compress_saves);
  drive_paths(plain, packed, NULL);
  if (!nspire_drive_put(plain, gamepak_backup, size))
  {
    drive_mirror = 0;
    return 0;
  }
  if (nspire_drive_stat(packed, NULL, NULL))
    nspire_drive_remove(packed);   /* (one form there, as on the calculator) */
  nspire_game_drive_claim();
  drive_mirror = 1;
  drive_sum = save_sum(gamepak_backup, size);
  drive_size = size;
  drive_seen = nspire_drive_epoch();
  synced_dirties = backup_dirties;
  synced_size = size;
  if (calc_whole)
    mark_write(drive_sum, size);
  else
    mark_remove();   /* (the calculator's file is not these bytes: nothing to go by next time) */
  return 1;
}

/* The drive's save becomes the game's (at its start only: the game has not
 * read its save yet), and the calculator's file. */
static void drive_save_take(const u8 *data, u32 size)
{
  memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
  memcpy(gamepak_backup, data, size);
  backup_type_from_size(size);
  backup_unreadable = 0;
  calc_loaded = 1;
  backup_file = 0;
  drive_mirror = 1;
  drive_sum = save_sum(data, size);
  drive_size = size;
  drive_seen = nspire_drive_epoch();
  synced_dirties = backup_dirties;
  synced_size = size;
  if (write_backup_as(nspire_compress_saves))
    mark_write(drive_sum, size);
  else
    mark_remove();
}

/* The two saves made one, as far as that can be known (see above). at_load:
 * the game has not run yet, so the drive's can be taken. */
static void drive_reconcile(int at_load)
{
  u8 *theirs = NULL;
  u32 their_size = 0, size, sum = 0, mark_sum = 0, mark_size = 0, their_sum = 0;
  int have, marked, pick = -1;

  drive_mirror = 0;
  drive_seen = nspire_drive_epoch();
  if (backup_unreadable)
    return;   /* (SAV-1: a save on the calculator that didn't load is sorted out first) */
  have = drive_save_read(&theirs, &their_size);
  size = calc_save_size();
  if (size)
    sum = save_sum(gamepak_backup, size);
  if (have < 0)
  {
    drive_note_pending = NSPIRE_SAVE_NOTE_LEFT;
    nspire_link_log("save: the USB drive's file is no save: left as it is");
    return;
  }
  if (!have)
  {
    if (size && !drive_save_write(size))
      nspire_saves_drive_failed();
    else if (!size)
    {
      drive_mirror = 1;   /* none yet on either side: the first save goes there */
      drive_sum = drive_size = 0;
      synced_dirties = backup_dirties;
      synced_size = 0;
    }
    return;
  }
  their_sum = save_sum(theirs, their_size);
  marked = mark_read(&mark_sum, &mark_size);
  if (size && size == their_size && sum == their_sum)
  {
    drive_mirror = 1;
    drive_sum = sum;
    drive_size = size;
    synced_dirties = backup_dirties;
    synced_size = size;
    if (backup_file == backup_file_now(nspire_compress_saves) || at_load)
      mark_write(sum, size);
  }
  else if (!size || (marked && mark_sum == sum && mark_size == size))
  {
    /* the calculator has none, or the one both had: the drive's is the newer */
    if (at_load)
    {
      drive_save_take(theirs, their_size);
      nspire_link_log("save: taken from the USB drive (%u bytes)", (unsigned)their_size);
    }
    else
      drive_note_pending = NSPIRE_SAVE_NOTE_LEFT;   /* (the game runs on its own: taken at its next start) */
  }
  else if (marked && mark_sum == their_sum && mark_size == their_size)
  {
    /* the drive has the one both had: the calculator's is the newer */
    if (drive_save_write(size))
      drive_note_pending = NSPIRE_SAVE_NOTE_CALC_NEWER;
    else
      nspire_saves_drive_failed();
    nspire_link_log("save: the calculator's is newer than the USB drive's: %s", drive_mirror ? "copied there" : "not copied");
  }
  else
  {
    /* two saves nobody can order */
    if (at_load && nspire_ask)
    {
      struct ui_dialog_view d;
      memset(&d, 0, sizeof(d));
      d.title = "Two saves of this game";
      d.text = "The calculator's and the USB drive's differ. The one you don't pick is replaced.";
      d.first = "Calculator's";
      d.second = "USB drive's";
      if (keys_wait_release)
        keys_wait_release();   /* (the key that started the game is no answer) */
      pick = nspire_ask(&d, NULL, NULL);
      if (keys_wait_release)
        keys_wait_release();
    }
    if (pick == 1)
      drive_save_take(theirs, their_size);
    else if (pick == 0)
    {
      if (!drive_save_write(size))
        nspire_saves_drive_failed();
    }
    else
      drive_note_pending = NSPIRE_SAVE_NOTE_LEFT;
    nspire_link_log("save: the calculator's and the USB drive's differ, neither known as newer: %s",
                    pick == 1 ? "the drive's picked" : pick == 0 ? "the calculator's picked" : "the drive's left as it is");
  }
  free(theirs);
}

void nspire_backup_drive_load(void)
{
  drive_forget();
  if (!gamepak_filename[0] || !nspire_saves_on_drive || !nspire_saves_on_drive())
    return;
  drive_reconcile(1);
}

void nspire_backup_drive_sync(void)
{
  u32 size;

  if (!gamepak_filename[0] || !nspire_saves_on_drive)
    return;
  nspire_backup_finish();
  if (!nspire_saves_on_drive())
  {
    drive_mirror = 0;
    drive_seen = ~0u;
    return;
  }
  if (!drive_mirror && drive_seen == nspire_drive_epoch())
    return;   /* this drive's was left as it is (another save, a file that is none) */
  if (!drive_mirror || drive_seen != nspire_drive_epoch())
  {
    /* the drive came since the game started, or it is another one */
    update_backup_force();   /* (the calculator's file as the memory is: the two are compared) */
    drive_reconcile(0);
    return;
  }
  size = calc_save_size();
  if (!size || (synced_dirties == backup_dirties && synced_size == size))
    return;
  if (drive_size == size && drive_sum == save_sum(gamepak_backup, size))
  {
    synced_dirties = backup_dirties;   /* (written to, the same bytes) */
    return;
  }
  if (!drive_save_write(size))
    nspire_saves_drive_failed();
}

int nspire_backup_drive_noted(void)
{
  return drive_note_pending != 0;
}

int nspire_backup_drive_note(void)
{
  int note = drive_note_pending;
  drive_note_pending = 0;
  return note;
}

/* The save manager deleted the save: the drive's copy too, while it is this game's. */
static void drive_deleted(void)
{
  char plain[512], packed[512];

  if (!drive_mirror || !nspire_saves_on_drive || !nspire_saves_on_drive())
    return;
  drive_paths(plain, packed, NULL);
  if (nspire_drive_stat(plain, NULL, NULL))
    nspire_drive_remove(plain);
  if (nspire_drive_stat(packed, NULL, NULL))
    nspire_drive_remove(packed);
  mark_remove();
  drive_sum = drive_size = 0;
  synced_dirties = backup_dirties;
  synced_size = 0;
}

int nspire_backup_copy_to_drive(int replace)
{
  u8 *theirs = NULL;
  u32 their_size = 0, size;
  int have, result;

  nspire_backup_finish();
  if (!gamepak_filename[0] || !nspire_drive_put)
    return STATES_COPY_FAILED;
  if (drive_mirror && nspire_saves_on_drive && nspire_saves_on_drive())
    return STATES_COPY_SAME;   /* (kept there already: nspire_backup_drive_sync) */
  size = calc_save_size();
  if (!size)
    return STATES_COPY_FAILED;
  have = drive_save_read(&theirs, &their_size);
  if (have > 0 && their_size == size && !memcmp(theirs, gamepak_backup, size))
    result = STATES_COPY_SAME;
  else if (have && !replace)
    result = STATES_COPY_OTHER;
  else
    result = drive_save_write(size) ? STATES_COPY_DONE : STATES_COPY_FAILED;
  drive_forget();   /* (a copy: the calculator is this game's place) */
  free(theirs);
  return result;
}
