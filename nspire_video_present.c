/* Game frames to the LCD, with the FPS counter and messages over them.
 *
 * The core draws 240x160 RGB565 frames (gba_screen_pixels, 240 pixels a
 * line). On the CX II's portrait LCD they're scaled and turned straight into
 * the next screen buffer, in one pass: nspire_turn.c for the sharp ones,
 * nspire_blend.c for the blended ones (with interframe blending on the way).
 * On landscape LCDs notaz's scalers (nspire/upscale_aspect.s, gpSP's 5:5:5
 * menu format) write the screen buffer directly, as before. */

#include "common.h"
#include "video.h"
#include "nspire.h"
#include "nspire_gui_video.h"
#include "nspire_frontend.h"
#include "nspire_frameskip.h"
#include "nspire_link.h"
#include "nspire_ui.h"
#include "nspire_usbpad_glue.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern u32 resolution_width;
extern u32 resolution_height;

/* ---- Text over the picture ---- */

/* R11w part 2: the line's rule is nspire_osd.c's (osd_line_*), in ms. */
static struct osd_line line;

static unsigned now_ms(void)
{
  return (unsigned)((u64)nspire_ticks() * 1000 / NSPIRE_TICK_HZ);
}

char nspire_fps_text[64];
u32 nspire_fps_full;   /* full speed's game frames a second (60; fast forward's top speed, 0 at max): the PC tests read it */
u16 nspire_fps_color;
char nspire_perf_text[80];

void nspire_osd_message(const char *format, ...)
{
  char message[OSD_TEXT_MAX];
  va_list ap;
  va_start(ap, format);
  vsnprintf(message, sizeof(message), format, ap);
  va_end(ap);
  nspire_link_log_toast(message);
  osd_line_post(&line, message, now_ms());
}

void nspire_osd_clear(void)
{
  memset(&line, 0, sizeof(line));
  nspire_fps_text[0] = 0;
  nspire_perf_text[0] = 0;
}

static const char *visible_message(void)
{
  return osd_line_visible(&line, now_ms());
}

const char *nspire_osd_visible(void)
{
  return visible_message();
}

void nspire_osd_extend(void)
{
  osd_line_extend(&line, now_ms());
}

void nspire_osd_withdraw(const char *text)
{
  osd_line_withdraw(&line, text);
}

/* Colors in the LCD's 5:6:5 (portrait) and gpSP's 5:5:5 BGR (landscape). */
#define RGB565(r, g, b) ((u16)(((r) << 11) | ((g) << 5) | (b)))
#define BGR555(r, g, b) ((u16)(((b) << 10) | ((g) << 5) | (r)))

u16 nspire_osd_color(int which)
{
  static const u16 portrait[] = { RGB565(31, 63, 31), RGB565(4, 58, 4), RGB565(31, 44, 0), RGB565(31, 8, 4) };
  static const u16 landscape[] = { BGR555(31, 31, 31), BGR555(2, 29, 2), BGR555(31, 22, 0), BGR555(31, 4, 2) };
  return nspire_lcd_portrait ? portrait[which] : landscape[which];
}

/* A 320x12 band of the landscape picture: black, with text in it. */
#define BAND_H OSD_BAND_H
static u16 band[320 * BAND_H] __attribute__((aligned(4)));

static void band_text(const char *left, u16 left_color, const char *centered, u16 centered_color)
{
  nspire_osd_band(band, left, left_color, centered, centered_color, NULL, 0);
}

/* A text box on a black plate, for pictures that fill the screen. */
static void portrait_box(u16 *dst, u32 x, u32 y, const char *text, u16 color)
{
  u32 w = nspire_osd_width(text) + 4;
  if (w > 320 - x)
    w = 320 - x;
  memset(band, 0, sizeof(band));
  nspire_osd_text(band, 2, text, color, w);
  nspire_turn_rect_565(dst, band, 320, x, y, w, BAND_H);
}

/* The icons at the top right (nspire_osd_band): the link when it's up, a
 * controller when one is in, a low battery (only when the battery is
 * known: ui_battery). One of eight constant texts, nothing built a frame. */
static const char *link_right(void)
{
  static const char *const names[8] = { "", "link", "pad", "link pad", "battery", "link battery", "pad battery",
                                        "link pad battery" };
  const char *s = nspire_link_status();
  int icons = (s && !strncmp(s, "Linked", 6)) | (nspire_usbpad_name() ? 2 : 0) |
              (ui_battery >= 0 && ui_battery <= 15 && !ui_charging ? 4 : 0);
  return names[icons];
}

/* What each screen buffer's bands hold, so that a band is only drawn again
 * when its text changes (the pictures outside fullscreen never cover them). */
#define SCREENS 3
static struct
{
  int valid;
  char top[sizeof(nspire_perf_text)];
  const char *right;   /* link_right()'s: one of its constant texts */
  u16 top_color;
  char bottom[OSD_TEXT_MAX];
} bands_shown[SCREENS];

/* Whether a buffer's top band must be drawn again for top, right and color,
 * which it then holds (drawn next). Checked every picture: a compare, no
 * key built (an snprintf a picture before: 0.05 ms on a CX at 192 MHz). */
static int top_band_stale(int screen, const char *top, const char *right, u16 color)
{
  size_t n;
  if (bands_shown[screen].valid && bands_shown[screen].right == right &&
      bands_shown[screen].top_color == color && !strcmp(bands_shown[screen].top, top))
    return 0;
  n = strlen(top);
  if (n >= sizeof(bands_shown[screen].top))
    n = sizeof(bands_shown[screen].top) - 1;
  memcpy(bands_shown[screen].top, top, n);
  bands_shown[screen].top[n] = 0;
  bands_shown[screen].right = right;
  bands_shown[screen].top_color = color;
  return 1;
}

/* F2 (2026-11-11): the buffers a screen of the menus' kind was
 * drawn into during play (the link's wait screen: drawn whole into one
 * buffer). The pictures leave some lines alone (the CX's 3:2 picture: line
 * 226; the unscaled one: its border), so what that screen had there stayed
 * and showed in every third frame until the next menu. Each such buffer is
 * cleared whole the next time a picture is drawn into it (never the one on
 * show): a bit per buffer, set with the bands forgotten. */
static u32 screens_stale;

void nspire_present_forget_bands(void)
{
  int i;
  nspire_reuse_pictures_forget();   /* (speed10: the pictures' groups too) */
  for (i = 0; i < SCREENS; i++)
    bands_shown[i].valid = 0;
  screens_stale = (1u << SCREENS) - 1;
}

/* The counters go in the top band and messages in the bottom one, which are
 * outside the picture except in fullscreen mode. A band is drawn whenever
 * its text differs from what that buffer has, so text that goes away
 * doesn't linger in the buffers. */
static void draw_text_portrait(u16 *dst)
{
  const char *msg = visible_message();
  const char *top = nspire_perf_text[0] ? nspire_perf_text : nspire_fps_text[0] ? nspire_fps_text : NULL;
  u16 top_color = nspire_perf_text[0] ? nspire_osd_color(OSD_WHITE) : nspire_fps_color;
  int screen = nspire_screen_index() % SCREENS;

  if (screen_scale == fullscreen)
  {
    bands_shown[screen].valid = 0;
    if (top)
      portrait_box(dst, 0, 0, top, top_color);
    if (msg)
    {
      u32 w = nspire_osd_width(msg) + 4;
      portrait_box(dst, w < 320 ? (320 - w) / 2 : 0, 240 - BAND_H, msg, nspire_osd_color(OSD_WHITE));
    }
    return;
  }
  const char *right = link_right();
  if (!top)
    top = "";
  if (!msg)
    msg = "";
  if (top_band_stale(screen, top, right, top_color))
  {
    nspire_osd_band(band, top, top_color, NULL, 0, right, nspire_osd_color(OSD_GOOD));
    nspire_turn_rect_565(dst, band, 320, 0, 0, 320, BAND_H);
  }
  if (!bands_shown[screen].valid || strcmp(bands_shown[screen].bottom, msg))
  {
    band_text(NULL, 0, msg, nspire_osd_color(OSD_WHITE));
    nspire_turn_rect_565(dst, band, 320, 0, 240 - BAND_H, 320, BAND_H);
    snprintf(bands_shown[screen].bottom, sizeof(bands_shown[screen].bottom), "%s", msg);
  }
  bands_shown[screen].valid = 1;
}

/* A text box on a black plate, straight into a landscape buffer. */
static void landscape_box(u16 *dst, u32 x, u32 y, const char *text, u16 color)
{
  u32 w = nspire_osd_width(text) + 4, row;
  if (w > 320 - x)
    w = 320 - x;
  memset(band, 0, sizeof(band));
  nspire_osd_text(band, 2, text, color, w);
  for (row = 0; row < BAND_H; row++)
    memcpy(dst + (y + row) * 320 + x, band + row * 320, w * 2);
}

/* The same bands on a landscape LCD (an older CX), where the pictures but
 * fullscreen leave them alone too: text drawn straight into a buffer stayed
 * there after it went away ("Resumed from slot" for good) and the counter's
 * digits piled up, so each band is drawn whole, black with its text, when it
 * changes in that buffer. */
static void draw_text_landscape(u16 *dst)
{
  const char *msg = visible_message();
  const char *top = nspire_perf_text[0] ? nspire_perf_text : nspire_fps_text[0] ? nspire_fps_text : NULL;
  u16 top_color = nspire_perf_text[0] ? nspire_osd_color(OSD_WHITE) : nspire_fps_color;
  int screen = nspire_screen_index() % SCREENS;

  if (screen_scale == fullscreen)
  {
    bands_shown[screen].valid = 0;
    if (top)
      landscape_box(dst, 0, 0, top, top_color);
    if (msg)
    {
      u32 w = nspire_osd_width(msg) + 4;
      landscape_box(dst, w < 320 ? (320 - w) / 2 : 0, 240 - BAND_H, msg, nspire_osd_color(OSD_WHITE));
    }
    return;
  }
  const char *right = link_right();
  if (!top)
    top = "";
  if (!msg)
    msg = "";
  if (top_band_stale(screen, top, right, top_color))
  {
    nspire_osd_band(band, top, top_color, NULL, 0, right, nspire_osd_color(OSD_GOOD));
    memcpy(dst, band, sizeof(band));
  }
  if (!bands_shown[screen].valid || strcmp(bands_shown[screen].bottom, msg))
  {
    band_text(NULL, 0, msg, nspire_osd_color(OSD_WHITE));
    memcpy(dst + (240 - BAND_H) * 320, band, sizeof(band));
    snprintf(bands_shown[screen].bottom, sizeof(bands_shown[screen].bottom), "%s", msg);
  }
  bands_shown[screen].valid = 1;
}

/* ---- Frames ---- */

#define NSPIRE_UPSCALE_SRC_PITCH 320

static u16 nspire_upscale_src[NSPIRE_UPSCALE_SRC_PITCH * GBA_SCREEN_HEIGHT];

/* Inverse of convert_palette() in common.h: core outputs libretro RGB packing for correct
 * blending; Nspire upscale/LCD expect xBBBBBGGGGGRRRRR like legacy gpSP. */
#define NSPIRE_LCD565_FROM_CORE(p) \
  ((u16)(((p) & 0x1Fu) << 10) | ((((p) & 0x07C0u) >> 1) & 0x03E0u) | (((p) >> 11) & 0x1Fu))

static void repack_gba_to_upscale_src(const u16 *frame, u32 pitch)
{
  u32 y, x;
  for (y = 0; y < GBA_SCREEN_HEIGHT; y++)
  {
    u16 *__restrict dst = nspire_upscale_src + y * NSPIRE_UPSCALE_SRC_PITCH;
    const u16 *__restrict src = frame + y * pitch;
    for (x = 0; x < GBA_SCREEN_WIDTH; x += 8)
    {
      dst[x + 0] = NSPIRE_LCD565_FROM_CORE(src[x + 0]);
      dst[x + 1] = NSPIRE_LCD565_FROM_CORE(src[x + 1]);
      dst[x + 2] = NSPIRE_LCD565_FROM_CORE(src[x + 2]);
      dst[x + 3] = NSPIRE_LCD565_FROM_CORE(src[x + 3]);
      dst[x + 4] = NSPIRE_LCD565_FROM_CORE(src[x + 4]);
      dst[x + 5] = NSPIRE_LCD565_FROM_CORE(src[x + 5]);
      dst[x + 6] = NSPIRE_LCD565_FROM_CORE(src[x + 6]);
      dst[x + 7] = NSPIRE_LCD565_FROM_CORE(src[x + 7]);
    }
  }
}

#define NSPIRE_MIX_PIXELS (GBA_SCREEN_PITCH * GBA_SCREEN_HEIGHT)

static u16 mix_prev[NSPIRE_MIX_PIXELS] __attribute__((aligned(32)));
static u16 mix_out[GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT] __attribute__((aligned(4)));

/* The blended pictures' interframe blending: the core draws frames into
 * frame_a (its own buffer) and frame_b in turn, and prev_frame is the one
 * drawn before (NULL: none yet). Reading the previous frame where it was
 * drawn saves copying each frame (the old way, still used by the other
 * pictures with mix_prev): a whole frame written back to memory. */
/* 2026-11-17 (final/speed8): the renderer draws an object window's inside
 * on the line after the one it draws (video.cc render_scanline_objs,
 * PIXCOPY: "Render at the next scanline!"), so a frame buffer has a 161st
 * line, as the core's own (GBA_SCREEN_BUFFER_SIZE); frame_b had 160, and
 * line 159 with an object window wrote up to 480 bytes past it, into
 * nspire.c's saved OS timer state (timer_tried, saved_timer_load/control/
 * clock/ris, clock_log), put back at the quit. The extra line is a whole
 * 4 KB (a cache way) so that the game's data after it keeps its place in
 * a way (nspire_lineio.ld). */
#define FRAME_B_EXTRA (4096 / sizeof(u16))
typedef char frame_b_has_the_scratch_line[FRAME_B_EXTRA >= GBA_SCREEN_PITCH ? 1 : -1];
static u16 frame_b[NSPIRE_MIX_PIXELS + FRAME_B_EXTRA] __attribute__((aligned(32)));
static u16 *frame_a;
static const u16 *prev_frame;

static int blends_frames_itself(void);

/* 2026-11-11 (F1): which game frame the previous picture was. With
 * frameskip, the frame blended with is the last one drawn, often 2-3 game
 * frames earlier (the core draws nothing in a skipped frame): over a scroll
 * or a warp that mixes two pictures far apart. Counted for the log
 * (nspire_video_blend_report) and the PC's pictures.txt (nspire_blend_gap:
 * this picture's, 0 when it wasn't blended). */
extern u32 frame_counter;
static u32 last_drawn_frame;
static int last_drawn_valid;
static u32 blend_pictures, blend_far, blend_gap_max;
u32 nspire_blend_gap;

void nspire_video_mix_reset(void)
{
  nspire_reuse_pictures_forget();
  memset(mix_prev, 0xFF, sizeof(mix_prev));
  prev_frame = NULL;
  last_drawn_valid = 0;
}

void nspire_video_blend_report(char *text, u32 size)
{
  if (!nspire_frame_mix_choice)
    snprintf(text, size, "blending off");
  else
    snprintf(text, size, "blending on: %lu pictures blended, %lu with a frame 2 or more before, largest gap %lu",
             (unsigned long)blend_pictures, (unsigned long)blend_far, (unsigned long)blend_gap_max);
  blend_pictures = blend_far = blend_gap_max = 0;
}

void nspire_video_frame_start(void)
{
  if (!gba_screen_pixels || !nspire_frame_mix_choice || !blends_frames_itself())
  {
    prev_frame = NULL;
    if (!nspire_frame_mix_choice || blends_frames_itself())
      last_drawn_valid = 0;   /* (mix_frames keeps its own previous frame) */
    return;
  }
  if (!frame_a)
    frame_a = gba_screen_pixels;
  prev_frame = gba_screen_pixels;
  gba_screen_pixels = gba_screen_pixels == frame_b ? frame_a : frame_b;
}

/* Interframe blending: the same 50:50 packed RGB565 mix as libretro's
 * video_post_process_mix(), into mix_out (240 pixels a line). */
static void mix_frames(void)
{
  u32 y, x;
  for (y = 0; y < GBA_SCREEN_HEIGHT; y++)
  {
    const u16 *__restrict src = gba_screen_pixels + y * GBA_SCREEN_PITCH;
    u16 *__restrict prev_row = mix_prev + y * GBA_SCREEN_PITCH;
    u16 *__restrict dst = mix_out + y * GBA_SCREEN_WIDTH;
    for (x = 0; x < GBA_SCREEN_WIDTH; x++)
    {
      u16 c = src[x], p = prev_row[x];
      prev_row[x] = c;
      dst[x] = (u16)((c + p + ((c ^ p) & 0x821u)) >> 1);
    }
  }
}

static void present_portrait(const u16 *frame, u32 pitch)
{
  u16 *dst = (u16 *)nspire_screen;
  const u16 *prev = nspire_frame_mix_choice ? prev_frame : NULL;

  switch (screen_scale)
  {
  case unscaled:
    nspire_turn_1x(dst, frame, pitch);
    break;
  case scaled_aspect:
    nspire_reuse_picture(dst, gba_screen_pixels, prev, 1);   /* speed10: the groups that changed */
    break;
  case fullscreen:
    nspire_blend_turn_full(dst, gba_screen_pixels, prev);
    break;
  case scaled_raw:
  default:
    nspire_turn_raw(dst, frame, pitch);
    break;
  }
  draw_text_portrait(dst);
}

/* The blended pictures on a portrait LCD, and the 3:2 one on a landscape
 * LCD, do interframe blending themselves. */
static int blends_frames_itself(void)
{
  return nspire_lcd_portrait ? screen_scale == scaled_aspect || screen_scale == fullscreen
                             : screen_scale == scaled_aspect;
}

static void present_landscape(const u16 *frame, u32 pitch)
{
  u16 *dst = (u16 *)nspire_screen;

  if (screen_scale == scaled_aspect)
    nspire_reuse_picture(dst, gba_screen_pixels, nspire_frame_mix_choice ? prev_frame : NULL, 0);
  else if (frame == gba_screen_pixels && screen_scale == scaled_raw)
    upscale_aspect_raw_from_gba(dst, frame);
  else
  {
    repack_gba_to_upscale_src(frame, pitch);
    if (screen_scale == fullscreen)
      upscale_aspect_fast(dst, nspire_upscale_src);
    else if (screen_scale == scaled_raw)
      upscale_aspect_raw(dst, nspire_upscale_src);
    else
      upscale_aspect(dst, nspire_upscale_src);
  }
  draw_text_landscape(dst);
}

/* Shows the frame the core just drew. Not inlined into the frame loop: the
 * PC speed estimate (tests/calc_speed.py) counts the picture's time by this
 * function. */
__attribute__((noinline)) void nspire_present_frame(void)
{
  const u16 *frame = gba_screen_pixels;
  u32 pitch = GBA_SCREEN_PITCH;

  if (resolution_width != 240 || resolution_height != 160 || !gba_screen_pixels)
    return;
  /* the frame blended with (F1): prev_frame, or mix_prev for mix_frames */
  nspire_blend_gap = 0;
  if (nspire_frame_mix_choice && last_drawn_valid && (prev_frame || !blends_frames_itself()))
  {
    nspire_blend_gap = frame_counter - last_drawn_frame;
    blend_pictures++;
    blend_far += nspire_blend_gap > 1;
    if (nspire_blend_gap > blend_gap_max)
      blend_gap_max = nspire_blend_gap;
  }
  last_drawn_frame = frame_counter;
  last_drawn_valid = 1;
  if (nspire_frame_mix_choice && !blends_frames_itself())
  {
    mix_frames();
    frame = mix_out;
    pitch = GBA_SCREEN_WIDTH;
  }
  {
    u32 bit = 1u << (nspire_screen_index() % SCREENS);
    if (screens_stale & bit)
    {
      memset(nspire_screen, 0, 320 * 240 * 2);
      screens_stale &= ~bit;
    }
  }
  if (nspire_lcd_portrait)
    present_portrait(frame, pitch);
  else
    present_landscape(frame, pitch);
  if (nspire_frameskip_before_flip())
    nspire_flip();   /* (else fast forward flips it once the LCD has the last one: nspire_frameskip.c ff_flush) */
}
