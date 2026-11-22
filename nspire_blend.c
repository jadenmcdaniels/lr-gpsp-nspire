/*
 * The blended pictures on portrait LCDs (CX II), drawn in one pass: the GBA
 * frame is read once, scaled with blending, and written straight into the
 * turned screen buffer (landscape pixel (x, y) at position 239 - y of LCD
 * line x, as in nspire_turn.c). Interframe blending is done on the way, a few
 * source lines at a time.
 *
 * gpSP used to do this in four passes over memory: frame mixing, a copy into
 * the old 5:5:5 menu format, notaz's scaler (nspire/upscale_aspect.s) into a
 * landscape buffer, and turning that back into 5:6:5. Each pass read a whole
 * picture back from memory (about 120 CPU cycles for every 32 bytes, as the
 * data cache is only 8 KB), about 10 ms a frame in all.
 *
 * The math is notaz's, in the LCD's 5:6:5: red and blue come out exactly as
 * before, green keeps its sixth bit (the old path dropped it).
 *   3:2 (240x160 -> 320x213): each 3x3 block of source pixels becomes 4x4.
 *     Along each direction the four are: the first source pixel, 3:5 of the
 *     first and the middle one, 5:3 of the middle and the last one, the last
 *     one. Where both directions are blended: 14:7:7:4 of the middle pixel,
 *     its two neighbours toward that corner, and the corner pixel. The last
 *     source line is only blended across.
 *   fullscreen (240x160 -> 320x240): across, each 3 pixels become a b (b+c)/2
 *     c; down, each 2 lines become a (a+b)/2 b (rounded down).
 */

#include "common.h"
#include "nspire.h"
#include "nspire_frontend.h"

#define DST_W 240   /* pixels per LCD line */
#define SRC_W GBA_SCREEN_PITCH

/* Pixels "unpacked" with room to add them up: 000000gg gggg0000 rrrrr000 00bbbbb
 * (green in bits 21-26, red 11-15, blue 0-4); sums of up to 32 of them fit. */
#define UNPACKED_MASK 0x07E0F81Fu
#define UNPACKED_ONES 0x00200801u   /* 1 in each color */

static inline u32 unpack(u32 p)
{
  return (p | (p << 16)) & UNPACKED_MASK;
}

/* (3 a + 5 b) / 8, rounded (notaz's do_3_5) */
static inline u32 blend_3_5(u32 a, u32 b)
{
  return ((a + (a << 1) + b + (b << 2) + (UNPACKED_ONES << 2)) >> 3) & UNPACKED_MASK;
}

/* (14 c + 7 (h + v) + 4 d) / 32, rounded (notaz's do_14_7_7_4) */
static inline u32 blend_14_7_7_4(u32 c, u32 h, u32 v, u32 d)
{
  u32 s = (c << 1) + h + v;
  return (((s << 3) - s + (d << 2) + (UNPACKED_ONES << 4)) >> 5) & UNPACKED_MASK;
}

/* The word holding unpacked pixels lo (the lower address) and hi. */
static inline u32 pack_pair(u32 lo, u32 hi)
{
  return (((lo | (lo >> 16)) << 16) >> 16) | ((hi | (hi << 16)) & 0xFFFF0000u);
}

/* (a + b) / 2 for each color of two 5:6:5 pixels a word, rounded down */
static inline u32 average_down(u32 a, u32 b)
{
  return (a & b) + (((a ^ b) >> 1) & 0x7BEF7BEFu);
}

/* The same, rounded up: libretro's gpsp_frame_mixing. */
static inline u32 average_up(u32 a, u32 b)
{
  return (a | b) - (((a ^ b) >> 1) & 0x7BEF7BEFu);
}

/* Stores as one STM (one burst): the screen buffers aren't in the data
 * cache, and GCC would write the words one at a time. */
static inline void store_2_words(u32 *d, u32 a, u32 b)
{
#if defined(__arm__)
  register u32 r4 __asm__("r4") = a;
  register u32 r5 __asm__("r5") = b;
  __asm__ volatile ("stmia %0, {%1, %2}" : : "r" (d), "r" (r4), "r" (r5) : "memory");
#else
  d[0] = a;
  d[1] = b;
#endif
}

static inline void store_3_words(u32 *d, u32 a, u32 b, u32 c)
{
#if defined(__arm__)
  register u32 r4 __asm__("r4") = a;
  register u32 r5 __asm__("r5") = b;
  register u32 r6 __asm__("r6") = c;
  __asm__ volatile ("stmia %0, {%1, %2, %3}" : : "r" (d), "r" (r4), "r" (r5), "r" (r6) : "memory");
#else
  d[0] = a;
  d[1] = b;
  d[2] = c;
#endif
}

/* Source lines with interframe blending done, for the lines being scaled. */
static u16 mixed_lines[4 * SRC_W] __attribute__((aligned(32)));

/* Interframe blending of n lines from src (SRC_W a line) with the previous
 * frame's (prev) into mixed_lines. */
static const u16 *mix_lines(const u16 *src, const u16 *prev, u32 n)
{
  const u32 *s = (const u32 *)src, *p = (const u32 *)prev;
  u32 *d = (u32 *)mixed_lines;
  u32 i;

  for (i = 0; i < n * SRC_W / 2; i++)
    d[i] = average_up(s[i], p[i]);
  return mixed_lines;
}

/* ---- scaled/blended 3:2 ---- */

/* The 320x213 picture is at landscape lines 14-226, with line 13 black.
 * In a portrait LCD line (240 pixels, landscape line y at position 239 - y)
 * group g's 4 lines start at position 222 - 4g: 2 mod 4, an even position,
 * so a column's 4 pixels are two word-aligned words (byte offset 444 - 8g:
 * STM safe on the ARM926). Half of those 2-word stores cross a 16-byte
 * line (F4: its cost on the CX II's uncached buffers is unmeasured;
 * compare "out" ms in gpsp_perf.txt with R9F's 3.9). ASPECT_TOP 13 would
 * give odd positions (unaligned), so 14 is the only aligned choice under a
 * 13-line band. The text
 * bands (nspire_video_present.c, OSD_BAND_H) are lines 0-12 and 227-239 and
 * are drawn only when their text changes, so the picture must never write
 * them nor they the picture: at lines 12-224 (until 2026-11-11) the top
 * band's last line covered the picture's first one in each frame that drew
 * the band (the CX II's top row flashed black), and the black under the
 * picture covered the bottom band's first line. */
#define ASPECT_TOP 14

/* On the calculator (and its ARM tests) the groups are blend_portrait.S's
 * nspire_p_groups: the same math in hand-written ARM (GCC spilled registers
 * here: 13 instructions an LCD pixel). The C below is what the PC tests run
 * and what the assembly follows. */
#if defined(__arm__)
void nspire_p_groups(u16 *dst, const u16 *src, const u16 *prev_or_null);
void nspire_p_groups_n(u16 *dst_group, const u16 *src_group, const u16 *prev_group_or_null, u32 n);
#else
/* A source pixel of a word of two (the lower address in the low half),
 * unpacked. */
static inline u32 unpack_lo(u32 w)
{
  u32 t = w << 16;
  return (t | (t >> 16)) & UNPACKED_MASK;
}

static inline u32 unpack_hi(u32 w)
{
  u32 t = w >> 16;
  return (t | (t << 16)) & UNPACKED_MASK;
}

/* A 3x3 block of source pixels (columns a, b, c; rows 0-2, unpacked) -> its
 * 4 picture columns of 4 pixels, each column's bottom two pixels, then its
 * top two, at *d and the next 3 LCD lines. */
static inline __attribute__((always_inline)) void aspect_block(u32 **dp, u32 a0, u32 a1, u32 a2,
                                                                u32 b0, u32 b1, u32 b2,
                                                                u32 c0, u32 c1, u32 c2)
{
  u32 *d = *dp;
  store_2_words(d, pack_pair(a2, blend_3_5(a2, a1)), pack_pair(blend_3_5(a0, a1), a0));
  d += DST_W / 2;
  store_2_words(d, pack_pair(blend_3_5(a2, b2), blend_14_7_7_4(b1, b2, a1, a2)),
                pack_pair(blend_14_7_7_4(b1, b0, a1, a0), blend_3_5(a0, b0)));
  d += DST_W / 2;
  store_2_words(d, pack_pair(blend_3_5(c2, b2), blend_14_7_7_4(b1, b2, c1, c2)),
                pack_pair(blend_14_7_7_4(b1, b0, c1, c0), blend_3_5(c0, b0)));
  d += DST_W / 2;
  store_2_words(d, pack_pair(c2, blend_3_5(c2, c1)), pack_pair(blend_3_5(c0, c1), c0));
  d += DST_W / 2;
  *dp = d;
}

/* Source lines s, s + SRC_W, s + 2 SRC_W (with interframe blending: averaged
 * with the previous frame's, p) -> 4 picture lines, whose bottom one is at
 * position pos of each LCD line. Six source columns at a time: three words
 * a line, two 3x3 blocks. */
static inline __attribute__((always_inline)) void aspect_group(u16 *dst, const u16 *s, const u16 *p,
                                                               u32 pos, int mix)
{
  const u32 *r = (const u32 *)s, *q = (const u32 *)p;
  u32 *d = (u32 *)(dst + pos);
  u32 x;

#define ROW(n) (SRC_W / 2 * (n))
  for (x = 0; x < GBA_SCREEN_WIDTH; x += 6, r += 3, q += 3)
  {
    u32 w00 = r[0], w01 = r[1], w02 = r[2];
    u32 w10 = r[ROW(1)], w11 = r[ROW(1) + 1], w12 = r[ROW(1) + 2];
    u32 w20 = r[ROW(2)], w21 = r[ROW(2) + 1], w22 = r[ROW(2) + 2];
    if (mix)
    {
      w00 = average_up(w00, q[0]);
      w01 = average_up(w01, q[1]);
      w02 = average_up(w02, q[2]);
      w10 = average_up(w10, q[ROW(1)]);
      w11 = average_up(w11, q[ROW(1) + 1]);
      w12 = average_up(w12, q[ROW(1) + 2]);
      w20 = average_up(w20, q[ROW(2)]);
      w21 = average_up(w21, q[ROW(2) + 1]);
      w22 = average_up(w22, q[ROW(2) + 2]);
    }
    aspect_block(&d, unpack_lo(w00), unpack_lo(w10), unpack_lo(w20),
                 unpack_hi(w00), unpack_hi(w10), unpack_hi(w20),
                 unpack_lo(w01), unpack_lo(w11), unpack_lo(w21));
    aspect_block(&d, unpack_hi(w01), unpack_hi(w11), unpack_hi(w21),
                 unpack_lo(w02), unpack_lo(w12), unpack_lo(w22),
                 unpack_hi(w02), unpack_hi(w12), unpack_hi(w22));
  }
#undef ROW
}

static void aspect_groups_mixed(u16 *dst, const u16 *src, const u16 *prev, u32 first, u32 n)
{
  u32 group;
  for (group = first; group < first + n; group++)
    aspect_group(dst, src + group * 3 * SRC_W, prev + group * 3 * SRC_W,
                 DST_W - 1 - (ASPECT_TOP + group * 4 + 3), 1);
}

static void aspect_groups(u16 *dst, const u16 *src, u32 first, u32 n)
{
  u32 group;
  for (group = first; group < first + n; group++)
    aspect_group(dst, src + group * 3 * SRC_W, src, DST_W - 1 - (ASPECT_TOP + group * 4 + 3), 0);
}
#endif

/* src's lines 3 first to 3 (first + n) - 1 (with prev: blended with it):
 * groups first to first + n - 1 of the turned buffer dst, landscape lines
 * 14 + 4 first on (the 53 groups: 14-225). */
void nspire_blend_aspect_groups(u16 *dst, const u16 *src, const u16 *prev, u32 first, u32 n)
{
#if defined(__arm__)
  nspire_p_groups_n((u16 *)((u8 *)dst + 444 - 8 * first), src + first * 3 * SRC_W,
                    prev ? prev + first * 3 * SRC_W : NULL, n);
#else
  if (prev)
    aspect_groups_mixed(dst, src, prev, first, n);
  else
    aspect_groups(dst, src, first, n);
#endif
}

/* A pixel unpacked -> 5:6:5 */
static inline u16 pack_one(u32 p)
{
  return (u16)(p | (p >> 16));
}

/* The last source line (blended across only) at landscape line 226
 * (position 13 of each LCD line), and line 13 above the picture black
 * (position 226): a halfword each, so neither band's line is touched. */
static void aspect_last_line(u16 *dst, const u16 *s)
{
  const u32 last = DST_W - 1 - (ASPECT_TOP + 212), gap = DST_W - 1 - (ASPECT_TOP - 1);
  u16 *line = dst;
  u32 x;

  for (x = 0; x < GBA_SCREEN_WIDTH; x += 3, s += 3)
  {
    u32 a = unpack(s[0]), b = unpack(s[1]), c = unpack(s[2]);
    line[last] = pack_one(a);
    line[gap] = 0;
    line += DST_W;
    line[last] = pack_one(blend_3_5(a, b));
    line[gap] = 0;
    line += DST_W;
    line[last] = pack_one(blend_3_5(c, b));
    line[gap] = 0;
    line += DST_W;
    line[last] = pack_one(c);
    line[gap] = 0;
    line += DST_W;
  }
}

/* The scaled/blended 3:2 picture of src (240x160, GBA_SCREEN_PITCH pixels a
 * line), turned into dst at landscape lines 14-226 (13 black). With
 * prev (the previous frame, only read), frames are blended with it, in the
 * same pass: the source is read once and nothing but the picture is
 * written. */
void nspire_blend_turn_aspect(u16 *dst, const u16 *src, const u16 *prev)
{
  /* The groups read src and prev a word at a time and store with STM, as
   * the C version did: all three are word aligned (malloc'd frames, the
   * screen buffers), or the calculator stops. */
#if defined(__arm__)
  nspire_p_groups(dst, src, prev);
#else
  nspire_blend_aspect_groups(dst, src, prev, 0, GBA_SCREEN_HEIGHT / 3);
#endif
  nspire_blend_aspect_last(dst, src, prev);
}

/* nspire_blend_turn_aspect's last source line, and line 13. */
void nspire_blend_aspect_last(u16 *dst, const u16 *src, const u16 *prev)
{
  const u16 *last = src + (GBA_SCREEN_HEIGHT - 1) * SRC_W;
  if (prev)
    last = mix_lines(last, prev + (GBA_SCREEN_HEIGHT - 1) * SRC_W, 1);
  aspect_last_line(dst, last);
}

/* ---- fullscreen/blended ---- */

/* Source lines a1, b1, a2, b2 (s, s + SRC_W, ...) -> 6 picture lines, the
 * bottom one at position pos of each LCD line: 3 aligned words a column. */
static void full_group(u16 *dst, const u16 *s, u32 pos)
{
  u32 *d = (u32 *)(dst + pos);
  u32 x;

  for (x = 0; x < GBA_SCREEN_WIDTH; x += 3, s += 3)
  {
    /* Each register holds two lines' pixels of one column: a1 | b1 << 16
     * and a2 | b2 << 16. */
    u32 p1 = s[0] | ((u32)s[SRC_W] << 16), p2 = s[2 * SRC_W] | ((u32)s[3 * SRC_W] << 16);
    u32 q1 = s[1] | ((u32)s[SRC_W + 1] << 16), q2 = s[2 * SRC_W + 1] | ((u32)s[3 * SRC_W + 1] << 16);
    u32 r1 = s[2] | ((u32)s[SRC_W + 2] << 16), r2 = s[2 * SRC_W + 2] | ((u32)s[3 * SRC_W + 2] << 16);
    u32 h1 = average_down(q1, r1), h2 = average_down(q2, r2);

#define FULL_COLUMN(c1, c2)                                                   \
    {                                                                         \
      u32 m1 = average_down(c1, c1 >> 16) & 0xFFFF;                           \
      u32 m2 = average_down(c2, c2 >> 16) & 0xFFFF;                           \
      /* from the bottom: b2, (a2+b2)/2, a2, b1, (a1+b1)/2, a1 */             \
      store_3_words(d, (c2 >> 16) | (m2 << 16), (c2 & 0xFFFF) | (c1 & 0xFFFF0000u), \
                    m1 | (c1 << 16));                                         \
      d += DST_W / 2;                                                         \
    }
    FULL_COLUMN(p1, p2)
    FULL_COLUMN(q1, q2)
    FULL_COLUMN(h1, h2)
    FULL_COLUMN(r1, r2)
#undef FULL_COLUMN
  }
}

/* The fullscreen/blended picture of src, turned into all of dst. prev as in
 * nspire_blend_turn_aspect. */
void nspire_blend_turn_full(u16 *dst, const u16 *src, const u16 *prev)
{
  u32 group;

  for (group = 0; group < GBA_SCREEN_HEIGHT / 4; group++)
  {
    const u16 *s = src + group * 4 * SRC_W;
    if (prev)
      s = mix_lines(s, prev + group * 4 * SRC_W, 4);
    full_group(dst, s, DST_W - 1 - (group * 6 + 5));
  }
}

/* ---- scaled/blended 3:2 on a landscape LCD (an older CX) ---- */

/* The same picture as notaz's upscale_aspect (nspire/upscale_aspect.s) made
 * of the frame copied into 5:5:5 BGR (and before that blended with the
 * previous one), as gpSP drew it: three passes over memory (mixing, the copy,
 * the scaler), 17 ms a frame on a CX by the PC estimate. Here it is one, from
 * the core's 5:6:5 straight into the LCD's 5:5:5 BGR, pixel for pixel the
 * same (blend_test): green's sixth bit is dropped before any blending, as
 * the copy did. Unpacked as notaz's: 000000gg ggg00000 0bbbbb00 000rrrrr. */
#define L_MASK 0x03E07C1Fu
#define L_ONES 0x00200401u
#define L_W 320   /* pixels per LCD line */
#define L_TOP 13  /* the picture's first line (lines 13-225) */

static inline u32 l_unpack(u32 p)   /* p: one core pixel (5:6:5, red on top) */
{
  return ((p >> 11) & 0x1Fu) | ((p & 0x1Fu) << 10) | ((p << 15) & 0x03E00000u);
}

static inline u32 l_3_5(u32 a, u32 b)
{
  return ((a + (a << 1) + b + (b << 2) + (L_ONES << 2)) >> 3) & L_MASK;
}

static inline u32 l_14_7_7_4(u32 c, u32 h, u32 v, u32 d)
{
  u32 s = (c << 1) + h + v;
  return (((s << 3) - s + (d << 2) + (L_ONES << 4)) >> 5) & L_MASK;
}

/* On the calculator (and its ARM tests) the groups are blend_landscape.S's
 * nspire_l_groups: the same math in hand-written ARM (GCC spilled registers
 * here: 16 instructions an LCD pixel). The C below is what the PC tests run
 * and what the assembly follows. */
#if defined(__arm__)
void nspire_l_groups(u32 *dst_line13, const u16 *src, const u16 *prev_or_null);
void nspire_l_groups_n(u32 *dst_group, const u16 *src_group, const u16 *prev_group_or_null, u32 n);
#else
/* A 3x3 block (pNM: row N, column M, unpacked) -> 4 lines of 4 pixels, two
 * words a line at d. */
static inline __attribute__((always_inline)) void l_block(u32 *d, u32 p00, u32 p01, u32 p02,
                                                           u32 p10, u32 p11, u32 p12,
                                                           u32 p20, u32 p21, u32 p22)
{
  store_2_words(d, pack_pair(p00, l_3_5(p00, p01)), pack_pair(l_3_5(p02, p01), p02));
  d += L_W / 2;
  store_2_words(d, pack_pair(l_3_5(p00, p10), l_14_7_7_4(p11, p01, p10, p00)),
                pack_pair(l_14_7_7_4(p11, p12, p01, p02), l_3_5(p02, p12)));
  d += L_W / 2;
  store_2_words(d, pack_pair(l_3_5(p20, p10), l_14_7_7_4(p11, p21, p10, p20)),
                pack_pair(l_14_7_7_4(p11, p12, p21, p22), l_3_5(p22, p12)));
  d += L_W / 2;
  store_2_words(d, pack_pair(p20, l_3_5(p20, p21)), pack_pair(l_3_5(p22, p21), p22));
}

static inline __attribute__((always_inline)) void l_group(u32 *d, const u16 *s, const u16 *p, int mix)
{
  const u32 *r = (const u32 *)s, *q = (const u32 *)p;
  u32 x;

#define ROW(n) (SRC_W / 2 * (n))
  for (x = 0; x < GBA_SCREEN_WIDTH; x += 6, r += 3, q += 3, d += 4)
  {
    u32 w00 = r[0], w01 = r[1], w02 = r[2];
    u32 w10 = r[ROW(1)], w11 = r[ROW(1) + 1], w12 = r[ROW(1) + 2];
    u32 w20 = r[ROW(2)], w21 = r[ROW(2) + 1], w22 = r[ROW(2) + 2];
    if (mix)
    {
      w00 = average_up(w00, q[0]);
      w01 = average_up(w01, q[1]);
      w02 = average_up(w02, q[2]);
      w10 = average_up(w10, q[ROW(1)]);
      w11 = average_up(w11, q[ROW(1) + 1]);
      w12 = average_up(w12, q[ROW(1) + 2]);
      w20 = average_up(w20, q[ROW(2)]);
      w21 = average_up(w21, q[ROW(2) + 1]);
      w22 = average_up(w22, q[ROW(2) + 2]);
    }
    l_block(d, l_unpack(w00), l_unpack(w00 >> 16), l_unpack(w01),
            l_unpack(w10), l_unpack(w10 >> 16), l_unpack(w11),
            l_unpack(w20), l_unpack(w20 >> 16), l_unpack(w21));
    l_block(d + 2, l_unpack(w01 >> 16), l_unpack(w02), l_unpack(w02 >> 16),
            l_unpack(w11 >> 16), l_unpack(w12), l_unpack(w12 >> 16),
            l_unpack(w21 >> 16), l_unpack(w22), l_unpack(w22 >> 16));
  }
#undef ROW
}

static void l_groups_mixed(u16 *dst, const u16 *src, const u16 *prev, u32 first, u32 n)
{
  u32 group;
  for (group = first; group < first + n; group++)
    l_group((u32 *)(dst + (L_TOP + group * 4) * L_W), src + group * 3 * SRC_W, prev + group * 3 * SRC_W, 1);
}

static void l_groups(u16 *dst, const u16 *src, u32 first, u32 n)
{
  u32 group;
  for (group = first; group < first + n; group++)
    l_group((u32 *)(dst + (L_TOP + group * 4) * L_W), src + group * 3 * SRC_W, src, 0);
}
#endif

/* Groups first to first + n - 1 of the landscape 3:2 picture (source lines
 * 3 first on, with prev blended with them) -> LCD lines 13 + 4 first on. */
void nspire_blend_landscape_groups(u16 *dst, const u16 *src, const u16 *prev, u32 first, u32 n)
{
#if defined(__arm__)
  nspire_l_groups_n((u32 *)(dst + (L_TOP + first * 4) * L_W), src + first * 3 * SRC_W,
                    prev ? prev + first * 3 * SRC_W : NULL, n);
#else
  if (prev)
    l_groups_mixed(dst, src, prev, first, n);
  else
    l_groups(dst, src, first, n);
#endif
}

/* The scaled/blended 3:2 picture of src (with prev: blended with it, only
 * read) into a landscape screen buffer's lines 13-225, in its 5:5:5 BGR. */
void nspire_blend_aspect_landscape(u16 *dst, const u16 *src, const u16 *prev)
{
  /* The groups read src and prev a word at a time and store with STM, as
   * the C version did: all three are word aligned (malloc'd frames, the
   * screen buffers), or the calculator stops. */
#if defined(__arm__)
  nspire_l_groups((u32 *)(dst + L_TOP * L_W), src, prev);
#else
  nspire_blend_landscape_groups(dst, src, prev, 0, GBA_SCREEN_HEIGHT / 3);
#endif
  nspire_blend_landscape_last(dst, src, prev);
}

/* nspire_blend_aspect_landscape's last source line, blended across only. */
void nspire_blend_landscape_last(u16 *dst, const u16 *src, const u16 *prev)
{
  const u16 *last = src + (GBA_SCREEN_HEIGHT - 1) * SRC_W;
  u32 *d = (u32 *)(dst + (L_TOP + GBA_SCREEN_HEIGHT / 3 * 4) * L_W);
  u32 x;

  if (prev)
    last = mix_lines(last, prev + (GBA_SCREEN_HEIGHT - 1) * SRC_W, 1);
  /* The last source line, blended across only. */
  for (x = 0; x < GBA_SCREEN_WIDTH; x += 3, last += 3, d += 2)
  {
    u32 a = l_unpack(last[0]), b = l_unpack(last[1]), c = l_unpack(last[2]);
    store_2_words(d, pack_pair(a, l_3_5(a, b)), pack_pair(l_3_5(c, b), c));
  }
}
