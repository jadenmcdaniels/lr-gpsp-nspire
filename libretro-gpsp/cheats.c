/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
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

typedef struct
{
  bool cheat_active;
  struct {
    u32 address;
    u32 value;
  } codes[MAX_CHEAT_CODES];
  unsigned cheat_count;
} cheat_type;

cheat_type core_cheat_slots[MAX_CHEATS];
u32 max_cheat = 0;
u32 cheat_master_hook = 0xffffffff;

/* Continuation lines are payload, even when their first nibble happens
 * to look like an encrypted or master opcode. */
static unsigned codebreaker_extra(const cheat_type *cheat, unsigned i)
{
  unsigned opcode = cheat->codes[i].address >> 28;
  if (opcode == 4)
    return 1;
  if (opcode == 5)
    return ((cheat->codes[i].value & 0xffff) * 2 + 5) / 6;
  return 0;
}

static void update_hook_codebreaker(void)
{
  unsigned slot, i;
  u32 hook = 0xffffffff;
  for (slot = 0; slot <= max_cheat; slot++)
  {
    const cheat_type *cheat = &core_cheat_slots[slot];
    if (!cheat->cheat_active)
      continue;
    for (i = 0; i < cheat->cheat_count; i++)
    {
      u32 code = cheat->codes[i].address;
      if ((code >> 28) == 1)
      {
        hook = 0x08000000 | (code & 0x1ffffff);
        break;
      }
      i += codebreaker_extra(cheat, i);
    }
    if (hook != 0xffffffff)
      break; /* The core supports one hook. */
  }
#ifdef HAVE_DYNAREC
  if (cheat_master_hook != hook)
    flush_dynarec_caches();
#endif
  cheat_master_hook = hook;
}

static void process_cheat_codebreaker(cheat_type *cheat, u16 pad)
{
  int i;
  unsigned j;
  for(i = 0; i < cheat->cheat_count; i++)
  {
    u32 code    = cheat->codes[i].address;
    u16 value   = cheat->codes[i].value;
    u32 address = code & 0xfffffff;
    u32 opcode  = code >> 28;

    switch (opcode) {
    case 0:   /* Game CRC, ignored for now */
      break;
    case 1:   /* Master code function */
      break;
    case 2:   /* 16 bit OR */
      write_memory16(address, read_memory16(address) | value);
      break;
    case 3:   /* 8 bit write */
      write_memory8(address, value);
      break;
    case 4:   /* Slide code, writes a buffer with addr/value strides */
      if (i + 1 < cheat->cheat_count)
      {
        u16 count = cheat->codes[++i].address;
        u16 vincr = cheat->codes[  i].address >> 16;
        u16 aincr = cheat->codes[  i].value;
        for (j = 0; j < count; j++)
        {
          write_memory16(address, value);
          address += aincr;
          value += vincr;
        }
      }
      break;
    case 5:   /* Super code: copies bytes to a buffer addr */
      for (j = 0; j < value * 2 && i < cheat->cheat_count; j++)
      {
        u8 bvalue, off = j % 6;
        if (!off && i + 1 >= cheat->cheat_count)
          break;
        switch (off) {
        case 0:
          bvalue = cheat->codes[++i].address >> 24;
          break;
        case 1 ... 3:
          bvalue = cheat->codes[i].address >> (24 - off*8);
          break;
        case 4 ... 5:
          bvalue = cheat->codes[i].value >> (40 - off*8);
          break;
        };
        write_memory8(address, bvalue);
        address++;
      }
      break;
    case 6:   /* 16 bit AND */
      write_memory16(address, read_memory16(address) & value);
      break;
    case 7:   /* Compare mem value and execute next cheat */
      if (read_memory16(address) != value)
        i++;
      break;
    case 8:   /* 16 bit write */
      write_memory16(address, value);
      break;
    case 10:   /* Compare mem value and skip next cheat */
      if (read_memory16(address) == value)
        i++;
      break;
    case 11:   /* Compare mem value and skip next cheat */
      if (read_memory16(address) <= value)
        i++;
      break;
    case 12:   /* Compare mem value and skip next cheat */
      if (read_memory16(address) >= value)
        i++;
      break;
    case 13:   /* Check button state and execute next cheat */
      switch ((address >> 4) & 0xf) {
      case 0:
        if (((~pad) & 0x3ff) == value)
          i++;
        break;
      case 1:
        if ((pad & value) == value)
          i++;
        break;
      case 2:
        if ((pad & value) == 0)
          i++;
        break;
      };
      break;
    case 14:   /* Increase 16/32 bit memory value */
      if (address & 1)
      {
        u32 value32 = (u32)((s16)value);  /* Sign extend to 32 bit */
        address &= ~1U;
        write_memory32(address, read_memory32(address) + value32);
      }
      else
      {
        write_memory16(address, read_memory16(address) + value);
      }
      break;
    case 15:   /* Immediate and check and skip */
      if ((read_memory16(address) & value) == 0)
        i++;
      break;
    }
  }
}

void process_cheats(void)
{
   u32 i;

   for(i = 0; i <= max_cheat; i++)
   {
      if(!core_cheat_slots[i].cheat_active)
         continue;

      process_cheat_codebreaker(&core_cheat_slots[i], 0x3ff ^ read_ioreg(REG_P1));
   }
}

void cheat_clear(void)
{
   memset(core_cheat_slots, 0, sizeof(core_cheat_slots));
   max_cheat = 0;
   update_hook_codebreaker();
}

static int cheat_hex(char c)
{
   if (c >= '0' && c <= '9') return c - '0';
   if (c >= 'a' && c <= 'f') return c - 'a' + 10;
   if (c >= 'A' && c <= 'F') return c - 'A' + 10;
   return -1;
}

cheat_error cheat_parse(unsigned index, const char *code)
{
   unsigned pos = 0, codelen, i;
   cheat_type parsed = {0};
   char buf[1024];
   if (index >= MAX_CHEATS)
      return CheatErrorTooMany;
   if (!code)
      return CheatErrorNotSupported;
   codelen = strnlen(code, sizeof(buf));
   if (codelen >= sizeof(buf))
      return CheatErrorTooBig;
   memcpy(buf, code, codelen + 1);
   for (i = 0; i < codelen; i++)
      if (cheat_hex(buf[i]) < 0)
         buf[i] = ' ';

   /* Parse the actual token lengths: the old fixed pos += 13 assumed
    * exactly one separator, and could reread or silently drop digits. */
   while (pos < codelen)
   {
      unsigned j;
      u32 address = 0, value = 0;
      while (pos < codelen && buf[pos] == ' ') pos++;
      if (pos == codelen) break;
      if (parsed.cheat_count == MAX_CHEAT_CODES)
         return CheatErrorTooBig;
      for (j = 0; j < 8; j++)
      {
         if (pos == codelen || cheat_hex(buf[pos]) < 0)
            return CheatErrorNotSupported;
         address = (address << 4) | cheat_hex(buf[pos++]);
      }
      while (pos < codelen && buf[pos] == ' ') pos++;
      for (j = 0; j < 4; j++)
      {
         if (pos == codelen || cheat_hex(buf[pos]) < 0)
            return CheatErrorNotSupported;
         value = (value << 4) | cheat_hex(buf[pos++]);
      }
      if (pos < codelen && buf[pos] != ' ')
         return CheatErrorNotSupported;
      parsed.codes[parsed.cheat_count].address = address;
      parsed.codes[parsed.cheat_count++].value = value;
   }
   if (!parsed.cheat_count)
      return CheatErrorNotSupported;
   for (i = 0; i < parsed.cheat_count; i++)
   {
      unsigned extra = codebreaker_extra(&parsed, i);
      if (extra > parsed.cheat_count - i - 1)
         return CheatErrorNotSupported;
      if ((parsed.codes[i].address >> 28) == 9)
         return CheatErrorEncrypted;
      i += extra;
   }
   /* Commit only a complete code: a rejected replacement must not leave
    * its partially parsed writes enabled in the old slot. */
   parsed.cheat_active = true;
   core_cheat_slots[index] = parsed;
   if (index > max_cheat)
      max_cheat = index;
   update_hook_codebreaker();
   return CheatNoError;
}
