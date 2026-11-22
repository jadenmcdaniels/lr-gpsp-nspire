/* Save state slots: see nspire_states.h. */

#include <stdarg.h>
#include "common.h"
#include "nspire_states.h"
#include "nspire_fastdeflate.h"
#include "nspire_zip.h"
#include "nspire_gbz.h"
#include "nspire_frontend.h"
#include "nspire_paths.h"
#include "nspire_ui.h"
#include "nspire.h"
#include "nspire_audio.h"
#include "nspire_drive.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <zlib.h>

#define STATE_MAGIC   "GPSZ"
#define STATE_VERSION 1
/* In the header's flags: written by a build that finds the newest state by
 * its counter (this one). The first QoL build's states don't have it. */
#define STATE_FLAG_COUNTED 1u
/* The state follows the header as it is, not packed (Compress saves off). */
#define STATE_FLAG_STORED  2u
/* A picture of the game follows the state (reserved[0] is its size). After
 * the state, so builds that don't know it still load the state. */
#define STATE_FLAG_PICTURE 4u
#define PICTURE_MAX   (8 + STATE_PIC_W * STATE_PIC_H * 2 + 1024)
/* States and their pictures are packed by nspire_fastdeflate.c: zlib
 * streams, so older builds and zlib read them. An Emerald state packs in
 * 41.6 ms instead of 130.4 at zlib level 1 (zlib 6: 254), its picture in
 * 5.9 ms less and 136 bytes smaller (fastdeflate_picture), the file 4%
 * smaller (59,122 bytes, 61,682), and the state unpacks 0.7 ms faster, its
 * picture 0.09 ms (tests/libbench.sh, CX II). */
#define CHUNK         16384   /* file reads go through this */

struct state_header
{
  char magic[4];
  u32 version;
  u32 counter;       /* saves are numbered; the newest has the highest */
  u32 raw_size;      /* GBA_STATE_MEM_SIZE */
  u32 packed_size;   /* zlib bytes after the header (raw_size if stored) */
  u32 flags;
  u32 reserved[2];   /* [0]: the picture's size with STATE_FLAG_PICTURE; [1]: when it was saved (seconds, the
                      * calculator's clock; 0: not kept, the file's time says it: r16 and older wrote 0, and
                      * their readers never look at it) */
};

/* The picture: width and height (u16 each), the zlib size (u32), then the
 * 5:6:5 pixels packed. */
struct picture_header
{
  u16 w, h;
  u32 packed_size;
};

static char base_path[512];   /* "<saves folder>/Pokemon.gba"; on a drive "usb:/gpsp/states/Pokemon.gba" */
static unsigned char used[STATE_SLOTS];
static unsigned char counted[STATE_SLOTS];   /* has STATE_FLAG_COUNTED */
static u32 counters[STATE_SLOTS];
static u32 mtimes[STATE_SLOTS];             /* the file's modified time (with KIND_TIME) */
static u32 sizes[STATE_SLOTS];              /* the file's size (with KIND_FOUND) */
static unsigned char kinds[STATE_SLOTS];    /* KIND_*: what is known of the slot's file */
static u8 io_buffer[CHUNK];
/* a state being saved over frames is finished before anything else reads
 * or changes the slots (states_save_begin) */
static void settle(void);

/* kinds[]: a slot's file was looked at (its header and size read: one OS
 * open, no stat), found, is this build's kind (GPSZ), stored (not packed);
 * its time is known (a stat, only when something shows it). */
#define KIND_LOOKED 1u
#define KIND_FOUND  2u
#define KIND_GPSZ   4u
#define KIND_STORED 8u
#define KIND_TIME   16u

static void listing_forget(void);

/* ---- Files in two places (2026-11-17, final/usbsave8) ----
 * The slots this file keeps (used[], states_exists, states_save ...) are the
 * calculator's, as always: zgpSP_files. A USB drive is a second place for
 * the open game's states (nspire_places.h; "The open game's states on a USB
 * drive" below): "/gpsp/states/<game's name><slot>.svs", the calculator's
 * names without ".tns", the same bytes. The file calls after this comment
 * serve both: a path that starts with "usb:/" is the drive's
 * (nspire_drive.h). Nothing in this file reads or writes a drive from a
 * game's frame: the drive's calls are for safe points (the menus, a game's
 * start and end; nspire_drive_saves.c). */
void nspire_link_log(const char *fmt, ...) __attribute__((format(printf, 1, 2), weak));
void nspire_backup_finish(void) __attribute__((weak));   /* (save17:the game's save borrows the kept buffers) */
void nspire_backup_step(void) __attribute__((weak));
int nspire_backup_next_is_file(void) __attribute__((weak));
/* (weak: the PC's unit tests build without nspire_drive.c: no drive) */
#pragma weak nspire_drive_path
#pragma weak nspire_drive_fopen
#pragma weak nspire_drive_epoch
#pragma weak nspire_drive_put
#pragma weak nspire_drive_settle
#pragma weak nspire_drive_discard
#pragma weak nspire_drive_get
#pragma weak nspire_drive_get_into
#pragma weak nspire_drive_stat
#pragma weak nspire_drive_dir
#pragma weak nspire_backup_drive_top
#pragma weak nspire_drive_mem_held
#pragma weak nspire_drive_open_room
#pragma weak nspire_drive_trim

static unsigned long drive_held(void) { return nspire_drive_mem_held ? nspire_drive_mem_held() : 0; }
static unsigned long drive_open_room(void) { return nspire_drive_open_room ? nspire_drive_open_room() : 0; }

static int on_drive(const char *path)
{
  return nspire_drive_path && nspire_drive_path(path);
}

static unsigned drive_epoch(void)
{
  return nspire_drive_epoch ? nspire_drive_epoch() : 0;
}

static FILE *open_read(const char *path)
{
  FILE *f = on_drive(path) ? nspire_drive_fopen(path, "rb") : fopen(path, "rb");
  if (f)
    setvbuf(f, NULL, _IONBF, 0);
  return f;
}

/* nspire_file_recover; a drive's files have no .rollback (usbms_put) */
static int file_recover(const char *path)
{
  return on_drive(path) ? 1 : nspire_file_recover(path);
}

/* What changes when a place's files may have changed behind what is kept
 * of them: the calculator's moves, a drive's coming and going. */
static unsigned stamp_of(const char *path)
{
  return on_drive(path) ? drive_epoch() ^ 0x80000000u : nspire_save_folder_moves;
}

/* ---- How long saves and loads take ----
 *
 * The parts of the last saves and loads, in ticks, written to
 * gpsp_states_time.txt in the saves folder when gpSP closes: writing files
 * is what the PC can't measure. */
#define TIMINGS 40
enum { T_COPY, T_PACK, T_PICTURE, T_WRITE, T_REPLACE, T_READ = 0, T_UNPACK, T_APPLY, T_PARTS = 5 };

struct timing
{
  char what;     /* 's' save, 'l' load */
  int slot;
  long bytes;
  u32 parts[T_PARTS], total;
  u32 os_longest, os_calls;  /* (r19-snd) a save's OS calls: the longest (ticks), how many */
  u32 bank_ms, learned_ms;   /* (r19-bank) the most the sound's ring was banked for it, what was learned then */
};

static struct timing timings[TIMINGS];
static int timing_count;
static u32 timing_mark;

static struct timing *timing_start(char what, int slot)
{
  struct timing *t = &timings[timing_count++ % TIMINGS];
  memset(t, 0, sizeof(*t));
  t->what = what;
  t->slot = slot;
  timing_mark = nspire_ticks();
  return t;
}

/* The time since the last part goes to part. */
static void timing_part(struct timing *t, int part)
{
  u32 now = nspire_ticks();
  t->parts[part] += now - timing_mark;
  t->total += now - timing_mark;
  timing_mark = now;
}

#define TICKS_MS(t) (unsigned)((t) * 1000u / NSPIRE_TICK_HZ), (unsigned)((t) * 10000u / NSPIRE_TICK_HZ % 10)

/* Free-form lines for the same file: the ROM load's steps (load_gamepak). */
#define NOTES 12   /* (the ROM loads' and the game's saves': nspire_backup.c) */
static char notes[NOTES][160];
static int note_count;

void states_timing_note(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(notes[note_count % NOTES], sizeof(notes[0]), fmt, ap);
  va_end(ap);
  note_count++;
}

/* What the menus' look-ups took (the file system's part is what the PC
 * can't measure): the last few that read files, and how many came from
 * memory, written with the saves and loads. */
#define MENU_NOTES 24
static struct menu_timing
{
  char what;            /* 'g' a game's newest and picture, 'h' the same, stopped by a key; 'p' a picture;
                         * 'm' a menu's own step (states_menu_note) */
  char name[48];        /* the game's base name */
  short slot, headers;  /* the picture's slot; headers read */
  u32 listed, total;    /* ticks: the folder's names (0: kept), all of it */
} menu_timings[MENU_NOTES];
static int menu_timing_count;
static unsigned menu_kept_games, menu_kept_pictures;

static void menu_note(char what, const char *base, int slot, int headers, u32 listed, u32 total)
{
  struct menu_timing *t = &menu_timings[menu_timing_count++ % MENU_NOTES];
  const char *slash = strrchr(base, '/');
  t->what = what;
  snprintf(t->name, sizeof(t->name), "%s", slash ? slash + 1 : base);
  t->slot = (short)slot;
  t->headers = (short)headers;
  t->listed = listed;
  t->total = total;
}

void states_menu_note(const char *what, u32 ticks)
{
  menu_note('m', what, -1, 0, 0, ticks);
}

static int memory_look_count;          /* (save17:the memory looks, below) */
static void memory_looks_write(FILE *f);

void states_timing_flush(void)
{
  static const char *const save_parts[] = { "copy", "pack", "picture", "write", "replace" };
  static const char *const load_parts[] = { "read", "unpack", "apply" };
  char path[600];
  FILE *f;
  int i, j, first;

  if (!timing_count && !note_count && !menu_timing_count && !memory_look_count)
    return;
  /* a new file each time: appending has garbled files on the calculator */
  nspire_save_file(path, sizeof(path), "gpsp_states_time.txt.tns");
  f = fopen(path, "w");
  if (!f)
    return;
  nspire_log_buffered(f);   /* (r19-usb-exit: one OS call, not one a KB) */
  for (i = note_count > NOTES ? note_count - NOTES : 0; i < note_count; i++)
    fprintf(f, "%s\n", notes[i % NOTES]);
  note_count = 0;
  fprintf(f, "Saves and loads, ms (the last %d)\n", TIMINGS);
  first = timing_count > TIMINGS ? timing_count - TIMINGS : 0;
  for (i = first; i < timing_count; i++)
  {
    const struct timing *t = &timings[i % TIMINGS];
    fprintf(f, "%s slot %03d, %ld bytes: total %u.%u", t->what == 's' ? "save" : "load", t->slot, t->bytes,
            TICKS_MS(t->total));
    for (j = 0; j < (t->what == 's' ? 5 : 3); j++)
      fprintf(f, ", %s %u.%u", t->what == 's' ? save_parts[j] : load_parts[j], TICKS_MS(t->parts[j]));
    if (t->os_calls)
      fprintf(f, "; %u OS calls, the longest %u.%u; bank %u ms (learned %u)", (unsigned)t->os_calls,
              TICKS_MS(t->os_longest), (unsigned)t->bank_ms, (unsigned)t->learned_ms);
    fprintf(f, "\n");
  }
  memory_looks_write(f);
  fprintf(f, "Menu look-ups that read files, ms (the last %d; from memory: %u games, %u pictures)\n", MENU_NOTES,
          menu_kept_games, menu_kept_pictures);
  first = menu_timing_count > MENU_NOTES ? menu_timing_count - MENU_NOTES : 0;
  for (i = first; i < menu_timing_count; i++)
  {
    const struct menu_timing *t = &menu_timings[i % MENU_NOTES];
    if (t->what == 'p')
      fprintf(f, "picture %s%d: %u.%u\n", t->name, t->slot, TICKS_MS(t->total));
    else if (t->what == 'm')
      fprintf(f, "%s: %u.%u\n", t->name, TICKS_MS(t->total));
    else
      fprintf(f, "%s %s: %u.%u, %d headers, names %u.%u%s\n", t->what == 'g' ? "newest" : "part of newest",
              t->name, TICKS_MS(t->total), t->headers, TICKS_MS(t->listed), t->listed ? "" : " (kept)");
  }
  fclose(f);
  timing_count = 0;
  menu_timing_count = 0;
}

/* A zip's saves are named after the ROM in it, so the zipped and unzipped
 * game share them. The name is looked up once per zip. */
static int zip_name(const char *rom_path, char *out, size_t size)
{
  static char last_zip[512], last_inner[256];
  size_t len = strlen(rom_path);

  if (!((len > 8 && !strcasecmp(rom_path + len - 8, ".zip.tns")) ||
        (len > 4 && !strcasecmp(rom_path + len - 4, ".zip"))))
    return 0;
  if (strcmp(last_zip, rom_path))
  {
    if (!zip_rom_name(rom_path, last_inner, sizeof(last_inner)))
      return 0;
    snprintf(last_zip, sizeof(last_zip), "%s", rom_path);
  }
  snprintf(out, size, "%s", last_inner);
  return 1;
}

void nspire_game_name(const char *rom_path, char *out, size_t size)
{
  const char *slash = strrchr(rom_path, '/');
  const char *file = slash ? slash + 1 : rom_path;
  const char *dot = strrchr(file, '.');
  size_t len = dot && dot != file ? (size_t)(dot - file) : strlen(file), all = strlen(file);

  if (zip_name(rom_path, out, size))
    return;
  /* A USB drive's game needs no ".tns": "Game.gba" there and "Game.gba.tns"
   * on the calculator are one game, with one save and one set of states
   * (before, the drive's lost its ".gba" too and got files of its own). */
  if (all > 4 && (!strcasecmp(file + all - 4, ".gba") || !strcasecmp(file + all - 4, ".gbz") ||
                  !strcasecmp(file + all - 4, ".agb")))
    len = all;
  if (len >= size)
    len = size - 1;
  memcpy(out, file, len);
  out[len] = 0;
  /* (".GBA" on a drive too: the calculator's copy is ".gba.tns") */
  if (len >= 4 && (!strcasecmp(out + len - 4, ".gbz") || !strcasecmp(out + len - 4, ".gba")))
    memcpy(out + len - 4, ".gba", 4);
}

/* ZIP-6: saves go by the game's name, so two different games with the same
 * name (two zips holding "rom.gba", or "rom.gba" in two folders) shared one
 * save and its states. The first game to use a name leaves its ROM header's
 * game code beside the save ("<name>.id.tns", 4 bytes). A game of that name
 * with another code gets its own files, "<name>~CODE": it never reads or
 * overwrites the other's. The same game as a zip, a .gbz or a .gba has one
 * code, so they keep sharing. Hacks of one game (same code) still share. */
static char id_name[300], id_tag[8];   /* the game identified last, and its ~CODE (or "") */
static char claim_path[640];            /* its id file, to write once it has started ("" if there is one) */
static u8 claim_code[4];
/* The same on a USB drive (2026-11-17): the drive may hold another
 * calculator's games. "/gpsp/saves/<name>.id" has the code of the game that
 * owns the name there; a game of another code gets "<name>~CODE" on the
 * drive. Looked up when the game's files on a drive are first named
 * (nspire_game_drive_base), again for another drive; the id is written with
 * the game's first file there (nspire_game_drive_claim). */
static u8 id_code[4];
static int id_code_known;
static char drive_tag[8];
static int drive_tag_for = -1;          /* the drive (its epoch) the tag was looked up on; -1: not yet */
static int drive_claim;                 /* the name is free on the drive: ours with the first file written */

#define DRIVE_FOLDER "usb:/gpsp/"

void nspire_game_base(const char *rom_path, char *out, size_t size)
{
  char name[300];
  nspire_game_name(rom_path, name, sizeof(name));
  nspire_save_file(out, size, name);
  if (id_tag[0] && !strcmp(name, id_name) && strlen(out) + strlen(id_tag) < size)
    strcat(out, id_tag);
}

/* The 4-byte game code at 0xAC of the ROM, wherever it is stored; 0 if it
 * can't be read. */
static int rom_game_code(const char *rom_path, u8 code[4])
{
  int ok = 0;

  if (zip_is_zip(rom_path))
  {
    u8 head[192];
    zip_rom *zip = zip_open(rom_path);
    if (zip && zip_size(zip) >= 192 && zip_read(zip, head, 192) == 0)
    {
      memcpy(code, head + 0xAC, 4);
      ok = 1;
    }
    zip_close(zip);
    return ok;
  }
  {
    RFILE *file = filestream_open(rom_path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
    if (!file)
      return 0;
    if (gbz_is_gbz(file))
    {
      gbz_rom *gbz = gbz_open(file);
      u8 *page = gbz ? (u8 *)malloc(GBZ_PAGE_SIZE) : NULL;
      if (page && gbz_read_page(gbz, 0, page) == 0 && gbz_rom_size(gbz) >= 192)
      {
        memcpy(code, page + 0xAC, 4);
        ok = 1;
      }
      free(page);
      gbz_close(gbz);
    }
    else if (filestream_seek(file, 0xAC, RETRO_VFS_SEEK_POSITION_START) == 0)
      ok = filestream_read(file, code, 4) == 4;
    filestream_close(file);
  }
  return ok;
}

static void code_tag(const u8 code[4], char tag[8])
{
  int i;
  tag[0] = '~';
  for (i = 0; i < 4; i++)
    tag[1 + i] = isalnum(code[i]) ? (char)code[i] : '_';
  tag[5] = 0;
}

void nspire_game_identify(const char *rom_path)
{
  char name[300], base[600], id_path[640];
  u8 code[4], had[4];
  FILE *f;
  int known;

  nspire_game_name(rom_path, name, sizeof(name));
  snprintf(id_name, sizeof(id_name), "%s", name);
  id_tag[0] = 0;
  claim_path[0] = 0;
  id_code_known = 0;
  drive_tag[0] = 0;
  drive_tag_for = -1;
  drive_claim = 0;
  if (!rom_game_code(rom_path, code))
    return;
  memcpy(id_code, code, 4);
  id_code_known = 1;
  nspire_save_file(base, sizeof(base), name);
  snprintf(id_path, sizeof(id_path), "%s.id.tns", base);
  f = fopen(id_path, "rb");
  known = f && fread(had, 1, 4, f) == 4;
  if (f)
    fclose(f);
  if (known && memcmp(had, code, 4))
  {
    code_tag(code, id_tag);
    return;
  }
  if (!known)
  {
    /* The first game of this name keeps it, once it has started
     * (nspire_game_claim): a cancelled load leaves no file. */
    snprintf(claim_path, sizeof(claim_path), "%s", id_path);
    memcpy(claim_code, code, 4);
  }
}

void nspire_game_claim(void)
{
  FILE *f;

  if (!claim_path[0])
    return;
  nspire_save_dir_make();
  f = fopen(claim_path, "wb");
  if (f)
  {
    fwrite(claim_code, 1, 4, f);
    fclose(f);
  }
  claim_path[0] = 0;
}

void nspire_game_drive_base(const char *rom_path, const char *folder, char *out, size_t size)
{
  char name[300];
  int ours;

  nspire_game_name(rom_path, name, sizeof(name));
  ours = !strcmp(name, id_name);
  if (ours && id_code_known && drive_tag_for != (int)(drive_epoch() & 0x7FFFFFFF) && nspire_drive_get)
  {
    char id_path[400];
    unsigned long got = 0;
    u8 *had;

    snprintf(id_path, sizeof(id_path), DRIVE_FOLDER "saves/%s.id", name);
    had = (u8 *)nspire_drive_get(id_path, 16, &got);
    drive_tag[0] = 0;
    drive_claim = 0;
    if (had && got == 4 && memcmp(had, id_code, 4))
      code_tag(id_code, drive_tag);
    else if (!had && nspire_drive_stat && !nspire_drive_stat(id_path, NULL, NULL))
      drive_claim = 1;   /* (no id there; one that is there and can't be read is not written over) */
    free(had);
    drive_tag_for = (int)(drive_epoch() & 0x7FFFFFFF);
  }
  snprintf(out, size, DRIVE_FOLDER "%s/%s%s", folder, name, ours ? drive_tag : "");
}

void nspire_game_drive_claim(void)
{
  char id_path[400];

  if (!drive_claim || !id_code_known || !nspire_drive_put)
    return;
  drive_claim = 0;
  snprintf(id_path, sizeof(id_path), DRIVE_FOLDER "saves/%s.id", id_name);
  nspire_drive_put(id_path, id_code, 4);
}

void nspire_game_old_base(const char *rom_path, char *out, size_t size)
{
  char name[300];
  const char *slash = strrchr(rom_path, '/');
  nspire_game_name(rom_path, name, sizeof(name));
  snprintf(out, size, "%.*s%s", slash ? (int)(slash - rom_path) + 1 : 0, rom_path, name);
}

void nspire_game_move_old_files(const char *rom_path)
{
  char dir[512], old_base[600], base[600];
  const char *slash = strrchr(rom_path, '/');
  settle();

  snprintf(dir, sizeof(dir), "%.*s", slash ? (slash == rom_path ? 1 : (int)(slash - rom_path)) : 1,
           slash ? rom_path : ".");
  nspire_move_old_files(dir, 0);
  /* A game whose name the folder sweep doesn't know (a zip's ROM named
   * otherwise than .gba). */
  nspire_game_old_base(rom_path, old_base, sizeof(old_base));
  nspire_game_base(rom_path, base, sizeof(base));
  nspire_save_dir_make();
  nspire_move_game_files(old_base, base);
}

/* What follows the slot's number in a state's name: ".svs.tns", on a drive ".svs". */
static const char *state_suffix(const char *base)
{
  return on_drive(base) ? ".svs" : ".svs.tns";
}

/* A slot's file of a base ("<base>12.svs.tns"; a drive's "<base>12.svs"). */
static void slot_path(const char *base, int slot, char *out, size_t size)
{
  snprintf(out, size, "%s%d%s", base, slot, state_suffix(base));
}

void states_file_name(int slot, char *out, size_t size)
{
  slot_path(base_path, slot, out, size);
}

/* The slot of a file name like "Pokemon.gba12.svs.tns", or -1. */
static int slot_from_name(const char *name, const char *prefix, size_t prefix_len, const char *suffix)
{
  const char *p = name + prefix_len;
  int slot = 0, digits = 0;

  if (strncasecmp(name, prefix, prefix_len) != 0)
    return -1;
  while (isdigit((unsigned char)*p) && digits < 4)
  {
    slot = slot * 10 + (*p++ - '0');
    digits++;
  }
  if (!digits || slot >= STATE_SLOTS || (digits > 1 && p[-digits] == '0'))
    return -1;
  return strcasecmp(p, suffix) == 0 ? slot : -1;
}

/* A header and its file's size agree: the state was written whole (SAV-2). */
/* save17: a save time a wrong clock wrote (before 2020, or more than a day from now) is not used:
 * the file's time is then */
static int saved_time_ok(u32 t)
{
  time_t now = time(NULL);
  return t >= 1577836800u && (time_t)t <= now + 86400;
}

static int header_whole(const struct state_header *header, long size)
{
  return !memcmp(header->magic, STATE_MAGIC, 4) &&
         header->packed_size <= header->raw_size + header->raw_size / 8 + 1024 &&
         (u32)size == sizeof(*header) + header->packed_size +
                          ((header->flags & STATE_FLAG_PICTURE) ? header->reserved[0] : 0);
}

/* 1: a whole state; 0: not; -1: it can't be opened (a drive's that failed: not known) */
static int whole_state(const char *path)
{
  struct state_header header;
  FILE *f = open_read(path);
  long size = -1;
  int ok;

  if (!f)
    return -1;
  ok = fread(&header, sizeof(header), 1, f) == 1;
  if (fseek(f, 0, SEEK_END) == 0)
    size = ftell(f);
  fclose(f);
  return ok && size >= 0 && header_whole(&header, size);
}

/* What the menus need of a slot's file: its header's counter and flags, and
 * its size (from the open file: the stat and second open it took before cost
 * the calculator ~15 ms a slot). Its time is looked up only when shown
 * (slot_time). */
struct slot_info
{
  u32 counter, size;
  u32 saved_at;      /* its header's save time (0: none, the file's time) */
  unsigned char counted, kind;
};

static void read_info(const char *path, struct slot_info *info)
{
  struct state_header header;
  FILE *f = open_read(path);
  long size = -1;
  int got;

  memset(info, 0, sizeof(*info));
  info->kind = KIND_LOOKED;
  if (!f)
    return;
  got = fread(&header, sizeof(header), 1, f) == 1;
  if (fseek(f, 0, SEEK_END) == 0)
    size = ftell(f);
  fclose(f);
  if (size < 0)
    return;
  info->size = (u32)size;
  info->kind |= KIND_FOUND;
  if (got && !memcmp(header.magic, STATE_MAGIC, 4))
  {
    info->kind |= KIND_GPSZ | ((header.flags & STATE_FLAG_STORED) ? KIND_STORED : 0);
    /* SAV-2: a partial file (a reset in its write) has the newest counter
     * in its header: it never counts as the newest slot. */
    if (header_whole(&header, size))
    {
      info->counter = header.counter;
      info->counted = (header.flags & STATE_FLAG_COUNTED) != 0;
      info->saved_at = saved_time_ok(header.reserved[1]) ? header.reserved[1] : 0;
      if (info->saved_at)
        info->kind |= KIND_TIME;
    }
  }
}

/* The header's counter (0 for an old uncompressed state), its flag, the
 * file's size and kind. */
static void read_slot_info(int slot)
{
  char path[600];
  struct slot_info info;

  states_file_name(slot, path, sizeof(path));
  read_info(path, &info);
  counters[slot] = info.counter;
  counted[slot] = info.counted;
  sizes[slot] = info.size;
  kinds[slot] = info.kind;
  mtimes[slot] = info.saved_at;   /* (0 and no KIND_TIME: the file's, looked up when shown) */
}

/* A file's modified time (st_mtim: Ndless's os.h undefines the st_mtime
 * shorthand), 0 if it can't be read. */
static u32 file_time(const char *path)
{
  struct stat st;
  if (on_drive(path))
  {
    unsigned long when = 0;
    return nspire_drive_stat(path, NULL, &when) ? (u32)when : 0;
  }
  return stat(path, &st) == 0 ? (u32)st.st_mtim.tv_sec : 0;
}

/* R6: a slot's state is written as "<its name>.tmp" over an existing one,
 * then the old one is removed and the .tmp renamed (the calculator's rename
 * doesn't replace a file). A reset in between left only the .tmp, whole:
 * the slot was lost. Found when the slots are listed: a .tmp next to its
 * state is a write that didn't finish (dropped); a .tmp alone is the state
 * if it's whole (its header and its size say so), and takes its name. */
static unsigned char tmp_seen[STATE_SLOTS];

/* ---- The saves folder's state files, kept while gpSP runs ----
 * Its names (states and their .tmp and .rollback leftovers) are read once
 * and kept for the menus' look-ups of other games: an OS folder read is ~70
 * calls with a real calculator's saves. A game opened (states_open_game) reads it
 * again; gpSP's own changes to the names (a new or deleted state, a
 * recovered one, files moved in: nspire_save_folder_moves) drop it. */
static struct listing
{
  char dir[512];
  char *names;            /* the names, each ending in a 0 */
  size_t size, room;
  int ok;
  unsigned stamp;         /* stamp_of(dir) when it was read */
} listings[2];            /* the calculator's saves folder, a drive's states folder */

static struct listing *listing_of(const char *dir_path)
{
  return &listings[on_drive(dir_path) ? 1 : 0];
}

static void listing_forget(void)
{
  listings[0].ok = listings[1].ok = 0;
}

static int ends_with_case(const char *s, size_t n, const char *end, size_t m)
{
  return n >= m && !strncasecmp(s + n - m, end, m);
}

/* A name a state's slot can be read from (see states_scan): a state, or a
 * state's .tmp or .rollback (a drive's: ".svs", and its .tmp). */
static int state_like_name(const char *name)
{
  size_t n = strlen(name);
  if (ends_with_case(name, n, ".svs.tns", 8) || ends_with_case(name, n, ".svs", 4))
    return 1;
  if (ends_with_case(name, n, ".tmp", 4))
    return ends_with_case(name, n - 4, ".svs.tns", 8) || ends_with_case(name, n - 4, ".svs", 4);
  return n > 9 && !strcmp(name + n - 9, ".rollback") && ends_with_case(name, n - 9, ".svs.tns", 8);
}

static int listing_add(const char *name, unsigned long size, unsigned long mtime, void *arg)
{
  struct listing *l = (struct listing *)arg;
  size_t n = strlen(name) + 1;
  (void)size;
  (void)mtime;

  if (!state_like_name(name))
    return 1;
  if (l->size + n > l->room)
  {
    size_t want = (l->size + n) * 2 + 1024;
    char *grown = (char *)realloc(l->names, want);
    if (!grown)
      return 0;
    l->names = grown;
    l->room = want;
  }
  memcpy(l->names + l->size, name, n);
  l->size += n;
  return 1;
}

static int listing_read(const char *dir_path)
{
  struct listing *l = listing_of(dir_path);
  int ok = 1;

  l->ok = 0;
  l->size = 0;
  if (on_drive(dir_path))
    ok = nspire_drive_dir(dir_path, listing_add, l) == 0;
  else
  {
    DIR *dir = opendir(dir_path);
    struct dirent *entry;

    if (!dir)
      return 0;
    while (ok && (entry = readdir(dir)) != NULL)
      ok = listing_add(entry->d_name, 0, 0, l);
    closedir(dir);
  }
  if (!ok)
    return 0;
  snprintf(l->dir, sizeof(l->dir), "%s", dir_path);
  l->stamp = stamp_of(dir_path);
  l->ok = 1;
  /* r17: no more room kept than the names take (it doubled as it grew: 139 KB for 1,878 states' 68 KB) */
  if (l->names && l->room > l->size + 64)
  {
    char *fit = (char *)realloc(l->names, l->size + 64);
    if (fit)
    {
      l->names = fit;
      l->room = l->size + 64;
    }
  }
  return 1;
}

/* The slots base ("<folder>/Pokemon.gba") has (in its_used), with the R6
 * leftovers dealt with: from the kept listing unless fresh is set. 0: its
 * folder could not be read (no slots). */
static int states_scan(const char *base, unsigned char *its_used, int fresh)
{
  char dir_path[512], name[300];
  const char *prefix, *suffix = state_suffix(base);
  struct listing *l;
  size_t prefix_len, at;
  char *slash;
  int slot;
  settle();

  memset(its_used, 0, STATE_SLOTS);
  snprintf(dir_path, sizeof(dir_path), "%s", base);
  slash = strrchr(dir_path, '/');
  if (slash)
  {
    prefix = base + (slash - dir_path) + 1;
    if (slash == dir_path)
      slash[1] = 0;
    else
      *slash = 0;
  }
  else
  {
    prefix = base;
    strcpy(dir_path, ".");
  }
  prefix_len = strlen(prefix);

  l = listing_of(dir_path);
  if ((fresh || !l->ok || l->stamp != stamp_of(dir_path) || strcmp(l->dir, dir_path)) &&
      !listing_read(dir_path))
    return 0;
  memset(tmp_seen, 0, sizeof(tmp_seen));
  for (at = 0; at < l->size; at += strlen(l->names + at) + 1)
  {
    const char *entry = l->names + at;
    size_t n = strlen(entry);
    slot = slot_from_name(entry, prefix, prefix_len, suffix);
    if (slot >= 0)
      its_used[slot] = 1;
    else if (n > 4 && n < sizeof(name) && !strcasecmp(entry + n - 4, ".tmp"))
    {
      memcpy(name, entry, n - 4);
      name[n - 4] = 0;
      slot = slot_from_name(name, prefix, prefix_len, suffix);
      if (slot >= 0)
        tmp_seen[slot] |= 1;
    }
    else if (n > 9 && n < sizeof(name) && !strcmp(entry + n - 9, ".rollback"))
    {
      memcpy(name, entry, n - 9);
      name[n - 9] = 0;
      slot = slot_from_name(name, prefix, prefix_len, suffix);
      if (slot >= 0)
        tmp_seen[slot] |= 2;
    }
  }
  /* R6: after the folder's read (not renamed while it's read). A complete
   * lone .tmp may be the only surviving save: kept if its rename failed, so
   * the next scan can retry recovery. */
  for (slot = 0; slot < STATE_SLOTS; slot++)
  {
    char path[600];
    if (!tmp_seen[slot])
      continue;
    snprintf(path, sizeof(path), "%s%d%s", base, slot, suffix);
    if (tmp_seen[slot] & 2)
    {
      if (nspire_file_recover(path))
        its_used[slot] = 1;
      listing_forget();
    }
    if ((tmp_seen[slot] & 1) && on_drive(path))
    {
      /* A drive's: what a write cut short left of the slot (the drive pulled out) is made one file
       * again by its driver. A .tmp that is a whole state is the new one: it takes the name
       * (nspire_drive_settle). One that is not is dropped, and the state of the name stays
       * (nspire_drive_discard). Never the .tmp removed by its name here: after a rename cut in two
       * it shares its clusters with the state, and removing it freed them. One that can't be read
       * now is left as it is (it may be the only copy of a state): the next read decides. */
      char temp_path[620];
      unsigned long size = 0;
      int whole;
      snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
      whole = whole_state(temp_path);
      if (whole > 0)
        nspire_drive_settle(path);
      else if (whole == 0)
        nspire_drive_discard(path);
      its_used[slot] = nspire_drive_stat(path, &size, NULL) && size != 0;
      listing_forget();
    }
    else if (tmp_seen[slot] & 1)
    {
      char temp_path[620];
      snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
      if (!its_used[slot] && whole_state(temp_path) > 0)
      {
        if (rename(temp_path, path) == 0)
          its_used[slot] = 1;
      }
      else
        remove(temp_path);
      listing_forget();
    }
  }
  return 1;
}

static void kept_put_open(void);
static unsigned open_moves;   /* nspire_save_folder_moves when the open game's slots were read */
static void drive_view_game(const char *rom_path);

void states_open_game(const char *rom_path)
{
  int slot;

  settle();
  if (base_path[0] && open_moves == nspire_save_folder_moves)
    kept_put_open();   /* the game left, as it is now */
  nspire_game_base(rom_path, base_path, sizeof(base_path));
  drive_view_game(rom_path);
  open_moves = nspire_save_folder_moves;
  memset(counted, 0, sizeof(counted));
  memset(counters, 0, sizeof(counters));
  memset(mtimes, 0, sizeof(mtimes));
  memset(sizes, 0, sizeof(sizes));
  memset(kinds, 0, sizeof(kinds));
  states_scan(base_path, used, 1);
  for (slot = 0; slot < STATE_SLOTS; slot++)
    if (used[slot])
      read_slot_info(slot);
  kept_put_open();
}

int states_exists(int slot)
{
  settle();
  return slot >= 0 && slot < STATE_SLOTS && used[slot];
}

int states_count(void)
{
  int slot, count = 0;
  settle();
  for (slot = 0; slot < STATE_SLOTS; slot++)
    count += used[slot];
  return count;
}

/* This build's states count their saves, which still orders them after the
 * calculator's clock is reset (as a reset of the calculator does). States
 * from older builds don't: while a game has none of this build's, the most
 * recently modified file is the newest. */
static u32 slot_time(int slot);

int states_newest(void)
{
  int slot, newest = -1, any_counted = 0;
  settle();

  for (slot = 0; slot < STATE_SLOTS; slot++)
    any_counted |= used[slot] && counted[slot];
  if (!any_counted)
    for (slot = 0; slot < STATE_SLOTS; slot++)
      if (used[slot])
        slot_time(slot);   /* (the times are looked up only now) */
  for (slot = 0; slot < STATE_SLOTS; slot++)
  {
    if (!used[slot] || (any_counted && !counted[slot]))
      continue;
    if (newest < 0 ||
        (any_counted ? counters[slot] >= counters[newest] :
                       mtimes[slot] > mtimes[newest] ||
                       (mtimes[slot] == mtimes[newest] && counters[slot] >= counters[newest])))
      newest = slot;
  }
  return newest;
}

/* (r19-fix: the slots being saved or asked for counted as used, nothing finished for it: in a game the
 * key's auto-increment and the autosave's new slot asked this, and finished the save under way at once,
 * masked) */
int states_next_new_slot(void)
{
  int slot, highest = -1;
  for (slot = 0; slot < STATE_SLOTS; slot++)
    if (states_exists_soon(slot))
      highest = slot;
  if (highest < STATE_SLOTS - 1)
    return highest + 1;
  for (slot = 0; slot < STATE_SLOTS; slot++)
    if (!states_exists_soon(slot))
      return slot;
  return -1;
}

/* The state in memory. The buffer starts zeroed so the state's unused room
 * (its "zpadding" field) packs down to almost nothing. */
static u8 *state_to_memory(void)
{
  u8 *raw = (u8 *)calloc(1, GBA_STATE_MEM_SIZE);
  if (raw)
    gba_save_state(raw);
  return raw;
}

/* The game's last frame at a fifth of its size... at 3/5: 144x96. */
static int make_picture(u16 *pic)
{
  if (!gba_screen_pixels)
    return 0;
  ui_scale(pic, STATE_PIC_W, STATE_PIC_H, gba_screen_pixels, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT,
           GBA_SCREEN_PITCH);
  return 1;
}

/* Packs the picture into block (PICTURE_MAX bytes); returns its size, 0 if
 * it didn't pack. */
static u32 pack_picture(const u16 *pic, u8 *block)
{
  struct picture_header ph;
  size_t packed;
  /* (any picture fits: never left out for room) */
  (void)sizeof(char[FASTDEFLATE_BOUND(STATE_PIC_W * STATE_PIC_H * 2) <= PICTURE_MAX - sizeof(ph) ? 1 : -1]);
  packed = fastdeflate_picture(block + sizeof(ph), PICTURE_MAX - sizeof(ph), (const unsigned char *)pic,
                               STATE_PIC_W * STATE_PIC_H * 2, STATE_PIC_W * 2);
  if (!packed)
    return 0;
  ph.w = STATE_PIC_W;
  ph.h = STATE_PIC_H;
  ph.packed_size = (u32)packed;
  memcpy(block, &ph, sizeof(ph));
  return (u32)(sizeof(ph) + packed);
}

/* The picture block after a state, or 0. */
static u32 read_picture_block(FILE *f, const struct state_header *header, u8 *block)
{
  u32 size = header->reserved[0];
  if (!(header->flags & STATE_FLAG_PICTURE) || size < sizeof(struct picture_header) || size > PICTURE_MAX ||
      fseek(f, (long)(sizeof(*header) + header->packed_size), SEEK_SET) != 0)
    return 0;
  return fread(block, 1, size, f) == size ? size : 0;
}

static int unpack_picture(const u8 *block, u32 size, u16 *pic)
{
  struct picture_header ph;
  uLongf raw = STATE_PIC_W * STATE_PIC_H * 2;
  memcpy(&ph, block, sizeof(ph));
  if (ph.w != STATE_PIC_W || ph.h != STATE_PIC_H || ph.packed_size + sizeof(ph) > size)
    return 0;
  return uncompress((Bytef *)pic, &raw, block + sizeof(ph), ph.packed_size) == Z_OK &&
         raw == STATE_PIC_W * STATE_PIC_H * 2;
}

/* The picture of the state in a file: 1 if it has one. */
static int file_picture(const char *path, u16 *pic)
{
  static u8 block[PICTURE_MAX];
  struct state_header header;
  int ok = 0;
  FILE *f = open_read(path);
  u32 size;

  if (!f)
    return 0;
  if (fread(&header, sizeof(header), 1, f) == 1 && !memcmp(header.magic, STATE_MAGIC, 4) &&
      (size = read_picture_block(f, &header, block)) != 0)
    ok = unpack_picture(block, size, pic);
  fclose(f);
  return ok;
}

/* ---- What the menus show, kept while gpSP runs ----
 * The menus look the same files up again and again: the list each game's
 * newest state and its picture each time it opens, the paused menu the
 * other games' at each step of Resume's up and down, the strips their
 * slots' pictures each time the menu opens. What was read is kept: other
 * games' slots (each used slot's counter, size, kind, and time once looked
 * up) and the last pictures, unpacked. gpSP is the only one that writes
 * the saves folder while it runs: what it writes there is the open game's
 * (states_save, states_delete, states_set_compressed, which keep its slots
 * as they are), or moves (nspire_save_folder_moves), which drop it all. */
struct kept_slot
{
  u16 slot;
  unsigned char counted, kind;
  u32 counter, size, mtime;
};

#define KEPT_GAMES 16
static struct kept_game
{
  char base[sizeof(base_path)];
  unsigned moves;               /* nspire_save_folder_moves when it was read */
  int count;                    /* used slots */
  struct kept_slot *slots;      /* malloc'd, in slot order */
  u32 last;                     /* when last used: the oldest goes first */
} kept[KEPT_GAMES];
static u32 kept_clock;

static struct kept_game *kept_find(const char *base)
{
  int i;
  for (i = 0; i < KEPT_GAMES; i++)
    if (kept[i].slots && kept[i].moves == stamp_of(base) && !strcmp(kept[i].base, base))
    {
      kept[i].last = ++kept_clock;
      return &kept[i];
    }
  return NULL;
}

/* An entry for base with room for count slots (the oldest goes); NULL with
 * no memory. */
static struct kept_game *kept_make(const char *base, int count)
{
  struct kept_game *k = NULL;
  struct kept_slot *slots;
  int i;

  for (i = 0; i < KEPT_GAMES && !k; i++)
    if (kept[i].slots && !strcmp(kept[i].base, base))
      k = &kept[i];
  for (i = 0; i < KEPT_GAMES && !k; i++)
    if (!kept[i].slots)
      k = &kept[i];
  if (!k)
  {
    k = &kept[0];
    for (i = 1; i < KEPT_GAMES; i++)
      if (kept[i].last < k->last)
        k = &kept[i];
  }
  slots = (struct kept_slot *)malloc(sizeof(*slots) * (count ? count : 1));
  if (!slots)
    return NULL;
  free(k->slots);
  memset(k, 0, sizeof(*k));
  snprintf(k->base, sizeof(k->base), "%s", base);
  k->moves = stamp_of(base);
  k->count = count;
  k->slots = slots;
  k->last = ++kept_clock;
  return k;
}

/* The open game's slots as they are now, kept (a game's start can take
 * them, and the menus once another game is open). */
static void kept_put_open(void)
{
  struct kept_game *k;
  int slot, n = 0;

  for (slot = 0; slot < STATE_SLOTS; slot++)
    n += used[slot];
  k = kept_make(base_path, n);
  if (!k)
    return;
  for (slot = 0, n = 0; slot < STATE_SLOTS; slot++)
    if (used[slot])
    {
      struct kept_slot *e = &k->slots[n++];
      e->slot = (u16)slot;
      e->counted = counted[slot];
      e->kind = kinds[slot];
      e->counter = counters[slot];
      e->size = sizes[slot];
      e->mtime = mtimes[slot];
    }
}

/* The newest of a kept game's slots, as states_newest (-1: none; -2: a
 * time is needed that isn't known). */
static int kept_newest(const struct kept_game *k)
{
  int i, newest = -1, any_counted = 0;

  for (i = 0; i < k->count; i++)
    any_counted |= k->slots[i].counted;
  for (i = 0; i < k->count; i++)
  {
    const struct kept_slot *e = &k->slots[i];
    if (any_counted && !e->counted)
      continue;
    if (!any_counted && !(e->kind & KIND_TIME))
      return -2;
    if (newest < 0 ||
        (any_counted ? e->counter >= k->slots[newest].counter :
                       e->mtime > k->slots[newest].mtime ||
                       (e->mtime == k->slots[newest].mtime && e->counter >= k->slots[newest].counter)))
      newest = i;
  }
  return newest;
}

/* ---- The pictures, unpacked (27 KB each), the last few ---- */
#define KEPT_PICTURES 12
static struct
{
  int kept;                     /* 0: nothing kept here */
  u32 game, counter, size;      /* game: the base's hash; counter and size: which state */
  int slot;
  int has;                      /* 0: the state has no picture */
  u32 last;
  u16 *pixels;                  /* malloc'd once, kept for the next one */
} pictures[KEPT_PICTURES];
static u32 picture_clock;
static unsigned picture_moves;   /* nspire_save_folder_moves they were read at: files moved in drop them */

static void pictures_check_moves(void)
{
  int i;
  if (picture_moves == nspire_save_folder_moves)
    return;
  for (i = 0; i < KEPT_PICTURES; i++)
    pictures[i].kept = 0;
  picture_moves = nspire_save_folder_moves;
}

static u32 base_hash(const char *base)
{
  u32 hash = 2166136261u;   /* FNV-1a */
  for (; *base; base++)
    hash = (hash ^ (unsigned char)*base) * 16777619u;
  return hash;
}

static void picture_keep(u32 game, int slot, u32 counter, u32 size, int has, const u16 *pic)
{
  int i, k = 0;

  pictures_check_moves();
  for (i = 0; i < KEPT_PICTURES; i++)
    if (pictures[i].kept && pictures[i].slot == slot && pictures[i].game == game)
    {
      k = i;   /* the slot's older picture goes */
      break;
    }
    else if (pictures[i].last < pictures[k].last)
      k = i;
  if (has && !pictures[k].pixels)
    pictures[k].pixels = (u16 *)malloc(STATE_PIC_W * STATE_PIC_H * 2);
  pictures[k].kept = !has || pictures[k].pixels != NULL;
  if (!pictures[k].kept)
    return;
  pictures[k].game = game;
  pictures[k].slot = slot;
  pictures[k].counter = counter;
  pictures[k].size = size;
  pictures[k].has = has;
  pictures[k].last = ++picture_clock;
  if (has)
    memcpy(pictures[k].pixels, pic, STATE_PIC_W * STATE_PIC_H * 2);
}

static uint32_t serve_begin(int *began);
static void serve_end(int began, uint32_t cpsr);

/* A state's picture (1, in pic), none (0), from memory or its file; -1 when
 * it isn't in memory and read_files is 0. */
static int picture_of(const char *base, int slot, u32 counter, u32 size, u16 *pic, int read_files)
{
  char path[600];
  u32 game = base_hash(base), start;
  int i, has;

  pictures_check_moves();
  for (i = 0; i < KEPT_PICTURES; i++)
    if (pictures[i].kept && pictures[i].slot == slot && pictures[i].game == game && pictures[i].counter == counter &&
        pictures[i].size == size)
    {
      pictures[i].last = ++picture_clock;
      if (pictures[i].has && pic)
        memcpy(pic, pictures[i].pixels, STATE_PIC_W * STATE_PIC_H * 2);
      menu_kept_pictures++;
      return pictures[i].has;
    }
  if (!read_files)
    return -1;
  start = nspire_ticks();
  slot_path(base, slot, path, sizeof(path));
  {
    static u16 scratch[STATE_PIC_W * STATE_PIC_H];
    u16 *into = pic ? pic : scratch;
    int began = 0;
    /* (r19-snd: the menu's: 110 ms a picture in the r19 log, served; a drive's: its driver's own way) */
    uint32_t cpsr = on_drive(path) ? 0 : serve_begin(&began);
    has = file_picture(path, into);
    serve_end(began, cpsr);
    picture_keep(game, slot, counter, size, has, into);
  }
  menu_note('p', base, slot, 0, 0, nspire_ticks() - start);
  return has;
}

int states_picture(int slot, u16 *pic)
{
  return states_picture_soon(slot, pic, 1) == 1;
}

int states_picture_soon(int slot, u16 *pic, int read_files)
{
  settle();
  if (!states_exists(slot))
    return 0;
  if (!(kinds[slot] & KIND_LOOKED))
  {
    if (!read_files)
      return -1;
    read_slot_info(slot);
  }
  return picture_of(base_path, slot, counters[slot], sizes[slot], pic, read_files);
}

static u32 slot_time(int slot)
{
  if (!(kinds[slot] & KIND_TIME))
  {
    char path[600];
    states_file_name(slot, path, sizeof(path));
    mtimes[slot] = file_time(path);
    kinds[slot] |= KIND_TIME;
  }
  return mtimes[slot];
}

/* When slot was saved (its file's modified time), 0 when not known. */
long states_time(int slot)
{
  settle();
  return slot >= 0 && slot < STATE_SLOTS && used[slot] ? (long)slot_time(slot) : 0;
}

long states_time_soon(int slot, int read_files)
{
  if (slot >= 0 && slot < STATE_SLOTS && used[slot] && !(kinds[slot] & KIND_TIME) && !read_files)
    return -1;
  return states_time(slot);
}

int (*states_yield)(void);

int states_peek_soon(const char *rom_path, int *count, int *newest, u16 *pic, long *when, int read_files)
{
  char base[sizeof(base_path)];
  struct kept_game *k;
  int i, n, read = 0, has;
  u32 start = 0, listed = 0, headers = 0;   /* (the clock is read only when files are) */
  settle();

  *count = 0;
  *newest = -1;
  if (when)
    *when = 0;
  nspire_game_base(rom_path, base, sizeof(base));
  /* the open game: its slots as they are */
  if (!strcmp(base, base_path) && open_moves == nspire_save_folder_moves)
  {
    if (!read_files)
    {
      /* only older builds' states: the newest by their times, which may not be known */
      int slot, any_counted = 0, times = 1;
      for (slot = 0; slot < STATE_SLOTS; slot++)
        if (used[slot])
        {
          any_counted |= counted[slot];
          times &= (kinds[slot] & KIND_TIME) != 0;
        }
      if (!any_counted && !times)
        return -1;
    }
    *count = states_count();
    *newest = states_newest();
    if (when)
      *when = 0;
    if (*newest < 0)
      return 0;
    if (!(kinds[*newest] & KIND_TIME) && when && !read_files)
      return -1;
    has = pic ? states_picture_soon(*newest, pic, read_files) : 0;
    if (has >= 0 && when)
      *when = states_time(*newest);
    return has;
  }

  k = kept_find(base);
  if (!k)
  {
    unsigned char *its_used;
    if (!read_files)
      return -1;
    its_used = (unsigned char *)malloc(STATE_SLOTS);
    if (!its_used)
      return 0;
    start = nspire_ticks();
    states_scan(base, its_used, 0);
    for (i = n = 0; i < STATE_SLOTS; i++)
      n += its_used[i];
    k = kept_make(base, n);
    if (k)
      for (i = n = 0; i < STATE_SLOTS; i++)
        if (its_used[i])
        {
          memset(&k->slots[n], 0, sizeof(k->slots[n]));
          k->slots[n++].slot = (u16)i;
        }
    free(its_used);
    if (!k)
      return 0;
    listed = nspire_ticks() - start;
  }
  /* the slots' headers not read yet; a key going down stops it after one
   * (the next call goes on) */
  for (i = 0; i < k->count; i++)
  {
    struct kept_slot *e = &k->slots[i];
    struct slot_info info;
    char path[600];
    if (e->kind & KIND_LOOKED)
      continue;
    if (!read_files || (read && states_yield && states_yield()))
    {
      if (read || listed)
        menu_note('h', base, -1, read, listed, nspire_ticks() - start);
      return -1;
    }
    if (!read && !listed)
      start = nspire_ticks();
    slot_path(base, e->slot, path, sizeof(path));
    read_info(path, &info);
    e->counter = info.counter;
    e->counted = info.counted;
    e->mtime = info.saved_at;
    e->size = info.size;
    e->kind = info.kind;
    read++;
  }
  headers = read || listed ? nspire_ticks() - start : 0;
  *count = k->count;
  i = kept_newest(k);
  if (i == -2)
  {
    /* only older builds' states: the newest is the latest file */
    if (!read_files)
      return -1;
    for (n = 0; n < k->count; n++)
      if (!(k->slots[n].kind & KIND_TIME))
      {
        char path[600];
        slot_path(base, k->slots[n].slot, path, sizeof(path));
        k->slots[n].mtime = file_time(path);
        k->slots[n].kind |= KIND_TIME;
      }
    i = kept_newest(k);
  }
  *newest = i >= 0 ? k->slots[i].slot : -1;
  if (when)
    *when = 0;
  if (i < 0)
  {
    if (read || listed)
      menu_note('g', base, -1, read, listed, headers);
    return 0;
  }
  if (when && !(k->slots[i].kind & KIND_TIME))
  {
    char path[600];
    if (!read_files)
      return -1;
    slot_path(base, k->slots[i].slot, path, sizeof(path));
    k->slots[i].mtime = file_time(path);
    k->slots[i].kind |= KIND_TIME;
  }
  if (when)
    *when = (long)k->slots[i].mtime;
  has = pic ? picture_of(base, k->slots[i].slot, k->slots[i].counter, k->slots[i].size, pic, read_files) : 0;
  if (read || listed)
    menu_note('g', base, -1, read, listed, nspire_ticks() - start);
  else if (has >= 0)
    menu_kept_games++;
  return has;
}

int states_peek_when(const char *rom_path, int *count, int *newest, u16 *pic, long *when)
{
  int (*yield)(void) = states_yield, has;
  states_yield = NULL;   /* all of it now */
  has = states_peek_soon(rom_path, count, newest, pic, when, 1);
  states_yield = yield;
  return has > 0;
}

int states_peek(const char *rom_path, int *count, int *newest, u16 *pic)
{
  return states_peek_when(rom_path, count, newest, pic, NULL);
}

void states_open_game_kept(const char *rom_path)
{
  char base[sizeof(base_path)];
  struct kept_game *k;
  int i;

  settle();
  nspire_game_base(rom_path, base, sizeof(base));
  if (!strcmp(base, base_path) && open_moves == nspire_save_folder_moves)
    return;   /* open already: its slots are as they are */
  k = kept_find(base);
  for (i = 0; k && i < k->count; i++)
    if (!(k->slots[i].kind & KIND_LOOKED))
      k = NULL;   /* not all read: read them now */
  if (!k)
  {
    states_open_game(rom_path);
    return;
  }
  if (base_path[0] && open_moves == nspire_save_folder_moves)
    kept_put_open();   /* the game left, as it is now (k, just used, stays) */
  snprintf(base_path, sizeof(base_path), "%s", base);
  drive_view_game(rom_path);
  open_moves = nspire_save_folder_moves;
  memset(used, 0, sizeof(used));
  memset(counted, 0, sizeof(counted));
  memset(counters, 0, sizeof(counters));
  memset(mtimes, 0, sizeof(mtimes));
  memset(sizes, 0, sizeof(sizes));
  memset(kinds, 0, sizeof(kinds));
  for (i = 0; i < k->count; i++)
  {
    const struct kept_slot *e = &k->slots[i];
    used[e->slot] = 1;
    counted[e->slot] = e->counted;
    counters[e->slot] = e->counter;
    sizes[e->slot] = e->size;
    mtimes[e->slot] = e->mtime;
    kinds[e->slot] = e->kind;
  }
}

/* A state just written (states_save): its size and kind, and its picture
 * (the one it was given) kept; its name may be new in the folder. */
static void slot_saved(int slot, long size, const struct state_header *header, const u16 *pic)
{
  sizes[slot] = (u32)size;
  kinds[slot] = KIND_LOOKED | KIND_FOUND | KIND_GPSZ | KIND_TIME |
                ((header->flags & STATE_FLAG_STORED) ? KIND_STORED : 0);
  listing_forget();
  picture_keep(base_hash(base_path), slot, header->counter, (u32)size, (header->flags & STATE_FLAG_PICTURE) != 0,
               pic);
}

/* ---- The open game's states on a USB drive (2026-11-17, final/usbsave8) ----
 *
 * The second place of nspire_places.h (the paused menu's USB DRIVE tab;
 * nspire_drive_saves.c has its table of functions and what gpSP does by
 * itself with Saves on: USB drive). The game's files there are
 * "/gpsp/states/<name><slot>.svs", the calculator's bytes.
 *
 * What is known of them is kept in memory, and the menus' questions
 * (exists, next, time, picture) are answered from that alone. The drive is
 * read by states_drive_read (the folder's names, sizes and times, then the
 * highest slot's header and picture: a new save goes after the highest, so
 * that one is the newest), by a picture asked for with read_file, and by a
 * load or a copy; it is written by states_drive_save and a copy there. All
 * of these are for safe points: none is called from a game's frame.
 *
 * Counts: a state's header counts the saves (the newest has the highest
 * count). So that the newest of both places can be told without clocks, a
 * save in either place counts from the highest count known of both
 * (counter_floor: the drive's top as read, and what nspire_drive_saves.c
 * kept of it on the calculator from the runs before). */
struct drive_slot
{
  u16 slot;
  u32 size, mtime;   /* from the folder (its time: FAT's, 2 s steps; the picture cache's key) */
  u32 saved_at;      /* its header's save time where read or written here (0: not known: mtime says it) */
  u32 counter;       /* its header's count, where it was read or written here (0: not known) */
};
/* The newest state there is its highest slot's (one header read when the
 * folder is read), unless this calculator knows of a higher count saved on
 * a drive (nspire_backup_drive_top, kept beside the calculator's save): a
 * copy of an older state made with c or T is in the highest slot then, and
 * Resume last save must not take it for the newest: the slots below are
 * looked at too, DRIVE_LOOKED in all (a header read is 4 or 5 commands: the
 * path's folders, then the file). */
#define DRIVE_LOOKED 4

static struct
{
  char rom[512];              /* the open game's ROM (its name on the drive is asked of the drive) */
  char base[512];             /* "usb:/gpsp/states/Pokemon.gba" ("" until read) */
  unsigned stamp;             /* stamp_of(base) when it was read: another drive, or this one put back, is read again */
  int known, count, room;
  struct drive_slot *slots;   /* in slot order */
  u32 top;                    /* the highest count known there */
  int newest;                 /* the slot that has it (-1: none) */
} dv;
static u32 counter_floor;

static void drive_view_game(const char *rom_path)
{
  snprintf(dv.rom, sizeof(dv.rom), "%s", rom_path);
  dv.base[0] = 0;
  dv.known = dv.count = 0;
  dv.top = 0;
  dv.newest = -1;
  counter_floor = 0;
}

void states_counter_floor(u32 count)
{
  if (count > counter_floor)
    counter_floor = count;
}

u32 states_counter_floor_now(void)
{
  return counter_floor;
}

u32 states_newest_counter(void)
{
  u32 counter = 0;
  int slot;
  settle();
  for (slot = 0; slot < STATE_SLOTS; slot++)
    if (used[slot] && counters[slot] > counter)
      counter = counters[slot];
  return counter;
}

int states_drive_known(void)
{
  return dv.known && dv.base[0] && dv.stamp == stamp_of(dv.base);
}

static struct drive_slot *dv_find(int slot)
{
  int i;
  for (i = 0; i < dv.count; i++)
    if (dv.slots[i].slot == slot)
      return &dv.slots[i];
  return NULL;
}

/* The slot's entry, made if it has none (in slot order); NULL: no memory. */
static struct drive_slot *dv_add(int slot)
{
  struct drive_slot *e = dv_find(slot);
  int i;

  if (e)
    return e;
  if (dv.count == dv.room)
  {
    int room = dv.room ? dv.room * 2 : 16;
    struct drive_slot *grown = (struct drive_slot *)realloc(dv.slots, sizeof(*grown) * (size_t)room);
    if (!grown)
      return NULL;
    dv.slots = grown;
    dv.room = room;
  }
  for (i = dv.count; i > 0 && dv.slots[i - 1].slot > slot; i--)
    dv.slots[i] = dv.slots[i - 1];
  dv.count++;
  memset(&dv.slots[i], 0, sizeof(dv.slots[i]));
  dv.slots[i].slot = (u16)slot;
  return &dv.slots[i];
}

struct dv_walk
{
  const char *prefix;
  size_t prefix_len;
};

static int dv_each(const char *name, unsigned long size, unsigned long mtime, void *arg)
{
  const struct dv_walk *w = (const struct dv_walk *)arg;
  int slot = slot_from_name(name, w->prefix, w->prefix_len, ".svs");
  struct drive_slot *e = slot >= 0 ? dv_find(slot) : NULL;
  if (e)
  {
    e->size = (u32)size;
    e->mtime = (u32)mtime;
  }
  return 1;
}

u32 states_drive_top(void)
{
  return dv.top;
}

int states_drive_read(void)
{
  static unsigned char found[STATE_SLOTS];
  char dir[sizeof(dv.base)], path[600];
  struct dv_walk walk;
  struct slot_info info;
  char *slash;
  int slot;

  settle();
  dv.known = dv.count = 0;
  dv.top = 0;
  dv.newest = -1;
  if (!dv.rom[0] || !nspire_drive_dir || !nspire_drive_path)
    return 0;
  nspire_game_drive_base(dv.rom, "states", dv.base, sizeof(dv.base));
  /* the folder's names (no folder yet: none); a cut write's leftovers dealt with, as the calculator's */
  if (!states_scan(dv.base, found, 1))
    return 0;
  for (slot = 0; slot < STATE_SLOTS; slot++)
    if (found[slot] && !dv_add(slot))
      return 0;
  /* their sizes and times, from the folder as the driver has it now (no read of the drive more) */
  snprintf(dir, sizeof(dir), "%s", dv.base);
  slash = strrchr(dir, '/');
  walk.prefix = dv.base + (slash - dir) + 1;
  walk.prefix_len = strlen(walk.prefix);
  *slash = 0;
  if (dv.count && nspire_drive_dir(dir, dv_each, &walk) != 0)
  {
    dv.count = 0;
    return 0;
  }
  dv.stamp = stamp_of(dv.base);
  dv.known = 1;
  if (dv.count)
  {
    /* the newest and its count (a save here counts from it); the highest slot's picture kept for
     * the strip (the menus open the drive's tab on it) */
    struct drive_slot *e;
    u32 kept = nspire_backup_drive_top ? nspire_backup_drive_top() : 0;
    int i;
    for (i = dv.count - 1; i >= 0 && i >= dv.count - DRIVE_LOOKED; i--)
    {
      e = &dv.slots[i];
      slot_path(dv.base, e->slot, path, sizeof(path));
      read_info(path, &info);
      e->counter = info.counter;
      if (info.saved_at)
        e->saved_at = info.saved_at;   /* (save17: its header's save time, shown over the file's) */
      if (dv.newest < 0 || info.counter > dv.top)
      {
        dv.top = info.counter;
        dv.newest = e->slot;
      }
      if (dv.top >= kept)
        break;   /* (nothing newer was ever saved on a drive from here) */
    }
    e = &dv.slots[dv.count - 1];
    picture_of(dv.base, e->slot, e->mtime, e->size, NULL, 1);
  }
  states_counter_floor(dv.top);
  return 1;
}

/* A state of that count is in the drive's slot now. */
static void dv_counted(struct drive_slot *e, u32 counter)
{
  if (e)
    e->counter = counter;
  if (e && (dv.newest < 0 || counter >= dv.top))
    dv.newest = e->slot;
  if (counter > dv.top)
    dv.top = counter;
  states_counter_floor(dv.top);
}

int states_drive_exists(int slot)
{
  return states_drive_known() && dv_find(slot) != NULL;
}

int states_drive_count(void)
{
  return states_drive_known() ? dv.count : 0;
}

int states_drive_newest(void)
{
  if (!states_drive_known() || !dv.count)
    return -1;
  return dv.newest >= 0 && dv_find(dv.newest) ? dv.newest : dv.slots[dv.count - 1].slot;
}

int states_drive_next(void)
{
  int slot, highest;

  if (!states_drive_known())
    return -1;
  highest = dv.count ? dv.slots[dv.count - 1].slot : -1;
  if (highest < STATE_SLOTS - 1)
    return highest + 1;
  for (slot = 0; slot < STATE_SLOTS; slot++)
    if (!dv_find(slot))
      return slot;
  return -1;
}

long states_drive_time(int slot)
{
  const struct drive_slot *e = states_drive_known() ? dv_find(slot) : NULL;
  return e ? (long)(e->saved_at ? e->saved_at : e->mtime) : 0;
}

int states_drive_picture(int slot, u16 *out, int read_file)
{
  const struct drive_slot *e = states_drive_known() ? dv_find(slot) : NULL;
  if (!e)
    return 0;
  return picture_of(dv.base, slot, e->mtime, e->size, out, read_file);
}

void nspire_link_state_loaded(int slot) __attribute__((weak));
static void failure_log(const char *what, int slot);

int states_drive_load(int slot)
{
  char path[600];
  int result;

  settle();
  if (!states_drive_exists(slot))
    return 0;
  slot_path(dv.base, slot, path, sizeof(path));
  states_memory_look("load (USB drive)", 1);
  result = states_load_file(path);
  timings[(timing_count - 1) % TIMINGS].slot = slot;
  if (result < 0)
    failure_log("load (USB drive)", slot);
  if (result == 1 && nspire_link_state_loaded)
    nspire_link_state_loaded(slot);
  return result ? result : -1;   /* (the folder had it and it can't be opened: the drive went) */
}

enum { SJ_TO_CALC, SJ_TO_DRIVE };   /* where a save's file goes (the save job, below) */
static int save_begin(int slot, int in_game, int to);
static int save_take(int may_finish);

int states_drive_save(int slot)
{
  if (!states_drive_known() || slot < 0 || slot >= STATE_SLOTS || dv_find(slot))
    return 0;   /* (never over a state there) */
  if (!save_begin(slot, 0, SJ_TO_DRIVE))
    return 0;
  return states_save_finish() == STATES_SAVE_DONE;
}

/* A state was written to the drive's slot: kept as the folder will have it. */
/* A drive's slot's time as its folder says it (FAT's, 2 s steps): the picture cache's key; the clock's when it
 * can't be read. */
static u32 drive_folder_time(int slot)
{
  char path[600];
  unsigned long when = 0;
  slot_path(dv.base, slot, path, sizeof(path));
  if (nspire_drive_stat && nspire_drive_stat(path, NULL, &when) && when)
    return (u32)when;
  return (u32)time(NULL) & ~1u;
}

static void drive_view_saved(int slot, u32 size, u32 counter, const u16 *picture, u32 saved_at)
{
  struct drive_slot *e = dv_add(slot);
  if (e)
  {
    e->size = size;
    e->saved_at = saved_at;
    e->mtime = drive_folder_time(slot);
    picture_keep(base_hash(dv.base), slot, e->mtime, e->size, picture != NULL, picture);
  }
  else
    dv.known = 0;   /* (no memory for it: read again when next asked) */
  dv_counted(e, counter);
  listing_forget();
  nspire_game_drive_claim();
}

/* 1 when the file at path is exactly these bytes (read a piece at a time). */
static int file_is(const char *path, const u8 *data, size_t size)
{
  FILE *f = open_read(path);
  size_t at = 0;
  int same = 1;

  if (!f)
    return 0;
  while (same)
  {
    size_t n = fread(io_buffer, 1, CHUNK, f);
    if (!n)
      break;
    same = at + n <= size && !memcmp(io_buffer, data + at, n);
    at += n;
  }
  same = same && at == size && !ferror(f);
  fclose(f);
  return same;
}


/* r17: what the menus keep of the saves folder given up (the pictures, the
 * folder's names, other games' slots), for the saves' memory first
 * (states_keep_buffers); read again when next needed. The bytes freed. */
long states_drop_caches(void)
{
  long freed = 0;
  int i;
  for (i = 0; i < KEPT_PICTURES; i++)
    if (pictures[i].pixels)
    {
      free(pictures[i].pixels);
      pictures[i].pixels = NULL;
      pictures[i].kept = 0;
      freed += STATE_PIC_W * STATE_PIC_H * 2;
    }
  for (i = 0; i < KEPT_GAMES; i++)
    if (kept[i].slots && strcmp(kept[i].base, base_path))   /* (the open game's are what a start takes) */
    {
      freed += (long)sizeof(*kept[i].slots) * (kept[i].count ? kept[i].count : 1);
      free(kept[i].slots);
      memset(&kept[i], 0, sizeof(kept[i]));
    }
  for (i = 0; i < 2; i++)
    if (listings[i].names)
    {
      freed += (long)listings[i].room;
      free(listings[i].names);
      listings[i].names = NULL;
      listings[i].size = listings[i].room = 0;
      listings[i].ok = 0;
    }
  if (nspire_drive_trim)
  {
    /* ms17: the drive's write buffer, kept through a burst of writes (none waits in it: given back) */
    unsigned long before = drive_held();
    nspire_drive_trim();
    freed += (long)(before - drive_held());
  }
  return freed;
}

/* ---- The memory a save and a load use, kept (2026-11-18, final/save17) ----
 *
 * On r16 states would not save, with no telling whether memory or storage
 * was short. The CX II's 70-minute Emerald run: the state resumed at 116 s,
 * then every save failed ("out of space?"; the first, the minute's
 * autosave, 60 s after that resume), and a load at 1092 s said "That save
 * can't be loaded": the file opened, its memory didn't come. A save took
 * ~0.95 MB of fresh memory each time (the state's copy 416 KB, the file's
 * 455 KB, the packer's 78 KB); a load ~0.5 MB. On gpsp-host only a heap
 * short of memory gives that pair; on the calculator memory was short from
 * the game's start, and what took it the logs don't say.
 *
 * Now the three are taken once after each game's ROM is loaded
 * (states_keep_buffers; dropped before a ROM load, which may need the room:
 * a drive's ROM grows the ROM buffer to the whole game, then the loader
 * reserves saves before mapping pages, reducing its optional cache if
 * necessary; the frontend retries after loading) and kept: every
 * save and load uses them and needs no fresh memory. Unless that would
 * leave less than KEEP_MARGIN in one piece for everything else (Bluetooth,
 * the drive, USB sound, the OS): then nothing is kept and each save takes
 * its own as before, said loudly in the logs. The game's own save (its
 * battery memory, nspire_backup.c) borrows the same two buffers while no
 * state needs them (states_lend_buffers). A failure keeps its cause, a save's,
 * a load's and a copy's apart (states_failure); the link log has each. And
 * the memory free is looked at (states_memory_look: the largest block, and
 * all of it in pieces of 64 KB or more) at each save and load, and at the
 * menus at most every 10 s: gpsp_states_time.txt.tns lists the last ones. */
#define FILE_ROOM (sizeof(struct state_header) + compressBound(GBA_STATE_MEM_SIZE) + PICTURE_MAX)
/* What has to stay free in one piece after the take, from what the rest
 * takes later in a game (r17, measured from their code): Bluetooth's
 * firmware upload at its peak 116 KB (41.5 KB held with a dongle), the
 * drive's write buffer 64 KB and an open file's cluster map (~32 KB for a
 * 32 MB ROM), the game list read again (629 names ~50 KB, the saves folder's
 * names ~120 KB for 1,878 states: a test library on gpsp-host), the
 * menus' picture cache (up to 324 KB, but each picture is skipped when
 * there's no room, and all of it given up for this take). USB sound's
 * (~200 KB) is taken at gpSP's start (init_input), before.
 * ms17: the drive's driver takes nothing at gpSP's start now: its read cache
 * (256 KB) and transfer memory (64 KB) when a drive is opened, given back
 * when it goes, its write buffer (64 KB) through a burst of writes, a file's
 * handle (~4.6 KB) while it is open (nspire-usbms ms17). While none is open,
 * a drive may still be plugged in, opened and saved to after the take: then
 * the margin is KEEP_MARGIN and those 389 KB too (nspire_drive_open_room; 0
 * with the drive open, whose cache and transfer memory are held already, or
 * with no driver): 901 KB, so that the rest still finds 512 KB once the
 * drive has what its first save needs. (r17 had the drive's 512 KB taken
 * before the take and asked 512 KB on top: 1,024 KB in all.) */
#define KEEP_MARGIN (512L * 1024)
static struct
{
  u8 *raw, *file;               /* GBA_STATE_MEM_SIZE, FILE_ROOM */
  void *work;                   /* fastdeflate_work_size(): the packer's, and a load's unpacking */
  size_t work_size;
  int raw_busy, file_busy, lent;
  long refused_at;              /* not kept: the largest block it would have left (-1: it didn't all come) */
  long dropped;                 /* the last take: the menus' caches given up first (bytes) */
  long margin;                  /* the last take's: KEEP_MARGIN, and room for a drive's opening (ms17) */
} kb;

#ifdef NSPIRE_ALLOC_HOOK
int nspire_states_fail_alloc;   /* tests: a save's or load's own malloc "fails" (the kept buffers don't) */
#define own_malloc(n) (nspire_states_fail_alloc ? NULL : malloc(n))
#else
#define own_malloc(n) malloc(n)
#endif

static struct states_failure failures[3];
static int failing;               /* whose failures[] failed() fills: STATES_SAVING, _LOADING, _COPYING */
#define failure (failures[failing])
/* (the most largest_block looks for: r17 N1, a probe holds one block at a time and never more than
 * this, so whatever else allocates meanwhile (the OS, USB, Bluetooth, the link) still finds the rest) */
#define LARGEST_MOST (1L << 20)

/* The largest block malloc gives now, up to most bytes (4 KB steps): one
 * malloc and its free at a time. */
static long largest_block(long most)
{
  long low = 0, high = most;
  while (high - low > 4096)
  {
    long mid = low + (high - low) / 2;
    void *p = malloc((size_t)mid);
    __asm__ volatile("" : : "r"(p) : "memory");   /* (else GCC drops the malloc and its free: never NULL) */
    if (p)
      low = mid;
    else
      high = mid;
    free(p);
  }
  return low;
}

#define MEMORY_LOOKS 16
static struct memory_look
{
  char what[28];
  u32 at;                        /* ticks */
  long largest;                  /* bytes (LARGEST_MOST: that or more) */
  unsigned long drive;           /* ms17: what the USB drive's driver held then (nspire_drive_mem_held) */
} memory_looks[MEMORY_LOOKS];
static int memory_look_count;
static u32 memory_look_last;

void states_memory_look(const char *what, int now)
{
  struct memory_look *m;
  u32 t = nspire_ticks();
  if (!now && memory_look_count && t - memory_look_last < 10u * NSPIRE_TICK_HZ)
    return;
  memory_look_last = t;
  m = &memory_looks[memory_look_count++ % MEMORY_LOOKS];
  snprintf(m->what, sizeof(m->what), "%s", what);
  m->at = t;
  m->largest = largest_block(LARGEST_MOST);
  m->drive = drive_held();
}

/* (the start's line for gpsp_states_time.txt.tns, timing_flush) */
static void memory_looks_write(FILE *f)
{
  int i, first = memory_look_count > MEMORY_LOOKS ? memory_look_count - MEMORY_LOOKS : 0;
  if (!memory_look_count)
    return;
  fprintf(f, "Memory free, KB (the last %d looks: the largest block, looked for up to %ld; what the USB drive's "
          "driver held)\n", MEMORY_LOOKS,
          LARGEST_MOST / 1024);
  for (i = first; i < memory_look_count; i++)
  {
    const struct memory_look *m = &memory_looks[i % MEMORY_LOOKS];
    fprintf(f, "%s at %u s: largest %ld%s; USB drive's driver %lu\n", m->what, (unsigned)(m->at / NSPIRE_TICK_HZ),
            m->largest / 1024, m->largest >= LARGEST_MOST - 4096 ? " or more" : "", m->drive / 1024);
  }
  memory_look_count = 0;
}

static void kept_free(void)
{
  fastdeflate_give_work(NULL, 0);
  free(kb.raw);
  free(kb.file);
  free(kb.work);
  kb.raw = kb.file = NULL;
  kb.work = NULL;
  kb.work_size = 0;
  kb.raw_busy = kb.file_busy = kb.lent = 0;
}

static long keep_try(void)
{
  if (!kb.raw)
    kb.raw = (u8 *)malloc(GBA_STATE_MEM_SIZE);
  if (!kb.file)
    kb.file = (u8 *)malloc(FILE_ROOM);
  if (!kb.work)
  {
    kb.work_size = fastdeflate_work_size();
    kb.work = malloc(kb.work_size);
  }
  return kb.raw && kb.file && kb.work ? largest_block(LARGEST_MOST) : -1;
}

int states_keep_buffers(void)
{
  long left, freed = 0;
  if (kb.raw && kb.file && kb.work)
    return 1;
  left = keep_try();
  kb.refused_at = 0;
  kb.dropped = 0;
  kb.margin = KEEP_MARGIN + (long)drive_open_room();
  if (left < kb.margin)
  {
    /* r17: saves first: what the menus keep (pictures, the saves folder's names, other games' slots) given
     * up, and tried again */
    kept_free();
    freed = states_drop_caches();
    left = keep_try();
    kb.dropped = freed;
  }
  if (left < kb.margin)
  {
    /* not all of it, or too little left in one piece for the rest: nothing kept */
    kb.refused_at = left;
    kept_free();
    return 0;
  }
  fastdeflate_give_work(kb.work, kb.work_size);
  return 1;
}

void states_drop_buffers(void)
{
  settle();
  if (kb.lent && nspire_backup_finish)
    nspire_backup_finish();   /* (the game's save has them) */
  kept_free();
}

/* save17:the game's own save (nspire_backup.c) borrows the two big
 * buffers while no state has them: 1, and *a (a_size bytes), *b. */
int states_lend_buffers(unsigned char **a, unsigned long *a_size, unsigned char **b, unsigned long *b_size)
{
  if (!kb.raw || !kb.file || kb.raw_busy || kb.file_busy)
    return 0;
  kb.raw_busy = kb.file_busy = kb.lent = 1;
  *a = kb.raw;
  *a_size = GBA_STATE_MEM_SIZE;
  *b = kb.file;
  *b_size = FILE_ROOM;
  return 1;
}

void states_return_buffers(void)
{
  if (kb.lent)
    kb.raw_busy = kb.file_busy = kb.lent = 0;
}

/* A buffer of the kept ones (*kept 1), or one of its own of size bytes
 * (malloc'd, *kept 0); NULL: no memory for it. Lent to the game's save and
 * no memory for its own: that save is finished first, and they are back. */
static u8 *take_buffer_as(int file, size_t size, int *kept, int may_finish)
{
  u8 **mine = file ? &kb.file : &kb.raw;
  int *busy = file ? &kb.file_busy : &kb.raw_busy;
  u8 *p;
  *kept = *mine && !*busy;
  if (*kept)
  {
    *busy = 1;
    return *mine;
  }
  p = (u8 *)own_malloc(size);
  if (!p && may_finish && *mine && kb.lent && nspire_backup_finish)
  {
    nspire_backup_finish();
    if (!*busy)
    {
      *busy = *kept = 1;
      return *mine;
    }
  }
  return p;
}

/* (outside a game's frame: the game's save that has them may be finished at once) */
static u8 *take_buffer(int file, size_t size, int *kept)
{
  return take_buffer_as(file, size, kept, 1);
}

static void give_back(u8 *p, int kept)
{
  if (!p)
    return;
  if (!kept)
    free(p);
  else if (p == kb.file)
    kb.file_busy = 0;
  else if (p == kb.raw)
    kb.raw_busy = 0;
}

/* zlib's memory for a load's unpacking: from the packer's kept work area
 * (free then: a save under way is finished first), else its own. */
static size_t arena_used;
static voidpf arena_alloc(voidpf opaque, uInt items, uInt size)
{
  size_t n = ((size_t)items * size + 7) & ~(size_t)7;
  (void)opaque;
  if (n > kb.work_size - arena_used)
    return Z_NULL;
  arena_used += n;
  return (u8 *)kb.work + arena_used - n;
}

static void arena_free(voidpf opaque, voidpf address)
{
  (void)opaque;
  (void)address;
}

#ifdef NSPIRE_ALLOC_HOOK
static voidpf hook_alloc(voidpf opaque, uInt items, uInt size)
{
  (void)opaque;
  return own_malloc((size_t)items * size);
}

static void hook_free(voidpf opaque, voidpf address)
{
  (void)opaque;
  free(address);
}
#endif

/* uncompress, with the kept work area as zlib's memory when there is one */
static int unpack_state(u8 *raw, uLongf *raw_size, const u8 *packed, u32 packed_size)
{
  z_stream z;
  int status;

  memset(&z, 0, sizeof(z));
  if (kb.work)
  {
    arena_used = 0;
    z.zalloc = arena_alloc;
    z.zfree = arena_free;
  }
#ifdef NSPIRE_ALLOC_HOOK
  else
  {
    z.zalloc = hook_alloc;
    z.zfree = hook_free;
  }
#endif
  z.next_in = (Bytef *)packed;
  z.avail_in = packed_size;
  z.next_out = raw;
  z.avail_out = (uInt)*raw_size;
  status = inflateInit(&z);
  if (status != Z_OK)
    return status;
  status = inflate(&z, Z_FINISH);
  *raw_size = z.total_out;
  inflateEnd(&z);
  return status == Z_STREAM_END ? Z_OK : status == Z_NEED_DICT || (status == Z_BUF_ERROR && !z.avail_in) ?
         Z_DATA_ERROR : status;
}

static void failed(int why, const char *step, int err, unsigned long need, long free_bytes)
{
  failure.why = why;
  failure.step = step;
  failure.err = err;
  failure.need = need;
  failure.free = free_bytes;
}

void states_failure_memory(struct states_failure *f, const char *step, unsigned long need)
{
  f->why = STATES_WHY_MEMORY;
  f->step = step;
  f->err = 0;
  f->need = need;
  f->free = largest_block(LARGEST_MOST);
}

static void failed_memory(const char *step, unsigned long need)
{
  states_failure_memory(&failure, step, need);
}

/* The storage free where path's file goes, measured outside a game's frame
 * only (save17 N3): zeros written to it unbuffered, 4 KB at a
 * time, up to most bytes, each counted only as the OS took it; then removed.
 * -1: it can't be made. */
static long storage_free(const char *path, long most)
{
  long written = 0;
  FILE *f = fopen(path, "wb");
  if (!f)
    return -1;
  setvbuf(f, NULL, _IONBF, 0);
  memset(io_buffer, 0, sizeof(io_buffer));
  while (written < most)
  {
    size_t n = fwrite(io_buffer, 1, 4096, f);
    written += (long)n;
    if (n != 4096)
      break;
  }
  fclose(f);
  remove(path);
  return written;
}

/* A file call failed (step; err: its errno, 0 when the OS set none; wrote:
 * the bytes the OS took of need before it refused, -1 not known; temp: the
 * file it was making, gone now). Full storage only when the OS said ENOSPC,
 * or (outside a game's frame) the space measured is under the file's size;
 * a short write with no errno in a game's frame is "file write failed"
 * (Ndless often sets none: an I/O error is not full storage). */
void states_failure_file(struct states_failure *f, const char *step, int err, long wrote, unsigned long need,
                         const char *temp, int in_game)
{
  long room = -1;
  f->step = step;
  f->err = err;
  f->need = need;
  if (err == ENOSPC)
  {
    f->why = STATES_WHY_FULL;
    f->free = wrote >= 0 ? wrote : -1;
    return;
  }
  if (!in_game && temp && err != EMFILE && err != ENFILE)
    room = storage_free(temp, (long)need + 4096);
  f->why = room >= 0 && room < (long)need ? STATES_WHY_FULL : STATES_WHY_FILE;
  f->free = f->why == STATES_WHY_FULL ? room : wrote;
}

static void failed_file(const char *step, int err, long wrote, unsigned long need, const char *temp, int in_game)
{
  states_failure_file(&failure, step, err, wrote, need, temp, in_game);
}

const struct states_failure *states_failure(int what)
{
  return &failures[what < 0 || what > 2 ? 0 : what];
}

const char *states_failure_words(int what)
{
  return states_failure_text(states_failure(what));
}

const char *states_failure_text(const struct states_failure *f)
{
  static char words[80];
  switch (f->why)
  {
  case STATES_WHY_MEMORY:
    return "not enough memory";
  case STATES_WHY_FULL:
    if (f->free < 0)
      return "storage full";
    snprintf(words, sizeof(words), "storage full, %ld KB free", f->free / 1024);
    return words;
  case STATES_WHY_FILE:
    snprintf(words, sizeof(words), "file %s failed", f->step ? f->step : "write");
    return words;
  case STATES_WHY_DAMAGED:
    return "it's damaged";
  case STATES_WHY_FOREIGN:
    return "not a state it can read";
  case STATES_WHY_DRIVE:
    return "USB drive full or gone";
  case STATES_WHY_OTHER:
    snprintf(words, sizeof(words), "%s failed", f->step ? f->step : "it");
    return words;
  default:
    return "it failed";
  }
}

/* The failure into the link log (the next hardware run's proof). */
static void failure_log(const char *what, int slot)
{
  static const char *const why[] = { "none", "memory", "storage full", "file call", "damaged", "USB drive", "other",
                                     "not a state it reads" };
  if (!nspire_link_log)
    return;
  nspire_link_log("states: %s slot %d failed: %s, at %s; errno %d; needs %lu bytes; %s %ld; kept buffers: %s",
                  what, slot + 1, why[failure.why % 8], failure.step ? failure.step : "-", failure.err, failure.need,
                  failure.why == STATES_WHY_MEMORY ? "the largest free block" :
                  failure.why == STATES_WHY_FULL ? "storage free" : "bytes the OS took",
                  failure.free, kb.raw ? (kb.lent ? "lent to the game's save" : "yes") : "no");
}

void states_log_buffers(void)
{
  if (kb.raw && kb.file && kb.work)
  {
    /* (in gpsp_states_time.txt.tns: a quiet run writes no link log) */
    states_memory_look(kb.dropped ? "kept, caches dropped; after" : "saves' memory kept, after", 1);
    if (kb.dropped && nspire_link_log)
      nspire_link_log("states: memory for saves and loads kept after the menus' caches were given up (%ld bytes)",
                      kb.dropped);
    return;
  }
  states_memory_look("saves' memory NOT kept", 1);
  if (nspire_link_log)
  {
    char why[80];
    if (kb.refused_at >= 0)
      snprintf(why, sizeof(why), "it would have left less than %ld KB in one piece for the rest", kb.margin / 1024);
    else
      snprintf(why, sizeof(why), "there wasn't that much free");
    nspire_link_log("states: memory for saves and loads NOT KEPT: %s (the menus' caches given up first: %ld "
                    "bytes); each save and load takes its own, as r16 did", why, kb.dropped);
  }
  if (nspire_link_log)
    nspire_link_log("states: the margin was %ld KB (%s); the USB drive's driver holds %lu bytes", kb.margin / 1024,
                    kb.margin > KEEP_MARGIN ? "with room for a USB drive's opening" : "no drive to open", drive_held());
}

/* save17:a slot's whole file into the kept file buffer (*kept 1) or
 * its own (room bytes at least); NULL with the cause kept (memory told apart
 * from the drive). */
static u8 *file_get_kept(const char *path, size_t room, size_t *size, int *kept)
{
  unsigned long n = 0, got = 0;
  long len = 0;
  FILE *f;
  u8 *data;

  *size = 0;
  *kept = 0;
  if (on_drive(path))
  {
    if (!nspire_drive_stat || !nspire_drive_stat(path, &n, NULL))
    {
      failed(STATES_WHY_DRIVE, "read", ENOENT, 0, -1);
      return NULL;
    }
  }
  else
  {
    errno = 0;
    if (!(f = open_read(path)))
    {
      failed(STATES_WHY_FILE, "open", errno, 0, -1);
      return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0)
    {
      fclose(f);
      failed(STATES_WHY_FILE, "read", errno, 0, -1);
      return NULL;
    }
    fclose(f);
    n = (unsigned long)len;
  }
  if (n > FILE_ROOM)
  {
    failed(STATES_WHY_DAMAGED, "its size", 0, n, -1);
    return NULL;
  }
  if (!(data = take_buffer(1, n > room ? n : room ? room : 1, kept)))
  {
    failed_memory("the file's buffer", n);
    return NULL;
  }
  errno = 0;
  if (on_drive(path))
    got = nspire_drive_get_into && nspire_drive_get_into(path, data, n, &got) ? got : (unsigned long)-1;
  else if ((f = open_read(path)) != NULL)
  {
    got = (unsigned long)fread(data, 1, n, f);
    fclose(f);
  }
  else
    got = (unsigned long)-1;
  if (got != n)
  {
    failed(on_drive(path) ? STATES_WHY_DRIVE : STATES_WHY_FILE, "read", errno, n, -1);
    give_back(data, *kept);
    return NULL;
  }
  *size = n;
  return data;
}

/* file_put, its failure's cause kept: the drive's, or the calculator's step */
static int file_put_why(const char *path, const void *data, size_t size)
{
  char tmp[660];
  FILE *f;
  const char *step = NULL;
  long wrote = -1;
  int err;

  if (on_drive(path))
  {
    if (nspire_drive_put(path, data, (unsigned long)size))
      return 1;
    failed(STATES_WHY_DRIVE, "write", 0, size, -1);
    return 0;
  }
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  errno = 0;
  if (!(f = fopen(tmp, "wb")))
    step = "create";
  else
  {
    setvbuf(f, NULL, _IONBF, 0);
    if ((size_t)(wrote = (long)fwrite(data, 1, size, f)) != size)
      step = "write";
  }
  err = errno;
  if (f && fclose(f) != 0 && !step)
  {
    step = "close";
    err = errno;
    wrote = -1;
  }
  if (!step)
  {
    errno = 0;
    if (nspire_file_replace(tmp, path))
      return 1;
    remove(tmp);
    failed(STATES_WHY_FILE, "rename", errno, size, -1);
    return 0;
  }
  remove(tmp);
  failed_file(step, err, wrote, size, tmp, 0);
  return 0;
}

int states_place_copy(int to_drive, int slot, int *to_slot)
{
  static u16 kept_pic[STATE_PIC_W * STATE_PIC_H];
  char from[640], to[640];
  size_t size = 0;
  struct state_header header;
  u8 *data;
  int target, i, ok, has_header, kept = 0;

  settle();
  failing = STATES_COPYING;
  memset(&failure, 0, sizeof(failure));
  *to_slot = -1;
  if (!states_drive_known() || slot < 0 || slot >= STATE_SLOTS)
    return STATES_COPY_FAILED;
  if (to_drive ? !states_exists(slot) : !dv_find(slot))
    return STATES_COPY_FAILED;
  slot_path(to_drive ? base_path : dv.base, slot, from, sizeof(from));
  /* (save17:read into the kept file buffer, the cause kept) */
  data = file_get_kept(from, 0, &size, &kept);
  if (!data)
    return STATES_COPY_FAILED;
  memset(&header, 0, sizeof(header));
  has_header = size >= sizeof(header) && !memcmp(data, STATE_MAGIC, 4);
  if (has_header)
    memcpy(&header, data, sizeof(header));
  /* the same state there already (the same size, then the same bytes): that slot, nothing written */
  if (to_drive)
  {
    for (i = 0; i < dv.count; i++)
      if (dv.slots[i].size == size)
      {
        slot_path(dv.base, dv.slots[i].slot, to, sizeof(to));
        if (file_is(to, data, size))
        {
          *to_slot = dv.slots[i].slot;
          give_back(data, kept);
          return STATES_COPY_SAME;
        }
      }
  }
  else
  {
    for (i = 0; i < STATE_SLOTS; i++)
      if (used[i] && (kinds[i] & KIND_FOUND) && sizes[i] == size && counters[i] == (has_header ? header.counter : 0))
      {
        slot_path(base_path, i, to, sizeof(to));
        if (file_is(to, data, size))
        {
          *to_slot = i;
          give_back(data, kept);
          return STATES_COPY_SAME;
        }
      }
  }
  target = to_drive ? states_drive_next() : states_next_new_slot();
  if (target < 0)
  {
    give_back(data, kept);
    return STATES_COPY_FAILED;
  }
  slot_path(to_drive ? dv.base : base_path, target, to, sizeof(to));
  ok = file_put_why(to, data, size);
  give_back(data, kept);
  if (!ok)
    return STATES_COPY_FAILED;
  if (to_drive)
  {
    /* its picture as the calculator's slot has it kept (else read when the strip asks) */
    int has = picture_of(base_path, slot, counters[slot], sizes[slot], kept_pic, 0);
    struct drive_slot *e = dv_add(target);
    if (e)
    {
      e->size = (u32)size;
      e->mtime = drive_folder_time(target);
      e->saved_at = has_header && saved_time_ok(header.reserved[1]) ? header.reserved[1] : 0;
      if (has >= 0)
        picture_keep(base_hash(dv.base), target, e->mtime, e->size, has, kept_pic);
    }
    else
      dv.known = 0;
    dv_counted(e, has_header ? header.counter : 0);
    nspire_game_drive_claim();
  }
  else
  {
    used[target] = 1;
    read_slot_info(target);
    kept_put_open();
  }
  listing_forget();
  *to_slot = target;
  return STATES_COPY_DONE;
}

/* save17 (for final/ui17's drive tab): a drive's state packed or stored
 * (compress 1 or 0), its age kept (the header's save time; an older state's:
 * its file's). Its file read into the kept file buffer, the state into the
 * kept state buffer, the new file made in the file buffer and put whole
 * (nspire_drive_put: under another name, synced, renamed). 1 done (or it was
 * that way already); 0 with the cause kept (states_failure(STATES_SAVING)). */
int states_drive_set_compressed(int slot, int compress)
{
  static u8 picture[PICTURE_MAX];
  char path[600];
  struct state_header header;
  struct drive_slot *e;
  u8 *file, *raw;
  size_t size = 0, at;
  u32 block_size = 0;
  int file_kept = 0, raw_kept = 0, ok = 0, had_header;

  settle();
  failing = STATES_SAVING;
  memset(&failure, 0, sizeof(failure));
  if (!states_drive_known() || slot < 0 || slot >= STATE_SLOTS || !(e = dv_find(slot)))
  {
    failed(STATES_WHY_DRIVE, "read", ENOENT, 0, -1);
    return 0;
  }
  slot_path(dv.base, slot, path, sizeof(path));
  if (!(file = file_get_kept(path, FILE_ROOM, &size, &file_kept)))   /* (the new file is made in it) */
    return 0;
  memset(&header, 0, sizeof(header));
  had_header = size >= sizeof(header) && !memcmp(file, STATE_MAGIC, 4);
  if (had_header)
  {
    memcpy(&header, file, sizeof(header));
    if (!(header.flags & STATE_FLAG_STORED) == !!compress)
    {
      give_back(file, file_kept);
      return 1;   /* already that way */
    }
    if (header.version != STATE_VERSION || header.raw_size != GBA_STATE_MEM_SIZE)
      failed(STATES_WHY_FOREIGN, "its header", 0, size, -1);
    else if (!header_whole(&header, (long)size))
      failed(STATES_WHY_DAMAGED, "its header", 0, size, -1);
  }
  else if (size != GBA_STATE_MEM_SIZE)
    failed(STATES_WHY_FOREIGN, "its header", 0, size, -1);
  else if (!compress)
  {
    give_back(file, file_kept);
    return 1;   /* an older build's state is stored as it is already */
  }
  if (failure.why || !(raw = take_buffer(0, GBA_STATE_MEM_SIZE, &raw_kept)))
  {
    if (!failure.why)
      failed_memory("the state's room", GBA_STATE_MEM_SIZE);
    give_back(file, file_kept);
    return 0;
  }
  /* the state and its picture out of the file */
  if (!had_header)
    ok = (memcpy(raw, file, GBA_STATE_MEM_SIZE), 1);
  else if (header.flags & STATE_FLAG_STORED)
    ok = header.packed_size == GBA_STATE_MEM_SIZE && (memcpy(raw, file + sizeof(header), GBA_STATE_MEM_SIZE), 1);
  else
  {
    uLongf n = GBA_STATE_MEM_SIZE;
    int z = unpack_state(raw, &n, file + sizeof(header), header.packed_size);
    ok = z == Z_OK && n == GBA_STATE_MEM_SIZE;
    if (z == Z_MEM_ERROR)
      failed_memory("the unpacking", 48 * 1024);
  }
  if (!ok && !failure.why)
    failed(STATES_WHY_DAMAGED, "its unpacking", 0, size, -1);
  if (ok && had_header && (header.flags & STATE_FLAG_PICTURE))
  {
    block_size = header.reserved[0];
    if (block_size > PICTURE_MAX || sizeof(header) + header.packed_size + block_size > size)
      block_size = 0;
    else
      memcpy(picture, file + sizeof(header) + header.packed_size, block_size);
  }
  if (ok)
  {
    if (!had_header)
    {
      memcpy(header.magic, STATE_MAGIC, 4);
      header.version = STATE_VERSION;
      header.raw_size = GBA_STATE_MEM_SIZE;
      header.counter = e->counter;
      header.flags = e->counter ? STATE_FLAG_COUNTED : 0;
    }
    if (!block_size)
    {
      header.flags &= ~STATE_FLAG_PICTURE;
      header.reserved[0] = 0;
    }
    if (!saved_time_ok(header.reserved[1]))   /* (its age kept: an older state's, its file's time) */
      header.reserved[1] = e->saved_at ? e->saved_at : saved_time_ok(e->mtime) ? e->mtime : 0;
    header.flags = compress ? header.flags & ~STATE_FLAG_STORED : header.flags | STATE_FLAG_STORED;
    /* the new file in the file buffer (its old contents are in raw and picture now) */
    at = sizeof(header);
    if (compress)
    {
      header.packed_size = (u32)fastdeflate(file + at, FILE_ROOM - at - PICTURE_MAX, raw, GBA_STATE_MEM_SIZE);
      ok = header.packed_size != 0;
      if (!ok)
        failed(STATES_WHY_OTHER, "packing", 0, GBA_STATE_MEM_SIZE, -1);
    }
    else
    {
      memcpy(file + at, raw, GBA_STATE_MEM_SIZE);
      header.packed_size = GBA_STATE_MEM_SIZE;
    }
    at += header.packed_size;
    if (ok && block_size)
    {
      memcpy(file + at, picture, block_size);
      at += block_size;
    }
    memcpy(file, &header, sizeof(header));
    ok = ok && file_put_why(path, file, at);
  }
  give_back(raw, raw_kept);
  give_back(file, file_kept);
  if (ok)
  {
    e->size = (u32)at;
    e->saved_at = header.reserved[1];
    e->mtime = drive_folder_time(slot);   /* (the picture cache's key, as the next folder read has it) */
    listing_forget();
  }
  return ok;
}

/* ---- A state saved over the game's frames (2026-11-17, final/pace6) ----
 *
 * The game used to freeze for a split second when saving. The CX
 * II's gpsp_states_time.txt.tns: a save stopped the game 133-311 ms (the
 * copy 12.7 ms, packing 30-65, the picture 15-26, the file's write 28-205,
 * its replacing 36-71), the sound paused for it. Now states_save_begin
 * takes the state and the picture (the copy and the picture's scaling, one
 * frame's time) and states_save_step does the rest a part at a time, one
 * part each frame's end (nspire_host_main.c): the packing a deflate block at
 * a time (fastdeflate_begin, the same bytes), then the picture's packing,
 * then the OS's write, then its replace, each OS call with the sound's
 * queue served through it (nspire_audio_os_call_begin). What is left is the
 * OS's calls: one frame each, the longest (an outlier write) ~200 ms.
 * states_save (the menu, the quit) runs the same parts at once; any other
 * states_ call finishes a save under way first. */
enum { SJ_IDLE, SJ_PACK, SJ_PICTURE, SJ_WRITE, SJ_REPLACE, SJ_WAIT, SJ_TAKE, SJ_CREATE, SJ_CLOSE };
/* (r19-snd: in a game's frame the take waits for the ring to cover it (SJ_TAKE: the frame's end
 * nspire_audio_save_frame lets it, a frame or a few after the key; the interrupts on in it), and the
 * file's write is an OS call per part: its create, SJ_WRITE_BYTES at a time, its close, the replace's
 * steps (nspire_file_replace_step): a hardware log's 229.7 ms write was one call) */
#define SJ_WRITE_BYTES 8192
/* (sj.to: SJ_TO_CALC the calculator's slot, as always; SJ_TO_DRIVE the drive's: states_drive_save, a
 * safe point, written there before the save returns) */
static struct
{
  int step, slot, ok, has_picture, in_game, to, tag;
  u8 *raw, *file;
  int raw_kept, file_kept;      /* (save17: the kept buffers, given back, not freed) */
  unsigned waited;              /* (SJ_WAIT: the frames it waited for the game's save) */
  u32 block_size, size;
  struct state_header header;
  struct fastdeflate_job pack;
  struct timing *t;
  FILE *f;                      /* (r19-snd: the file being written, SJ_CREATE to SJ_CLOSE) */
  u32 written;
  struct nspire_replace rp;
  char path[600], temp_path[620];
} sj;
static u16 pic[STATE_PIC_W * STATE_PIC_H];
static u8 block[PICTURE_MAX];
/* (r19-fix 5) A save asked for in a game while one is under way (the key twice, the
 * key during an autosave, an autosave during the key's) waits for it, in its order: begun at a frame's
 * end once the one before is written (its state taken then), not that one finished at once with the
 * interrupts masked. A slot asked for again before its save has taken its state is that save. Whatever
 * finishes saves (the menu, a load, the quit) writes the queue too, now. Their results in their order. */
#define SJ_QUEUE 4
static struct { int slot, tag; } sj_queue[SJ_QUEUE];
static int sj_queued;
#define SJ_RESULTS (SJ_QUEUE + 2)
static struct { int r, slot, tag; } sj_results[SJ_RESULTS];
static int sj_result_n, sj_result_tag;
static int save_start(int slot, int in_game, int to, int tag);

/* (weak: builds without nspire_audio.c, tests/arm's libbench and the PC's
 * unit builds, have none: the calls as they were) */
int nspire_audio_os_call_begin(uint32_t *cpsr) __attribute__((weak));
void nspire_audio_os_call_end(int began, uint32_t cpsr) __attribute__((weak));
int nspire_audio_serve_begin(uint32_t *cpsr) __attribute__((weak));
void nspire_audio_pause(void) __attribute__((weak));
uint32_t nspire_audio_save_bank_ms(void) __attribute__((weak));
extern u32 nspire_save_call_ms __attribute__((weak));
void nspire_audio_serve_end(int began, uint32_t cpsr) __attribute__((weak));

/* (r19-snd) a whole stretch served (states_serve_os: the menu's save, a load key's load) */
static uint32_t serve_begin(int *began)
{
  uint32_t cpsr = 0;
  *began = states_serve_os && nspire_audio_serve_begin && nspire_audio_serve_begin(&cpsr);
  return cpsr;
}

static void serve_end(int began, uint32_t cpsr)
{
  if (began)
    nspire_audio_serve_end(began, cpsr);
}

/* An OS file call of the save: in a game (states_save_step's in_game) the
 * sound's queue served through it; elsewhere (the quit) as before. r19-snd:
 * the menu and the load keys too (states_serve_os, nspire_host_main.c): the
 * sound paused then, the device kept in packets of silence, no wire gap. */
int states_serve_os;
static uint32_t file_call_begin(int in_game, int *began)
{
  uint32_t cpsr = 0;
  *began = (in_game || states_serve_os) && nspire_audio_os_call_begin && nspire_audio_os_call_begin(&cpsr);
  return cpsr;
}

static void file_call_end(int began, uint32_t cpsr)
{
  if (began)
    nspire_audio_os_call_end(began, cpsr);
}

static void settle(void)
{
  /* (r19: the saves asked for behind the one under way too. Between a save's last part and the take
   * of the one queued behind it sj.step is 0: a load key there loaded its state first, and the queued save then
   * took the game just loaded, into its slot) */
  if (sj.step || sj_queued)
    states_save_finish();
}

int states_save_busy(void)
{
  return sj.step != SJ_IDLE || sj_queued;
}

/* in_game: its result is kept for states_save_result (the frames' message);
 * to: SJ_TO_CALC, or SJ_TO_DRIVE (never from a game's frame). */
static int save_begin_as(int slot, int in_game, int to, int tag)
{
  int i;
  if (sj.step || sj_queued)
  {
    if (in_game && to == SJ_TO_CALC)
    {
      /* (r19-fix) in a game: after the one under way (and the ones asked for before) */
      if (slot < 0 || slot >= STATE_SLOTS)
        return 0;
      if (sj.step && sj.to == SJ_TO_CALC && sj.slot == slot && (sj.step == SJ_TAKE || sj.step == SJ_WAIT))
      {
        sj.tag = tag;   /* (the later request's word: the Save key after an autosave says "Saved") */
        return 1;   /* (its state not taken yet: it takes the game then) */
      }
      for (i = 0; i < sj_queued; i++)
        if (sj_queue[i].slot == slot)
        {
          sj_queue[i].tag = tag;
          return 1;
        }
      if (sj_queued == SJ_QUEUE)
        sj_queued--;   /* (more than it holds: the newest in the last one's place) */
      sj_queue[sj_queued].slot = slot;
      sj_queue[sj_queued++].tag = tag;
      return 1;
    }
    states_save_finish();
  }
  return save_start(slot, in_game, to, tag);
}

static int save_begin(int slot, int in_game, int to)
{
  return save_begin_as(slot, in_game, to, 0);
}

/* (r19-fix) the next save asked for: begun (its result kept, as a game's) */
static int save_start_next(void)
{
  int slot = sj_queue[0].slot, tag = sj_queue[0].tag;
  sj_queued--;
  memmove(sj_queue, sj_queue + 1, sizeof(sj_queue[0]) * (size_t)sj_queued);
  return save_start(slot, 1, SJ_TO_CALC, tag);
}

static int save_start(int slot, int in_game, int to, int tag)
{
  if (slot < 0 || slot >= STATE_SLOTS)
    return 0;
  sj.tag = tag;
  sj.in_game = in_game;
  sj.to = to;
  memset(&sj.header, 0, sizeof(sj.header));
  sj.slot = slot;
  sj.ok = 1;
  sj.block_size = sj.size = 0;
  /* No dynarec flush: the state doesn't hold its translations (RAM's code
   * tags aren't in it), and loading one flushes them. */
  {
    /* (save17:the memory free at each save; in a game's frame only its largest block) */
    char what[28];
    snprintf(what, sizeof(what), "save slot %d", slot + 1);
    states_memory_look(what, 1);
  }
  sj.t = timing_start('s', slot);
  failing = STATES_SAVING;
  memset(&failure, 0, sizeof(failure));
  /* A temporary file first (SAV-2: for a new slot too: a reset mid-write
   * left a partial state with the newest counter). Over an existing state
   * it can't destroy it. (A drive's slot: its driver does the same.) */
  slot_path(to == SJ_TO_DRIVE ? dv.base : base_path, slot, sj.path, sizeof(sj.path));
  snprintf(sj.temp_path, sizeof(sj.temp_path), "%s.tmp", sj.path);
  sj.f = NULL;
  sj.written = 0;
  if (in_game)
  {
    sj.step = SJ_TAKE;          /* (taken at a frame's end the ring covers: states_save_step) */
    return 1;
  }
  if (!save_take(!in_game))
  {
    if (in_game && kb.lent)
    {
      /* save17 N2: in a game's frame the kept buffers are the game's save's and there's no memory
       * of its own: that save goes on a part a frame (its OS calls served, nspire_backup_step) and the state
       * is taken once they are back (SJ_WAIT) */
      sj.step = SJ_WAIT;
      sj.waited = 0;
      return 1;
    }
    failure_log("save", slot);
    return 0;
  }
  return 1;
}

/* The state and its picture taken into the save's buffers: 1; 0 with the cause kept (no memory).
 * may_finish: outside a game's frame, a game's save that has the kept buffers is finished at once. */
static int save_take(int may_finish)
{
  sj.raw = take_buffer_as(0, GBA_STATE_MEM_SIZE, &sj.raw_kept, may_finish);
  sj.file = take_buffer_as(1, FILE_ROOM, &sj.file_kept, may_finish);
  if (!sj.raw || !sj.file)
  {
    failed_memory(!sj.raw ? "the state's copy" : "the file's buffer", !sj.raw ? GBA_STATE_MEM_SIZE : FILE_ROOM);
    give_back(sj.raw, sj.raw_kept);
    give_back(sj.file, sj.file_kept);
    sj.raw = sj.file = NULL;
    return 0;
  }
  /* zeroed first, so the state's unused room (its "zpadding" field) packs down to almost nothing */
  memset(sj.raw, 0, GBA_STATE_MEM_SIZE);
  gba_save_state(sj.raw);
  memcpy(sj.header.magic, STATE_MAGIC, 4);
  sj.header.version = STATE_VERSION;
  sj.header.raw_size = GBA_STATE_MEM_SIZE;
  sj.header.flags = STATE_FLAG_COUNTED | (nspire_compress_saves ? 0 : STATE_FLAG_STORED);
  if (sj.header.flags & STATE_FLAG_STORED)
  {
    memcpy(sj.file + sizeof(sj.header), sj.raw, GBA_STATE_MEM_SIZE);
    sj.header.packed_size = GBA_STATE_MEM_SIZE;
    give_back(sj.raw, sj.raw_kept);
    sj.raw = NULL;
    sj.step = SJ_PICTURE;
  }
  else if (fastdeflate_begin(&sj.pack, sj.file + sizeof(sj.header), compressBound(GBA_STATE_MEM_SIZE), sj.raw,
                             GBA_STATE_MEM_SIZE))
    sj.step = SJ_PACK;
  else
  {
    failed_memory("the packer", (unsigned long)fastdeflate_work_size());
    give_back(sj.raw, sj.raw_kept);
    give_back(sj.file, sj.file_kept);
    sj.raw = sj.file = NULL;
    return 0;
  }
  /* the game's picture now (packed later) */
  sj.has_picture = make_picture(pic);
  timing_part(sj.t, T_COPY);
  return 1;
}

int states_save_begin(int slot)
{
  return save_begin(slot, 1, SJ_TO_CALC);
}

int states_save_begin_as(int slot, int tag)
{
  return save_begin_as(slot, 1, SJ_TO_CALC, tag);
}

static int save_done(void)
{
  int slot = sj.slot, r = sj.ok ? STATES_SAVE_DONE : STATES_SAVE_FAILED;
  give_back(sj.raw, sj.raw_kept);
  give_back(sj.file, sj.file_kept);
  sj.raw = sj.file = NULL;
  if (!sj.ok)
    failure_log(sj.to == SJ_TO_DRIVE ? "save (USB drive)" : "save", slot);
  sj.t->bytes = (long)sj.size;
  if (sj.in_game)
  {
    /* (r19-bank: what the sound's bank was for it, and what the saves had learned) */
    sj.t->bank_ms = nspire_audio_save_bank_ms ? nspire_audio_save_bank_ms() : 0;
    sj.t->learned_ms = &nspire_save_call_ms ? nspire_save_call_ms : 0;
  }
  sj.step = SJ_IDLE;
  if (sj.in_game)
  {
    /* (for states_save_result, whoever finished it; r19-fix: in their order) */
    if (sj_result_n == SJ_RESULTS)
    {
      sj_result_n--;
      memmove(sj_results, sj_results + 1, sizeof(sj_results[0]) * (size_t)sj_result_n);
    }
    sj_results[sj_result_n].r = r;
    sj_results[sj_result_n].slot = slot;
    sj_results[sj_result_n++].tag = sj.tag;
  }
  if (!sj.ok)
    return r;
  if (sj.to == SJ_TO_DRIVE)
  {
    drive_view_saved(slot, sj.size, sj.header.counter, (sj.header.flags & STATE_FLAG_PICTURE) ? pic : NULL,
                     sj.header.reserved[1]);
    return r;
  }
  used[slot] = 1;
  counted[slot] = 1;
  counters[slot] = sj.header.counter;
  /* (final/menu6) its size, kind and picture kept for the menus */
  slot_saved(slot, (long)sj.size, &sj.header, pic);
  return r;
}

int states_save_result(int *slot)
{
  int r;
  if (!sj_result_n)
  {
    *slot = 0;
    return STATES_SAVE_IDLE;
  }
  r = sj_results[0].r;
  *slot = sj_results[0].slot;
  sj_result_tag = sj_results[0].tag;
  sj_result_n--;
  memmove(sj_results, sj_results + 1, sizeof(sj_results[0]) * (size_t)sj_result_n);
  return r;
}

int states_save_result_tag(void)
{
  return sj_result_tag;
}

int states_exists_soon(int slot)
{
  int i;
  if (slot < 0 || slot >= STATE_SLOTS)
    return 0;
  if (used[slot] || (sj.step && sj.to == SJ_TO_CALC && sj.slot == slot))
    return 1;
  for (i = 0; i < sj_queued; i++)
    if (sj_queue[i].slot == slot)
      return 1;
  return 0;
}

int states_save_next_is_file(void)
{
  return sj.step == SJ_CREATE || sj.step == SJ_WRITE || sj.step == SJ_CLOSE || sj.step == SJ_REPLACE ||
         (sj.step == SJ_WAIT && nspire_backup_next_is_file && nspire_backup_next_is_file());
}

int states_save_next_is_take(void)
{
  return sj.step == SJ_TAKE || (!sj.step && sj_queued);   /* (r19-fix: the next one asked for: its take) */
}

/* (r19-snd) A file call of the save failed: the file closed and dropped, the cause kept (save17's
 * words: the step, its errno, what the OS took) */
static void save_file_failed(const char *step, int err, long wrote, int in_game)
{
  if (sj.f)
  {
    fclose(sj.f);
    sj.f = NULL;
  }
  remove(sj.temp_path);
  sj.ok = 0;
  failed_file(step, err, wrote, sj.size, sj.temp_path, in_game);
}

/* An OS part's time into its part, the longest and their count kept (the timing file's line) */
static void timing_os(struct timing *t, int part)
{
  u32 before = t->total;
  timing_part(t, part);
  if (t->total - before > t->os_longest)
    t->os_longest = t->total - before;
  t->os_calls++;
}

/* The input packed a step (CX II: ~1-3 ms of the 30-65 a state takes) */
#define SJ_PACK_BYTES 16384

int states_save_step(int in_game)
{
  int began, i;
  uint32_t cpsr;

  if (!sj.step && sj_queued && !save_start_next())
    return STATES_SAVE_FAILED;
  timing_mark = nspire_ticks();
  failing = STATES_SAVING;
  switch (sj.step)
  {
  case SJ_PACK:
    if (fastdeflate_step(&sj.pack, SJ_PACK_BYTES))
    {
      timing_part(sj.t, T_PACK);
      return STATES_SAVE_BUSY;
    }
    sj.header.packed_size = (u32)fastdeflate_end(&sj.pack);
    sj.ok = sj.header.packed_size != 0;
    if (!sj.ok)
      failed(STATES_WHY_OTHER, "packing", 0, GBA_STATE_MEM_SIZE, -1);
    give_back(sj.raw, sj.raw_kept);
    sj.raw = NULL;
    timing_part(sj.t, T_PACK);
    sj.step = SJ_PICTURE;
    return STATES_SAVE_BUSY;
  case SJ_PICTURE:
    if (sj.ok && sj.has_picture && (sj.block_size = pack_picture(pic, block)) != 0)
    {
      sj.header.flags |= STATE_FLAG_PICTURE;
      sj.header.reserved[0] = sj.block_size;
      memcpy(sj.file + sizeof(sj.header) + sj.header.packed_size, block, sj.block_size);
    }
    sj.header.counter = 1;
    for (i = 0; i < STATE_SLOTS; i++)
      if (used[i] && counters[i] >= sj.header.counter)
        sj.header.counter = counters[i] + 1;
    /* (above the game's states on a USB drive too, as far as known: the newest of both places is
     * the one with the highest count: states_counter_floor) */
    if (counter_floor >= sj.header.counter)
      sj.header.counter = counter_floor + 1;
    /* save17: when it was saved, kept in it: a pack or unpack later (a new file) keeps its age */
    sj.header.reserved[1] = (u32)time(NULL);
    if (!saved_time_ok(sj.header.reserved[1]))
      sj.header.reserved[1] = 0;   /* (a clock before 2020 is wrong: the file's time then, as for older states) */
    memcpy(sj.file, &sj.header, sizeof(sj.header));
    sj.size = sizeof(sj.header) + sj.header.packed_size + sj.block_size;
    timing_part(sj.t, T_PICTURE);
    if (!sj.ok)
      return save_done();
    sj.step = sj.to == SJ_TO_DRIVE ? SJ_WRITE : SJ_CREATE;
    return STATES_SAVE_BUSY;
  case SJ_WRITE:
    if (sj.to == SJ_TO_DRIVE)
    {
      /* the drive's driver writes it under another name, syncs, renames (usbms_put) */
      sj.ok = nspire_drive_put != NULL && nspire_drive_put(sj.path, sj.file, sj.size);
      if (!sj.ok)
        failed(STATES_WHY_DRIVE, "the drive's write", 0, sj.size, -1);
      timing_part(sj.t, T_WRITE);
      return save_done();
    }
    /* (r19-snd) a part of SJ_WRITE_BYTES (the whole file when not in a game's frame, as before) */
    cpsr = file_call_begin(in_game, &began);
    {
      u32 n = sj.size - sj.written;
      long wrote;
      int err;
      if (in_game && n > SJ_WRITE_BYTES)
        n = SJ_WRITE_BYTES;
      errno = 0;
      wrote = (long)fwrite(sj.file + sj.written, 1, n, sj.f);
      err = errno;
      if ((u32)wrote == n)
        sj.written += n;
      else
        save_file_failed("write", err, (long)sj.written + (wrote > 0 ? wrote : 0), in_game);
    }
    file_call_end(began, cpsr);
    timing_os(sj.t, T_WRITE);
    if (!sj.ok)
      return save_done();
    if (sj.written == sj.size)
      sj.step = SJ_CLOSE;
    return STATES_SAVE_BUSY;
  case SJ_CREATE:
    cpsr = file_call_begin(in_game, &began);
    errno = 0;
    sj.f = fopen(sj.temp_path, "wb");
    if (!sj.f)
      save_file_failed("create", errno, -1, in_game);
    else
      setvbuf(sj.f, NULL, _IONBF, 0);   /* (unbuffered: a part's write is what reached the file) */
    file_call_end(began, cpsr);
    timing_os(sj.t, T_WRITE);
    if (!sj.ok)
      return save_done();
    sj.step = SJ_WRITE;
    return STATES_SAVE_BUSY;
  case SJ_CLOSE:
    cpsr = file_call_begin(in_game, &began);
    errno = 0;
    i = fclose(sj.f);
    sj.f = NULL;
    if (i != 0)
      save_file_failed("close", errno, -1, in_game);
    file_call_end(began, cpsr);
    timing_os(sj.t, T_WRITE);
    if (!sj.ok)
      return save_done();
    if (!nspire_file_replace_begin(&sj.rp, sj.temp_path, sj.path))
    {
      sj.ok = 0;
      failed(STATES_WHY_FILE, "rename", 0, sj.size, -1);
      remove(sj.temp_path);
      return save_done();
    }
    sj.step = SJ_REPLACE;
    return STATES_SAVE_BUSY;
  case SJ_TAKE:
    /* the state and its picture, now (the interrupts on in a game's frame: the sound plays through it) */
    cpsr = file_call_begin(in_game, &began);
    i = save_take(!in_game);
    file_call_end(began, cpsr);
    if (i)
      return STATES_SAVE_BUSY;
    if (in_game && kb.lent)
    {
      /* save17 N2: in a game's frame the kept buffers are the game's save's and there's no memory
       * of its own: that save goes on a part a frame (its OS calls served, nspire_backup_step) and the state
       * is taken once they are back (SJ_WAIT) */
      sj.step = SJ_WAIT;
      sj.waited = 0;
      return STATES_SAVE_BUSY;
    }
    failure_log("save", sj.slot);
    sj.ok = 0;
    return save_done();
  case SJ_WAIT:
    /* (N2) the game's save has the buffers: its next part (in a frame, its OS calls served; else all of it) */
    if (kb.lent)
    {
      sj.waited++;
      if (in_game && nspire_backup_step)
        nspire_backup_step();
      else if (nspire_backup_finish)
        nspire_backup_finish();
      if (kb.lent && (in_game || !nspire_backup_finish))
        return STATES_SAVE_BUSY;
    }
    states_timing_note("save slot %d waited %u frames for the game's save to give the buffers back", sj.slot + 1,
                       sj.waited);
    if (save_take(!in_game))
      return STATES_SAVE_BUSY;
    sj.ok = 0;
    return save_done();
  case SJ_REPLACE:
    cpsr = file_call_begin(in_game, &began);
    errno = 0;
    i = nspire_file_replace_step(&sj.rp);
    if (i == NSPIRE_REPLACE_BUSY && !in_game)
      while ((i = nspire_file_replace_step(&sj.rp)) == NSPIRE_REPLACE_BUSY)
        ;                       /* (not in a game's frame: all of it now, as before) */
    if (i == 0)
    {
      sj.ok = 0;
      failed(STATES_WHY_FILE, "rename", errno, sj.size, -1);
      remove(sj.temp_path);
    }
    else if (i == 1)
    {
      /* when it was saved, as the strip and the list say it: its header's (save17) */
      mtimes[sj.slot] = sj.header.reserved[1] ? sj.header.reserved[1] : file_time(sj.path);
    }
    file_call_end(began, cpsr);
    timing_os(sj.t, T_REPLACE);
    if (i == NSPIRE_REPLACE_BUSY)
      return STATES_SAVE_BUSY;
    return save_done();
  default:
    return STATES_SAVE_IDLE;
  }
}

/* (r19-fix) a save not taken yet let go: nothing written, nothing said (its timing line says why) */
static void save_drop(void)
{
  states_timing_note("save slot %d dropped: the game stopped before its state was taken", sj.slot + 1);
  give_back(sj.raw, sj.raw_kept);
  give_back(sj.file, sj.file_kept);
  sj.raw = sj.file = NULL;
  sj.step = SJ_IDLE;
}

void states_save_before_change(int how)
{
  uint32_t cpsr = 0;
  int began, i;
  if (how == STATES_CHANGE_BROKEN)
  {
    for (i = 0; i < sj_queued; i++)
      states_timing_note("save slot %d dropped: the game stopped before its state was taken", sj_queue[i].slot + 1);
    sj_queued = 0;
    if (sj.step == SJ_TAKE || sj.step == SJ_WAIT)
      save_drop();
    return;   /* (taken: the game before the change: it goes on) */
  }
  if (sj_queued)
  {
    /* (saves asked for behind it: each takes the game as it is now, so all of them now: the sound paused
     * first, faded, its device kept in packets of silence through them) */
    if (nspire_audio_pause)
      nspire_audio_pause();
    began = nspire_audio_serve_begin && nspire_audio_serve_begin(&cpsr);
    states_save_finish();
    serve_end(began, cpsr);
    return;
  }
  if (sj.step != SJ_TAKE && sj.step != SJ_WAIT)
    return;   /* (none, or taken: the game before the change) */
  /* the take now, the sound's queue served through it (as at a frame's end; a game's save that has the
   * kept buffers finished first), the rest over the frames */
  began = nspire_audio_serve_begin && nspire_audio_serve_begin(&cpsr);
  (void)states_save_step(0);
  serve_end(began, cpsr);
}

#ifdef NSPIRE_HOST
/* (the PC tests: a save stopped between its parts as a reset stops it: the files as they are) */
void states_drop_job_for_test(void)
{
  if (sj.f)
    fclose(sj.f);
  sj.f = NULL;
  give_back(sj.raw, sj.raw_kept);
  give_back(sj.file, sj.file_kept);
  sj.raw = sj.file = NULL;
  sj.step = SJ_IDLE;
  sj_queued = sj_result_n = 0;
}
#endif

int states_save_finish(void)
{
  int r = STATES_SAVE_IDLE;
  while (sj.step || sj_queued)
    r = states_save_step(0);   /* (r19-fix: the ones asked for after it too, now, in their order) */
  return r;
}

#ifdef NSPIRE_HOST
int states_save_serve_seen = -1;   /* (the PC tests: states_serve_os at the last states_save) */
#endif
int states_save(int slot)
{
  uint32_t cpsr;
  int began, r;
#ifdef NSPIRE_HOST
  states_save_serve_seen = states_serve_os;
#endif
  cpsr = serve_begin(&began);   /* (r19-snd: the menu's: the queue served through all of it) */
  r = save_begin(slot, 0, SJ_TO_CALC) && states_save_finish() == STATES_SAVE_DONE;
  serve_end(began, cpsr);
  return r;
}

static int read_packed(FILE *f, u8 *raw, const struct state_header *header)
{
  z_stream z;
  int status = Z_OK;
  u32 left = header->packed_size;

  memset(&z, 0, sizeof(z));
  if (inflateInit(&z) != Z_OK)
    return 0;
  z.next_out = raw;
  z.avail_out = GBA_STATE_MEM_SIZE;
  while (left && status == Z_OK)
  {
    u32 want = left < CHUNK ? left : CHUNK;
    if (fread(io_buffer, 1, want, f) != want)
      break;
    left -= want;
    z.next_in = io_buffer;
    z.avail_in = want;
    status = inflate(&z, Z_NO_FLUSH);
  }
  inflateEnd(&z);
  return status == Z_STREAM_END && z.total_out == GBA_STATE_MEM_SIZE;
}

static int load_file(const char *path);

/* r19-snd: with states_serve_os (the load keys, the menu) the whole load with the interrupts on: the
 * queue served from the ring (the paused sound's silence), not left to run dry on the wire for its
 * 135 ms (the r19 log's load: read 96.2, unpack 21.6, apply 17.1) */
int states_load_file(const char *path)
{
  uint32_t cpsr;
  int began, r;
  began = 0;
  cpsr = on_drive(path) ? 0 : serve_begin(&began);   /* (a drive's: its driver's own way) */
  settle();
  r = load_file(path);
  serve_end(began, cpsr);
  return r;
}

static int load_file(const char *path)
{
  struct state_header header;
  struct timing *t;
  const u8 *state = NULL;
  u8 *file = NULL, *raw = NULL;
  FILE *f;
  int ok = 0, file_kept = 0, raw_kept = 0, err = 0, intact = 0;
  long size = 0;
  settle();

  t = timing_start('l', -1);
  failing = STATES_LOADING;
  memset(&failure, 0, sizeof(failure));
  if (!file_recover(path))
  {
    failed(STATES_WHY_FILE, "rollback", errno, 0, -1);
    return 0;
  }
  /* the whole file at once, then unpacked in memory (save17: into the kept
   * file buffer; a file bigger than any state is none, not read) */
  if (on_drive(path))
  {
    /* (a drive's: its driver's own read of the whole file, not a stream's) */
    unsigned long got = 0, n = 0;
    if (!nspire_drive_stat || !nspire_drive_stat(path, &n, NULL))
    {
      failed(STATES_WHY_DRIVE, "read", ENOENT, 0, -1);
      return 0;
    }
    size = (long)n;
    if (n > FILE_ROOM)
      failed(STATES_WHY_DAMAGED, "its size", 0, n, -1);
    else if (!(file = take_buffer(1, n ? n : 1, &file_kept)))
      failed_memory("the file's buffer", n);
    else
    {
      errno = 0;
      if (!nspire_drive_get_into || !nspire_drive_get_into(path, file, n, &got) || got != n)
      {
        failed(STATES_WHY_DRIVE, "read", errno, n, -1);
        give_back(file, file_kept);
        file = NULL;
        size = 0;
      }
    }
  }
  else
  {
    errno = 0;
    f = open_read(path);
    if (!f)
    {
      failed(STATES_WHY_FILE, "open", errno, 0, -1);
      return 0;
    }
    if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0)
    {
      if ((size_t)size > FILE_ROOM)
        failed(STATES_WHY_DAMAGED, "its size", 0, (unsigned long)size, -1);
      else if (!(file = take_buffer(1, (size_t)size, &file_kept)))
        failed_memory("the file's buffer", (unsigned long)size);
      else
      {
        errno = 0;
        if (fread(file, 1, (size_t)size, f) != (size_t)size)
        {
          err = errno;
          give_back(file, file_kept);
          file = NULL;
          failed(STATES_WHY_FILE, "read", err, (unsigned long)size, -1);
        }
      }
    }
    else
      failed(STATES_WHY_FILE, "read", errno, 0, -1);
    fclose(f);
  }
  t->bytes = size;
  timing_part(t, T_READ);

  if (file && size >= (long)sizeof(header) && !memcmp(file, STATE_MAGIC, 4))
  {
    memcpy(&header, file, sizeof(header));
    if (header.version != STATE_VERSION || header.raw_size != GBA_STATE_MEM_SIZE)
      failed(STATES_WHY_FOREIGN, "its header", 0, (unsigned long)size, -1);
    else if (header.packed_size > (u32)size - sizeof(header))
      failed(STATES_WHY_DAMAGED, "its header", 0, (unsigned long)size, -1);
    else if (header.flags & STATE_FLAG_STORED)
    {
      state = header.packed_size == GBA_STATE_MEM_SIZE ? file + sizeof(header) : NULL;
      if (!state)
        failed(STATES_WHY_DAMAGED, "its header", 0, (unsigned long)size, -1);
    }
    else if ((raw = take_buffer(0, GBA_STATE_MEM_SIZE, &raw_kept)) != NULL)
    {
      uLongf raw_size = GBA_STATE_MEM_SIZE;
      int z = unpack_state(raw, &raw_size, file + sizeof(header), header.packed_size);
      if (z == Z_OK && raw_size == GBA_STATE_MEM_SIZE)
        state = raw;
      else if (z == Z_MEM_ERROR)
        failed_memory("the unpacking", 48 * 1024);
      else
        failed(STATES_WHY_DAMAGED, "its unpacking", 0, (unsigned long)size, -1);
    }
    else
      failed_memory("the state's room", GBA_STATE_MEM_SIZE);
  }
  else if (file && size == GBA_STATE_MEM_SIZE)
    state = file;   /* gpSP's older states: the state as it is in memory */
  else if (file)
    failed(STATES_WHY_FOREIGN, "its header", 0, (unsigned long)size, -1);
  intact = state != NULL;
  timing_part(t, T_UNPACK);

  if (state)
    ok = gba_load_state(state);
  /* (whole and unpacked, refused by the core: another build's or emulator's, not damaged) */
  if (!ok && intact && !failure.why)
    failed(STATES_WHY_FOREIGN, "the core's check", 0, (unsigned long)size, -1);
  if (!ok && !failure.why)
    failed(STATES_WHY_DAMAGED, "its contents", 0, (unsigned long)size, -1);
  timing_part(t, T_APPLY);
  give_back(raw, raw_kept);
  give_back(file, file_kept);
  return ok ? 1 : -1;
}

int states_load(int slot)
{
  char path[600], what[28];
  int result;
  settle();

  if (!states_exists(slot))
    return 0;
  states_file_name(slot, path, sizeof(path));
  snprintf(what, sizeof(what), "load slot %d", slot + 1);
  states_memory_look(what, 1);
  result = states_load_file(path);
  timings[(timing_count - 1) % TIMINGS].slot = slot;
  if (result < 0)
    failure_log("load", slot);
  /* R4: the link's log says what the state brought (its serial port). */
  if (result == 1 && nspire_link_state_loaded)
    nspire_link_state_loaded(slot);
  if (result == 0)
    used[slot] = 0;   /* the file went away */
  return result;
}

void *states_snapshot(void)
{
#ifdef HAVE_DYNAREC
  if (dynarec_enable)
    flush_dynarec_caches();
#endif
  return state_to_memory();
}

int states_restore(const void *snapshot)
{
  return snapshot && gba_load_state(snapshot);
}

/* ---- For the save manager ---- */

int states_file_info(int slot, long *bytes, int *compressed)
{
  settle();
  if (!states_exists(slot))
    return 0;
  /* known since the game's slots were read (one open each, not a stat and
   * an open again for every slot each time the save manager opens) */
  if (!(kinds[slot] & KIND_LOOKED))
    read_slot_info(slot);
  if (!(kinds[slot] & KIND_FOUND))
    return 0;
  *bytes = (long)sizes[slot];
  *compressed = (kinds[slot] & KIND_GPSZ) && !(kinds[slot] & KIND_STORED);
  return 1;
}

/* Rewrites a state packed or stored, keeping its counter (so which one is
 * the newest doesn't change). A state from an older build gets a header
 * (it's then this build's, counted as older than the others). */
int states_set_compressed(int slot, int compress)
{
  static u8 block[PICTURE_MAX];
  u32 block_size = 0;
  char path[600];
  struct state_header header;
  u8 *raw, *out = NULL;
  FILE *f;
  long size = 0;
  size_t cap, at;
  int ok = 0, had_header = 0, raw_kept = 0, out_kept = 0;
  settle();
  failing = STATES_SAVING;
  memset(&failure, 0, sizeof(failure));

  if (!states_exists(slot))
    return 0;
  states_file_name(slot, path, sizeof(path));
  raw = take_buffer(0, GBA_STATE_MEM_SIZE, &raw_kept);   /* (save17: the kept ones) */
  f = open_read(path);
  if (raw && f && fread(&header, sizeof(header), 1, f) == 1)
  {
    if (!memcmp(header.magic, STATE_MAGIC, 4))
    {
      had_header = 1;
      if (!(header.flags & STATE_FLAG_STORED) == !!compress)
      {
        fclose(f);
        give_back(raw, raw_kept);
        return 1;   /* already that way */
      }
      ok = header.version == STATE_VERSION && header.raw_size == GBA_STATE_MEM_SIZE &&
           ((header.flags & STATE_FLAG_STORED) ? fread(raw, 1, GBA_STATE_MEM_SIZE, f) == GBA_STATE_MEM_SIZE
                                               : read_packed(f, raw, &header));
      /* the picture stays with it */
      if (ok)
        block_size = read_picture_block(f, &header, block);
      if (!block_size)
      {
        header.flags &= ~STATE_FLAG_PICTURE;
        header.reserved[0] = 0;
      }
    }
    else if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) == GBA_STATE_MEM_SIZE &&
             fseek(f, 0, SEEK_SET) == 0)
    {
      if (!compress)
      {
        fclose(f);
        give_back(raw, raw_kept);
        return 1;   /* an older build's state is stored as it is already */
      }
      ok = fread(raw, 1, GBA_STATE_MEM_SIZE, f) == GBA_STATE_MEM_SIZE;
    }
  }
  if (f)
    fclose(f);
  if (!ok)
  {
    give_back(raw, raw_kept);
    return 0;
  }

  if (!had_header)
  {
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, STATE_MAGIC, 4);
    header.version = STATE_VERSION;
    header.raw_size = GBA_STATE_MEM_SIZE;
    header.counter = counters[slot];
    header.flags = counted[slot] ? STATE_FLAG_COUNTED : 0;
  }
  header.flags = compress ? header.flags & ~STATE_FLAG_STORED : header.flags | STATE_FLAG_STORED;
  /* save17: its age kept: the time it was saved stays in it (an older state's: its file's, now) */
  if (!saved_time_ok(header.reserved[1]))
    header.reserved[1] = saved_time_ok(file_time(path)) ? file_time(path) : 0;

  /* the file in memory (the header, the state, its picture), then written
   * whole: under another name that then takes its place (file_put) */
  cap = sizeof(header) + FASTDEFLATE_BOUND(GBA_STATE_MEM_SIZE) + PICTURE_MAX;
  out = cap <= FILE_ROOM ? take_buffer(1, cap, &out_kept) : (u8 *)malloc(cap);
  ok = out != NULL;
  if (ok)
  {
    at = sizeof(header);
    if (header.flags & STATE_FLAG_STORED)
    {
      memcpy(out + at, raw, GBA_STATE_MEM_SIZE);
      header.packed_size = GBA_STATE_MEM_SIZE;
    }
    else
    {
      header.packed_size = (u32)fastdeflate(out + at, FASTDEFLATE_BOUND(GBA_STATE_MEM_SIZE), raw, GBA_STATE_MEM_SIZE);
      ok = header.packed_size != 0;
    }
    at += header.packed_size;
    if (ok && block_size)
    {
      memcpy(out + at, block, block_size);
      at += block_size;
    }
    memcpy(out, &header, sizeof(header));
    ok = ok && file_put_why(path, out, at);
  }
  give_back(out, out_kept);
  give_back(raw, raw_kept);
  if (ok)
    read_slot_info(slot);
  return ok;
}

int states_delete(int slot)
{
  char path[600];
  settle();

  if (!states_exists(slot))
    return 0;
  states_file_name(slot, path, sizeof(path));
  if (!nspire_file_delete(path))
    return 0;
  used[slot] = counted[slot] = 0;
  counters[slot] = mtimes[slot] = 0;
  sizes[slot] = kinds[slot] = 0;
  listing_forget();
  return 1;
}
