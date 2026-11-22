/* Compressing and uncompressing ROMs: see nspire_rom_convert.h. */

#include "common.h"
#include "nspire_rom_convert.h"
#include "nspire_gbz.h"
#include "nspire_paths.h"
#include "nspire_states.h"
#include "nspire_zip.h"
#include "nspire.h"
#include "nspire_frontend.h"
#include "streams/file_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#define CHUNK      GBZ_PAGE_SIZE   /* a .gbz page, so a read is one page */
/* What a full storage says (a write came back short). */
#define ROM_NO_ROOM rom_convert_no_room
const char rom_convert_no_room[] = "Not enough free storage space for it.";

/* How a .gbz's pages are deflated. Level 6 is what the states use: on real
 * ROMs (Emerald, Minish Cap, Golden Sun, Advance Wars) its files are within
 * 0.2% of level 9's, in half the time. Deflate's memory is
 * 2^(window bits + 2) + 2^(memory level + 9) bytes: 64 KB with 13 and 6,
 * where zlib's defaults (15 and 8) take 256 KB. A .gbz is made when memory
 * is short (a zip that didn't fit), and on those ROMs 13 and 6 make files
 * only 0.3% to 0.5% bigger than 15 and 8 do: a page is 32 KB, so a longer
 * window finds little more. */
#define GBZ_LEVEL       6
#define GBZ_WINDOW_BITS 13
#define GBZ_MEM_LEVEL   6
/* gpSP maps at most 32 MB of ROM: 1024 pages. */
#define GBZ_MAX_PAGES   1024u

static u8 in_buf[CHUNK], out_buf[CHUNK];

/* What a conversion cost, per stage in ticks (nspire_ticks), for
 * gpsp_convert.txt.tns: no timing of inflate or deflate on the calculator
 * existed. Written once the conversion is over, never while it runs. */
static struct
{
  u32 inflate, deflate, write, verify, total;
  unsigned long in_bytes, out_bytes;
} cv;

/* Ms and tenths of a ms of a count of ticks. */
#define CV_MS(t) (unsigned long)((t) * 1000ull / NSPIRE_TICK_HZ), (unsigned)((t) * 10000ull / NSPIRE_TICK_HZ % 10)

static size_t timed_write(const void *p, size_t n, FILE *f)
{
  u32 t = nspire_ticks();
  size_t r = fwrite(p, 1, n, f);
  cv.write += nspire_ticks() - t;
  return r;
}

/* A new file each time: appending has garbled files on the calculator. */
static void convert_log(const char *path, const char *why)
{
  char file[600];
  FILE *f;

  nspire_save_file(file, sizeof(file), "gpsp_convert.txt.tns");
  f = fopen(file, "w");
  if (!f)
    return;   /* a log only: never retried */
  fprintf(f, "convert %s\nmodel %s\nresult %s\nlevel %d window %d memlevel %d\n", path, is_cx2 ? "CX II" : "CX",
          why ? why : "ok", GBZ_LEVEL, GBZ_WINDOW_BITS, GBZ_MEM_LEVEL);
  fprintf(f, "free_mb %u need_mb %u\n", (unsigned)nspire_rom_load_free_mb, (unsigned)nspire_rom_load_need_mb);
  fprintf(f, "bytes_in %lu\nbytes_out %lu\n", cv.in_bytes, cv.out_bytes);
  fprintf(f, "read_inflate_ms %lu.%u\ndeflate_ms %lu.%u\nwrite_ms %lu.%u\nverify_ms %lu.%u\ntotal_ms %lu.%u\n",
          CV_MS(cv.inflate), CV_MS(cv.deflate), CV_MS(cv.write), CV_MS(cv.verify), CV_MS(cv.total));
  fclose(f);
}

static void put32(u8 *p, u32 v)
{
  p[0] = (u8)v;
  p[1] = (u8)(v >> 8);
  p[2] = (u8)(v >> 16);
  p[3] = (u8)(v >> 24);
}

static int ends_with(const char *s, const char *end)
{
  size_t a = strlen(s), b = strlen(end);
  return a >= b && !strcasecmp(s + a - b, end);
}

int rom_kind(const char *path)
{
  u8 head[4];
  size_t n;
  FILE *f = fopen(path, "rb");

  if (!f)
    return ROM_OTHER;
  n = fread(head, 1, 4, f);
  fclose(f);
  if (n == 4 && !memcmp(head, "PK\3\4", 4))
    return ROM_ZIP;
  if (n == 4 && !memcmp(head, "GBZ1", 4))
    return ROM_GBZ;
  return ends_with(path, ".gba.tns") || ends_with(path, ".gba") ? ROM_PLAIN : ROM_OTHER;
}

/* ---- Reading a ROM whatever it's in ---- */

struct source
{
  int kind;
  FILE *f;           /* plain */
  RFILE *rfile;      /* .gbz */
  gbz_rom *gbz;
  u32 gbz_crc;
  zip_rom *zip;
  u32 size, done;
};

static int source_open(struct source *s, const char *path)
{
  memset(s, 0, sizeof(*s));
  s->kind = rom_kind(path);
  if (s->kind == ROM_ZIP)
  {
    s->zip = zip_open(path);
    if (!s->zip)
      return 0;
    s->size = zip_size(s->zip);
    return 1;
  }
  if (s->kind == ROM_GBZ)
  {
    u8 head[24];
    FILE *f = fopen(path, "rb");
    if (!f)
      return 0;
    if (fread(head, 1, sizeof(head), f) == sizeof(head))
      s->gbz_crc = head[20] | (head[21] << 8) | (head[22] << 16) | ((u32)head[23] << 24);
    fclose(f);
    s->rfile = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
    if (!s->rfile || !(s->gbz = gbz_open(s->rfile)))
      return 0;
    s->size = gbz_rom_size(s->gbz);
    return 1;
  }
  s->f = fopen(path, "rb");
  if (!s->f)
    return 0;
  fseek(s->f, 0, SEEK_END);
  s->size = (u32)ftell(s->f);
  fseek(s->f, 0, SEEK_SET);
  return 1;
}

/* The next CHUNK bytes (fewer at the end) into in_buf: how many, 0 at the
 * end, -1 if they couldn't be read. */
static long source_read(struct source *s)
{
  u32 n = s->size - s->done < CHUNK ? s->size - s->done : CHUNK;

  if (!n)
    return 0;
  if (s->zip)
  {
    if (zip_read(s->zip, in_buf, n))
      return -1;
  }
  else if (s->gbz)
  {
    if (gbz_read_page(s->gbz, s->done / CHUNK, in_buf))
      return -1;
  }
  else if (fread(in_buf, 1, n, s->f) != n)
    return -1;
  s->done += n;
  return (long)n;
}

static long timed_read(struct source *s)
{
  u32 t = nspire_ticks();
  long n = source_read(s);
  cv.inflate += nspire_ticks() - t;
  return n;
}


/* Closes it: 0 if everything read was right (a zip's CRC, a .gbz's). */
static int source_close(struct source *s, u32 crc)
{
  int ok = 1;
  if (s->zip)
  {
    ok = zip_check(s->zip) == 0;
    zip_close(s->zip);
  }
  if (s->gbz)
  {
    ok = !s->gbz_crc || s->gbz_crc == crc;
    gbz_close(s->gbz);
  }
  if (s->rfile)
    filestream_close(s->rfile);
  if (s->f)
    fclose(s->f);
  memset(s, 0, sizeof(*s));
  return ok ? 0 : -1;
}

/* ---- Names ---- */

/* "dir/" of a path ("" for none) into dir. */
static void folder_of(const char *path, char *dir, size_t size)
{
  const char *slash = strrchr(path, '/');
  snprintf(dir, size, "%.*s", slash ? (int)(slash - path) + 1 : 0, path);
}

/* A ROM's name without its extension: "Game" for "Game.gba". */
static void strip_extension(char *name)
{
  char *dot = strrchr(name, '.');
  if (dot && dot != name)
    *dot = 0;
}

static int file_exists(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (f)
    fclose(f);
  return f != NULL;
}

/* ---- Writing ---- */

/* The job's progress; a nonzero answer from the caller stops the job at
 * the next page (then it ends as rom_convert_cancelled). */
static int stop_asked;

const char rom_convert_cancelled[] = "Cancelled.";

static void report(rom_progress_fn progress, unsigned long done, unsigned long total)
{
  if (progress && !stop_asked && progress(done, total))
    stop_asked = 1;
}

/* The page table of the .gbz being written: where each page starts in the
 * file, and where the last one ends (32 MB of ROM is 1024 pages). */
static u8 gbz_table[4 * (GBZ_MAX_PAGES + 1)];

/* The ROM at path made into a .gbz at gbz_path (nspire_gbz.h): each 32 KB
 * page deflated into its own zlib stream, or stored as it is when that
 * doesn't make it shorter, as tools/gbz.py does. The header and the page
 * table are written last, when the pages' lengths are known. crc gets the
 * ROM's CRC-32. */
static const char *write_gbz(const char *path, const char *gbz_path, rom_progress_fn progress,
                             unsigned long *total, u32 *crc)
{
  struct source s;
  z_stream z;
  u32 rom_size, pages, header_size, offset, page;
  const char *why = NULL, *damaged;
  FILE *f;

  *crc = crc32(0, NULL, 0);
  if (!source_open(&s, path))
  {
    why = s.kind == ROM_ZIP ? "No GBA ROM in this zip, or it's damaged." : "Couldn't read it.";
    source_close(&s, 0);
    return why;
  }
  damaged = s.kind == ROM_ZIP ? "The zip is damaged: its ROM doesn't read back right." : "Couldn't read it.";
  rom_size = s.size;
  if (!rom_size || rom_size > GBZ_MAX_PAGES * GBZ_PAGE_SIZE)
  {
    source_close(&s, 0);
    return "It isn't a GBA ROM.";
  }
  *total = 2ul * rom_size;
  pages = (rom_size + GBZ_PAGE_SIZE - 1) / GBZ_PAGE_SIZE;
  header_size = 32 + 4 * (pages + 1);
  memset(&z, 0, sizeof(z));
  /* Deflate's 70 KB, after the zip reader's 56 KB (nspire_zip.c: its
   * 16 KB chunk, inflate's state and 32 KB window). */
  if (deflateInit2(&z, GBZ_LEVEL, Z_DEFLATED, GBZ_WINDOW_BITS, GBZ_MEM_LEVEL, Z_DEFAULT_STRATEGY) != Z_OK)
  {
    source_close(&s, 0);
    return "Not enough memory to compress it (about 130 KB).";
  }
  f = fopen(gbz_path, "wb");
  if (!f)
  {
    deflateEnd(&z);
    source_close(&s, 0);
    return "Couldn't make the new file.";
  }

  /* Room for the header and the table, filled in at the end. */
  memset(out_buf, 0, header_size);
  if (fwrite(out_buf, 1, header_size, f) != header_size)
    why = ROM_NO_ROOM;
  offset = header_size;
  put32(gbz_table, offset);
  for (page = 0; page < pages && !why && !stop_asked; page++)
  {
    long n = timed_read(&s);
    const u8 *data = in_buf;
    u32 length = CHUNK;

    if (n <= 0)
    {
      why = damaged;
      break;
    }
    *crc = crc32(*crc, in_buf, (uInt)n);
    /* The last page is padded with zeros past the end of the ROM. */
    memset(in_buf + n, 0, CHUNK - (u32)n);
    deflateReset(&z);
    z.next_in = in_buf;
    z.avail_in = CHUNK;
    z.next_out = out_buf;
    z.avail_out = CHUNK;
    /* Z_STREAM_END: the whole page fit in out_buf. A page that doesn't
     * get shorter is stored as it is (exactly CHUNK bytes means stored). */
    {
      u32 t = nspire_ticks();
      int r = deflate(&z, Z_FINISH);
      cv.deflate += nspire_ticks() - t;
      if (r != Z_STREAM_END)
        z.total_out = CHUNK;
    }
    cv.in_bytes += (unsigned long)n;
    if (z.total_out < CHUNK)
    {
      data = out_buf;
      length = (u32)z.total_out;
    }
    if (timed_write(data, length, f) != length)
      why = ROM_NO_ROOM;
    cv.out_bytes += length;
    offset += length;
    put32(gbz_table + 4 * (page + 1), offset);
    report(progress, s.done, *total);
  }
  deflateEnd(&z);
  /* A zip's CRC is checked here, once all of its ROM was read. */
  if (source_close(&s, *crc) && !why && !stop_asked)
    why = damaged;

  if (!why && !stop_asked)
  {
    u8 head[32];
    memset(head, 0, sizeof(head));
    memcpy(head, "GBZ1", 4);
    put32(head + 4, header_size);
    put32(head + 8, rom_size);
    put32(head + 12, GBZ_PAGE_SIZE);
    put32(head + 16, pages);
    put32(head + 20, *crc);
    if (fseek(f, 0, SEEK_SET) != 0 || fwrite(head, 1, 32, f) != 32 ||
        fwrite(gbz_table, 1, 4 * (pages + 1), f) != 4 * (pages + 1))
      why = ROM_NO_ROOM;
  }
  {
    u32 t = nspire_ticks();
    int bad = fclose(f) != 0;
    cv.write += nspire_ticks() - t;
    if (bad && !why)
      why = ROM_NO_ROOM;
  }
  return why;
}

/* The ROM at path as it is into out_path. */
static const char *write_plain(const char *path, const char *out_path, rom_progress_fn progress,
                               unsigned long *total, u32 *crc)
{
  struct source s;
  const char *why = NULL;
  long n = 0;
  FILE *f;

  *crc = crc32(0, NULL, 0);
  if (!source_open(&s, path))
  {
    source_close(&s, 0);
    return "Couldn't read it.";
  }
  *total = 2ul * s.size;
  f = fopen(out_path, "wb");
  if (!f)
  {
    source_close(&s, 0);
    return "Couldn't make the new file.";
  }
  while (!stop_asked && (n = source_read(&s)) > 0)
  {
    *crc = crc32(*crc, in_buf, (uInt)n);
    if (fwrite(in_buf, 1, (size_t)n, f) != (size_t)n)
    {
      why = ROM_NO_ROOM;
      break;
    }
    report(progress, s.done, *total);
  }
  if (!why && n < 0)
    why = "Couldn't read it.";
  if (source_close(&s, *crc) && !why)
    why = "It's damaged: it doesn't read back right.";
  if (fclose(f) != 0 && !why)
    why = ROM_NO_ROOM;
  return why;
}

/* Reads the new file back, every page of a .gbz: it must give the ROM's
 * bytes (their CRC-32 is crc, the one of what was read from the old file).
 * A .gbz's header has the CRC too, and it must agree. */
static int check_new_file(const char *path, u32 crc, unsigned long half, rom_progress_fn progress)
{
  struct source s;
  u32 got = crc32(0, NULL, 0);
  long n = 0;
  int ok;

  if (!source_open(&s, path))
  {
    source_close(&s, 0);
    return 0;
  }
  while (!stop_asked && (n = source_read(&s)) > 0)
  {
    got = crc32(got, in_buf, (uInt)n);
    report(progress, half + s.done, 2 * half);
  }
  ok = n == 0 && s.done == half;
  if (source_close(&s, got))
    ok = 0;
  return ok && got == crc;
}

/* The last step of both: the checked file takes its name, the old one
 * goes. */
static const char *finish(const char *path, const char *temp, const char *new_path, int keep_old)
{
  if (rename(temp, new_path) != 0)
  {
    remove(temp);
    return "Couldn't name the new file.";
  }
  if (!keep_old)
    remove(path);
  nspire_file_list_touched();   /* a game folder's names changed */
  return NULL;
}

/* "dir/Game.gbz.tns" for a ROM: its game name (in a zip, the ROM's name in
 * it) without its extension, as tools/gbz.py names it. */
static void gbz_name(const char *path, char *gbz_path, size_t size)
{
  char dir[512], name[300];
  folder_of(path, dir, sizeof(dir));
  nspire_game_name(path, name, sizeof(name));
  strip_extension(name);
  snprintf(gbz_path, size, "%s%s.gbz.tns", dir, name);
}

int rom_zip_gbz_name(const char *path, char *gbz_path, size_t size)
{
  char inner[300];
  if (rom_kind(path) != ROM_ZIP || !zip_rom_name(path, inner, sizeof(inner)) || !ends_with(inner, ".gba"))
    return 0;
  gbz_name(path, gbz_path, size);
  return file_exists(gbz_path) ? -1 : 1;
}

int rom_convert_kept_original;

const char *rom_compress(const char *path, char *new_path, size_t size, rom_progress_fn progress)
{
  char temp[700], from[600], to[600];
  unsigned long total = 0;
  const char *why;
  u32 crc = 0, t0;
  int kind = rom_kind(path), keep_zip;

  rom_convert_kept_original = 0;
  if (kind == ROM_GBZ)
    return "It's a .gbz already.";
  if (kind == ROM_OTHER)
    return "It isn't a GBA ROM.";
  gbz_name(path, new_path, size);
  if (file_exists(new_path))
    return "Its .gbz is there already.";
  snprintf(temp, sizeof(temp), "%s.tmp", new_path);
  /* ZIP-3: only the ROM goes into the .gbz; a zip with more in it (another
   * ROM, a patch, a save) stays. */
  keep_zip = kind == ROM_ZIP && zip_file_count(path) > 1;

  stop_asked = 0;
  memset(&cv, 0, sizeof(cv));
  t0 = nspire_ticks();
  why = write_gbz(path, temp, progress, &total, &crc);
  if (!why && !stop_asked)
  {
    u32 t = nspire_ticks();
    int ok = check_new_file(temp, crc, total / 2, progress);
    cv.verify = nspire_ticks() - t;
    if (!ok && !stop_asked)
      why = "The new file didn't check out.";
  }
  if (!why && stop_asked)
    why = rom_convert_cancelled;
  cv.total = nspire_ticks() - t0;
  if (why)
  {
    /* The half-written temp goes first: on a full disk the log competes
     * with it for the space. */
    remove(temp);
    convert_log(path, why);
    return why;
  }
  /* A zip's ROM named otherwise than .gba ("Game.agb"): its saves go by the
   * .gbz's name, "Game.gba". */
  nspire_game_base(path, from, sizeof(from));
  why = finish(path, temp, new_path, keep_zip);
  rom_convert_kept_original = !why && keep_zip;
  nspire_game_base(new_path, to, sizeof(to));
  if (!why && strcasecmp(from, to))
    nspire_move_game_files(from, to);
  convert_log(path, why);
  return why;
}

const char *rom_uncompress(const char *path, char *new_path, size_t size, rom_progress_fn progress)
{
  char dir[512], inner[300], name[300], temp[700];
  unsigned long total = 0;
  const char *why;
  u32 crc;
  int kind = rom_kind(path), renamed, keep_zip;

  rom_convert_kept_original = 0;

  if (kind == ROM_PLAIN || kind == ROM_OTHER)
    return kind == ROM_PLAIN ? "It isn't compressed." : "It isn't a GBA ROM.";
  folder_of(path, dir, sizeof(dir));
  nspire_game_name(path, inner, sizeof(inner));
  /* A ROM named otherwise than .gba in a zip becomes a .gba: the file list
   * shows only those. */
  renamed = !ends_with(inner, ".gba");
  snprintf(name, sizeof(name), "%s", inner);
  if (renamed)
  {
    strip_extension(name);
    snprintf(new_path, size, "%s%s.gba.tns", dir, name);
  }
  else
    snprintf(new_path, size, "%s%s.tns", dir, name);
  if (file_exists(new_path))
    return "Its .gba is there already.";
  snprintf(temp, sizeof(temp), "%s.tmp", new_path);
  keep_zip = kind == ROM_ZIP && zip_file_count(path) > 1;   /* ZIP-3 */

  stop_asked = 0;
  why = write_plain(path, temp, progress, &total, &crc);
  if (!why && !stop_asked && !check_new_file(temp, crc, total / 2, progress) && !stop_asked)
    why = "The new file didn't check out.";
  if (!why && stop_asked)
    why = rom_convert_cancelled;
  if (why)
  {
    remove(temp);
    return why;
  }
  if (renamed)
  {
    /* Its saves go by the new name. */
    char from[600], to[600];
    nspire_game_base(path, from, sizeof(from));
    why = finish(path, temp, new_path, keep_zip);
    rom_convert_kept_original = !why && keep_zip;
    nspire_game_base(new_path, to, sizeof(to));
    if (!why)
      nspire_move_game_files(from, to);
    return why;
  }
  why = finish(path, temp, new_path, keep_zip);
  rom_convert_kept_original = !why && keep_zip;
  return why;
}
