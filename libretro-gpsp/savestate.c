/* gameplaySP
 *
 * Copyright (C) 2023 David Guillen Fandos <david@davidgf.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "common.h"

const u8 *state_mem_read_ptr;
u8 *state_mem_write_ptr;

/* A state is a fixed-size buffer. Validate its entire tree before any reader
 * changes the machine; nested lengths and unterminated keys used to walk out
 * of that buffer. The readers also bound their own walks for unit callers. */
static bool bson_element(const u8 *p, const u8 *end, const u8 **name,
                         const u8 **value, const u8 **next)
{
  const u8 *nul;
  unsigned bytes;
  if (p >= end || !(nul = (const u8 *)memchr(p + 1, 0, end - p - 1)))
    return false;
  *name = p + 1;
  *value = nul + 1;
  p = *value;
  switch ((*name)[-1]) {
  case BSON_TYPE_INT32:
    bytes = 4;
    break;
  case BSON_TYPE_DOC:
  case BSON_TYPE_ARR:
    if (end - p < 4)
      return false;
    bytes = bson_read_u32(p);
    if (bytes < 5)
      return false;
    break;
  case BSON_TYPE_BIN:
    if (end - p < 5)
      return false;
    bytes = bson_read_u32(p);
    if (bytes > (unsigned)(end - p - 5))
      return false;
    bytes += 5;
    break;
  default:
    return false;
  }
  if (bytes > (unsigned)(end - p))
    return false;
  *next = p + bytes;
  return true;
}

static bool bson_validate(const u8 *src, unsigned available, unsigned depth)
{
  unsigned size;
  const u8 *p, *end, *name, *value, *next;
  if (available < 5 || depth > 16)
    return false;
  size = bson_read_u32(src);
  if (size < 5 || size > available || src[size - 1] != 0)
    return false;
  end = src + size - 1;
  for (p = src + 4; p < end; p = next) {
    if (!bson_element(p, end, &name, &value, &next))
      return false;
    if ((*p == BSON_TYPE_DOC || *p == BSON_TYPE_ARR) &&
        !bson_validate(value, next - value, depth + 1))
      return false;
  }
  return p == end;
}

static const u8 *bson_lookup(const u8 *src, const char *key, u8 *type)
{
  unsigned size, keylen = strlen(key) + 1;
  const u8 *p, *end, *name, *value, *next;
  if (!src)
    return NULL;
  size = bson_read_u32(src);
  if (size < 5 || size > GBA_STATE_MEM_SIZE)
    return NULL;
  end = src + size - 1;
  for (p = src + 4; p < end; p = next) {
    if (!bson_element(p, end, &name, &value, &next))
      return NULL;
    if ((unsigned)(value - name) == keylen && !memcmp(name, key, keylen)) {
      *type = *p;
      return value;
    }
  }
  return NULL;
}

bool bson_contains_key(const u8 *src, const char *key, u8 keytype)
{
  u8 type;
  return bson_lookup(src, key, &type) && type == keytype;
}

const u8 *bson_find_key(const u8 *src, const char *key)
{
  u8 type;
  return bson_lookup(src, key, &type);
}

const u8 *bson_find_document(const u8 *src, const char *key)
{
  u8 type;
  const u8 *p = bson_lookup(src, key, &type);
  return p && type == BSON_TYPE_DOC ? p : NULL;
}

bool bson_read_int32(const u8 *src, const char *key, u32 *value)
{
  u8 type;
  const u8 *p = bson_lookup(src, key, &type);
  if (!p || type != BSON_TYPE_INT32)
    return false;
  *value = bson_read_u32(p);
  return true;
}

bool bson_check_int32_array(const u8 *src, const char *key, unsigned cnt)
{
  unsigned i;
  u8 type;
  const u8 *p = bson_lookup(src, key, &type);
  if (!p || type != BSON_TYPE_ARR || cnt > (GBA_STATE_MEM_SIZE - 5) / 8 ||
      (unsigned)bson_read_u32(p) != 5 + cnt * 8 || p[4 + cnt * 8] != 0)
    return false;
  p += 4;
  for (i = 0; i < cnt; i++, p += 8)
    if (p[0] != BSON_TYPE_INT32 || p[1] != '0' + i / 10 ||
        p[2] != '0' + i % 10 || p[3] != 0)
      return false;
  return true;
}

bool bson_read_int32_array(const u8 *src, const char *key, u32 *value, unsigned cnt)
{
  const u8 *p;
  if (!bson_check_int32_array(src, key, cnt))
    return false;
  p = bson_find_key(src, key) + 8;
  while (cnt--) {
    *value++ = bson_read_u32(p);
    p += 8;
  }
  return true;
}

bool bson_check_bytes(const u8 *src, const char *key, unsigned cnt)
{
  u8 type;
  const u8 *p = bson_lookup(src, key, &type);
  return p && type == BSON_TYPE_BIN && (unsigned)bson_read_u32(p) == cnt && p[4] == 0;
}

bool bson_read_bytes(const u8 *src, const char *key, void *buffer, unsigned cnt)
{
  if (!bson_check_bytes(src, key, cnt))
    return false;
  memcpy(buffer, bson_find_key(src, key) + 5, cnt);
  return true;
}

bool gba_load_state(const void* src)
{
  u32 i, tmp;
  u8* srcptr = (u8*)src;
  if (!srcptr || (unsigned)bson_read_u32(srcptr) != GBA_STATE_MEM_SIZE ||
      !bson_validate(srcptr, GBA_STATE_MEM_SIZE, 0))
    return false;

  if (!bson_read_int32(srcptr, "info-magic", &tmp) || tmp != GBA_STATE_MAGIC)
    return false;
  if (!bson_read_int32(srcptr, "info-version", &tmp) || tmp != GBA_STATE_VERSION)
    return false;

  // Validate that the state file makes sense before unconditionally reading it.
  if (!cpu_check_savestate(srcptr) ||
      !input_check_savestate(srcptr) ||
      !main_check_savestate(srcptr) ||
      !memory_check_savestate(srcptr) ||
      !sound_check_savestate(srcptr))
     return false;

#ifdef NSPIRE_BUILD
  nspire_video_lines_reset();
#endif

  if (!(cpu_read_savestate(srcptr) &&
      input_read_savestate(srcptr) &&
      main_read_savestate(srcptr) &&
      memory_read_savestate(srcptr) &&
      sound_read_savestate(srcptr)))
  {
     // TODO: this should not happen if the validation above is accurate.
     return false;
  }

  // Generate converted palette (since it is not saved)
  for(i = 0; i < 512; i++)
  {
     palette_ram_converted[i] = convert_palette(eswap16(palette_ram[i]));
  }

  video_reload_counters();

  // Reset most of the frame state and dynarec state
#ifdef HAVE_DYNAREC
  if (dynarec_enable)
    flush_dynarec_caches();
#endif

  instruction_count = 0;
  reg[OAM_UPDATED] = 1;

  return true;
}

void gba_save_state(void* dst)
{
  u8 *stptr = (u8*)dst;
  u8 *wrptr = (u8*)dst;

  // Initial bson size
  bson_write_u32(wrptr, 0);

  // Add some info fields
  bson_write_int32(wrptr, "info-magic", GBA_STATE_MAGIC);
  bson_write_int32(wrptr, "info-version", GBA_STATE_VERSION);

  wrptr += cpu_write_savestate(wrptr);
  wrptr += input_write_savestate(wrptr);
  wrptr += main_write_savestate(wrptr);
  wrptr += memory_write_savestate(wrptr);
  wrptr += sound_write_savestate(wrptr);

  // The padding space is pushed into a padding field for easy parsing
  {
    unsigned padsize = GBA_STATE_MEM_SIZE - (wrptr - stptr);
    padsize -= 1 + 9 + 4 + 1 + 1;
    *wrptr++ = 0x05;    // Byte array
    bson_write_cstring(wrptr, "zpadding");
    bson_write_u32(wrptr, padsize);
    *wrptr++ = 0;
    wrptr += padsize;
  }

  *wrptr++ = 0;

  // Update the doc size  
  bson_write_u32(stptr, wrptr - stptr);
}


