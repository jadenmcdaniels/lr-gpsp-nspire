/*
 * core-16 (2026-11-15): a proven-pure idle loop's events (arm_emit.h
 * nspire_idle_body_pure, arm_stub.S arm_update_gba_pure_thumb, main.c
 * update_gba) on rom/idle_loop.S, under qemu-arm (tests/arm/run.sh):
 *
 *   idle_test ROM
 *
 * The ROM's wait loop is translated as pure; update_gba runs its next events
 * itself only while nothing the loop reads can change. A loop on an I/O
 * register (VCOUNT) must see every line (its loads mark REG_IDLE_TAINT), and
 * one on EWRAM must stop being repeated at the event whose DMA writes it.
 */

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern u32 nspire_dynarec_ram_policy, nspire_rom_buffer_size_choice;
extern u32 nspire_idle_pure_count;
extern u16 *gba_screen_pixels;
extern u32 execute_cycles;
void init_gamepak_buffer(void);

static int failures, checks;

static void check(int ok, const char *what)
{
  checks++;
  if (!ok)
  {
    failures++;
    printf("FAIL  %s\n", what);
  }
}

static u32 result(unsigned offset)
{
  u32 value;
  memcpy(&value, &ewram[0x3FF00 + offset], 4);
  return value;
}

int main(int argc, char **argv)
{
  char line[160];
  int frame;

  if (argc < 2)
  {
    fprintf(stderr, "usage: idle_test ROM\n");
    return 2;
  }
  nspire_rom_buffer_size_choice = 2;
  init_gamepak_buffer();
  init_sound();
  gba_screen_pixels = calloc(1, GBA_SCREEN_BUFFER_SIZE);
  memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
  if (load_gamepak(NULL, argv[1], FEAT_AUTODETECT, FEAT_AUTODETECT, SERIAL_MODE_AUTO) != 0)
  {
    printf("FAIL  couldn't load %s\n", argv[1]);
    return 1;
  }
  idle_loop_target_pc = 0x08000204;   /* the ROM's wait loop's bne */
  reset_gba();
  memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
  for (frame = 0; frame < 5; frame++)
    execute_arm_translate(execute_cycles);

  check(result(0) == 0x454E4F44, "the ROM ran to its end");
  snprintf(line, sizeof(line), "the wait loop was translated as pure (%u)",
           (unsigned)nspire_idle_pure_count);
  check(nspire_idle_pure_count >= 1, line);
  snprintf(line, sizeof(line), "waiting for VCOUNT 100 (I/O): left at line %u, expected 100",
           (unsigned)result(4));
  check(result(4) == 100, line);
  snprintf(line, sizeof(line), "waiting for a VBlank DMA into EWRAM: left at line %u, expected 160",
           (unsigned)result(8));
  check(result(8) == 160, line);

  printf("%s: pure idle loop: %d of %d checks passed\n", failures ? "FAIL" : "PASS",
         checks - failures, checks);
  return failures != 0;
}
