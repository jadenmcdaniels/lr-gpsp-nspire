/*
 * arm_stub.S's nspire_block_check / nspire_block_check_tag (core-12,
 * 2026-11-15), the check of every fast push/pop/ldm/stm (arm_emit.h
 * nspire_block_fast_check), built for the calculator's CPU and run under
 * qemu-arm (tests/arm/run.sh). Until core-12 the check was inline; this
 * checks that the stubs decide as it did, for every K = 1-16 words:
 *
 *   slow if the top byte of the address isn't 3, or if (address << 17) +
 *   (K << 19) carries (the offset in IWRAM's 32 KB mirror plus 4K reaches
 *   0x8000), or, for _tag, if the tag word of the last word (IWRAM's first
 *   half, at offset + 4K - 4) isn't 0; fast: r0 = the first word's host
 *   address, IWRAM's data + offset
 *
 * (git fa69627, arm_emit.h nspire_block_fast_check and
 * nspire_block_fast_smc_check), for every top byte at a few offsets and
 * every word offset of IWRAM under five mirror patterns, with tags clear,
 * set in the last word's low or high byte, or set around it only. Both
 * ways the GBA's flags (the host's) and r9 must come back unchanged.
 */

#include "common.h"

#include <stdio.h>

void nspire_block_check(void);
void nspire_block_check_tag(void);
u32 block_check_call(u32 entry, u32 address, u32 flags, u32 *out);

static int failures, checks, fast_seen, slow_seen;
static u32 flag_step;

static void check(int ok, const char *what, int tag, u32 k, u32 address)
{
  checks++;
  if (!ok && failures++ < 20)
    printf("FAIL  block_check_test: %s, %s, K %lu, address %08lx\n", what, tag ? "tag" : "plain",
           (unsigned long)k, (unsigned long)address);
}

/* The inline check before core-12. */
static int old_fast(u32 address, u32 k, int tag)
{
  u32 offset = address & 0x7FFF;
  if ((address >> 24) != 3)
    return 0;
  if ((((u64)(u32)(address << 17)) + ((u64)k << 19)) >> 32)
    return 0;
  if (tag && *(u32 *)(iwram + offset + 4 * k - 4) != 0)
    return 0;
  return 1;
}

static void one(int tag, u32 k, u32 address)
{
  u32 entry = (u32)(tag ? nspire_block_check_tag : nspire_block_check) + 8 * (k - 1);
  u32 flags = ((flag_step++ * 7) & 31) << 27;
  u32 out[3];
  int fast = block_check_call(entry, address, flags, out);
  int want = old_fast(address, k, tag);

  fast ? fast_seen++ : slow_seen++;
  check(fast == want, want ? "slow where the old check was fast" : "fast where the old check was slow", tag,
        k, address);
  check((out[1] & 0xF8000000) == flags, "flags changed", tag, k, address);
  check(out[2] == address, "r9 changed", tag, k, address);
  if (fast && want)
    check(out[0] == (u32)iwram + 0x8000 + (address & 0x7FFF), "wrong host address", tag, k, address);
}

static void set_tag(u32 offset, u32 value)
{
  if (offset < 0x8000)
    *(u32 *)(iwram + offset) = value;
}

int main(void)
{
  static const u32 others[] = { 0, 0x4000, 0x7FFC, 0xFF8000 | 0x7FFC, 0x123450 };
  static const u32 mirrors[] = { 0, 0x8000, 0x7F8000, 0xFF8000, 0x150000 };
  int tag;
  u32 k, b, i, m, offset, p;

  if ((u32)reg - (u32)iwram != 0xA8000)
  {
    printf("FAIL  block_check_test: IWRAM's data is 0x%lx below reg, the stubs expect 0xa0000\n",
           (unsigned long)((u32)reg - (u32)iwram - 0x8000));
    return 1;
  }
  for (tag = 0; tag < 2; tag++)
    for (k = 1; k <= 16; k++)
    {
      for (b = 0; b < 256; b++)
        for (i = 0; i < sizeof(others) / sizeof(others[0]); i++)
          one(tag, k, (b << 24) | others[i]);
      for (m = 0; m < sizeof(mirrors) / sizeof(mirrors[0]); m++)
        for (offset = 0; offset < 0x8000; offset += 4)
        {
          u32 address = 0x03000000 | mirrors[m] | offset;
          u32 last = offset + 4 * k - 4;
          for (p = 0; p < (tag ? 4u : 1u); p++)
          {
            if (p == 1)
              set_tag(last, 0x00000001);
            else if (p == 2)
              set_tag(last, 0x01000000);
            else if (p == 3)
            {
              if (last >= 4)
                set_tag(last - 4, 0xFFFFFFFF);
              set_tag(last + 4, 0xFFFFFFFF);
            }
            one(tag, k, address);
            if (last >= 4)
              set_tag(last - 4, 0);
            set_tag(last, 0);
            set_tag(last + 4, 0);
          }
        }
    }
  printf("%s: block_check_test: %d checks, %d failed (%d fast, %d slow; K 1-16, every IWRAM word offset, tags)\n",
         failures ? "FAIL" : "PASS", checks, failures, fast_seen, slow_seen);
  return failures != 0;
}
