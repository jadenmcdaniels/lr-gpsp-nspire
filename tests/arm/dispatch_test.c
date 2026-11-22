/*
 * arm_stub.S's mem_dispatch_* stubs (every load and store the dynarec's
 * code makes goes through one), built for the calculator's CPU and run
 * under qemu-arm (tests/arm/run.sh). Since 2026-11-15 (core-10) they index
 * a 256-entry table per kind by the address's whole top byte instead of
 * clamping it to the 17-entry tables' "-1" entry; this checks that every
 * address reaches the handler the old clamp formula gave:
 *
 *   old: r2 = top byte (with bit 0, or bits 1:0, of the address moved to
 *        bits 31(:30) first for u16 (u32) loads); r2 > 15 -> -1;
 *        handler = 17-entry table[r2 + 1]
 *
 * for every top byte 0x00-0xFF, every low-2-bit pattern and a few middle
 * bit patterns, through each of the nine stubs, after init_emitter with and
 * without the ROM's byte swap. The stub is run for real with every table
 * entry pointed at dispatch_probe.S's probe, which reports the entry it was
 * read from; the handler is that entry's value as init_emitter wrote it.
 * Then the flush handlers nspire_lines.c swaps in while picture lines wait:
 * regions 0x05-0x07 of the four store tables only, and back. Since core-17
 * no PC word follows the call (dispatch_probe.S).
 */

#include "common.h"

#include <stdio.h>
#include <string.h>

void mem_dispatch_store_u8(void);
void mem_dispatch_store_u16(void);
void mem_dispatch_store_u32(void);
void mem_dispatch_store_u32_safe(void);
void mem_dispatch_load_u8(void);
void mem_dispatch_load_s8(void);
void mem_dispatch_load_u16(void);
void mem_dispatch_load_s16(void);
void mem_dispatch_load_u32(void);

u32 dispatch_call(void (*stub)(void), u32 address);
void dispatch_probe(void);
extern u32 dispatch_probe_regs[3];

extern u32 *nspire_mem_tables[9];
extern u32 st_handler_functions[4][17];
extern u32 ld_handler_functions[5][17];
extern u32 ld_swap_handler_functions[5][17];
extern u32 st_flush_functions[4][3];
extern u32 nspire_batch_lines;
extern u32 nspire_rom_buffer_size_choice;
extern u16 *gba_screen_pixels;

static void (*const stubs[9])(void) = {
  mem_dispatch_store_u8, mem_dispatch_store_u16, mem_dispatch_store_u32, mem_dispatch_store_u32_safe,
  mem_dispatch_load_u8, mem_dispatch_load_s8, mem_dispatch_load_u16, mem_dispatch_load_s16,
  mem_dispatch_load_u32,
};
static const char *const names[9] = {
  "store_u8", "store_u16", "store_u32", "store_u32_safe", "load_u8", "load_s8", "load_u16", "load_s16",
  "load_u32",
};
static const int abits[9] = { 0, 0, 0, 0, 0, 0, 1, 1, 2 };

static u32 saved[9][256];
static int failures, checks;

static void check(int ok, const char *what, int t, u32 address)
{
  checks++;
  if (!ok && failures++ < 20)
    printf("FAIL  dispatch_test: %s, %s, address %08lx\n", what, names[t], (unsigned long)address);
}

/* The handler the dispatch before core-10 reached (arm_stub.S, git 252cf80). */
static u32 old_handler(int t, u32 address, int must_swap)
{
  u32 r2 = abits[t] ? (address | (address << (32 - abits[t]))) >> 24 : address >> 24;
  int index = (s32)(15 - r2) < 0 ? -1 : (int)r2;
  const u32 *table = t < 4 ? st_handler_functions[t]
                           : (must_swap ? ld_swap_handler_functions : ld_handler_functions)[t - 4];
  return table[index + 1];
}

static void all_entries_probe(void)
{
  int t, i;
  for (t = 0; t < 9; t++)
    for (i = 0; i < 256; i++)
      nspire_mem_tables[t][i] = (u32)dispatch_probe;
}

static void restore(void)
{
  int t;
  for (t = 0; t < 9; t++)
    memcpy(nspire_mem_tables[t], saved[t], sizeof(saved[t]));
}

static void dispatch_all(int must_swap)
{
  static const u32 middles[] = { 0, 0x00FFFFFC, 0x00123450, 0x00800000, 0x007FFFF0 };
  int t, b, lo, m;

  init_dynarec_caches();   /* as init_main does before init_emitter */
  init_emitter(must_swap);
  for (t = 0; t < 9; t++)
    memcpy(saved[t], nspire_mem_tables[t], sizeof(saved[t]));
  all_entries_probe();
  for (t = 0; t < 9; t++)
    for (b = 0; b < 256; b++)
      for (lo = 0; lo < 4; lo++)
        for (m = 0; m < (int)(sizeof(middles) / sizeof(middles[0])); m++)
        {
          u32 address = ((u32)b << 24) | middles[m] | lo;
          u32 index = dispatch_call(stubs[t], address);
          u32 entry = (u32)reg + 4 * index;
          u32 slot = (entry - (u32)nspire_mem_tables[t]) / 4;
          check(dispatch_probe_regs[0] == address && dispatch_probe_regs[1] == 0x5A17C0DE,
                "r0/r1 changed on the way to the handler", t, address);
          check(entry >= (u32)nspire_mem_tables[t] && slot < 256, "entry outside the stub's table", t, address);
          if (slot < 256)
            check(saved[t][slot] == old_handler(t, address, must_swap),
                  must_swap ? "handler differs from the old clamp (byte-swapped ROM)"
                            : "handler differs from the old clamp", t, address);
        }
  restore();
}

static void flush_handlers(void)
{
  int t, i;

  init_dynarec_caches();
  init_emitter(0);
  for (t = 0; t < 9; t++)
    memcpy(saved[t], nspire_mem_tables[t], sizeof(saved[t]));
  skip_next_frame = 0;
  nspire_batch_lines = 1;
  nspire_video_line_ready(0);
  for (t = 0; t < 9; t++)
    for (i = 0; i < 256; i++)
    {
      u32 want = t < 4 && i >= 5 && i <= 7 ? st_flush_functions[t][i - 5] : saved[t][i];
      check(nspire_mem_tables[t][i] == want, "entry while lines wait", t, (u32)i << 24);
    }
  nspire_video_lines_reset();
  for (t = 0; t < 9; t++)
    check(!memcmp(nspire_mem_tables[t], saved[t], sizeof(saved[t])), "entries after the lines were dropped", t, 0);
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    fprintf(stderr, "usage: dispatch_test ROM\n");
    return 2;
  }
  /* The core as dynarec_test sets it up: init_emitter translates the BIOS's
   * SWI entry, so it needs the BIOS and a game loaded. */
  nspire_rom_buffer_size_choice = 2;
  init_gamepak_buffer();
  init_sound();
  gba_screen_pixels = calloc(1, GBA_SCREEN_BUFFER_SIZE);
  memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
  if (load_gamepak(NULL, argv[1], FEAT_AUTODETECT, FEAT_AUTODETECT, SERIAL_MODE_AUTO) != 0)
  {
    printf("FAIL  dispatch_test: couldn't load %s\n", argv[1]);
    return 1;
  }
  reset_gba();
  dispatch_all(0);
  dispatch_all(1);
  flush_handlers();
  printf("%s: dispatch_test: %d checks, %d failed (every top byte and low bits through the 9 mem_dispatch "
         "stubs, the flush handlers)\n", failures ? "FAIL" : "PASS", checks, failures);
  return failures != 0;
}
