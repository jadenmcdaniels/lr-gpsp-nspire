/*
 * gpSP's dynarec, built for the calculator's CPU and run under qemu-arm
 * (tests/arm/run.sh), on rom/ram_code.S: code in IWRAM and EWRAM that
 * branches between its own blocks and is rewritten by DMA and by the CPU.
 *
 *   dynarec_test ROM POLICY      POLICY: 0 = Partial SMC, 1 = Classic
 *
 * Partial SMC used to leave such branches as a placeholder (B +0) that ran
 * the next host instructions instead: Pokemon Emerald froze or crashed at
 * once. A branch linked straight into RAM code would run stale code after a
 * DMA, so the results after each rewrite are checked too.
 */

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern u32 nspire_dynarec_ram_policy, nspire_rom_buffer_size_choice;
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
  static const struct { unsigned offset; u32 expected; const char *what; } cases[] = {
    { 4, 165, "ARM code in IWRAM: a call, a loop and a jump between its blocks" },
    { 8, 275, "the same after DMA rewrote it" },
    { 12, 385, "the same after a CPU store rewrote it" },
    { 16, 165, "Thumb code in IWRAM" },
    { 20, 275, "the same after a 16-bit DMA rewrote it" },
    { 24, 165, "ARM code in EWRAM" },
    { 28, 275, "the same after DMA rewrote it" },
    /* Block transfers leaving the IWRAM fast path (core-11, 2026-11-15:
     * their per-register code at the block's tail) */
    { 36, 495, "ARM code in IWRAM after an stmia rewrote it" },
    { 40, 385, "Thumb code in IWRAM after a push rewrote it" },
    { 44, 495, "ARM code in IWRAM with its stack in EWRAM" },
    { 48, 385, "Thumb code in IWRAM with its stack in EWRAM" },
    { 52, 0xE1A00005, "an ldmia across IWRAM's end: the word from its start" },
    { 56, 495, "ARM code in IWRAM after an stmia across IWRAM's end" },
    /* An ignored store's return (ext_store_ignore) after a u32_safe call
     * skipped the tail's "b join" (before fdc1adf, and in the PCWORD_CHECK
     * build until 2026-11-15) */
    { 60, 2, "a Thumb push {r1, lr} into the BIOS, then one into EWRAM: both adds after them ran" },
    { 64, 0x13131313, "the EWRAM push's first word" },
    /* An SWI's exit from IWRAM code is linked into the ROM cache; the
     * block made again after a flush (speed10b: copied from the kept one,
     * that branch moved) */
    { 68, 12, "ARM code in IWRAM calling the BIOS (Sqrt)" },
    { 72, 13, "the same after a flush" },
    { 76, 14, "Thumb code in IWRAM calling the BIOS" },
    { 80, 15, "the same after a flush" },
  };
  char line[160];
  unsigned i;
  int frame;

  if (argc < 3)
  {
    fprintf(stderr, "usage: dynarec_test ROM POLICY\n");
    return 2;
  }
  nspire_dynarec_ram_policy = atoi(argv[2]);
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
  reset_gba();
  memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
  /* The fast block transfers (arm_emit.h nspire_block_fast) need IWRAM's
   * data 0xA0000 below reg; without them the cases above test less. */
  check((u8 *)reg - (iwram + 0x8000) == 0xA0000,
        "the dynarec's IWRAM fast path for block transfers is on");
  for (frame = 0; frame < 10; frame++)
  {
    execute_arm_translate(execute_cycles);
#ifdef NSPIRE_KEEP_TRACE
    {
      /* speed10b's trace builds: the RAM cache as the frame ends */
      void nspire_keep_trace(char *text, u32 size);
      char trace[160];
      nspire_keep_trace(trace, sizeof(trace));
      printf("%s\n", trace);
    }
#endif
  }
#ifdef NSPIRE_KEEP_CHECK
  {
    /* speed10b's check build: the copies of kept blocks against their
     * translations */
    extern u32 nspire_keep_check_ok, nspire_keep_check_bad, nspire_keep_check_rom;
    printf("kept-block check: %u blocks as made, %u not; %u copies linking into the ROM cache\n",
           (unsigned)nspire_keep_check_ok, (unsigned)nspire_keep_check_bad,
           (unsigned)nspire_keep_check_rom);
  }
#endif
#ifdef NSPIRE_KEEP_TRACE
  printf("RAM blocks copied %u\n", (unsigned)reg[REG_RAM_KEEP_USES]);
#endif

  snprintf(line, sizeof(line), "the ROM ran to its end (it was at step %u)", (unsigned)result(32));
  check(result(0) == 0x454E4F44, line);
  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    snprintf(line, sizeof(line), "%s: sum %u, expected %u", cases[i].what,
             (unsigned)result(cases[i].offset), (unsigned)cases[i].expected);
    check(result(cases[i].offset) == cases[i].expected, line);
  }

  printf("%s: dynarec with %s SMC: %d of %d checks passed\n", failures ? "FAIL" : "PASS",
         nspire_dynarec_ram_policy ? "Classic" : "Partial", checks - failures, checks);
  return failures != 0;
}
