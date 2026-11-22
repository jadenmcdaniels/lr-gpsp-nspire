/*
 * The one-pass blended pictures (nspire_blend.c) against the four-pass path
 * gpSP had (frame mixing, the copy into the old 5:5:5 format, notaz's ARM
 * scalers in nspire/upscale_aspect.s, nspire_turn_rect_555), both built for
 * the calculator's CPU and run under qemu-arm (tests/arm/run.sh).
 *
 * Every picture pixel must have the same red and blue as before, and green
 * within 3 of 63: the new path keeps green's sixth bit, the old one dropped
 * it (and fullscreen's blended corners averaged twice at 5 bits). Every
 * pixel must also match, exactly, a plain per-pixel version of the math
 * written from nspire_blend.c's description. The 3:2 picture is one line
 * lower than gpSP had it (lines 14-226, with 13 black; until 2026-11-11 it
 * was at 12-224 with 225-227 black, under the text bands' lines 12 and
 * 227); nothing else of the screen buffer may be written: lines 0-12 and
 * 227-239 are the text bands', drawn only when their text changes.
 *
 * The 3:2 pictures' groups are hand-written ARM here as on the calculator
 * (nspire/blend_portrait.S for the CX II's turned picture,
 * nspire/blend_landscape.S for the older CX's); nspire_blend.c keeps their
 * C for the PC tests. The frames with flat parts (made for the shortcuts
 * both take for a column of one colour, and the landscape code for a
 * block) go through both.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint16_t u16;
typedef uint32_t u32;

void upscale_aspect(u16 *dst, u16 *src);
void upscale_aspect_fast(u16 *dst, u16 *src);
void nspire_turn_rect_555(u16 *dst, const u16 *src, u32 src_pitch, u32 x0, u32 y0, u32 w, u32 h);
void nspire_blend_turn_aspect(u16 *dst, const u16 *src, const u16 *prev);
void nspire_blend_turn_full(u16 *dst, const u16 *src, const u16 *prev);
void nspire_blend_aspect_landscape(u16 *dst, const u16 *src, const u16 *prev);
/* speed10: their parts (nspire_reuse.c makes the groups that changed) */
void nspire_blend_aspect_groups(u16 *dst, const u16 *src, const u16 *prev, u32 first, u32 n);
void nspire_blend_aspect_last(u16 *dst, const u16 *src, const u16 *prev);
void nspire_blend_landscape_groups(u16 *dst, const u16 *src, const u16 *prev, u32 first, u32 n);
void nspire_blend_landscape_last(u16 *dst, const u16 *src, const u16 *prev);
/* blend_ref.c: nspire_blend.c before the fused 3:2 picture (prev got each
 * frame then). */
void ref_blend_turn_aspect(u16 *dst, const u16 *src, u16 *prev);
void ref_blend_turn_full(u16 *dst, const u16 *src, u16 *prev);

#define W 240
#define H 160
#define SENTINEL 0x5A5A

static u16 frame[W * H] __attribute__((aligned(32)));
static u16 old_mix_prev[W * H], old_mixed[W * H];
static u16 new_mix_prev[W * H] __attribute__((aligned(32)));
static u16 ref_mix_prev[W * H] __attribute__((aligned(32)));
static u16 prev_copy[W * H];
static u16 ref_screen[240 * 320] __attribute__((aligned(32)));
static u16 upscale_src[320 * H] __attribute__((aligned(32)));
static u16 landscape[320 * 240] __attribute__((aligned(32)));
static u16 old_screen[240 * 320] __attribute__((aligned(32)));
static u16 new_screen[240 * 320] __attribute__((aligned(32)));
static u16 land_mix_prev[W * H] __attribute__((aligned(32)));
static u16 land_old[320 * 240] __attribute__((aligned(32)));
static u16 land_new[320 * 240] __attribute__((aligned(32)));

static int failures, checks, worst_green;

static void check(int ok, const char *what)
{
  checks++;
  if (!ok)
  {
    failures++;
    printf("FAIL  %s\n", what);
  }
}

/* ---- The old path, as nspire_video_present.c had it ---- */

static void old_mix(void)
{
  u32 i;
  for (i = 0; i < W * H; i++)
  {
    u16 c = frame[i], p = old_mix_prev[i];
    old_mix_prev[i] = c;
    old_mixed[i] = (u16)((c + p + ((c ^ p) & 0x821u)) >> 1);
  }
}

static void old_repack(const u16 *src)
{
  u32 x, y;
  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++)
    {
      u32 p = src[y * W + x];
      upscale_src[y * 320 + x] = (u16)(((p & 0x1Fu) << 10) | (((p & 0x07C0u) >> 1) & 0x03E0u) | ((p >> 11) & 0x1Fu));
    }
}

static void old_aspect(int mix)
{
  if (mix)
    old_mix();
  old_repack(mix ? old_mixed : frame);
  upscale_aspect(landscape, upscale_src);
  nspire_turn_rect_555(old_screen, landscape + 13 * 320, 320, 0, 13, 320, 213);
}

static void old_full(int mix)
{
  if (mix)
    old_mix();
  old_repack(mix ? old_mixed : frame);
  upscale_aspect_fast(landscape, upscale_src);
  nspire_turn_rect_555(old_screen, landscape, 320, 0, 0, 320, 240);
}

/* The older CX's landscape 3:2 picture as gpSP drew it (mixing, the copy,
 * notaz's scaler, straight into the screen buffer) against the one pass
 * (nspire_blend_aspect_landscape): every pixel of the screen buffer the
 * same, the bands outside lines 13-225 untouched. */
static void landscape_check(int mix, const u16 *prev, const char *what)
{
  char name[128];
  u32 n;
  for (n = 0; n < 320 * 240; n++)
    land_old[n] = land_new[n] = SENTINEL;
  if (mix)
    old_mix();
  old_repack(mix ? old_mixed : frame);
  upscale_aspect(land_old, upscale_src);
  nspire_blend_aspect_landscape(land_new, frame, prev);
  snprintf(name, sizeof(name), "landscape 3:2 (an older CX), %s: as before, exactly", what);
  check(!memcmp(land_old, land_new, sizeof(land_new)), name);
}

/* ---- Comparing ---- */

static u16 at(const u16 *screen, u32 x, u32 y)
{
  return screen[x * 240 + 239 - y];
}

/* The old picture's line y_old against the new one's y_new, for h lines. */
static int same_picture(u32 y_old, u32 y_new, u32 h, const char *name)
{
  u32 x, y;
  int bad = 0;

  for (y = 0; y < h; y++)
    for (x = 0; x < 320; x++)
    {
      u32 o = at(old_screen, x, y_old + y), n = at(new_screen, x, y_new + y);
      int dg = (int)((n >> 5) & 63) - (int)((o >> 5) & 63);
      if (dg < 0)
        dg = -dg;
      if (dg > worst_green)
        worst_green = dg;
      if ((o & 0xF81Fu) != (n & 0xF81Fu) || dg > 3)
      {
        if (!bad)
          printf("  %s: (%u, %u) was %04x, now %04x\n", name, (unsigned)x, (unsigned)(y_new + y),
                 (unsigned)o, (unsigned)n);
        bad = 1;
      }
    }
  return !bad;
}

/* ---- The math, a pixel at a time ---- */

static const u16 *ref_src;   /* the frame the pictures are made of (mixed) */

static u32 channel(u32 p, int c)
{
  return c == 0 ? p >> 11 : c == 1 ? (p >> 5) & 63 : p & 31;
}

static u32 from_channels(const u32 *v)
{
  return (v[0] << 11) | (v[1] << 5) | v[2];
}

static u32 src_px(u32 x, u32 y)
{
  return ref_src[y * W + x];
}

/* Along one direction, picture position i of a group of 4 from source 3k..:
 * the weights (out of 8) of the source positions 3k, 3k+1, 3k+2. */
static const u32 weights_3_2[4][3] = { { 8, 0, 0 }, { 3, 5, 0 }, { 0, 5, 3 }, { 0, 0, 8 } };

static u32 ref_aspect(u32 x, u32 y)
{
  u32 gx = x / 4 * 3, gy = y / 4 * 3, px = x % 4, py = y % 4, v[3];
  int c;

  if (y == 212)
    py = 0;   /* the last source line, blended across only */
  for (c = 0; c < 3; c++)
  {
    u32 i, j, sum = 0;
    if ((px == 1 || px == 2) && (py == 1 || py == 2))
    {
      /* 14:7:7:4: the middle pixel, its neighbours toward the corner, the corner */
      u32 ex = px == 1 ? gx : gx + 2, ey = py == 1 ? gy : gy + 2;
      sum = 14 * channel(src_px(gx + 1, gy + 1), c) + 7 * channel(src_px(gx + 1, ey), c) +
            7 * channel(src_px(ex, gy + 1), c) + 4 * channel(src_px(ex, ey), c);
      v[c] = (sum + 16) >> 5;
      continue;
    }
    for (j = 0; j < 3; j++)
      for (i = 0; i < 3; i++)
        if (weights_3_2[py][j] && weights_3_2[px][i])
          sum += weights_3_2[py][j] * weights_3_2[px][i] * channel(src_px(gx + i, gy + j), c);
    v[c] = (sum + 32) >> 6;
  }
  return from_channels(v);
}

static u32 average_channels(u32 a, u32 b)
{
  u32 v[3];
  int c;
  for (c = 0; c < 3; c++)
    v[c] = (channel(a, c) + channel(b, c)) >> 1;
  return from_channels(v);
}

/* Across: a b (b+c)/2 c; down: a (a+b)/2 b. */
static u32 ref_full_line(u32 x, u32 sy)
{
  u32 gx = x / 4 * 3;
  switch (x % 4)
  {
  case 0: return src_px(gx, sy);
  case 1: return src_px(gx + 1, sy);
  case 2: return average_channels(src_px(gx + 1, sy), src_px(gx + 2, sy));
  default: return src_px(gx + 2, sy);
  }
}

static u32 ref_full(u32 x, u32 y)
{
  u32 sy = y / 3 * 2;
  switch (y % 3)
  {
  case 0: return ref_full_line(x, sy);
  case 1: return average_channels(ref_full_line(x, sy + 1), ref_full_line(x, sy));
  default: return ref_full_line(x, sy + 1);
  }
}

static int matches_math(u32 (*ref)(u32, u32), u32 top, u32 h, const char *name)
{
  u32 x, y;
  for (y = 0; y < h; y++)
    for (x = 0; x < 320; x++)
    {
      u32 want = ref(x, y), got = at(new_screen, x, top + y);
      if (want != got)
      {
        printf("  %s: (%u, %u) should be %04x, is %04x\n", name, (unsigned)x, (unsigned)(top + y),
               (unsigned)want, (unsigned)got);
        return 0;
      }
    }
  return 1;
}

/* The 3:2 picture as blend_ref.c's (lines 12-224) two lines lower now:
 * every pixel the same, line 13 black, the bands (0-12, 227-239) not
 * written (SENTINEL). */
static int same_as_ref_lower(void)
{
  u32 x, y;
  for (y = 0; y < 240; y++)
    for (x = 0; x < 320; x++)
    {
      u32 want = y >= 14 && y <= 226 ? at(ref_screen, x, y - 2) : y == 13 ? 0 : SENTINEL;
      if (at(new_screen, x, y) != want)
      {
        printf("  (%u, %u) should be %04x, is %04x\n", (unsigned)x, (unsigned)y, (unsigned)want,
               (unsigned)at(new_screen, x, y));
        return 0;
      }
    }
  return 1;
}

static int lines_are(const u16 *screen, u32 y0, u32 y1, u16 value)
{
  u32 x, y;
  for (y = y0; y < y1; y++)
    for (x = 0; x < 320; x++)
      if (at(screen, x, y) != value)
        return 0;
  return 1;
}

/* ---- Frames ---- */

static u32 seed = 12345;

static u32 next_random(void)
{
  seed ^= seed << 13;
  seed ^= seed >> 17;
  seed ^= seed << 5;
  return seed;
}

static void make_frame(int kind)
{
  static const u16 extremes[] = { 0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0x0821, 0xF7DE };
  u32 x, y;

  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++)
    {
      u16 p;
      switch (kind)
      {
      case 0: p = (u16)next_random(); break;
      case 1: p = 0xFFFF; break;
      case 2: p = ((x ^ y) & 1) ? 0xFFFF : 0x0000; break;
      case 3: p = (u16)(((x % 32) << 11) | ((y % 64) << 5) | ((x + y) % 32)); break;
      default: p = extremes[next_random() % (sizeof(extremes) / sizeof(extremes[0]))]; break;
      }
      frame[y * W + x] = p;
    }
}

static const char *const kind_names[] = { "noise", "white", "checkerboard", "gradients", "extreme colors" };
#define KINDS 5

/* ---- Frames with flat parts ----
 * The 3:2 pictures (nspire/blend_landscape.S, nspire/blend_portrait.S) take
 * a shortcut for a column (6x3 source pixels -> 8x4 LCD pixels) that is one
 * colour, in the frame and (with interframe blending) in the previous
 * frame, and the landscape one in other columns for a block (3x3 -> 4x4)
 * whose mixed pixels are one colour. */

static u16 other_frame[W * H];   /* a previous frame for these cases */

static u16 random_colour(void)
{
  static const u16 extremes[] = { 0x0000, 0xFFFF, 0xF800, 0x07E0, 0x001F, 0x0821, 0xF7DE, 0x0020, 0x8410 };
  u32 r = next_random();
  return (r >> 16) % 5 ? (u16)r : extremes[r % (sizeof(extremes) / sizeof(extremes[0]))];
}

/* Tiles of tw x th pixels from (-ox, -oy), each one colour. */
static void make_tiles(u32 tw, u32 th, u32 ox, u32 oy)
{
  static u16 colours[W * H];
  u32 x, y, across = (W + ox) / tw + 1;

  for (x = 0; x < W * H; x++)
    colours[x] = random_colour();
  for (y = 0; y < H; y++)
    for (x = 0; x < W; x++)
      frame[y * W + x] = colours[(y + oy) / th * across + (x + ox) / tw];
}

/* One colour, but for one pixel in most columns: column c of group g has
 * its pixel (c + 7 g) % 20 of 18 (line by line) in the other colour; 18
 * and 19: none. Every one of the 18 places comes up in every group. */
static void make_one_off(u16 base, u16 other)
{
  u32 c, g, n;

  for (n = 0; n < W * H; n++)
    frame[n] = base;
  for (g = 0; g < H / 3; g++)
    for (c = 0; c < W / 6; c++)
    {
      u32 k = (c + 7 * g) % 20;
      if (k < 18)
        frame[(g * 3 + k / 6) * W + c * 6 + k % 6] = other;
    }
}

static void make_plain(u16 colour)
{
  u32 n;
  for (n = 0; n < W * H; n++)
    frame[n] = colour;
}

/* Upright stripes a pixel wide: every source word of a column is the same,
 * but its two pixels aren't. */
static void make_stripes(u16 even, u16 odd)
{
  u32 n;
  for (n = 0; n < W * H; n++)
    frame[n] = n % W % 2 ? odd : even;
}

/* The value (bit 0: its first pixel is the other colour, bit 1: its second)
 * of word 0-8 of a column (3 a line) in word pattern p: all 9 words one
 * value (4 of them), but for none, one (9 x 3 other values) or two
 * (36 x 9) of them: 4 x 352 patterns. */
#define WORD_PATTERNS (4 * 352)
static u32 word_pattern(u32 p, u32 word)
{
  u32 base = p / 352 % 4, q = p % 352, i, j, k = 0;

  if (q == 0)
    return base;
  if (q < 28)
    return word == (q - 1) / 3 ? (base + 1 + (q - 1) % 3) % 4 : base;
  q -= 28;
  for (i = 0; i < 9; i++)
    for (j = i + 1; j < 9; j++, k++)
      if (k == q / 9)
        return word == i ? (base + 1 + q % 9 / 3) % 4 : word == j ? (base + 1 + q % 3) % 4 : base;
  return base;
}

/* The frame's columns take the word patterns in turn, from pattern first:
 * every way a column can be one colour but for one or two of its words. */
static void make_word_patterns(u16 x, u16 y, u32 first)
{
  u32 c, g, w;

  for (g = 0; g < H / 3; g++)
    for (c = 0; c < W / 6; c++)
      for (w = 0; w < 9; w++)
      {
        u32 v = word_pattern((g * (W / 6) + c + first) % WORD_PATTERNS, w);
        u16 *at = frame + (g * 3 + w / 3) * W + c * 6 + w % 3 * 2;
        at[0] = v & 1 ? y : x;
        at[1] = v & 2 ? y : x;
      }
}

/* The same with a column of x alone before each of them (the odd columns
 * take the patterns): what a column's test finds when the column before
 * took the flat column's shortcut. 20 patterns a group, 1060 a frame. */
static void make_word_patterns_apart(u16 x, u16 y, u32 first)
{
  u32 c, g, w, n;

  for (n = 0; n < W * H; n++)
    frame[n] = x;
  for (g = 0; g < H / 3; g++)
    for (c = 1; c < W / 6; c += 2)
      for (w = 0; w < 9; w++)
      {
        u32 v = word_pattern((g * (W / 12) + c / 2 + first) % WORD_PATTERNS, w);
        u16 *at = frame + (g * 3 + w / 3) * W + c * 6 + w % 3 * 2;
        at[0] = v & 1 ? y : x;
        at[1] = v & 2 ? y : x;
      }
}

/* Every 3x3 block of two colours (512: bit 3 line + place is that pixel's
 * colour), the frame's columns taking them in turn: how 0: in the left
 * block, the right one one colour; 1: in the right block; 2, 3: in both,
 * each with different ones beside it. */
static void make_block_patterns(u16 x, u16 y, int how)
{
  u32 c, g, r, i;

  for (g = 0; g < H / 3; g++)
    for (c = 0; c < W / 6; c++)
    {
      u32 n = g * (W / 6) + c, turn = n / 512, left, right;
      switch (how)
      {
      case 0: left = n % 512; right = turn & 1 ? 511 : 0; break;
      case 1: left = turn & 1 ? 0 : 511; right = n % 512; break;
      case 2: left = n % 512; right = (n * 7 + turn * 37 + 3) % 512; break;
      default: left = (n * 5 + turn * 101 + 1) % 512; right = n % 512; break;
      }
      for (r = 0; r < 3; r++)
        for (i = 0; i < 3; i++)
        {
          u16 *at = frame + (g * 3 + r) * W + c * 6 + i;
          at[0] = (left >> (r * 3 + i)) & 1 ? y : x;
          at[3] = (right >> (r * 3 + i)) & 1 ? y : x;
        }
    }
}

static void keep_as_other(void)
{
  memcpy(other_frame, frame, sizeof(frame));
}

/* The CX II's 3:2 picture of frame (nspire_blend_turn_aspect: its groups are
 * nspire/blend_portrait.S's nspire_p_groups), mixed with other_frame or not:
 * every pixel of the screen buffer as blend_ref.c's C makes it (two lines
 * lower, line 13 black, the bands not written), as the old four-pass path's
 * (green within 3) and as the math a pixel at a time; the previous frame
 * only read. */
static void portrait_check(int mix, const char *what)
{
  char name[160];
  u32 n;

  for (n = 0; n < 240 * 320; n++)
    new_screen[n] = ref_screen[n] = SENTINEL;
  if (mix)
  {
    memcpy(old_mix_prev, other_frame, sizeof(old_mix_prev));
    memcpy(new_mix_prev, other_frame, sizeof(new_mix_prev));
    memcpy(ref_mix_prev, other_frame, sizeof(ref_mix_prev));
  }
  old_aspect(mix);
  nspire_blend_turn_aspect(new_screen, frame, mix ? new_mix_prev : NULL);
  ref_blend_turn_aspect(ref_screen, frame, mix ? ref_mix_prev : NULL);
  snprintf(name, sizeof(name), "portrait 3:2 (CX II), %s%s: as the C reference, exactly", what,
           mix ? ", with interframe blending" : "");
  check(same_as_ref_lower(), name);
  snprintf(name, sizeof(name), "portrait 3:2 (CX II), %s%s: as the old path", what,
           mix ? ", with interframe blending" : "");
  check(same_picture(13, 14, 213, name), name);
  ref_src = mix ? old_mixed : frame;
  snprintf(name, sizeof(name), "portrait 3:2 (CX II), %s%s: the math", what,
           mix ? ", with interframe blending" : "");
  check(matches_math(ref_aspect, 14, 213, name), name);
  if (mix)
  {
    snprintf(name, sizeof(name), "portrait 3:2, %s: the previous frame only read", what);
    check(!memcmp(new_mix_prev, other_frame, sizeof(other_frame)), name);
  }
}

/* speed10: the 3:2 pictures made in parts (nspire_reuse.c: runs of the
 * groups that changed, then the last line) are the whole picture, every
 * pixel of the screen buffer, for any runs (random here, from 1 to 9
 * groups); and a frame blended with itself is the frame (average_up(a, a)
 * = a: the groups whose lines didn't change are made unblended). */
static u16 parts_a[240 * 320] __attribute__((aligned(32)));
static u16 parts_b[240 * 320] __attribute__((aligned(32)));

static void parts_check(int mix, const char *what)
{
  static u32 runs = 777;   /* (its own numbers: the frames' stay as they were) */
  const u16 *prev = mix ? other_frame : NULL;
  char name[160];
  u32 g, n, side;

  for (side = 0; side < 2; side++)
  {
    for (n = 0; n < 240 * 320; n++)
      parts_a[n] = parts_b[n] = SENTINEL;
    if (side == 0)
      nspire_blend_turn_aspect(parts_a, frame, prev);
    else
      nspire_blend_aspect_landscape(parts_a, frame, prev);
    for (g = 0; g < 53; g += n)
    {
      runs = runs * 1103515245u + 12345u;
      n = 1 + (runs >> 16) % 9;
      if (g + n > 53)
        n = 53 - g;
      if (side == 0)
        nspire_blend_aspect_groups(parts_b, frame, prev, g, n);
      else
        nspire_blend_landscape_groups(parts_b, frame, prev, g, n);
    }
    if (side == 0)
      nspire_blend_aspect_last(parts_b, frame, prev);
    else
      nspire_blend_landscape_last(parts_b, frame, prev);
    snprintf(name, sizeof(name), "%s 3:2 in runs of groups, %s%s: the whole picture", side ? "landscape" : "portrait",
             what, mix ? ", with interframe blending" : "");
    check(!memcmp(parts_a, parts_b, sizeof(parts_a)), name);
    if (!mix)
    {
      for (n = 0; n < 240 * 320; n++)
        parts_b[n] = SENTINEL;
      if (side == 0)
        nspire_blend_turn_aspect(parts_b, frame, frame);
      else
        nspire_blend_aspect_landscape(parts_b, frame, frame);
      snprintf(name, sizeof(name), "%s 3:2, %s blended with itself: unblended", side ? "landscape" : "portrait",
               what);
      check(!memcmp(parts_a, parts_b, sizeof(parts_a)), name);
    }
  }
}

/* The landscape picture of frame, mixed with other_frame or not, and the
 * portrait one of the same frames. */
static void landscape_flat_check(int mix, const char *what)
{
  char name[128];

  parts_check(mix, what);
  portrait_check(mix, what);
  if (mix)
  {
    memcpy(old_mix_prev, other_frame, sizeof(old_mix_prev));
    memcpy(land_mix_prev, other_frame, sizeof(land_mix_prev));
  }
  snprintf(name, sizeof(name), "%s%s", what, mix ? ", with interframe blending" : "");
  landscape_check(mix, mix ? land_mix_prev : NULL, name);
  if (mix)
  {
    snprintf(name, sizeof(name), "landscape 3:2, %s: the previous frame only read", what);
    check(!memcmp(land_mix_prev, other_frame, sizeof(other_frame)), name);
  }
}

static void landscape_flat_cases(void)
{
  static const u16 pairs[][2] = { { 0x2145, 0xDEBA }, { 0xFFFF, 0x0000 }, { 0x0000, 0xFFFF }, { 0x8410, 0x8411 },
                                  { 0x07E0, 0xF81F }, { 0x7BEF, 0x8410 }, { 0x1234, 0x1235 }, { 0xA5A5, 0x5A5A } };
  static const u16 pattern_pairs[][2] = { { 0x2145, 0xDEBA }, { 0xFFFF, 0x0000 }, { 0x0000, 0xFFFF },
                                          { 0x1235, 0x246A }, { 0x1235, 0x891A }, { 0x1235, 0x091A } };
  static const u32 tiles[][4] = { { 6, 3, 0, 0 },      /* every column its own colour */
                                  { 3, 3, 0, 0 },      /* every block */
                                  { 12, 6, 0, 0 }, { 48, 30, 0, 0 },
                                  { 16, 12, 5, 2 }, { 5, 4, 0, 0 }, { 7, 5, 3, 1 }, { 30, 9, 2, 0 },
                                  { 6, 3, 1, 0 }, { 6, 3, 0, 1 }, { 6, 3, 2, 2 }, { 12, 3, 4, 0 }, { 6, 1, 0, 0 },
                                  { 2, 3, 0, 0 }, { 1, 160, 0, 0 }, { 240, 1, 0, 0 } };
  char name[128];
  u32 n, m;

  /* One colour all over, another (or the same) in the previous frame. */
  for (n = 0; n < sizeof(pairs) / sizeof(pairs[0]); n++)
  {
    make_plain(pairs[n][1]);
    keep_as_other();
    make_plain(pairs[n][0]);
    snprintf(name, sizeof(name), "all %04x over all %04x", pairs[n][0], pairs[n][1]);
    landscape_flat_check(0, name);
    landscape_flat_check(1, name);
    snprintf(name, sizeof(name), "all %04x twice", pairs[n][0]);
    keep_as_other();
    landscape_flat_check(1, name);
  }

  /* Flat but for one pixel of a column: in the frame, in the previous
   * frame, in both (at the same place and at different places). */
  for (n = 0; n < sizeof(pairs) / sizeof(pairs[0]); n++)
  {
    u16 base = pairs[n][0], other = pairs[n][1];

    make_plain(base);
    keep_as_other();
    make_one_off(base, other);
    snprintf(name, sizeof(name), "%04x but one pixel a column %04x", base, other);
    landscape_flat_check(0, name);
    landscape_flat_check(1, name);

    snprintf(name, sizeof(name), "%04x, before: but one pixel a column %04x", base, other);
    keep_as_other();
    make_plain(base);
    landscape_flat_check(1, name);

    snprintf(name, sizeof(name), "%04x over %04x, before: but one pixel a column %04x", other, base, other);
    make_plain(other);
    landscape_flat_check(1, name);

    snprintf(name, sizeof(name), "%04x but one pixel a column %04x, twice", base, other);
    make_one_off(base, other);
    landscape_flat_check(1, name);

    snprintf(name, sizeof(name), "%04x but one pixel a column %04x, the colours swapped before", base, other);
    make_one_off(other, base);
    keep_as_other();
    make_one_off(base, other);
    landscape_flat_check(1, name);

    /* Stripes: over one colour, under one colour, over themselves. */
    make_plain(base);
    keep_as_other();
    make_stripes(base, other);
    snprintf(name, sizeof(name), "stripes %04x %04x", base, other);
    landscape_flat_check(0, name);
    landscape_flat_check(1, name);
    snprintf(name, sizeof(name), "%04x over stripes %04x %04x", base, base, other);
    keep_as_other();
    make_plain(base);
    landscape_flat_check(1, name);
    snprintf(name, sizeof(name), "stripes %04x %04x twice", base, other);
    make_stripes(base, other);
    landscape_flat_check(1, name);
  }

  /* Columns one colour but for one or two words (the column test's words),
   * and every two-colour 3x3 block (the block test's pixels): alone, over
   * one colour, under it, over themselves, over others, and over their
   * own negative (then only the mixed picture is one colour). The last
   * three pairs: the second colour is the first turned a bit left, right,
   * and shifted right (a test comparing at the wrong shift takes them for
   * equal). */
  for (n = 0; n < sizeof(pattern_pairs) / sizeof(pattern_pairs[0]); n++)
  {
    u16 x = pattern_pairs[n][0], y = pattern_pairs[n][1];
    int how;

    make_word_patterns(x, y, 0);
    snprintf(name, sizeof(name), "word patterns %04x %04x", x, y);
    landscape_flat_check(0, name);
    keep_as_other();
    landscape_flat_check(1, name);                      /* over themselves */
    snprintf(name, sizeof(name), "%04x over word patterns %04x %04x", x, x, y);
    make_plain(x);
    landscape_flat_check(1, name);
    snprintf(name, sizeof(name), "%04x over word patterns %04x %04x", y, x, y);
    make_plain(y);
    landscape_flat_check(1, name);
    snprintf(name, sizeof(name), "word patterns %04x %04x over others", x, y);
    make_word_patterns(x, y, 353);
    landscape_flat_check(1, name);
    snprintf(name, sizeof(name), "word patterns %04x %04x over %04x", x, y, x);
    make_plain(x);
    keep_as_other();
    make_word_patterns(x, y, 0);
    landscape_flat_check(1, name);
    snprintf(name, sizeof(name), "word patterns %04x %04x over their negative", x, y);
    make_word_patterns(y, x, 0);
    keep_as_other();
    make_word_patterns(x, y, 0);
    landscape_flat_check(1, name);

    /* The picture's first column one colour but for its last word, the one
     * word only the corner test looks at (the portrait code gets to its
     * first column with the flags of another compare). */
    make_word_patterns(x, y, 25);
    snprintf(name, sizeof(name), "word patterns %04x %04x from the last word's", x, y);
    landscape_flat_check(0, name);
    keep_as_other();
    landscape_flat_check(1, name);

    /* Each of them after a column of one colour: alone, under one colour
     * (the previous frame's test after a flat column), over it. */
    for (m = 0; n < 3 && m < 2; m++)
    {
      make_word_patterns_apart(x, y, m * 1060);
      snprintf(name, sizeof(name), "word patterns %04x %04x from %u, a column of %04x before each", x, y,
               (unsigned)(m * 1060), x);
      landscape_flat_check(0, name);
      keep_as_other();
      make_plain(x);
      snprintf(name, sizeof(name), "%04x over word patterns %04x %04x from %u, a column of %04x before each", x,
               x, y, (unsigned)(m * 1060), x);
      landscape_flat_check(1, name);
      keep_as_other();
      make_word_patterns_apart(x, y, m * 1060);
      snprintf(name, sizeof(name), "word patterns %04x %04x from %u, a column of %04x before each, over %04x", x,
               y, (unsigned)(m * 1060), x, x);
      landscape_flat_check(1, name);
    }

    for (how = 0; how < 4; how++)
    {
      make_block_patterns(x, y, how);
      snprintf(name, sizeof(name), "block patterns %d, %04x %04x", how, x, y);
      landscape_flat_check(0, name);
      keep_as_other();
      landscape_flat_check(1, name);                    /* over themselves */
      snprintf(name, sizeof(name), "block patterns %d, %04x %04x, over %04x", how, x, y, x);
      make_plain(x);
      keep_as_other();
      make_block_patterns(x, y, how);
      landscape_flat_check(1, name);
      snprintf(name, sizeof(name), "block patterns %d, %04x %04x, over others", how, x, y);
      make_block_patterns(x, y, (how + 1) % 4);
      keep_as_other();
      make_block_patterns(x, y, how);
      landscape_flat_check(1, name);
      snprintf(name, sizeof(name), "block patterns %d, %04x %04x, over their negative", how, x, y);
      make_block_patterns(y, x, how);
      keep_as_other();
      make_block_patterns(x, y, how);
      landscape_flat_check(1, name);
      snprintf(name, sizeof(name), "%04x over block patterns %d, %04x %04x", y, how, x, y);
      keep_as_other();
      make_plain(y);
      landscape_flat_check(1, name);
    }
  }

  /* The previous frame's test of a column must not take what the column
   * before left in a register for one of its words: after a blended column
   * whose first line's fourth pixel is white, r10 holds that pixel spread,
   * 03E07C1F, which is also the word of the pixels 7C1F, 03E0. So: odd
   * columns one colour over upright stripes of those two, even columns
   * noise with that pixel white, twice. */
  make_frame(0);
  for (n = 0; n < W * H; n++)
  {
    u32 px = n % W;
    if (px / 6 % 2)
      frame[n] = px % 2 ? 0x03E0 : 0x7C1F;
    else if (px % 6 == 3 && n / W % 3 == 0)
      frame[n] = 0xFFFF;
  }
  keep_as_other();
  for (n = 0; n < W * H; n++)
    if (n % W / 6 % 2)
      frame[n] = 0x2145;
  landscape_flat_check(1, "one colour over stripes 7C1F 03E0 beside noise");

  /* The same for the portrait picture's registers (nspire/blend_portrait.S):
   * after a blended column whose sixth pixel is white in its second and
   * third lines, r10 holds that pixel unpacked, 07E0F81F, which is also the
   * word of the pixels F81F, 07E0. */
  make_frame(0);
  for (n = 0; n < W * H; n++)
  {
    u32 px = n % W;
    if (px / 6 % 2)
      frame[n] = px % 2 ? 0x07E0 : 0xF81F;
    else if (px % 6 == 5 && n / W % 3 != 0)
      frame[n] = 0xFFFF;
  }
  keep_as_other();
  for (n = 0; n < W * H; n++)
    if (n % W / 6 % 2)
      frame[n] = 0x2145;
  landscape_flat_check(1, "one colour over stripes F81F 07E0 beside noise");

  /* Tiles of one colour each, over every other kind of tiles and noise. */
  for (n = 0; n < sizeof(tiles) / sizeof(tiles[0]); n++)
  {
    make_tiles(tiles[n][0], tiles[n][1], tiles[n][2], tiles[n][3]);
    snprintf(name, sizeof(name), "%ux%u tiles from (-%u, -%u)", (unsigned)tiles[n][0], (unsigned)tiles[n][1],
             (unsigned)tiles[n][2], (unsigned)tiles[n][3]);
    landscape_flat_check(0, name);
    for (m = 0; m < sizeof(tiles) / sizeof(tiles[0]); m++)
    {
      make_tiles(tiles[m][0], tiles[m][1], tiles[m][2], tiles[m][3]);
      keep_as_other();
      make_tiles(tiles[n][0], tiles[n][1], tiles[n][2], tiles[n][3]);
      snprintf(name, sizeof(name), "%ux%u tiles from (-%u, -%u) over %ux%u from (-%u, -%u)",
               (unsigned)tiles[n][0], (unsigned)tiles[n][1], (unsigned)tiles[n][2], (unsigned)tiles[n][3],
               (unsigned)tiles[m][0], (unsigned)tiles[m][1], (unsigned)tiles[m][2], (unsigned)tiles[m][3]);
      landscape_flat_check(1, name);
    }
    make_frame(0);
    keep_as_other();
    make_tiles(tiles[n][0], tiles[n][1], tiles[n][2], tiles[n][3]);
    snprintf(name, sizeof(name), "%ux%u tiles from (-%u, -%u) over noise", (unsigned)tiles[n][0],
             (unsigned)tiles[n][1], (unsigned)tiles[n][2], (unsigned)tiles[n][3]);
    landscape_flat_check(1, name);
    keep_as_other();
    make_frame(0);
    snprintf(name, sizeof(name), "noise over %ux%u tiles from (-%u, -%u)", (unsigned)tiles[n][0],
             (unsigned)tiles[n][1], (unsigned)tiles[n][2], (unsigned)tiles[n][3]);
    landscape_flat_check(1, name);
  }

  /* Noise over noise. */
  for (n = 0; n < 3; n++)
  {
    make_frame(0);
    keep_as_other();
    make_frame(n == 2 ? 4 : 0);
    landscape_flat_check(1, n == 2 ? "extreme colors over noise" : "noise over noise");
  }
}

int main(void)
{
  char name[128];
  int kind, mix, n;

  for (kind = 0; kind < KINDS; kind++)
  {
    make_frame(kind);

    old_aspect(0);
    for (n = 0; n < 240 * 320; n++)
      new_screen[n] = SENTINEL;
    nspire_blend_turn_aspect(new_screen, frame, NULL);
    for (n = 0; n < 240 * 320; n++)
      ref_screen[n] = SENTINEL;
    ref_blend_turn_aspect(ref_screen, frame, NULL);
    snprintf(name, sizeof(name), "3:2, %s: as before (two lines lower), exactly", kind_names[kind]);
    check(same_as_ref_lower(), name);
    snprintf(name, sizeof(name), "3:2, %s", kind_names[kind]);
    check(same_picture(13, 14, 213, name), name);
    ref_src = frame;
    snprintf(name, sizeof(name), "3:2, %s: the math", kind_names[kind]);
    check(matches_math(ref_aspect, 14, 213, name), name);
    snprintf(name, sizeof(name), "3:2, %s: line 13 black", kind_names[kind]);
    check(lines_are(new_screen, 13, 14, 0), name);
    snprintf(name, sizeof(name), "3:2, %s: the text bands untouched", kind_names[kind]);
    check(lines_are(new_screen, 0, 13, SENTINEL) && lines_are(new_screen, 227, 240, SENTINEL), name);

    landscape_check(0, NULL, kind_names[kind]);

    old_full(0);
    nspire_blend_turn_full(new_screen, frame, NULL);
    snprintf(name, sizeof(name), "fullscreen, %s", kind_names[kind]);
    check(same_picture(0, 0, 240, name), name);
    snprintf(name, sizeof(name), "fullscreen, %s: the math", kind_names[kind]);
    check(matches_math(ref_full, 0, 240, name), name);
  }

  /* Interframe blending: a few frames in a row, each path with its own
   * previous frame (all start from 0xFFFF). The new path only reads it:
   * the caller keeps the frame drawn before (nspire_video_present.c draws
   * into two buffers in turn), here by copying. */
  for (mix = 0; mix < 2; mix++)
  {
    memset(old_mix_prev, 0xFF, sizeof(old_mix_prev));
    memset(new_mix_prev, 0xFF, sizeof(new_mix_prev));
    memset(ref_mix_prev, 0xFF, sizeof(ref_mix_prev));
    for (n = 0; n < 4; n++)
    {
      make_frame(n == 2 ? 2 : n == 3 ? 4 : 0);
      memcpy(prev_copy, new_mix_prev, sizeof(prev_copy));
      memset(new_screen, 0x5A, sizeof(new_screen));
      memset(ref_screen, 0x5A, sizeof(ref_screen));
      if (mix == 0)
      {
        old_aspect(1);
        nspire_blend_turn_aspect(new_screen, frame, new_mix_prev);
        ref_blend_turn_aspect(ref_screen, frame, ref_mix_prev);
        snprintf(name, sizeof(name), "3:2 with interframe blending, frame %d: as before (two lines lower), exactly",
                 n + 1);
        check(same_as_ref_lower(), name);
        snprintf(name, sizeof(name), "3:2 with interframe blending, frame %d", n + 1);
        check(same_picture(13, 14, 213, name), name);
        ref_src = old_mixed;
        snprintf(name, sizeof(name), "3:2 with interframe blending, frame %d: the math", n + 1);
        check(matches_math(ref_aspect, 14, 213, name), name);
      }
      else
      {
        old_full(1);
        nspire_blend_turn_full(new_screen, frame, new_mix_prev);
        ref_blend_turn_full(ref_screen, frame, ref_mix_prev);
        snprintf(name, sizeof(name), "fullscreen with interframe blending, frame %d: as before, exactly", n + 1);
        check(!memcmp(new_screen, ref_screen, sizeof(new_screen)), name);
        snprintf(name, sizeof(name), "fullscreen with interframe blending, frame %d", n + 1);
        check(same_picture(0, 0, 240, name), name);
        ref_src = old_mixed;
        snprintf(name, sizeof(name), "fullscreen with interframe blending, frame %d: the math", n + 1);
        check(matches_math(ref_full, 0, 240, name), name);
      }
      snprintf(name, sizeof(name), "%s: the previous frame only read, frame %d", mix ? "fullscreen" : "3:2", n + 1);
      check(!memcmp(new_mix_prev, prev_copy, sizeof(prev_copy)), name);
      memcpy(new_mix_prev, frame, sizeof(frame));
    }
  }

  /* The landscape 3:2 with interframe blending, a few frames in a row. */
  memset(old_mix_prev, 0xFF, sizeof(old_mix_prev));
  memset(land_mix_prev, 0xFF, sizeof(land_mix_prev));
  for (n = 0; n < 4; n++)
  {
    make_frame(n == 2 ? 2 : n == 3 ? 4 : 1);
    memcpy(prev_copy, land_mix_prev, sizeof(prev_copy));
    snprintf(name, sizeof(name), "with interframe blending, frame %d", n + 1);
    landscape_check(1, land_mix_prev, name);
    snprintf(name, sizeof(name), "landscape 3:2: the previous frame only read, frame %d", n + 1);
    check(!memcmp(land_mix_prev, prev_copy, sizeof(prev_copy)), name);
    memcpy(land_mix_prev, frame, sizeof(frame));
  }

  landscape_flat_cases();

  printf("%s: %d of %d checks passed; green differed by up to %d (of 63)\n",
         failures ? "FAIL" : "PASS", checks - failures, checks, worst_green);
  return failures != 0;
}
