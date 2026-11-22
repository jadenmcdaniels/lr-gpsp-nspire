/* A fast zlib stream from a buffer in memory: see nspire_fastdeflate.h.
 *
 * Matches: one pass over the input where it is (distances up to 32 KB, as
 * deflate allows). A table of 1024 hashes of three bytes keeps the last two
 * positions of each (16-bit offsets) with their first two bytes, so most
 * positions that can't match are passed over without reading them (each is
 * a cache miss); every match is checked byte by byte, so a stale entry only
 * costs time. Runs of one byte are found without the table. A match of
 * fewer than 32 bytes is kept only if the next position has no longer one
 * (one step of zlib's lazy matching), and a match of 3 bytes only if it's
 * at most 4 KB back (zlib's TOO_FAR): fewer, longer matches, which also
 * unpack faster. The table (8 KB) and the input's next 4 KB, whose Adler-32
 * is summed just before, are what the 8 KB data cache works on.
 *
 * Pictures (fastdeflate_picture): the pixels one row up and one pixel back
 * are tried before the table, the table has 8192 hashes, the lazy step is
 * taken below 8 bytes, and the positions inside a match of up to 4 bytes
 * go into the table too (as at zlib level 1): smaller than zlib 1's, which
 * fastdeflate's table alone wasn't (Emerald's picture 12,847 bytes against
 * 12,303).
 *
 * Blocks: the symbols of up to FASTDEFLATE_BLOCK_SYMBOLS are kept, then
 * the block is written stored, with the fixed codes or with its own
 * (Huffman, at most 15 bits; 7 for the code lengths' code), whichever
 * comes out smallest, counted to the bit. If the whole stream would be
 * bigger than the input stored, it is written stored.
 *
 * Measured (tests/libbench.sh, ARM926 model, CX II / older CX): the
 * 425,984-byte Emerald state packs in 41.6 / 93.0 ms (zlib level 1: 130.4
 * / 266.0), to 46,915 bytes (49,339), and unpacks in 26.6 ms instead of
 * 27.3 (CX II); its 144x96 picture is made and packed in 22.0 / 43.8 ms
 * (27.9 / 51.1), to 12,167 bytes (12,303). The pictures of five games
 * (Emerald, Advance Wars, Fire Emblem, Golden Sun, Minish Cap) all pack
 * 136-551 bytes smaller than at zlib 1, 1.7-7.4 ms faster, and unpack
 * 0.03-0.15 ms faster (CX II). */

#ifdef NSPIRE_ALLOC_HOOK
int nspire_fastdeflate_fail_alloc;   /* tests: the work area's malloc "fails" */
#endif
#include "nspire_fastdeflate.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef FASTDEFLATE_HASH_BITS
#define FASTDEFLATE_HASH_BITS 10
#endif
#ifndef FASTDEFLATE_BLOCK_SYMBOLS
#define FASTDEFLATE_BLOCK_SYMBOLS 16383
#endif
#define HASH_SIZE   (1u << FASTDEFLATE_HASH_BITS)
#define WAYS        2       /* positions per hash */
#define WINDOW      32768u
#define MAX_MATCH   258u
#define LAZY_MAX    32u     /* a match this long is taken as it is */
#define TOO_FAR     4096u   /* a 3-byte match further back is left out */
/* Pictures (fastdeflate_picture): a bigger table (64 KB, after struct fd),
 * the lazy step only below 8 bytes, and the positions inside a match of up
 * to 4 bytes go into the table too (as zlib level 1 does) */
#define PICTURE_HASH_BITS  13
#define PICTURE_LAZY_MAX   8u
#define PICTURE_INSERT_MAX 4u
#define ADLER_CHUNK 4096u
#define ADLER_MOD   65521u
/* Adler-32's sums fit in 32 bits for this many bytes at a time. */
#define ADLER_NMAX  5552u
#define STORED_MAX  65535u

#define L_CODES 286   /* literals, end of block, lengths */
#define D_CODES 30
#define C_CODES 19    /* the code lengths' code */
#define END_BLOCK 256

/* Words of bytes read as numbers: the first byte is the lowest. */
#define LITTLE_ENDIAN_WORDS (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)

/* GCC: words read from byte buffers. */
typedef uint32_t __attribute__((may_alias)) word_t;

struct adler
{
  uint32_t a, b;
  size_t done;                       /* the input's bytes summed */
};

struct writer
{
  uint8_t *out, *out_end;
  uint32_t bits;                     /* bit_count of them not written yet */
  unsigned bit_count;
};

/* a symbol and its frequency, for build_lengths */
struct sym_freq
{
  uint32_t key;
  uint16_t sym;
};

struct fd
{
  /* per hash, the newest first: an offset (16 bits) | its first two
   * bytes << 16 */
  uint32_t head[HASH_SIZE * WAYS];
  uint16_t lit_freq[L_CODES + 2];
  uint16_t dist_freq[D_CODES + 2];
  /* a literal (< 256), or (distance << 8) | (length - 3) */
  uint32_t syms[FASTDEFLATE_BLOCK_SYMBOLS];
  /* build_lengths' two sort buffers (on the heap: not 4.6 KB of stack) */
  struct sym_freq sort[2][L_CODES + 2];
  struct adler sum;
  struct writer w;
  unsigned nsyms;                    /* the block's symbols so far */
  size_t block_start;                /* its first byte */
  int in_block;
};

/* ---- Tables, made once ---- */

static uint8_t length_code[256];     /* length - 3 -> its code - 257 */
static uint8_t dist_code[512];       /* see DIST_CODE */
static uint16_t length_base[29], dist_base[30];
static const uint8_t length_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                          3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const uint8_t code_order[C_CODES] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
static uint16_t fixed_lcode[L_CODES + 2], fixed_dcode[D_CODES];
static uint8_t fixed_llen[L_CODES + 2], fixed_dlen[D_CODES];
static int tables_made;

/* The code of distance - 1 (zlib's _dist_code). */
#define DIST_CODE(dm1) ((dm1) < 256 ? dist_code[dm1] : dist_code[256 + ((dm1) >> 7)])

static unsigned reverse(unsigned code, unsigned len)
{
  unsigned r = 0;
  while (len--)
  {
    r = (r << 1) | (code & 1);
    code >>= 1;
  }
  return r;
}

/* Canonical codes for lengths, bit-reversed (deflate sends codes from their
 * top bit, the bit writer from the bottom). */
static void make_codes(const uint8_t *lens, int n, uint16_t *codes)
{
  unsigned count[16] = { 0 }, next[16], code = 0;
  int i;

  for (i = 0; i < n; i++)
    count[lens[i]]++;
  count[0] = 0;
  for (i = 1; i < 16; i++)
  {
    code = (code + count[i - 1]) << 1;
    next[i] = code;
  }
  for (i = 0; i < n; i++)
    codes[i] = lens[i] ? (uint16_t)reverse(next[lens[i]]++, lens[i]) : 0;
}

static void make_tables(void)
{
  unsigned code, i, base;

  for (code = 0, base = 0; code < 28; code++)
  {
    length_base[code] = (uint16_t)base;
    for (i = 0; i < (1u << length_extra[code]); i++)
      length_code[base++] = (uint8_t)code;
  }
  /* 258 has its own code; 227 + 31 = 258 would be code 27's too */
  length_code[255] = 28;
  length_base[28] = 255;
  for (code = 0, base = 0; code < 16; code++)
  {
    dist_base[code] = (uint16_t)base;
    for (i = 0; i < (1u << dist_extra[code]); i++)
      dist_code[base++] = (uint8_t)code;
  }
  for (base >>= 7; code < D_CODES; code++)
  {
    dist_base[code] = (uint16_t)(base << 7);
    for (i = 0; i < (1u << (dist_extra[code] - 7)); i++)
      dist_code[256 + base++] = (uint8_t)code;
  }
  for (i = 0; i < L_CODES + 2; i++)
    fixed_llen[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
  make_codes(fixed_llen, L_CODES + 2, fixed_lcode);
  for (i = 0; i < D_CODES; i++)
    fixed_dlen[i] = 5;
  make_codes(fixed_dlen, D_CODES, fixed_dcode);
  tables_made = 1;
}

/* ---- Adler-32 ---- */

static void adler_to(struct adler *s, const uint8_t *src, size_t to)
{
  uint32_t a = s->a, b = s->b;
  const uint8_t *p = src + s->done, *end = src + to;

  while (p < end)
  {
    size_t n = (size_t)(end - p) < ADLER_NMAX ? (size_t)(end - p) : ADLER_NMAX;
    const uint8_t *stop = p + n;
#if LITTLE_ENDIAN_WORDS
    for (; p < stop && ((uintptr_t)p & 3); p++)
    {
      a += *p;
      b += a;
    }
    /* 16 words at a time, two bytes of each in the halves of e and o
     * (bytes 0 and 2, 1 and 3), and their running sums in qe and qo: no
     * half gets past 16 bits (255 * (1 + 2 + ... + 16) = 34,680) */
    while (stop - p >= 64)
    {
      const word_t *w = (const word_t *)p;
      uint32_t e = 0, o = 0, qe = 0, qo = 0, sum, run;
      int j;
      for (j = 0; j < 16; j++)
      {
        uint32_t x = w[j];
        e += x & 0x00FF00FFu;
        o += (x >> 8) & 0x00FF00FFu;
        qe += e;
        qo += o;
      }
      sum = (e & 0xFFFF) + (e >> 16) + (o & 0xFFFF) + (o >> 16);
      run = (qe & 0xFFFF) + (qe >> 16) + (qo & 0xFFFF) + (qo >> 16);
      /* each byte goes into b 64 - (its place) times: 4 times for each
       * word from its own on, less its place in the word */
      b += 64 * a + 4 * run - ((o & 0xFFFF) + 2 * (e >> 16) + 3 * (o >> 16));
      a += sum;
      p += 64;
    }
#endif
    for (; p + 8 <= stop; p += 8)
    {
      a += p[0]; b += a;
      a += p[1]; b += a;
      a += p[2]; b += a;
      a += p[3]; b += a;
      a += p[4]; b += a;
      a += p[5]; b += a;
      a += p[6]; b += a;
      a += p[7]; b += a;
    }
    for (; p < stop; p++)
    {
      a += *p;
      b += a;
    }
    a %= ADLER_MOD;
    b %= ADLER_MOD;
  }
  s->a = a;
  s->b = b;
  s->done = to;
}

/* ---- Matches ---- */

/* The length of the match of p at c (p > c), whose first 3 bytes match,
 * up to max. */
static inline unsigned match_length(const uint8_t *p, const uint8_t *c, unsigned max)
{
  unsigned len = 3;

  if (!((p - c) & 3))
  {
    /* the same alignment: words */
    while (len < max && ((uintptr_t)(p + len) & 3))
    {
      if (p[len] != c[len])
        return len;
      len++;
    }
#if LITTLE_ENDIAN_WORDS
    while (len + 4 <= max)
    {
      /* the first byte that differs is the lowest of x that isn't 0 */
      uint32_t x = *(const word_t *)(p + len) ^ *(const word_t *)(c + len);
      if (x)
        return len + (__builtin_ctz(x) >> 3);
      len += 4;
    }
#else
    while (len + 4 <= max && *(const word_t *)(p + len) == *(const word_t *)(c + len))
      len += 4;
#endif
  }
  while (len < max && p[len] == c[len])
    len++;
  return len;
}

/* The length of the run of byte b from p (whose first 3 bytes are b), up
 * to max. */
static inline unsigned run_length(const uint8_t *p, unsigned b, unsigned max)
{
  uint32_t pattern = b * 0x01010101u;
  unsigned len = 3;

  while (len < max && ((uintptr_t)(p + len) & 3))
  {
    if (p[len] != b)
      return len;
    len++;
  }
#if LITTLE_ENDIAN_WORDS
  while (len + 4 <= max)
  {
    uint32_t x = *(const word_t *)(p + len) ^ pattern;
    if (x)
      return len + (__builtin_ctz(x) >> 3);
    len += 4;
  }
#else
  while (len + 4 <= max && *(const word_t *)(p + len) == pattern)
    len += 4;
#endif
  while (len < max && p[len] == b)
    len++;
  return len;
}

/* The bucket of the 3 bytes in v (their hash, in a table of 1 << hash_bits
 * hashes). */
#define HASH_OF(head, v, hash_bits) ((head) + (((v) * 2654435761u) >> (32 - (hash_bits))) * WAYS)

/* The longest match at p (off bytes into the input, 3 or more before its
 * end), of at most max bytes: its length and its distance (*dist), or 0 if
 * none is worth it. p goes into the table (of 1 << hash_bits hashes). With
 * row (pictures: the bytes of a row), the pixels one row up and one pixel
 * (2 bytes) back are tried first, without the table. */
static inline __attribute__((always_inline)) unsigned find(uint32_t *head, const uint8_t *p, uint32_t off,
                                                           unsigned max, uint32_t *dist,
                                                           const unsigned hash_bits, unsigned row)
{
  uint32_t v = p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16), tag = v & 0xFFFF;
  uint32_t *b = HASH_OF(head, v, hash_bits);
  unsigned best = 2, w;

  if (off && v == p[-1] * 0x010101u)
  {
    /* a run of one byte: taken as the match, without the table */
    *dist = 1;
    return run_length(p, p[0], max);
  }
  if (row && off >= row)
  {
    const uint8_t *c = p - row;
    if (c[0] == p[0] && c[1] == p[1] && c[2] == p[2])
    {
      best = match_length(p, c, max);
      *dist = row;
    }
  }
  if (row && off >= 2 && best < max)
  {
    const uint8_t *c = p - 2;
    if (c[best] == p[best] && c[0] == p[0] && c[1] == p[1] && c[2] == p[2])
    {
      unsigned len = match_length(p, c, max);
      if (len > best)
      {
        best = len;
        *dist = 2;
      }
    }
  }
  for (w = 0; w < WAYS && (!row || best < max); w++)
  {
    uint32_t e = b[w];
    if ((e >> 16) == tag)
    {
      /* never before the input (the table starts with 0x7FFF: see
       * fastdeflate), so only the window is checked */
      uint32_t d = (uint16_t)(off - e);
      if (d - 1 < WINDOW)
      {
        const uint8_t *c = p - d;
        /* (c[best] first: only a longer match is of use) */
        if (c[best] == p[best] && c[0] == p[0] && c[1] == p[1] && c[2] == p[2])
        {
          unsigned len = match_length(p, c, max);
          if (len > best)
          {
            best = len;
            *dist = d;
            if (best >= max)
              break;
          }
        }
      }
    }
  }
  b[1] = b[0];
  b[0] = (uint16_t)off | (tag << 16);
  if (best < 3 || (best == 3 && *dist > TOO_FAR))
    return 0;
  return best;
}

/* The symbols from *pos until the block is full or the input ends; their
 * counts in lit_freq and dist_freq, s->nsyms of them so far in the block.
 * Returns 1 when it stopped at stop (an offset) with neither (the block goes
 * on at the next call: fastdeflate_step; where it stops changes nothing of
 * what it makes), else 0. head, hash_bits and row: see find; a match
 * shorter than lazy_max is kept only if the next position has no longer
 * one; the positions inside a match of up to insert_max bytes go into the
 * table too. */
static inline __attribute__((always_inline)) int parse(struct fd *s, uint32_t *head, const uint8_t *src,
                                                       size_t *pos, size_t n, size_t stop, const unsigned hash_bits,
                                                       unsigned row, const unsigned lazy_max,
                                                       const unsigned insert_max)
{
  uint16_t *lf = s->lit_freq, *df = s->dist_freq;
  /* (room for a literal and a match after the lazy step) */
  uint32_t *sym = s->syms + s->nsyms, *sym_end = s->syms + FASTDEFLATE_BLOCK_SYMBOLS - 1;
  const uint8_t *p = src + *pos, *end = src + n;
  /* the last position with 3 bytes to hash */
  const uint8_t *hash_end = n >= 3 ? end - 2 : src;
  int paused = 0;

  while (sym < sym_end)
  {
    const uint8_t *summed = src + s->sum.done, *lim;

    if (p >= summed)
    {
      size_t to;
      if (p >= end)
        break;
      if ((size_t)(p - src) >= stop)
      {
        paused = 1;
        break;
      }
      /* the next 4 KB summed now, and then worked on while in the cache */
      to = (size_t)(p - src) + ADLER_CHUNK;
      adler_to(&s->sum, src, to < n ? to : n);
      continue;
    }
    lim = summed < hash_end ? summed : hash_end;
    if (p >= lim)
    {
      /* the last two bytes */
      *sym++ = *p;
      lf[*p++]++;
      continue;
    }
    while (p < lim && sym < sym_end)
    {
      uint32_t d = 0;
      unsigned max = (unsigned)(end - p) < MAX_MATCH ? (unsigned)(end - p) : MAX_MATCH;
      unsigned len = find(head, p, (uint32_t)(p - src), max, &d, hash_bits, row), looked = 0;

      if (!len)
      {
        *sym++ = *p;
        lf[*p++]++;
        continue;
      }
      /* a longer match one byte on: this byte as a literal, then that */
      while (len < lazy_max && p + 1 < hash_end && sym + 1 < sym_end)
      {
        uint32_t d1 = 0;
        unsigned max1 = (unsigned)(end - p - 1) < MAX_MATCH ? (unsigned)(end - p - 1) : MAX_MATCH;
        unsigned len1 = find(head, p + 1, (uint32_t)(p + 1 - src), max1, &d1, hash_bits, row);
        if (len1 <= len)
        {
          looked = 1;   /* (p + 1 is in the table) */
          break;
        }
        *sym++ = *p;
        lf[*p++]++;
        len = len1;
        d = d1;
      }
      *sym++ = (d << 8) | (len - 3);
      lf[257 + length_code[len - 3]]++;
      df[DIST_CODE(d - 1)]++;
      if (insert_max && len <= insert_max)
      {
        const uint8_t *q = p + 1 + looked, *q_end = p + len < hash_end ? p + len : hash_end;
        for (; q < q_end; q++)
        {
          uint32_t vq = q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16);
          uint32_t *bq = HASH_OF(head, vq, hash_bits);
          bq[1] = bq[0];
          bq[0] = (uint16_t)(q - src) | ((vq & 0xFFFF) << 16);
        }
      }
      p += len;
    }
  }
  *pos = (size_t)(p - src);
  s->nsyms = (unsigned)(sym - s->syms);
  return paused;
}

/* ---- Huffman codes ---- */

/* build_lengths below (the sort, the minimum-redundancy lengths and the
 * length limiter) is adapted from miniz 3.1.2's tdefl_radix_sort_syms,
 * tdefl_calculate_minimum_redundancy and tdefl_huffman_enforce_max_code_size
 * (miniz.c, https://github.com/richgel999/miniz), under this licence:
 *
 * Copyright 2013-2014 RAD Game Tools and Valve Software
 * Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/* Code lengths of at most max_len for freq (n symbols; at least two get a
 * code: inflate wants complete codes). Three times a block: not inlined
 * (6 KB of program). */
static __attribute__((noinline, noclone)) void build_lengths(struct sym_freq (*sort)[L_CODES + 2],
                                                                      const uint16_t *restrict freq, int n,
                                                                      int max_len, uint8_t *restrict lens)
{
  struct sym_freq *a = sort[0], *b = sort[1], *in, *out, *t;
  unsigned hist[256];
  unsigned num[33];
  int count = 0, i, pass, root, leaf, next, avail, used, depth;
  uint32_t total;

  for (i = 0; i < n; i++)
  {
    lens[i] = 0;
    if (freq[i])
    {
      a[count].key = freq[i];
      a[count++].sym = (uint16_t)i;
    }
  }
  for (i = 0; count < 2; i++)
    if (!freq[i])
    {
      a[count].key = 1;
      a[count++].sym = (uint16_t)i;
    }

  /* sorted by frequency (radix, two bytes) */
  in = a;
  out = b;
  for (pass = 0; pass < 16; pass += 8)
  {
    unsigned sum = 0;
    memset(hist, 0, sizeof(hist));
    for (i = 0; i < count; i++)
      hist[(in[i].key >> pass) & 255]++;
    for (i = 0; i < 256; i++)
    {
      unsigned c = hist[i];
      hist[i] = sum;
      sum += c;
    }
    for (i = 0; i < count; i++)
      out[hist[(in[i].key >> pass) & 255]++] = in[i];
    t = in;
    in = out;
    out = t;
  }

  /* the optimal lengths, in place (Moffat and Katajainen) */
  in[0].key += in[1].key;
  root = 0;
  leaf = 2;
  for (next = 1; next < count - 1; next++)
  {
    if (leaf >= count || in[root].key < in[leaf].key)
    {
      in[next].key = in[root].key;
      in[root++].key = (uint32_t)next;
    }
    else
      in[next].key = in[leaf++].key;
    if (leaf >= count || (root < next && in[root].key < in[leaf].key))
    {
      in[next].key += in[root].key;
      in[root++].key = (uint32_t)next;
    }
    else
      in[next].key += in[leaf++].key;
  }
  in[count - 2].key = 0;
  for (next = count - 3; next >= 0; next--)
    in[next].key = in[in[next].key].key + 1;
  avail = 1;
  used = depth = 0;
  root = count - 2;
  next = count - 1;
  while (avail > 0)
  {
    while (root >= 0 && (int)in[root].key == depth)
    {
      used++;
      root--;
    }
    while (avail > used)
    {
      in[next--].key = (uint32_t)depth;
      avail--;
    }
    avail = 2 * used;
    depth++;
    used = 0;
  }

  /* at most max_len bits: the longer ones moved up, the code kept complete */
  memset(num, 0, sizeof(num));
  for (i = 0; i < count; i++)
    num[in[i].key]++;
  for (i = max_len + 1; i <= 32; i++)
    num[max_len] += num[i];
  for (i = max_len, total = 0; i > 0; i--)
    total += num[i] << (max_len - i);
  while (total != (1u << max_len))
  {
    num[max_len]--;
    for (i = max_len - 1; i > 0; i--)
      if (num[i])
      {
        num[i]--;
        num[i + 1] += 2;
        break;
      }
    total--;
  }
  /* the shortest to the most frequent */
  for (i = 1, next = count; i <= max_len; i++)
  {
    unsigned k;
    for (k = num[i]; k > 0; k--)
      lens[in[--next].sym] = (uint8_t)i;
  }
}

/* ---- Output ---- */

#define PUT(value, len)                                   \
  do                                                      \
  {                                                       \
    bits |= (uint32_t)(value) << bit_count;               \
    bit_count += (len);                                   \
    if (bit_count >= 16)                                  \
    {                                                     \
      out[0] = (uint8_t)bits;                             \
      out[1] = (uint8_t)(bits >> 8);                      \
      out += 2;                                           \
      bits >>= 16;                                        \
      bit_count -= 16;                                    \
    }                                                     \
  } while (0)

/* The symbols with these codes. */
static void put_symbols(struct writer *w, const uint32_t *sym, unsigned nsyms, const uint16_t *lcode,
                        const uint8_t *llen, const uint16_t *dcode, const uint8_t *dlen)
{
  uint8_t *out = w->out;
  uint32_t bits = w->bits;
  unsigned bit_count = w->bit_count, i;

  for (i = 0; i < nsyms; i++)
  {
    uint32_t v = sym[i];
    if (v < 256)
      PUT(lcode[v], llen[v]);
    else
    {
      unsigned l = v & 255, dm1 = (v >> 8) - 1, lc = length_code[l], dc = DIST_CODE(dm1);
      PUT(lcode[257 + lc], llen[257 + lc]);
      if (length_extra[lc])
        PUT(l - length_base[lc], length_extra[lc]);
      PUT(dcode[dc], dlen[dc]);
      if (dist_extra[dc])
        PUT(dm1 - dist_base[dc], dist_extra[dc]);
    }
  }
  PUT(lcode[END_BLOCK], llen[END_BLOCK]);
  w->out = out;
  w->bits = bits;
  w->bit_count = bit_count;
}

/* To a byte boundary. */
static void put_align(struct writer *w)
{
  while (w->bit_count > 0)
  {
    *w->out++ = (uint8_t)w->bits;
    w->bits >>= 8;
    w->bit_count = w->bit_count > 8 ? w->bit_count - 8 : 0;
  }
  w->bits = 0;
}

static void put_bits(struct writer *w, uint32_t value, unsigned len)
{
  uint8_t *out = w->out;
  uint32_t bits = w->bits;
  unsigned bit_count = w->bit_count;
  PUT(value, len);
  w->out = out;
  w->bits = bits;
  w->bit_count = bit_count;
}

/* in (len bytes) as stored blocks; last: the stream's last. */
static void put_stored(struct writer *w, const uint8_t *in, size_t len, int last)
{
  do
  {
    unsigned piece = len < STORED_MAX ? (unsigned)len : STORED_MAX;
    len -= piece;
    put_bits(w, last && !len, 3);
    put_align(w);
    w->out[0] = (uint8_t)piece;
    w->out[1] = (uint8_t)(piece >> 8);
    w->out[2] = (uint8_t)~piece;
    w->out[3] = (uint8_t)(~piece >> 8);
    memcpy(w->out + 4, in, piece);
    w->out += 4 + piece;
    in += piece;
  } while (len);
}

/* Its bytes: the zlib header, len bytes stored, the Adler-32. */
static size_t stored_size(size_t len)
{
  return 2 + (len ? (len + STORED_MAX - 1) / STORED_MAX : 1) * 5 + len + 4;
}

/* The bits of in (len bytes) stored, from bit_count. */
static size_t stored_bits(size_t len, unsigned bit_count)
{
  size_t pieces = len ? (len + STORED_MAX - 1) / STORED_MAX : 1;
  return (size_t)(3 + (8 - (bit_count + 3) % 8) % 8 + 32) + (pieces - 1) * 40 + len * 8;
}

/* The block parsed from in (len bytes, nsyms symbols): stored, fixed or
 * dynamic, the smallest. 0 if it doesn't fit. */
static int put_block(struct fd *s, const uint8_t *in, size_t len, unsigned nsyms, int last)
{
  struct writer *w = &s->w;
  uint8_t llen[L_CODES + 2], dlen[D_CODES + 2], clen[C_CODES], lens[L_CODES + D_CODES], rle[L_CODES + D_CODES];
  uint8_t rle_extra[L_CODES + D_CODES];
  uint16_t lcode[L_CODES + 2], dcode[D_CODES + 2], ccode[C_CODES], cfreq[C_CODES];
  uint16_t *lf = s->lit_freq, *df = s->dist_freq;
  size_t extra = 0, fixed = 3, dynamic, stored, room;
  int hlit, hdist, hclen, total, i, nrle = 0;

  lf[END_BLOCK] = 1;
  for (i = 0; i < 29; i++)
    extra += (size_t)lf[257 + i] * length_extra[i];
  for (i = 0; i < D_CODES; i++)
    extra += (size_t)df[i] * dist_extra[i];

  build_lengths(s->sort, lf, L_CODES, 15, llen);
  build_lengths(s->sort, df, D_CODES, 15, dlen);
  for (hlit = L_CODES; hlit > 257 && !llen[hlit - 1]; hlit--)
    ;
  for (hdist = D_CODES; hdist > 1 && !dlen[hdist - 1]; hdist--)
    ;
  memcpy(lens, llen, hlit);
  memcpy(lens + hlit, dlen, hdist);
  total = hlit + hdist;

  /* the lengths run-length coded (16: the last 3-6 times, 17: 3-10 zeros,
   * 18: 11-138 zeros) */
  memset(cfreq, 0, sizeof(cfreq));
  for (i = 0; i < total;)
  {
    int v = lens[i], run = 1;
    while (i + run < total && lens[i + run] == v)
      run++;
    i += run;
    if (!v)
      while (run > 0)
      {
        if (run < 3)
        {
          cfreq[0] += run;
          while (run--)
            rle[nrle++] = 0;
          break;
        }
        else
        {
          int k = run > 138 ? 138 : run;
          rle[nrle] = k < 11 ? 17 : 18;
          rle_extra[nrle] = (uint8_t)(k < 11 ? k - 3 : k - 11);
          cfreq[rle[nrle++]]++;
          run -= k;
        }
      }
    else
    {
      rle[nrle++] = (uint8_t)v;
      cfreq[v]++;
      run--;
      while (run > 0)
      {
        if (run < 3)
        {
          cfreq[v] += run;
          while (run--)
            rle[nrle++] = (uint8_t)v;
          break;
        }
        else
        {
          int k = run > 6 ? 6 : run;
          rle[nrle] = 16;
          rle_extra[nrle++] = (uint8_t)(k - 3);
          cfreq[16]++;
          run -= k;
        }
      }
    }
  }
  build_lengths(s->sort, cfreq, C_CODES, 7, clen);
  for (hclen = C_CODES; hclen > 4 && !clen[code_order[hclen - 1]]; hclen--)
    ;

  dynamic = 3 + 5 + 5 + 4 + 3 * hclen + extra + 2 * cfreq[16] + 3 * cfreq[17] + 7 * cfreq[18];
  for (i = 0; i < C_CODES; i++)
    dynamic += (size_t)cfreq[i] * clen[i];
  for (i = 0; i < L_CODES; i++)
  {
    dynamic += (size_t)lf[i] * llen[i];
    fixed += (size_t)lf[i] * fixed_llen[i];
  }
  for (i = 0; i < D_CODES; i++)
  {
    dynamic += (size_t)df[i] * dlen[i];
    fixed += (size_t)df[i] * 5;
  }
  fixed += extra;
  stored = stored_bits(len, w->bit_count);

  /* the bytes it takes, and the Adler-32 after the last */
  room = (size_t)(w->out_end - w->out);
  if (room < 4)
    return 0;
  room -= 4;
  if (stored <= fixed && stored <= dynamic)
  {
    if ((w->bit_count + stored + 7) / 8 > room)
      return 0;
    put_stored(w, in, len, last);
  }
  else if (fixed <= dynamic)
  {
    if ((w->bit_count + fixed + 7) / 8 > room)
      return 0;
    put_bits(w, last | (1 << 1), 3);
    put_symbols(w, s->syms, nsyms, fixed_lcode, fixed_llen, fixed_dcode, fixed_dlen);
  }
  else
  {
    if ((w->bit_count + dynamic + 7) / 8 > room)
      return 0;
    make_codes(llen, hlit, lcode);
    make_codes(dlen, hdist, dcode);
    make_codes(clen, C_CODES, ccode);
    put_bits(w, last | (2 << 1), 3);
    put_bits(w, hlit - 257, 5);
    put_bits(w, hdist - 1, 5);
    put_bits(w, hclen - 4, 4);
    for (i = 0; i < hclen; i++)
      put_bits(w, clen[code_order[i]], 3);
    for (i = 0; i < nrle; i++)
    {
      put_bits(w, ccode[rle[i]], clen[rle[i]]);
      if (rle[i] >= 16)
        put_bits(w, rle_extra[i], rle[i] == 16 ? 2 : rle[i] == 17 ? 3 : 7);
    }
    put_symbols(w, s->syms, nsyms, lcode, llen, dcode, dlen);
  }
  return 1;
}

/* ---- The stream ---- */

static void put_adler(struct writer *w, const struct adler *sum)
{
  uint32_t adler = (sum->b << 16) | sum->a;
  put_align(w);
  w->out[0] = (uint8_t)(adler >> 24);
  w->out[1] = (uint8_t)(adler >> 16);
  w->out[2] = (uint8_t)(adler >> 8);
  w->out[3] = (uint8_t)adler;
  w->out += 4;
}

/* save17: the work area given (fastdeflate_give_work), taken by one packing
 * at a time instead of a malloc of its own */
static void *given_work;
static size_t given_size;
static int given_taken;

size_t fastdeflate_work_size(void)
{
  return sizeof(struct fd) + ((size_t)WAYS << PICTURE_HASH_BITS) * sizeof(uint32_t);
}

void fastdeflate_give_work(void *area, size_t size)
{
  given_work = area;
  given_size = area ? size : 0;
  given_taken = 0;
}

static void work_free(void *work)
{
  if (work && work == given_work)
    given_taken = 0;
  else
    free(work);
}

/* fastdeflate and fastdeflate_picture: parse's settings as given; a table
 * bigger than s->head goes after the struct. In three parts (2026-11-17,
 * final/pace6), so that a state can be packed a block at a time over the
 * game's frames (fastdeflate_begin): the same blocks, the same bytes. */
static inline __attribute__((always_inline)) int pack_begin(struct fastdeflate_job *j, unsigned char *dst,
                                                            size_t cap, const unsigned char *src, size_t n,
                                                            const unsigned hash_bits)
{
  const int own_table = hash_bits > FASTDEFLATE_HASH_BITS;
  const size_t entries = (size_t)WAYS << hash_bits;
  struct fd *s;
  size_t i;

  if (!tables_made)
    make_tables();
  const size_t need = sizeof(*s) + (own_table ? entries * sizeof(uint32_t) : 0);
  memset(j, 0, sizeof(*j));
  j->dst = dst;
  j->cap = cap;
  j->src = src;
  j->n = n;
#ifdef NSPIRE_ALLOC_HOOK
  if (nspire_fastdeflate_fail_alloc)
    s = NULL;
  else
#endif
  if (given_work && !given_taken && need <= given_size)
  {
    s = (struct fd *)given_work;
    given_taken = 1;
  }
  else
    s = (struct fd *)malloc(need);
  /* No room for the work: 0, not the input stored (426 KB for a state);
   * the caller keeps what it had. */
  if (!s)
    return 0;
  j->work = s;
  j->more = cap >= 2 + 4;
  if (j->more)
  {
    uint32_t *head = own_table ? (uint32_t *)(s + 1) : s->head;
    /* an offset that is either over the window or at most the offset
     * from any offset: no match from it is ever before the input */
    for (i = 0; i < entries; i++)
      head[i] = 0x7FFF;
    s->sum.a = 1;
    s->sum.b = 0;
    s->sum.done = 0;
    s->w.out = dst + 2;
    s->w.out_end = dst + cap;
    s->w.bits = 0;
    s->w.bit_count = 0;
    s->in_block = 0;
    dst[0] = 0x78;   /* deflate, 32 KB window; "fastest" */
    dst[1] = 0x01;
  }
  return 1;
}

/* The input up to stop (or a little past it: parse) packed; 1 while more
 * is to come. */
static inline __attribute__((always_inline)) int pack_blocks(struct fastdeflate_job *j, size_t stop,
                                                             const unsigned hash_bits, unsigned row,
                                                             const unsigned lazy_max, const unsigned insert_max)
{
  struct fd *s = (struct fd *)j->work;
  uint32_t *head = hash_bits > FASTDEFLATE_HASH_BITS ? (uint32_t *)(s + 1) : s->head;

  while (j->more)
  {
    int last, ok;
    if (!s->in_block)
    {
      memset(s->lit_freq, 0, sizeof(s->lit_freq));
      memset(s->dist_freq, 0, sizeof(s->dist_freq));
      s->nsyms = 0;
      s->block_start = j->pos;
      s->in_block = 1;
    }
    if (parse(s, head, j->src, &j->pos, j->n, stop, hash_bits, row, lazy_max, insert_max))
      return 1;
    s->in_block = 0;
    last = j->pos == j->n;
    ok = put_block(s, j->src + s->block_start, j->pos - s->block_start, s->nsyms, last);
    if (ok && last)
    {
      /* what the last matches went over */
      adler_to(&s->sum, j->src, j->n);
      put_adler(&s->w, &s->sum);
      j->size = (size_t)(s->w.out - j->dst);
    }
    j->more = ok && !last;
  }
  return j->more;
}

static size_t pack_end(struct fastdeflate_job *j)
{
  struct writer stored;
  struct adler sum;
  size_t size = j->size, n = j->n;
  unsigned char *dst = j->dst;

  work_free(j->work);
  j->work = NULL;
  j->more = 0;
  /* never more than the input stored */
  if (size && size <= stored_size(n))
    return size;
  if (j->cap < stored_size(n))
    return 0;
  sum.a = 1;
  sum.b = 0;
  sum.done = 0;
  adler_to(&sum, j->src, n);
  stored.out = dst + 2;
  stored.bits = 0;
  stored.bit_count = 0;
  dst[0] = 0x78;
  dst[1] = 0x01;
  put_stored(&stored, j->src, n, 1);
  put_adler(&stored, &sum);
  return (size_t)(stored.out - dst);
}

/* (one copy of a state's parse for fastdeflate and its steps) */
static __attribute__((noinline)) int pack_state(struct fastdeflate_job *j, size_t stop)
{
  return pack_blocks(j, stop, FASTDEFLATE_HASH_BITS, 0, LAZY_MAX, 0);
}

size_t fastdeflate(unsigned char *dst, size_t cap, const unsigned char *src, size_t n)
{
  struct fastdeflate_job j;
  if (!pack_begin(&j, dst, cap, src, n, FASTDEFLATE_HASH_BITS))
    return 0;
  pack_state(&j, (size_t)-1);
  return pack_end(&j);
}

int fastdeflate_begin(struct fastdeflate_job *j, unsigned char *dst, size_t cap, const unsigned char *src, size_t n)
{
  return pack_begin(j, dst, cap, src, n, FASTDEFLATE_HASH_BITS);
}

int fastdeflate_step(struct fastdeflate_job *j, size_t bytes)
{
  return j->work && pack_state(j, bytes < (size_t)-1 - j->pos ? j->pos + bytes : (size_t)-1);
}

size_t fastdeflate_end(struct fastdeflate_job *j)
{
  if (!j->work)
    return 0;
  if (j->more)
  {
    work_free(j->work);   /* (given up before the last block) */
    j->work = NULL;
    j->more = 0;
    return 0;
  }
  return pack_end(j);
}

size_t fastdeflate_picture(unsigned char *dst, size_t cap, const unsigned char *src, size_t n, unsigned row)
{
  struct fastdeflate_job j;
  if (!pack_begin(&j, dst, cap, src, n, PICTURE_HASH_BITS))
    return 0;
  pack_blocks(&j, (size_t)-1, PICTURE_HASH_BITS, row - 1 < WINDOW ? row : 0, PICTURE_LAZY_MAX, PICTURE_INSERT_MAX);
  return pack_end(&j);
}
