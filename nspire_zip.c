/* Zipped games: see nspire_zip.h. */

#include "common.h"
#include "nspire_zip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#define CHUNK 16384

extern FILE *nspire_drive_fopen(const char *, const char *) __attribute__((weak));
static FILE *rom_fopen(const char *path, const char *mode)
{
  if (!strncmp(path, "usb:/", 5))
    return nspire_drive_fopen ? nspire_drive_fopen(path, mode) : NULL;
  return fopen(path, mode);
}

struct zip_entry
{
  char name[256];
  u32 method, crc, packed, size, header_offset;
};

struct zip_rom
{
  FILE *f;
  struct zip_entry entry;
  z_stream z;
  int inflating;     /* z is set up (deflated files) */
  u32 packed_left;   /* compressed bytes not read from the file yet */
  u32 done;          /* ROM bytes given out */
  u32 crc;
  int failed;
  u8 chunk[CHUNK];
};

static u32 get16(const u8 *p)
{
  return p[0] | (p[1] << 8);
}

static u32 get32(const u8 *p)
{
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

int zip_is_zip(const char *path)
{
  u8 head[4];
  FILE *f = rom_fopen(path, "rb");
  int zip;

  if (!f)
    return 0;
  zip = fread(head, 1, 4, f) == 4 && get32(head) == 0x04034B50;
  fclose(f);
  return zip;
}

static int has_rom_extension(const char *name)
{
  const char *dot = strrchr(name, '.');
  return dot && (!strcasecmp(dot, ".gba") || !strcasecmp(dot, ".agb"));
}

/* Finds the ROM through the central directory at the end of the zip (the
 * sizes are always right there, even when a file's own header leaves them
 * out). */
static int find_rom(FILE *f, struct zip_entry *rom, u32 *files)
{
  static u8 tail[22 + 0xFFFF];
  long file_size, tail_size, end = -1, i;
  u32 count, offset, n, listed = 0;
  int found = 0;

  if (fseek(f, 0, SEEK_END) != 0)
    return 0;
  file_size = ftell(f);
  tail_size = file_size < (long)sizeof(tail) ? file_size : (long)sizeof(tail);
  if (tail_size < 22 || fseek(f, file_size - tail_size, SEEK_SET) != 0 ||
      fread(tail, 1, (size_t)tail_size, f) != (size_t)tail_size)
    return 0;

  /* The end record: the last "PK\5\6". */
  for (i = tail_size - 22; i >= 0; i--)
    if (get32(tail + i) == 0x06054B50)
    {
      end = i;
      break;
    }
  if (end < 0)
    return 0;
  count = get16(tail + end + 10);
  offset = get32(tail + end + 16);
  if (fseek(f, (long)offset, SEEK_SET) != 0)
    return 0;

  for (n = 0; n < count; n++)
  {
    u8 header[46];
    struct zip_entry entry;
    u32 name_len, extra, comment;
    size_t keep, len;
    int take;

    if (fread(header, 1, 46, f) != 46 || get32(header) != 0x02014B50)
      break;
    name_len = get16(header + 28);
    extra = get16(header + 30);
    comment = get16(header + 32);
    keep = name_len < sizeof(entry.name) - 1 ? name_len : sizeof(entry.name) - 1;
    if (fread(entry.name, 1, keep, f) != keep)
      break;
    entry.name[keep] = 0;
    entry.method = get16(header + 10);
    entry.crc = get32(header + 16);
    entry.packed = get32(header + 20);
    entry.size = get32(header + 24);
    entry.header_offset = get32(header + 42);
    if (fseek(f, (long)(name_len - keep + extra + comment), SEEK_CUR) != 0)
      break;

    len = strlen(entry.name);
    if (len && entry.name[len - 1] != '/')
      listed++;
    /* A GBA ROM is at least its 192-byte header, and at most 32 MB. */
    if (!len || entry.name[len - 1] == '/' || (entry.method != 0 && entry.method != 8) ||
        entry.size < 192 || entry.size > 32 * 1024 * 1024)
      continue;
    /* The first .gba or .agb: a zip without one has no ROM (ZIP-4: the
     * biggest file was taken, and a .sav or .gb then booted as ARM code). */
    if (has_rom_extension(entry.name) && !found)
    {
      *rom = entry;
      found = 1;
    }
  }
  /* A directory that ends early: more may follow. */
  if (files)
    *files = n < count ? listed + 1 : listed;
  return found;
}

int zip_rom_name(const char *path, char *name, size_t size)
{
  struct zip_entry rom;
  const char *slash;
  FILE *f = rom_fopen(path, "rb");
  int found;

  if (!f)
    return 0;
  found = find_rom(f, &rom, NULL);
  fclose(f);
  if (!found)
    return 0;
  slash = strrchr(rom.name, '/');
  snprintf(name, size, "%s", slash ? slash + 1 : rom.name);
  return 1;
}

int zip_file_count(const char *path)
{
  struct zip_entry rom;
  u32 files = 0;
  FILE *f = rom_fopen(path, "rb");

  if (!f)
    return 0;
  find_rom(f, &rom, &files);
  fclose(f);
  return (int)files;
}

zip_rom *zip_open(const char *path)
{
  zip_rom *zip = (zip_rom *)calloc(1, sizeof(zip_rom));
  u8 local[30];

  if (!zip)
    return NULL;
  zip->f = rom_fopen(path, "rb");
  if (!zip->f || !find_rom(zip->f, &zip->entry, NULL) ||
      fseek(zip->f, (long)zip->entry.header_offset, SEEK_SET) != 0 ||
      fread(local, 1, 30, zip->f) != 30 || get32(local) != 0x04034B50 ||
      fseek(zip->f, (long)(get16(local + 26) + get16(local + 28)), SEEK_CUR) != 0)
  {
    zip_close(zip);
    return NULL;
  }
  zip->packed_left = zip->entry.packed;
  zip->crc = crc32(0, NULL, 0);
  if (zip->entry.method == 8)
  {
    if (inflateInit2(&zip->z, -MAX_WBITS) != Z_OK)
    {
      zip_close(zip);
      return NULL;
    }
    zip->inflating = 1;
  }
  return zip;
}

u32 zip_size(const zip_rom *zip)
{
  return zip->entry.size;
}

int zip_read(zip_rom *zip, u8 *dst, u32 n)
{
  if (zip->failed || n > zip->entry.size - zip->done)
    return -1;
  if (!zip->inflating)
  {
    if (fread(dst, 1, n, zip->f) != n)
      zip->failed = 1;
  }
  else
  {
    int status = Z_OK;
    zip->z.next_out = dst;
    zip->z.avail_out = n;
    while (zip->z.avail_out && status == Z_OK)
    {
      if (zip->z.avail_in == 0)
      {
        u32 want = zip->packed_left < CHUNK ? zip->packed_left : CHUNK;
        if (want == 0 || fread(zip->chunk, 1, want, zip->f) != want)
          break;
        zip->packed_left -= want;
        zip->z.next_in = zip->chunk;
        zip->z.avail_in = want;
      }
      status = inflate(&zip->z, Z_NO_FLUSH);
    }
    if (zip->z.avail_out)
      zip->failed = 1;
  }
  if (zip->failed)
    return -1;
  zip->crc = crc32(zip->crc, dst, n);
  zip->done += n;
  return 0;
}

int zip_check(const zip_rom *zip)
{
  return !zip->failed && zip->done == zip->entry.size && zip->crc == zip->entry.crc ? 0 : -1;
}

void zip_close(zip_rom *zip)
{
  if (!zip)
    return;
  if (zip->inflating)
    inflateEnd(&zip->z);
  if (zip->f)
    fclose(zip->f);
  free(zip);
}
