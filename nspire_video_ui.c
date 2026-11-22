/* Nspire menu / file-browser framebuffer (legacy gpSP video API subset). */

#include "common.h"
#include "nspire_gui_video.h"
#include "nspire.h"
#include "nspire_frontend.h"
#include "nspire_ui.h"

#include <stdio.h>
#include <string.h>

static u32 screen_offset;
static u16 *screen_pixels;
#define get_screen_pixels() (screen_pixels)
#define get_screen_pitch()  ((u32)320)

u32 screen_scale = scaled_raw;
u32 current_scale = scaled_raw;
u32 screen_filter = filter_bilinear;

u32 resolution_width = 320;
u32 resolution_height = 240;

u32 frame_speed = 15000;

/* Portrait LCD: menus draw landscape into nspire_landscape, and
 * flip_screen turns it into the display buffer. Menus draw in 5:6:5, the
 * LCD's (and the game's) own format. */
static u16 *ui_screen(void)
{
  return nspire_lcd_portrait ? nspire_landscape : (u16 *)nspire_screen;
}

void init_video(void)
{
  screen_pixels = ui_screen();
}

void video_resolution_large(void)
{
  screen_offset = 0;
  resolution_width = 320;
  resolution_height = 240;
  flip_screen();
  clear_screen(0);
}

void video_resolution_small(void)
{
  switch (screen_scale)
  {
    case unscaled:
      screen_offset = 320 * 40 + 40;
      break;
    case scaled_aspect:
      screen_offset = 320 * (80 - 14) + 80;
      break;
    case fullscreen:
      screen_offset = 320 * 80 + 80;
      break;
    case scaled_raw:
      screen_offset = 320 * (80 - 14) + 80;
      break;
    default:
      break;
  }

  clear_screen(0);
  flip_screen();
  clear_screen(0);
  flip_screen();
  clear_screen(0);
  flip_screen();
  clear_screen(0);

  resolution_width = 240;
  resolution_height = 160;
}

void set_gba_resolution(video_scale_type scale)
{
  screen_scale = scale;
}

/* Clears the frame menus draw into. On a portrait LCD that is the landscape
 * frame flip_screen turns: the screen buffers themselves are only ever
 * written whole by flip_screen and the game's frames, never the one being
 * shown. (video_resolution_small clears all of them by flipping cleared
 * frames into each in turn.) */
void clear_screen(u16 color)
{
  u32 col = ((u32)color << 16) | color;
  u32 *p = (u32 *)ui_screen();
  int c = 320 * 240 / 2;
  while (c-- > 0)
    *p++ = col;
}

/* The game's last frame, 240x160 in 5:6:5 (the menus' format too): the
 * main menu shows it. */
u16 *copy_screen(void)
{
  static u16 copy[240 * 160];
  u32 row;

  if (!gba_screen_pixels)
  {
    memset(copy, 0, sizeof(copy));
    return copy;
  }
  for (row = 0; row < 160; row++)
    memcpy(copy + row * 240, gba_screen_pixels + row * GBA_SCREEN_PITCH, 240 * 2);
  return copy;
}

/* Menus draw in 5:6:5; a landscape LCD here runs in 1:5:5:5 with red in
 * the low bits (gui.c's old color16). dst may be src. */
static void to_landscape_lcd(u16 *dst, const u16 *src)
{
  u32 n = 320 * 240;
  while (n--)
  {
    u32 c = *src++, g = (c >> 5) & 63;
    *dst++ = (u16)(((g & 1) << 15) | ((c & 31) << 10) | ((g >> 1) << 5) | (c >> 11));
  }
}

void flip_screen(void)
{
  ui_shown[0] = 0;
  if ((resolution_width == 240) && (resolution_height == 160))
  {
    if (screen_scale == fullscreen)
      upscale_aspect_fast(ui_screen(), screen_pixels);
    else if (screen_scale == scaled_raw)
      upscale_aspect_raw(ui_screen(), screen_pixels);
    else
      upscale_aspect(ui_screen(), screen_pixels);
  }
  if (nspire_lcd_portrait)
    nspire_turn_rect_565(nspire_screen, nspire_landscape, 320, 0, 0, 320, 240);
  else if (resolution_width == 320)
    to_landscape_lcd(ui_screen(), ui_screen());
  /* The menus covered the bands the game's text goes in (on either LCD). */
  nspire_present_forget_bands();
  nspire_flip();
  screen_pixels = ui_screen() + screen_offset;
}

void flip_screen_ui(void)
{
  if (nspire_lcd_portrait)
    nspire_turn_rect_565(nspire_screen, nspire_landscape, 320, 0, 0, 320, 240);
  else
    to_landscape_lcd((u16 *)nspire_screen, nspire_landscape);
  nspire_present_forget_bands();
  nspire_flip();
  screen_pixels = ui_screen() + screen_offset;
}

void update_screen(void) {}

void video_write_mem_savestate(file_tag_type savestate_file)
{
  (void)savestate_file;
}

void video_read_savestate(file_tag_type savestate_file)
{
  (void)savestate_file;
}
