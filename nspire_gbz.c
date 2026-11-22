/* Compressed ROMs (.gbz.tns): see nspire_gbz.h. */

#include "common.h"
#include "nspire_gbz.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* Test hook: 1 makes open behave as if inflateInit ran out of heap, so every
 * page takes the per-page uncompress path. */
int nspire_gbz_test_no_stream;

#define GBZ_HEADER_FIXED 32u

struct gbz_rom
{
  RFILE *file;
  u32 rom_size;
  u32 pages;
  u32 *offsets;   /* pages + 1 */
  int64_t pos;    /* file position after the last read, -1 if unknown */
  z_stream zs;    /* one inflate state for all pages (inflateReset per page) */
  int zs_ok;
  /* fast17: the load's read-ahead (gbz_read_ahead): the file's bytes
   * [ahead_from, ahead_from + ahead_len) in ahead, many pages a read */
  u8 *ahead;
  u32 ahead_size, ahead_from, ahead_len;
  int ahead_done;   /* the last page was read: let go until gbz_load_ahead changes */
};

struct gbz_stats gbz_stats;
static void ahead_free(gbz_rom *rom);
u32 nspire_ticks(void);

/* A compressed page is never longer than a page (the tool stores such pages
 * as they are), so one page of room holds any of them. */
static u8 packed[GBZ_PAGE_SIZE];

static u32 read_le32(const u8 *p)
{
  return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

int gbz_is_gbz(RFILE *file)
{
  u8 magic[4];
  int is_gbz;

  if (filestream_seek(file, 0, RETRO_VFS_SEEK_POSITION_START) != 0)
    return 0;
  is_gbz = filestream_read(file, magic, 4) == 4 && memcmp(magic, "GBZ1", 4) == 0;
  filestream_seek(file, 0, RETRO_VFS_SEEK_POSITION_START);
  return is_gbz;
}

gbz_rom *gbz_open(RFILE *file)
{
  u8 head[GBZ_HEADER_FIXED];
  u8 *table = NULL;
  gbz_rom *rom = NULL;
  int64_t file_size = filestream_get_size(file);
  u32 header_size, rom_size, page_size, pages, i;

  if (filestream_seek(file, 0, RETRO_VFS_SEEK_POSITION_START) != 0 ||
      filestream_read(file, head, sizeof(head)) != (int64_t)sizeof(head) ||
      memcmp(head, "GBZ1", 4) != 0)
    return NULL;

  header_size = read_le32(head + 4);
  rom_size = read_le32(head + 8);
  page_size = read_le32(head + 12);
  pages = read_le32(head + 16);
  /* gpSP maps at most 32 MB of ROM. */
  if (page_size != GBZ_PAGE_SIZE || rom_size == 0 || rom_size > 32u * 1024 * 1024 ||
      pages != (rom_size + GBZ_PAGE_SIZE - 1) / GBZ_PAGE_SIZE ||
      header_size != GBZ_HEADER_FIXED + 4 * (pages + 1) || file_size < (int64_t)header_size)
    return NULL;

  rom = (gbz_rom *)calloc(1, sizeof(*rom));
  table = (u8 *)malloc(4 * (pages + 1));
  if (!rom || !table)
    goto fail;
  rom->offsets = (u32 *)malloc(4 * (pages + 1));
  if (!rom->offsets ||
      filestream_read(file, table, 4 * (pages + 1)) != (int64_t)(4 * (pages + 1)))
    goto fail;

  for (i = 0; i <= pages; i++)
  {
    rom->offsets[i] = read_le32(table + 4 * i);
    if (rom->offsets[i] < header_size || (int64_t)rom->offsets[i] > file_size)
      goto fail;
    if (i && (rom->offsets[i] < rom->offsets[i - 1] ||
              rom->offsets[i] - rom->offsets[i - 1] > GBZ_PAGE_SIZE ||
              rom->offsets[i] == rom->offsets[i - 1]))
      goto fail;
  }

  free(table);
  rom->zs_ok = !nspire_gbz_test_no_stream && inflateInit(&rom->zs) == Z_OK;
  rom->pos = (int64_t)header_size;
  rom->file = file;
  rom->rom_size = rom_size;
  rom->pages = pages;
  return rom;

fail:
  free(table);
  if (rom)
    free(rom->offsets);
  free(rom);
  return NULL;
}

void gbz_close(gbz_rom *rom)
{
  if (!rom)
    return;
  if (rom->zs_ok)
    inflateEnd(&rom->zs);
  ahead_free(rom);
  free(rom->offsets);
  free(rom);
}

u32 gbz_load_ahead;
static gbz_rom *ahead_owner;   /* the one gbz_rom holding a read-ahead buffer */

static void ahead_free(gbz_rom *rom)
{
  free(rom->ahead);
  rom->ahead = NULL;
  rom->ahead_len = 0;
  if (ahead_owner == rom)
    ahead_owner = NULL;
}

void gbz_load_ahead_end(void)
{
  gbz_load_ahead = 0;
  if (ahead_owner)
  {
    ahead_owner->ahead_size = 0;
    ahead_owner->ahead_done = 0;
    ahead_free(ahead_owner);
  }
}

/* The read-ahead as gbz_load_ahead says now: made (once a load: no memory,
 * no read-ahead) or let go. */
static void ahead_follow(gbz_rom *rom)
{
  if (rom->ahead_size == gbz_load_ahead)
    return;
  rom->ahead_done = 0;
  ahead_free(rom);
  rom->ahead_size = gbz_load_ahead;
  if (gbz_load_ahead >= GBZ_PAGE_SIZE)
    rom->ahead = (u8 *)malloc(gbz_load_ahead);
  if (rom->ahead)
  {
    if (ahead_owner && ahead_owner != rom)
      ahead_free(ahead_owner);   /* (one at a time: never two buffers) */
    ahead_owner = rom;
  }
}

/* The page's packed bytes, from the read-ahead (read again from the page
 * on when it hasn't them); NULL if the file wouldn't read. */
static const u8 *ahead_page(gbz_rom *rom, u32 page, u32 length)
{
  u32 from = rom->offsets[page], want, end = rom->offsets[rom->pages];
  int64_t got;
  u32 t;

  if (from >= rom->ahead_from && from + length <= rom->ahead_from + rom->ahead_len)
    return rom->ahead + (from - rom->ahead_from);
  rom->ahead_len = 0;
  if (rom->pos != (int64_t)from)
  {
    rom->pos = -1;
    if (filestream_seek(rom->file, from, RETRO_VFS_SEEK_POSITION_START) != 0)
      return NULL;
  }
  rom->pos = -1;
  want = end - from < rom->ahead_size ? end - from : rom->ahead_size;
  t = nspire_ticks();
  got = filestream_read(rom->file, rom->ahead, want);
  gbz_stats.read_ticks += nspire_ticks() - t;
  gbz_stats.reads++;
  if (got < (int64_t)length)
    return NULL;
  gbz_stats.bytes += (u32)got;
  rom->ahead_from = from;
  rom->ahead_len = (u32)got;
  rom->pos = (int64_t)from + got;
  return rom->ahead;
}

u32 gbz_rom_size(const gbz_rom *rom)
{
  return rom->rom_size;
}

int gbz_read_page(gbz_rom *rom, u32 page, u8 *dst)
{
  const u8 *src = packed;
  u32 length, t;
  int ok;
  if (page >= rom->pages)
    goto fail;
  length = rom->offsets[page + 1] - rom->offsets[page];
  if (length > GBZ_PAGE_SIZE)
    goto fail;
  ahead_follow(rom);
  if (rom->ahead && !rom->ahead_done)
  {
    src = ahead_page(rom, page, length);
    if (!src)
      goto fail;
    if (page == rom->pages - 1)
    {
      /* the whole file read (a load reads it in order): the read-ahead's
       * memory goes now, not when play next reads a page */
      memcpy(packed, src, length);
      src = packed;
      ahead_free(rom);
      rom->ahead_done = 1;
    }
    if (length == GBZ_PAGE_SIZE)
    {
      memcpy(dst, src, GBZ_PAGE_SIZE);
      return 0;
    }
  }
  else
  {
    /* Seek only when the page does not follow the last read. */
    if (rom->pos != (int64_t)rom->offsets[page])
    {
      rom->pos = -1;
      if (filestream_seek(rom->file, rom->offsets[page], RETRO_VFS_SEEK_POSITION_START) != 0)
        goto fail;
    }
    rom->pos = -1;

    t = nspire_ticks();
    if (length == GBZ_PAGE_SIZE)
    {
      ok = filestream_read(rom->file, dst, GBZ_PAGE_SIZE) == GBZ_PAGE_SIZE;
      gbz_stats.read_ticks += nspire_ticks() - t;
      gbz_stats.reads++;
      if (!ok)
        goto fail;
      gbz_stats.bytes += GBZ_PAGE_SIZE;
      rom->pos = (int64_t)rom->offsets[page + 1];
      return 0;
    }

    ok = filestream_read(rom->file, packed, length) == (int64_t)length;
    gbz_stats.read_ticks += nspire_ticks() - t;
    gbz_stats.reads++;
    if (!ok)
      goto fail;
    gbz_stats.bytes += length;
    rom->pos = (int64_t)rom->offsets[page + 1];
  }
  t = nspire_ticks();
  if (!rom->zs_ok)
  {
    /* No stream at open (heap was tight): a transient one per page. */
    uLongf out = GBZ_PAGE_SIZE;
    ok = uncompress(dst, &out, src, length) == Z_OK && out == GBZ_PAGE_SIZE;
  }
  else
  {
    ok = inflateReset(&rom->zs) == Z_OK;
    if (ok)
    {
      rom->zs.next_in = (Bytef *)src;
      rom->zs.avail_in = length;
      rom->zs.next_out = dst;
      rom->zs.avail_out = GBZ_PAGE_SIZE;
      ok = inflate(&rom->zs, Z_FINISH) == Z_STREAM_END && rom->zs.total_out == GBZ_PAGE_SIZE;
    }
  }
  gbz_stats.inflate_ticks += nspire_ticks() - t;
  if (ok)
    return 0;

fail:
  memset(dst, 0xFF, GBZ_PAGE_SIZE);
  return -1;
}
