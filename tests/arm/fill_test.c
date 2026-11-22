/*
 * video_fill_u16_strip_arm (nspire/scanline_fill16.S), video.cc's backdrop
 * fill, at every start pixel and length, run under qemu-arm like the other
 * ARM tests (tests/arm/run.sh).
 *
 * The fill used to store 16 bytes at a time from wherever it started. A
 * window edge on an odd pixel (Pokemon battle wipes, door transitions)
 * started it 2 bytes off a word, and the calculator checks alignment: it
 * crashed. qemu stops the same way (SIGBUS), so the old fill fails here.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint16_t u16;
typedef uint32_t u32;

void video_fill_u16_strip_arm(u16 *dst, u32 nh, u32 value);
void video_merge_alpha_arm(u16 *dst, const u32 *src, u32 n, u32 ab_low, const u16 *pal);
void video_merge_obj_tab_arm(u16 *dst, const u32 *src, u32 n, const u32 *tab, const u16 *pal);

#define SENTINEL 0x5A5A
#define LINE 240

static u16 buffer[LINE + 64] __attribute__((aligned(32)));
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

/* Fills [start, start + count) of the line, which begins 16 pixels into
 * the buffer, and checks every pixel of the buffer. */
static void fill_and_check(u32 start, u32 count, u32 value)
{
  u16 *line = buffer + 16;
  char what[96];
  u32 i, wrong = 0;

  for (i = 0; i < sizeof(buffer) / sizeof(buffer[0]); i++)
    buffer[i] = SENTINEL;
  video_fill_u16_strip_arm(line + start, count, value);
  for (i = 0; i < sizeof(buffer) / sizeof(buffer[0]); i++)
  {
    u32 pixel = i - 16;
    u16 want = (i >= 16 && pixel >= start && pixel < start + count) ? (u16)value : SENTINEL;
    if (buffer[i] != want)
      wrong++;
  }
  snprintf(what, sizeof(what), "fill of %u pixels from pixel %u: %u wrong", (unsigned)count,
           (unsigned)start, (unsigned)wrong);
  check(wrong == 0, what);
}

/* (r19-fx) video_merge_alpha_arm against video.cc merge_blend's C (its
 * OBJ_BLEND and BLEND_ONLY loops, RGB565), on random pixel pairs and
 * palettes at every pair of factors 0..16 and each blending-bit set. */
#define BLND_MSK 0x07E0F81Fu
static u32 rnd_state = 12345;
static u32 rnd(void)
{
  rnd_state = rnd_state * 1103515245u + 12345u;
  return rnd_state >> 1 ^ rnd_state << 15;
}

static u16 merge_ref(u32 pp, u32 a, u32 b, u32 low, const u16 *pal)
{
  if ((pp & 0x04000000) && (pp & low))
  {
    u16 p1 = pal[pp & 0x1FF], p2 = pal[(pp >> 16) & 0x1FF];
    u32 p1e = (p1 | (p1 << 16)) & BLND_MSK;
    u32 p2e = (p2 | (p2 << 16)) & BLND_MSK;
    u32 pfe = ((p1e * a) + (p2e * b)) >> 4;
    if (a + b > 16)
    {
      if (pfe & 0x08000000) pfe |= 0x07E00000;
      if (pfe & 0x00010000) pfe |= 0x0000F800;
      if (pfe & 0x00000020) pfe |= 0x0000001F;
    }
    pfe &= BLND_MSK;
    return (u16)((pfe >> 16) | pfe);
  }
  return pal[pp & 0x1FF];
}

static void merge_checks(void)
{
  static u16 pal[512], out[256 + 8];
  static u32 src[256];
  static const u32 lows[] = { 0x800, 0x200, 0xA00, 0 };
  u32 a, b, l, i, n, wrong = 0, runs = 0;
  char what[96];

  for (a = 0; a <= 16; a++)
    for (b = 0; b <= 16; b++)
      for (l = 0; l < 4; l++)
      {
        for (i = 0; i < 512; i++)
          pal[i] = (u16)rnd();
        pal[1] = 0xFFFF; pal[2] = 0xF7DE; pal[0x101] = 0xFFFF;   /* saturating whites */
        n = 1 + rnd() % 240;
        for (i = 0; i < n; i++)
        {
          src[i] = rnd() & 0x0FFF0FFF;   /* random flags and indices */
          if (i % 7 == 0) src[i] = 0x04000801 | (2u << 16);
        }
        out[n] = SENTINEL;
        video_merge_alpha_arm(out, src, n, a | b << 8 | lows[l] << 16, pal);
        for (i = 0; i < n; i++)
          wrong += out[i] != merge_ref(src[i], a, b, lows[l], pal);
        wrong += out[n] != SENTINEL;
        runs++;
      }
  snprintf(what, sizeof(what), "merge_blend's alpha pass in ARM: %u wrong pixels in %u lines",
           (unsigned)wrong, (unsigned)runs);
  check(wrong == 0, what);

  /* The ST objects' pass with its table (video.cc builds it the same way):
   * pairs as render_object's stacked stores make them, a blended pixel's
   * top an object (0x100 | i, 0x800) and the pixel below not (< 0x100). */
  {
    static u32 tab[512];
    wrong = runs = 0;
    for (a = 0; a <= 16; a++)
      for (b = 0; b <= 16; b++)
      {
        for (i = 0; i < 512; i++)
          pal[i] = (u16)rnd();
        pal[1] = 0xFFFF; pal[0x101] = 0xFFFF; pal[0x102] = 0xF7DE;
        for (i = 0; i < 256; i++)
        {
          u32 o = pal[0x100 + i], g = pal[i];
          tab[i] = ((o | (o << 16)) & BLND_MSK) * a;
          tab[256 + i] = ((g | (g << 16)) & BLND_MSK) * b;
        }
        n = 1 + rnd() % 240;
        for (i = 0; i < n; i++)
        {
          u32 r = rnd(), below = (r >> 16) & 0x6FF;   /* index < 0x100, flags 0x200/0x400 */
          switch (r % 4)
          {
            case 0: src[i] = 0x900 | (r & 0x6FF & ~0x100) | below << 16; break;   /* an ST object */
            case 1: src[i] = 0x901 | 0x04010000; break;                    /* white over white */
            case 2: src[i] = (r & 0x7FF) | below << 16; break;            /* no ST object */
            default: src[i] = (r & 0x6FF) | 0x100 | below << 16; break;  /* a plain object */
          }
        }
        out[n] = SENTINEL;
        video_merge_obj_tab_arm(out, src, n, tab, pal);
        for (i = 0; i < n; i++)
          wrong += out[i] != merge_ref(src[i], a, b, 0x800, pal);
        wrong += out[n] != SENTINEL;
        runs++;
      }
    snprintf(what, sizeof(what), "merge_blend's ST objects' pass with its table in ARM: %u wrong pixels in %u lines",
             (unsigned)wrong, (unsigned)runs);
    check(wrong == 0, what);
  }
}

int main(void)
{
  merge_checks();

  u32 start, count;

  /* Every start pixel within a few words, every short length (the tail
   * loop and the 8-pixel stores), and whole lines. */
  for (start = 0; start < 8; start++)
    for (count = 0; count <= 40; count++)
      fill_and_check(start, count, 0xABCD1234u + start * 977u + count);
  for (start = 0; start < 8; start++)
    fill_and_check(start, LINE - start, 0x7FFFu);
  /* Window edges as a battle wipe has them, anywhere on the line. */
  for (start = 1; start < LINE; start += 7)
    fill_and_check(start, (LINE - start) / 3, 0x0000u);

  printf("%s: scanline fill: %d of %d checks passed\n", failures ? "FAIL" : "PASS",
         checks - failures, checks);
  return failures != 0;
}
