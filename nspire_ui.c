/* The menus' drawing kit: see nspire_ui.h. */

#include "nspire_ui.h"
#include "nspire.h"
#include "nspire_gui_video.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "nspire_ui_font.h"
#define UI_ICON_BITS
#include "nspire_ui_icons.h"

u16 *ui_fb = nspire_landscape;
char ui_shown[32];
int ui_battery = -1;
int ui_charging;

/* ---- The look's colours ----
 * Black and white only: a white ground, black type, black
 * hairlines and outlines; the picked thing a black block with white type
 * on it. No light grey anywhere: the one grey is a dark one, and only for
 * what is deliberately in the back (r16: values and facts are black): the
 * tab you are not on and where a page is opened from, a list's section
 * names, the keys on the keypad that do nothing, what isn't there (the link
 * card's absent cable), the Controller page's raw details. */
#define C UI_RGB
#define PLAIN_THEME "Plain", \
  /* ground              ground (same)        tile (outlined)      line */ \
  C(0xFF, 0xFF, 0xFF), C(0xFF, 0xFF, 0xFF), C(0xFF, 0xFF, 0xFF), C(0x00, 0x00, 0x00), \
  /* muted               text2                text                 picked */ \
  C(0x54, 0x54, 0x54), C(0x00, 0x00, 0x00), C(0x00, 0x00, 0x00), C(0x00, 0x00, 0x00), \
  /* ink                 ink2                 destroys             on it */ \
  C(0xFF, 0xFF, 0xFF), C(0xFF, 0xFF, 0xFF), C(0x00, 0x00, 0x00), C(0xFF, 0xFF, 0xFF), \
  /* detail              mark                 label                second               shadow */ \
  C(0xFF, 0xFF, 0xFF), C(0x00, 0x00, 0x00), C(0x54, 0x54, 0x54), C(0x00, 0x00, 0x00), C(0xFF, 0xFF, 0xFF)

const struct ui_theme ui_themes[UI_THEMES] = { { PLAIN_THEME } };
const char *const ui_theme_names[UI_THEMES] = { "Plain" };
struct ui_theme ui_theme = { PLAIN_THEME };
#undef C

void ui_theme_set(unsigned n)
{
  ui_theme = ui_themes[n < UI_THEMES ? n : 0];
}

void ui_begin(void)
{
  ui_fb = nspire_landscape;
}

int ui_defer_present;

static void band_battery(void);

void ui_present(void)
{
  band_battery();
  if (!ui_defer_present)
    flip_screen_ui();
}

/* ---- Pixels ---- */

/* fg over bg, alpha 0-16, all three channels at once: the pixels spread to
 * 0x07E0F81F leave room for the products between the fields. */
static inline u16 blend(u16 bg, u16 fg, u32 a)
{
  u32 b = (bg | ((u32)bg << 16)) & 0x07E0F81Fu;
  u32 f = (fg | ((u32)fg << 16)) & 0x07E0F81Fu;
  u32 r = ((f * a + b * (16 - a)) >> 4) & 0x07E0F81Fu;
  return (u16)(r | (r >> 16));
}

/* 4-bit coverage -> alpha 0-16 */
static const u8 alpha16[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16 };

static int clip(int *x, int *y, int *w, int *h)
{
  if (*x < 0) { *w += *x; *x = 0; }
  if (*y < 0) { *h += *y; *y = 0; }
  if (*x + *w > UI_W) *w = UI_W - *x;
  if (*y + *h > UI_H) *h = UI_H - *y;
  return *w > 0 && *h > 0;
}

void ui_fill(u16 color)
{
  u32 c = color | ((u32)color << 16), *p = (u32 *)ui_fb;
  int n = UI_W * UI_H / 2;
  while (n-- > 0)
    *p++ = c;
}

/* (r16) The ground around a rectangle something opaque is drawn over next (the paused menu's picture): its
 * pixels are written once, not twice. x and w even. */
void ui_fill_around(u16 color, int x, int y, int w, int h)
{
  u32 c = color | ((u32)color << 16), *p = (u32 *)ui_fb;
  int n, j;
  for (n = y * UI_W / 2; n-- > 0;)
    *p++ = c;
  for (j = 0; j < h; j++)
  {
    for (n = x / 2; n-- > 0;)
      *p++ = c;
    p += w / 2;
    for (n = (UI_W - x - w) / 2; n-- > 0;)
      *p++ = c;
  }
  for (n = (UI_H - y - h) * UI_W / 2; n-- > 0;)
    *p++ = c;
}

void ui_rect(int x, int y, int w, int h, u16 color)
{
  int i, j;
  if (!clip(&x, &y, &w, &h))
    return;
  for (j = 0; j < h; j++)
  {
    u16 *row = ui_fb + (y + j) * UI_W + x;
    for (i = 0; i < w; i++)
      row[i] = color;
  }
}

void ui_rect_alpha(int x, int y, int w, int h, u16 color, int alpha)
{
  int i, j;
  if (alpha >= 16)
  {
    ui_rect(x, y, w, h, color);
    return;
  }
  if (alpha <= 0 || !clip(&x, &y, &w, &h))
    return;
  for (j = 0; j < h; j++)
  {
    u16 *row = ui_fb + (y + j) * UI_W + x;
    for (i = 0; i < w; i++)
      row[i] = blend(row[i], color, (u32)alpha);
  }
}

void ui_hline(int x, int y, int w, u16 color)
{
  ui_rect(x, y, w, 1, color);
}

static void put(int x, int y, u16 color, int a)
{
  if (a <= 0 || x < 0 || y < 0 || x >= UI_W || y >= UI_H)
    return;
  ui_fb[y * UI_W + x] = a >= 16 ? color : blend(ui_fb[y * UI_W + x], color, (u32)a);
}

/* ---- Small corners ---- */

/* Coverage (0-16) of the pixels of a top-left corner of radius 1-3, from 4x4
 * points a pixel (worked out for a quarter circle of that radius). */
static const u8 corner1[1][1] = { { 10 } };
static const u8 corner2[2][2] = { { 3, 13 }, { 13, 16 } };
static const u8 corner3[3][3] = { { 0, 7, 14 }, { 7, 16, 16 }, { 14, 16, 16 } };

static int corner_cover(int i, int j, int r)
{
  switch (r)
  {
  case 1: return corner1[j][i];
  case 2: return corner2[j][i];
  default: return corner3[j][i];
  }
}

/* Coverage (0-16) of pixel (i, j) of a w x h rectangle with corners r. */
static int rr_cover(int i, int j, int w, int h, int r)
{
  int ci, cj;
  if (i < 0 || j < 0 || i >= w || j >= h)
    return 0;
  if (r <= 0)
    return 16;
  ci = i < r ? i : i >= w - r ? w - 1 - i : -1;
  cj = j < r ? j : j >= h - r ? h - 1 - j : -1;
  if (ci < 0 || cj < 0)
    return 16;
  return corner_cover(ci, cj, r);
}

void ui_rrect_alpha(int x, int y, int w, int h, int r, u16 color, int alpha)
{
  int j;
  if (r > 3)
    r = 3;
  if (r > w / 2)
    r = w / 2;
  if (r > h / 2)
    r = h / 2;
  if (r <= 0)
  {
    ui_rect_alpha(x, y, w, h, color, alpha);
    return;
  }
  /* the middle rows whole, the corner rows pixel by pixel */
  ui_rect_alpha(x, y + r, w, h - 2 * r, color, alpha);
  for (j = 0; j < h; j += j == r - 1 ? h - 2 * r + 1 : 1)
  {
    int i;
    ui_rect_alpha(x + r, y + j, w - 2 * r, 1, color, alpha);
    for (i = 0; i < r; i++)
    {
      put(x + i, y + j, color, rr_cover(i, j, w, h, r) * alpha / 16);
      put(x + w - 1 - i, y + j, color, rr_cover(w - 1 - i, j, w, h, r) * alpha / 16);
    }
  }
}

void ui_rrect(int x, int y, int w, int h, int r, u16 color)
{
  ui_rrect_alpha(x, y, w, h, r, color, 16);
}

void ui_rframe(int x, int y, int w, int h, int r, u16 color)
{
  int i, j;
  for (j = 0; j < h; j++)
    for (i = 0; i < w; i++)
    {
      int a;
      if (j > r && j < h - 1 - r && i > 0 && i < w - 1)
      {
        i = w - 2;   /* only the edges in the middle rows */
        continue;
      }
      a = rr_cover(i, j, w, h, r) - rr_cover(i - 1, j - 1, w - 2, h - 2, r > 1 ? r - 1 : 0);
      put(x + i, y + j, color, a);
    }
}

/* A filled circle (the controller's drawing): 4x4 points a pixel at its edge. */
void ui_disc(int cx2, int cy2, int r2, u16 color)
{
  /* centre and radius in half pixels */
  int x, y, x0 = (cx2 - r2) / 2 - 1, x1 = (cx2 + r2) / 2 + 1, y0 = (cy2 - r2) / 2 - 1, y1 = (cy2 + r2) / 2 + 1;
  for (y = y0; y <= y1; y++)
    for (x = x0; x <= x1; x++)
    {
      int n = 0, i, j;
      for (j = 0; j < 4; j++)
        for (i = 0; i < 4; i++)
        {
          int dx = (x * 8 + 2 * i + 1) - cx2 * 4, dy = (y * 8 + 2 * j + 1) - cy2 * 4;
          n += dx * dx + dy * dy <= r2 * r2 * 16;
        }
      put(x, y, color, n);
    }
}

/* ---- Text ---- */

static int clip_x0 = 0, clip_x1 = UI_W;

void ui_clip(int x0, int x1)
{
  clip_x0 = x0 < 0 ? 0 : x0;
  clip_x1 = x1 > UI_W ? UI_W : x1;
}

static int kern(const struct ui_font *f, int a, int b)
{
  u16 pair = (u16)((a << 8) | b);
  int lo = 0, hi = f->kern_count - 1;
  while (lo <= hi)
  {
    int mid = (lo + hi) / 2;
    if (f->kern[mid].pair == pair)
      return f->kern[mid].adjust;
    if (f->kern[mid].pair < pair)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return 0;
}

static int code_of(unsigned char c)
{
  /* r19: no small letters, anywhere: every text is set in capitals, whatever it is given as (small
   * letters mixed with capitals read as two type sizes) */
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 'A';
  if ((c >= 32 && c < 127) || (c >= 1 && c <= 8))
    return c;
  /* a non-breaking space (UI_NBSP) looks like a space */
  return c == '\t' || c == '\n' || c == 0xa0 ? ' ' : '?';
}

/* 4-bit rows (two pixels a byte) at x, y */
static void draw_bits(const u8 *bits, int w, int h, int x, int y, u16 color)
{
  int i, j, bpr = (w + 1) / 2;
  for (j = 0; j < h; j++, bits += bpr)
  {
    int py = y + j;
    u16 *row;
    if (py < 0 || py >= UI_H)
      continue;
    row = ui_fb + py * UI_W;
    for (i = 0; i < w; i++)
    {
      int px = x + i, a = (bits[i >> 1] >> ((i & 1) * 4)) & 15;
      if (!a || px < clip_x0 || px >= clip_x1)
        continue;
      row[px] = a == 15 ? color : blend(row[px], color, alpha16[a]);
    }
  }
}

/* Lays s out from x (in 1/64 pixel); draws it when draw is set. Returns the
 * pen's end, in 1/64 pixel. */
static int track64;   /* extra space after each character, in 1/64 pixel (ui_tracking) */

void ui_tracking(int extra64)
{
  track64 = extra64;
}

static int layout(int x64, int y, const char *s, const struct ui_font *f, u16 color, int draw)
{
  int pen = x64, prev = 0;
  for (; *s; s++)
  {
    int c = code_of((unsigned char)*s);
    if (prev >= 32 && c >= 32)
      pen += kern(f, prev, c);
    if (draw)
    {
      /* the pen to the nearest of the font's positions between pixels */
      /* (r19: a negative pen, a text running out of its place at the left, floors: C's % and / don't) */
      int q = (pen * f->phases + 32) >> 6;
      int ph = ((q % f->phases) + f->phases) % f->phases;
      const struct ui_glyph *g = &f->glyphs[c * f->phases + ph];
      if (g->w)
        draw_bits(f->bits + g->bits, g->w, g->h, (q - ph) / f->phases + g->x, y + g->y, color);
    }
    pen += f->advance[c] + track64;
    prev = c;
  }
  return pen;
}

int ui_text(int x, int y, const char *s, const struct ui_font *f, u16 color)
{
  return (layout(x * 64, y, s, f, color, 1) + 32) >> 6;
}

int ui_text_width(const char *s, const struct ui_font *f)
{
  return (layout(0, 0, s, f, 0, 0) + 32) >> 6;
}

int ui_text_right(int right, int y, const char *s, const struct ui_font *f, u16 color)
{
  return ui_text(right - ui_text_width(s, f), y, s, f, color);
}

int ui_text_center(int center, int y, const char *s, const struct ui_font *f, u16 color)
{
  return ui_text(center - ui_text_width(s, f) / 2, y, s, f, color);
}

/* ---- The three voices: big words, the plain cut (ui_text with ui_font_body), labels ---- */

const char *ui_caps(const char *s)
{
  static char out[6][200];
  static unsigned next;
  char *o = out[next++ % 6];
  size_t i;
  for (i = 0; s[i] && i + 1 < sizeof(out[0]); i++)
    o[i] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 'a' + 'A' : s[i]);
  o[i] = 0;
  return o;
}

static int shadow_on;   /* ui_shadow */

void ui_shadow(int on)
{
  shadow_on = on;
}

int ui_big(int x, int y, const char *s, u16 color)
{
  if (shadow_on)
    ui_text(x + 2, y + 2, ui_caps(s), &ui_font_large, UI_SHADOW);
  return ui_text(x, y, ui_caps(s), &ui_font_large, color);
}

int ui_big_width(const char *s)
{
  return ui_text_width(ui_caps(s), &ui_font_large);
}

int ui_big_number(int x, int y, const char *s, u16 color)
{
  if (shadow_on)
    ui_text(x + 2, y + 2, ui_caps(s), &ui_font_d125, UI_SHADOW);
  return ui_text(x, y, ui_caps(s), &ui_font_d125, color);
}

int ui_big_narrow_width(const char *s)
{
  return ui_text_width(ui_caps(s), &ui_font_d62);
}

/* 4-bit rows drawn k256/256 times as wide (under 256: narrower), each row
 * read between its pixels: only for a word too long for the narrowest cut,
 * squeezed a little. */
static void draw_bits_squeezed(const u8 *bits, int w, int h, int x, int y, u16 color, int k256)
{
  /* each column's place in a row, 1/256 pixel from pixel 0's middle: worked out once a letter (the CPU has
   * no divide) */
  static s16 place[64];
  int dw = (w * k256 + 255) >> 8, i, j, bpr = (w + 1) / 2;
  if (dw > (int)(sizeof(place) / sizeof(place[0])))
    dw = (int)(sizeof(place) / sizeof(place[0]));
  for (i = 0; i < dw; i++)
    place[i] = (s16)((2 * i + 1) * 32768 / k256 - 128);
  for (j = 0; j < h; j++, bits += bpr)
  {
    int py = y + j;
    u16 *row;
    if (py < 0 || py >= UI_H)
      continue;
    row = ui_fb + py * UI_W;
    for (i = 0; i < dw; i++)
    {
      int px = x + i, u = place[i];
      int i0 = u >> 8, t = u & 255, c0, c1, c;
      if (px < clip_x0 || px >= clip_x1)
        continue;
      c0 = i0 < 0 || i0 >= w ? 0 : (bits[i0 >> 1] >> ((i0 & 1) * 4)) & 15;
      c1 = i0 + 1 < 0 || i0 + 1 >= w ? 0 : (bits[(i0 + 1) >> 1] >> (((i0 + 1) & 1) * 4)) & 15;
      c = ((c0 * 256 + (c1 - c0) * t) * 16 + 1920) / 3840;   /* 0 to 16 */
      if (c)
        row[px] = c >= 16 ? color : blend(row[px], color, (u32)c);
    }
  }
}

/* A big word made width wide the way a poster does it: in the widest of the
 * four cuts that fits, its letters then spaced out to the width. A word so
 * short that this would pull its letters apart (over BIG_SPACE_MAX each) is
 * left as the widest cut sets it; one too long for the narrowest cut is
 * squeezed a little (to 0.8 at most), and one too long even for that is cut
 * short ("...") at the cut's own width. */
#define BIG_SPACE_MAX (8 * 64)
int ui_big_wide(int x, int y, const char *s, u16 color, int width)
{
  static const struct ui_font *const cuts[4] = { &ui_font_d125, &ui_font_d100, &ui_font_d75, &ui_font_d62 };
  const struct ui_font *f = cuts[3];
  const char *caps = ui_caps(s), *c;
  char fit[200];
  int natural = 0, n = 0, k, k256, pen = 0, prev = 0;

  if (width <= 0 || !caps[0])
    return x;
  if (shadow_on)
  {
    shadow_on = 0;
    ui_big_wide(x + 2, y + 2, s, UI_SHADOW, width);
    shadow_on = 1;
  }
  for (c = caps; *c; c++)
    n++;
  for (k = 0; k < 4; k++)
  {
    natural = layout(0, 0, caps, cuts[k], 0, 0) - 19;   /* (without the space after the last letter: 0.3 px) */
    /* a cut a hair too wide (8% at most) is taken and squeezed that hair: better than the next narrower
     * one with wide gaps between its letters */
    if (natural <= width * 64 * 27 / 25)
    {
      f = cuts[k];
      break;
    }
  }
  if (natural <= width * 64)
  {
    /* it fits: the room that is left goes between the letters */
    int space = n > 1 ? (width * 64 - natural) / (n - 1) : 0, end;
    track64 = space > BIG_SPACE_MAX ? 0 : space;
    end = ui_text(x, y, caps, f, color);
    track64 = 0;
    return end;
  }
  if (natural > width * 64 * 5 / 4)
  {
    /* too long even squeezed: cut short, at the narrowest cut's own width */
    return ui_text(x, y, ui_fit(caps, f, width, fit, sizeof(fit)), f, color);
  }
  /* a hair too wide, or a little too long for the narrowest cut: squeezed */
  k256 = (int)((long long)width * 64 * 256 / natural);
  for (c = caps; *c; c++)
  {
    int ch = code_of((unsigned char)*c);
    const struct ui_glyph *g = &f->glyphs[ch * f->phases];
    if (prev >= 32 && ch >= 32)
      pen += kern(f, prev, ch);
    if (g->w)
      draw_bits_squeezed(f->bits + g->bits, g->w, g->h, x + (int)(((long long)(pen + g->x * 64) * k256) >> 14),
                         y + g->y, color, k256);
    pen += f->advance[ch];
    prev = ch;
  }
  return x + (int)(((long long)pen * k256) >> 14);
}

#define LABEL_TRACK 0    /* a label: the monospaced capitals as they are */

int ui_label_width(const char *s)
{
  int w;
  track64 = LABEL_TRACK;
  w = ui_text_width(ui_caps(s), &ui_font_small_strong);
  track64 = 0;
  return w > 0 ? w - 1 : 0;   /* the space after the last letter isn't the word's */
}

int ui_label(int x, int y, const char *s, u16 color)
{
  int end;
  track64 = LABEL_TRACK;
  end = ui_text(x, y, ui_caps(s), &ui_font_small_strong, color);
  track64 = 0;
  return end - 1;
}

int ui_label_right(int right, int y, const char *s, u16 color)
{
  return ui_label(right - ui_label_width(s), y, s, color);
}

/* The coverage (0-15) of pixel (i, j) of a glyph's rows; nothing outside it. */
static int cover_at(const u8 *bits, int w, int h, int i, int j)
{
  if (i < 0 || j < 0 || i >= w || j >= h)
    return 0;
  return (bits[j * ((w + 1) / 2) + (i >> 1)] >> ((i & 1) * 4)) & 15;
}

#define WORD_SCALE 3

static int isqrt(int v)
{
  int r = 0, bit = 1 << 14;
  while (bit)
  {
    if ((r + bit) * (r + bit) <= v)
      r += bit;
    bit >>= 1;
  }
  return r;
}

/* ---- The ground ---- */

/* A circle's line, one pixel wide, white at alpha (0-16), between rows y0
 * and y1 and left of x1: a court's line. */
static void court_line(int cx, int cy, int r, int alpha, int y0, int y1, int x1)
{
  int py, prev = -1;
  for (py = cy - r; py <= cy + r; py++)
  {
    int dy = py - cy, dx = isqrt(r * r - dy * dy), a, b, l, w;
    /* from the row before's end to this row's, so the line has no gaps where it runs flat */
    a = prev < 0 ? dx : prev < dx ? prev + 1 : dx;
    b = prev < 0 ? dx : prev < dx ? dx : prev - 1 < dx ? dx : prev - 1;
    prev = dx;
    if (py < y0 || py >= y1)
      continue;
    l = cx - b;
    w = b - a + 1;
    if (l < x1)
      ui_rect_alpha(l, py, l + w > x1 ? x1 - l : w, 1, 0xFFFF, alpha);
    l = cx + a;
    if (l < x1)
      ui_rect_alpha(l, py, l + w > x1 ? x1 - l : w, 1, 0xFFFF, alpha);
  }
}

void ui_lean(int x, int w, int y0, int y1, u16 color, int alpha)
{
  int y;
  for (y = y0 < 0 ? 0 : y0; y < y1 && y < UI_H; y++)
    ui_rect_alpha(x + UI_LEAN(y), y, w, 1, color, alpha);
}

/* The lit ground between rows y0 and y1, left of the x cut gives each row
 * (all of it when cut is 0): charcoal under a spotlight above the middle
 * of the top edge, to black at the bottom and the far corners. 8 bits a
 * channel, dithered (4x4) to the screen's 5 and 6. */
static void ground_rows(int cut)
{
  static const u8 bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
  u16 c0 = UI_BG, c1 = ui_theme.bg2;
  int r0 = (c0 >> 11) << 3, g0 = ((c0 >> 5) & 63) << 2, b0 = (c0 & 31) << 3;
  int r1 = (c1 >> 11) << 3, g1 = ((c1 >> 5) & 63) << 2, b1 = (c1 & 31) << 3;
  int x, y;

  for (y = 0; y < UI_H; y++)
  {
    u16 *row = ui_fb + y * UI_W;
    int dy = y + 40, end = cut ? UI_CUT(y) : UI_W;
    for (x = 0; x < end && x < UI_W; x++)
    {
      int dx = x - UI_W / 2, t = 256 - ((dx * dx + dy * dy) >> 8), d = bayer[y & 3][x & 3] << 4, r, g, b;
      if (t < 0)
        t = 0;
      r = (((r0 << 8) + (r1 - r0) * t) >> 3) + d;
      g = (((g0 << 8) + (g1 - g0) * t) >> 2) + d;
      b = (((b0 << 8) + (b1 - b0) * t) >> 3) + d;
      r >>= 8;
      g >>= 8;
      b >>= 8;
      row[x] = (u16)(((r > 31 ? 31 : r) << 11) | ((g > 63 ? 63 : g) << 5) | (b > 31 ? 31 : b));
    }
  }
  /* in it, tone on tone: a court's three-point line and its key's circle, and light streaks leaning across */
  court_line(UI_W / 2, UI_H + 40, 190, 2, 0, UI_H, cut ? UI_CUT(UI_H - 1) : UI_W);
  court_line(UI_W / 2, UI_H + 40, 96, 2, 0, UI_H, cut ? UI_CUT(UI_H - 1) : UI_W);
  if (!cut)
  {
    ui_lean(196, 54, 0, UI_H, 0xFFFF, 1);
    ui_lean(262, 3, 0, UI_H, 0xFFFF, 2);
    ui_lean(272, 1, 0, UI_H, 0xFFFF, 2);
  }
  else
  {
    ui_lean(18, 30, 0, UI_H, 0xFFFF, 1);
    ui_lean(56, 2, 0, UI_H, 0xFFFF, 2);
  }
}

void ui_ground(void)
{
  if (UI_BG == ui_theme.bg2)
    ui_fill(UI_BG);   /* a plain ground: nothing in it */
  else
    ground_rows(0);
}

void ui_ground_cut(int all)
{
  int y;
  ground_rows(!all);
  for (y = 0; y < UI_H; y++)
    ui_rect(UI_CUT(y), y, UI_CUT_STRIPE, 1, UI_MARK);
}

/* The coverage at (i, j) of a glyph drawn scale times the size (i, j in
 * its pixels; step: 65536 / scale), read between the glyph's pixels. */
static int scaled_cover(const u8 *bits, int w, int h, int i, int j, int step)
{
  int sx = (((2 * i + 1) * step) >> 9) - 128, sy = (((2 * j + 1) * step) >> 9) - 128;
  int i0 = sx >> 8, fx = sx & 255, j0 = sy >> 8, fy = sy & 255, a;
  if (i < 0 || j < 0)
    return 0;
  a = (cover_at(bits, w, h, i0, j0) * (256 - fx) + cover_at(bits, w, h, i0 + 1, j0) * fx) * (256 - fy) +
      (cover_at(bits, w, h, i0, j0 + 1) * (256 - fx) + cover_at(bits, w, h, i0 + 1, j0 + 1) * fx) * fy;
  return (a + 32768) >> 16;
}

/* The width of s in font f scale times the size, in 1/64 pixel. */
static int scaled_width(const char *caps, const struct ui_font *f, int scale)
{
  int total = 0, prev = 0;
  for (; *caps; caps++)
  {
    int k = code_of((unsigned char)*caps);
    if (prev >= 32 && k >= 32)
      total += kern(f, prev, k);
    total += f->advance[k];
    prev = k;
  }
  return total * scale;
}

void ui_giant(const char *s, int right, int y, int scale, int outline, u16 color, int y0, int y1, int x1)
{
  const struct ui_font *f = &ui_font_d125;
  const char *caps = ui_caps(s), *c;
  int step = 65536 / scale, pen = right * 64 - scaled_width(caps, f, scale), prev = 0;

  if (y0 < 0)
    y0 = 0;
  if (y1 > UI_H)
    y1 = UI_H;
  if (x1 > UI_W)
    x1 = UI_W;
  for (c = caps; *c; c++)
  {
    int k = code_of((unsigned char)*c);
    const struct ui_glyph *g = &f->glyphs[k * f->phases];
    const u8 *bits = f->bits + g->bits;
    if (prev >= 32 && k >= 32)
      pen += kern(f, prev, k) * scale;
    if (g->w)
    {
      int gx = (pen >> 6) + g->x * scale, gy = y + g->y * scale, i, j;
      for (j = 0; j < g->h * scale; j++)
      {
        int py = gy + j;
        if (py < y0 || py >= y1)
          continue;
        for (i = 0; i < g->w * scale; i++)
        {
          int px = gx + i;
          if (px < 0 || px >= x1 || scaled_cover(bits, g->w, g->h, i, j, step) < 8)
            continue;
          /* an outline: only where the letter ends within 2 pixels */
          if (outline && scaled_cover(bits, g->w, g->h, i - 2, j, step) >= 8 &&
              scaled_cover(bits, g->w, g->h, i + 2, j, step) >= 8 && scaled_cover(bits, g->w, g->h, i, j - 2, step) >= 8 &&
              scaled_cover(bits, g->w, g->h, i, j + 2, step) >= 8)
            continue;
          ui_fb[py * UI_W + px] = color;
        }
      }
    }
    pen += f->advance[k] * scale;
    prev = k;
  }
}

/* The screen's name in the big words' glyphs three times the size (their
 * coverage read between its pixels: smooth edges), white at a sixth, over
 * the ground: its right end at right, baseline y. */
void ui_backdrop_word(const char *s, int right, int y, int y0, int y1)
{
  const struct ui_font *f = &ui_font_d100;
  const char *caps = ui_caps(s), *c;
  int pen = right * 64 - scaled_width(caps, f, WORD_SCALE), prev = 0;

  if (y0 < 0)
    y0 = 0;
  if (y1 > UI_H)
    y1 = UI_H;
  for (c = caps; *c; c++)
  {
    int k = code_of((unsigned char)*c);
    const struct ui_glyph *g = &f->glyphs[k * f->phases];
    const u8 *bits = f->bits + g->bits;
    if (prev >= 32 && k >= 32)
      pen += kern(f, prev, k) * WORD_SCALE;
    if (g->w)
    {
      int gx = (pen >> 6) + g->x * WORD_SCALE, gy = y + g->y * WORD_SCALE, i, j;
      for (j = 0; j < g->h * WORD_SCALE; j++)
      {
        int py = gy + j;
        u16 *row;
        if (py < y0 || py >= y1)
          continue;
        row = ui_fb + py * UI_W;
        for (i = 0; i < g->w * WORD_SCALE; i++)
        {
          int px = gx + i, a;
          if (px < 0 || px >= UI_W)
            continue;
          a = (alpha16[scaled_cover(bits, g->w, g->h, i, j, 65536 / WORD_SCALE)] * 3 + 8) >> 4;
          if (a)
            row[px] = blend(row[px], 0xFFFF, (u32)a);
        }
      }
    }
    pen += f->advance[k] * WORD_SCALE;
    prev = k;
  }
}

char *ui_fit(const char *s, const struct ui_font *f, int max_w, char *out, size_t size)
{
  /* (r16) each prefix's width measured once, as layout() adds it up (ui_text_width's of out cut there and
   * ended with "..." is end[len] and the ellipsis' advance: it kerns with nothing): the same cut as measuring
   * each shorter one again, in one pass */
  static int end[256];   /* (texts longer fall back to measuring each cut) */
  size_t len, n, i;
  int pen = 0, prev = 0, ell;
  snprintf(out, size, "%s", s);
  if (ui_text_width(out, f) <= max_w)
    return out;
  len = n = strlen(out);
  if (n < sizeof(end) / sizeof(end[0]))
  {
    end[0] = 0;
    for (i = 0; i < n; i++)
    {
      int c = code_of((unsigned char)out[i]);
      if (prev >= 32 && c >= 32)
        pen += kern(f, prev, c);
      pen += f->advance[c] + track64;
      prev = c;
      end[i + 1] = pen;
    }
    ell = f->advance[(unsigned char)UI_ELLIPSIS[0]] + track64;
  }
  else
    ell = -1;
  while (len > 0)
  {
    len--;
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == ':' || out[len - 1] == ','))
      len--;
    if (len + 2 > size)
      continue;
    if (ell >= 0 && ((end[len] + ell + 32) >> 6) > max_w)
      continue;
    out[len] = UI_ELLIPSIS[0];
    out[len + 1] = 0;
    if (ell >= 0 || ui_text_width(out, f) <= max_w)
      return out;
  }
  out[0] = 0;
  return out;
}

/* A line of a wrapped text: from x, or centered on it; or nothing, when
 * center is -1 (ui_wrap_lines only counts the lines). */
static void wrap_line(int x, int y, const char *s, const struct ui_font *f, u16 color, int center)
{
  if (center < 0)
    return;
  if (center)
    ui_text_center(x, y, s, f, color);
  else
    ui_text(x, y, s, f, color);
}

static int wrap(int x, int y, int line_h, int max_w, int max_lines, const char *s, const struct ui_font *f,
                u16 color, int center)
{
  char line[330], word[80], trial[330];
  int lines = 0;

  line[0] = 0;
  while (lines < max_lines)
  {
    size_t n = 0;
    while (*s == ' ' || *s == '\n')
      s++;
    while (*s && *s != ' ' && *s != '\n' && n + 1 < sizeof(word))
      word[n++] = *s++;
    word[n] = 0;
    if (!n)
      break;
    snprintf(trial, sizeof(trial), "%s%s%s", line, line[0] ? " " : "", word);
    if (!line[0] || ui_text_width(trial, f) <= max_w)
    {
      snprintf(line, sizeof(line), "%s", trial);
      continue;
    }
    if (lines == max_lines - 1)
    {
      /* the last line that fits: what's left, cut short */
      char fitted[330];
      snprintf(trial, sizeof(trial), "%s %s%s", line, word, s);
#if defined(NSPIRE_HOST)
      /* For the PC tests: texts must fit their place. */
      if (center >= 0)
        fprintf(stderr, "gpsp-host: text cut short: %s\n", trial);
#endif
      wrap_line(x, y + lines * line_h, ui_fit(trial, f, max_w, fitted, sizeof(fitted)), f, color, center);
      return lines + 1;
    }
    wrap_line(x, y + lines * line_h, line, f, color, center);
    lines++;
    snprintf(line, sizeof(line), "%s", word);
  }
  if (line[0] && lines < max_lines)
    wrap_line(x, y + lines++ * line_h, line, f, color, center);
  return lines;
}

int ui_wrap(int x, int y, int line_h, int max_w, int max_lines, const char *s, const struct ui_font *f,
            u16 color)
{
  return wrap(x, y, line_h, max_w, max_lines, s, f, color, 0);
}

int ui_wrap_center(int center, int y, int line_h, int max_w, int max_lines, const char *s,
                   const struct ui_font *f, u16 color)
{
  return wrap(center, y, line_h, max_w, max_lines, s, f, color, 1);
}

int ui_wrap_lines(int max_w, int max_lines, const char *s, const struct ui_font *f)
{
  return wrap(0, 0, 0, max_w, max_lines, s, f, 0, -1);
}

/* ---- Icons ---- */

void ui_icon(int x, int y, int id, int size, u16 color)
{
  if (id < 0 || id >= UI_ICON_COUNT)
    return;
  if (size == 12)
    draw_bits(ui_icons_12 + id * 72, 12, 12, x, y, color);
  else
    draw_bits(ui_icons_16 + id * 128, 16, 16, x, y, color);
}

/* ui18: a Settings page's glyph (UI_GLYPH_*), UI_GLYPH_SIZE square, its top-left at x, y */
void ui_page_glyph(int x, int y, int id, u16 color)
{
  if (id < 0 || id >= UI_GLYPH_COUNT)
    return;
  draw_bits(ui_glyphs_24 + id * (UI_GLYPH_SIZE * UI_GLYPH_SIZE / 2), UI_GLYPH_SIZE, UI_GLYPH_SIZE, x, y, color);
}

/* ---- Components ---- */

void ui_led(int x, int y, u16 color)
{
  ui_rect(x, y, 4, 4, color);
}

int ui_wordmark(int x, int y)
{
  return ui_big(x, y, "gpSP", UI_TEXT);
}

/* The battery, its right edge at right: the charge in figures ("82%", or
 * "64%+" while charging), red at 15% and below. Returns its left edge. */
static const char *battery_text(void)
{
  static char text[12];
  int level = ui_battery > 100 ? 100 : ui_battery;
  snprintf(text, sizeof(text), ui_charging ? "%d%%+" : "%d%%", level);
  return text;
}

/* r19: the battery is the band's right end on every screen (the top row is the tabs', the whole width): drawn
 * as the screen is shown (ui_present), over whatever reached that far. */
static void band_battery(void)
{
  int w;
  if (ui_battery < 0)
    return;
  w = ui_text_width(battery_text(), &ui_font_small);
  ui_rect(UI_W - UI_MARGIN - w - 6, UI_FOOT_Y, w + 6 + UI_MARGIN, UI_H - UI_FOOT_Y, UI_BG);
  ui_text(UI_W - UI_MARGIN - w, UI_FOOT_BASE, battery_text(), &ui_font_small, UI_TEXT);
}

/* Where the band's words end: the right margin, or before the battery. */
static int band_right(void)
{
  if (ui_battery < 0)
    return UI_W - UI_MARGIN;
  return UI_W - UI_MARGIN - ui_text_width(battery_text(), &ui_font_small) - 12;
}

int ui_header_battery(int right)
{
  return right;   /* (r19: the battery is in the band) */
}

/* r19: the top row of a page: where it is, as a path. The steps before the last in grey with the slashes, the
 * last one black; a step too long is cut ("..."): one before the last to 100 px, the last to what is left. A
 * note at the right, in the regular weight. */
void ui_path(const char *path, const char *note)
{
  char step[96], fit[96];
  int right = UI_W - UI_MARGIN, x = UI_MARGIN;
  const char *p = path ? path : "";

  if (note && note[0])
  {
    const char *n = ui_fit(note, &ui_font_small, 150, fit, sizeof(fit));
    right -= ui_text_width(n, &ui_font_small);
    ui_text(right, UI_HEAD_BASE, n, &ui_font_small, UI_TEXT);
    right -= 12;
  }
  while (*p)
  {
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    snprintf(step, sizeof(step), "%.*s", (int)n, p);
    if (slash)
    {
      /* (r19: a step before the last in what the note leaves, 100 px at most, room kept for the slash
       * and a few letters of the last step: the path ran over the note) */
      int room = right - x - ui_text_width(" / ", &ui_font_small) - 40;
      if (room > 100)
        room = 100;
      if (room < 14)
        break;
      x = ui_text(x, UI_HEAD_BASE, ui_fit(step, &ui_font_small_strong, room, fit, sizeof(fit)), &ui_font_small_strong,
                  UI_MUTED);
      x = ui_text(x, UI_HEAD_BASE, " / ", &ui_font_small, UI_MUTED);
      p = slash + 1;
    }
    else
    {
      ui_text(x, UI_HEAD_BASE, ui_fit(step, &ui_font_small_strong, right - x, fit, sizeof(fit)), &ui_font_small_strong,
              UI_TEXT);
      break;
    }
  }
}

void ui_header_from(const char *from, const char *title, const char *note)
{
  char path[200];
  snprintf(path, sizeof(path), "%s%s%s", from && from[0] ? from : "", from && from[0] ? "/" : "", title ? title : "");
  ui_path(path, note);
}

void ui_header(const char *title, const char *note)
{
  ui_path(title, note);
}

/* r19: the top row of every top-level screen, as the site's: a word in each corner (the first name at the left
 * margin, the last at the right one) and the selector between them, centred (the names between: "SAVES GAMES",
 * or "GAMES" alone). */
void ui_nav(const char *const *names, int count, int on, int focus)
{
  const struct ui_font *f = &ui_font_small_strong;
  int widths[12], xs[12], total = 0, i, x;
  const int gap = 14;
  if (count > 12)
    count = 12;
  for (i = 0; i < count; i++)
    widths[i] = ui_text_width(names[i], f);
  for (i = 1; i < count - 1; i++)
    total += widths[i] + (i > 1 ? gap : 0);
  if (count == 1)
    xs[0] = (UI_W - widths[0]) / 2;
  else if (count > 1)
  {
    xs[0] = UI_MARGIN;
    xs[count - 1] = UI_W - UI_MARGIN - widths[count - 1];
    for (i = 1, x = (UI_W - total) / 2; i < count - 1; i++)
    {
      xs[i] = x;
      x += widths[i] + gap;
    }
  }
  for (i = 0; i < count; i++)
  {
    u16 ink = focus ? UI_TEXT : i == on ? UI_TEXT : UI_MUTED;
    if (focus && i == on)
    {
      ui_rect(xs[i] - 4, UI_HEAD_Y, widths[i] + 7, UI_LINE_H, UI_ACCENT);
      ink = UI_FOCUS_INK;
    }
    ui_text(xs[i], UI_HEAD_BASE, names[i], f, ink);
  }
}

/* A red tile, as a seed or a score is shown: full red at its top, a third
 * darker at its bottom. */
u16 ui_focus(int x, int y, int w, int h)
{
  int j;
  (void)j;
  ui_rect(x, y, w, h, UI_FOCUS);
  return UI_FOCUS_INK;
}

u16 ui_focus_cut(int y, int h)
{
  int j;
  for (j = 0; j < h; j++)
    ui_rect(0, y + j, UI_CUT(y + j) - 3, 1, blend(0, UI_FOCUS, (u32)(16 - j * 6 / h)));
  return UI_FOCUS_INK;
}

int ui_key_hint(int x, int baseline, const char *key)
{
  return ui_text(x, baseline, key, &ui_font_small_strong, UI_TEXT);
}

/* enter and esc do the same on every screen (enter does, esc goes back):
 * they aren't said, unless the screen asks with a '!' before the key
 * ("!enter", "Mark": here enter does something else). */
static int key_said(const char *key)
{
  return strcmp(key, "enter") && strcmp(key, "esc");
}

static const char *key_name_of(const char *key)
{
  return key[0] == '!' ? key + 1 : key;
}

static int legend_width(const char *key, const char *what)
{
  /* A key that says what it does can stand alone. */
  return ui_text_width(key_name_of(key), &ui_font_small_strong) + (what[0] ? 5 + ui_text_width(what, &ui_font_small) : 0);
}

static int legend_at(int x, int base, const char *key, const char *what);
#define legend(x, key, what) legend_at(x, UI_FOOT_BASE, key, what)

/* The band's key hints: the key, a 5 px gap, its action in capitals; the pairs 12 apart, from the left
 * margin, while they fit. The same on every screen (the managers' column of actions too). */
#define LEGEND_GAP 12

static int legend_at(int x, int base, const char *key, const char *what)
{
  int end = ui_key_hint(x, base, key_name_of(key));
  return what[0] ? ui_text(end + 5, base, what, &ui_font_small, UI_TEXT) : end;
}

void ui_footer(const char *const *pairs, int count)
{
  int k, x = UI_MARGIN;
  ui_rect(0, UI_FOOT_Y, UI_W, UI_H - UI_FOOT_Y, UI_BG);
  for (k = 0; k < count; k++)
    if (key_said(pairs[2 * k]) && x + legend_width(pairs[2 * k], pairs[2 * k + 1]) <= band_right())
      x = legend(x, pairs[2 * k], pairs[2 * k + 1]) + LEGEND_GAP;
}

int ui_keys_column(int x, int y, const char *const *pairs)
{
  int r;
  for (r = 0; pairs && pairs[r] && y <= UI_FOOT_Y - 6; r += 2, y += UI_LINE_H)
    legend_at(x, y, pairs[r], pairs[r + 1]);
  return y;
}

void ui_footer_status(const char *text, u16 color)
{
  char fit[96];
  /* over the keys: what just happened matters more. In capitals, as every word of the band; cut ("...") when
   * even that is too long (the PC build says so: a text must fit its place). */
  ui_rect(0, UI_FOOT_Y, UI_W, UI_H - UI_FOOT_Y, UI_BG);
  if (ui_text_width(text, &ui_font_small) > band_right() - UI_MARGIN)
  {
#if defined(NSPIRE_HOST)
    fprintf(stderr, "gpsp-host: text cut short: %s\n", text);
#endif
    text = ui_fit(text, &ui_font_small, band_right() - UI_MARGIN, fit, sizeof(fit));
  }
  ui_text(UI_MARGIN, UI_FOOT_BASE, text, &ui_font_small, color);
}

void ui_footer_help(const char *text)
{
  /* one sentence, as it is written, black: the band's one line (r16: "only when the label and value still
   * can't say what it does"; never two lines, never grey) */
  ui_rect(0, UI_FOOT_Y, UI_W, UI_H - UI_FOOT_Y, UI_BG);
  ui_wrap(UI_MARGIN, UI_FOOT_BASE, UI_LINE_H, band_right() - UI_MARGIN, 1, text, &ui_font_small, UI_TEXT);
}

/* r19: the band of a screen of items: at its left what the picked one is (its facts, in the regular weight), at
 * its right, before the battery, the keys that aren't obvious: as many of the pairs as fit beside the facts, the
 * first ones first. */
void ui_band(const char *left, const char *const *pairs, int count)
{
  int x = UI_MARGIN, right = band_right(), k, n = 0, w = 0, room;
  char fit[96];
  ui_rect(0, UI_FOOT_Y, UI_W, UI_H - UI_FOOT_Y, UI_BG);
  if (left && left[0])
  {
    const char *t = ui_fit(left, &ui_font_small, right - UI_MARGIN, fit, sizeof(fit));
    x = ui_text(UI_MARGIN, UI_FOOT_BASE, t, &ui_font_small, UI_TEXT) + 16;
  }
  room = right - x;
  for (k = 0; k < count; k++)
  {
    int lw = legend_width(pairs[2 * k], pairs[2 * k + 1]) + (n ? LEGEND_GAP : 0);
    if (w + lw > room)
      break;
    w += lw;
    n++;
  }
  for (k = 0, x = right - w; k < n; k++)
    x = legend(x, pairs[2 * k], pairs[2 * k + 1]) + LEGEND_GAP;
}

void ui_footer_note(const char *text)
{
  int right = band_right(), w = ui_text_width(text, &ui_font_small);
  ui_rect(right - w - 6, UI_FOOT_Y, w + 6, UI_H - UI_FOOT_Y, UI_BG);
  ui_text(right - w, UI_FOOT_BASE, text, &ui_font_small, UI_TEXT);
}

static const u8 chevron[9][5] = {
  { 0, 0, 0, 1, 0 }, { 0, 0, 3, 16, 4 }, { 0, 1, 14, 11, 0 }, { 0, 10, 15, 1, 0 }, { 3, 16, 5, 0, 0 },
  { 0, 13, 13, 0, 0 }, { 0, 2, 15, 8, 0 }, { 0, 0, 6, 16, 4 }, { 0, 0, 0, 5, 0 },
};

void ui_chevrons(int left, int right, int y, u16 color)
{
  int i, j;
  for (j = 0; j < 9; j++)
    for (i = 0; i < 5; i++)
    {
      put(left + i, y + j, color, chevron[j][i]);
      put(right - 1 - i, y + j, color, chevron[j][i]);
    }
}

void ui_progress(int x, int y, int w, u32 done, u32 total)
{
  int d = total ? (int)((u64)w * (done > total ? total : done) / total) : 0;
  ui_rect(x, y, d, 6, UI_MARK);
}

void ui_scrollbar(int x, int y0, int y1, int first, int shown, int count)
{
  int track = y1 - y0, bar, top;
  if (count <= shown)
    return;
  bar = track * shown / count;
  if (bar < 12)
    bar = 12;
  top = (track - bar) * first / (count - shown);
  ui_rect(x - 1, y0 + top, 3, bar, UI_TEXT);   /* where these are: a black bar, no track (r16: no rules) */
}

/* ---- Pictures ---- */

void ui_blit(int x, int y, const u16 *src, int w, int h, int pitch)
{
  int j, x0 = x, y0 = y, cw = w, ch = h;
  if (!clip(&x0, &y0, &cw, &ch))
    return;
  src += (y0 - y) * pitch + (x0 - x);
  for (j = 0; j < ch; j++)
    memcpy(ui_fb + (y0 + j) * UI_W + x0, src + j * pitch, (size_t)cw * 2);
}

/* A picture stands by itself: no edge round it. */
void ui_picture(int x, int y, const u16 *src, int w, int h, int pitch)
{
  ui_blit(x, y, src, w, h, pitch);
}

/* A 5:6:5 pixel spread over 32 bits (green on top), so the three fields
 * have room to be multiplied by up to 32 and summed at once. */
#define SPREAD_MASK 0x07E0F81Fu
#define SPREAD(p) (((u32)(p) | (u32)(p) << 16) & SPREAD_MASK)

/* The source pixels under output pixel i of n (out of m), and how much of
 * each (in 1/256 of the output pixel): at most 8. */
static int spans(int i, int n, int m, int *first, int *weight)
{
  int a = i * m * 256 / n, b = (i + 1) * m * 256 / n;   /* in 1/256 source pixel */
  int k = 0, s = a >> 8, total = b - a;
  *first = s;
  while (s * 256 < b && k < 8)
  {
    int lo = s * 256 > a ? s * 256 : a, hi = (s + 1) * 256 < b ? (s + 1) * 256 : b;
    weight[k++] = (hi - lo) * 256 / total;
    s++;
  }
  return k;
}

/* The same in 1/32, summing to 32, for spread pixels (SPREAD). */
static int spans32(int i, int n, int m, int *first, int *weight)
{
  int k = spans(i, n, m, first, weight), j, sum = 0, big = 0;
  for (j = 0; j < k; j++)
  {
    weight[j] = (weight[j] + 4) >> 3;
    sum += weight[j];
    if (weight[j] > weight[big])
      big = j;
  }
  weight[big] += 32 - sum;
  return k;
}

/* Across, then down: each source line's pixels across once (kept for the
 * output lines that share it), on spread pixels, a field's sum at most 31
 * times 32. The CPU has no divide: the spans are worked out once. Each
 * source line is spread once (not once for each output pixel over it), the
 * weights are bytes (they fit the 8 KB data cache with the lines), and the
 * usual counts (one to three pixels across or lines down) have their own
 * code: the same sums, 1.3-2.4x faster (the menu's opening, the list's
 * pictures, a save's picture). Sources up to SCALE_SRC_MAX pixels wide (the
 * widest is the screen's 320). */
#define SCALE_SRC_MAX 512
/* (one copy: the compiler would make one for each set of sizes it sees) */
__attribute__((noclone, noinline)) void ui_scale(u16 *dst, int dw, int dh, const u16 *src, int sw, int sh, int pitch)
{
  static u16 xfirst[UI_W];
  static u8 xcount[UI_W], xweight[UI_W][8];
  static u32 across[8][UI_W], spread[SCALE_SRC_MAX];
  int across_of[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
  int x, y, i, j, w[8], last = 0;
  /* the lines across kept: 4 (5 KB: they stay in the data cache) while an
   * output line takes at most 4 source lines, which it does up to 2:1 */
  int ring = sh <= 2 * dh ? 3 : 7;

  if (dw <= 0 || dw > UI_W || sw > SCALE_SRC_MAX)
    return;
  for (x = 0; x < dw; x++)
  {
    int first, n = spans32(x, dw, sw, &first, w);
    xfirst[x] = (u16)first;
    xcount[x] = (u8)n;
    for (i = 0; i < n; i++)
      xweight[x][i] = (u8)w[i];
    if (first + n > last)
      last = first + n;
  }
  for (y = 0; y < dh; y++)
  {
    int fy, wy[8], ny = spans32(y, dh, sh, &fy, wy);
    u16 *out = dst + y * dw;
    for (j = 0; j < ny; j++)
    {
      int line = fy + j;
      u32 *a = across[line & ring];
      const u16 *in = src + line * pitch;
      if (across_of[line & ring] == line)
        continue;
      across_of[line & ring] = line;
      for (i = xfirst[0]; i < last; i++)
        spread[i] = SPREAD(in[i]);
      for (x = 0; x < dw; x++)
      {
        const u32 *p = spread + xfirst[x];
        const u8 *wx = xweight[x];
        u32 sum;
        switch (xcount[x])
        {
          case 1:
            a[x] = p[0];   /* its weight is 32: the sum shifted back is the pixel */
            continue;
          case 2:
            sum = p[0] * wx[0] + p[1] * wx[1];
            break;
          case 3:
            sum = p[0] * wx[0] + p[1] * wx[1] + p[2] * wx[2];
            break;
          default:
            sum = 0;
            for (i = 0; i < xcount[x]; i++)
              sum += p[i] * wx[i];
            break;
        }
        a[x] = (sum >> 5) & SPREAD_MASK;
      }
    }
    if (ny == 1)
    {
      const u32 *a0 = across[fy & ring];
      for (x = 0; x < dw; x++)
        out[x] = (u16)(a0[x] | a0[x] >> 16);
    }
    else if (ny == 2)
    {
      const u32 *a0 = across[fy & ring], *a1 = across[(fy + 1) & ring];
      u32 w0 = (u32)wy[0], w1 = (u32)wy[1];
      for (x = 0; x < dw; x++)
      {
        u32 v = ((a0[x] * w0 + a1[x] * w1) >> 5) & SPREAD_MASK;
        out[x] = (u16)(v | v >> 16);
      }
    }
    else if (ny == 3)
    {
      const u32 *a0 = across[fy & ring], *a1 = across[(fy + 1) & ring], *a2 = across[(fy + 2) & ring];
      u32 w0 = (u32)wy[0], w1 = (u32)wy[1], w2 = (u32)wy[2];
      for (x = 0; x < dw; x++)
      {
        u32 v = ((a0[x] * w0 + a1[x] * w1 + a2[x] * w2) >> 5) & SPREAD_MASK;
        out[x] = (u16)(v | v >> 16);
      }
    }
    else
      for (x = 0; x < dw; x++)
      {
        u32 sum = 0, v;
        for (j = 0; j < ny; j++)
          sum += across[(fy + j) & ring][x] * (u32)wy[j];
        v = (sum >> 5) & SPREAD_MASK;
        out[x] = (u16)(v | v >> 16);
      }
  }
}


void ui_picture_fit(int x, int y, int dw, int dh, const u16 *src, int sw, int sh, int pitch, int grey)
{
  static u16 buf[UI_W * UI_H];
  int i, n = dw * dh;
  if (dw <= 0 || dh <= 0 || n > UI_W * UI_H)
    return;
  if (dw == sw && dh == sh && !grey)
  {
    ui_blit(x, y, src, dw, dh, pitch);   /* its own size: as it is */
    return;
  }
  if (dw == sw && dh == sh)
  {
    for (i = 0; i < dh; i++)
      memcpy(buf + i * dw, src + i * pitch, (size_t)dw * 2);
  }
  else
    ui_scale(buf, dw, dh, src, sw, sh, pitch);
  if (grey)
    for (i = 0; i < n; i++)
    {
      /* its light (green counted most, as the eye does): a black and white print, not washed out */
      u16 c = buf[i];
      int l = (((c >> 11) << 3) * 77 + (((c >> 5) & 63) << 2) * 150 + ((c & 31) << 3) * 29) >> 8;
      buf[i] = UI_RGB(l, l, l);
    }
  ui_blit(x, y, buf, dw, dh, dw);
}

/* ---- The paused game ----
 * r16: the paused menu shows the game as it was, 240 x 160, pixel for pixel (no scaling: precise, and its
 * opening is a copy, not the 320 x 213 scale it was: about 20 ms less on the CX II). */

static u16 paused_frame[240 * 160];
static int have_backdrop;

void ui_backdrop_clear(void)
{
  have_backdrop = 0;
}

void ui_backdrop_make(const u16 *frame, int pitch)
{
  int y;
  for (y = 0; y < 160; y++)
    memcpy(paused_frame + y * 240, frame + y * pitch, 240 * 2);
  have_backdrop = 1;
}

const u16 *ui_backdrop_picture(void)
{
  return have_backdrop ? paused_frame : NULL;
}

void ui_backdrop_draw(void)
{
  ui_fill(UI_BG);
  if (have_backdrop)
    ui_blit((UI_W - 240) / 2, (UI_H - 160) / 2, paused_frame, 240, 160, 240);
}

/* ---- Game names ---- */

static int ends_with(const char *s, size_t len, const char *end)
{
  size_t n = strlen(end);
  return len >= n && !strncasecmp(s + len - n, end, n);
}

void ui_game_title(const char *file_name, char *title, size_t title_size, char *tags, size_t tags_size)
{
  static const char *const exts[] = { ".gba", ".gbz", ".zip", ".agb", ".bin" };
  static const char *const articles[] = { ", The", ", A", ", An" };
  char name[256], groups[8][64];
  const char *slash = strrchr(file_name, '/');
  size_t len, i;
  int ngroups = 0;
  char *dash;

  snprintf(name, sizeof(name), "%s", slash ? slash + 1 : file_name);
  len = strlen(name);
  if (ends_with(name, len, ".tns"))
    name[len -= 4] = 0;
  for (i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
    if (ends_with(name, len, exts[i]))
    {
      name[len -= 4] = 0;
      break;
    }
  for (i = 0; i < len; i++)
    if (name[i] == '_')
      name[i] = ' ';
  /* a release number in front: "1986 - Pokemon - Emerald Version (U)" */
  if (len > 7 && isdigit((unsigned char)name[0]) && isdigit((unsigned char)name[1]) &&
      isdigit((unsigned char)name[2]) && isdigit((unsigned char)name[3]) && !strncmp(name + 4, " - ", 3))
  {
    memmove(name, name + 7, len - 6);
    len -= 7;
  }

  /* "(USA, Europe) (Rev 1)" and "[!]" at the end become tags, and so does
   * whatever follows the first of them ("(USA) - Rev 1") */
  {
    char *open = NULL;
    for (i = 1; i < len; i++)
      if ((name[i] == '(' || name[i] == '[') && name[i - 1] == ' ')
      {
        open = name + i;
        break;
      }
    while (open)
    {
      char *start = open + 1, *close = strpbrk(start, ")]");
      size_t n = close ? (size_t)(close - start) : strlen(start);
      if (ngroups < 8)
        snprintf(groups[ngroups++], sizeof(groups[0]), "%.*s", (int)n, start);
      open = close ? strpbrk(close + 1, "([") : NULL;
    }
    for (i = 1; i < len; i++)
      if ((name[i] == '(' || name[i] == '[') && name[i - 1] == ' ')
      {
        name[i] = 0;
        len = i;
        break;
      }
    while (len && (name[len - 1] == ' ' || name[len - 1] == '-'))
      name[--len] = 0;
  }

  /* "Legend of Zelda, The - The Minish Cap" -> "The Legend of Zelda - ..." */
  for (i = 0; i < sizeof(articles) / sizeof(articles[0]); i++)
  {
    char *at = strstr(name, articles[i]);
    size_t an = strlen(articles[i]);
    if (at && (at[an] == 0 || !strncmp(at + an, " - ", 3)))
    {
      char moved[256];
      snprintf(moved, sizeof(moved), "%s %.*s%s", articles[i] + 2, (int)(at - name), name, at + an);
      snprintf(name, sizeof(name), "%s", moved);
      break;
    }
  }
  /* the first " - " separates a subtitle */
  dash = strstr(name, " - ");
  if (dash && dash != name)
  {
    char joined[256];
    snprintf(joined, sizeof(joined), "%.*s: %s", (int)(dash - name), name, dash + 3);
    snprintf(name, sizeof(name), "%s", joined);
  }
  /* "Pokemon: Emerald Version" -> "Pokemon: Emerald": shorter, as good */
  {
    char *ver = strstr(name, " Version");
    if (ver && ver[8] == 0 && ver != name)
      *ver = 0;
  }
  snprintf(title, title_size, "%s", name[0] ? name : file_name);

  tags[0] = 0;
  for (i = 0; i < (size_t)ngroups; i++)
  {
    size_t used = strlen(tags);
    snprintf(tags + used, tags_size - used, "%s%s", used ? ", " : "", groups[i]);
  }
}

/* ---- When ---- */

/* Month and day of a day number (days since 1970-01-01), civil calendar. */
static void civil(long days, int *month, int *day)
{
  long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
  long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy;
  long mp, d;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  *day = (int)d;
  *month = (int)(mp < 10 ? mp + 3 : mp - 9);
}

void ui_when(long then, long now, int style, char *out, size_t size)
{
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  static const char weekdays[] = "SunMonTueWedThuFriSat";
  long diff = now - then, d_now = now / 86400, d_then = then / 86400, gone = d_now - d_then;
  int month, day;

  out[0] = 0;
  if (then < 1577836800L || diff < -120)   /* not known: before 2020, or from the future */
    return;
  if (diff < 0)
    diff = 0;
  if (diff < 60)
    snprintf(out, size, "Now");   /* one word for it, in the list, the card and the strip */
  else if (diff < 3600)
    snprintf(out, size, style ? "%ld min" : "%ld min ago", diff / 60);
  else if (diff < 86400)
    snprintf(out, size, style ? "%ld h" : (diff < 7200 ? "%ld hour ago" : "%ld hours ago"), diff / 3600);
  else if (gone == 1 && !style)
    snprintf(out, size, "Yesterday");
  else if (gone < 7 && style)
    snprintf(out, size, "%.3s", weekdays + ((d_then + 4) % 7) * 3);
  else if (gone < 7)
    snprintf(out, size, "%ld days ago", gone);
  else if (gone < 14 && !style)
    snprintf(out, size, "Last week");
  else
  {
    civil(d_then, &month, &day);
    snprintf(out, size, "%.3s %d", months + (month - 1) * 3, day);
  }
}

/* ---- Widgets of the grid (r16) ---- */

int ui_tabs(int x, int y, const char *const *names, int count, int on)
{
  int i, end = x;
  for (i = 0; i < count; i++)
  {
    if (i)
      x = end + 12;
    end = ui_label(x, y, names[i], i == on ? UI_TEXT : UI_MUTED);   /* (no rule: black is "here") */
  }
  return end;
}

/* r19: a text too long for its place, picked: it waits a second, then runs to the left a pixel at a time,
 * 30 px a second, and comes round again from the right after a gap of UI_MARQUEE_GAP (smooth, and
 * wrapping round; until r19 it jumped a whole character at a time, stopped at its end and started over). How far
 * it has run after ms, 0 to period - 1 (period: the text's width and the gap); each time round it waits its
 * second again at the start. */
#define UI_MARQUEE_GAP 24
int ui_scroll_after(unsigned ms, int period)
{
  unsigned loop, t;
  if (period <= 0)
    return 0;
  loop = 1000 + (unsigned)period * 1000 / 30;
  t = ms % loop;
  if (t <= 1000)
    return 0;
  t = (t - 1000) * 30 / 1000;
  return t >= (unsigned)period ? 0 : (int)t;
}

int ui_line_text(int x, int y, int w, const char *s, const struct ui_font *f, u16 color, int picked,
                 unsigned ms)
{
  char fit[200];
  int tw = ui_text_width(s, f), moved, c0 = clip_x0, c1 = clip_x1;
  if (tw <= w)
  {
    ui_text(x, y, s, f, color);
    return 0;
  }
  if (!picked)
  {
    ui_text(x, y, ui_fit(s, f, w, fit, sizeof(fit)), f, color);
    return 0;
  }
  /* the whole text, drawn only inside its place, and once more a gap behind it: the one that leaves at the
   * left is the one that comes in at the right */
  moved = ui_scroll_after(ms, tw + UI_MARQUEE_GAP);
  ui_clip(x > c0 ? x : c0, x + w < c1 ? x + w : c1);
  ui_text(x - moved, y, s, f, color);
  if (moved && x - moved + tw + UI_MARQUEE_GAP < x + w)
    ui_text(x - moved + tw + UI_MARQUEE_GAP, y, s, f, color);
  clip_x0 = c0;
  clip_x1 = c1;
  return 1;
}

int ui_signal(int right, int y, int bars, u16 color)
{
  int i, x = right - UI_SIGNAL_W;
  for (i = 0; i < 4; i++)
  {
    int h = 3 + 2 * i;
    if (i < bars)
      ui_rect(x + 3 * i, y - h, 2, h, color);
    else
      ui_rect(x + 3 * i, y - 1, 2, 1, color);
  }
  return x;
}

int ui_list_line(int y, const char *name, const char *value, int picked, int heading, unsigned ms)
{
  const int x1 = UI_W - UI_MARGIN;
  u16 ink = UI_TEXT, ink2 = UI_TEXT;
  int room = x1 - UI_MARGIN, w;
  if (heading)
  {
    /* a section's name: grey capitals on the line's baseline */
    ui_label(UI_MARGIN, y + UI_LINE_BASE, name, UI_MUTED);
    return 0;
  }
  if (picked)
  {
    ui_rect(4, y, UI_W - 8, UI_LINE_H, UI_FOCUS);
    ink = ink2 = UI_FOCUS_INK;
  }
  if (value && value[0])
  {
    w = ui_text_width(value, &ui_font_small);
    ui_text(x1 - w, y + UI_LINE_BASE, value, &ui_font_small, ink2);
    room -= w + 12;
  }
  return ui_line_text(UI_MARGIN, y + UI_LINE_BASE, room, name, &ui_font_small, ink, picked, ms);
}
