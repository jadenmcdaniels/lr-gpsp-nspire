/* nspire_fastdeflate.c, as compiled for the calculator: every input packs
 * (by fastdeflate, and by fastdeflate_picture with rows of 288 bytes, as
 * states' pictures, and of other sizes) into a zlib stream that zlib
 * unpacks to the same bytes, within FASTDEFLATE_BOUND (and compressBound);
 * with less room than it needs it returns 0 and writes nothing past the
 * room it has.
 *   fastdeflate_test [FILE...]
 * Inputs: zeros, 0xFF, random bytes and others at sizes around the block,
 * stored-block and Adler-32 boundaries, at every start alignment; each FILE
 * as it is, and when it is a gpSP state (GPSZ, nspire_states.c) its state
 * and its picture unpacked. Buffers are exactly as big as they have to be,
 * so reading past them shows under a checker. */

#include "nspire_fastdeflate.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define GUARD 64

static int tests, failed;
static unsigned rng = 12345;

/* 0: fastdeflate; else fastdeflate_picture with rows of this many bytes
 * (over 32768: none) */
static unsigned row;

static size_t pack(unsigned char *out, size_t cap, const unsigned char *in, size_t n)
{
  return row ? fastdeflate_picture(out, cap, in, n, row) : fastdeflate(out, cap, in, n);
}

static unsigned rnd(void)
{
  rng = rng * 1103515245u + 12345u;
  return rng >> 8;
}

static void check(const char *what, const unsigned char *in, size_t n)
{
  size_t bound = FASTDEFLATE_BOUND(n), got, small;
  unsigned char *out = malloc(bound + GUARD), *back = malloc(n ? n : 1);
  uLongf back_len = n;
  size_t k;
  int ok;

  tests++;
  if (!out || !back)
  {
    printf("FAIL  %s, %u bytes: no memory\n", what, (unsigned)n);
    failed++;
    free(out);
    free(back);
    return;
  }
  memset(out + bound, 0xA5, GUARD);
  got = pack(out, bound, in, n);
  ok = got > 0 && got <= bound && got <= compressBound(n);
  for (k = bound; ok && k < bound + GUARD; k++)
    ok = out[k] == 0xA5;
  ok = ok && uncompress(back, &back_len, out, got) == Z_OK && back_len == n && !memcmp(back, in, n);
  if (ok && got > 6)
  {
    /* one byte less room: 0, and nothing written past it */
    small = got - 1;
    memset(out + small, 0x5A, bound + GUARD - small);
    ok = pack(out, small, in, n) == 0;
    for (k = small; ok && k < bound + GUARD; k++)
      ok = out[k] == 0x5A;
    if (!ok)
      printf("FAIL  %s, %u bytes, row %u: with %u bytes of room\n", what, (unsigned)n, row, (unsigned)small);
  }
  else if (!ok)
    printf("FAIL  %s, %u bytes, row %u: packed to %u (bound %u)\n", what, (unsigned)n, row, (unsigned)got,
           (unsigned)bound);
  if (!ok)
    failed++;
  free(out);
  free(back);
}

static void fill(unsigned char *b, size_t n, int kind)
{
  size_t k;
  for (k = 0; k < n; k++)
    switch (kind)
    {
    case 0: b[k] = 0; break;
    case 1: b[k] = 0xFF; break;
    case 2: b[k] = (unsigned char)rnd(); break;
    case 3: b[k] = (unsigned char)(rnd() % 4); break;                     /* short matches */
    case 4: b[k] = (k / 300) & 1 ? (unsigned char)rnd() : 7; break;      /* runs, noise */
    case 5: b[k] = (unsigned char)("abcabcabd"[k % 9] + (rnd() % 50 == 0)); break;
    case 6: b[k] = rnd() % 16 == 0 ? (unsigned char)rnd() : k ? b[k - 1] : 0; break;
    case 7: b[k] = k >= 40000 ? b[k - 40000 + (k & 1)] : (unsigned char)rnd(); break;  /* over the window */
    default: b[k] = (unsigned char)(k * 2 + (k >> 9)); break;            /* 16-bit-ish */
    }
}

/* The state (and picture) in a gpSP state file, unpacked. */
static void check_state_file(const char *path, const unsigned char *d, size_t size)
{
  uint32_t h[8];
  if (size < 32 || memcmp(d, "GPSZ", 4))
    return;
  memcpy(h, d, 32);
  if (!(h[5] & 2) && h[4] <= size - 32)
  {
    uLongf raw_len = h[3];
    unsigned char *raw = malloc(h[3]);
    if (raw && uncompress(raw, &raw_len, d + 32, h[4]) == Z_OK && raw_len == h[3])
    {
      char what[300];
      snprintf(what, sizeof(what), "%s: its state", path);
      check(what, raw, raw_len);
    }
    free(raw);
  }
  /* the picture: w, h (u16), its zlib size (u32), the pixels */
  if ((h[5] & 4) && h[6] >= 8 && (size_t)h[4] + 32 + h[6] <= size)
  {
    const unsigned char *pic = d + 32 + h[4];
    uint16_t w = (uint16_t)(pic[0] | pic[1] << 8), hh = (uint16_t)(pic[2] | pic[3] << 8);
    uint32_t packed = pic[4] | pic[5] << 8 | pic[6] << 16 | (uint32_t)pic[7] << 24;
    uLongf raw_len = (uLongf)w * hh * 2;
    unsigned char *raw = malloc(raw_len ? raw_len : 1);
    if (raw && packed + 8 <= h[6] && uncompress(raw, &raw_len, pic + 8, packed) == Z_OK &&
        raw_len == (uLongf)w * hh * 2)
    {
      char what[300];
      snprintf(what, sizeof(what), "%s: its picture", path);
      check(what, raw, raw_len);
    }
    free(raw);
  }
}

int main(int argc, char **argv)
{
  static const size_t sizes[] = { 0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 17, 31, 63, 64, 65, 255, 256, 257, 1000, 4095,
                                  4096, 4097, 5552, 5553, 27648, 32767, 32768, 32769, 65534, 65535, 65536, 65537,
                                  70000, 131071, 200000, 425984 };
  /* fastdeflate, a state's picture's rows, then other rows on the inputs
   * up to 70000 bytes */
  static const unsigned rows[] = { 0, 288, 1, 2, 3, 289, 32768, 32769 };
  size_t si, n;
  int kind, i, ri;

  for (ri = 0; ri < (int)(sizeof(rows) / sizeof(rows[0])); ri++)
  {
    int all = ri < 2;
    row = rows[ri];
    for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]) && (all || sizes[si] <= 70000); si++)
      for (kind = 0; kind < 9; kind++)
      {
        unsigned char *b = malloc((n = sizes[si]) ? n : 1);
        static const char *kinds[] = { "zeros", "0xFF", "random", "4 values", "runs and noise", "abc",
                                       "runs of random", "copies 40000 back", "16-bit steps" };
        if (!b)
          return 2;
        fill(b, n, kind);
        check(kinds[kind], b, n);
        free(b);
      }
    /* every alignment of the input against words */
    for (i = 1; i < 8; i++)
    {
      unsigned char *b = malloc(70000 + i);
      if (!b)
        return 2;
      fill(b, 70000 + i, 6);
      check("runs of random, unaligned", b + i, 70000);
      fill(b, 70000 + i, 8);
      check("16-bit steps, unaligned", b + i, 70000);
      free(b);
    }
    for (i = 1; all && i < argc; i++)
    {
      FILE *f = fopen(argv[i], "rb");
      long size;
      unsigned char *d;
      if (!f || fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0 ||
          !(d = malloc((size_t)size)) || fread(d, 1, (size_t)size, f) != (size_t)size)
      {
        printf("FAIL  can't read %s\n", argv[i]);
        failed++;
        if (f)
          fclose(f);
        continue;
      }
      fclose(f);
      check(argv[i], d, (size_t)size);
      check_state_file(argv[i], d, (size_t)size);
      free(d);
    }
  }
  printf("%s: fastdeflate: %d of %d inputs packed and unpacked the same, within the bound\n",
         failed ? "FAIL" : "PASS", tests - failed, tests);
  return failed != 0;
}
