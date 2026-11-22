/* PC tests for frontend parts that don't need the emulator running:
 * turning frames for the portrait LCD (nspire_turn.c), key bindings
 * (nspire_keys.c), the settings files (nspire_config.c), screen brightness
 * (nspire_brightness.c), the file list's order (nspire_game_list.c) and
 * the link's device and hello (nspire_link.c).
 *
 *   frontend_tests DIR   files are written in DIR */

#include <stdarg.h>
#include "nspire_sram.h"
#include "common.h"
#include "nspire.h"
#include "nspire_frontend.h"
#include "nspire_gui_video.h"
#include "nspire_keys.h"
#include "nspire_bind.h"
#include "nspire_brightness.h"
#include "nspire_game_list.h"
#include "nspire_clock.h"
#include "nspire_paths.h"
#include "nspire_selfstore.h"
#include "nspire_link.h"
#include "nspire_usbport.h"
#include "nspire_arbiter.h"
#include "nspire_os.h"
#include "nspire_audio.h"
#include "nspire_padstatus.h"
#include "nspire_padlog.h"
#include "nspire_usbpad_glue.h"
#include "nspire_ui.h"
#include "gui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <sys/resource.h>

extern char unit_documents_dir[600];

static int failures, checks;
static char dir[512];

#define CHECK(cond, ...)                                      \
  do                                                          \
  {                                                           \
    checks++;                                                 \
    if (!(cond))                                              \
    {                                                         \
      failures++;                                             \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
      printf(__VA_ARGS__);                                    \
      printf("\n");                                           \
    }                                                         \
  } while (0)

/* host_stubs.c */
extern uint32_t unit_backlight;
extern long unit_backlight_writes;
extern char unit_osd_message[64];
extern int unit_cable_on, unit_cable_in_len;
extern uint8_t unit_cable_in[64];
extern uint8_t unit_cable_out[2048];
extern unsigned unit_cable_out_len;
extern uint8_t unit_cable_log[16][64];
extern unsigned unit_cable_log_len[16];
extern int unit_cable_logged;
extern uint32_t unit_cable_nonce;
extern long unit_cable_pings;
extern long unit_cable_receives;
void unit_cable_push(const void *msg, unsigned len);
void unit_cable_clear(void);
void unit_cable_push(const void *msg, unsigned len);
void unit_cable_clear(void);

#define PORTRAIT (240 * 320)
#define SENTINEL 0xBEEF

static u16 src[GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT];
static u16 got[PORTRAIT], want[PORTRAIT];

static void fill_random(u16 *p, u32 n, u32 seed)
{
  while (n--)
  {
    seed = seed * 1103515245u + 12345u;
    *p++ = (u16)(seed >> 8);
  }
}

static void fill(u16 *p, u32 n, u16 value)
{
  while (n--)
    *p++ = value;
}

/* Landscape (x, y) of the 320x240 picture in a portrait buffer. */
#define AT(buf, x, y) (buf)[(x) * 240 + 239 - (y)]

/* nspire_turn_raw as it was before the strip version (2026-10-05), which
 * matched gpSP's upscale_aspect_raw pixel for pixel. */
static const u16 old_zero_row[GBA_SCREEN_WIDTH];
static const u16 *old_raw_row(const u16 *s, u32 src_pitch, u32 y)
{
  u32 r, k, phase, src_y;
  if (y < 13 || y > 226)
    return old_zero_row;
  r = y - 13;
  k = r >> 2;
  phase = r & 3;
  src_y = 3 * k + (phase == 3 ? 2 : phase);
  if (src_y > GBA_SCREEN_HEIGHT - 1)
    src_y = GBA_SCREEN_HEIGHT - 1;
  return s + src_y * src_pitch;
}

static void old_turn_raw(u16 *dst, const u16 *s, u32 src_pitch)
{
  u32 y;
  for (y = 12; y < 228; y += 2)
  {
    const u32 *a = (const u32 *)old_raw_row(s, src_pitch, y);
    const u32 *b = (const u32 *)old_raw_row(s, src_pitch, y + 1);
    u32 *d = (u32 *)dst + (238 - y) / 2;
    u32 g;
    for (g = 0; g < GBA_SCREEN_WIDTH / 6; g++)
    {
      u32 a0 = a[0], a1 = a[1], a2 = a[2];
      u32 b0 = b[0], b1 = b[1], b2 = b[2];
      u32 w1 = (b0 >> 16) | (a0 & 0xFFFF0000u);
      u32 w4 = (b2 & 0xFFFFu) | (a2 << 16);
      d[0 * 120] = (b0 & 0xFFFFu) | (a0 << 16);
      d[1 * 120] = w1;
      d[2 * 120] = w1;
      d[3 * 120] = (b1 & 0xFFFFu) | (a1 << 16);
      d[4 * 120] = (b1 >> 16) | (a1 & 0xFFFF0000u);
      d[5 * 120] = w4;
      d[6 * 120] = w4;
      d[7 * 120] = (b2 >> 16) | (a2 & 0xFFFF0000u);
      a += 3;
      b += 3;
      d += 8 * 120;
    }
  }
}

static u32 count_differences(const u16 *a, const u16 *b, u32 n)
{
  u32 i, count = 0;
  for (i = 0; i < n; i++)
    count += a[i] != b[i];
  return count;
}

static void test_turn_raw(void)
{
  u32 x, y, pitch;

  for (pitch = GBA_SCREEN_WIDTH; pitch <= GBA_SCREEN_WIDTH + 16; pitch += 16)
  {
    static u16 frame[(GBA_SCREEN_WIDTH + 16) * GBA_SCREEN_HEIGHT];
    fill_random(frame, sizeof(frame) / 2, pitch);
    fill(got, PORTRAIT, SENTINEL);
    fill(want, PORTRAIT, SENTINEL);
    nspire_turn_raw(got, frame, pitch);
    old_turn_raw(want, frame, pitch);
    /* 2026-11-11: lines 12 and 227 (black in the old turn) are the text
     * bands' (0-12, 227-239), drawn only when their text changes: the
     * picture leaves them as they are. */
    for (x = 0; x < 320; x++)
      AT(want, x, 12) = AT(want, x, 227) = SENTINEL;
    CHECK(count_differences(got, want, PORTRAIT) == 0, "raw (pitch %u): %u pixels differ from the old turn",
          pitch, count_differences(got, want, PORTRAIT));

    /* And from first principles: 4 columns from 3, 4 lines from 3. */
    fill(want, PORTRAIT, SENTINEL);
    for (y = 13; y < 227; y++)
      for (x = 0; x < 320; x++)
      {
        s32 r = (s32)y - 13;
        static const u32 col[4] = { 0, 1, 1, 2 }, row[4] = { 0, 1, 2, 2 };
        u16 p = 0;
        if (r >= 0 && r < 214)
        {
          u32 src_y = 3 * (r >> 2) + row[r & 3];
          if (src_y > 159)
            src_y = 159;   /* 160 rows make 213 1/3 */
          p = frame[src_y * pitch + 3 * (x >> 2) + col[x & 3]];
        }
        AT(want, x, y) = p;
      }
    CHECK(count_differences(got, want, PORTRAIT) == 0, "raw (pitch %u): %u pixels wrong", pitch,
          count_differences(got, want, PORTRAIT));
  }
}

static void test_turn_1x(void)
{
  u32 x, y;
  fill_random(src, sizeof(src) / 2, 99);
  fill(got, PORTRAIT, SENTINEL);
  fill(want, PORTRAIT, SENTINEL);
  nspire_turn_1x(got, src, GBA_SCREEN_WIDTH);
  for (y = 0; y < 160; y++)
    for (x = 0; x < 240; x++)
      AT(want, 40 + x, 40 + y) = src[y * GBA_SCREEN_WIDTH + x];
  CHECK(count_differences(got, want, PORTRAIT) == 0, "1x: %u pixels wrong",
        count_differences(got, want, PORTRAIT));
}

static u16 to565(u16 p)
{
  u32 r = p & 0x1F, g = (p >> 5) & 0x1F, b = (p >> 10) & 0x1F;
  return (u16)((r << 11) | (((g << 1) | (g >> 4)) << 5) | b);
}

static void test_turn_rects(void)
{
  static u16 landscape[320 * 240];
  static const u32 rects[][4] =
  {
    { 0, 0, 320, 240 }, { 0, 13, 320, 213 }, { 2, 2, 128, 10 }, { 0, 228, 320, 12 },
    { 5, 7, 33, 17 }, { 300, 1, 20, 3 }, { 1, 0, 1, 240 }, { 17, 9, 64, 1 },
  };
  u32 i, x, y;

  fill_random(landscape, 320 * 240, 5);
  for (i = 0; i < sizeof(rects) / sizeof(rects[0]); i++)
  {
    u32 x0 = rects[i][0], y0 = rects[i][1], w = rects[i][2], h = rects[i][3];
    const u16 *s = landscape + y0 * 320 + x0;

    fill(got, PORTRAIT, SENTINEL);
    fill(want, PORTRAIT, SENTINEL);
    nspire_turn_rect_565(got, s, 320, x0, y0, w, h);
    for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
        AT(want, x0 + x, y0 + y) = s[y * 320 + x];
    CHECK(count_differences(got, want, PORTRAIT) == 0, "rect 565 %u,%u %ux%u: %u wrong", x0, y0, w, h,
          count_differences(got, want, PORTRAIT));

    fill(got, PORTRAIT, SENTINEL);
    fill(want, PORTRAIT, SENTINEL);
    nspire_turn_rect_555(got, s, 320, x0, y0, w, h);
    for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
        AT(want, x0 + x, y0 + y) = to565(s[y * 320 + x]);
    CHECK(count_differences(got, want, PORTRAIT) == 0, "rect 555 %u,%u %ux%u: %u wrong", x0, y0, w, h,
          count_differences(got, want, PORTRAIT));
  }
}

static void test_keys(void)
{
  char text[32];
  key_binding b;
  int i, slot;

  keys_set_defaults(key_bindings);
  CHECK(key_bindings[ACTION_A][0].key == KEY_DEL && key_bindings[ACTION_MENU][0].key == KEY_MENU,
        "defaults");
  CHECK(key_bindings[ACTION_RESTART][0].key == 0 && key_bindings[ACTION_A][1].key == 0, "new ones unbound");

  b = binding_unpack(KEY_S | (KEY_SHIFT << 8));
  CHECK(b.key == KEY_S && b.with == KEY_SHIFT, "unpack combo");
  CHECK(binding_pack(b) == (KEY_S | (KEY_SHIFT << 8)), "pack combo");
  CHECK(!strcmp(binding_name(b, text, sizeof(text)), "shift+S"), "name %s", text);
  b = binding_unpack(KEY_UP);
  CHECK(b.key == 0, "arrows can't be bound");
  b = binding_unpack(KEY_CLICK);
  CHECK(b.key == 0, "click can't be bound");
  b = binding_unpack(3);   /* no key 3 */
  CHECK(b.key == 0, "nonexistent key");
  b = binding_unpack(KEY_S | (KEY_S << 8));
  CHECK(b.key == KEY_S && b.with == 0, "a key with itself is just the key");
  b = binding_unpack(KEY_S | (KEY_UP << 8));
  CHECK(b.key == KEY_S && b.with == 0, "can't hold an arrow");
  b.key = 0;
  b.with = 0;
  CHECK(!strcmp(binding_name(b, text, sizeof(text)), "none"), "none: %s", text);

  for (i = 0; i < NUM_ACTIONS; i++)
    for (slot = 0; slot < KEY_SLOTS; slot++)
    {
      key_binding k = key_bindings[i][slot];
      CHECK(!k.key || key_is_bindable(k.key), "default %d/%d bindable", i, slot);
    }

  /* A USB gamepad's keys, the fifth word: all bindable but its D-pad's
   * (the arrows'), and in a binding's 8 bits. */
  CHECK(NUM_KEYS == 160, "five key words: %d keys", NUM_KEYS);
  CHECK(!strcmp(key_name(KEY_PAD_A), "Pad A") && !strcmp(key_name(KEY_PAD_B), "Pad B") &&
        !strcmp(key_name(KEY_PAD_START), "Pad Start") && !strcmp(key_name(KEY_PAD_AUX), "Pad Aux"),
        "pad key names");
  for (i = KEY_PAD_FIRST; i <= NUM_KEYS; i++)
    CHECK(key_is_bindable(i) == (i >= KEY_PAD_B && i <= KEY_PAD_AUX), "pad key %d bindable: %d", i,
          key_is_bindable(i));
  CHECK(key_name(NUM_KEYS + 1) == NULL, "no key past the last");
  b = binding_unpack(KEY_PAD_A | (KEY_PAD_L1 << 8));
  CHECK(b.key == KEY_PAD_A && b.with == KEY_PAD_L1 && binding_pack(b) == (KEY_PAD_A | (KEY_PAD_L1 << 8)),
        "pad combination packs");
  CHECK(!strcmp(binding_name(b, text, sizeof(text)), "Pad L1+Pad A"), "name %s", text);
}

static long file_size(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

/* Settings and keys go through gpsp.cfg.tns (in the saves folder) and
 * a game's own file. */
static int file_contains(const char *path, const char *text);

static void test_config(void)
{
  char path[600], old_path[600], rom[600], game_cfg[600];
  FILE *f;
  u32 old[37];

  snprintf(main_path, sizeof(main_path), "%s", dir);
  nspire_save_file(path, sizeof(path), "gpsp.cfg.tns");
  snprintf(old_path, sizeof(old_path), "%s/gpsp.cfg.tns", dir);
  remove(path);
  remove(old_path);

  /* Values out of the way of the defaults. */
  keys_set_defaults(key_bindings);
  key_bindings[ACTION_A][1] = binding_unpack(KEY_7);
  key_bindings[ACTION_SAVE_STATE][1] = binding_unpack(KEY_S | (KEY_SHIFT << 8));
  key_bindings[ACTION_RESTART][0] = binding_unpack(KEY_9);
  screen_scale = fullscreen;
  current_frameskip_type = manual_frameskip;
  frameskip_value = 3;
  nspire_fps_overlay = 2;
  nspire_auto_increment = 0;
  nspire_save_on_exit = 0;
  nspire_load_on_start = 1;
  nspire_cpu_speed = 2;
  nspire_rom_buffer_size_choice = 12;
  nspire_audio_enabled = 0; nspire_audio_volume = 4;
  nspire_audio_mono = 1; nspire_audio_buffer = 2;
  CHECK(nspire_config_save() == 0, "save");
  CHECK(file_size(path) == 328, "gpsp.cfg.tns is %ld bytes", file_size(path));
  CHECK(file_size(old_path) < 0, "nothing written next to gpSP");

  keys_set_defaults(key_bindings);
  screen_scale = scaled_raw;
  current_frameskip_type = auto_frameskip;
  frameskip_value = 0;
  nspire_fps_overlay = 0;
  nspire_auto_increment = nspire_save_on_exit = nspire_load_on_start = 1;
  nspire_cpu_speed = 0;
  nspire_rom_buffer_size_choice = 8;
  nspire_audio_enabled = 1; nspire_audio_volume = 7;
  nspire_audio_mono = nspire_audio_buffer = 0;
  CHECK(nspire_config_load() == 0, "load");
  CHECK(!nspire_audio_enabled && nspire_audio_volume == 4 && nspire_audio_mono == 1 && nspire_audio_buffer == 2,
        "USB audio settings round trip without changing file size");
  CHECK(screen_scale == fullscreen && current_frameskip_type == manual_frameskip && frameskip_value == 3,
        "graphics settings %u %u %u", screen_scale, current_frameskip_type, frameskip_value);
  CHECK(nspire_fps_overlay == 2 && !nspire_auto_increment && !nspire_save_on_exit && nspire_load_on_start &&
        nspire_cpu_speed == 2 && nspire_rom_buffer_size_choice == 12, "the new settings");
  CHECK(key_bindings[ACTION_A][0].key == KEY_DEL && key_bindings[ACTION_A][1].key == KEY_7, "A's keys");
  CHECK(key_bindings[ACTION_SAVE_STATE][1].key == KEY_S && key_bindings[ACTION_SAVE_STATE][1].with == KEY_SHIFT,
        "combo kept");
  CHECK(key_bindings[ACTION_RESTART][0].key == KEY_9, "new hotkey kept");

  /* The first start: a 148-byte gpsp.cfg.tns from gpSP before, its keys
   * and settings, new defaults. */
  remove(path);
  memset(old, 0, sizeof(old));
  old[0] = scaled_aspect;
  old[7 + ACTION_UP] = KEY_8;
  old[7 + ACTION_DOWN] = KEY_2;
  old[7 + ACTION_LEFT] = KEY_4;
  old[7 + ACTION_RIGHT] = KEY_6;
  old[7 + ACTION_A] = KEY_CTRL;
  old[7 + ACTION_B] = KEY_SHIFT;
  old[7 + ACTION_L] = 0;
  old[7 + ACTION_R] = 200;   /* garbage */
  old[7 + ACTION_START] = KEY_ENTER;
  old[7 + ACTION_SELECT] = KEY_VAR;
  old[7 + ACTION_MENU] = KEY_ESC;
  old[7 + ACTION_FAST_FORWARD] = KEY_F;
  old[7 + ACTION_LOAD_STATE] = KEY_L;
  old[7 + ACTION_SAVE_STATE] = KEY_S;
  old[7 + ACTION_QUIT] = KEY_Q;
  old[23] = 1;
  old[24] = manual_frameskip;
  old[25] = 2;
  old[26] = 0xFF000000u;
  old[28] = 1;   /* sprite limit off */
  old[29] = 2;   /* RTC disabled */
  old[30] = 1;
  old[31] = 12;
  old[32] = 1;
  old[33] = 1;
  old[34] = 1;
  old[35] = 1;
  old[36] = 1;   /* the removed video renderer: Legacy (video.c) selected */
  f = fopen(old_path, "wb");
  fwrite(old, 4, 37, f);
  fclose(f);
  nspire_auto_increment = 0;
  CHECK(nspire_config_load() == 0, "load an old file");
  CHECK(nspire_audio_enabled == 1 && nspire_audio_volume == NSPIRE_VOLUME_DEFAULT && !nspire_audio_mono &&
        !nspire_audio_buffer, "old files get USB audio defaults");
  /* r19: the volume is 0 to 20. The file's word 3 has it in bits 16-20, bit 21 saying so, and half of it
   * (rounded) in bits 4-7 where a build before r19 reads its eleven steps; a file without bit 21 is one of
   * those builds': its volume is twice its 0-10. */
  {
    u32 words[82];
    nspire_audio_volume = 17;
    CHECK(nspire_config_save() == 0, "save the volume");
    f = fopen(path, "rb");
    CHECK(f && fread(words, 4, 82, f) == 82, "read the settings back");
    if (f)
      fclose(f);
    CHECK((words[3] & 0x00200000u) && ((words[3] >> 16) & 31) == 17 && ((words[3] >> 4) & 15) == 9,
          "the volume's 21 steps in word 3, half of it for older builds: %08x", (unsigned)words[3]);
    nspire_audio_volume = 0;
    CHECK(nspire_config_load() == 0 && nspire_audio_volume == 17, "17/20 round trip: %u", nspire_audio_volume);
    words[3] = (words[3] & ~0x003F00F0u) | (7u << 4);   /* as a build before r19 wrote 70% */
    f = fopen(path, "wb");
    CHECK(f && fwrite(words, 4, 82, f) == 82, "write an older build's word");
    if (f)
      fclose(f);
    CHECK(nspire_config_load() == 0 && nspire_audio_volume == 14, "an older file's 7 of 10 is 14 of 20: %u",
          nspire_audio_volume);
    nspire_audio_volume = 20;
    CHECK(nspire_config_save() == 0 && nspire_config_load() == 0 && nspire_audio_volume == 20, "20/20 round trip");
    nspire_audio_volume = NSPIRE_VOLUME_DEFAULT;
    CHECK(nspire_config_save() == 0, "the default volume saved again");
  }
  /* final/r19-bank: word 2, the longest OS call of a save learned (nspire_audio.c's bank): 0 in the file
   * until something was learned; then 0x5C mark | ms, read back eased a quarter; 0 again once it is 0 */
  {
    u32 words[82];
    extern u32 nspire_save_call_ms;
    CHECK(nspire_save_call_ms == 0, "nothing learned yet: %u", nspire_save_call_ms);
    f = fopen(path, "rb");
    CHECK(f && fread(words, 4, 82, f) == 82 && words[2] == 0, "word 2 is 0 until learned: %08x", (unsigned)words[2]);
    if (f)
      fclose(f);
    nspire_save_call_ms = 230;
    CHECK(nspire_config_save() == 0, "save what was learned");
    f = fopen(path, "rb");
    CHECK(f && fread(words, 4, 82, f) == 82 && words[2] == 0x5C0000E6u, "word 2: %08x", (unsigned)words[2]);
    if (f)
      fclose(f);
    CHECK(file_size(path) == 328, "the settings' size kept: %ld", file_size(path));
    nspire_save_call_ms = 0;
    CHECK(nspire_config_load() == 0 && nspire_save_call_ms == 173, "230 ms read back eased: %u",
          nspire_save_call_ms);
    nspire_save_call_ms = 0;
    CHECK(nspire_config_save() == 0 && nspire_config_load() == 0 && nspire_save_call_ms == 0, "0 round trip");
    f = fopen(path, "rb");
    CHECK(f && fread(words, 4, 82, f) == 82 && words[2] == 0, "word 2 back to 0: %08x", (unsigned)words[2]);
    if (f)
      fclose(f);
  }
  /* Older builds showed every scaling as the 320x214 picture, so that's
   * what their settings start with. */
  CHECK(screen_scale == scaled_raw && current_frameskip_type == manual_frameskip && frameskip_value == 2 &&
        nspire_fps_overlay == 1, "old settings");
  /* The removed renderer's word (36) holds 1 here: it's no error, and every
   * other setting around it keeps its value, except blending Off (word 34 = 1):
   * the legacy renderer never honoured it, so it loads as On (F3). */
  CHECK(nspire_sprlim_choice == 1 && nspire_rtc_choice == 2 && nspire_frame_mix_choice == 1 &&
        nspire_rom_buffer_size_choice == 12 && nspire_lcd_cache_clean_full == 1 && nspire_gba_blend_off == 0 &&
        nspire_dynarec_ram_policy == 1 && nspire_bios_choice == 1,
        "settings next to the removed renderer's word: %u %u %u %u %u %u %u", nspire_sprlim_choice,
        nspire_rtc_choice, nspire_frame_mix_choice, nspire_rom_buffer_size_choice, nspire_lcd_cache_clean_full,
        nspire_gba_blend_off, nspire_dynarec_ram_policy);
  CHECK(key_bindings[ACTION_A][0].key == KEY_CTRL && key_bindings[ACTION_MENU][0].key == KEY_ESC &&
        key_bindings[ACTION_QUIT][0].key == KEY_Q, "old keys");
  CHECK(key_bindings[ACTION_L][0].key == 0 && key_bindings[ACTION_R][0].key == 0, "unbound and garbage keys");
  CHECK(key_bindings[ACTION_A][1].key == 0 && key_bindings[ACTION_RESTART][0].key == 0, "new slots empty");
  CHECK(key_bindings[ACTION_BRIGHTER][0].key == KEY_PLUS && key_bindings[ACTION_DIMMER][0].key == KEY_MINUS,
        "brightness keys, + and - being free");
  /* A file written before has auto_increment etc. at their defaults: they
   * aren't in it, so whatever was set stays (main() starts from defaults).
   * Saving writes this build's own file and leaves the old one alone. */
  CHECK(nspire_config_save() == 0, "save after the old file");
  CHECK(file_size(path) == 328 && file_size(old_path) == 148, "files %ld %ld", file_size(path),
        file_size(old_path));
  {
    u32 saved[72];
    f = fopen(path, "rb");
    CHECK(f && fread(saved, 4, 72, f) == 72, "read the saved file");
    if (f)
      fclose(f);
    CHECK(saved[36] == 0 && saved[28] == 1 && saved[29] == 2 && saved[31] == 12 && saved[35] == 1,
          "word 36 is written as 0 (reserved), its neighbours as they were: %u %u %u %u", saved[36], saved[35],
          saved[31], saved[29]);
  }

  /* A game's own settings. */
  snprintf(rom, sizeof(rom), "%s/Game.gba.tns", dir);
  nspire_save_file(game_cfg, sizeof(game_cfg), "Game.gba.settings.tns");
  remove(game_cfg);
  screen_scale = scaled_raw;
  frameskip_value = 4;
  nspire_config_open_game(rom);
  CHECK(nspire_settings_for == 0 && nspire_keys_for == 0, "no own settings yet");
  nspire_settings_for = 1;
  nspire_config_settings_for_changed();
  screen_scale = unscaled;   /* the game's own */
  nspire_audio_volume = 3; nspire_audio_mono = 1;
  nspire_keys_for = 1;
  nspire_config_keys_for_changed();
  key_bindings[ACTION_B][0] = binding_unpack(KEY_9);
  CHECK(nspire_config_save() == 0, "save with a game open");
  CHECK(file_size(game_cfg) > 0, "game file written");
  nspire_config_close_game();
  CHECK(screen_scale == scaled_raw, "all games' scale back: %u", screen_scale);
  CHECK(nspire_audio_volume == NSPIRE_VOLUME_DEFAULT && !nspire_audio_mono, "global sound restored after leaving a game");
  CHECK(file_contains(game_cfg, "volume20=3") && !file_contains(game_cfg, "audio_volume="),
        "a game's file has the volume as volume20");
  CHECK(key_bindings[ACTION_B][0].key == KEY_SHIFT, "all games' keys back");

  /* The compressed ROM has the same settings. */
  snprintf(rom, sizeof(rom), "%s/Game.gbz.tns", dir);
  nspire_config_open_game(rom);
  CHECK(nspire_settings_for == 1 && nspire_keys_for == 1, "own settings found");
  CHECK(screen_scale == unscaled && key_bindings[ACTION_B][0].key == KEY_9, "own values used");
  CHECK(nspire_audio_volume == 3 && nspire_audio_mono, "per-game sound survives ROM format changes");
  frameskip_value = 7;   /* a change while the game has its own settings */
  nspire_config_close_game();
  CHECK(frameskip_value == 4, "all games' frameskip untouched: %u", frameskip_value);
  /* R6: the game's file written whole as a .tmp then renamed: a lone whole
   * .tmp (a reset between) is the file; one without its last line isn't. */
  {
    char tmp[620];
    snprintf(tmp, sizeof(tmp), "%s.tmp", game_cfg);
    CHECK(file_contains(game_cfg, "end=1") && file_size(tmp) < 0, "the game file ends with end=1, no .tmp left");
    rename(game_cfg, tmp);
    nspire_config_open_game(rom);
    CHECK(nspire_settings_for == 1 && frameskip_value == 7 && file_size(game_cfg) > 0 && file_size(tmp) < 0,
          "a lone whole .tmp is the game's file");
    nspire_config_close_game();
    rename(game_cfg, tmp);
    CHECK(truncate(tmp, file_size(tmp) - 6) == 0, "cut");   /* without "end=1" */
    nspire_config_open_game(rom);
    CHECK(nspire_settings_for == 0 && file_size(tmp) < 0, "a .tmp without its last line is dropped");
    nspire_config_close_game();
    nspire_config_open_game(rom);   /* its own settings back, as the test goes on */
    nspire_settings_for = 1;
    nspire_config_settings_for_changed();
    screen_scale = unscaled;
    nspire_keys_for = 1;
    nspire_config_keys_for_changed();
    key_bindings[ACTION_B][0] = binding_unpack(KEY_9);
    frameskip_value = 7;
    nspire_config_close_game();
  }
  nspire_config_load();
  CHECK(screen_scale == scaled_raw, "the file for all games kept its scale: %u", screen_scale);

  nspire_config_open_game(rom);
  CHECK(frameskip_value == 7, "the game's change was kept: %u", frameskip_value);
  nspire_settings_for = 0;
  nspire_config_settings_for_changed();
  CHECK(screen_scale == scaled_raw && frameskip_value == 4, "back to all games' settings");
  nspire_keys_for = 0;
  nspire_config_keys_for_changed();
  nspire_config_close_game();
  CHECK(file_size(game_cfg) < 0, "the game's file goes when it has nothing of its own");

  /* Changes made while a game has no settings of its own are for all games. */
  nspire_config_open_game(rom);
  nspire_fps_overlay = 1;
  nspire_config_close_game();
  CHECK(nspire_fps_overlay == 1, "kept for all games");
}

static void write_words(const char *path, const u32 *words, u32 count)
{
  FILE *f = fopen(path, "wb");
  fwrite(words, 4, count, f);
  fclose(f);
}

static int file_contains(const char *path, const char *text)
{
  char buffer[4096];
  size_t n;
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  n = fread(buffer, 1, sizeof(buffer) - 1, f);
  fclose(f);
  buffer[n] = 0;
  return strstr(buffer, text) != NULL;
}

/* This build's settings, and files from the first QoL build ("QOL1"). */
static void test_config_versions(void)
{
  char path[600], old_path[600], rom[600], game_cfg[600];
  u32 o[72];
  FILE *f;

  snprintf(main_path, sizeof(main_path), "%s", dir);
  nspire_save_file(path, sizeof(path), "gpsp.cfg.tns");
  snprintf(old_path, sizeof(old_path), "%s/gpsp.cfg.tns", dir);
  remove(old_path);

  /* Brightness and the brightness keys go round. */
  keys_set_defaults(key_bindings);
  CHECK(key_bindings[ACTION_BRIGHTER][0].key == KEY_PLUS && key_bindings[ACTION_DIMMER][0].key == KEY_MINUS,
        "brightness defaults");
  key_bindings[ACTION_BRIGHTER][1] = binding_unpack(KEY_9 | (KEY_SHIFT << 8));
  key_bindings[ACTION_DIMMER][0] = binding_unpack(KEY_7);
  nspire_brightness = 7;
  nspire_cpu_speed = 9;
  screen_scale = unscaled;   /* chosen in this build: kept */
  CHECK(nspire_config_save() == 0, "save");
  keys_set_defaults(key_bindings);
  nspire_brightness = 0;
  nspire_cpu_speed = 0;
  screen_scale = scaled_raw;
  CHECK(nspire_config_load() == 0, "load");
  CHECK(nspire_brightness == 7 && nspire_cpu_speed == 9 && screen_scale == unscaled,
        "brightness %u, cpu %u, scale %u", nspire_brightness, nspire_cpu_speed, screen_scale);
  CHECK(key_bindings[ACTION_BRIGHTER][0].key == KEY_PLUS && key_bindings[ACTION_BRIGHTER][1].key == KEY_9 &&
        key_bindings[ACTION_BRIGHTER][1].with == KEY_SHIFT && key_bindings[ACTION_DIMMER][0].key == KEY_7,
        "brightness keys kept");
  CHECK(key_bindings[ACTION_RESTART][0].key == 0 && key_bindings[ACTION_A][0].key == KEY_DEL, "others kept");

  /* The first QoL build's file: its unscaled picture becomes the 320x214
   * one, its CPU speeds (normal, 432, 456, 480) move to today's list, and
   * brightness gets + and - unless they're taken. */
  memset(o, 0, sizeof(o));
  o[0] = unscaled;
  o[37] = 0x314C4F51u;   /* "QOL1" */
  o[38 + ACTION_START] = KEY_PLUS;   /* + on Start */
  o[38 + ACTION_A] = KEY_DEL;
  o[58] = 1;
  o[59] = 1;
  o[60] = 1;
  o[61] = 1;   /* 432 MHz */
  o[62] = 1;
  write_words(path, o, 72);
  keys_set_defaults(key_bindings);
  nspire_brightness = 4;
  CHECK(nspire_config_load() == 0, "load QOL1");
  CHECK(screen_scale == scaled_raw, "QOL1's unscaled is now the 320x214 picture: %u", screen_scale);
  CHECK(nspire_cpu_speed == 3, "432 MHz is speed %u now", nspire_cpu_speed);
  CHECK(key_bindings[ACTION_START][0].key == KEY_PLUS, "QOL1's keys");
  CHECK(key_bindings[ACTION_BRIGHTER][0].key == 0 && key_bindings[ACTION_BRIGHTER][1].key == 0,
        "+ is Start's, so brightness up has no key");
  CHECK(key_bindings[ACTION_DIMMER][0].key == KEY_MINUS, "- was free");
  CHECK(nspire_brightness == 4, "brightness isn't in QOL1 files: %u", nspire_brightness);
  o[61] = 3;   /* 480 MHz */
  write_words(path, o, 72);
  nspire_config_load();
  CHECK(nspire_cpu_speed == 7, "480 MHz is speed %u now", nspire_cpu_speed);
  o[61] = 2;
  write_words(path, o, 72);
  nspire_config_load();
  CHECK(nspire_cpu_speed == 5, "456 MHz is speed %u now", nspire_cpu_speed);
  CHECK(nspire_config_save() == 0 && file_size(path) == 328, "saved as this build's");
  f = fopen(path, "rb");
  CHECK(f && fread(o, 4, 72, f) == 72, "read back");
  if (f)
    fclose(f);
  CHECK(o[37] == 0x334C4F51u && o[61] == 1005 && o[0] == scaled_raw && o[66] == 1,
        "QOL3 %08x, cpu %u, scale %u, compress %u", o[37], o[61], o[0], o[66]);

  /* A game's own settings from the first QoL build (no version line). */
  snprintf(rom, sizeof(rom), "%s/Old.gba.tns", dir);
  nspire_save_file(game_cfg, sizeof(game_cfg), "Old.gba.settings.tns");
  f = fopen(game_cfg, "w");
  /* "renderer=1" (Legacy) was a setting of its own then: ignored now. */
  fprintf(f, "own_settings=1\nscreen_scale=0\ncpu_speed=3\nrenderer=1\nframeskip_value=6\nrtc=1\n");
  fclose(f);
  screen_scale = fullscreen;
  nspire_rtc_choice = 0;
  nspire_config_open_game(rom);
  CHECK(screen_scale == scaled_raw && nspire_cpu_speed == 7 && frameskip_value == 6 && nspire_rtc_choice == 1,
        "game file: scale %u cpu %u frameskip %u rtc %u", screen_scale, nspire_cpu_speed, frameskip_value,
        nspire_rtc_choice);
  nspire_config_close_game();
  /* (version=5 since final/pace: the pace rule's mark, test_pace_rule_mark;
   * it was 4) */
  CHECK(file_contains(game_cfg, "version=5") && file_contains(game_cfg, "cpu_speed=1007") &&
        file_contains(game_cfg, "rtc=1") && !file_contains(game_cfg, "renderer"), "rewritten, without renderer");
  nspire_config_open_game(rom);
  CHECK(nspire_cpu_speed == 7 && screen_scale == scaled_raw, "a version 3 file is read as it is");
  screen_scale = unscaled;
  nspire_config_close_game();
  nspire_config_open_game(rom);
  CHECK(screen_scale == unscaled, "unscaled chosen for the game since stays");
  nspire_settings_for = 0;
  nspire_config_settings_for_changed();
  nspire_config_close_game();
  remove(game_cfg);
  remove(path);
}

static int read_settings_words(const char *path, u32 *words, u32 count)
{
  FILE *f = fopen(path, "rb");
  int ok = f && fread(words, 4, count, f) == count;
  if (f)
    fclose(f);
  return ok;
}

/* The pace rule's once (2026-11-15, final/pace; nspire_config.c
 * PACE_RULE_MARK, word 26 bit 0): automatic frameskip's rule
 * is wanted on calculators, where Off is stored. A file without the mark that
 * says Off loads as Automatic; every file written has the mark, so an Off
 * chosen after that stays Off. The file's size and words otherwise as they
 * were; a game's own file the same by its version (5). */
static void test_pace_rule_mark(void)
{
  char path[600], rom[600], game_cfg[600];
  u32 o[82];
  FILE *f;

  snprintf(main_path, sizeof(main_path), "%s", dir);
  nspire_save_file(path, sizeof(path), "gpsp.cfg.tns");
  current_frameskip_type = no_frameskip;
  frameskip_value = 1;   /* the reference */
  CHECK(nspire_config_save() == 0 && file_size(path) == 328, "save: %ld bytes", file_size(path));
  CHECK(read_settings_words(path, o, 82) && o[24] == no_frameskip && o[25] == 1 && o[26] == 0xFF000001u,
        "this build writes Off with the mark: %u %u %08x", o[24], o[25], o[26]);

  /* the reference file as the builds before wrote it: Off, no mark */
  o[26] = 0xFF000000u;
  write_words(path, o, 82);
  current_frameskip_type = manual_frameskip;
  CHECK(nspire_config_load() == 0, "load the old file");
  CHECK(current_frameskip_type == auto_frameskip && frameskip_value == 1,
        "a stored Off without the mark is Automatic (its value kept): %u %u", current_frameskip_type, frameskip_value);
  CHECK(nspire_config_save() == 0 && read_settings_words(path, o, 82) && o[24] == auto_frameskip && o[26] == 0xFF000001u &&
        file_size(path) == 328, "written back Automatic, with the mark, the same size: %u %08x", o[24], o[26]);

  /* Off chosen after that: Off from then on */
  current_frameskip_type = no_frameskip;
  CHECK(nspire_config_save() == 0, "save the chosen Off");
  current_frameskip_type = auto_frameskip;
  nspire_config_load();
  CHECK(current_frameskip_type == no_frameskip, "an Off chosen after the mark stays Off: %u", current_frameskip_type);
  nspire_config_save();
  nspire_config_load();
  CHECK(current_frameskip_type == no_frameskip, "and again: %u", current_frameskip_type);

  /* Manual and Automatic without the mark stay as they were; an Off of
   * value 0 gets 4 (0 is never skip for Automatic, as before for it) */
  read_settings_words(path, o, 82);
  o[24] = manual_frameskip;
  o[25] = 3;
  o[26] = 0xFF000000u;
  write_words(path, o, 82);
  nspire_config_load();
  CHECK(current_frameskip_type == manual_frameskip && frameskip_value == 3, "Manual without the mark kept: %u %u",
        current_frameskip_type, frameskip_value);
  o[24] = no_frameskip;
  o[25] = 0;
  write_words(path, o, 82);
  nspire_config_load();
  CHECK(current_frameskip_type == auto_frameskip && frameskip_value == 4, "Off with value 0: Automatic, 4: %u %u",
        current_frameskip_type, frameskip_value);
  /* the bit alone, without the newer encoding's top byte, is no mark */
  o[24] = 0;   /* the older encoding's 0: Off */
  o[25] = 2;
  o[26] = 1;
  write_words(path, o, 82);
  nspire_config_load();
  CHECK(current_frameskip_type == auto_frameskip, "an older encoding's Off: Automatic: %u", current_frameskip_type);

  /* A game's own settings from before (version 4): Off is Automatic once */
  current_frameskip_type = auto_frameskip;
  frameskip_value = 4;
  snprintf(rom, sizeof(rom), "%s/Pace.gba.tns", dir);
  nspire_save_file(game_cfg, sizeof(game_cfg), "Pace.gba.settings.tns");
  f = fopen(game_cfg, "w");
  fprintf(f, "version=4\nown_settings=1\nframeskip_type=2\nframeskip_value=1\nend=1\n");
  fclose(f);
  nspire_config_open_game(rom);
  CHECK(nspire_settings_for == 1 && current_frameskip_type == auto_frameskip && frameskip_value == 1,
        "a game's own Off from before: Automatic: %u %u", current_frameskip_type, frameskip_value);
  nspire_config_close_game();
  CHECK(file_contains(game_cfg, "version=5") && file_contains(game_cfg, "frameskip_type=0"),
        "the game's file written as version 5, Automatic");
  nspire_config_open_game(rom);
  current_frameskip_type = no_frameskip;   /* Off chosen for the game after */
  nspire_config_close_game();
  CHECK(current_frameskip_type == auto_frameskip, "all games' setting back: %u", current_frameskip_type);
  nspire_config_open_game(rom);
  CHECK(current_frameskip_type == no_frameskip, "the game's Off chosen after stays: %u", current_frameskip_type);
  nspire_settings_for = 0;
  nspire_config_settings_for_changed();
  nspire_config_close_game();
  remove(game_cfg);
  remove(path);
  current_frameskip_type = auto_frameskip;
  frameskip_value = 4;
}

/* The slot keys, ) and ( as in PocketSNES: defaults, and settings from
 * before them (QOL2 files, version 2 game files) getting them only where
 * the keys are free; QOL3 files keep what was chosen, even no key. */
/* The settings for all games go inside the program file too, and come from
 * there first; a program without them (a new one copied over) takes the
 * saves folder's copy, and before that this build's file next to gpSP. */
static void test_settings_in_program(void)
{
  char program[600], copy[600], beside[600];
  long bare, stored;
  u8 data[400];
  FILE *f;
  int i;

#ifdef __linux__
  /* An I/O failure while locating the settings area must never fall
   * through to a write at an uninitialised append offset. */
  {
    int ends[2];
    char nonseekable[64], byte;
    CHECK(pipe(ends) == 0, "non-seekable settings destination");
    snprintf(nonseekable, sizeof(nonseekable), "/proc/self/fd/%d", ends[1]);
    CHECK(!selfstore_write(nonseekable, "settings", 8), "a failed seek rejects the write");
    close(ends[1]);
    CHECK(read(ends[0], &byte, 1) == 0, "a failed seek writes nothing");
    close(ends[0]);
  }
#endif

  snprintf(main_path, sizeof(main_path), "%s", dir);
  snprintf(program, sizeof(program), "%s/_gpSP.tns", dir);
  nspire_save_file(copy, sizeof(copy), "gpsp.cfg.tns");
  snprintf(beside, sizeof(beside), "%s/gpsp_qol.cfg.tns", dir);
  f = fopen(program, "wb");
  for (i = 0; i < 5000; i++)
    fputc(i & 0xFF, f);
  fclose(f);
  bare = file_size(program);
  snprintf(nspire_program_path, sizeof(nspire_program_path), "%.500s", program);

  keys_set_defaults(key_bindings);
  nspire_brightness = 4;
  CHECK(nspire_config_save() == 0, "save");
  stored = file_size(program);
  CHECK(stored == bare + SELFSTORE_SIZE + 16, "the program file grew by its settings area: %ld", stored);
  CHECK(selfstore_read(program, data, sizeof(data)) == 328, "328 bytes stored in it");
  f = fopen(program, "rb");
  for (i = 0; f && i < 5000 && fgetc(f) == (i & 0xFF); i++)
    ;
  if (f)
    fclose(f);
  CHECK(i == 5000, "the program's own bytes untouched (%d)", i);
  CHECK(file_size(copy) == 328, "and the copy in the saves folder");

  /* An older build's area with the removed renderer's word (36) set to 1:
   * loads without an error, the settings around it kept. */
  nspire_sprlim_choice = 1;
  nspire_rtc_choice = 2;
  nspire_dynarec_ram_policy = 1;
  CHECK(nspire_config_save() == 0, "save with the neighbours set");
  CHECK(selfstore_read(program, data, sizeof(data)) == 328, "read the area");
  CHECK(!memcmp(data + 36 * 4, "\0\0\0\0", 4), "word 36 is written as 0");
  data[36 * 4] = 1;
  CHECK(selfstore_write(program, data, 288), "an older build's area (72 words) with Legacy selected");
  nspire_sprlim_choice = nspire_rtc_choice = nspire_dynarec_ram_policy = 0;
  remove(copy);
  CHECK(nspire_config_load() == 0 && nspire_sprlim_choice == 1 && nspire_rtc_choice == 2 &&
        nspire_dynarec_ram_policy == 1 && nspire_brightness == 4,
        "loaded from the old area: %u %u %u %u", nspire_sprlim_choice, nspire_rtc_choice,
        nspire_dynarec_ram_policy, nspire_brightness);
  nspire_sprlim_choice = nspire_rtc_choice = nspire_dynarec_ram_policy = 0;
  CHECK(nspire_config_save() == 0, "save over it");
  CHECK(selfstore_read(program, data, sizeof(data)) == 328 && !data[36 * 4], "word 36 is 0 again");

  /* From the program file first. */
  remove(copy);
  nspire_brightness = 9;
  CHECK(nspire_config_load() == 0 && nspire_brightness == 4, "loaded from the program: %u", nspire_brightness);
  nspire_brightness = 6;
  CHECK(nspire_config_save() == 0 && file_size(program) == stored, "saved again in place");
  CHECK(nspire_config_load() == 0 && nspire_brightness == 6, "the new value: %u", nspire_brightness);

  /* A new program file copied over: the copy in the saves folder. */
  f = fopen(program, "wb");
  for (i = 0; i < 5000; i++)
    fputc(i & 0xFF, f);
  fclose(f);
  nspire_brightness = 2;
  CHECK(nspire_config_load() == 0 && nspire_brightness == 6, "from the saves folder's copy: %u",
        nspire_brightness);
  CHECK(nspire_config_save() == 0 && file_size(program) == stored, "back in the program");

  /* R6: a reset in the program file's write (half new, half old): its
   * sum doesn't match, and the saves folder's copy (written first, whole)
   * is read instead. */
  {
    char tmp[620];
    nspire_brightness = 7;
    CHECK(nspire_config_save() == 0, "save 7");
    f = fopen(program, "r+b");
    fseek(f, bare + 200, SEEK_SET);   /* inside the 288 bytes */
    fputc(0x5A, f);
    fclose(f);
    CHECK(selfstore_read(program, data, sizeof(data)) < 0, "a damaged program-file copy isn't read");
    nspire_brightness = 1;
    CHECK(nspire_config_load() == 0 && nspire_brightness == 7, "the saves folder's copy instead: %u",
          nspire_brightness);
    /* A reset between the saves folder copy's removal and its rename: its
     * .tmp alone, whole, is the copy; a short one is dropped. */
    snprintf(tmp, sizeof(tmp), "%s.tmp", copy);
    rename(copy, tmp);
    nspire_brightness = 1;
    CHECK(nspire_config_load() == 0 && nspire_brightness == 7 && file_size(copy) == 328 && file_size(tmp) < 0,
          "a lone whole .tmp is the saves folder's copy: %u", nspire_brightness);
    rename(copy, tmp);
    CHECK(truncate(tmp, 100) == 0, "cut");
    nspire_brightness = 1;
    CHECK(nspire_config_load() != 0 || nspire_brightness != 7, "a short .tmp isn't read");
    CHECK(file_size(tmp) < 0 && file_size(copy) < 0, "and is dropped");
    nspire_brightness = 6;
    CHECK(nspire_config_save() == 0, "saved whole again");
  }

  /* Before the saves folder: the file next to gpSP. */
  rename(copy, beside);
  f = fopen(program, "wb");
  fclose(f);
  nspire_brightness = 1;
  CHECK(nspire_config_load() == 0 && nspire_brightness == 6, "from the file next to gpSP: %u",
        nspire_brightness);
  remove(beside);
  remove(program);
  nspire_program_path[0] = 0;
  nspire_brightness = 0;
  remove(copy);
}

static void test_slot_keys(void)
{
  char path[600], rom[600], game_cfg[600];
  u32 o[72];
  FILE *f;

  keys_set_defaults(key_bindings);
  CHECK(key_bindings[ACTION_NEXT_SLOT][0].key == KEY_RPAREN, "next slot: )");
  CHECK(key_bindings[ACTION_PREVIOUS_SLOT][0].key == KEY_LPAREN, "previous slot: (");

  /* A QOL2 file (the build before) with the slots unbound and ( on Start. */
  nspire_save_file(path, sizeof(path), "gpsp.cfg.tns");
  keys_set_defaults(key_bindings);
  key_bindings[ACTION_NEXT_SLOT][0].key = 0;
  key_bindings[ACTION_PREVIOUS_SLOT][0].key = 0;
  key_bindings[ACTION_START][1].key = KEY_LPAREN;
  nspire_compress_saves = 0;
  CHECK(nspire_config_save() == 0, "save");
  f = fopen(path, "r+b");
  CHECK(f && fread(o, 4, 72, f) == 72, "read");
  o[37] = 0x324C4F51u;   /* "QOL2" */
  o[66] = 0;
  fseek(f, 0, SEEK_SET);
  fwrite(o, 4, 72, f);
  fclose(f);
  keys_set_defaults(key_bindings);
  nspire_compress_saves = 0;
  CHECK(nspire_config_load() == 0, "load QOL2");
  CHECK(key_bindings[ACTION_NEXT_SLOT][0].key == KEY_RPAREN, "QOL2: next slot gets ), which is free");
  CHECK(key_bindings[ACTION_PREVIOUS_SLOT][0].key == 0 && key_bindings[ACTION_PREVIOUS_SLOT][1].key == 0,
        "QOL2: ( is Start's second key, so previous slot has none");
  CHECK(key_bindings[ACTION_START][1].key == KEY_LPAREN, "Start keeps (");
  CHECK(nspire_compress_saves == 1, "QOL2: Compress saves starts on");

  /* Saved as QOL3: a slot key cleared on purpose stays cleared. */
  memset(key_bindings[ACTION_NEXT_SLOT], 0, sizeof(key_bindings[ACTION_NEXT_SLOT]));
  nspire_compress_saves = 0;
  CHECK(nspire_config_save() == 0, "save QOL3");
  keys_set_defaults(key_bindings);
  nspire_compress_saves = 1;
  CHECK(nspire_config_load() == 0, "load QOL3");
  CHECK(key_bindings[ACTION_NEXT_SLOT][0].key == 0, "QOL3: next slot stays without a key");
  CHECK(nspire_compress_saves == 0, "QOL3: Compress saves off is kept");

  /* A game's own keys from before version 3. */
  keys_set_defaults(key_bindings);
  snprintf(rom, sizeof(rom), "%s/Keys.gba.tns", dir);
  nspire_save_file(game_cfg, sizeof(game_cfg), "Keys.gba.settings.tns");
  f = fopen(game_cfg, "w");
  fprintf(f, "version=2\nown_keys=1\nkey_next_slot=0\nkey_previous_slot=0\nkey_a=%u\n", KEY_RPAREN);
  fclose(f);
  nspire_config_open_game(rom);
  CHECK(key_bindings[ACTION_A][0].key == KEY_RPAREN, "the game's A is )");
  CHECK(key_bindings[ACTION_NEXT_SLOT][0].key == 0, "so its next slot has no key");
  CHECK(key_bindings[ACTION_PREVIOUS_SLOT][0].key == KEY_LPAREN, "and its previous slot gets (");
  nspire_config_close_game();
  /* this build's version: 5 since final/pace (the pace rule's mark; was 4) */
  CHECK(file_contains(game_cfg, "version=5"), "the game file is version 5 now");
  remove(game_cfg);
  nspire_compress_saves = 1;
  remove(path);
}

static void test_brightness(void)
{
  u32 level;
  int i;

  CHECK(nspire_brightness_value(BRIGHTNESS_LEVELS) == 0 && nspire_brightness_value(1) == 225,
        "the OS's range: 0 brightest, 225 darkest");
  for (level = 1; level <= BRIGHTNESS_LEVELS; level++)
  {
    CHECK(nspire_brightness_level(nspire_brightness_value(level)) == level, "level %u round trip", level);
    if (level > 1)
      CHECK(nspire_brightness_value(level) < nspire_brightness_value(level - 1), "level %u brighter", level);
  }
  CHECK(nspire_brightness_level(255) == 1 && nspire_brightness_level(0) == BRIGHTNESS_LEVELS &&
        nspire_brightness_level(120) == 5 && nspire_brightness_level(13) == 9 && nspire_brightness_level(12) == 10,
        "nearest levels");
  /* An older CX: 0x11A dimmest to 0x1CE brightest, higher brighter. */
  CHECK(nspire_brightness_cx_value(1) == 0x11A && nspire_brightness_cx_value(BRIGHTNESS_LEVELS) == 0x1CE,
        "the CX's range: 0x11a dimmest, 0x1ce brightest");
  for (level = 1; level <= BRIGHTNESS_LEVELS; level++)
  {
    CHECK(nspire_brightness_cx_level(nspire_brightness_cx_value(level)) == level, "CX level %u round trip", level);
    if (level > 1)
      CHECK(nspire_brightness_cx_value(level) > nspire_brightness_cx_value(level - 1), "CX level %u brighter", level);
  }
  CHECK(nspire_brightness_cx_level(0x174) == 6 && nspire_brightness_cx_level(0x101) == 1 &&
        nspire_brightness_cx_level(0x1D0) == BRIGHTNESS_LEVELS, "CX nearest levels");

  /* Setting 0 leaves the calculator's own alone. */
  unit_backlight = 120;
  unit_backlight_writes = 0;
  nspire_brightness = 0;
  nspire_brightness_init();
  nspire_brightness_apply();
  nspire_brightness_restore();
  CHECK(unit_backlight_writes == 0 && unit_backlight == 120, "untouched: %ld writes", unit_backlight_writes);

  /* The hotkeys start from the level nearest the calculator's. */
  nspire_brightness_step(1);
  CHECK(nspire_brightness == 6 && unit_backlight == 100 && !strcmp(unit_osd_message, "Brightness 6/10"),
        "one up: level %u, %u, \"%s\"", nspire_brightness, unit_backlight, unit_osd_message);
  for (i = 0; i < 12; i++)
    nspire_brightness_step(1);
  CHECK(nspire_brightness == 10 && unit_backlight == 0, "stops at the brightest");
  for (i = 0; i < 12; i++)
    nspire_brightness_step(-1);
  CHECK(nspire_brightness == 1 && unit_backlight == 225, "stops at the darkest");
  nspire_brightness = 0;
  nspire_brightness_apply();
  CHECK(unit_backlight == 120, "back to the calculator's own");
  nspire_brightness = 3;
  nspire_brightness_apply();
  CHECK(unit_backlight == nspire_brightness_value(3), "level 3");
  nspire_brightness = 99;
  nspire_brightness_apply();
  CHECK(nspire_brightness == 0 && unit_backlight == 120, "a bad setting is the calculator's own");
  nspire_brightness = 8;
  nspire_brightness_apply();
  nspire_brightness_restore();
  CHECK(unit_backlight == 120, "gpSP closing puts the calculator's own back");

  /* A register that doesn't read like the OS's is left alone. */
  unit_backlight = 0x12345;
  unit_backlight_writes = 0;
  nspire_brightness_init();
  nspire_brightness = 5;
  nspire_brightness_apply();
  nspire_brightness_step(1);
  nspire_brightness_restore();
  CHECK(unit_backlight_writes == 0 && !strcmp(unit_osd_message, "Brightness: not on this calculator"),
        "unknown register: %ld writes, \"%s\"", unit_backlight_writes, unit_osd_message);

  /* An older CX: its register (0x900F0020), the calculator's own back at
   * the end; with the backlight off (0x100) it's left alone. */
  host_is_cx2 = 0;
  unit_backlight = 0x174;
  unit_backlight_writes = 0;
  nspire_brightness_init();
  nspire_brightness = BRIGHTNESS_LEVELS;
  nspire_brightness_apply();
  CHECK(unit_backlight == 0x1CE, "CX: the brightest level: %x", (unsigned)unit_backlight);
  nspire_brightness_step(-1);
  CHECK(unit_backlight == nspire_brightness_cx_value(BRIGHTNESS_LEVELS - 1), "CX: one dimmer");
  nspire_brightness_restore();
  CHECK(unit_backlight == 0x174, "CX: the calculator's own back");
  unit_backlight = 0x100;
  unit_backlight_writes = 0;
  nspire_brightness_init();
  nspire_brightness_apply();
  nspire_brightness_restore();
  CHECK(unit_backlight_writes == 0, "CX: a register reading 0x100 (off) is left alone");
  host_is_cx2 = 1;

  unit_backlight = 120;
  nspire_brightness = 0;
  nspire_brightness_init();
}

static void test_game_list(void)
{
  char *names[] = { "b.gba.tns", "A.gba.tns", "c.gbz.tns", "D.gba.tns" };
  char *dirs[] = { "zelda", "..", "Alpha" };
  char order_path[700];
  FILE *f;

  game_list_sort(names, 4);
  CHECK(!strcmp(names[0], "A.gba.tns") && !strcmp(names[1], "b.gba.tns") && !strcmp(names[2], "c.gbz.tns") &&
        !strcmp(names[3], "D.gba.tns"), "A to Z, ignoring case");
  game_list_sort(dirs, 3);
  CHECK(!strcmp(dirs[0], "..") && !strcmp(dirs[1], "Alpha") && !strcmp(dirs[2], "zelda"), "folders, .. first");

  game_list_move(names, 4, 3, 0);
  CHECK(!strcmp(names[0], "D.gba.tns") && !strcmp(names[1], "A.gba.tns") && !strcmp(names[3], "c.gbz.tns"),
        "moved up");
  game_list_move(names, 4, 0, 2);
  CHECK(!strcmp(names[0], "A.gba.tns") && !strcmp(names[1], "b.gba.tns") && !strcmp(names[2], "D.gba.tns"),
        "moved down");
  game_list_move(names, 4, 4, 0);
  game_list_move(names, 4, 1, 1);
  CHECK(!strcmp(names[1], "b.gba.tns") && !strcmp(names[3], "c.gbz.tns"), "nothing moves out of range");

  nspire_order_file(order_path, sizeof(order_path), dir);
  game_list_forget_order(dir);
  game_list_sort(names, 4);
  CHECK(game_list_apply_order(names, 4, dir) == 0 && !strcmp(names[0], "A.gba.tns"), "no order: A to Z");

  /* D first, then c; a game since deleted is skipped. */
  f = fopen(order_path, "w");
  fprintf(f, "D.gba.tns\nGone.gba.tns\r\nc.gbz.tns\n");
  fclose(f);
  CHECK(game_list_apply_order(names, 4, dir) == 1, "has an order");
  CHECK(!strcmp(names[0], "D.gba.tns") && !strcmp(names[1], "c.gbz.tns") && !strcmp(names[2], "A.gba.tns") &&
        !strcmp(names[3], "b.gba.tns"), "the order, then the rest A to Z: %s %s %s %s", names[0], names[1],
        names[2], names[3]);

  game_list_move(names, 4, 3, 1);
  CHECK(game_list_save_order(names, 4, dir) == 1, "saved");
  game_list_sort(names, 4);
  game_list_apply_order(names, 4, dir);
  CHECK(!strcmp(names[0], "D.gba.tns") && !strcmp(names[1], "b.gba.tns") && !strcmp(names[2], "c.gbz.tns") &&
        !strcmp(names[3], "A.gba.tns"), "the saved order comes back");

  /* A folder name ending in a slash. */
  {
    char slashed[600];
    snprintf(slashed, sizeof(slashed), "%s/", dir);
    game_list_sort(names, 4);
    CHECK(game_list_apply_order(names, 4, slashed) == 1 && !strcmp(names[0], "D.gba.tns"), "dir with a slash");
  }

  /* UI-3: a rename reports a write that fails; the order is written whole
   * (.tmp, then renamed), so a cut write leaves the old order. */
  CHECK(game_list_rename(dir, "A.gba.tns", "A2.gba.tns") == 1, "rename: written");
  {
    char tmp_path[700], before[4096], after[4096];
    struct rlimit lim, none;
    long n;
    FILE *g;

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", order_path);
    g = fopen(order_path, "r");
    n = (long)fread(before, 1, sizeof(before) - 1, g);
    fclose(g);
    before[n] = 0;

    /* The disk fills after 10 bytes (RLIMIT_FSIZE: the write fails short). */
    signal(SIGXFSZ, SIG_IGN);
    getrlimit(RLIMIT_FSIZE, &lim);
    none = lim;
    none.rlim_cur = 10;
    setrlimit(RLIMIT_FSIZE, &none);
    CHECK(game_list_rename(dir, "A2.gba.tns", "A3.gba.tns") == 0, "UI-3: cut write is reported");
    CHECK(game_list_save_order(names, 4, dir) == 0, "UI-3: cut save is reported");
    setrlimit(RLIMIT_FSIZE, &lim);

    g = fopen(order_path, "r");
    n = g ? (long)fread(after, 1, sizeof(after) - 1, g) : -1;
    if (g)
      fclose(g);
    CHECK(n >= 0, "UI-3: old order file still there");
    if (n >= 0)
      after[n] = 0;
    CHECK(n >= 0 && !strcmp(before, after), "UI-3: old order unchanged by the cut write");
    CHECK(file_size(tmp_path) < 0, "UI-3: the cut .tmp is dropped");

    /* A left-over .tmp next to the order (a write that did not finish) is dropped at load. */
    g = fopen(tmp_path, "w");
    fprintf(g, "Junk.gba.tns\nHalf");
    fclose(g);
    game_list_sort(names, 4);
    CHECK(game_list_apply_order(names, 4, dir) == 1 && file_size(tmp_path) < 0, "UI-3: stale .tmp dropped, order kept");
    g = fopen(order_path, "r");
    n = (long)fread(after, 1, sizeof(after) - 1, g);
    fclose(g);
    after[n] = 0;
    CHECK(!strcmp(before, after), "UI-3: order kept with a stale .tmp");

    /* The order gone and a whole .tmp left (reset between remove and rename): taken. */
    remove(order_path);
    g = fopen(tmp_path, "w");
    fprintf(g, "c.gbz.tns\nb.gba.tns\n");
    fclose(g);
    game_list_sort(names, 4);
    CHECK(game_list_apply_order(names, 4, dir) == 1 && !strcmp(names[0], "c.gbz.tns") &&
          !strcmp(names[1], "b.gba.tns") && file_size(tmp_path) < 0 && file_size(order_path) > 0,
          "UI-3: lone whole .tmp becomes the order");

    /* F1: the order gone, a whole .tmp left, and a rename before any list:
     * the rename recovers the .tmp first, so it lands in the order. */
    remove(order_path);
    g = fopen(tmp_path, "w");
    fprintf(g, "c.gbz.tns\nb.gba.tns\n");
    fclose(g);
    CHECK(game_list_rename(dir, "b.gba.tns", "b2.gba.tns") == 1 && file_size(tmp_path) < 0,
          "F1: rename with a lone whole .tmp recovers it");
    g = fopen(order_path, "r");
    n = g ? (long)fread(after, 1, sizeof(after) - 1, g) : -1;
    if (g)
      fclose(g);
    if (n >= 0)
      after[n] = 0;
    CHECK(n >= 0 && !strcmp(after, "c.gbz.tns\nb2.gba.tns\n"), "F1: the rename is in the recovered order");

    /* The order gone and a cut .tmp (no final newline): dropped. */
    remove(order_path);
    g = fopen(tmp_path, "w");
    fprintf(g, "c.gbz.tns\nb.gb");
    fclose(g);
    CHECK(game_list_apply_order(names, 4, dir) == 0 && file_size(tmp_path) < 0 && file_size(order_path) < 0,
          "UI-3: lone cut .tmp dropped");
  }

  game_list_forget_order(dir);
  CHECK(file_size(order_path) < 0, "sorting A to Z forgets it");
}

/* A game just loaded (its code, and the serial_mode gba_over.h gave it)
 * under a Link setting: the serial_mode it plays with. */
static int link_mode_for(const char *code, int game_mode, uint32_t device)
{
  memcpy(gamepak_code, code, 5);
  serial_mode = game_mode;
  nspire_link_device = device;
  nspire_link_apply_device();
  unit_cable_nonce = 1;   /* the next link's: lower than hello_from's */
  return serial_mode;
}

/* The other gpSP's hello (nspire_link.c's): its serial_mode, a higher
 * number than ours (1, host_stubs.c), Emerald. */
static void hello_from(int mode)
{
  static const u8 head[8] = { 1, 0, 16, 0, 0, 0, 0, 0 };
  u8 *body = unit_cable_in + 8;

  memset(unit_cable_in, 0, sizeof(unit_cable_in));
  memcpy(unit_cable_in, head, 8);
  memcpy(body, "GPLK", 4);
  body[4] = 1;
  body[6] = (u8)mode;
  body[8] = 2;
  memcpy(body + 12, "BPEE", 4);
  unit_cable_in_len = 24;
}


static void test_link(void)
{
  static const struct
  {
    const char *code;
    int game;           /* gba_over.h's serial_mode for it */
    uint32_t device;
    int mode;
  } cases[] =
  {
    /* Pokemon: Gen 3's stand-in on the cable, every language; Ruby and
     * Sapphire have no adapter, the others keep theirs on Auto. */
    { "AXVE", SERIAL_MODE_SERIAL_POKE, LINK_DEVICE_AUTO, SERIAL_MODE_SERIAL_POKE },   /* Ruby */
    { "AXVE", SERIAL_MODE_SERIAL_POKE, LINK_DEVICE_WIRELESS, SERIAL_MODE_RFU },
    { "AXVE", SERIAL_MODE_SERIAL_POKE, LINK_DEVICE_EXACT, SERIAL_MODE_CABLE },
    { "AXPJ", SERIAL_MODE_SERIAL_POKE, LINK_DEVICE_AUTO, SERIAL_MODE_SERIAL_POKE },   /* Sapphire */
    { "BPEE", SERIAL_MODE_RFU, LINK_DEVICE_AUTO, SERIAL_MODE_RFU },                   /* Emerald */
    { "BPEE", SERIAL_MODE_RFU, LINK_DEVICE_CABLE, SERIAL_MODE_SERIAL_POKE },
    { "BPEE", SERIAL_MODE_RFU, LINK_DEVICE_EXACT, SERIAL_MODE_CABLE },
    { "BPED", SERIAL_MODE_RFU, LINK_DEVICE_CABLE, SERIAL_MODE_SERIAL_POKE },
    { "BPRF", SERIAL_MODE_RFU, LINK_DEVICE_AUTO, SERIAL_MODE_RFU },                   /* FireRed */
    { "BPRF", SERIAL_MODE_RFU, LINK_DEVICE_CABLE, SERIAL_MODE_SERIAL_POKE },
    { "BPGS", SERIAL_MODE_RFU, LINK_DEVICE_CABLE, SERIAL_MODE_SERIAL_POKE },          /* LeafGreen */
    { "BPGS", SERIAL_MODE_RFU, LINK_DEVICE_EXACT, SERIAL_MODE_CABLE },
    { "BPPE", SERIAL_MODE_AUTO, LINK_DEVICE_AUTO, SERIAL_MODE_CABLE },        /* Pokemon Pinball */
    /* Advance Wars: the table's stand-in on the cable (Auto and Link
     * cable), as wired on Link cable (exact); Advance Wars 2's its own. */
    { "AWRE", SERIAL_MODE_SERIAL_AW1, LINK_DEVICE_AUTO, SERIAL_MODE_SERIAL_AW1 },
    { "AWRE", SERIAL_MODE_SERIAL_AW1, LINK_DEVICE_CABLE, SERIAL_MODE_SERIAL_AW1 },
    { "AWRE", SERIAL_MODE_SERIAL_AW1, LINK_DEVICE_EXACT, SERIAL_MODE_CABLE },
    { "AWRE", SERIAL_MODE_SERIAL_AW1, LINK_DEVICE_WIRELESS, SERIAL_MODE_RFU },
    { "AWRP", SERIAL_MODE_SERIAL_AW1, LINK_DEVICE_AUTO, SERIAL_MODE_SERIAL_AW1 },
    { "AW2E", SERIAL_MODE_SERIAL_AW2, LINK_DEVICE_AUTO, SERIAL_MODE_SERIAL_AW2 },
    { "AW2P", SERIAL_MODE_SERIAL_AW2, LINK_DEVICE_CABLE, SERIAL_MODE_SERIAL_AW2 },
    { "AW2E", SERIAL_MODE_SERIAL_AW2, LINK_DEVICE_EXACT, SERIAL_MODE_CABLE },
    { "GBPX", SERIAL_MODE_GBP, LINK_DEVICE_AUTO, SERIAL_MODE_GBP },
    { "GBPX", SERIAL_MODE_GBP, LINK_DEVICE_CABLE, SERIAL_MODE_CABLE },
    { "BE8E", SERIAL_MODE_AUTO, LINK_DEVICE_AUTO, SERIAL_MODE_SERIAL_FE },  /* Fire Emblem 8 */
    { "BE8E", SERIAL_MODE_AUTO, LINK_DEVICE_CABLE, SERIAL_MODE_SERIAL_FE },
    { "BE8E", SERIAL_MODE_AUTO, LINK_DEVICE_EXACT, SERIAL_MODE_CABLE },
    { "BE8P", SERIAL_MODE_AUTO, LINK_DEVICE_AUTO, SERIAL_MODE_SERIAL_FE },
    /* A stand-in for TEST (as gpsp-host --stand-in TEST:poke): on the
     * cable, Auto's and Link cable's, never exact's or the adapter's. */
    { "TEST", SERIAL_MODE_AUTO, LINK_DEVICE_AUTO, SERIAL_MODE_SERIAL_POKE },
    { "TEST", SERIAL_MODE_AUTO, LINK_DEVICE_CABLE, SERIAL_MODE_SERIAL_POKE },
    { "TEST", SERIAL_MODE_AUTO, LINK_DEVICE_EXACT, SERIAL_MODE_CABLE },
    { "TEST", SERIAL_MODE_RFU, LINK_DEVICE_AUTO, SERIAL_MODE_RFU },
    { "TEST", SERIAL_MODE_RFU, LINK_DEVICE_CABLE, SERIAL_MODE_SERIAL_POKE },
    { "TEST", SERIAL_MODE_AUTO, LINK_DEVICE_WIRELESS, SERIAL_MODE_RFU },
    /* Golden Sun: the MultiSio stand-in; not The Lost Age (not tried) */
    { "AGSE", SERIAL_MODE_AUTO, LINK_DEVICE_AUTO, SERIAL_MODE_SERIAL_MSIO },
    { "AGSJ", SERIAL_MODE_AUTO, LINK_DEVICE_CABLE, SERIAL_MODE_SERIAL_MSIO },
    { "AGSE", SERIAL_MODE_AUTO, LINK_DEVICE_EXACT, SERIAL_MODE_CABLE },
    { "AGFE", SERIAL_MODE_AUTO, LINK_DEVICE_AUTO, SERIAL_MODE_CABLE },
  };
  unsigned i;

  CHECK(nspire_link_try_stand_in("TEST:nope") < 0 && nspire_link_try_stand_in("TES:poke") < 0,
        "stand-ins that don't exist");
  CHECK(nspire_link_try_stand_in("TEST:poke") == 0, "a stand-in to try");
  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    int mode = link_mode_for(cases[i].code, cases[i].game, cases[i].device);
    CHECK(mode == cases[i].mode, "%s, game's mode %d, device %u: mode %d, not %d", cases[i].code,
          cases[i].game, (unsigned)cases[i].device, mode, cases[i].mode);
  }

  unit_cable_on = 0;
  CHECK(nspire_link_try_stand_in("TEST:fe") == 0 &&
        link_mode_for("TEST", SERIAL_MODE_AUTO, LINK_DEVICE_AUTO) == SERIAL_MODE_SERIAL_FE, "fe to try");
  CHECK(nspire_link_try_stand_in("TEST:exact") == 0, "back to no stand-in");

  /* The setting goes round the settings file; older files' 0-2 mean what
   * they did. */
  snprintf(main_path, sizeof(main_path), "%s", dir);
  for (i = 0; i < LINK_DEVICES; i++)
  {
    nspire_link_device = i;
    CHECK(nspire_config_save() == 0, "save");
    nspire_link_device = (i + 1) % LINK_DEVICES;
    CHECK(nspire_config_load() == 0 && nspire_link_device == i, "Link setting %u loads as %u", i,
          (unsigned)nspire_link_device);
  }
  nspire_link_device = LINK_DEVICE_AUTO;
}

/* ---- Linking on its own (nspire_link.c's watch_cable) ---- */

extern int unit_watch_seen, unit_usb_plugged, unit_watch_plugged, unit_cable_opened_automatic;
extern long unit_watch_calls, unit_watch_may_connect, unit_unwatches, unit_watch_fresh;
extern int unit_usb_gated;
extern long unit_gated_calls;
extern u32 unit_ticks;
extern unsigned nspire_os_wanted;

/* The USB gamepad's driver (the usb-gamepad branch's), here: whether it
 * has the port (its USB driver: Direct). Over nspire_link.c's weak one. */
static int unit_pad_owns;
int nspire_usbpad_owns_usb(void) { return unit_pad_owns; }

/* Game frames (their start and end), n times, the time moving a frame
 * each. */
static void link_frames(int n)
{
  while (n-- > 0)
  {
    nspire_link_frame_begin();
    nspire_link_frame_end();
    unit_ticks += NSPIRE_TICK_HZ / 60;
  }
}


/* The USB port observer (nspire_usbport.c): its state from the registers'
 * words, a change counted and called back once, a sample at most every 8
 * frames and none of the OS's time. */
extern long unit_usb_reads;
extern uint32_t unit_usb_bits_set;
extern int unit_reenums, unit_reenum_result;
static int edge_count;
static enum port_state edge_from, edge_to;
static void edge_cb(enum port_state from, enum port_state to) { edge_count++; edge_from = from; edge_to = to; }

static void port_frames(int n)
{
  while (n-- > 0)
  {
    nspire_usbport_poll();
    unit_ticks += NSPIRE_TICK_HZ / 60;
  }
}

/* ---- nspire_usbcycle.c's power cycle on a simulated controller ----
 * (built as nspire_usb_reenumerate_real: the arbiter's tests keep
 * host_stubs.c's stand-in). uc_kind 2: a CX II's FOTG210 (OTGCSR 0x80: bits
 * 16-23 status, read only; 4 A_BUS_REQ, 5 A_BUS_DROP; the rest the OS's
 * control bits; PORTSC 0x30 bit 0 connected), 1: a CX's ChipIdea (PORTSC
 * 0x184: 0 CCS, 1 CSC, 2 PE, 3 PEC, 5 OCC, 6-8 FPR SUSP PR, 12 PP; OTGSC
 * 0x1A4: 5 IDPU, 8 ID, 9 AVV; USBMODE 0x1A8). uc_gate_on_drop: the CX's OS
 * gates the controller at the next OS tick once the port reads empty (its
 * reads then stale, its writes lost: uc_lost). uc_os_bit: the OS's OTG
 * interrupt sets this OTGCSR bit at an OS tick during the drop. */
static int uc_kind, uc_gate_on_drop, uc_lost, uc_writes;
static u32 uc_otgcsr, uc_portsc_f, uc_portsc, uc_otgsc, uc_usbmode, uc_os_bit;
static u32 uc_written[16];
static char uc_log[600];
int nspire_usb_reenumerate_real(int attempt);
int nspire_usb_power_restore_real(void);
int nspire_usb_kind(void) { return unit_usb_gated ? 0 : uc_kind; }
void unit_cycle_log(const char *fmt, ...)
{
  va_list ap;
  size_t n = strlen(uc_log);

  va_start(ap, fmt);
  if (n < sizeof(uc_log) - 2)
    vsnprintf(uc_log + n, sizeof(uc_log) - n - 1, fmt, ap);
  va_end(ap);
  n = strlen(uc_log);
  if (n < sizeof(uc_log) - 1)
    strcat(uc_log, "|");
}
uint32_t host_usb_reg_read(uint32_t addr)
{
  switch (addr - 0xB0000000u)
  {
  case 0x80: return uc_otgcsr;
  case 0x30: return uc_portsc_f;
  case 0x184: return uc_portsc;
  case 0x1A4: return uc_otgsc;
  case 0x1A8: return uc_usbmode;
  }
  return 0;
}
void host_usb_reg_write(uint32_t addr, uint32_t v)
{
  if (uc_writes < 16)
    uc_written[uc_writes] = v;
  uc_writes++;
  if (unit_usb_gated)
  {
    uc_lost++;
    return;
  }
  if (addr == 0xB0000080u)
  {
    uc_otgcsr = (uc_otgcsr & 0x00FF0000u) | (v & 0xFF00FFFFu);
    if (v & 0x20u)
      uc_otgcsr &= ~(1u << 19), uc_portsc_f &= ~1u;          /* VBUS off: the device is gone */
    else if (v & 0x10u)
      uc_otgcsr |= 1u << 19, uc_portsc_f |= 1u;              /* back on */
  }
  else if (addr == 0xB0000184u)
  {
    /* CCS read only, CSC/PEC/OCC write 1 to clear, PE written 0 disables (a 1 can't enable) */
    uc_portsc = (v & ~0x2Fu) | (uc_portsc & 0x2Au & ~v) | (uc_portsc & 0x4u & v) | (uc_portsc & 0x1u);
    if (!(v & (1u << 12)))
      uc_portsc &= ~0x5u, uc_otgsc &= ~(1u << 9);            /* PP off: VBUS off, the device gone */
    else
      uc_portsc |= 0x1u, uc_otgsc |= 1u << 9;
  }
}
extern int unit_timer_dead;
extern long unit_os_sleeps;
extern void (*unit_os_sleep_hook)(void);
static void uc_os_tick(void)
{
  if (unit_os_sleeps > 5000)
    unit_ticks += NSPIRE_TICK_HZ;   /* a wait that never ends: the test ends it (and fails) */
  if (uc_gate_on_drop && uc_kind == 1 && !(uc_portsc & (1u << 12)))
    unit_usb_gated = 1;
  if (uc_os_bit && uc_kind == 2 && (uc_otgcsr & 0x20u))
    uc_otgcsr |= uc_os_bit;           /* the OS's OTG interrupt during the drop */
  if (uc_os_bit && uc_kind == 1 && !(uc_portsc & (1u << 12)))
    uc_portsc &= ~uc_os_bit;          /* the OS clears a PORTSC bit during the drop */
}

static void uc_setup(int kind)
{
  uc_kind = kind;
  uc_gate_on_drop = uc_lost = uc_writes = 0;
  uc_os_bit = 0;
  uc_log[0] = 0;
  unit_usb_gated = unit_timer_dead = 0;
  unit_os_sleeps = 0;
  unit_os_sleep_hook = uc_os_tick;
  uc_otgcsr = 0x000e2e10u;   /* the host's end with VBUS, the OS's bits 9-11 and 13 (the r2/r4 CX II logs) */
  uc_portsc_f = 0x5u;
  uc_portsc = 0x1005u;       /* PP, enabled, connected */
  uc_otgsc = 0x220u;         /* IDPU, AVV, ID 0: the host's end */
  uc_usbmode = 3;
}

/* SEAM-1/4/7/9/11: the cycle itself, on the registers. */
static void test_usb_cycle(void)
{
  int r;

  /* A CX whose timers don't run: no cycle at all (its wait would never end). */
  uc_setup(2);
  unit_timer_dead = 1;
  r = nspire_usb_reenumerate_real(1);
  CHECK(r == -2 && uc_writes == 0 && unit_os_sleeps == 0, "power cycle: refused when the timer doesn't run (%d, %d writes, "
        "%ld OS sleeps)", r, uc_writes, unit_os_sleeps);
  /* Ticks that stand still anyway: the power still goes back, the waits
   * bounded by OS sleeps (300/10 + 4 and 500/10 + 4). */
  uc_setup(2);
  unit_timer_dead = 2;
  r = nspire_usb_reenumerate_real(1);
  CHECK((uc_otgcsr & 0x30u) == 0x10u && unit_os_sleeps <= 34 + 54, "power cycle: frozen ticks: power back (%08x) after "
        "%ld OS sleeps", (unsigned)uc_otgcsr, unit_os_sleeps);
  /* FOTG210: only bits 4 and 5 change, read anew before each write: the
   * OS's control bits and its own rewrite during the drop are kept. */
  uc_setup(2);
  uc_os_bit = 1u << 14;
  r = nspire_usb_reenumerate_real(1);
  CHECK(r == 1 && uc_writes == 2 && uc_written[0] == 0x000e2e20u && (uc_written[1] & 0xFF00FFFFu) == 0x00006e10u &&
        (uc_otgcsr & 0xFFFFu) == 0x6e10u, "power cycle: OTGCSR read-modify-write (%d writes: %08x %08x, now %08x)",
        r, (unsigned)uc_written[0], (unsigned)uc_written[1], (unsigned)uc_otgcsr);
  CHECK(strstr(uc_log, "reenum: cycle 1 (fotg)") && strstr(uc_log, "before OTGCSR 000e2e10 PORTSC 00000005") &&
        strstr(uc_log, "in the drop OTGCSR 00066e20 PORTSC 00000004 (VBUS off, device gone)") &&
        strstr(uc_log, "vbus_valid=1"), "power cycle: the log has the registers before, in the drop and after: %s", uc_log);
  /* ChipIdea: the restore reads PORTSC anew (a bit the OS changed in the
   * drop isn't put back), W1C bits are written 0, PE 0. */
  uc_setup(1);
  uc_portsc = 0x10AFu;       /* PP, SUSP, OCC, PEC, PE, CSC, CCS */
  uc_os_bit = 1u << 7;       /* the OS ends the suspend in the drop */
  r = nspire_usb_reenumerate_real(2);
  CHECK(r == 1 && uc_writes == 2 && (uc_written[0] & 0x102Eu) == 0 && (uc_written[1] & 0x10AEu) == 0x1000u &&
        (uc_portsc & 0x1080u) == 0x1000u, "power cycle: PORTSC: W1C and PE written 0, PP 0 then 1, read anew (%08x %08x, "
        "now %08x)", (unsigned)uc_written[0], (unsigned)uc_written[1], (unsigned)uc_portsc);
  CHECK(strstr(uc_log, "reenum: cycle 2 (ci)") && strstr(uc_log, "(VBUS off, device gone)"), "power cycle: CX log: %s", uc_log);
  /* Refused: not the host mode, or no ID pull-up (ID reads 0 then). */
  uc_setup(1);
  uc_usbmode = 2;
  r = nspire_usb_reenumerate_real(1);
  CHECK(r < 0 && uc_writes == 0, "power cycle: refused with USBMODE device (%d, %d writes)", r, uc_writes);
  uc_setup(1);
  uc_otgsc = 0x200u;
  r = nspire_usb_reenumerate_real(1);
  CHECK(r < 0 && uc_writes == 0, "power cycle: refused without the ID pull-up (%d, %d writes)", r, uc_writes);
  /* A CX gated off in the drop: no write into it, no stale read taken
   * for VBUS; 0 (no second cycle), said loudly; PP back at the first look
   * after the gate opens, if the ID pin still says the host's end. */
  uc_setup(1);
  uc_gate_on_drop = 1;
  r = nspire_usb_reenumerate_real(1);
  CHECK(r == 0 && uc_lost == 0 && uc_writes == 1 && strstr(uc_log, "gated off in the drop"),
        "power cycle: gated in the drop: no write lost (%d, %d lost, %d writes): %s", r, uc_lost, uc_writes, uc_log);
  CHECK(nspire_usb_power_restore_real() == 0 && uc_writes == 1, "power cycle: still gated: nothing written (%d)", uc_writes);
  unit_usb_gated = 0;
  uc_otgsc |= 0x200u;
  CHECK(nspire_usb_power_restore_real() == 1 && uc_writes == 2 && (uc_portsc & 0x1000u), "power cycle: the gate opens: PP back "
        "(%d writes, %08x)", uc_writes, (unsigned)uc_portsc);
  CHECK(nspire_usb_power_restore_real() == 0 && uc_writes == 2, "power cycle: restored once");
  /* ...but not when the cable's end changed meanwhile (the device's end). */
  uc_setup(1);
  uc_gate_on_drop = 1;
  nspire_usb_reenumerate_real(1);
  unit_usb_gated = 0;
  uc_otgsc |= 0x100u;
  CHECK(nspire_usb_power_restore_real() == 0 && uc_writes == 1, "power cycle: the ID pin says the device's end: PP left (%d)",
        uc_writes);
  CHECK(nspire_usb_power_restore_real() == 0, "power cycle: and forgotten");
  unit_os_sleep_hook = 0;
  unit_usb_gated = 0;
  uc_kind = 0;
}

static void test_usbport(void)
{
  long reads;
  uint32_t samples;

  nspire_usbport_reset();
  unit_usb_plugged = 0;
  unit_usb_bits_set = 0;
  unit_usb_gated = 0;
  CHECK(nspire_usbport_on_edge(edge_cb) == 0, "port: edge callback registered");
  reads = unit_usb_reads;
  port_frames(600);
  CHECK(nspire_usbport_state() == PORT_EMPTY && nspire_usbport_changed_counter() == 0 && edge_count == 0,
        "port: 600 frames EMPTY: state %d, %u changes, %d edges", nspire_usbport_state(),
        (unsigned)nspire_usbport_changed_counter(), edge_count);
  CHECK(unit_usb_reads - reads <= 600 / 8 && unit_usb_reads - reads >= 600 / 8 - 1 && nspire_usbport_samples() <= 600 / 8,
        "port: EMPTY reads the port at most once in 8 frames: %ld in 600", unit_usb_reads - reads);
  CHECK(nspire_os_wanted == 0, "port: the observer asks for no OS time (%u)", nspire_os_wanted);
  /* S8: 3000 frames in each state: one look in 8 frames at most, 122 ms
   * apart at least, the longest wait for a look 8 frames (133 ms at 60 a
   * second): a plug-in is seen within a look, far inside the 3 s / 5 s
   * targets, so the period stays 8 frames. No OS time in any of them. */
  {
    static const struct { int plugged, gated; uint32_t bits; const char *name; } st[] = {
      {0, 0, 0, "EMPTY"}, {1, 0, 0x20000001u, "CX II host end"}, {1, 0, 0x20000020u, "CX II device end"},
      {1, 0, 0x10000000u, "CX host end"}, {-1, 1, 0, "CX gated"}};
    unsigned k;

    for (k = 0; k < sizeof(st) / sizeof(st[0]); k++)
    {
      nspire_usbport_reset();
      unit_usb_plugged = st[k].plugged;
      unit_usb_gated = st[k].gated;
      unit_usb_bits_set = st[k].bits;
      reads = unit_usb_reads;
      port_frames(3000);
      CHECK(nspire_usbport_samples() >= 3000 / 8 - 1 && nspire_usbport_samples() <= 3000 / 8 &&
            unit_usb_reads - reads == (long)nspire_usbport_samples() && nspire_os_wanted == 0 &&
            nspire_usbport_max_gap_ms() >= 122 && nspire_usbport_max_gap_ms() <= 8 * 1000 / 60 + 6,
            "port S8: %s, 3000 frames: %u looks (%ld reads), longest gap %u ms, OS wanted %u", st[k].name,
            (unsigned)nspire_usbport_samples(), unit_usb_reads - reads, (unsigned)nspire_usbport_max_gap_ms(),
            nspire_os_wanted);
    }
    nspire_usbport_reset();
    unit_usb_plugged = 0;
    unit_usb_gated = 0;
    unit_usb_bits_set = 0;
    nspire_usbport_on_edge(edge_cb);   /* the reset dropped it */
  }

  /* A CX II's pad: ID bit (bit 5) clear = this calculator is the host. */
  unit_usb_plugged = 1;
  unit_usb_bits_set = 0x20000001u;
  port_frames(8);
  CHECK(nspire_usbport_state() == PORT_HOST_END && nspire_usbport_changed_counter() == 1 && edge_count == 1 &&
        edge_from == PORT_EMPTY && edge_to == PORT_HOST_END && nspire_usbport_bits() == 0x20000001u,
        "port: CX II adapter end: state %d, %u changes", nspire_usbport_state(), (unsigned)nspire_usbport_changed_counter());
  port_frames(120);
  CHECK(nspire_usbport_changed_counter() == 1 && nspire_usbport_age_ms() >= 1900, "port: steady: 1 change, age %u ms",
        (unsigned)nspire_usbport_age_ms());
  /* Session bits alone: a change, no edge. */
  unit_usb_bits_set = 0x20000003u;
  port_frames(8);
  CHECK(nspire_usbport_changed_counter() == 2 && edge_count == 1 && nspire_usbport_age_ms() < 300,
        "port: session bits: change counted (%u), no edge (%d)", (unsigned)nspire_usbport_changed_counter(), edge_count);
  nspire_usbport_os_report(PORT_OS_OFFER, 3, 0x2563);
  CHECK(nspire_usbport_os() == PORT_OS_OFFER && nspire_usbport_os_class() == 3 && nspire_usbport_os_vid() == 0x2563,
        "port: OS report kept");
  unit_usb_bits_set = 0x20000020u;   /* the cable's end */
  port_frames(8);
  CHECK(nspire_usbport_state() == PORT_DEVICE_END && edge_count == 2 && nspire_usbport_os() == PORT_OS_NONE,
        "port: CX II cable end, OS report forgotten: state %d", nspire_usbport_state());
  /* A CX's OTGSC: bit 0 the device's end. */
  unit_usb_bits_set = 0x10000000u;
  port_frames(8);
  CHECK(nspire_usbport_state() == PORT_HOST_END, "port: CX adapter end");
  unit_usb_bits_set = 0x10000001u;
  port_frames(8);
  CHECK(nspire_usbport_state() == PORT_DEVICE_END, "port: CX cable end");
  /* Gated or stale: can't tell. */
  unit_usb_plugged = -1;
  unit_usb_gated = 1;
  port_frames(8);
  CHECK(nspire_usbport_state() == PORT_UNKNOWN, "port: gated: can't tell");
  unit_usb_gated = 0;
  unit_usb_plugged = 0;
  unit_usb_bits_set = 0;
  samples = nspire_usbport_samples();
  port_frames(8);
  CHECK(nspire_usbport_state() == PORT_EMPTY && nspire_usbport_samples() == samples + 1, "port: unplugged: EMPTY");
  nspire_usbport_reset();
}

/* The pad driver's claim, as the arbiter test sets it (a strong definition
 * over the arbiter's weak default; frontend_tests has no pad glue). */
static int unit_pad_claim;
int nspire_usbpad_active(void) { return unit_pad_claim; }
static int unit_audio_claim, unit_audio_owns;
/* nspire_audio_os.c's count of a pad's audio interfaces left alone (the pad log reads it). */
static unsigned unit_audio_declined;
unsigned nspire_audio_declined(uint32_t *ids, unsigned *ifno)
{
  if (ids)
    *ids = 0x054c0ce6u;
  if (ifno)
    *ifno = 2;
  return unit_audio_declined;
}
int nspire_usbaudio_active(void) { return unit_audio_claim; }
int nspire_usbaudio_owns_usb(void) { return unit_audio_owns; }
static int unit_drive_claim;
int nspire_usbms_active(void) { return unit_drive_claim; }
static int hook_calls[5];
static void hook1(void) { hook_calls[1]++; }
static void hook3(void) { hook_calls[3]++; }

extern int unit_osd_count, unit_reenum_to_zero;
extern char unit_osd_message[64], unit_osd_log[2048];
/* DETECT: the come-up's message for an adapter's end nobody claims (from
 * 1 s after the port change, nspire_link.c BRING_HOST_QUIET_MS). */
#define COMEUP_DEVICE "USB device in: connecting..."
#define COMEUP_NODE   "Calculator found: connecting..."
extern unsigned unit_looks, unit_look_nodes, unit_looks_now, unit_last_nodes;
static int unit_pad_offered, unit_offered_calls;
int nspire_usbpad_offered(void) { unit_offered_calls++; return unit_pad_offered; }
extern int unit_restore_pending;

/* A game with a link (the watch runs), something at the port with these
 * bits (0: nothing), nobody claiming it, the cycle's stand-in saying 1. */
static void arb_setup(u32 bits)
{
  link_mode_for("BPEE", SERIAL_MODE_RFU, LINK_DEVICE_AUTO);
  unit_usb_plugged = 0;   /* the last test's port goes (the link's timeline forgets its node) */
  unit_usb_bits_set = 0;
  unit_watch_seen = 0;
  unit_look_nodes = unit_last_nodes = 0;
  link_frames(2);
  nspire_usbport_reset();
  nspire_arb_reset();
  nspire_padstatus_reset();   /* R11w part 2: a new plug's one message that asks (nspire_padstatus_may_ask) */
  unit_pad_claim = unit_pad_offered = unit_offered_calls = 0;
  unit_audio_claim = unit_audio_owns = 0;
  unit_drive_claim = 0;
  unit_usb_gated = unit_restore_pending = 0;
  unit_watch_seen = 0;
  unit_looks = unit_look_nodes = unit_looks_now = unit_last_nodes = 0;
  unit_usb_plugged = bits ? 1 : 0;
  unit_usb_bits_set = bits;
  unit_reenums = 0;
  unit_reenum_result = 1;
  unit_reenum_to_zero = 0;
  unit_osd_message[0] = 0;
  unit_osd_count = 0;
  unit_osd_log[0] = 0;
}

/* Game frames: the link's frame start (the watch, then the arbiter's frame). */
static void arb_frames(int n)
{
  link_frames(n);
}

/* The keys read outside a game (menus, the file list): bookkeeping only. */
static void arb_keys(int n)
{
  while (n-- > 0)
  {
    unit_ticks += NSPIRE_TICK_HZ / 60;
    nspire_usbport_poll();
    nspire_arb_tick();
  }
}

#define HOST_END_PORT 0x20000500u   /* a CX II's host end: connected (0x100), enabled (0x400) */

/* DETECT: a cable plugged in during play at the cable's end (a CX II's
 * device end): "Link cable in: connecting..." in the first frame, kept on
 * screen every frame (no frame without a message), "Calculator found" when
 * a look lists the node, and when no gpSP answers there for 10 s the node
 * hint, the come-up's last word: nothing after it. The adapter's end:
 * nothing for a pad the OS offers at once (it says "Controller connected"
 * itself), the message after 1 s for a device nobody claims. */
/* The port empty past the flap detector's gap (FLAP_GAP_MS 8 s: a test's
 * plug-ins in a row would be "USB port keeps resetting"). */
static void comeup_quiet(void)
{
  unit_usb_plugged = 0;
  unit_usb_bits_set = 0;
  arb_frames(60 * 9);
}

static void test_comeup_messages(void)
{
  int f, dark = 0, shown_at = -1;

  comeup_quiet();
  arb_setup(0x20000020u);   /* the cable's end, from the first frame */
  for (f = 0; f < 60 * 3; f++)
  {
    arb_frames(1);
    if (shown_at < 0 && nspire_osd_visible())
      shown_at = f;
    if (shown_at >= 0 && !nspire_osd_visible())
      dark++;
  }
  /* R11w part 2: this checked "Link cable in: connecting..." at once. At
   * the cable's plain end with Link not picked nothing is said until the
   * other calculator's node is listed: a computer's cable (charging) is
   * that end too and got "Link cable in" and then "No answer on the link
   * cable" (no message when it isn't its case). */
  CHECK(shown_at < 0 && !unit_osd_log[0], "come-up: the cable's end, no node yet: nothing said (frame %d): %s", shown_at,
        unit_osd_log);
  unit_watch_seen = 1;   /* a node listed (no gpSP service there) */
  for (f = 0; f < 60 * 9; f++)
  {
    arb_frames(1);
    if (shown_at < 0 && nspire_osd_visible())
      shown_at = f;
    else if (shown_at >= 0)
      dark += !nspire_osd_visible();
  }
  CHECK(dark == 0 && !strcmp(unit_osd_log, COMEUP_NODE "|"),
        "come-up: the node's message, kept on screen every frame (%d frames without): %s", dark, unit_osd_log);
  arb_frames(60 * 2);
  CHECK(!strcmp(unit_osd_log, COMEUP_NODE "|Other calculator found: start gpSP there|"),
        "come-up: the node hint at 10 s ends it: %s", unit_osd_log);
  arb_frames(60 * 20);
  CHECK(!strcmp(unit_osd_log, COMEUP_NODE "|Other calculator found: start gpSP there|") && !nspire_osd_visible(),
        "come-up: nothing after the last word: %s", unit_osd_log);
  unit_watch_seen = 0;
  /* the adapter's end: a pad that claims at once: no come-up message */
  comeup_quiet();
  arb_setup(HOST_END_PORT);
  unit_pad_claim = 1;
  arb_frames(60 * 3);
  CHECK(unit_osd_count == 0, "come-up: a pad the OS offers: its own toast only (%s)", unit_osd_log);
  unit_pad_claim = 0;
  /* a device nobody claims: the message after 1 s, kept */
  comeup_quiet();
  arb_setup(HOST_END_PORT);
  arb_frames(55);
  CHECK(unit_osd_count == 0, "come-up: the adapter's end: nothing in the first 0.9 s (%s)", unit_osd_log);
  arb_frames(10);
  CHECK(!strcmp(unit_osd_log, COMEUP_DEVICE "|"), "come-up: the adapter's end: the message at 1 s (%s)", unit_osd_log);
  for (f = 0, dark = 0; f < 60 * 4; f++)
  {
    arb_frames(1);
    dark += !nspire_osd_visible();
  }
  CHECK(dark == 0, "come-up: the adapter's end: kept up until the arbiter's own (%d frames without; %s)", dark, unit_osd_log);
  unit_usb_plugged = 0;
  unit_usb_bits_set = 0;
  arb_frames(60 * 3);
  CHECK(!nspire_osd_visible(), "come-up: the port empty: the message goes (%s)", unit_osd_log);
}

/* LINK2 (r5, r6: the CX II at the host's end, Link picked, its controller
 * stopped 29-38 s with a device connected, PORTSC 803): one bus power cycle
 * once the pick is 1.5 s and the port's change 2.5 s old, then, if the
 * controller still hasn't run 3 s later, "Unplug the link cable and plug it
 * in again" on screen and as the status (not "Looking..." for 30 s); none
 * of it when the controller runs; the hint at once when no cycle can be
 * made; a replug starts over. */
extern int unit_cable_wait, unit_usb_stopped;
#define STUCK_PORT 0x20000100u   /* a CX II's host end: connected (0x100), not enabled */
#define STUCK_TEXT "Unplug the link cable and plug it in again"
static void test_stuck_host(void)
{
  int f, cycled_at = -1, told_at = -1;

  comeup_quiet();
  arb_setup(STUCK_PORT);
  unit_usb_stopped = 1;
  unit_cable_on = unit_cable_wait = 1;
  arb_frames(30);
  nspire_link_connect();
  CHECK(nspire_link_state() == LINK_LOOKING, "stuck host: Link picked, looking (%d)", nspire_link_state());
  for (f = 0; f < 60 * 8; f++)
  {
    arb_frames(1);
    if (cycled_at < 0 && unit_reenums)
      cycled_at = f;
    if (told_at < 0 && strstr(unit_osd_log, STUCK_TEXT))
      told_at = f;
  }
  CHECK(cycled_at >= 115 && cycled_at <= 125 && unit_reenums == 1,
        "stuck host: one power cycle 2.5 s after the port's change (the pick 2 s after it): frame %d, %d cycles", cycled_at,
        unit_reenums);
  CHECK(told_at >= cycled_at + 175 && told_at <= cycled_at + 185 && !strcmp(nspire_link_status(), STUCK_TEXT) &&
        nspire_link_state() == LINK_LOOKING,
        "stuck host: the replug hint 3 s after it, as the status, still looking: frame %d, \"%s\"", told_at,
        nspire_link_status());
  CHECK(nspire_osd_visible() && !strcmp(nspire_osd_visible(), STUCK_TEXT), "stuck host: the hint kept on screen: %s",
        nspire_osd_visible() ? nspire_osd_visible() : "(nothing)");
  /* The replug: the port's change, its controller runs and enables it: nothing more. */
  unit_usb_bits_set = 0x20000000u;
  arb_frames(20);
  unit_usb_stopped = 0;
  unit_usb_bits_set = 0x20000500u;
  arb_frames(60 * 6);
  CHECK(unit_reenums == 1, "stuck host: after a replug that works, no more cycles (%d)", unit_reenums);
  nspire_link_disconnect();

  /* The controller runs: no cycle, no hint. */
  comeup_quiet();
  arb_setup(STUCK_PORT);
  unit_cable_on = unit_cable_wait = 1;
  arb_frames(30);
  nspire_link_connect();
  unit_osd_log[0] = 0;
  arb_frames(60 * 8);
  CHECK(unit_reenums == 0 && !strstr(unit_osd_log, STUCK_TEXT), "stuck host: a running controller: no cycle (%d), no hint (%s)",
        unit_reenums, unit_osd_log);
  nspire_link_disconnect();

  /* No cycle can be made (the device's end says -1: nothing to cycle): the hint at once. */
  comeup_quiet();
  arb_setup(STUCK_PORT);
  unit_usb_stopped = 1;
  unit_reenum_result = -1;
  unit_cable_on = unit_cable_wait = 1;
  arb_frames(30);
  nspire_link_connect();
  told_at = -1;
  for (f = 0; f < 60 * 4; f++)
  {
    arb_frames(1);
    if (told_at < 0 && strstr(unit_osd_log, STUCK_TEXT))
      told_at = f;
  }
  CHECK(told_at >= 115 && told_at <= 125 && unit_reenums == 1, "stuck host: no cycle possible: the hint at once (frame %d, %d asked)",
        told_at, unit_reenums);
  /* The link's -1 latches nothing: the stages' own cycle is
   * still there for what the port holds next this session. */
  nspire_link_disconnect();
  unit_cable_on = 0;
  unit_reenum_result = 1;
  arb_frames(60 * 10);
  CHECK(unit_reenums == 2, "stuck host: after the link's -1 the stages may still cycle (%d asked)", unit_reenums);
  unit_usb_stopped = unit_cable_wait = 0;
  comeup_quiet();

  /* The cycle works (the CX II's OS starts its controller on
   * the new connect, ~170 ms enumeration, the node 1.6 s after: r6) and the
   * other's hello comes 2 s after the cycle, inside ARB_CYCLE_QUIET_MS:
   * the fast transport is not refused for the link's own cycle. */
  {
    extern int unit_go_fasts;
    extern const char *unit_go_fast_refused;
    const char *before;

    arb_setup(STUCK_PORT);
    unit_usb_stopped = 1;
    unit_cable_on = unit_cable_wait = 1;
    arb_frames(30);
    nspire_link_connect();
    for (f = 0; f < 60 * 3 && !unit_reenums; f++)
      arb_frames(1);
    CHECK(unit_reenums == 1, "stuck host, the cycle works: one cycle (%d)", unit_reenums);
    unit_usb_bits_set = 0x20000000u;   /* the device's end let go (VBUS off) */
    arb_frames(12);
    unit_usb_bits_set = 0x20000100u;   /* back, not yet enabled */
    arb_frames(12);
    before = nspire_arb_fast_refusal();
    unit_usb_stopped = 0;              /* the OS started its controller, enabled the port */
    unit_usb_bits_set = HOST_END_PORT;
    arb_frames(60 + 36);               /* 2 s after the cycle */
    CHECK(nspire_arb_cycling() && before && strstr(before, "power cycle") && !nspire_arb_fast_refusal(),
          "stuck host, the cycle works: still in its quiet window (%d), the fast transport refused before the port's "
          "enabled (%s), not after (%s)", nspire_arb_cycling(), before ? before : "none",
          nspire_arb_fast_refusal() ? nspire_arb_fast_refusal() : "none");
    unit_go_fasts = 0;
    unit_go_fast_refused = "(not asked)";
    unit_watch_seen = 1;
    unit_cable_wait = 0;
    hello_from(SERIAL_MODE_RFU);
    arb_frames(1);
    CHECK(nspire_link_state() == LINK_LINKED && nspire_arb_cycling() && unit_go_fasts == 1 && !unit_go_fast_refused,
          "stuck host, the cycle works: linked %s inside the quiet window (%d), the fast transport tried %d time(s), "
          "refused: %s", nspire_link_status(), nspire_arb_cycling(), unit_go_fasts,
          unit_go_fast_refused ? unit_go_fast_refused : "no");
    nspire_link_disconnect();
    /* R10a (PAD3 x LINK2): only the link's own cycle ends its quiet window
     * early. The cable out, a restart by hand on the Controller page (the
     * menus), the cable back and Link picked within ARB_CYCLE_QUIET_MS: the
     * port enabled with the controller running, the fast transport is still
     * refused (the session's earlier link cycle doesn't count for it). */
    {
      int r, n = unit_reenums;
      const char *why;

      unit_watch_seen = 0;
      unit_cable_on = 0;
      arb_frames(60);
      arb_keys(30);
      r = nspire_arb_manual_cycle();
      unit_cable_on = 1;
      nspire_link_connect();
      for (f = 0; f < 60 && nspire_arb_owner() != ARB_LINK; f++)
        arb_frames(1);
      why = nspire_arb_fast_refusal();
      CHECK(r == 1 && unit_reenums == n + 1 && nspire_arb_owner() == ARB_LINK && nspire_arb_cycling() && why &&
            strstr(why, "power cycle"),
            "stuck host, a restart by hand after the link's cycle: made (%d, %d cycles), LINK (%s), in its quiet window (%d): "
            "the fast transport refused (%s)", r, unit_reenums - n, nspire_arb_owner_name(nspire_arb_owner()),
            nspire_arb_cycling(), why ? why : "none");
      nspire_link_disconnect();
    }
    unit_watch_seen = 0;
    unit_usb_stopped = unit_cable_wait = 0;
    unit_reenum_result = 1;
    comeup_quiet();
  }
}

/* S4 with its guards: the first cycle once the link's watch has
 * looked twice since the port changed, the last look 2.5 s after it, and a
 * fresh look found no node (SEAM-2): about 3 s; the second at the port's
 * age 6 s and 4.5 s after the first; at most 2; none with a claimed port; a
 * refusal ends them. */
static void test_arbiter_cycles(void)
{
  arb_setup(HOST_END_PORT);
  arb_frames(175);
  CHECK(unit_reenums == 0, "arbiter cycles: none in the first 2.9 s: the watch hasn't looked long enough (%d)", unit_reenums);
  arb_frames(15);
  CHECK(unit_reenums == 1 && nspire_arb_cycles() == 1 && unit_looks_now == 1, "arbiter cycles: one at ~3 s, after a fresh look "
        "(%d, %u fresh looks)", unit_reenums, unit_looks_now);
  arb_frames(250);
  CHECK(unit_reenums == 1, "arbiter cycles: still one at ~7.3 s (%d)", unit_reenums);
  CHECK(!strcmp(unit_osd_message, "Starting the USB device..."), "arbiter cycles: 'Starting' at 6 s: %s", unit_osd_message);
  arb_frames(30);
  CHECK(unit_reenums == 2 && nspire_arb_cycles() == 2 && unit_looks_now == 2, "arbiter cycles: the second at ~7.5 s (%d, %u)",
        unit_reenums, unit_looks_now);
  arb_frames(60 * 20);
  CHECK(unit_reenums == 2 && nspire_arb_stage() == ARB_STAGE_GAVE_UP && unit_osd_count == 3 &&
        !strcmp(unit_osd_log, COMEUP_DEVICE "|Starting the USB device...|" ARB_REPLUG_TEXT "|"),
        "arbiter cycles: no third; the come-up message, 'Starting' kept up, the last resort once (%d, stage %u, %d toasts, %s)",
        unit_reenums, nspire_arb_stage(), unit_osd_count, unit_osd_log);   /* (R11w part 2: its words plainer) */
  /* a pad claims: no cycle ever */
  arb_setup(HOST_END_PORT);
  unit_pad_claim = 1;
  arb_frames(60 * 10);
  CHECK(unit_reenums == 0, "arbiter cycles: a pad up, none (%d)", unit_reenums);
  /* nothing to cycle (-1): one try, no second. R11w part 2: this checked
   * the cable hint ("Swap the cable: adapter to the other calculator"):
   * with nothing on the wires that is what a controller pulled out of an
   * adapter that stayed in got (R11w pad log 245420-252289: a message
   * to switch the cable ends). Nothing on the
   * wires and Link not picked: nothing is said; while linking: the swap. */
  arb_setup(0x20000000u);
  unit_reenum_result = -1;
  arb_frames(60 * 12);
  CHECK(unit_reenums == 1 && !unit_osd_message[0], "arbiter cycles: refused once, not again, nothing said (%d, %s)",
        unit_reenums, unit_osd_message);
  /* can't here (-2: a dead timer...): no second, the replug text */
  arb_setup(HOST_END_PORT);
  unit_reenum_result = -2;
  arb_frames(60 * 12);
  CHECK(unit_reenums == 1 && strstr(unit_osd_log, ARB_REPLUG_TEXT),
        "arbiter cycles: can't (-2): once, the replug text (%d, %s)", unit_reenums, unit_osd_log);
  /* VBUS not back (0): no second cycle */
  arb_setup(HOST_END_PORT);
  unit_reenum_result = 0;
  arb_frames(60 * 12);
  CHECK(unit_reenums == 1, "arbiter cycles: VBUS not back: no second (%d)", unit_reenums);

  /* SEAM-2: a calculator whose node shows at 1.6 s (after a look at 1.5 s
   * would have missed it): the next look sees it, no cycle, the link's. */
  arb_setup(HOST_END_PORT);
  arb_frames(96);
  unit_watch_seen = 1;
  arb_frames(60 * 10);
  CHECK(unit_reenums == 0 && nspire_arb_owner() == ARB_LINK, "arbiter: a node at 1.6 s: no cycle (%d), LINK (%s)", unit_reenums,
        nspire_arb_owner_name(nspire_arb_owner()));
  /* ...and one only the look right before the cycle finds. */
  arb_setup(HOST_END_PORT);
  unit_look_nodes = 1;
  arb_frames(60 * 10);
  CHECK(unit_reenums == 0 && unit_looks_now == 1 && nspire_arb_owner() == ARB_LINK &&
        !strcmp(unit_osd_log, COMEUP_DEVICE "|" COMEUP_NODE "|"),
        "arbiter: the fresh look lists a node: no cycle (%d, %u looks), LINK (%s), the come-up's messages only (%s)", unit_reenums,
        unit_looks_now, nspire_arb_owner_name(nspire_arb_owner()), unit_osd_log);

  /* R9: the Controller page's restart by hand (in the menus) asks a
   * fresh NavNet look first: never under the other calculator. */
  {
    int r;

    arb_setup(HOST_END_PORT);
    unit_look_nodes = 1;
    arb_keys(30);
    r = nspire_arb_manual_cycle();
    CHECK(r == -4 && unit_reenums == 0 && unit_looks_now == 1, "arbiter: by hand, a node listed: refused (%d, %d cycles, %u looks)", r,
          unit_reenums, unit_looks_now);
    arb_setup(HOST_END_PORT);
    arb_keys(30);
    r = nspire_arb_manual_cycle();
    CHECK(r == 1 && unit_reenums == 1, "arbiter: by hand, no node: the restart (%d, %d cycles)", r, unit_reenums);
    arb_setup(HOST_END_PORT);
  }

  /* SEAM-3/12: in the menus and the file list (the keys' bookkeeping only)
   * nothing is cycled however long; in the game that follows, only after
   * the watch's looks. */
  arb_setup(HOST_END_PORT);
  arb_keys(60 * 20);
  CHECK(unit_reenums == 0 && nspire_arb_stage() == ARB_STAGE_WAIT_OFFER && unit_osd_count == 0,
        "arbiter: 20 s in the menus: no cycle, no stage, no toast (%d, %u)", unit_reenums, nspire_arb_stage());
  arb_frames(60);
  CHECK(unit_reenums == 0, "arbiter: the game's first second: no cycle yet (%d)", unit_reenums);
  arb_frames(60);
  CHECK(unit_reenums == 1, "arbiter: two looks in the game: the cycle (%d)", unit_reenums);
  /* a game without a link (the watch off): never */
  arb_setup(HOST_END_PORT);
  link_mode_for("GBPX", SERIAL_MODE_GBP, LINK_DEVICE_AUTO);
  arb_frames(60 * 20);
  CHECK(unit_reenums == 0 && unit_osd_count == 0, "arbiter: the watch off (no link in the game): no cycle, no toast (%d)",
        unit_reenums);

  /* SEAM-5: the pad leaves an adapter that stays in: its detach first
   * (WAIT), the port's bits a moment later: no toast. */
  arb_setup(HOST_END_PORT);
  unit_pad_claim = 1;
  arb_frames(60);
  unit_pad_claim = 0;
  arb_frames(3);
  unit_usb_bits_set = 0x20000000u;
  unit_reenum_result = -1;
  arb_frames(60 * 15);
  CHECK(unit_osd_count == 0, "arbiter: a pad out of an adapter that stays: no toast (%s)", unit_osd_message);
  CHECK(nspire_arb_owner() == ARB_WAIT, "arbiter: and nobody owns the port (%s)", nspire_arb_owner_name(nspire_arb_owner()));

  /* SEAM-6: the port reads UNKNOWN for a moment after each cycle (a CX's
   * gate) and comes back with new bits: still 2 cycles at most, each toast
   * once. A real unplug (EMPTY 1 s) gives the next session its own. */
  arb_setup(HOST_END_PORT);
  {
    int i;
    for (i = 0; i < 6; i++)
    {
      int before = unit_reenums, k;

      for (k = 0; k < 600 && unit_reenums == before; k++)
        arb_frames(1);
      unit_usb_plugged = -1;   /* gated: the bits read 0 */
      unit_usb_gated = 1;
      arb_frames(30);
      unit_usb_gated = 0;
      unit_usb_plugged = 1;
      unit_usb_bits_set = i & 1 ? HOST_END_PORT : 0x20000500u | 0x1u;
      arb_frames(30);
    }
  }
  CHECK(unit_reenums == 2 && nspire_arb_cycles() == 2 && unit_osd_count <= 3 && !strstr(unit_osd_log, "resetting"),
        "arbiter: a gate after each cycle: 2 cycles a session (%d), toasts %d (%s: not the flap toast for our own cycles)",
        unit_reenums, unit_osd_count, unit_osd_log);
  unit_usb_plugged = 0;
  unit_usb_bits_set = 0;
  arb_frames(60);
  unit_usb_plugged = 1;
  unit_usb_bits_set = HOST_END_PORT;
  arb_frames(60 * 12);
  CHECK(unit_reenums == 4, "arbiter: after a real unplug, the new session's 2 (%d)", unit_reenums);
  /* and each next one */
  {
    int i;
    for (i = 0; i < 3; i++)
    {
      unit_usb_plugged = 0;
      unit_usb_bits_set = 0;
      arb_frames(60);
      unit_usb_plugged = 1;
      unit_usb_bits_set = HOST_END_PORT;
      arb_frames(60 * 12);
    }
  }
  CHECK(unit_reenums == 10, "arbiter: each real unplug, a new session's 2 (%d)", unit_reenums);

  /* SEAM-8: the OS offered the device (a match, an attach under way): no
   * cycle; nor with the OS side's report of an offer. */
  arb_setup(HOST_END_PORT);
  unit_pad_offered = 1;
  arb_frames(60 * 12);
  CHECK(unit_reenums == 0 && !strcmp(unit_osd_log, COMEUP_DEVICE "|"),
        "arbiter: offered to the pad driver: no cycle, no toast but the come-up's (%d, %s)", unit_reenums, unit_osd_log);
  /* SEAM-15: asked once, then latched (not a diag text each frame) */
  CHECK(unit_offered_calls == 1, "arbiter: the pad driver's offer asked once in 12 s (%d)", unit_offered_calls);
  /* ...until the port goes through EMPTY: the next device is asked again */
  unit_pad_offered = 0;
  unit_usb_plugged = 0;
  unit_usb_bits_set = 0;
  arb_frames(60);
  unit_usb_plugged = 1;
  unit_usb_bits_set = HOST_END_PORT;
  arb_frames(60 * 12);
  CHECK(unit_offered_calls >= 2 && unit_reenums == 2, "arbiter: replugged: asked again (%d), cycled (%d)", unit_offered_calls,
        unit_reenums);

  /* SEAM-14: a CX gates its controller once the port is empty (UNKNOWN,
   * not EMPTY). A pad adapter's session (2 cycles, a toast), unplugged
   * (gated 1 s), then the cable the wrong way round: its own 2 cycles and
   * 'swap the cable'. */
  arb_setup(HOST_END_PORT);
  unit_pad_claim = 1;
  arb_frames(60);
  unit_pad_claim = 0;
  unit_usb_bits_set = 0x20000500u | 0x1u;
  arb_frames(60 * 16);
  {
    int before = unit_reenums;

    unit_usb_plugged = -1;
    unit_usb_gated = 1;
    arb_frames(60);
    unit_usb_gated = 0;
    unit_usb_plugged = 1;
    unit_usb_bits_set = 0x20000100u;
    unit_osd_count = 0;
    arb_frames(60 * 16);
    /* (R11w part 2: 'swap' only while linking; Link not picked here: the
     * plain replug, the plug's one message) */
    CHECK(unit_reenums - before == 2 && unit_osd_count >= 1 && !strcmp(unit_osd_message, ARB_REPLUG_TEXT),
          "arbiter: gated 1 s after a pad session: the new plug-in's own 2 cycles (%d) and its last resort (%d, %s)",
          unit_reenums - before, unit_osd_count, unit_osd_message);
  }
  /* ...but a gate whose PP our cycle still has to put back is ours: no new
   * budget however long it lasts. */
  arb_setup(HOST_END_PORT);
  {
    int k;

    for (k = 0; k < 600 && !unit_reenums; k++)
      arb_frames(1);
  }
  unit_usb_plugged = -1;
  unit_usb_gated = unit_restore_pending = 1;
  arb_frames(60 * 5);
  unit_usb_gated = unit_restore_pending = 0;
  unit_usb_plugged = 1;
  arb_frames(60 * 16);
  CHECK(unit_reenums == 2, "arbiter: our own cycle's gate (PP pending) 5 s: still 2 cycles a session (%d)", unit_reenums);
  /* ...nor one inside its quiet window (1.5 s gated, PP back already) */
  arb_setup(HOST_END_PORT);
  {
    int k;

    for (k = 0; k < 600 && !unit_reenums; k++)
      arb_frames(1);
  }
  unit_usb_plugged = -1;
  unit_usb_gated = 1;
  arb_frames(90);
  unit_usb_gated = 0;
  unit_usb_plugged = 1;
  arb_frames(60 * 16);
  CHECK(unit_reenums == 2, "arbiter: a gate 1.5 s in our cycle's quiet window: still 2 cycles a session (%d)", unit_reenums);
  arb_setup(HOST_END_PORT);
  arb_frames(30);
  nspire_usbport_os_report(PORT_OS_OFFER, 3, 0x2563);
  arb_frames(60 * 12);
  CHECK(unit_reenums == 0 && nspire_arb_owner() == ARB_OTHER, "arbiter: an offer reported: OTHER, no cycle (%d, %s)", unit_reenums,
        nspire_arb_owner_name(nspire_arb_owner()));

  /* SEAM-10: connected but never enabled (a CX host end with a calculator
   * on the cable): the cycles, then the last resort. R11w part 2: this
   * checked 'swap the cable'; a controller the OS never enables has the same
   * shape, and with Link not picked nothing tells the two apart (the message
   * to switch the cable ends would be wrong): the plain
   * replug now; while linking the link owns the port and its own hint
   * ("Unplug the link cable and plug it in again") speaks. */
  arb_setup(0x20000100u);
  arb_frames(60 * 16);
  CHECK(unit_reenums == 2 && !strcmp(unit_osd_message, ARB_REPLUG_TEXT) && !nspire_arb_swap_hint(),
        "arbiter: never enabled: the replug (%d, %s)", unit_reenums, unit_osd_message);
  /* R11w part 2: the same while linking: the link owns
   * the port (classify: ARB_LINK), the arbiter's stages never reach their
   * last resort, so it says nothing (no swap, no replug) and its swap hint
   * stays 0; the swap advice is the link's own (flap / stuck hints) */
  arb_setup(0x20000100u);
  unit_cable_on = 1;
  nspire_link_connect();
  {
    int f;
    for (f = 0; f < 60 && nspire_arb_owner() != ARB_LINK; f++)
      arb_frames(1);
  }
  CHECK(nspire_arb_owner() == ARB_LINK && nspire_arb_stage() != ARB_STAGE_GAVE_UP,
        "arbiter: linking, never enabled: the link's port, the stages held (%s, stage %u)",
        nspire_arb_owner_name(nspire_arb_owner()), nspire_arb_stage());
  /* (here the unit link gives up after a while: the port is the arbiter's
   * again and its last resort is the replug; at no point a swap from it) */
  arb_frames(60 * 16);
  CHECK(!nspire_arb_swap_hint() && !strstr(unit_osd_log, "Swap"), "arbiter: never a swap from the arbiter (%s)",
        unit_osd_log);
  nspire_link_disconnect();
  unit_cable_on = 0;

  /* SEAM-13: a cycle that ends at tick 0 keeps its quiet window: the bits
   * it moved aren't a new port change (no stage restart, no early 2nd). */
  arb_setup(HOST_END_PORT);
  unit_reenum_to_zero = 1;
  arb_frames(60 * 4);
  unit_reenum_to_zero = 0;
  unit_usb_bits_set = 0x20000100u;
  arb_frames(20);
  unit_usb_bits_set = HOST_END_PORT;
  arb_frames(20);
  CHECK(unit_reenums == 1 && nspire_arb_stage() == ARB_STAGE_WAIT_AGAIN, "arbiter: a cycle ending at tick 0: still quiet (%d, "
        "stage %u)", unit_reenums, nspire_arb_stage());
  /* ...and ms are ms (32768 ticks a second, not 32 a ms) */
  nspire_usbport_reset();
  unit_usb_plugged = 1;
  port_frames(8);
  unit_ticks += 60u * 32768u;
  CHECK(nspire_usbport_age_ms() >= 59990 && nspire_usbport_age_ms() <= 60200, "observer: 60 s are %u ms",
        (unsigned)nspire_usbport_age_ms());

  unit_usb_plugged = 0;
  unit_usb_bits_set = 0;
  unit_watch_seen = 0;
  unit_look_nodes = unit_last_nodes = 0;
  nspire_usbport_reset();
  nspire_arb_reset();
  link_frames(60 * 10);   /* the link's flap count forgets these tests' port changes (8 s) */
}

static void test_arbiter(void)
{
  unsigned ch;

  arb_setup(0);
  CHECK(nspire_arb_set_hook(1, hook1) == 0 && nspire_arb_set_hook(3, hook3) == 0 && nspire_arb_set_hook(2, hook1) < 0,
        "arbiter: hooks for stages 1 and 3 only");
  arb_frames(60);
  CHECK(nspire_arb_owner() == ARB_NONE && nspire_arb_changes() == 0, "arbiter: empty port, no holder, no change");
  /* Something unclaimed at the host's end: WAIT, the stages by time. */
  unit_usb_plugged = 1;
  unit_usb_bits_set = 0x20000000u;
  arb_frames(60);
  CHECK(nspire_arb_owner() == ARB_WAIT && nspire_arb_stage() == ARB_STAGE_WAIT_OFFER && !nspire_arb_link_blocked(),
        "arbiter: unclaimed host end waits for the OS (stage %u)", nspire_arb_stage());
  arb_frames(60);   /* about 2 s: past 1.5 s */
  CHECK(nspire_arb_stage() == ARB_STAGE_WAIT_AGAIN && hook_calls[1] == 1, "arbiter: stage 2 after the cycle hook (%u, %d)",
        nspire_arb_stage(), hook_calls[1]);
  arb_frames(60 * 4);
  CHECK(nspire_arb_stage() == ARB_STAGE_GAVE_UP && hook_calls[3] == 1 && hook_calls[1] == 1,
        "arbiter: gave up after the direct hook (%u, %d %d)", nspire_arb_stage(), hook_calls[1], hook_calls[3]);
  /* The OS offers a pad: PAD, the link may not start or watch. */
  unit_pad_claim = 1;
  arb_frames(2);
  CHECK(nspire_arb_owner() == ARB_PAD && nspire_arb_link_blocked() && nspire_arb_link_refusal() &&
        !strcmp(nspire_arb_link_refusal(), "The USB port has a controller in it"), "arbiter: pad owns, link refused");
  /* Pad out, port still showing the adapter: waiting again from the start, no stage at once. */
  unit_pad_claim = 0;
  arb_frames(2);
  CHECK(nspire_arb_owner() == ARB_WAIT && nspire_arb_stage() == ARB_STAGE_WAIT_OFFER && !nspire_arb_link_refusal(),
        "arbiter: pad gone: wait again at stage 0 (%u), link may start", nspire_arb_stage());
  unit_usb_plugged = 0;
  unit_usb_bits_set = 0;
  arb_frames(20);
  ch = nspire_arb_changes();
  CHECK(nspire_arb_owner() == ARB_NONE && nspire_arb_stage() == 0, "arbiter: EMPTY edge resets (holder %d, stage %u)",
        nspire_arb_owner(), nspire_arb_stage());
  arb_frames(600);
  CHECK(nspire_arb_changes() == ch && hook_calls[1] == 1 && hook_calls[3] == 1,
        "arbiter: EMPTY for 600 frames: no change, no more hooks");
  nspire_arb_reset();
  nspire_usbport_reset();
  link_frames(60 * 10);   /* the link's flap count forgets these tests' port changes (8 s) */
}

/* A configured-but-not-streaming adapter and a detached adapter whose OS
 * transfers still drain both own the port. Neither may lose power or the
 * controller registers to link setup, even when the port observer is stale. */
static void test_arbiter_audio(void)
{
  int draining;

  for (draining = 0; draining < 2; draining++)
  {
    long calls;
    const char *why;

    arb_setup(HOST_END_PORT);
    unit_audio_claim = !draining;
    unit_audio_owns = draining;
    calls = unit_watch_calls;
    arb_frames(60 * 16);
    CHECK(nspire_arb_owner() == ARB_OTHER && nspire_arb_link_blocked() && unit_reenums == 0 &&
          unit_watch_calls == calls,
          "audio %s: no auto cycle or link watch (%d cycles, %ld watches, holder %s)",
          draining ? "draining" : "attached", unit_reenums, unit_watch_calls - calls,
          nspire_arb_owner_name(nspire_arb_owner()));
    CHECK(nspire_arb_manual_cycle() == -3 && unit_reenums == 0,
          "audio %s: manual USB restart refused", draining ? "draining" : "attached");
    unit_cable_opened_automatic = -1;
    nspire_link_connect();
    CHECK(nspire_link_state() == LINK_OFF && unit_cable_opened_automatic == -1 &&
          !strcmp(nspire_link_status(), "The USB port is in use for sound"),
          "audio %s: Link refused before opening the cable: %s",
          draining ? "draining" : "attached", nspire_link_status());

    /* A stale calculator report retains LINK precedence, but can never let
     * the fast transport take the controller away from the audio pipe. */
    nspire_usbport_os_report(PORT_OS_NODE_SERVICE, 0, 0);
    nspire_arb_tick();
    why = nspire_arb_fast_refusal();
    CHECK(nspire_arb_owner() == ARB_LINK && why && !strcmp(why, "USB audio has the port"),
          "audio %s: fast takeover refused despite stale node (%s)",
          draining ? "draining" : "attached", why ? why : "none");
    CHECK(nspire_arb_link_cycle() == -2 && unit_reenums == 0,
          "audio %s: a stale link cannot restart USB power",
          draining ? "draining" : "attached");

    unit_usb_plugged = 0;
    unit_usb_bits_set = 0;
    arb_keys(20);
    CHECK(nspire_arb_owner() == ARB_OTHER && nspire_arb_manual_cycle() == -3 && unit_reenums == 0,
          "audio %s: an empty port reading does not override resource ownership",
          draining ? "draining" : "attached");
    unit_audio_claim = unit_audio_owns = 0;
    arb_keys(1);
    CHECK(nspire_arb_owner() == ARB_NONE && !nspire_arb_link_blocked() && !nspire_arb_link_refusal(),
          "audio %s: the port is released only after the sink retires",
          draining ? "draining" : "attached");
  }
  arb_setup(0);
  link_frames(60 * 10);
}

/* Automatic watching already avoided drives; picking Link explicitly must
 * also refuse before any NavNet transport can open on their endpoints. */
static void test_arbiter_drive(void)
{
  arb_setup(HOST_END_PORT);
  unit_drive_claim = 1;
  arb_frames(60 * 16);
  CHECK(nspire_arb_owner() == ARB_OTHER && nspire_arb_link_blocked() && unit_reenums == 0,
        "drive: attached endpoint owns the port without power cycles");
  unit_cable_opened_automatic = -1;
  nspire_link_connect();
  CHECK(nspire_link_state() == LINK_OFF && unit_cable_opened_automatic == -1 &&
        !strcmp(nspire_link_status(), "The USB port has a drive in it"),
        "drive: explicit Link refused before opening the cable: %s", nspire_link_status());
  nspire_usbport_os_report(PORT_OS_NODE_SERVICE, 0, 0);
  nspire_arb_tick();
  CHECK(nspire_arb_link_cycle() == -2 && unit_reenums == 0,
        "drive: a stale link cannot restart the attached drive");
  unit_drive_claim = 0;
  arb_setup(0);
  link_frames(60 * 10);
}

/* A hub can expose several passive OS drivers at once. PAD is just the
 * displayed priority, not an exclusive grant over an audio pipe.
 * Removing one device must not release the other device's claims. */
static void test_arbiter_hub(void)
{
  for (int with_drive = 0; with_drive < 2; with_drive++)
    for (int draining = 0; draining < 2; draining++)
    {
      arb_setup(HOST_END_PORT);
      unit_pad_claim = 1;
      unit_audio_claim = !draining;
      unit_audio_owns = draining;
      unit_drive_claim = with_drive;
      arb_frames(60 * 16);
      CHECK(nspire_arb_owner() == ARB_PAD && nspire_arb_link_blocked() && unit_reenums == 0,
            "hub: pad plus %s audio%s prevents automatic takeover",
            draining ? "draining" : "attached", with_drive ? " and drive" : "");
      CHECK(nspire_arb_manual_cycle() == -3 && nspire_arb_comeup_cycle() == -3 && unit_reenums == 0,
            "hub: neither manual nor controller discovery restarts the shared bus");
      unit_cable_opened_automatic = -1;
      nspire_link_connect();
      CHECK(nspire_link_state() == LINK_OFF && unit_cable_opened_automatic == -1 &&
            unit_pad_claim && (unit_audio_claim || unit_audio_owns),
            "hub: Link refuses without disabling either passive driver's claim");
      unit_audio_claim = unit_audio_owns = 0;
      nspire_arb_tick();
      CHECK(nspire_arb_owner() == ARB_PAD && nspire_arb_link_blocked(),
            "hub: audio removal does not release the controller");
      unit_audio_owns = 1;
      unit_pad_claim = 0;
      nspire_arb_tick();
      CHECK(nspire_arb_owner() == ARB_OTHER && nspire_arb_link_blocked() && nspire_arb_manual_cycle() == -3,
            "hub: controller removal does not release audio still draining");
      unit_audio_owns = 0;
      nspire_arb_tick();
      CHECK(nspire_arb_link_blocked() == with_drive,
            "hub: only the remaining drive, if any, keeps the final claim");
      unit_drive_claim = 0;
    }
  arb_setup(0);
  link_frames(60 * 10);
}

static void test_link_watch(void)
{
  long calls, unwatches;
  int opened;

  unit_cable_on = 1;
  unit_cable_clear();
  unit_watch_seen = 0;

  /* Nothing on the USB port: no watching at all, and the OS isn't let run
   * (its dimming and power-down stay out of the game). */
  link_mode_for("BPEE", SERIAL_MODE_RFU, LINK_DEVICE_AUTO);
  unit_usb_plugged = 0;
  calls = unit_watch_calls;
  link_frames(5);
  CHECK(unit_watch_calls == calls && !nspire_os_wanted, "nothing plugged in: %ld watches, OS wanted %u",
        unit_watch_calls - calls, nspire_os_wanted);

  /* Something on it: watched each frame, the OS let run; a port that
   * can't tell is watched too (the transport sleeps less often then). */
  unit_usb_plugged = 1;
  link_frames(3);
  CHECK(unit_watch_calls == calls + 3 && unit_watch_plugged == 1 && unit_watch_may_connect &&
        (nspire_os_wanted & OS_WANT_LINK), "plugged in: watched (%ld), OS wanted %u",
        unit_watch_calls - calls, nspire_os_wanted);
  /* P2: 'fresh' (a tick each frame) for 10 s after the port's change,
   * then not (the tick when it's near); a change makes it fresh again. */
  CHECK(unit_watch_fresh == 1, "P2: just plugged in: fresh (%ld)", unit_watch_fresh);
  link_frames(60 * 9);
  CHECK(unit_watch_fresh == 1, "P2: 9 s after the change: still fresh (%ld)", unit_watch_fresh);
  link_frames(60 * 2);
  CHECK(unit_watch_fresh == 0, "P2: 11 s after the change: not fresh (%ld)", unit_watch_fresh);
  unit_usb_bits_set = 0x20000031u;
  link_frames(1);
  CHECK(unit_watch_fresh == 1, "P2: the port's bits changed: fresh again (%ld)", unit_watch_fresh);
  unit_usb_bits_set = 0;
  unit_usb_plugged = -1;
  link_frames(1);
  CHECK(unit_watch_plugged == -1 && (nspire_os_wanted & OS_WANT_LINK), "a port that can't tell is watched");
  /* But a CX's controller its OS turned off (nothing plugged in): not
   * watched, no spare time, only its ticks (link_cable_gated); a link
   * held for the other side is let go. */
  unit_usb_gated = 1;
  calls = unit_watch_calls;
  unwatches = unit_unwatches;
  {
    long gated = unit_gated_calls;
    link_frames(3);
    CHECK(unit_watch_calls == calls && unit_gated_calls == gated + 3 && unit_unwatches == unwatches + 3 &&
          !(nspire_os_wanted & OS_WANT_LINK), "a CX's controller off: not watched (%ld), its ticks (%ld), OS "
          "wanted %u", unit_watch_calls - calls, unit_gated_calls - gated, nspire_os_wanted);
  }
  unit_usb_gated = 0;
  /* The link's bit only: the gamepad's stays as it set it. */
  nspire_os_wanted |= OS_WANT_PAD;
  unit_usb_plugged = 0;
  link_frames(1);
  CHECK(nspire_os_wanted == OS_WANT_PAD, "unplugged: only the link's bit cleared (%u)", nspire_os_wanted);
  nspire_os_wanted = 0;

  /* A Game Boy Player game on Auto has no link: with the other's gpSP on
   * the cable it's not watched (what was held let go) and doesn't say
   * "no link" each frame. */
  link_mode_for("GBPX", SERIAL_MODE_GBP, LINK_DEVICE_AUTO);
  unit_usb_plugged = 1;
  unit_watch_seen = 2;
  calls = unit_watch_calls;
  unwatches = unit_unwatches;
  unit_osd_message[0] = 0;
  link_frames(10);
  CHECK(unit_watch_calls == calls && unit_unwatches == unwatches + 10 && nspire_link_state() == LINK_OFF &&
        !unit_osd_message[0] && !nspire_os_wanted, "a game with no link: %ld watches, \"%s\"",
        unit_watch_calls - calls, unit_osd_message);

  /* The gamepad's driver has the port: Link refused, nothing watched. */
  link_mode_for("BPEE", SERIAL_MODE_RFU, LINK_DEVICE_AUTO);
  unit_pad_owns = 1;
  unit_cable_opened_automatic = -1;
  nspire_link_connect();
  CHECK(nspire_link_state() == LINK_OFF && unit_cable_opened_automatic == -1 &&
        !strcmp(nspire_link_status(), "The USB port is the gamepad's (USB driver: Direct)") &&
        !strcmp(unit_osd_message, nspire_link_status()), "Link with the gamepad's driver on the port: %s",
        nspire_link_status());
  calls = unit_watch_calls;
  unwatches = unit_unwatches;
  link_frames(3);
  CHECK(unit_watch_calls == calls && unit_unwatches == unwatches && nspire_link_state() == LINK_OFF &&
        !nspire_os_wanted, "the gamepad's port isn't watched");
  unit_pad_owns = 0;

  /* The other gpSP there: linked on its own, never one channel; the game
   * goes on without its hello (frame_begin returns), and after
   * AUTO_HELLO_MS it's given up, held until something changes. (Its
   * close lets go of the connection: a watch then sees only the node.) */
  unit_cable_clear();
  link_frames(1);
  opened = unit_cable_opened_automatic;
  CHECK(nspire_link_state() == LINK_LOOKING && opened == 1, "linking on its own (opened %d)", opened);
  unit_watch_seen = 1;
  link_frames(60);
  CHECK(nspire_link_state() == LINK_LOOKING, "on its own, the game goes on while the hello's awaited");
  /* R3: the pad log writes no file in a link's come-up */
  CHECK(!nspire_link_quiet_for_writes(), "no pad log write while looking for the other GBA");
  link_frames(5 * 60);
  CHECK(nspire_link_state() == LINK_OFF, "no hello in 5 s: given up (%s)", nspire_link_status());
  link_frames(3);
  CHECK(nspire_link_state() == LINK_OFF && !unit_watch_may_connect, "held after that");
  /* The cable out (the port's state, a frame): the next time it's in, it
   * links again. */
  unit_usb_plugged = 0;
  link_frames(1);
  unit_usb_plugged = 1;
  unit_watch_seen = 1;
  link_frames(1);
  CHECK(unit_watch_may_connect, "the cable out and in: connects again");

  /* Linked on its own, then the cable broke: tried again after a while,
   * not held. */
  unit_watch_seen = 2;
  link_frames(1);
  unit_watch_seen = 1;
  hello_from(SERIAL_MODE_RFU);
  link_frames(1);
  CHECK(nspire_link_state() == LINK_LINKED, "the other's hello: linked on its own (%s)", nspire_link_status());
  unit_cable_on = 0;
  link_frames(1);
  CHECK(nspire_link_state() == LINK_OFF && !strcmp(nspire_link_status(), "The link cable came out."),
        "the cable broke: %s", nspire_link_status());
  CHECK(!nspire_link_quiet_for_writes(), "no pad log write right after a link ended (its retry)");
  unit_cable_on = 1;
  unit_watch_seen = 1;
  link_frames(2);
  CHECK(!unit_watch_may_connect, "right after a break: not yet");
  link_frames(5 * 60);
  CHECK(unit_watch_may_connect, "5 s after a break: connects again");

  /* An end that was asked for (unlinked) holds; Link picked releases it. */
  unit_watch_seen = 2;
  link_frames(1);
  unit_watch_seen = 1;
  hello_from(SERIAL_MODE_RFU);
  link_frames(1);
  nspire_link_disconnect();
  link_frames(6 * 60);
  CHECK(nspire_link_state() == LINK_OFF && !unit_watch_may_connect, "unlinked: held");
  nspire_link_connect();
  CHECK(nspire_link_state() == LINK_LOOKING && unit_cable_opened_automatic == 0, "Link picked: links");
  nspire_link_disconnect();
  unit_watch_seen = 0;
  unit_usb_plugged = 0;
  link_frames(1);
  unit_cable_clear();
}

/* LNK-3: the other's "in my menu" belongs to the link it came in. A link
 * that ended with the other in its menu (MENU 1, then its BYE: MENU 0 is
 * never sent then), and Link picked again with a peer that never says
 * hello: given up after WAIT_GIVE_UP_MS (15 s: 750 reads of 20 ms), not
 * held as "in the menu". The same for a MENU 1 that comes while looking. */
static void test_link_menu_stale(void)
{
  static const u8 menu1[8] = { 3, 1, 0, 0, 0, 0, 0, 0 }, bye[8] = { 4, 0, 0, 0, 0, 0, 0, 0 };
  long reads;

  link_mode_for("BPEE", SERIAL_MODE_RFU, LINK_DEVICE_CABLE);
  unit_cable_on = 1;
  unit_cable_clear();
  nspire_link_connect();
  hello_from(SERIAL_MODE_SERIAL_POKE);
  link_frames(2);
  CHECK(nspire_link_state() == LINK_LINKED, "menu, stale: linked (%s)", nspire_link_status());
  unit_cable_push(menu1, 8);
  unit_cable_push(bye, 8);
  link_frames(2);
  CHECK(nspire_link_state() == LINK_OFF && !strcmp(nspire_link_status(), "The other GBA unlinked"),
        "menu, stale: the other unlinked from its menu (%s)", nspire_link_status());
  unit_cable_clear();
  nspire_link_connect();
  reads = unit_cable_receives;
  link_frames(1);
  reads = unit_cable_receives - reads;
  CHECK(nspire_link_state() == LINK_OFF && !strcmp(nspire_link_status(), "The other GBA stopped answering.") &&
        reads >= 740 && reads <= 800, "menu, stale: Link picked, no hello: given up in 15 s (%ld reads, %s)",
        reads, nspire_link_status());
  unit_cable_clear();
  nspire_link_connect();
  unit_cable_push(menu1, 8);
  reads = unit_cable_receives;
  link_frames(1);
  reads = unit_cable_receives - reads;
  CHECK(nspire_link_state() == LINK_OFF && reads >= 740 && reads <= 800,
        "menu, stale: a MENU 1 while looking holds nothing (%ld reads, %s)", reads, nspire_link_status());
  unit_cable_clear();
  unit_cable_on = 0;
}

/* ---- The hello and the choice (nspire_link.c's hello comment) ---- */

/* The games it's checked with: one with the wireless adapter (Emerald, its
 * cable Gen 3's stand-in), a Gen 3 game without it (Ruby), two with other
 * stand-ins (Advance Wars, Golden Sun), one without (The Lost Age): its
 * code, gba_over.h's serial_mode, its stand-in. */
static const struct
{
  const char *name, *code;
  int game_mode, stand_in, adapter;
} kinds[] =
{
  { "Emerald", "BPEE", SERIAL_MODE_RFU, SERIAL_MODE_SERIAL_POKE, 1 },
  { "Ruby", "AXVE", SERIAL_MODE_SERIAL_POKE, SERIAL_MODE_SERIAL_POKE, 0 },
  { "Advance Wars", "AWRE", SERIAL_MODE_SERIAL_AW1, SERIAL_MODE_SERIAL_AW1, 0 },
  { "Golden Sun", "AGSE", SERIAL_MODE_AUTO, SERIAL_MODE_SERIAL_MSIO, 0 },
  { "The Lost Age", "AGFE", SERIAL_MODE_AUTO, SERIAL_MODE_CABLE, 0 },
};
#define KINDS (int)(sizeof(kinds) / sizeof(kinds[0]))
static const char *const setting_names[] = { "Auto", "Wireless", "Link cable", "exact" };

struct gba
{
  int kind;
  uint32_t setting;
};

/* The rule, written out again: a GBA wants the adapter
 * on Wireless, or on Auto with a game that has one; its cable is its
 * game's stand-in, or as wired on exact; alone it plays the adapter if it
 * wants it, else its cable. Two: the adapter if both want it, else the
 * stand-in if both have the same one, else the cable as wired. */
static int spec_wants(struct gba g)
{
  return g.setting == LINK_DEVICE_WIRELESS || (g.setting == LINK_DEVICE_AUTO && kinds[g.kind].adapter);
}

static int spec_cable(struct gba g)
{
  return g.setting == LINK_DEVICE_EXACT ? SERIAL_MODE_CABLE : kinds[g.kind].stand_in;
}

static int spec_alone(struct gba g)
{
  return spec_wants(g) ? SERIAL_MODE_RFU : spec_cable(g);
}

static int spec_choice(struct gba a, struct gba b)
{
  if (spec_wants(a) && spec_wants(b))
    return SERIAL_MODE_RFU;
  if (spec_cable(a) == spec_cable(b) && spec_cable(a) != SERIAL_MODE_CABLE)
    return spec_cable(a);
  return SERIAL_MODE_CABLE;
}

static const char *device_text(int mode)
{
  return mode == SERIAL_MODE_RFU ? "Wireless Adapter" :
         mode == SERIAL_MODE_CABLE ? "Link cable, exact" : "Link cable, quick";
}

static void spec_status(char *out, size_t size, int player, int mode, const char *why)
{
  snprintf(out, size, "Linked: player %d (%s)%s%s", player, device_text(mode), why ? ": " : "",
           why ? why : "");
}

/* Why this GBA (me) shows, the other's hello having an extension. */
static const char *spec_why(int chosen, struct gba me, struct gba other)
{
  if (chosen == spec_alone(me))
    return NULL;
  if (spec_wants(me))
    return "the other GBA has no Wireless Adapter";
  if (other.setting == LINK_DEVICE_EXACT)
    return "the other GBA is set to exact";
  if (spec_cable(other) == SERIAL_MODE_CABLE)
    return "the other game needs the exact cable";
  return "the other game's link is different";
}

/* A protocol-1 build's hello (e24876d's: 16 bytes, no extension). */
static unsigned old_hello(int mode, uint32_t nonce, u8 *out)
{
  memset(out, 0, 24);
  out[0] = 1;    /* MSG_HELLO */
  out[2] = 16;
  memcpy(out + 8, "GPLK", 4);
  out[12] = 1;   /* version */
  out[14] = (u8)mode;
  out[16] = nonce & 0xFF;
  out[17] = (nonce >> 8) & 0xFF;
  memcpy(out + 20, "BPEE", 4);
  return 24;
}

static uint32_t le16_at(const u8 *p) { return p[0] | p[1] << 8; }
static uint32_t le32_at(const u8 *p) { return le16_at(p) | le16_at(p + 2) << 16; }

/* This GBA's first hello (nonce given): as it says it, then nothing comes. */
static unsigned hello_of(struct gba g, uint32_t nonce, u8 *out)
{
  link_mode_for(kinds[g.kind].code, kinds[g.kind].game_mode, g.setting);
  unit_cable_nonce = nonce;
  unit_cable_clear();
  unit_cable_logged = 0;
  nspire_link_connect();
  nspire_link_frame_begin();   /* waits for the other's, gives up */
  memcpy(out, unit_cable_log[0], unit_cable_log_len[0]);
  return unit_cable_log_len[0];
}

/* This GBA (nonce given) with the other's hellos: its serial_mode once
 * linked (-1 if it isn't), the status, its player. */
static int link_to(struct gba g, uint32_t nonce, const u8 *hello, unsigned len, char *status,
                   size_t size, int *player)
{
  int mode, linked;

  link_mode_for(kinds[g.kind].code, kinds[g.kind].game_mode, g.setting);
  unit_cable_nonce = nonce;
  unit_cable_clear();
  unit_cable_logged = 0;
  nspire_link_connect();
  unit_cable_push(hello, len);
  nspire_link_frame_begin();
  snprintf(status, size, "%s", nspire_link_status());
  *player = (int)netplay_client_id + 1;
  mode = serial_mode;
  linked = nspire_link_state() == LINK_LINKED;
  nspire_link_disconnect();
  return linked ? mode : -1;
}

/* The answer (MSG_HELLO) this side sent a protocol-1 hello, or NULL. */
static const u8 *answer_sent(void)
{
  int i;
  for (i = 0; i < unit_cable_logged; i++)
    if (unit_cable_log[i][0] == 1 && unit_cable_log_len[i] >= 24)
      return unit_cable_log[i];
  return NULL;
}

static void test_link_choice(void)
{
  static u8 hellos[2][KINDS * 4][64];
  static unsigned hello_len[2][KINDS * 4];
  char status[128], want[128];
  struct gba a, b;
  int i, j, n, pa, pb, ma, mb, bad;
  u8 msg[64];

  unit_cable_on = 1;
  /* Every GBA's hello: MSG_HELLO_EXT, version 1, e24876d's mode in the
   * mode field, and the extension. */
  bad = 0;
  for (i = 0; i < KINDS * 4; i++)
  {
    a.kind = i / 4;
    a.setting = i % 4;
    for (n = 0; n < 2; n++)
      hello_len[n][i] = hello_of(a, n + 1, hellos[n][i]);
    if (hellos[0][i][0] != 8 || hello_len[0][i] < 8 + 18 || le32_at(hellos[0][i] + 8) != 0x4b4c5047 ||
        le16_at(hellos[0][i] + 12) != 1 ||
        le16_at(hellos[0][i] + 14) != (spec_wants(a) ? SERIAL_MODE_RFU : SERIAL_MODE_CABLE))
      bad++;
  }
  CHECK(!bad, "%d hellos aren't MSG_HELLO_EXT, version 1, e24876d's mode", bad);

  /* Two of this build: every pair of (game, setting), each linked with the
   * other's hello: both make the rule's choice, players 2 and 1, and say
   * it, and why when it isn't their setting's. */
  bad = 0;
  for (i = 0; i < KINDS * 4; i++)
    for (j = 0; j < KINDS * 4; j++)
    {
      char got_b[128], want_b[128];
      int chosen;
      a.kind = i / 4; a.setting = i % 4;
      b.kind = j / 4; b.setting = j % 4;
      chosen = spec_choice(a, b);
      ma = link_to(a, 1, hellos[1][j], hello_len[1][j], status, sizeof(status), &pa);
      mb = link_to(b, 2, hellos[0][i], hello_len[0][i], got_b, sizeof(got_b), &pb);
      spec_status(want, sizeof(want), 2, chosen, spec_why(chosen, a, b));
      spec_status(want_b, sizeof(want_b), 1, chosen, spec_why(chosen, b, a));
      if (ma != chosen || mb != chosen || pa != 2 || pb != 1 || strcmp(status, want) ||
          strcmp(got_b, want_b) || spec_choice(b, a) != chosen)
      {
        if (bad++ < 5)
          printf("  %s on %s with %s on %s: %d / %d, not %d; \"%s\" / \"%s\"\n", kinds[a.kind].name,
                 setting_names[a.setting], kinds[b.kind].name, setting_names[b.setting], ma, mb,
                 chosen, status, got_b);
      }
    }
  CHECK(!bad, "%d of %d pairs of this build chose differently or said it wrong", bad,
        KINDS * 4 * KINDS * 4);

  /* A few as words. */
  a.kind = 0; a.setting = LINK_DEVICE_AUTO;   /* Emerald on Auto */
  b.kind = 1; b.setting = LINK_DEVICE_AUTO;   /* Ruby on Auto */
  ma = link_to(a, 1, hellos[1][1 * 4 + LINK_DEVICE_AUTO], hello_len[1][1 * 4], status, sizeof(status), &pa);
  CHECK(ma == SERIAL_MODE_SERIAL_POKE &&
        !strcmp(status, "Linked: player 2 (Link cable, quick): the other GBA has no Wireless Adapter"),
        "Emerald (Auto) with Ruby: %s", status);
  b.kind = 0; b.setting = LINK_DEVICE_CABLE;   /* Emerald on Link cable */
  ma = link_to(a, 1, hellos[1][0 * 4 + LINK_DEVICE_CABLE], hello_len[1][LINK_DEVICE_CABLE], status,
               sizeof(status), &pa);
  CHECK(ma == SERIAL_MODE_SERIAL_POKE &&
        !strcmp(status, "Linked: player 2 (Link cable, quick): the other GBA has no Wireless Adapter"),
        "Emerald, Auto with Link cable: %s", status);
  a.setting = LINK_DEVICE_CABLE;
  ma = link_to(a, 1, hellos[1][0 * 4 + LINK_DEVICE_EXACT], hello_len[1][LINK_DEVICE_EXACT], status,
               sizeof(status), &pa);
  CHECK(ma == SERIAL_MODE_CABLE &&
        !strcmp(status, "Linked: player 2 (Link cable, exact): the other GBA is set to exact"),
        "Emerald, Link cable with exact: %s", status);
  a.kind = 3; a.setting = LINK_DEVICE_AUTO;   /* Golden Sun with The Lost Age */
  ma = link_to(a, 1, hellos[1][4 * 4 + LINK_DEVICE_AUTO], hello_len[1][4 * 4], status, sizeof(status), &pa);
  CHECK(ma == SERIAL_MODE_CABLE &&
        !strcmp(status, "Linked: player 2 (Link cable, exact): the other game needs the exact cable"),
        "Golden Sun with The Lost Age: %s", status);
  a.kind = 0; a.setting = LINK_DEVICE_WIRELESS;   /* Emerald, Wireless with Golden Sun, Wireless */
  ma = link_to(a, 1, hellos[1][3 * 4 + LINK_DEVICE_WIRELESS], hello_len[1][3 * 4 + 1], status,
               sizeof(status), &pa);
  CHECK(ma == SERIAL_MODE_RFU && !strcmp(status, "Linked: player 2 (Wireless Adapter)"),
        "two different games, both Wireless: %s", status);

  /* e24876d (protocol 1): its mode is the adapter on Wireless or Auto with
   * an adapter game, else the cable as wired (it has no exact; its cable
   * is). This side takes that, answers with a MSG_HELLO of it that e24876d
   * accepts (version 1, its mode, another nonce), and is the player the
   * two nonces make it. Its nonce 1 or ours (2: ours is rolled again). */
  bad = 0;
  for (i = 0; i < KINDS * 4; i++)
    for (j = 0; j < KINDS * 3; j++)
      for (n = 1; n <= 2; n++)
      {
        struct gba old;
        const u8 *answer;
        int mode, ok;
        const char *why;
        a.kind = i / 4; a.setting = i % 4;
        old.kind = j / 3; old.setting = j % 3;
        mode = spec_wants(old) ? SERIAL_MODE_RFU : SERIAL_MODE_CABLE;
        old_hello(mode, n, msg);
        ma = link_to(a, 2, msg, 24, status, sizeof(status), &pa);
        answer = answer_sent();
        why = mode == spec_alone(a) ? NULL : mode == SERIAL_MODE_RFU ? "the other gpSP is older and uses it" :
              spec_wants(a) ? "the other GBA has no Wireless Adapter" : "the other gpSP is older: exact only";
        spec_status(want, sizeof(want), answer && le32_at(answer + 16) > (uint32_t)n ? 1 : 2, mode, why);
        ok = ma == mode && answer && le32_at(answer + 8) == 0x4b4c5047 && le16_at(answer + 12) == 1 &&
             (int)le16_at(answer + 14) == mode && le32_at(answer + 16) != (uint32_t)n &&
             le16_at(answer + 2) >= 16 && !strcmp(status, want);
        if (!ok && bad++ < 5)
          printf("  %s on %s with e24876d's %s on %s (nonce %d): %d, not %d; \"%s\"\n", kinds[a.kind].name,
                 setting_names[a.setting], kinds[old.kind].name, setting_names[old.setting], n, ma, mode, status);
      }
  CHECK(!bad, "%d links with e24876d went wrong", bad);

  /* 8229804 (protocol 1 too, and its stand-ins): it takes this side's
   * answer if it's its own mode, or the cable as wired for its stand-in
   * (which it then plays as wired too). */
  {
    static const int modes[] = { SERIAL_MODE_RFU, SERIAL_MODE_CABLE, SERIAL_MODE_SERIAL_POKE,
                                 SERIAL_MODE_SERIAL_AW1, SERIAL_MODE_SERIAL_MSIO, SERIAL_MODE_SERIAL_FE };
    bad = 0;
    for (i = 0; i < KINDS * 4; i++)
      for (j = 0; j < (int)(sizeof(modes) / sizeof(modes[0])); j++)
      {
        const u8 *answer;
        int theirs = modes[j], chosen, takes;
        const char *why;
        a.kind = i / 4; a.setting = i % 4;
        chosen = theirs == SERIAL_MODE_RFU ? SERIAL_MODE_RFU :
                 spec_cable(a) == theirs ? theirs : SERIAL_MODE_CABLE;
        old_hello(theirs, 1, msg);
        ma = link_to(a, 2, msg, 24, status, sizeof(status), &pa);
        answer = answer_sent();
        takes = answer && ((int)le16_at(answer + 14) == theirs ||
                           ((int)le16_at(answer + 14) == SERIAL_MODE_CABLE && theirs != SERIAL_MODE_RFU));
        why = chosen == spec_alone(a) ? NULL : chosen == SERIAL_MODE_RFU ? "the other gpSP is older and uses it" :
              spec_wants(a) ? "the other GBA has no Wireless Adapter" :
              theirs == SERIAL_MODE_CABLE ? "the other gpSP is older: exact only" : "the other game's link is different";
        spec_status(want, sizeof(want), 1, chosen, why);
        if ((ma != chosen || !takes || (int)le16_at(answer + 14) != chosen || strcmp(status, want)) && bad++ < 5)
          printf("  %s on %s with 8229804's mode %d: %d, not %d; \"%s\"\n", kinds[a.kind].name,
                 setting_names[a.setting], theirs, ma, chosen, status);
      }
    CHECK(!bad, "%d links with 8229804 went wrong", bad);
  }

  /* A nonce tie: both roll again and say hello again (this side's next
   * nonce is 2; the other's 5), and then link. */
  a.kind = 0; a.setting = LINK_DEVICE_AUTO;
  link_mode_for("BPEE", SERIAL_MODE_RFU, LINK_DEVICE_AUTO);
  unit_cable_nonce = 1;
  unit_cable_clear();
  unit_cable_logged = 0;
  nspire_link_connect();
  memcpy(msg, hellos[0][0], hello_len[0][0]);   /* nonce 1: ours */
  unit_cable_push(msg, hello_len[0][0]);
  msg[16] = 5;
  unit_cable_push(msg, hello_len[0][0]);
  nspire_link_frame_begin();
  CHECK(nspire_link_state() == LINK_LINKED && netplay_client_id == 1 && unit_cable_logged >= 2 &&
        unit_cable_log[1][0] == 8 && le32_at(unit_cable_log[1] + 16) == 2,
        "a tie, then linked as player 2: %s (%d sent)", nspire_link_status(), unit_cable_logged);
  nspire_link_disconnect();
  /* Ties every time: it gives up after HELLO_TRIES (16). */
  link_mode_for("BPEE", SERIAL_MODE_RFU, LINK_DEVICE_AUTO);
  unit_cable_clear();
  nspire_link_connect();
  for (i = 0; i < 20; i++)
  {
    memcpy(msg, hellos[0][0], hello_len[0][0]);
    msg[16] = (u8)(1 + i);   /* our nonce each time */
    unit_cable_push(msg, hello_len[0][0]);
  }
  nspire_link_frame_begin();
  CHECK(nspire_link_state() == LINK_OFF && !strcmp(nspire_link_status(), "Couldn't pick player 1: try again"),
        "ties every time: %s", nspire_link_status());

  /* A later build's hello: a field this one doesn't know, a newer
   * protocol, a longer EXT_LINK. Read as far as it knows. */
  {
    static const u8 later_ext[] = { 9, 3, 'x', 'y', 'z', 1, 2, 7, 0,
                                    2, 6, 1 /* wants the adapter */, 0, 3, 0, 0xAA, 0xBB };
    memcpy(msg, hellos[1][0], 8 + 16);
    msg[0] = 8;
    msg[8 + 16] = sizeof(later_ext);
    msg[8 + 17] = 0;
    memcpy(msg + 8 + 18, later_ext, sizeof(later_ext));
    msg[2] = 18 + sizeof(later_ext);
    a.kind = 0; a.setting = LINK_DEVICE_AUTO;
    ma = link_to(a, 1, msg, 8 + 18 + sizeof(later_ext), status, sizeof(status), &pa);
    CHECK(ma == SERIAL_MODE_RFU && !strcmp(status, "Linked: player 2 (Wireless Adapter)"),
          "a later build's hello: %d, %s", ma, status);
  }

  /* The round trip at the start (ping_phase): two of this build ping
   * PINGS (32) times; with protocol 1 (e24876d's hello) never, as it
   * wouldn't answer. */
  a.kind = 0; a.setting = LINK_DEVICE_AUTO;
  unit_cable_pings = 0;
  link_to(a, 1, hellos[1][0], hello_len[1][0], status, sizeof(status), &pa);
  CHECK(unit_cable_pings == 32, "this build with this build: %ld pings, not 32", unit_cable_pings);
  unit_cable_pings = 0;
  old_hello(SERIAL_MODE_RFU, 2, msg);
  link_to(a, 1, msg, 24, status, sizeof(status), &pa);
  CHECK(unit_cable_pings == 0 && !strncmp(status, "Linked", 6), "with e24876d: %ld pings, %s",
        unit_cable_pings, status);

  /* Not gpSP's: refused. */
  memcpy(msg, hellos[1][0], hello_len[1][0]);
  msg[8] = 'X';
  ma = link_to(a, 1, msg, hello_len[1][0], status, sizeof(status), &pa);
  CHECK(ma < 0 && !strcmp(status, "The other side isn't a gpSP link"), "not gpSP: %s", status);
  memcpy(msg, hellos[1][0], hello_len[1][0]);
  msg[12] = 2;   /* a version no build sends */
  ma = link_to(a, 1, msg, hello_len[1][0], status, sizeof(status), &pa);
  CHECK(ma < 0 && !strcmp(status, "The other side isn't a gpSP link"), "version 2: %s", status);
  old_hello(SERIAL_MODE_GBP, 2, msg);
  ma = link_to(a, 1, msg, 24, status, sizeof(status), &pa);
  CHECK(ma < 0, "a protocol-1 mode no gpSP sends: %s", status);
  unit_cable_on = 0;
  unit_cable_clear();
}

/* Fire Emblem's stand-in (serial_fe.c) as player 2, linked: the other
 * GBA's packet (in a frame's message, nspire_link.c) comes out a word a
 * transfer of the pretend parent's, then 0x7FFF; this GBA's packet, put
 * out a word a transfer after the 0x4FFF of one it dropped, goes in this
 * side's next frame message; the empty plugs read 0xFFFF, and so does the
 * other GBA's unlinked. */
/* Link speed: kept in word 68's bit 8 with the device; the fast
 * transport's marker (nspire_link_speed.c): a run that died with it in use
 * turns it off, said once (log, toast), off in the runs after too, on
 * again when Link speed is set to Auto; Compatible never tries it. */
static void test_link_speed(void)
{
  char path[600], text[200];
  FILE *f;
  size_t n;

  snprintf(main_path, sizeof(main_path), "%s", dir);
  nspire_link_device = LINK_DEVICE_CABLE;
  nspire_link_speed = LINK_SPEED_COMPATIBLE;
  CHECK(nspire_config_save() == 0, "link speed: save");
  nspire_link_device = LINK_DEVICE_AUTO;
  nspire_link_speed = LINK_SPEED_AUTO;
  CHECK(nspire_config_load() == 0, "link speed: load");
  CHECK(nspire_link_device == LINK_DEVICE_CABLE && nspire_link_speed == LINK_SPEED_COMPATIBLE,
        "link speed: kept with the device: device %u, speed %u", (unsigned)nspire_link_device, (unsigned)nspire_link_speed);
  nspire_link_speed_reread();
  nspire_save_file(path, sizeof(path), "gpsp_fast.run");
  remove(path);
  CHECK(!nspire_link_fast_allowed() && strstr(nspire_link_fast_off_why(), "Compatible"), "link speed: Compatible: not tried");
  nspire_link_speed = LINK_SPEED_AUTO;
  CHECK(nspire_link_fast_allowed(), "link speed: Auto, no marker: tried");
  nspire_link_fast_begin();
  f = fopen(path, "rb");
  n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
  if (f)
    fclose(f);
  text[n] = 0;
  CHECK(!strncmp(text, "in use", 6), "link speed: the marker before a bring-up: \"%s\"", text);
  nspire_link_fast_end();
  CHECK(!(f = fopen(path, "rb")), "link speed: the marker gone at the link's end");
  if (f)
    fclose(f);
  /* A run that died with it in use, then the next run. */
  nspire_link_fast_begin();
  nspire_link_speed_reread();
  unit_osd_message[0] = 0;
  CHECK(!nspire_link_fast_allowed() && nspire_link_fast_crashed(), "link speed: off after a crash");
  CHECK(!strcmp(unit_osd_message, "Fast link turned off after a crash"), "link speed: the toast: \"%s\"", unit_osd_message);
  f = fopen(path, "rb");
  n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
  if (f)
    fclose(f);
  text[n] = 0;
  CHECK(!strncmp(text, "off after a crash", 17), "link speed: the marker keeps it off: \"%s\"", text);
  nspire_link_speed_reread();
  unit_osd_message[0] = 0;
  CHECK(!nspire_link_fast_allowed() && !unit_osd_message[0], "link speed: still off in the run after, no toast again");
  nspire_link_fast_begin();   /* (never called while off; if it were:) */
  nspire_link_fast_end();
  nspire_link_speed_reread();
  CHECK(nspire_link_fast_crashed(), "link speed: a begin and end while off don't turn it on");
  nspire_link_speed = LINK_SPEED_AUTO;
  nspire_link_speed_changed();
  CHECK(nspire_link_fast_allowed() && !(f = fopen(path, "rb")), "link speed: Auto picked again: on, the marker gone");
  if (f)
    fclose(f);
  /* F3 (2026-11-11): a marker that can't be written: no bring-up (it would
   * have no crash protection). A directory in its place stands in. */
  {
    extern const char *unit_fast_damage;
    char inside[700];
    nspire_link_speed_reread();
    CHECK(mkdir(path, 0700) == 0, "link speed: a directory where the marker goes");
    snprintf(inside, sizeof(inside), "%s/keep", path);
    f = fopen(inside, "w");
    if (f)
      fclose(f);
    CHECK(nspire_link_fast_begin() == 0, "link speed: a marker that can't be written: not tried");
    nspire_link_fast_end();
    remove(inside);
    rmdir(path);
    CHECK(nspire_link_fast_begin() == 1, "link speed: the marker written: tried");
    nspire_link_fast_end();
    /* A clean end that left the controller not as found: off, as after a
     * crash, said, toasted, and kept off in the next run. */
    unit_fast_damage = "the async schedule would not stop";
    unit_osd_message[0] = 0;
    CHECK(nspire_link_fast_begin() == 1, "link speed: damage: tried");
    nspire_link_fast_end();
    unit_fast_damage = NULL;
    f = fopen(path, "rb");
    n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
    if (f)
      fclose(f);
    text[n] = 0;
    CHECK(!strncmp(text, "off: the controller was not given back as found: the async schedule would not stop;",
                   strlen("off: the controller was not given back as found: the async schedule would not stop;")),
          "link speed: not given back as found: the marker says so: \"%s\"", text);
    CHECK(!nspire_link_fast_allowed() && nspire_link_fast_crashed(), "link speed: not given back as found: off now");
    CHECK(!strcmp(unit_osd_message, "Fast link turned off: USB not given back"), "link speed: damage toast: \"%s\"",
          unit_osd_message);
    nspire_link_speed_reread();
    CHECK(!nspire_link_fast_allowed(), "link speed: not given back as found: still off in the next run");
    nspire_link_speed = LINK_SPEED_AUTO;
    nspire_link_speed_changed();
    CHECK(nspire_link_fast_allowed() && !(f = fopen(path, "rb")), "link speed: after damage, Auto again: on");
    if (f)
      fclose(f);
  }
  nspire_link_device = LINK_DEVICE_AUTO;
}

static void test_link_fe(void)
{
  static const u16 theirs[5] = { 0x4FFF, 1, 0x1234, 0x5678, 0xABCD };
  static const u16 ours[6] = { 0x4FFF, 0x4FFF, 1, 0x1111, 0x2222, 0x3333 };
  static const u8 head[8] = { 2, 1, 16, 0, 0, 0, 0, 0 };   /* frame 0's, its end */
  u8 *body = unit_cable_in + 8;
  unsigned i;
  int ok;

  link_mode_for("BE8E", SERIAL_MODE_AUTO, LINK_DEVICE_AUTO);
  unit_cable_on = 1;
  nspire_link_connect();
  hello_from(SERIAL_MODE_SERIAL_FE);
  nspire_link_frame_begin();
  CHECK(nspire_link_state() == LINK_LINKED && netplay_client_id == 1, "linked as player 2: %s",
        nspire_link_status());
  write_ioreg(REG_RCNT, 0);
  write_ioreg(REG_SIOCNT, 0x6003);   /* multiplayer, 115200 bps, interrupt */
  serialfe_reset();

  /* Frame 0's message: one packet (MFE1, the five words). */
  memset(unit_cable_in, 0, sizeof(unit_cable_in));
  memcpy(unit_cable_in, head, 8);
  body[0] = 14;
  memcpy(body + 2, "MFE1", 4);
  for (i = 0; i < 5; i++)
  {
    body[6 + 2 * i] = theirs[i] >> 8;
    body[7 + 2 * i] = theirs[i] & 0xFF;
  }
  unit_cable_in_len = 8 + 16;
  for (i = 0; i < 2; i++)
  {
    nspire_link_frame_end();
    nspire_link_frame_begin();   /* frame 2: the other's frame 0 comes in */
  }

  ok = 1;
  for (i = 0; i < 6; i++)
  {
    write_ioreg(REG_SIOMLT_SEND, ours[i]);
    ok &= serialfe_update(300000) == true;
    ok &= read_ioreg(REG_SIOMULTI0) == (i < 5 ? theirs[i] : 0x7FFF) && read_ioreg(REG_SIOMULTI1) == ours[i] &&
          read_ioreg(REG_SIOMULTI2) == 0xFFFF && read_ioreg(REG_SIOMULTI3) == 0xFFFF;
  }
  CHECK(ok, "the other's packet, then 0x7FFF, at the pretend parent's transfers");
  CHECK(serialfe_update(1000) == false, "no transfer before its time");

  nspire_link_frame_end();
  ok = unit_cable_out_len == 8 + 2 + 4 + 10 && !memcmp(unit_cable_out + 10, "MFE1", 4);
  for (i = 0; ok && i < 5; i++)
    ok = unit_cable_out[14 + 2 * i] == ours[i + 1] >> 8 && unit_cable_out[15 + 2 * i] == (ours[i + 1] & 0xFF);
  CHECK(ok, "this GBA's packet goes in its frame's message (%d bytes)", unit_cable_out_len);

  nspire_link_disconnect();
  unit_cable_on = 0;
  write_ioreg(REG_SIOMLT_SEND, 0x4FFF);
  serialfe_master_send();
  CHECK(read_ioreg(REG_SIOMULTI0) == 0x4FFF && read_ioreg(REG_SIOMULTI1) == 0xFFFF,
        "unlinked, nothing's at the other end");
  serialfe_reset();
  write_ioreg(REG_SIOCNT, 0);
  serial_mode = SERIAL_MODE_AUTO;
}

/* The packet the link last sent at a frame's end (nspire_link.c's
 * MSG_FRAME: its first packet), into p; its length. */
static unsigned sent_packet(u8 *p)
{
  unsigned n;

  serialmsio_frame_update();
  nspire_link_frame_end();
  if (unit_cable_out_len < 10 || unit_cable_out[0] != 2)
    return 0;
  n = unit_cable_out[8] | (unit_cable_out[9] << 8);
  if (10 + n > unit_cable_out_len)
    return 0;
  memcpy(p, unit_cable_out + 10, n);
  return n;
}

/* Player 2's side of serial_msio.c, an event every 100 cycles from the
 * frame's start (serialmsio_frame_update) to cycle end: its interrupts (the
 * game giving A000 + n at the nth) into at[], their words checked against
 * words[]; how many there were from first on. */
static unsigned msio_child(const u16 *words, u32 *at, unsigned first, u32 end)
{
  u32 start = cpu_ticks, i;
  unsigned n = first;

  for (i = 100; i <= end; i += 100)
  {
    cpu_ticks += 100;
    if (update_serial(100))
    {
      CHECK(read_ioreg(REG_SIOMULTI0) == words[n] && read_ioreg(REG_SIOMULTI1) == 0xA000 + n,
            "player 2's transfer %u: %04x %04x", n, read_ioreg(REG_SIOMULTI0), read_ioreg(REG_SIOMULTI1));
      at[n++] = cpu_ticks - start;
      write_ioreg(REG_SIOMLT_SEND, 0xA000 + n);
    }
  }
  return n;
}

/* Player 2's answers (n words, first on) coming to player 1's side of
 * serial_msio.c. */
static void msio_answers(u16 first, unsigned n)
{
  u8 p[8 + 2 * 8];
  unsigned i;

  memcpy(p, "MSI1", 4);
  p[4] = n;
  p[5] = 0;
  p[6] = 5242 & 0xFF;
  p[7] = 5242 >> 8;
  for (i = 0; i < n; i++)
  {
    p[8 + 2 * i] = first + i;
    p[9 + 2 * i] = (first + i) >> 8;
  }
  serialmsio_net_receive(p, 8 + 2 * n, 1);
}

/* Player 1's game makes n transfers, 14000 cycles apart: what it heard from
 * player 2 at each, into heard. */
static void msio_parent(unsigned n, u16 *heard)
{
  unsigned i;

  for (i = 0; i < n; i++)
  {
    write_ioreg(REG_SIOMLT_SEND, 0x1000 + i);
    cpu_ticks += 14000;
    serialmsio_master_send(5242);
    heard[i] = read_ioreg(REG_SIOMULTI1);
  }
}

/* Golden Sun's stand-in (serial_msio.c), playing player 1 and then player
 * 2. Player 1's frame's transfers go out at its end, with their cycles in
 * it. Player 2's game gets them at those cycles of its frame, each with its
 * interrupt a transfer's time after (the next word only after that, even
 * if it's due at the same event), and gives its own, which go back once
 * the last is played; a frame's words not played by its end go at once.
 * Player 1's transfers take those answers in order, one each, from the
 * frame they come at on; two coming at one frame's start, the first came
 * late (dropped); none after four frames without transfers, nor once
 * unlinked. */
static void test_link_msio(void)
{
  /* The last one starts as the one before ends: 42000 + 5242. */
  static const u16 words[6] = { 0xFEFE, 0x0001, 0x7AA6, 0x8000, 0x1111, 0x2222 };
  static const u32 cycles[4] = { 14000, 28000, 42000, 47242 };
  u8 batch[512], answers[512];
  unsigned batch_len, answers_len, i, n;
  u32 at[6];
  u16 heard[6];

  link_mode_for("AGSE", SERIAL_MODE_AUTO, LINK_DEVICE_AUTO);
  CHECK(serial_mode == SERIAL_MODE_SERIAL_MSIO, "Golden Sun's cable: the stand-in (%d)", serial_mode);
  unit_cable_on = 1;
  nspire_link_connect();
  hello_from(SERIAL_MODE_SERIAL_MSIO);
  nspire_link_frame_begin();
  CHECK(!strcmp(nspire_link_status(), "Linked: player 2 (Link cable, quick)"), "linked: %s",
        nspire_link_status());
  write_ioreg(REG_RCNT, 0);
  write_ioreg(REG_SIOCNT, 0x6003);   /* multiplayer, 115200 bps, interrupt */

  /* Player 1: nothing from player 2 yet (0xFFFF); the frame's words go at
   * its end. */
  netplay_client_id = 0;
  serialmsio_reset();
  for (i = 0; i < 4; i++)
  {
    write_ioreg(REG_SIOMLT_SEND, words[i]);
    cpu_ticks += cycles[i] - (i ? cycles[i - 1] : 0);
    serialmsio_master_send(5242);
    CHECK(read_ioreg(REG_SIOMULTI0) == words[i] && read_ioreg(REG_SIOMULTI1) == 0xFFFF,
          "player 1's transfer %u: %04x %04x", i, read_ioreg(REG_SIOMULTI0), read_ioreg(REG_SIOMULTI1));
  }
  batch_len = sent_packet(batch);
  CHECK(batch_len == 8 + 6 * 4 && !memcmp(batch, "MSI1", 4) && batch[4] == 4 &&
        (batch[6] | (batch[7] << 8)) == 5242 && batch[14] == 0x00 && batch[15] == 0x80 &&
        (batch[16] | (batch[17] << 8)) == 14000 && (batch[28] | (batch[29] << 8)) == (47242 & 0xFFFF) &&
        batch[30] == 47242 >> 16, "player 1's frame: %u bytes", batch_len);

  /* Player 2: the words at player 1's cycles, the interrupts 5242 after
   * (at the next event); the last waits for the third's interrupt. */
  netplay_client_id = 1;
  serialmsio_reset();
  serialmsio_net_receive(batch, batch_len, 0);
  write_ioreg(REG_SIOMLT_SEND, 0xA000);
  n = msio_child(words, at, 0, 70000);
  CHECK(n == 4 && at[0] == 19300 && at[1] == 33300 && at[2] == 47300 && at[3] == 52600,
        "player 2's interrupts: %u, at %u %u %u %u", n, at[0], at[1], at[2], at[3]);
  answers_len = sent_packet(answers);
  CHECK(answers_len == 8 + 2 * 4 && !memcmp(answers, "MSI1", 4) && answers[8] == 0x00 && answers[9] == 0xA0 &&
        answers[14] == 0x03, "player 2's answers: %u bytes", answers_len);

  /* A frame whose second word isn't played by the frame's end (as after
   * a state's loaded): at the next frame's first event, and then the
   * answers go. */
  batch[4] = 2;
  memcpy(batch + 8, "\x11\x11\x22\x22", 4);
  memcpy(batch + 12, "\x10\x27\x00\x00\xa0\x86\x01\x00", 8);   /* 10000, 100000 */
  serialmsio_net_receive(batch, 8 + 6 * 2, 0);
  n = msio_child(words, at, 4, 80000);
  CHECK(n == 5 && at[4] == 15300, "the frame's first: %u interrupts, at %u", n, at[4]);
  CHECK(sent_packet(answers) == 0 || memcmp(answers, "MSI1", 4), "nothing back before the frame's played");
  n = msio_child(words, at, 5, 10000);
  CHECK(n == 6 && at[5] == 5400, "the rest at the next frame's start: %u interrupts, at %u", n, at[5]);
  CHECK(sent_packet(answers) == 8 + 2 * 2 && answers[8] == 0x04 && answers[10] == 0x05,
        "then its answers: %02x %02x", answers[8], answers[10]);

  /* Player 1 again: the answers one a transfer, the ones a frame doesn't
   * take in the next's first, then nothing. */
  netplay_client_id = 0;
  serialmsio_reset();
  msio_answers(0xA000, 4);
  msio_parent(3, heard);
  sent_packet(batch);
  msio_answers(0xA004, 1);
  msio_parent(3, heard + 3);
  CHECK(heard[0] == 0xA000 && heard[2] == 0xA002 && heard[3] == 0xA003 && heard[4] == 0xA004 &&
        heard[5] == 0xFFFF, "player 2's answers: %04x %04x %04x %04x %04x %04x", heard[0], heard[1],
        heard[2], heard[3], heard[4], heard[5]);

  /* Two at one frame's start: only the second's. */
  sent_packet(batch);
  msio_answers(0xB000, 2);
  msio_answers(0xC000, 2);
  msio_parent(3, heard);
  CHECK(heard[0] == 0xC000 && heard[1] == 0xC001 && heard[2] == 0xFFFF,
        "two at once, the later: %04x %04x %04x", heard[0], heard[1], heard[2]);

  /* What's left after three frames without transfers is still heard, not
   * after four. */
  sent_packet(batch);
  msio_answers(0xD000, 2);
  for (i = 0; i < 3; i++)
    sent_packet(batch);
  msio_parent(1, heard);
  for (i = 0; i < 1 + 4; i++)
    sent_packet(batch);
  msio_parent(1, heard + 1);
  CHECK(heard[0] == 0xD000 && heard[1] == 0xFFFF, "after the game's stopped: %04x %04x", heard[0], heard[1]);

  /* Unlinked: the answers that came are gone. */
  sent_packet(batch);
  msio_answers(0xE000, 2);
  nspire_link_disconnect();
  msio_parent(1, heard);
  CHECK(heard[0] == 0xFFFF, "unlinked, nothing: %04x", heard[0]);

  unit_cable_on = 0;
  write_ioreg(REG_SIOCNT, 0);
  serialmsio_reset();
}

/* Turbo (2026-11-12, the binding model): the press pattern frame by
 * frame at the speed set, a tap, buttons on their own phases, the D-pad,
 * a plain key and a turbo key for one button held together and apart;
 * every action bound to a calculator key and to a controller button
 * (nspire_bind.c, the Controls screen's model); the settings file keeps
 * the turbo bindings; old settings (the turbo choice and the Turbo key)
 * become the nearest bindings; damaged values never crash or lose a key. */
static void read_words(const char *path, u32 *words, u32 count)
{
  FILE *f = fopen(path, "rb");
  memset(words, 0, count * 4);
  if (f)
  {
    if (fread(words, 4, count, f) != count)
      words[0] = 0;
    fclose(f);
  }
}

static int turbo_presses(u32 speed_index, int frames, u32 bit)
{
  int f, presses = 0;
  u32 last = 0, b;
  nspire_turbo_speed = speed_index;
  nspire_turbo_buttons(0, 0);   /* let go: the count starts over */
  for (f = 0; f < frames; f++)
  {
    b = nspire_turbo_buttons(0, bit);
    presses += (b & bit) && !(last & bit);
    last = b;
  }
  return presses;
}

/* Frames of turbo 'bit' (and plain 'plain') as '1' down, '0' up. */
static void turbo_pattern(char *got, int frames, u32 plain, u32 bit)
{
  int f;
  for (f = 0; f < frames; f++)
    got[f] = (nspire_turbo_buttons(plain, bit) & bit) ? '1' : '0';
  got[frames] = 0;
}

static int bindings_valid(key_binding k[NUM_ACTIONS][KEY_SLOTS])
{
  int i, s;
  for (i = 0; i < NUM_ACTIONS; i++)
    for (s = 0; s < KEY_SLOTS; s++)
      if ((k[i][s].key && !key_is_bindable(k[i][s].key)) || (k[i][s].with && !key_is_bindable(k[i][s].with)) ||
          (!k[i][s].key && k[i][s].with) || (k[i][s].with && k[i][s].with == k[i][s].key))
        return 0;
  return 1;
}

static int has(int action, unsigned key, unsigned with)
{
  int s;
  for (s = 0; s < KEY_SLOTS; s++)
    if (key_bindings[action][s].key == key && key_bindings[action][s].with == with)
      return 1;
  return 0;
}

static void test_turbo_engine(void)
{
  static const char want30[] = "10101010", want15[] = "11001100", want10[] = "111000111000";
  char got[130];
  int speed, ok, i;

  nspire_turbo_speed = 29;   /* 30 a second */
  nspire_turbo_buttons(0, 0);
  turbo_pattern(got, 8, 0, 0x001);
  CHECK(!strcmp(got, want30), "30 a second: %s", got);
  nspire_turbo_speed = 14;
  nspire_turbo_buttons(0, 0);
  turbo_pattern(got, 8, 0, 0x001);
  CHECK(!strcmp(got, want15), "15 a second: %s", got);
  nspire_turbo_speed = 9;
  nspire_turbo_buttons(0, 0);
  turbo_pattern(got, 12, 0, 0x001);
  CHECK(!strcmp(got, want10), "10 a second: %s", got);
  nspire_turbo_speed = 0;   /* 1 a second: 30 down, 30 up */
  nspire_turbo_buttons(0, 0);
  turbo_pattern(got, 120, 0, 0x001);
  for (ok = 1, i = 0; i < 120; i++)
    ok &= (got[i] == '1') == ((i % 60) < 30);
  CHECK(ok, "1 a second: half a second down, half up");
  ok = 1;
  for (speed = 0; speed < TURBO_SPEEDS; speed++)
    for (i = 0; i < GBA_BUTTONS; i++)
      if (turbo_presses((u32)speed, 600, gba_button_bits[i]) != (speed + 1) * 10)
      {
        CHECK(0, "speed %d, button %d: %d presses in 10 s", speed + 1, i,
              turbo_presses((u32)speed, 600, gba_button_bits[i]));
        ok = 0;
      }
  CHECK(ok, "every speed 1-30 gives that many presses a second, on each of the ten buttons");

  /* a tap is a press; nothing turbo changes nothing; each button its own phase */
  nspire_turbo_speed = 29;
  nspire_turbo_buttons(0, 0);
  CHECK(nspire_turbo_buttons(0, 0x001) == 0x001 && nspire_turbo_buttons(0, 0) == 0, "a tap");
  CHECK(nspire_turbo_buttons(0x3FF, 0) == 0x3FF && nspire_turbo_buttons(0x3FF, 0) == 0x3FF, "no turbo: as held");
  nspire_turbo_buttons(0, 0);
  CHECK(nspire_turbo_buttons(0, 0x001) == 0x001, "A down");
  CHECK(nspire_turbo_buttons(0, 0x003) == 0x002, "B starts down, A up");
  CHECK(nspire_turbo_buttons(0, 0x003) == 0x001, "A down, B up");
  nspire_turbo_buttons(0, 0);
  /* the D-pad, Start and Select repeat too (they couldn't before) */
  CHECK(nspire_turbo_buttons(0, 0x0FC) == 0x0FC && nspire_turbo_buttons(0, 0x0FC) == 0,
        "the D-pad, Start and Select repeat");
  nspire_turbo_buttons(0, 0);

  /* A on one key, turbo A on another: held apart, each does its own; together A stays down */
  nspire_turbo_speed = 14;
  nspire_turbo_buttons(0, 0);
  turbo_pattern(got, 8, 0x001, 0);
  CHECK(!strcmp(got, "00000000"), "pattern of no turbo bit is 0: %s", got);
  for (ok = 1, i = 0; i < 8; i++)
    ok &= nspire_turbo_buttons(0x001, 0) == 0x001;
  CHECK(ok, "the plain key alone: A held down");
  nspire_turbo_buttons(0, 0);
  turbo_pattern(got, 8, 0, 0x001);
  CHECK(!strcmp(got, want15), "the turbo key alone: A repeats: %s", got);
  nspire_turbo_buttons(0, 0);
  turbo_pattern(got, 8, 0x001, 0x001);
  CHECK(!strcmp(got, "11111111"), "both held: A down: %s", got);
  /* the turbo key held on: the plain one let go, the count went on under it */
  turbo_pattern(got, 4, 0, 0x001);
  CHECK(!strcmp(got, "1100"), "plain let go after 8 frames, turbo held: on in its phase: %s", got);
  /* plain A and turbo B: A stays, B repeats */
  nspire_turbo_buttons(0, 0);
  CHECK(nspire_turbo_buttons(0x001, 0x002) == 0x003 && nspire_turbo_buttons(0x001, 0x002) == 0x003 &&
        nspire_turbo_buttons(0x001, 0x002) == 0x001, "plain A and turbo B: A down, B repeats");
  nspire_turbo_buttons(0, 0);
  nspire_turbo_speed = TURBO_SPEED_DEFAULT;
}

/* Every action on a calculator key and on a controller button, through
 * the Controls screen's model. */
static void test_bind_model(void)
{
  static const unsigned char calc_keys[] =
  {
    KEY_A, KEY_C, KEY_F, KEY_L, KEY_Q, KEY_S, KEY_U, KEY_0, KEY_2, KEY_4, KEY_5, KEY_6, KEY_8, KEY_VAR,
    KEY_RPAREN, KEY_LPAREN, KEY_MINUS, KEY_DEL, KEY_SCRATCH, KEY_PLUS, KEY_DOC, KEY_MENU, KEY_ESC, KEY_TAB,
    KEY_EQUALS, KEY_SHIFT, KEY_CTRL, 5, 6, 7, 17, 18, 19
  };
  static const unsigned char pad_keys[] =
  {
    KEY_PAD_B, KEY_PAD_A, KEY_PAD_Y, KEY_PAD_X, KEY_PAD_L1, KEY_PAD_R1, KEY_PAD_L2, KEY_PAD_R2,
    KEY_PAD_SELECT, KEY_PAD_START, KEY_PAD_HOME, KEY_PAD_L3, KEY_PAD_R3, KEY_PAD_AUX
  };
  key_binding b, shown[KEY_SLOTS];
  int a, ok_calc = 1, ok_pad = 1, ok_name = 1, n;

  CHECK(sizeof(calc_keys) >= NUM_ACTIONS, "enough keys for the test");
  for (a = 0; a < NUM_ACTIONS; a++)
    ok_name &= bind_row_of(a) >= 0 && strcmp(bind_action_name(a), "?") != 0;
  CHECK(ok_name, "every action has a row and a name");
  CHECK(!strcmp(bind_action_name(ACTION_TURBO_A), "Turbo A") && !strcmp(bind_action_name(ACTION_A), "A") &&
        !strcmp(bind_action_name(ACTION_TURBO_UP), "Turbo Up") && bind_row_of(ACTION_TURBO_A) == bind_row_of(ACTION_A),
        "turbo A: its own name, on A's row");

  /* each action its own calculator key: all 32 at once */
  memset(key_bindings, 0, sizeof(key_binding) * NUM_ACTIONS * KEY_SLOTS);
  for (a = 0; a < NUM_ACTIONS; a++)
  {
    b.key = calc_keys[a];
    b.with = 0;
    CHECK(key_is_bindable(b.key), "key %u bindable", b.key);
    CHECK(bind_owner(b, 0, a) < 0, "key %u free for action %d", b.key, a);
    bind_set(a, b, 0);
  }
  for (a = 0; a < NUM_ACTIONS; a++)
  {
    b.key = calc_keys[a];
    n = bind_shown(a, 0, shown);
    ok_calc &= n == 1 && shown[0].key == calc_keys[a] && bind_owner(b, 0, -1) == a;
  }
  CHECK(ok_calc, "every action (A ... Select, the hotkeys, Turbo Up ... Turbo Select) on a calculator key");

  /* and on a controller button: 14 buttons, so in rounds of 14 */
  for (a = 0; a < NUM_ACTIONS; a++)
  {
    int round_first = a / 14 * 14, who;
    if (a == round_first)
      for (n = 0; n < round_first; n++)
        bind_clear(n, 1);
    b.key = pad_keys[a - round_first];
    b.with = 0;
    who = bind_owner(b, 1, a);
    /* free, or shown only as a plain button's default job */
    CHECK(who < 0 || (!IS_TURBO(who) && !has(who, b.key, 0)), "pad key %u for action %d: %d", b.key, a, who);
    bind_take(a, b);
    bind_set(a, b, 1);
    n = bind_shown(a, 1, shown);
    ok_pad &= n == 1 && shown[0].key == b.key && has(a, b.key, 0) && has(a, calc_keys[a], 0);
  }
  CHECK(ok_pad, "every action on a controller button too, beside its calculator key");

  /* a key for A and another for turbo A side by side; taking one asks */
  keys_set_defaults(key_bindings);
  b.key = KEY_EQUALS;
  b.with = 0;
  CHECK(bind_owner(b, 0, ACTION_TURBO_A) < 0, "= alone is free (it's held with keys for turbo)");
  bind_set(ACTION_TURBO_A, b, 0);
  CHECK(has(ACTION_A, KEY_DEL, 0) && has(ACTION_TURBO_A, KEY_EQUALS, 0) && !has(ACTION_TURBO_A, KEY_DEL, KEY_EQUALS),
        "A on del, turbo A on = (it took the place of =+del)");
  b.key = KEY_DEL;
  CHECK(bind_owner(b, 0, ACTION_TURBO_B) == ACTION_A, "del is A's: asked about");
  bind_take(ACTION_TURBO_B, b);
  bind_set(ACTION_TURBO_B, b, 0);
  CHECK(!has(ACTION_A, KEY_DEL, 0) && has(ACTION_TURBO_B, KEY_DEL, 0), "del taken over by turbo B");
  /* a controller's turbo D-pad: a button on Turbo Up */
  b.key = KEY_PAD_L3;
  bind_set(ACTION_TURBO_UP, b, 1);
  CHECK(has(ACTION_TURBO_UP, KEY_PAD_L3, 0) && bind_owner(b, 1, -1) == ACTION_TURBO_UP, "L3 is turbo up");
  bind_clear(ACTION_TURBO_UP, 1);
  CHECK(!has(ACTION_TURBO_UP, KEY_PAD_L3, 0), "del: taken off");
  /* in the controller's view a plain button shows its default job, its turbo none */
  keys_set_defaults(key_bindings);
  CHECK(bind_shown(ACTION_A, 1, shown) == 1 && shown[0].key == KEY_PAD_A && bind_shown(ACTION_TURBO_A, 1, shown) == 0,
        "pad view: A is the right button by default, turbo A has none");
  b.key = KEY_PAD_A;
  CHECK(bind_owner(b, 1, ACTION_TURBO_A) == ACTION_A, "the right button is A's: asked about");
  keys_set_defaults(key_bindings);

  /* R10b: each family's own names for the same positions (the defaults are
   * by position on every pad: A the right face button, B the bottom one) */
  {
    static const struct { int family; const char *b, *a, *l1, *select, *start, *home, *aux; } names[] = {
      { PAD_FAMILY_NONE, "Bottom", "Right", "L1", "Select", "Start", "Home", "Aux" },
      { PAD_FAMILY_XBOX360, "A", "B", "LB", "Back", "Start", "Guide", "Aux" },
      { PAD_FAMILY_XBOX, "A", "B", "LB", "View", "Menu", "Xbox", "Share" },
      { PAD_FAMILY_DS4, "Cross", "Circle", "L1", "Share", "Options", "PS", "Touchpad" },
      { PAD_FAMILY_DUALSENSE, "Cross", "Circle", "L1", "Create", "Options", "PS", "Touchpad" },
      { PAD_FAMILY_SWITCH, "B", "A", "L", "Minus", "Plus", "Home", "Capture" },
    };
    char text[40];
    unsigned i;
    int ok = 1;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
      ok &= !strcmp(bind_pad_name(KEY_PAD_B, names[i].family), names[i].b) &&
            !strcmp(bind_pad_name(KEY_PAD_A, names[i].family), names[i].a) &&
            !strcmp(bind_pad_name(KEY_PAD_L1, names[i].family), names[i].l1) &&
            !strcmp(bind_pad_name(KEY_PAD_SELECT, names[i].family), names[i].select) &&
            !strcmp(bind_pad_name(KEY_PAD_START, names[i].family), names[i].start) &&
            !strcmp(bind_pad_name(KEY_PAD_HOME, names[i].family), names[i].home) &&
            !strcmp(bind_pad_name(KEY_PAD_AUX, names[i].family), names[i].aux);
    CHECK(ok, "every family's button names");
    CHECK(!strcmp(bind_pad_name(KEY_PAD_X, PAD_FAMILY_SWITCH), "X") && !strcmp(bind_pad_name(KEY_PAD_Y, PAD_FAMILY_DS4), "Square") &&
          !strcmp(bind_pad_name(KEY_PAD_R2, PAD_FAMILY_SWITCH), "ZR") && !strcmp(bind_pad_name(KEY_PAD_L3, PAD_FAMILY_XBOX), "LS"),
          "the top and left ones, the triggers, the sticks");
    CHECK(!strcmp(bind_pad_name(KEY_PAD_A, 99), "Right"), "an unknown family: by position");
    b.key = KEY_PAD_A;
    b.with = KEY_PAD_L1;
    bind_pad_family = PAD_FAMILY_DUALSENSE;
    bind_text(b, 1, text, sizeof(text));
    CHECK(!strcmp(text, "L1+Circle"), "bind_text in the pad's names: %s", text);
    /* layout=labels: the bottom and right buttons swapped, so the button
     * behind Pad A is the bottom one */
    bind_pad_labels = 1;
    b.with = 0;
    bind_text(b, 1, text, sizeof(text));
    CHECK(!strcmp(text, "Cross") && !strcmp(bind_pad_side(KEY_PAD_A), "bottom") && !strcmp(bind_pad_side(KEY_PAD_X), "left") &&
          !strcmp(bind_pad_name(KEY_PAD_L1, PAD_FAMILY_XBOX), "LB"), "labels: the names of the buttons that send them (%s)", text);
    bind_pad_labels = 0;
    bind_pad_family = PAD_FAMILY_NONE;
    CHECK(!strcmp(bind_pad_side(KEY_PAD_A), "right") && bind_pad_side(KEY_PAD_L1) == NULL, "sides by position");
  }
}

static void test_turbo(void)
{
  char path[600], rom[600], game_cfg[600];
  u32 o[82], speed;
  key_binding old_key[KEY_SLOTS];
  int i, made;
  FILE *f;

  test_turbo_engine();
  test_bind_model();

  /* the defaults: turbo A, B, L, R are = held with their key (the old Turbo key) */
  keys_set_defaults(key_bindings);
  CHECK(has(ACTION_TURBO_A, KEY_DEL, KEY_EQUALS) && has(ACTION_TURBO_B, KEY_VAR, KEY_EQUALS) &&
        has(ACTION_TURBO_L, KEY_SCRATCH, KEY_EQUALS) && has(ACTION_TURBO_R, KEY_DOC, KEY_EQUALS) &&
        !key_bindings[ACTION_TURBO_UP][0].key && !key_bindings[ACTION_TURBO_START][0].key,
        "defaults: turbo A B L R on = + their key, the others none");

  /* the old turbo turned into bindings (keys_turbo_from_old) */
  memset(old_key, 0, sizeof(old_key));
  old_key[0].key = KEY_EQUALS;
  keys_set_defaults(key_bindings);
  made = keys_turbo_from_old(key_bindings, 0, old_key);
  CHECK(made == 4 && has(ACTION_TURBO_A, KEY_DEL, KEY_EQUALS) && has(ACTION_A, KEY_DEL, 0),
        "choice off, Turbo key =: turbo A is =+del, A still del (%d)", made);
  keys_set_defaults(key_bindings);
  key_bindings[ACTION_A][1].key = KEY_PAD_Y;   /* A also on a controller button */
  old_key[1].key = KEY_PAD_X;                   /* and a second Turbo key, on the controller */
  made = keys_turbo_from_old(key_bindings, keys_old_turbo_mask(1), old_key);   /* "A B L R" */
  CHECK(has(ACTION_TURBO_A, KEY_DEL, 0) && has(ACTION_TURBO_A, KEY_PAD_Y, 0) && !key_bindings[ACTION_A][0].key &&
        !key_bindings[ACTION_A][1].key && has(ACTION_TURBO_R, KEY_DOC, 0) && !key_bindings[ACTION_R][0].key &&
        has(ACTION_START, KEY_SHIFT, 0) && made == 8 && has(ACTION_TURBO_B, KEY_PAD_B, 0) &&
        has(ACTION_TURBO_R, KEY_PAD_R1, 0) && !has(ACTION_TURBO_A, KEY_PAD_A, 0),
        "choice A B L R: their keys moved to their turbo, the controller's default buttons too where "
        "they had none of their own (%d)", made);
  keys_set_defaults(key_bindings);
  key_bindings[ACTION_B][1].key = KEY_PAD_Y;
  made = keys_turbo_from_old(key_bindings, keys_old_turbo_mask(2), old_key);   /* "A" */
  CHECK(has(ACTION_TURBO_A, KEY_DEL, 0) && !key_bindings[ACTION_A][0].key && has(ACTION_TURBO_A, KEY_PAD_A, 0) &&
        has(ACTION_TURBO_B, KEY_VAR, KEY_EQUALS) && has(ACTION_TURBO_B, KEY_PAD_Y, KEY_PAD_X) &&
        has(ACTION_B, KEY_VAR, 0) && has(ACTION_B, KEY_PAD_Y, 0) && has(ACTION_TURBO_L, KEY_SCRATCH, KEY_EQUALS),
        "choice A: A's key moved; B gets = with var and the controller's X with Y (%d)", made);
  memset(old_key, 0, sizeof(old_key));
  old_key[0] = binding_unpack(KEY_7 | (KEY_SHIFT << 8));   /* a Turbo key that was a combination */
  keys_set_defaults(key_bindings);
  made = keys_turbo_from_old(key_bindings, 0, old_key);
  CHECK(made == 0 && bindings_valid(key_bindings), "a combination Turbo key: no three-key bindings");
  CHECK(keys_old_turbo_mask(0) == 0 && keys_old_turbo_mask(1) == 15 && keys_old_turbo_mask(5) == 8 &&
        keys_old_turbo_mask(7) == 12 && keys_old_turbo_mask(16 + 2) == 1, "old choices' buttons");
  keys_set_defaults(key_bindings);
  key_bindings[ACTION_TURBO_A][0] = binding_unpack(KEY_A);
  key_bindings[ACTION_TURBO_A][1] = binding_unpack(KEY_C);
  keys_move_turbo(key_bindings, ACTION_A, 1);
  CHECK(has(ACTION_TURBO_A, KEY_A, 0) && has(ACTION_TURBO_A, KEY_C, 0) && has(ACTION_A, KEY_DEL, 0),
        "moving to a full turbo: nothing lost");

  /* the settings file: 82 words, the turbo bindings in 72-81 */
  snprintf(main_path, sizeof(main_path), "%s", dir);
  nspire_save_file(path, sizeof(path), "gpsp.cfg.tns");
  keys_set_defaults(key_bindings);
  nspire_turbo_speed = 29;
  key_bindings[ACTION_TURBO_A][0] = binding_unpack(KEY_EQUALS);
  key_bindings[ACTION_TURBO_A][1] = binding_unpack(KEY_PAD_X);
  key_bindings[ACTION_TURBO_UP][0] = binding_unpack(KEY_7 | (KEY_SHIFT << 8));
  key_bindings[ACTION_TURBO_SELECT][1] = binding_unpack(KEY_PAD_AUX);
  CHECK(nspire_config_save() == 0 && file_size(path) == 82 * 4, "save: %ld bytes", file_size(path));
  read_words(path, o, 82);
  CHECK((o[70] >> 30) == 3 && ((o[70] >> 8) & 0xFF) == 29 && (o[70] & 0xF) == 0 && (o[69] & ~1u) == 0x5A000100u &&
        o[72 + 4] == (KEY_EQUALS | ((unsigned)KEY_PAD_X << 16)) && o[72] == (KEY_7 | (KEY_SHIFT << 8)),
        "word 70 %08x, 72 %08x, 76 %08x", o[70], o[72], o[76]);
  /* word 69: On-chip memory (nspire_sram.c) under its mark, with the
   * default-on mark (bit 8, final/speed5); on without them */
  nspire_sram_code = 1;
  CHECK(nspire_config_save() == 0, "save with On-chip memory on");
  read_words(path, o, 82);
  CHECK(o[69] == 0x5A000101u, "On-chip memory on: word 69 %08x", o[69]);
  nspire_sram_code = 0;
  CHECK(nspire_config_load() == 0 && nspire_sram_code == 1, "On-chip memory on, read back (%u)", nspire_sram_code);
  nspire_sram_code = 0;
  CHECK(nspire_config_save() == 0, "save with On-chip memory off");
  read_words(path, o, 82);
  CHECK(o[69] == 0x5A000100u, "On-chip memory off: word 69 %08x", o[69]);
  nspire_sram_code = 1;
  CHECK(nspire_config_load() == 0 && nspire_sram_code == 0, "On-chip memory off chosen, read back (%u)",
        nspire_sram_code);
  /* an older build's off (its default) is read as on, once */
  o[69] = 0x5A000000u;
  write_words(path, o, 82);
  CHECK(nspire_config_load() == 0 && nspire_sram_code == 1, "an older build's off reads as on (%u)",
        nspire_sram_code);
  o[69] = 0;
  write_words(path, o, 82);
  CHECK(nspire_config_load() == 0 && nspire_sram_code == 1, "no mark at all reads as on (%u)", nspire_sram_code);
  nspire_sram_code = 0;
  CHECK(nspire_config_save() == 0, "save with On-chip memory off");
  nspire_turbo_speed = 3;
  keys_set_defaults(key_bindings);
  CHECK(nspire_config_load() == 0, "load");
  CHECK(nspire_turbo_speed == 29 && has(ACTION_TURBO_A, KEY_EQUALS, 0) && has(ACTION_TURBO_A, KEY_PAD_X, 0) &&
        has(ACTION_TURBO_UP, KEY_7, KEY_SHIFT) && has(ACTION_TURBO_SELECT, KEY_PAD_AUX, 0) &&
        has(ACTION_TURBO_B, KEY_VAR, KEY_EQUALS) && has(ACTION_A, KEY_DEL, 0),
        "turbo bindings and speed kept (speed %u)", nspire_turbo_speed);
  CHECK(key_bindings[ACTION_DIMMER][0].key == KEY_MINUS && key_bindings[ACTION_BRIGHTER][0].key == KEY_PLUS,
        "brightness keys still in their words");

  /* old files: 72 words, words 69-70 the old turbo */
  memset(o, 0, sizeof(o));
  read_words(path, o, 72);
  o[69] = KEY_EQUALS;
  o[70] = 0x80000000u | (20 << 8) | 0;   /* choice off, 21 a second */
  write_words(path, o, 72);
  CHECK(nspire_config_load() == 0, "load a 72-word file");
  CHECK(nspire_turbo_speed == 20 && has(ACTION_TURBO_A, KEY_DEL, KEY_EQUALS) && has(ACTION_TURBO_R, KEY_DOC, KEY_EQUALS) &&
        !key_bindings[ACTION_TURBO_UP][0].key && has(ACTION_A, KEY_DEL, 0) && has(ACTION_R, KEY_DOC, 0),
        "old, off, Turbo key =: = held with each key is its turbo, the keys stay");
  o[70] = 0x80000000u | (14 << 8) | 1;   /* "A B L R" */
  o[69] = KEY_EQUALS | ((unsigned)KEY_PAD_X << 16);
  write_words(path, o, 72);
  CHECK(nspire_config_load() == 0, "load");
  CHECK(has(ACTION_TURBO_A, KEY_DEL, 0) && has(ACTION_TURBO_B, KEY_VAR, 0) && has(ACTION_TURBO_L, KEY_SCRATCH, 0) &&
        has(ACTION_TURBO_R, KEY_DOC, 0) && !key_bindings[ACTION_A][0].key && has(ACTION_TURBO_A, KEY_PAD_A, 0) &&
        has(ACTION_START, KEY_SHIFT, 0) && !key_bindings[ACTION_START][1].key,
        "old, on for A B L R: their keys are their turbo keys");
  CHECK(nspire_config_save() == 0, "saved again");
  read_words(path, o, 82);
  CHECK((o[70] & 0xF) == 1 && (o[70] >> 30) == 3 && o[72 + 4] == (KEY_DEL | ((unsigned)KEY_PAD_A << 16)) && o[38 + 4] == 0,
        "the old choice kept for games' files, the bindings as they are now: %08x", o[70]);
  CHECK(nspire_config_load() == 0 && has(ACTION_TURBO_A, KEY_DEL, 0) && !key_bindings[ACTION_A][0].key,
        "loaded again: not turned twice");
  /* a file from before turbo (words 69-70 0): = where it's free */
  read_words(path, o, 72);
  keys_set_defaults(key_bindings);
  for (i = 0; i < 20; i++)
    o[38 + i] = binding_pack(key_bindings[i][0]) | (binding_pack(key_bindings[i][1]) << 16);
  o[69] = o[70] = 0;
  write_words(path, o, 72);
  nspire_turbo_speed = 3;
  CHECK(nspire_config_load() == 0, "load an older file");
  CHECK(nspire_turbo_speed == TURBO_SPEED_DEFAULT && has(ACTION_TURBO_B, KEY_VAR, KEY_EQUALS),
        "before turbo: 15 a second, = with B's key is turbo B");
  o[38 + ACTION_A] = KEY_DEL | (KEY_EQUALS << 16);   /* = is A's second key */
  write_words(path, o, 72);
  CHECK(nspire_config_load() == 0, "load");
  CHECK(!key_bindings[ACTION_TURBO_A][0].key && !key_bindings[ACTION_TURBO_B][0].key &&
        has(ACTION_A, KEY_EQUALS, 0), "before turbo, = in use: no turbo, = still A's");

  /* damaged values: never a crash, never a key that isn't one, never a lost key */
  read_words(path, o, 72);
  o[69] = 0xFFFFFFFFu;
  o[70] = 0xBFFFFFFFu;   /* the old mark, every choice bit, speed 0xFF, no bindings mark */
  write_words(path, o, 72);
  CHECK(nspire_config_load() == 0 && nspire_turbo_speed < TURBO_SPEEDS && bindings_valid(key_bindings) &&
        has(ACTION_TURBO_B, KEY_VAR, 0) && has(ACTION_TURBO_R, KEY_DOC, 0) && has(ACTION_A, KEY_DEL, 0) &&
        !key_bindings[ACTION_TURBO_A][0].key,
        "old word 70 all ones: choice 15 (B L R), a Turbo key that is no key: speed %u", nspire_turbo_speed);
  o[70] = 0xFFFFFFFFu;   /* the bindings mark in a 72-word file: no words 72-81 to read */
  write_words(path, o, 72);
  CHECK(nspire_config_load() == 0 && bindings_valid(key_bindings) && has(ACTION_TURBO_B, KEY_VAR, 0) &&
        has(ACTION_A, KEY_DEL, 0), "bindings mark in a short file: turned from the old turbo");
  memset(o, 0xFF, sizeof(o));
  read_words(path, o, 72);
  for (i = 72; i < 82; i++)
    o[i] = 0xFFFFFFFFu - (u32)i;
  o[70] = 0xFFFFFFFFu;
  write_words(path, o, 82);
  CHECK(nspire_config_load() == 0 && bindings_valid(key_bindings), "82 words of junk turbo bindings: none bound");
  {
    /* random words, both sizes */
    u32 seed = 12345;
    int run, all = 1;
    for (run = 0; run < 400; run++)
    {
      int words = run & 1 ? 82 : 72;
      for (i = 0; i < words; i++)
      {
        seed = seed * 1103515245u + 12345u;
        o[i] = seed ^ (seed << 13);
      }
      o[37] = 0x334C4F51u;
      write_words(path, o, (u32)words);
      if (nspire_config_load() != 0 || !bindings_valid(key_bindings) || nspire_turbo_speed >= TURBO_SPEEDS)
        all = 0;
    }
    CHECK(all, "400 settings files of random words: loaded, every binding a real key");
  }
  remove(path);

  /* a game's own file from before (version 3): its turbo line and Turbo key */
  keys_set_defaults(key_bindings);
  nspire_turbo_speed = TURBO_SPEED_DEFAULT;
  nspire_config_save();
  snprintf(rom, sizeof(rom), "%s/Turbo.gba.tns", dir);
  nspire_save_file(game_cfg, sizeof(game_cfg), "Turbo.gba.settings.tns");
  f = fopen(game_cfg, "w");
  fprintf(f, "version=3\nown_settings=1\nturbo=4\nturbo_speed=5\nown_keys=1\nkey_a=%u\nkey_l=%u\nkey_turbo=%u\n"
          "key_turbo_2=0\nend=1\n", KEY_A, KEY_L, KEY_EQUALS);
  fclose(f);
  nspire_config_open_game(rom);
  CHECK(nspire_turbo_speed == 5 && has(ACTION_TURBO_A, KEY_A, KEY_EQUALS) && has(ACTION_A, KEY_A, 0) &&
        has(ACTION_TURBO_L, KEY_L, 0) && !key_bindings[ACTION_L][0].key && has(ACTION_TURBO_B, KEY_VAR, KEY_EQUALS),
        "game v3, choice L, Turbo key =: L's key is turbo L, = with A's is turbo A");
  nspire_config_close_game();
  /* (this build's version: 5 since final/pace, the pace rule's mark; the
   * turbo bindings came with 4) */
  CHECK(file_contains(game_cfg, "version=5") && file_contains(game_cfg, "key_turbo_l=") &&
        !file_contains(game_cfg, "turbo=4") && !file_contains(game_cfg, "key_turbo="),
        "written back as version 5: turbo bindings, no old lines");
  CHECK(has(ACTION_A, KEY_DEL, 0) && has(ACTION_TURBO_A, KEY_DEL, KEY_EQUALS), "all games' keys back");
  nspire_config_open_game(rom);
  CHECK(has(ACTION_TURBO_L, KEY_L, 0) && !key_bindings[ACTION_L][0].key && has(ACTION_TURBO_A, KEY_A, KEY_EQUALS),
        "version 4 read back as written");
  nspire_config_close_game();
  /* own settings with a turbo choice, keys for all games */
  f = fopen(game_cfg, "w");
  fprintf(f, "version=3\nown_settings=1\nturbo=2\nturbo_speed=29\nend=1\n");
  fclose(f);
  nspire_config_open_game(rom);
  CHECK(nspire_keys_for == 1 && has(ACTION_TURBO_A, KEY_DEL, 0) && !key_bindings[ACTION_A][0].key &&
        has(ACTION_B, KEY_VAR, 0), "game's own choice A, keys for all games: its own keys, A's key turbo");
  nspire_config_close_game();
  CHECK(has(ACTION_A, KEY_DEL, 0), "all games' A untouched");
  /* damaged game lines */
  f = fopen(game_cfg, "w");
  fprintf(f, "version=3\nown_settings=1\nturbo=4294967295\nturbo_speed=99999\nown_keys=1\nkey_turbo=65535\n"
          "key_turbo_2=999999\nkey_turbo_a=4294967295\nkey_turbo_=3\nkey_turbo_select_2=113\nend=1\n");
  fclose(f);
  nspire_config_open_game(rom);
  CHECK(bindings_valid(key_bindings) && nspire_turbo_speed < TURBO_SPEEDS, "damaged game lines: real keys only");
  nspire_config_close_game();
  remove(game_cfg);
  speed = TURBO_SPEED_DEFAULT;
  nspire_turbo_speed = speed;
  keys_set_defaults(key_bindings);
  nspire_config_save();
  nspire_save_file(path, sizeof(path), "gpsp.cfg.tns");
  remove(path);
}

/* ---- R6: what the other calculator sends, at random ---- */

/* Whatever comes over the cable (a broken peer, a cut-off stream, a bad
 * byte) must never crash this side: nspire_link.c's messages and the
 * stand-ins' packets (serial_proto.c, rfu.c, serial_fe.c, serial_msio.c),
 * fed random and truncated input. On the sanitizer build (tests/san.sh)
 * any read or write out of bounds, unaligned access or overflow stops it;
 * here the link only has to stay linked or end. */
extern int (*unit_cable_source)(void *buf, unsigned size);
void rfu_test_state(unsigned state);

static u32 fuzz_state = 0x6a09e667u;
static unsigned fuzz_msgs;

static u32 fuzz_rand(void)
{
  /* xorshift32 */
  fuzz_state ^= fuzz_state << 13;
  fuzz_state ^= fuzz_state >> 17;
  fuzz_state ^= fuzz_state << 5;
  return fuzz_state;
}

/* A stand-in's packet: its magic (sometimes another's, or none), then random
 * words; the length sometimes what that stand-in wants, often not. */
static unsigned fuzz_packet(u8 *p, unsigned room)
{
  static const char magic[6][5] = { "MPK1", "MAW1", "RFU1", "MFE1", "MSI1", "GPLK" };
  unsigned n, i;

  if (room < 4)
    return 0;
  switch (fuzz_rand() % 6)
  {
  case 0: n = 24; break;                            /* poke's */
  case 1: n = 8 + 2 * (fuzz_rand() % 300); break;   /* aw's, msio's */
  case 2: n = 12 + fuzz_rand() % 160; break;        /* rfu's */
  case 3: n = fuzz_rand() % 12; break;              /* short */
  default: n = fuzz_rand() % 600; break;
  }
  if (n > room)
    n = room;
  for (i = 0; i < n; i++)
    p[i] = (u8)fuzz_rand();
  if (n >= 4 && fuzz_rand() % 5)
    memcpy(p, magic[fuzz_rand() % 6], 4);
  if (n >= 12 && fuzz_rand() % 2)
  {
    /* rfu's command (big-endian 0-7), a slot's id 0 (an empty slot's), any length */
    p[4] = p[5] = p[6] = 0;
    p[7] = (u8)(fuzz_rand() % 9);
    p[9] = (u8)(fuzz_rand() % 4);
    p[10] = p[11] = 0;
  }
  if (n >= 8 && fuzz_rand() % 2)
  {
    /* aw's and msio's counts, sometimes right */
    p[5] = (u8)(fuzz_rand() % 3);
    p[7] = (u8)((n - 8) / 2);
    p[4] = (u8)((n - 8) / (fuzz_rand() % 2 ? 2 : 6));
  }
  return n;
}

static int fuzz_message(void *buf, unsigned size)
{
  static const u8 types[] = { 1, 2, 2, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 12, 200 };
  static u32 frame;
  u8 *m = buf;
  unsigned len, i, body;

  if (fuzz_rand() % 3 == 0 || size < 8)
    return 0;
  fuzz_msgs++;
  switch (fuzz_rand() % 8)
  {
  case 0: len = fuzz_rand() % 8; break;                   /* shorter than a head */
  case 1: len = fuzz_rand() % (size + 1); break;          /* anything that fits */
  case 2: len = size; break;
  default: len = 8 + fuzz_rand() % 96; break;
  }
  if (len > size)
    len = size;
  for (i = 0; i < len; i++)
    m[i] = (u8)fuzz_rand();
  if (len < 8)
    return (int)len;
  m[0] = types[fuzz_rand() % sizeof(types)];
  m[1] = fuzz_rand() % 2 ? 1 : (u8)fuzz_rand();   /* FRAME_END mostly */
  body = len - 8;
  if (fuzz_rand() % 4)
  {
    m[2] = (u8)body;   /* the length right (else what was random: truncated or not) */
    m[3] = (u8)(body >> 8);
  }
  /* The other's frame: mostly the next, sometimes anything. */
  frame += fuzz_rand() % 3 == 0;
  {
    u32 f = fuzz_rand() % 8 ? frame + fuzz_rand() % 4 : fuzz_rand();
    m[4] = (u8)f, m[5] = (u8)(f >> 8), m[6] = (u8)(f >> 16), m[7] = (u8)(f >> 24);
  }
  if (m[0] == 2 && fuzz_rand() % 4)
  {
    /* A frame's packets: each its length, then it (the last one cut off sometimes). */
    unsigned at = 8;
    while (at + 2 < len && fuzz_rand() % 4)
    {
      unsigned n = fuzz_packet(m + at + 2, len - at - 2);
      if (fuzz_rand() % 8 == 0)
        n += fuzz_rand() % 64;   /* says more than there is */
      m[at] = (u8)n;
      m[at + 1] = (u8)(n >> 8);
      at += 2 + n;
    }
  }
  else if ((m[0] == 1 || m[0] == 8) && len >= 24 && fuzz_rand() % 2)
  {
    memcpy(m + 8, "GPLK", 4);   /* a hello that gets past its magic */
    m[12] = 1, m[13] = 0;
  }
  return (int)len;
}

static void test_link_fuzz(void)
{
  static const struct
  {
    const char *code;
    int game, mode;
    uint32_t device;
  } cases[] =
  {
    { "BPEE", SERIAL_MODE_RFU, SERIAL_MODE_SERIAL_POKE, LINK_DEVICE_CABLE },
    { "AWRE", SERIAL_MODE_SERIAL_AW1, SERIAL_MODE_SERIAL_AW1, LINK_DEVICE_AUTO },
    { "AW2E", SERIAL_MODE_SERIAL_AW2, SERIAL_MODE_SERIAL_AW2, LINK_DEVICE_AUTO },
    { "BE8E", SERIAL_MODE_AUTO, SERIAL_MODE_SERIAL_FE, LINK_DEVICE_AUTO },
    { "AGSE", SERIAL_MODE_AUTO, SERIAL_MODE_SERIAL_MSIO, LINK_DEVICE_AUTO },
    { "BPEE", SERIAL_MODE_RFU, SERIAL_MODE_RFU, LINK_DEVICE_AUTO },
    { "BPEE", SERIAL_MODE_RFU, SERIAL_MODE_CABLE, LINK_DEVICE_EXACT },
  };
  static u32 packet[(LINK_MSG_MAX + 3) / 4];   /* deliver's: word-aligned */
  unsigned c, f, i, relinks = 0, linked_frames = 0;
  int ok = 1;

  for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
  {
    link_mode_for(cases[c].code, cases[c].game, cases[c].device);
    unit_cable_on = 1;
    for (f = 0; f < 2000; f++)
    {
      if (nspire_link_state() != LINK_LINKED)
      {
        unit_cable_source = NULL;
        unit_cable_clear();
        nspire_link_connect();
        hello_from(cases[c].mode);
        nspire_link_frame_begin();
        if (nspire_link_state() != LINK_LINKED)
        {
          ok = 0;
          break;
        }
        relinks++;
        unit_cable_source = fuzz_message;
      }
      unit_ticks += NSPIRE_TICK_HZ / 60;
      /* The game's port as a game sets it, now and then. */
      if (f % 7 == 0)
        write_ioreg(REG_SIOCNT, (u16)(fuzz_rand() % 2 ? 0x6003 : 0x2000 | (fuzz_rand() & 0x4083)));
      for (i = 0; i < 8; i++)
      {
        cpu_ticks += 280896 / 8;
        update_serial(280896 / 8);
      }
      nspire_link_frame_end();
      linked_frames += nspire_link_state() == LINK_LINKED;
      nspire_link_frame_begin();
    }
    unit_cable_source = NULL;
    nspire_link_disconnect();
    unit_cable_clear();
    unit_cable_on = 0;
  }
  CHECK(ok, "fuzz: each link came up again after the other side's garbage ended it");
  CHECK(fuzz_msgs > 1000, "fuzz: %u random messages taken (%u frames linked, %u links)", fuzz_msgs,
        linked_frames, relinks);
  printf("  %u random messages, %u frames linked, %u links\n", fuzz_msgs, linked_frames, relinks);

  /* Straight to the stand-ins, as deliver hands them packets, in every
   * state an adapter can be in. */
  for (c = 0; c < 4; c++)
  {
    rfu_test_state(c);
    for (i = 0; i < 20000; i++)
    {
      unsigned n = fuzz_packet((u8 *)packet, sizeof(packet));
      u16 from = (u16)(fuzz_rand() % 2);
      rfu_net_receive(packet, n, from);
      serialpoke_net_receive(packet, n, from);
      serialaw_net_receive(packet, n, from);
      serialfe_net_receive(packet, n, from);
      serialmsio_net_receive(packet, n, from);
    }
  }
  rfu_test_state(0);
  rfu_reset();
  serialproto_reset();
  serialfe_reset();
  serialmsio_reset();
  write_ioreg(REG_SIOCNT, 0);
  serial_mode = SERIAL_MODE_AUTO;
  CHECK(1, "fuzz: 400000 packets straight to the stand-ins");
}

/* ---- nspire_clock.c's speed and freeze marker, with switches faked ----
 * Its hardware part is left out (NSPIRE_CLOCK_FAKE_HW); its exported
 * functions are renamed, gpSP's PC build has stubs (host_platform.c). */
#define NSPIRE_CLOCK_FAKE_HW
#define nspire_clock_crashed_rom fake_clock_crashed_rom
#define nspire_clock_check_crash fake_clock_check_crash
#define nspire_clock_game fake_clock_game
#define nspire_clock_normal fake_clock_normal
#define nspire_clock_left_game fake_clock_left_game
#define nspire_clock_report fake_clock_report
void fake_clock_report(char *text, u32 size);   /* nspire_clock.h came in with the real name */
static u32 fake_clock_control = 33u << 24 | 0x03;   /* CX II: x33, 396 MHz */
static u32 fake_cx_clock_now = 22u << 15 | 1u << 21 | 1u << 12 | 1u << 1;   /* CX: 132 MHz, bus 66 */
#define CLOCK_CONTROL fake_clock_control
#define CX_CLOCK_NOW fake_cx_clock_now
#include "../../nspire_clock.c"
#undef CLOCK_CONTROL
#undef CX_CLOCK_NOW
#undef nspire_clock_crashed_rom
#undef nspire_clock_check_crash
#undef nspire_clock_game
#undef nspire_clock_normal
#undef nspire_clock_left_game
#undef nspire_clock_report

static int fake_refuse;   /* switches still to refuse (nothing changes) */

static u32 switch_clock(u32 value)
{
  if (fake_clock_control == value)
    return (u32)CLOCK_MULTIPLIER(value) * 12;
  last_switch.before = fake_clock_control;
  last_switch.target = value;
  last_switch.refused = NULL;
  if (fake_refuse > 0)
  {
    fake_refuse--;
    last_switch.refused = "power controller in an unknown state";
  }
  else
    fake_clock_control = value;
  last_switch.after = fake_clock_control;
  return last_switch.mhz = (u32)CLOCK_MULTIPLIER(fake_clock_control) * 12;
}

static u32 cx_switch_clock(u32 value)
{
  if (fake_cx_clock_now == value)
    return cx_cpu_mhz(value);
  last_switch.before = fake_cx_clock_now;
  last_switch.target = value;
  last_switch.refused = NULL;
  if (fake_refuse > 0)
  {
    fake_refuse--;
    last_switch.refused = "power controller busy";
  }
  else
    fake_cx_clock_now = value;
  last_switch.after = fake_cx_clock_now;
  return last_switch.mhz = cx_cpu_mhz(fake_cx_clock_now);
}

/* The marker's game, "" if there is no marker. */
static const char *clock_marker_game(int *multiplier)
{
  static char line[600];
  char path[600];
  FILE *f;

  marker_path(path, sizeof(path));
  line[0] = 0;
  *multiplier = 0;
  f = fopen(path, "r");
  if (!f)
    return line;
  if (fscanf(f, "%d\n", multiplier) != 1 || !fgets(line, sizeof(line), f))
    snprintf(line, sizeof(line), "(damaged)");
  fclose(f);
  line[strcspn(line, "\n")] = 0;
  return line;
}

static void clock_fresh_start(void)
{
  char path[600];
  marker_path(path, sizeof(path));
  remove(path);
  clock_raised = marker_written = crashed = failed_multiplier = 0;
  clock_mhz = 0;
  shown_multiplier = -1;
  fake_refuse = 0;
  fake_clock_control = 33u << 24 | 0x03;
  fake_cx_clock_now = 22u << 15 | 1u << 21 | 1u << 12 | 1u << 1;
  memset(&last_switch, 0, sizeof(last_switch));
}

static void test_clock(void)
{
  int model, mult;
  char report[300];

  for (model = 1; model >= 0; model--)
  {
    const char *name = model ? "CX II" : "CX";
    u32 os_cx = fake_cx_clock_now;
    host_is_cx2 = model;

    /* OS-1: lowering the speed at exit refused every time: the CPU stays
     * raised, so the marker stays (the next start puts the game's speed
     * back to normal) and the report says so. */
    clock_fresh_start();
    os_cx = fake_cx_clock_now;
    snprintf(gamepak_filename, sizeof(gamepak_filename), "/documents/A.gba.tns");
    nspire_cpu_speed = 3;
    fake_clock_game();
    CHECK(clock_raised && strcmp(clock_marker_game(&mult), gamepak_filename) == 0 && mult == (model ? 36 : 32),
          "%s: raised, marker for A x%d", name, mult);
    fake_refuse = 4;   /* quit: nspire_clock_normal, then nspire_clock_left_game, two tries each */
    fake_clock_normal();
    fake_clock_left_game();
    CHECK(clock_raised, "%s: a refused lowering keeps the speed counted as raised", name);
    CHECK(strcmp(clock_marker_game(&mult), gamepak_filename) == 0, "%s: a refused lowering keeps the marker", name);
    {
      char path[600], text[1200];
      size_t got;
      FILE *f;
      marker_path(path, sizeof(path));
      f = fopen(path, "r");
      got = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
      text[got] = 0;
      if (f)
        fclose(f);
      CHECK(strstr(text, "not put back") != NULL, "%s: the marker says why it stayed: %s", name, text);
    }
    fake_clock_report(report, sizeof(report));
    CHECK(strstr(report, "not put back") != NULL, "%s: the report says the speed wasn't put back: %s", name, report);
    /* Refused once: tried again, and then it's back. */
    fake_refuse = 1;
    fake_clock_left_game();
    CHECK(!clock_raised && (model ? fake_clock_control == (33u << 24 | 0x03) : fake_cx_clock_now == os_cx),
          "%s: the second try puts the OS's speed back", name);
    CHECK(clock_marker_game(&mult)[0] == 0, "%s: then the marker goes", name);

    /* A raise that doesn't happen (refused): normal speed, no marker
     * kept forever. */
    clock_fresh_start();
    fake_refuse = 1;
    fake_clock_game();
    CHECK(!clock_raised, "%s: a refused raise isn't counted as raised", name);
    fake_clock_left_game();
    CHECK(clock_marker_game(&mult)[0] == 0, "%s: no marker after a refused raise", name);

    /* OS-2: game A raised, then B raised from the menu (nspire_leave_game
     * runs only nspire_clock_normal): the marker names B and B's speed. */
    clock_fresh_start();
    snprintf(gamepak_filename, sizeof(gamepak_filename), "/documents/A.gba.tns");
    nspire_cpu_speed = 3;
    fake_clock_game();
    fake_clock_normal();
    snprintf(gamepak_filename, sizeof(gamepak_filename), "/documents/B.gba.tns");
    nspire_cpu_speed = 5;
    fake_clock_game();
    CHECK(strcmp(clock_marker_game(&mult), "/documents/B.gba.tns") == 0 && mult == (model ? 38 : 36),
          "%s: the marker names B x%d: %s", name, mult, clock_marker_game(&mult));
    /* Same game, another speed (the menu's setting): the marker says it. */
    fake_clock_normal();
    nspire_cpu_speed = 6;
    fake_clock_game();
    CHECK(clock_marker_game(&mult)[0] && mult == (model ? 39 : 38), "%s: the marker has the new speed x%d", name, mult);
    /* B freezes; the next start blames B. */
    clock_raised = marker_written = 0;
    fake_clock_check_crash();
    CHECK(strcmp(fake_clock_crashed_rom, "/documents/B.gba.tns") == 0, "%s: a freeze blames B: %s", name,
          fake_clock_crashed_rom);
    /* C at normal speed after a raised game: no marker while it plays. */
    clock_fresh_start();
    nspire_cpu_speed = 3;
    fake_clock_game();
    fake_clock_normal();
    snprintf(gamepak_filename, sizeof(gamepak_filename), "/documents/C.gba.tns");
    nspire_cpu_speed = 0;
    fake_clock_game();
    CHECK(!clock_raised && clock_marker_game(&mult)[0] == 0, "%s: a normal-speed game leaves no marker", name);
  }
  host_is_cx2 = 1;
  nspire_cpu_speed = 0;
  gamepak_filename[0] = 0;
  fake_clock_crashed_rom[0] = 0;
}

/* P1: auto, the default CPU speed (192 MHz on a CX, normal on a CX II),
 * without overriding a choice: a file from before it with 0 (the old
 * default, normal) is auto; any other speed in it is kept; normal chosen in
 * the menu, or put back after a freeze, is saved marked (1000 + index). */
static void test_cpu_speed_auto(void)
{
  char path[600], rom[600], game_cfg[600], saved_program[sizeof(nspire_program_path)];
  u32 o[72], os_cx;
  int model, mult;
  FILE *f;

  snprintf(saved_program, sizeof(saved_program), "%s", nspire_program_path);
  nspire_program_path[0] = 0;
  snprintf(main_path, sizeof(main_path), "%s", dir);
  nspire_save_file(path, sizeof(path), "gpsp.cfg.tns");

  nspire_cpu_speed = 0;
  CHECK(nspire_config_save() == 0, "auto: save");
  read_words(path, o, 72);
  CHECK(o[61] == 1000, "auto: normal chosen is saved as 1000: %u", o[61]);
  o[61] = 0;   /* the file of a build before auto, at its default */
  write_words(path, o, 72);
  nspire_cpu_speed = 3;
  CHECK(nspire_config_load() == 0 && nspire_cpu_speed == CPU_SPEED_AUTO,
        "auto: an old file's 0 is auto: %u", nspire_cpu_speed);
  for (model = 1; model >= 0; model--)
  {
    host_is_cx2 = model;
    clock_fresh_start();
    os_cx = fake_cx_clock_now;
    snprintf(gamepak_filename, sizeof(gamepak_filename), "/documents/A.gba.tns");
    fake_clock_game();
    if (model)
      CHECK(!clock_raised && fake_clock_control == (33u << 24 | 0x03) && clock_marker_game(&mult)[0] == 0,
            "auto: no switch on a CX II (%08x)", fake_clock_control);
    else
      CHECK(clock_raised && fake_cx_clock_now == cx_setting(os_cx, 32) && cx_cpu_mhz(fake_cx_clock_now) == 192 &&
            cx_bus_mhz(fake_cx_clock_now) == 64 && strcmp(clock_marker_game(&mult), gamepak_filename) == 0 &&
            mult == 32, "auto: a CX goes to 192 MHz, bus 64 (%08x, x%d)", fake_cx_clock_now, mult);
    fake_clock_left_game();
  }

  o[61] = 4;   /* a raised speed chosen before auto: kept */
  write_words(path, o, 72);
  CHECK(nspire_config_load() == 0 && nspire_cpu_speed == 4, "auto: an old file's speed 4 kept: %u",
        nspire_cpu_speed);
  CHECK(nspire_config_save() == 0, "auto: save 4");
  read_words(path, o, 72);
  CHECK(o[61] == 1004, "auto: then saved marked: %u", o[61]);

  /* Normal chosen in the menu stays normal across a save and a load. */
  nspire_cpu_speed = 0;
  CHECK(nspire_config_save() == 0, "auto: save normal");
  nspire_cpu_speed = CPU_SPEED_AUTO;
  CHECK(nspire_config_load() == 0 && nspire_cpu_speed == 0, "auto: normal chosen stays normal: %u",
        nspire_cpu_speed);
  host_is_cx2 = 0;
  clock_fresh_start();
  fake_clock_game();
  CHECK(!clock_raised, "auto: normal chosen: no switch on a CX");
  nspire_cpu_speed = CPU_SPEED_AUTO;
  CHECK(nspire_config_save() == 0, "auto: save auto");
  read_words(path, o, 72);
  CHECK(o[61] == 0, "auto: saved as 0: %u", o[61]);

  /* A freeze at auto: normal next start, and marked so it stays normal. */
  clock_fresh_start();
  fake_clock_game();
  clock_raised = marker_written = 0;   /* frozen: the marker stayed */
  nspire_file_list_start();   /* (the next start's listing, which has it: fast17) */
  CHECK(fake_clock_check_crash() == 1 && nspire_cpu_speed == 0 && crashed,
        "auto: a freeze puts it to normal and says so (to save at once): %u", nspire_cpu_speed);
  CHECK(fake_clock_check_crash() == 0, "auto: no marker: says none");
  CHECK(nspire_config_save() == 0, "auto: save after the freeze");
  read_words(path, o, 72);
  nspire_cpu_speed = CPU_SPEED_AUTO;
  CHECK(o[61] == 1000 && nspire_config_load() == 0 && nspire_cpu_speed == 0,
        "auto: normal after a freeze is marked (%u) and stays: %u", o[61], nspire_cpu_speed);
  clock_fresh_start();
  fake_clock_game();
  CHECK(!clock_raised, "auto: not raised again after the freeze");
  fake_clock_crashed_rom[0] = 0;

  /* A game's own settings: the same. */
  snprintf(rom, sizeof(rom), "%s/Auto.gba.tns", dir);
  nspire_save_file(game_cfg, sizeof(game_cfg), "Auto.gba.settings.tns");
  f = fopen(game_cfg, "w");
  fprintf(f, "version=3\nown_settings=1\ncpu_speed=0\nend=1\n");
  fclose(f);
  nspire_cpu_speed = 0;
  nspire_config_open_game(rom);
  CHECK(nspire_cpu_speed == CPU_SPEED_AUTO, "auto: a game file's old 0 is auto: %u", nspire_cpu_speed);
  nspire_cpu_speed = 0;   /* normal, from the menu, for this game */
  nspire_config_close_game();
  CHECK(nspire_cpu_speed == 0 && file_contains(game_cfg, "cpu_speed=1000\n"),
        "auto: the game's normal saved marked, all games' kept: %u", nspire_cpu_speed);
  nspire_cpu_speed = CPU_SPEED_AUTO;
  nspire_config_open_game(rom);
  CHECK(nspire_cpu_speed == 0, "auto: the game's normal stays: %u", nspire_cpu_speed);
  nspire_cpu_speed = CPU_SPEED_AUTO;
  nspire_config_close_game();
  CHECK(nspire_cpu_speed == CPU_SPEED_AUTO && file_contains(game_cfg, "cpu_speed=0\n"),
        "auto: the game's auto saved as 0, all games' back: %u", nspire_cpu_speed);
  f = fopen(game_cfg, "w");
  fprintf(f, "version=3\nown_settings=1\ncpu_speed=5\nend=1\n");
  fclose(f);
  nspire_config_open_game(rom);
  CHECK(nspire_cpu_speed == 5, "auto: a game file's old 5 kept: %u", nspire_cpu_speed);
  nspire_settings_for = 0;
  nspire_config_settings_for_changed();
  nspire_config_close_game();
  remove(game_cfg);
  remove(path);

  host_is_cx2 = 1;
  nspire_cpu_speed = CPU_SPEED_AUTO;
  gamepak_filename[0] = 0;
  clock_fresh_start();
  snprintf(nspire_program_path, sizeof(nspire_program_path), "%s", saved_program);
}


/* ---- PAD3: the Controller status and the pad log ---- */

extern int unit_usb_raw_kind, unit_usb_plugged, unit_usb_gated;
extern uint32_t unit_usb_raw[6];

/* A snapshot from raw words: kind, plugged, OTGCSR/OTGSC, USBCMD, PORTSC
 * (word 1 is a CX's USBMODE), the driver registered and waiting. */
static struct pad_status ps_snap(int kind, int plugged, uint32_t otg, uint32_t cmd, uint32_t port)
{
  struct pad_status s;

  memset(&s, 0, sizeof(s));
  s.kind = kind;
  s.plugged = plugged;
  s.raw[0] = otg;
  s.raw[1] = kind == 1 ? 3 : 0;
  s.raw[2] = cmd;
  s.raw[4] = port;
  nspire_padstatus_decode(&s);
  s.drv.started = 1;
  s.drv.slot = -1;
  s.owner = ARB_WAIT;
  s.link = LINK_OFF;
  return s;
}

/* The R9F CX II's words: the adapter's end, VBUS valid, sessions; the host
 * controller stopped (USBCMD 00080b00). */
#define R9F_OTG 0x000e0010u
#define R9F_CMD 0x00080b00u
#define RUN_CMD 0x00080031u

static const char *ps_value(const struct pad_status *s, const char *label)
{
  static char label_[PADSTATUS_LINES][PADSTATUS_LABEL], value[PADSTATUS_LINES][PADSTATUS_VALUE];
  int i;

  nspire_padstatus_lines(s, label_, value);
  for (i = 0; i < PADSTATUS_LINES; i++)
    if (!strcmp(label_[i], label))
      return value[i];
  return "(no such line)";
}

static void test_padstatus(void)
{
  struct pad_status s, all[48];
  int n = 0, i, v;
  char label[PADSTATUS_LINES][PADSTATUS_LABEL], value[PADSTATUS_LINES][PADSTATUS_VALUE];

  /* Every state, its verdict and its words. */
  s = ps_snap(2, 0, 0x00310020, R9F_CMD, 0);
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_NOTHING && !strcmp(ps_value(&s, "Port"), "nothing plugged in"),
        "status: nothing plugged in: %s, %s", nspire_padstatus_name(nspire_padstatus_verdict(&s)), ps_value(&s, "Port"));
  s = ps_snap(0, -1, 0, 0, 0);
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_UNREADABLE && strstr(ps_value(&s, "Port"), "can't tell") &&
        !strcmp(ps_value(&s, "OS USB"), "-"), "status: the controller off: %s, %s", ps_value(&s, "Port"), ps_value(&s, "OS USB"));
  s = ps_snap(0, -1, 0, 0, 0);
  s.gated = 1;
  all[n++] = s;
  CHECK(strstr(ps_value(&s, "Port"), "gated") != NULL, "status: a CX gated: %s", ps_value(&s, "Port"));
  /* the cable's plain end (a computer, the other calculator's adapter): the user is told to use the adapter end */
  s = ps_snap(2, 1, 0x00360024, R9F_CMD, 0);
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_DEVICE_END && !strcmp(ps_value(&s, "Port"), "device end: use adapter end") &&
        !strcmp(ps_value(&s, "Bus power"), "on") && strstr(nspire_padstatus_cause(PV_DEVICE_END), "adapter goes in THIS calculator"),
        "status: device end: %s, power %s", ps_value(&s, "Port"), ps_value(&s, "Bus power"));
  /* the adapter alone */
  s = ps_snap(2, 1, R9F_OTG, R9F_CMD, 0);
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_EMPTY_ADAPTER && !strcmp(ps_value(&s, "Device"), "nothing on the data wires") &&
        !strcmp(ps_value(&s, "Port"), "host: the adapter's end") && !strcmp(ps_value(&s, "Bus power"), "on"),
        "status: the adapter alone: %s / %s", nspire_padstatus_name(nspire_padstatus_verdict(&s)), ps_value(&s, "Device"));
  /* the adapter with no power: not asked for yet (A_BUS_DROP set, as the R8 log's first look: 00010020) */
  s = ps_snap(2, 1, 0x00010020, R9F_CMD, 0);
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_NO_POWER && !strcmp(ps_value(&s, "Bus power"), "off"), "status: no power: %s",
        nspire_padstatus_name(nspire_padstatus_verdict(&s)));
  /* (r19-usb) asked for (A_BUS_REQ set, A_BUS_DROP clear) and below A VBUS: held down, sagging (00060010) or
   * below a session (00012e10, a CX II log of 567381a) */
  s = ps_snap(2, 1, 0x00060010, R9F_CMD, 0);
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_POWER_HELD && !strcmp(ps_value(&s, "Bus power"), "off"),
        "status: power held (sagging): %s", nspire_padstatus_name(nspire_padstatus_verdict(&s)));
  s = ps_snap(2, 1, 0x00012e10, R9F_CMD, 0);
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_POWER_HELD, "status: power held (below a session): %s",
        nspire_padstatus_name(nspire_padstatus_verdict(&s)));
  /* R9F's CX II at 193 s: a pull-up on the wires (PORTSC 00000800), never connected, the host stopped */
  s = ps_snap(2, 1, R9F_OTG, R9F_CMD, 0x800);
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_HOST_STOPPED && !strcmp(ps_value(&s, "Device"), "on wires, not connected") &&
        !strcmp(ps_value(&s, "OS USB"), "host controller stopped") && !strcmp(ps_value(&s, "Port enabled"), "no") &&
        !strcmp(ps_value(&s, "OS reported"), "none reported by the OS") &&
        !strcmp(ps_value(&s, "Driver"), "waiting for OS offer"),
        "status: R9F host stopped: %s | %s | %s | %s", ps_value(&s, "Device"), ps_value(&s, "OS USB"),
        ps_value(&s, "OS reported"), ps_value(&s, "Driver"));
  /* R9F's session 1: connected (PORTSC 00000803), the host stopped */
  s = ps_snap(2, 1, R9F_OTG, R9F_CMD, 0x803);
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_HOST_STOPPED && !strcmp(ps_value(&s, "Device"), "connected"),
        "status: connected, host stopped: %s", nspire_padstatus_name(nspire_padstatus_verdict(&s)));
  s.drv.gave_up = 1;
  all[n++] = s;
  CHECK(!strcmp(ps_value(&s, "Driver"), "no offer from the OS in 12 s"), "status: the glue's wait ran out: %s", ps_value(&s, "Driver"));
  /* the host runs, the port not enabled; enabled, nothing offered */
  s = ps_snap(2, 1, R9F_OTG, RUN_CMD, 0x801);
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_NOT_ENABLED && !strcmp(ps_value(&s, "OS USB"), "host controller running"),
        "status: not enabled: %s", nspire_padstatus_name(nspire_padstatus_verdict(&s)));
  s = ps_snap(2, 1, R9F_OTG, RUN_CMD, 0x805);
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_NOT_OFFERED && !strcmp(ps_value(&s, "Port enabled"), "yes"),
        "status: not offered: %s", nspire_padstatus_name(nspire_padstatus_verdict(&s)));
  /* PAD4: R10a after the Switch Pro (685064 on): enabled and suspended, nothing offered ever again */
  {
    struct pad_status t = ps_snap(2, 1, R9F_OTG, RUN_CMD, 0x88d);
    all[n++] = t;
    CHECK(nspire_padstatus_verdict(&t) == PV_SUSPENDED && nspire_padstatus_failing(PV_SUSPENDED) &&
          !strcmp(ps_value(&t, "Port enabled"), "yes, but suspended") && !strcmp(nspire_padstatus_name(PV_SUSPENDED), "suspended"),
          "status: suspended: %s | %s", nspire_padstatus_name(nspire_padstatus_verdict(&t)), ps_value(&t, "Port enabled"));
    t = ps_snap(2, 1, R9F_OTG, RUN_CMD, 0x103);
    all[n++] = t;
    CHECK(nspire_padstatus_verdict(&t) == PV_NOT_ENABLED && !strcmp(ps_value(&t, "Port enabled"), "no: the OS is resetting it"),
          "status: in reset: %s | %s", nspire_padstatus_name(nspire_padstatus_verdict(&t)), ps_value(&t, "Port enabled"));
    /* the come-up's times on the page: none yet; each step; one under way with wide numbers */
    t = ps_snap(2, 1, R9F_OTG, RUN_CMD, 0x805);
    CHECK(!strcmp(ps_value(&t, "Since plug"), "-"), "status: no come-up yet: %s", ps_value(&t, "Since plug"));
    t.drv.cu_on = 1;
    t.drv.cu_step[CU_HC] = 1;
    t.drv.cu_step[CU_CONN] = 1;
    t.drv.cu_step[CU_ENABLED] = 251;
    t.drv.cu_step[CU_OFFER] = 301;
    t.drv.cu_step[CU_ATTACH] = 352;
    t.drv.cu_step[CU_REPORT] = 401;
    all[n++] = t;
    CHECK(!strcmp(ps_value(&t, "Since plug"), "HC 0.0 off 0.3 pad 0.4 in 0.4 s"), "status: the come-up's times: %s",
          ps_value(&t, "Since plug"));
    /* the widest: every step just under 10 s (one decimal), and one under way */
    t.drv.cu_step[CU_HC] = t.drv.cu_step[CU_OFFER] = t.drv.cu_step[CU_ATTACH] = t.drv.cu_step[CU_REPORT] = 9900;
    all[n++] = t;
    CHECK(!strcmp(ps_value(&t, "Since plug"), "HC 9.9 off 9.9 pad 9.9 in 9.9 s"), "status: a come-up near 10 s: %s",
          ps_value(&t, "Since plug"));
    t.drv.cu_live = 1;
    t.drv.cu_age_ms = 59900;
    all[n++] = t;
    CHECK(!strcmp(ps_value(&t, "Since plug"), "HC 9.9 off 9.9 pad 9.9 (59s)"), "status: a come-up under way: %s",
          ps_value(&t, "Since plug"));
    memset(t.drv.cu_step, 0, sizeof(t.drv.cu_step));
    t.drv.cu_step[CU_HC] = 92001;
    all[n++] = t;
    CHECK(!strcmp(ps_value(&t, "Since plug"), "HC 92 off - pad - (59s)"), "status: the host controller late: %s",
          ps_value(&t, "Since plug"));
  }
  /* offers and the driver's answers */
  s.drv.offered = 1;
  s.drv.vid = 0x045e;
  s.drv.pid = 0x028e;
  s.drv.iface = 1;
  s.drv.cls = 0xff;
  s.drv.sub = 0x5d;
  s.drv.proto = 1;
  s.drv.score = 5;
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_STARTING && !strcmp(ps_value(&s, "OS reported"), "045e:028e, interface ff/5d/01") &&
        strstr(ps_value(&s, "Driver"), "took it"), "status: taken: %s | %s", ps_value(&s, "OS reported"), ps_value(&s, "Driver"));
  /* R7: taken, and no attach in the glue's 12 s: a failing verdict, not "starting" for ever */
  s.drv.gave_up = 1;
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_NOT_ATTACHED && nspire_padstatus_failing(PV_NOT_ATTACHED) &&
        !strcmp(ps_value(&s, "Driver"), "took it (5): no attach 12 s"), "status: taken, never attached: %s | %s",
        nspire_padstatus_name(nspire_padstatus_verdict(&s)), ps_value(&s, "Driver"));
  s.drv.gave_up = 0;
  s.drv.score = 0;
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_NOT_A_PAD && !strcmp(ps_value(&s, "Driver"), "said no: not a controller"),
        "status: not a pad: %s", ps_value(&s, "Driver"));
  /* R11u (2026-11-15): not a controller is no failure (no "USB not
   * working"): failing() said 1 for not_a_pad until then */
  CHECK(!nspire_padstatus_failing(PV_NOT_A_PAD), "status: not a pad is not failing");
  /* R11u: taken at match, then kept quietly by the driver as not a
   * controller (a sound card's volume keys): not "starting", nor after the
   * 12 s "no attach" */
  s.drv.score = 5;
  s.drv.not_pad = 1;
  snprintf(s.drv.detail, sizeof(s.drv.detail), "Not a controller (0d8c:0014 if 3)");
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_NOT_A_PAD && !strcmp(ps_value(&s, "Driver"), "Not a controller (0d8c:0014 if 3)"),
        "status: kept as not a pad: %s | %s", nspire_padstatus_name(nspire_padstatus_verdict(&s)), ps_value(&s, "Driver"));
  s.drv.gave_up = 1;
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_NOT_A_PAD, "status: kept as not a pad, past 12 s: %s",
        nspire_padstatus_name(nspire_padstatus_verdict(&s)));
  /* ...and a real pad's failure on the same device still says so */
  s.drv.event = 5;
  CHECK(nspire_padstatus_verdict(&s) == PV_UNSUPPORTED, "status: a failed pad beside a non-pad: %s",
        nspire_padstatus_name(nspire_padstatus_verdict(&s)));
  s.drv.event = 0;
  s.drv.gave_up = 0;
  s.drv.not_pad = 0;
  s.drv.detail[0] = 0;
  s.drv.score = 0;
  s.drv.score = -4;
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_REFUSED && !strcmp(ps_value(&s, "Driver"), "said no: no attach arguments"),
        "status: refused: %s", ps_value(&s, "Driver"));
  s.drv.score = -2;
  s.drv.vid = 0x0451;
  s.drv.pid = 0xe012;
  s.drv.iface = 0;
  s.drv.cls = s.drv.sub = s.drv.proto = 0;
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_CALCULATOR && !strcmp(ps_value(&s, "Driver"), "said no: TI calculator (link)") &&
        !strcmp(ps_value(&s, "OS reported"), "0451:e012, device 00/00/00"),
        "status: the other calculator: %s | %s", ps_value(&s, "OS reported"), ps_value(&s, "Driver"));
  /* the driver's own words */
  s = ps_snap(2, 1, R9F_OTG, RUN_CMD, 0x805);
  s.drv.event = 4;
  s.drv.ev_vid = 0x2dc8;
  s.drv.ev_pid = 0x310b;
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_NEEDS_POWER && !strcmp(ps_value(&s, "Driver"), "needs more power (2dc8:310b)"),
        "status: needs power: %s", ps_value(&s, "Driver"));
  s.drv.event = 5;
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_UNSUPPORTED, "status: unsupported: %s", nspire_padstatus_name(nspire_padstatus_verdict(&s)));
  s.drv.event = 6;
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_FAILED, "status: failed: %s", nspire_padstatus_name(nspire_padstatus_verdict(&s)));
  /* working */
  s = ps_snap(2, 1, R9F_OTG, RUN_CMD, 0x805);
  s.drv.pads = 1;
  s.drv.slot = 0;
  snprintf(s.drv.name, sizeof(s.drv.name), "Xbox 360");
  s.owner = ARB_PAD;
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_WORKING && !strcmp(ps_value(&s, "Driver"), "took it: Xbox 360 (slot 1)") &&
        !strcmp(ps_value(&s, "Port owner"), "the controller"), "status: working: %s | %s", ps_value(&s, "Driver"),
        ps_value(&s, "Port owner"));
  strcpy(s.drv.detail, "Switch Pro: handshake");
  CHECK(!strcmp(ps_value(&s, "Driver"), "Switch Pro: handshake"), "status: protocol progress: %s", ps_value(&s, "Driver"));
  /* no driver; the link; another driver */
  s = ps_snap(2, 1, R9F_OTG, R9F_CMD, 0x800);
  s.drv.started = 0;
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_NO_DRIVER && !strcmp(ps_value(&s, "Driver"), "not running"), "status: no driver: %s",
        ps_value(&s, "Driver"));
  s.drv.started = -1;
  snprintf(s.drv.status, sizeof(s.drv.status), "No USB on this OS");
  all[n++] = s;
  CHECK(!strcmp(ps_value(&s, "Driver"), "failed (No USB on this OS)"), "status: init failed: %s", ps_value(&s, "Driver"));
  s = ps_snap(2, 1, R9F_OTG, RUN_CMD, 0x805);
  s.link = LINK_LINKED;
  s.owner = ARB_LINK;
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_LINKED && !strcmp(ps_value(&s, "Port owner"), "the link (linked)"), "status: linked: %s",
        ps_value(&s, "Port owner"));
  s.link = LINK_LOOKING;
  all[n++] = s;
  CHECK(!strcmp(ps_value(&s, "Port owner"), "the link (looking for GBA)"), "status: looking: %s", ps_value(&s, "Port owner"));
  s = ps_snap(2, 1, R9F_OTG, RUN_CMD, 0x805);
  s.owner = ARB_OTHER;
  all[n++] = s;
  CHECK(nspire_padstatus_verdict(&s) == PV_OTHER && !nspire_padstatus_failing(PV_OTHER), "status: another driver: %s",
        nspire_padstatus_name(nspire_padstatus_verdict(&s)));
  s = ps_snap(2, 1, R9F_OTG, R9F_CMD, 0x800);
  s.cycles = 2;
  all[n++] = s;
  CHECK(!strcmp(ps_value(&s, "Port owner"), "nobody yet (2 restarts)"), "status: cycles: %s", ps_value(&s, "Port owner"));
  /* a CX (ChipIdea): OTGSC with the ID pin low (host), A VBUS valid, IDPU; USBMODE host; the lines J */
  s = ps_snap(1, 1, 0x00000220, 0x00080000, 0x1800);
  all[n++] = s;
  CHECK(s.role == PS_ROLE_HOST && s.power && nspire_padstatus_verdict(&s) == PV_HOST_STOPPED, "status: a CX host end, stopped: %s",
        nspire_padstatus_name(nspire_padstatus_verdict(&s)));
  s = ps_snap(1, 1, 0x00000920, 0x00080001, 0x1885);
  all[n++] = s;
  CHECK(s.role == PS_ROLE_DEVICE && s.power && nspire_padstatus_verdict(&s) == PV_DEVICE_END, "status: a CX device end: %s",
        nspire_padstatus_name(nspire_padstatus_verdict(&s)));

  /* Every text fits the page (r16: on the grid's margins, the details in the OSD's type): the values in their
   * column (208 px, from the margin + 84), the labels before it, the causes in three lines. */
  for (i = 0; i < n; i++)
  {
    int k;
    nspire_padstatus_lines(&all[i], label, value);
    for (k = 0; k < PADSTATUS_LINES; k++)
      CHECK(ui_text_width(value[k], &ui_font_osd) <= UI_W - UI_MARGIN - (UI_MARGIN + 84) &&
            ui_text_width(label[k], &ui_font_osd) <= 84 - 4,
            "status: %s: \"%s: %s\" fits (%d px)", nspire_padstatus_name(nspire_padstatus_verdict(&all[i])), label[k], value[k],
            ui_text_width(value[k], &ui_font_osd));
  }
  for (v = 0; v < PV_COUNT; v++)
    CHECK(nspire_padstatus_cause((enum pad_verdict)v)[0] &&
          ui_wrap_lines(UI_W - 24, 10, nspire_padstatus_cause((enum pad_verdict)v), &ui_font_small_strong) <= 3 &&
          strcmp(nspire_padstatus_name((enum pad_verdict)v), "?"),
          "status: verdict %d (%s): its words fit three lines", v, nspire_padstatus_name((enum pad_verdict)v));
  CHECK(ui_text_width(PADSTATUS_TOAST_TEXT, &ui_font_osd) <= UI_W - 16, "status: the toast fits (%d px)",
        ui_text_width(PADSTATUS_TOAST_TEXT, &ui_font_osd));

  /* The toast: once a failing state has held 2 s (PADSTATUS_STABLE_MS),
   * once a plug. (R11w part 2: this checked 3 s, again at 20 s, never more;
   * a message must show only when it applies, not repeat: once per plug now, after 2 s of the same
   * state.) */
  {
    const char *t[4];
    int k;
    nspire_padstatus_reset();
    s = ps_snap(2, 1, R9F_OTG, R9F_CMD, 0x800);
    s.stage_ms = 60000;   /* the arbiter stuck at stage 0 (a veto that holds) */
    nspire_padstatus_note(&s, nspire_padstatus_verdict(&s));
    unit_ticks += NSPIRE_TICK_HZ * 19 / 10;
    t[0] = nspire_padstatus_toast_due();
    unit_ticks += NSPIRE_TICK_HZ / 5;
    t[1] = nspire_padstatus_toast_due();
    unit_ticks += NSPIRE_TICK_HZ * 17;
    t[2] = nspire_padstatus_toast_due();
    unit_ticks += NSPIRE_TICK_HZ * 60;
    t[3] = nspire_padstatus_toast_due();
    CHECK(!t[0] && t[1] && !strcmp(t[1], PADSTATUS_TOAST_TEXT) && !t[2] && !t[3],
          "status toast: at 2 s of the same state, then never again this plug: %d %d %d %d", !!t[0], !!t[1], !!t[2], !!t[3]);
    /* a verdict that keeps changing (the OS working on the device) is not
     * stable: none until one holds 2 s */
    nspire_padstatus_reset();
    for (k = 0; k < 10; k++)
    {
      s = ps_snap(2, 1, R9F_OTG, k & 1 ? RUN_CMD : R9F_CMD, 0x800);
      s.stage_ms = 60000;
      nspire_padstatus_note(&s, nspire_padstatus_verdict(&s));
      unit_ticks += NSPIRE_TICK_HZ;
      CHECK(!nspire_padstatus_toast_due(), "status toast: none while the state moves (%d)", k);
    }
    /* gpSP's own power restart's drop and return is no new plug: no second toast */
    unit_ticks += NSPIRE_TICK_HZ * 3;
    CHECK(nspire_padstatus_toast_due() != NULL, "status toast: once it holds");
    CHECK(!nspire_padstatus_may_ask(), "status toast: the plug's one message said");
    /* the arbiter's stages say their own: none while it cycles or gave up, or may still cycle */
    nspire_padstatus_reset();
    s.stage = ARB_STAGE_WAIT_AGAIN;
    nspire_padstatus_note(&s, nspire_padstatus_verdict(&s));
    unit_ticks += NSPIRE_TICK_HZ * 30;
    CHECK(!nspire_padstatus_toast_due(), "status toast: none while the arbiter cycles");
    /* R1: once it gave up (stage 4, its last resort said), ours follows */
    s.stage = ARB_STAGE_GAVE_UP;
    nspire_padstatus_note(&s, nspire_padstatus_verdict(&s));
    CHECK(nspire_padstatus_toast_due() != NULL, "status toast: after the arbiter gave up");
    nspire_padstatus_reset();
    s.stage = 0;
    s.stage_ms = 1000;
    nspire_padstatus_note(&s, nspire_padstatus_verdict(&s));
    unit_ticks += NSPIRE_TICK_HZ * 5;
    CHECK(!nspire_padstatus_toast_due(), "status toast: none while stage 0 may still cycle");
    /* not while the link is picked, nor for a calculator */
    nspire_padstatus_reset();
    s.stage_ms = 60000;
    s.link = LINK_LOOKING;
    nspire_padstatus_note(&s, nspire_padstatus_verdict(&s));
    unit_ticks += NSPIRE_TICK_HZ * 5;
    CHECK(!nspire_padstatus_toast_due(), "status toast: none while Link is picked");
    /* the adapter alone: at 8 s; not once a device was there (pulled out, SEAM-5) */
    nspire_padstatus_reset();
    s = ps_snap(2, 1, R9F_OTG, R9F_CMD, 0);
    s.stage_ms = 60000;
    nspire_padstatus_note(&s, nspire_padstatus_verdict(&s));
    unit_ticks += NSPIRE_TICK_HZ * 5;
    t[0] = nspire_padstatus_toast_due();
    unit_ticks += NSPIRE_TICK_HZ * 4;
    t[1] = nspire_padstatus_toast_due();
    CHECK(!t[0] && t[1], "status toast: an adapter that never had a device: at 8 s (%d %d)", !!t[0], !!t[1]);
    nspire_padstatus_reset();
    s = ps_snap(2, 1, R9F_OTG, RUN_CMD, 0x805);
    s.drv.pads = 1;
    nspire_padstatus_note(&s, nspire_padstatus_verdict(&s));
    s = ps_snap(2, 1, R9F_OTG, R9F_CMD, 0);
    s.stage_ms = 60000;
    nspire_padstatus_note(&s, nspire_padstatus_verdict(&s));
    unit_ticks += NSPIRE_TICK_HZ * 30;
    CHECK(!nspire_padstatus_toast_due(), "status toast: none for an adapter a pad was pulled out of");
    /* R11u: a device that is not a controller (kept quietly, or 0 at every
     * offer): no "USB not working", not at 3 s, not at 20 s */
    {
      int k;
      for (k = 0; k < 2; k++)
      {
        nspire_padstatus_reset();
        s = ps_snap(2, 1, R9F_OTG, RUN_CMD, 0x805);
        s.stage_ms = 60000;
        s.drv.offered = 1;
        s.drv.iface = 1;
        s.drv.cls = 3;
        s.drv.score = k ? 0 : 5;
        s.drv.not_pad = (uint8_t)!k;
        nspire_padstatus_note(&s, nspire_padstatus_verdict(&s));
        unit_ticks += NSPIRE_TICK_HZ * 5;
        t[0] = nspire_padstatus_toast_due();
        unit_ticks += NSPIRE_TICK_HZ * 25;
        nspire_padstatus_note(&s, nspire_padstatus_verdict(&s));
        t[1] = nspire_padstatus_toast_due();
        CHECK(!t[0] && !t[1] && nspire_padstatus_verdict(&s) == PV_NOT_A_PAD,
              "status toast: none for a device that is not a controller (%s): %d %d", k ? "score 0" : "kept quietly",
              !!t[0], !!t[1]);
      }
    }
    nspire_padstatus_reset();
  }
}

static int file_has(const char *name, const char *text)
{
  char path[700], buf[65536];
  FILE *f;
  size_t n;

  nspire_save_file(path, sizeof(path), name);
  f = fopen(path, "r");
  if (!f)
    return -1;
  n = fread(buf, 1, sizeof(buf) - 1, f);
  buf[n] = 0;
  fclose(f);
  return strstr(buf, text) != NULL;
}

static void test_padlog(void)
{
  static char text[70000];
  char path[700];
  int i;

  nspire_save_file(path, sizeof(path), PADLOG_FILE);
  remove(path);
  nspire_save_file(path, sizeof(path), PADLOG_PREV);
  remove(path);
  nspire_padlog_reset();
  /* The CX II's idle off-and-on (R9F: 00000000 <-> 20000031 every 1.5 s): one line with a count. */
  for (i = 0; i < 40; i++)
  {
    unit_usb_raw_kind = i & 1 ? 0 : 2;
    unit_usb_plugged = i & 1 ? -1 : 0;
    memset(unit_usb_raw, 0, sizeof(unit_usb_raw));
    if (!(i & 1))
      unit_usb_raw[0] = 0x00310020;
    nspire_padlog_poll();
    unit_ticks += NSPIRE_TICK_HZ / 5;
  }
  nspire_padlog_text(text, sizeof(text), "test");
  CHECK(nspire_padlog_count() == 2 && strstr(text, "went off and on 39 time(s)"), "pad log: 40 idle flips are 2 lines: %u\n%s",
        nspire_padlog_count(), text);
  nspire_padlog_exit();
  CHECK(file_has(PADLOG_FILE, "gpSP") == -1, "pad log: nothing ever plugged in: no file");
  nspire_padlog_reset();
  /* The adapter, then a device on its wires with the host stopped (R9F's words). */
  unit_usb_raw_kind = 2;
  unit_usb_plugged = 1;
  memset(unit_usb_raw, 0, sizeof(unit_usb_raw));
  unit_usb_raw[0] = R9F_OTG;
  unit_usb_raw[2] = R9F_CMD;
  nspire_padlog_poll();
  unit_ticks += NSPIRE_TICK_HZ / 5;
  unit_usb_raw[4] = 0x800;
  nspire_padlog_poll();
  unit_ticks += NSPIRE_TICK_HZ / 5;
  nspire_padlog_poll();   /* no change: no line */
  CHECK(nspire_padlog_count() == 2 && nspire_padlog_wires_counter() == 1, "pad log: two port lines, the device on the wires once: %u, %u",
        nspire_padlog_count(), (unsigned)nspire_padlog_wires_counter());
  unit_usb_raw[4] = 0;
  unit_ticks += NSPIRE_TICK_HZ / 5;
  nspire_padlog_poll();
  CHECK(nspire_padlog_wires_gone() == 1, "pad log: the device off the wires: %u", (unsigned)nspire_padlog_wires_gone());
  nspire_padlog_text(text, sizeof(text), "test");
  CHECK(strstr(text, "port: host end (adapter), power on, a device on the wires (not connected), not enabled, host controller STOPPED | "
                     "OTGCSR 000e0010 OTGISR 00000000 USBCMD 00080b00 USBSTS 00000000 PORTSC 00000800 GISR 00000000") != NULL,
        "pad log: the R9F words decoded:\n%s", text);
  /* offers, events, holders, cycles, verdicts: their words */
  nspire_padlog_add(PL_OFFER, (uint8_t)-2, 0, 0, 0x0451e012u, 0, 1);
  nspire_padlog_add(PL_OFFER, 5, 1, 0, 0x045e028eu, 0xff5d01u, 2);
  nspire_padlog_add(PL_EVENT, 1, 2, 1, 0x045e028eu, 0, 0);
  nspire_padlog_add(PL_OWNER, ARB_PAD, ARB_WAIT, 0, 0, 0, 0);
  nspire_padlog_add(PL_CYCLE, 1, 0, 0, 1, 0, 0);
  nspire_padlog_add(PL_CYCLE, 1, 2, 0, (uint32_t)-1, 0, 0);   /* R10a: the link's own (LINK2 stuck host) */
  nspire_padlog_add(PL_VERDICT, PV_HOST_STOPPED, PV_EMPTY_ADAPTER, 0, 0, 0, 0);
  nspire_padlog_text(text, sizeof(text), "test");
  CHECK(strstr(text, "OS offer #1: 0451:e012 device 00/00/00 -> refused: a TI calculator (-2)") &&
        strstr(text, "OS offer #2: 045e:028e interface ff/5d/01 -> taken (5)") &&
        strstr(text, "driver: attach (connected), slot 0, 045e:028e") && strstr(text, "owner: PAD (was WAIT)") &&
        strstr(text, "arbiter: power restart 1 -> 1 (power back)") && strstr(text, "link: power restart 1 -> -1 (nothing to restart)") &&
        strstr(text, "status: host_stopped: A device is on the adapter but the OS hasn't started USB"),
        "pad log: the events' words:\n%s", text);
  CHECK(strstr(text, "Now: ") && strstr(text, "  Port: ") && strstr(text, "  raw: "), "pad log: the page's lines at the top");
  nspire_padlog_add(PL_DRIVER_ATTACH, 5, 0x81, 1, 0x057e2009u, 11, 4);
  nspire_padlog_add(PL_DRIVER_OUT, 1, 5, 255, 0x057e2009u, 3, 1);
  nspire_padlog_text(text, sizeof(text), "driver");
  CHECK(strstr(text, "attach #4: 057e:2009 step 5 in 81 out 01 error 11") &&
        strstr(text, "output: 057e:2009 endpoint 01 errors 3 refusal 5 completion none"), "pad log: driver failure survives: %s", text);
  /* A try's end: written at the next write point; not again for nothing new. */
  nspire_padlog_try_end(PL_TRY_NO_OFFER);
  nspire_padlog_write_point();
  CHECK(nspire_padlog_writes() == 1 && file_has(PADLOG_FILE, "pad log written the OS offered nothing in 12 s") == 1 &&
        file_has(PADLOG_FILE, "try ended: the OS offered nothing in 12 s") == 1, "pad log: a try's end is written (%u)",
        nspire_padlog_writes());
  nspire_padlog_write_point();
  CHECK(nspire_padlog_writes() == 1, "pad log: nothing due, no write");
  /* The timer: while failing and with news, every 10 s at most. */
  nspire_padlog_failing(1);
  nspire_padlog_add(PL_STAGE, 1, 0, 0, 0, 0, 0);
  unit_ticks += NSPIRE_TICK_HZ * 5;
  nspire_padlog_write_point();
  CHECK(nspire_padlog_writes() == 1, "pad log: the timer waits 10 s (%u)", nspire_padlog_writes());
  unit_ticks += NSPIRE_TICK_HZ * 6;
  nspire_padlog_write_point();
  CHECK(nspire_padlog_writes() == 2 && file_has(PADLOG_FILE, "on the timer") == 1, "pad log: the timer's write (%u)",
        nspire_padlog_writes());
  unit_ticks += NSPIRE_TICK_HZ * 30;
  nspire_padlog_write_point();
  CHECK(nspire_padlog_writes() == 2, "pad log: no news, no timer write (%u)", nspire_padlog_writes());
  nspire_padlog_failing(0);
  /* The ring keeps the last PADLOG_RING and says how many went. */
  for (i = 0; i < 300; i++)
    nspire_padlog_add(PL_STAGE, 2, 0, 0, 0, 0, 0);
  nspire_padlog_text(text, sizeof(text), "test");
  CHECK(nspire_padlog_count() == PADLOG_RING && strstr(text, "256 kept, ") && !strstr(text, "256 kept, 0 older"), "pad log: the ring: %u",
        nspire_padlog_count());
  /* Exit writes; the next run's first write keeps this one as _prev. */
  nspire_padlog_exit();
  CHECK(file_has(PADLOG_FILE, "pad log written at exit") == 1, "pad log: written at exit");
  nspire_padlog_reset();
  nspire_padlog_add(PL_TRY_END, PL_TRY_PAD, 0, 0, 0, 0, 0);
  nspire_padlog_exit();
  CHECK(file_has(PADLOG_PREV, "pad log written at exit") == 1 && file_has(PADLOG_FILE, "try ended: a controller attached") == 1,
        "pad log: the last run's file kept as %s", PADLOG_PREV);
  /* R2: a ring the idle lines pushed every try out of never replaces a
   * file that has one (the exit's write after minutes of a CX's flips). */
  /* (A try that worked: its lines aren't held, the idle lines push them out.) */
  nspire_padlog_reset();
  nspire_padlog_try_end(PL_TRY_PAD);
  nspire_padlog_write_point();
  for (i = 0; i < 300; i++)
    nspire_padlog_add(PL_LINK, i & 1, !(i & 1), 0, 0, 0, 0);
  nspire_padlog_exit();
  CHECK(file_has(PADLOG_FILE, "try ended: a controller attached") == 1 && file_has(PADLOG_FILE, "at exit") == 0,
        "pad log: a ring with nothing but idle lines keeps the try's file");
  /* R10b: a try that failed: its lines are held in the ring through the
   * idle lines, so the exit's write has them still (R10a's link log lost
   * its failed tries' lines). */
  nspire_padlog_reset();
  nspire_padlog_add(PL_OFFER, 13, 1, 0, 0x057e2009u, 0x030000u, 6);
  nspire_padlog_add(PL_EVENT, 5, 6, 0, 0x057e2009u, PL_EVENT_FACTS | 6 | 0x01 << 8, 19);
  nspire_padlog_try_end(PL_TRY_FAILED);
  nspire_padlog_write_point();
  CHECK(nspire_padlog_kept() == 3, "pad log: the failed try's 3 entries held (%u)", nspire_padlog_kept());
  for (i = 0; i < 300; i++)
    nspire_padlog_add(PL_LINK, i & 1, !(i & 1), 0, 0, 0, 0);
  CHECK(nspire_padlog_count() == PADLOG_RING && nspire_padlog_kept() == 3, "pad log: held through 300 idle lines (%u, %u)",
        nspire_padlog_count(), nspire_padlog_kept());
  nspire_padlog_exit();
  CHECK(file_has(PADLOG_FILE, "at exit") == 1 && file_has(PADLOG_FILE, "try ended: the driver gave up on it") == 1 &&
        file_has(PADLOG_FILE, "OS offer #6: 057e:2009 interface 03/00/00 -> taken (13)") == 1 &&
        file_has(PADLOG_FILE, "driver: not supported, slot -1, 057e:2009, kind 6; failed at step 6, ep 01, error 19") == 1,
        "pad log: the exit's write has the failed try's lines");
  /* ...a try that worked isn't held; the newest tries' are, PADLOG_KEEP_MAX at most */
  nspire_padlog_reset();
  nspire_padlog_add(PL_STAGE, 1, 0, 0, 0, 0, 0);
  nspire_padlog_try_end(PL_TRY_PAD);
  CHECK(nspire_padlog_kept() == 0, "pad log: a working pad's try isn't held (%u)", nspire_padlog_kept());
  for (i = 0; i < 40; i++)
  {
    int j;
    for (j = 0; j < 9; j++)
      nspire_padlog_add(PL_STAGE, 3, 0, 0, 0, 0, 0);
    nspire_padlog_try_end(PL_TRY_UNPLUGGED);
  }
  CHECK(nspire_padlog_kept() <= PADLOG_KEEP_MAX && nspire_padlog_kept() >= PADLOG_KEEP_MAX - 10 &&
        nspire_padlog_count() == PADLOG_RING, "pad log: held entries bounded (%u)", nspire_padlog_kept());
  for (i = 0; i < 2 * PADLOG_RING; i++)
    nspire_padlog_add(PL_LINK, i & 1, !(i & 1), 0, 0, 0, 0);
  nspire_padlog_text(text, sizeof(text), "test");
  CHECK(nspire_padlog_count() == PADLOG_RING && strstr(text, "try ended: the device was unplugged with nothing working"),
        "pad log: the newest failed tries outlast the idle lines");
  /* R10b: the driver's trace records (PL_TRACE) and a failed bring-up's
   * facts, in the weak defaults' numbers (the glue's words: pad_tests) */
  nspire_padlog_reset();
  {
    uint32_t w[6] = { 0x057e2009u, 2, 0x01u << 16 | 2, 7, 120, 3 };
    nspire_padlog_add6(PL_TRACE, 6, 'U', 1, w);
  }
  nspire_padlog_add(PL_HC, 1, 0, 0, 68, 0x00080000u, 0x0000088du);
  nspire_padlog_add(PL_HC, 0, 0, 0, 98000, 0x00080031u, 0x0000088du);
  /* GpSP's USB audio left a DualSense's audio
   * interfaces alone (nspire_audio_os.c): one line per change of the count. */
  {
    unsigned n0;
    nspire_padlog_poll();   /* the port's sample (then none for 100 ms: the clock stands) */
    n0 = nspire_padlog_count();
    nspire_padlog_poll();
    CHECK(nspire_padlog_count() == n0, "pad log: no audio decline, no line");
    unit_audio_declined = 2;
    nspire_padlog_poll();
    nspire_padlog_poll();
    CHECK(nspire_padlog_count() == n0 + 1, "pad log: one line for the audio declines (%u)", nspire_padlog_count() - n0);
  }
  nspire_padlog_text(text, sizeof(text), "test");
  CHECK(strstr(text, "audio: left 054c:0ce6 interface 2 alone: a game controller, the pad driver's (gpSP's USB audio "
                     "declined 2 interface(s) so far)") != NULL, "pad log: the audio decline's words:\n%s", text);
  CHECK(strstr(text, "bring-up #7: U p0 057e:2009 step 6 ep 01 err 2 arg 2") &&
        strstr(text, "host controller STOPPED 68 ms after the OS offered this device and no pad came of it (USBCMD 00080000 "
                     "PORTSC 0000088d)") &&
        strstr(text, "host controller runs again, 98000 ms after it stopped (USBCMD 00080031"),
        "pad log: trace and host controller lines:\n%s", text);
  /* R8: a write that can't open its file starts the timers too: not tried
   * again every frame (a full storage). Its path a folder: fopen fails. */
  nspire_padlog_reset();
  nspire_padlog_try_end(PL_TRY_GAVE_UP);
  nspire_padlog_write_point();   /* the run's first write (the last run's file to _prev) */
  nspire_save_file(path, sizeof(path), PADLOG_FILE);
  remove(path);
  mkdir(path, 0777);
  nspire_padlog_failing(1);
  nspire_padlog_add(PL_STAGE, 1, 0, 0, 0, 0, 0);
  unit_ticks += NSPIRE_TICK_HZ * 11;
  nspire_padlog_write_point();   /* the timer's: can't open */
  rmdir(path);
  nspire_padlog_add(PL_STAGE, 2, 0, 0, 0, 0, 0);
  unit_ticks += NSPIRE_TICK_HZ / 60;
  nspire_padlog_write_point();
  CHECK(file_has(PADLOG_FILE, "gpSP") == -1, "pad log: a failed open waits for the timer, not the next frame");
  unit_ticks += NSPIRE_TICK_HZ * 11;
  nspire_padlog_write_point();
  CHECK(file_has(PADLOG_FILE, "on the timer") == 1, "pad log: the timer's write after a failed open");
  /* R11t: while a sound adapter plays, no log file is written (a write
   * stalls the USB callbacks longer than the queued sound): the pad log's
   * timer and asked-for writes and every link log flush wait; a try that
   * ends in a failure is still written; all of it once the adapter is gone. */
  {
    unsigned w0, held0 = nspire_link_log_audio_held();
    nspire_padlog_add(PL_STAGE, 3, 0, 0, 0, 0, 0);
    unit_ticks += NSPIRE_TICK_HZ * 11;
    unit_audio_claim = 1;
    w0 = nspire_padlog_writes();
    nspire_padlog_write_point();
    CHECK(nspire_padlog_writes() == w0, "pad log: no timer write while the sound adapter plays (%u)", nspire_padlog_writes());
    nspire_padlog_try_end(PL_TRY_PAD);
    unit_ticks += NSPIRE_TICK_HZ * 11;
    nspire_padlog_write_point();
    CHECK(nspire_padlog_writes() == w0, "pad log: a try that worked isn't written while it plays (%u)", nspire_padlog_writes());
    nspire_padlog_try_end(PL_TRY_FAILED);
    nspire_padlog_write_point();
    CHECK(nspire_padlog_writes() == w0 + 1, "pad log: a try's failure is written even while it plays (%u)",
          nspire_padlog_writes());
    nspire_link_log("R11t: a line while the sound adapter plays");
    nspire_link_log_flush();
    CHECK(nspire_link_log_audio_held() == held0 + 1 && file_has("gpsp_link.txt.tns", "R11t: a line while") != 1,
          "link log: held in memory while the sound adapter plays (held %u)", nspire_link_log_audio_held() - held0);
    unit_audio_claim = 0;
    nspire_link_log_flush();
    CHECK(file_has("gpsp_link.txt.tns", "R11t: a line while") == 1, "link log: written once the adapter is gone");
    nspire_padlog_add(PL_STAGE, 4, 0, 0, 0, 0, 0);
    unit_ticks += NSPIRE_TICK_HZ * 11;
    nspire_padlog_write_point();
    CHECK(nspire_padlog_writes() == w0 + 2, "pad log: the timer writes again once the adapter is gone (%u)",
          nspire_padlog_writes());
  }
  nspire_padlog_failing(0);
  nspire_padlog_reset();
  unit_usb_raw_kind = 0;
  unit_usb_plugged = 0;
  memset(unit_usb_raw, 0, sizeof(unit_usb_raw));
}

/* ---- R12: the CX II's time base (nspire_tickrate.c tick_base, tick_follow) ---- */
#include "nspire_tickrate.h"

/* A count at hz (true) read every step_us (+ up to 1 ms, now and then a
 * 25 ms frame without a read), the start's estimate off by ppm; at
 * switch_at the speed goes to hz * factor and tick_base_recheck runs (a CPU
 * clock switch); clock_set: the real-time clock set an hour on at 8 s. Each
 * read's ticks: never back, never more than its time at the speed in use
 * (no jump at a refinement). 10 s after the start (or the switch): the
 * speed in use within 0.05% (500 ppm) of the true one, and (no switch,
 * time_ppm) the ticks within time_ppm of the true time. */
static void tb_run(const char *name, double hz, double ppm, unsigned step_us, double seconds, double switch_at,
                   double factor, int clock_set, double time_ppm)
{
  double t = 0, counts = 0, rate = hz, worst_rate = 0, worst_time = 0, settled = switch_at > 0 ? switch_at + 10 : 10, in_use;
  uint32_t last = 0, ticks, rtc;
  unsigned i;
  int back = 0, jumps = 0, switched = 0;

  tick_base_start(0, (uint32_t)(hz * (1 + ppm / 1e6) + 0.5), 0);
  for (i = 0; t < seconds; i++)
  {
    double dt = (step_us + (i * 7919u % 1000u)) / 1e6 + (i % 97 == 0 ? 0.025 : 0);
    t += dt;
    counts += dt * rate;
    if (switch_at > 0 && !switched && t >= switch_at)
    {
      switched = 1;
      rate = hz * factor;
      tick_base_recheck();
    }
    rtc = 1700000000u + (uint32_t)t + (clock_set && t >= 8 ? 3600 : 0);
    in_use = (double)tick_base_hz_q16() / 65536.0;   /* (this read's ticks are at the speed before it) */
    ticks = tick_base_ticks((uint64_t)counts, rtc);
    if (i && (int32_t)(ticks - last) < 0)
      back++;
    else if (i && ticks - last > dt * 32768.0 * rate / in_use * 1.0001 + 2)
      jumps++;
    last = ticks;
    if (t >= settled)
    {
      double r = fabs((double)tick_base_hz_q16() / 65536.0 - rate) / rate * 1e6;
      double e = fabs((double)ticks - t * 32768.0) / (t * 32768.0) * 1e6;
      if (r > worst_rate)
        worst_rate = r;
      if (e > worst_time)
        worst_time = e;
    }
  }
  CHECK(back == 0, "time base %s: the ticks went back %d times", name, back);
  CHECK(jumps == 0, "time base %s: %d reads moved the ticks by more than their time", name, jumps);
  CHECK(worst_rate <= 500, "time base %s: the speed in use %.0f ppm off 10 s in (500 at most)", name, worst_rate);
  CHECK(time_ppm <= 0 || worst_time <= time_ppm, "time base %s: the time %.0f ppm off 10 s in (%.0f at most)", name,
        worst_time, time_ppm);
  printf("  time base %s: %.0f ppm speed, %.0f ppm time after 10 s; %u refinements, %u disagreements\n", name, worst_rate,
         worst_time, (unsigned)tick_base_refinements(), (unsigned)tick_base_disagreements());
}

static void test_time_base(void)
{
  char text[600];

  /* off-nominal clocks, the CPU loop's estimate a little off: 0.1% at 10 s */
  tb_run("10 MHz", 10000000.0, 1000, 300, 30, 0, 1, 0, 1000);
  tb_run("12 MHz", 12000000.0, -3000, 2000, 30, 0, 1, 0, 0);
  tb_run("12 MHz, 0.1% start", 12000000.0, -1000, 2000, 30, 0, 1, 0, 1000);
  tb_run("32.768 MHz", 32768000.0, 500, 300, 30, 0, 1, 0, 1000);
  tb_run("32 kHz", 32768.0, 0, 300, 30, 0, 1, 0, 1000);
  /* the start's estimate far off (the clock register wrong): two stretches
   * agreeing on the speed make it */
  tb_run("12 MHz, start 20% off", 12000000.0, 200000, 300, 40, 0, 1, 0, 0);
  /* a CPU clock switch that changes the count's speed: 3% and 25% */
  tb_run("switch +3%", 10000000.0, 0, 300, 40, 12, 1.03, 0, 0);
  tb_run("switch +25%", 10000000.0, 0, 300, 40, 12, 1.25, 0, 0);
  /* the real-time clock set while it runs */
  tb_run("clock set", 12000000.0, 1000, 300, 40, 0, 1, 1, 0);
  tick_base_report(text, sizeof(text));
  CHECK(strstr(text, "counts/s in use") && strstr(text, "refinement(s)"), "time base: its report: %s", text);
}

static void test_tick_follow(void)
{
  struct tick_follow f;
  uint32_t v, s = 1700000000u, load = 0x04FFFFFFu;   /* periodic, 8.4 s at 10 MHz */
  uint64_t total = 0, want;
  unsigned i;

  /* a periodic count read every 1 ms (10000 counts) across many wraps */
  v = load;
  tick_follow_start(&f, v, 10000000u, s);
  for (i = 0; i < 30000; i++)
  {
    uint32_t step = 10000;
    v = v >= step ? v - step : load - (step - v - 1);
    if (i % 1000 == 999)
      s++;
    total = tick_follow_step(&f, v, load, s);
  }
  CHECK(total == 300000000ull && !f.jumps && !f.lost_wraps, "follow: periodic, 30 s: %llu counts (300000000), %u jumps",
        (unsigned long long)total, (unsigned)f.jumps);
  /* rewritten by the OS (its load again): a jump, no time made of it */
  want = total;
  v = load;
  total = tick_follow_step(&f, v, load, s);
  CHECK(f.jumps == 1 && total == want, "follow: a rewrite is a jump (%u) and moves nothing (%llu)", (unsigned)f.jumps,
        (unsigned long long)(total - want));
  /* rewritten lower by more than the real time that can have passed */
  v -= 0x02000000u;
  total = tick_follow_step(&f, v, load, s);
  CHECK(f.jumps == 2 && total == want, "follow: a jump down too (%u, %llu)", (unsigned)f.jumps,
        (unsigned long long)(total - want));
  /* a 10 s gap (two periods went by unseen): their counts come back */
  v -= 1000;
  s += 10;
  total = tick_follow_step(&f, v - (uint32_t)(100000000ull % ((uint64_t)load + 1)), load, s);
  CHECK(f.lost_wraps == 1 && total - want >= 90000000ull && total - want <= 110000000ull,
        "follow: a 10 s gap's whole periods (%u lost wraps, %llu counts)", (unsigned)f.lost_wraps,
        (unsigned long long)(total - want));
  /* a free-running count across 2^32 */
  v = 0x00001000u;
  tick_follow_start(&f, v, 33000000u, s);
  total = tick_follow_step(&f, 0xFFFFF000u, 0xFFFFFFFFu, s);
  CHECK(total == 0x2000 && !f.jumps, "follow: free running past 0: %llu (8192)", (unsigned long long)total);
  /* a stopped count: still */
  tick_follow_step(&f, 0xFFFFF000u, 0xFFFFFFFFu, s + 1);
  tick_follow_step(&f, 0xFFFFF000u, 0xFFFFFFFFu, s + 3);
  CHECK(f.still >= 2, "follow: a stopped count is still (%u s)", (unsigned)f.still);
  /* the pad log's line for the timers at a come-up's step */
  {
    uint32_t w[6] = { 0xE2, 0x7FFF, 327680, 1, 0x82, 327680 };
    static char log[16384];
    nspire_padlog_reset();
    nspire_padlog_add6(PL_TIMER, 1, 0, 0, w);
    nspire_padlog_text(log, sizeof(log), "unit");
    CHECK(strstr(log, "timers (the device is connected): 900c0020 control e2, load 00007fff, 32768.0 counts a ms; "
                      "select 900c0080 00000001; 900c0000 control 82, gpSP's count 32768.0 a ms") != NULL,
          "pad log: the timers' line: %s", strstr(log, "timers (") ? strstr(log, "timers (") : log);
    CHECK(strstr(log, "time base: unit test") != NULL, "pad log: the time base's header line");
    nspire_padlog_reset();
  }
}

/* R11w part 2 (2026-11-15): the message line's one rule (nspire_osd.c), the
 * USB side's words (at most 50 characters a toast, the page's top in two
 * lines, no register or driver words there). */
static void test_r11w_messages(void)
{
  static const char *const toasts[] = {
    PADSTATUS_TOAST_TEXT, ARB_REPLUG_TEXT, "The link cable keeps resetting: swap its ends",
    "Controller stopped. Plug it in again.", "DualShock 4 didn't start. Plug it in again.",
    "This controller isn't supported", "Controller needs more power: try a powered hub",
    "Plug the controller in again", "Controller connected (DualShock 4)", "Controller disconnected",
    "USB device in: connecting...", "Starting the USB device...", "Volume 100%", "USB drive removed; game is in memory",
  };
  static const char *const tech[] = { "PORTSC", "USBCMD", "driver", "OS USB", "host controller", "enumerat", "OTGCSR" };
  struct osd_line l;
  unsigned i, k;

  for (i = 0; i < sizeof(toasts) / sizeof(toasts[0]); i++)
    CHECK(strlen(toasts[i]) <= 50 && ui_text_width(toasts[i], &ui_font_osd) <= 316 - 30,
          "messages: \"%s\": %u characters, %d px (<= 50, fits the chip)", toasts[i], (unsigned)strlen(toasts[i]),
          ui_text_width(toasts[i], &ui_font_osd));
  /* the kinds */
  CHECK(nspire_osd_kind(PADSTATUS_TOAST_TEXT) == OSD_ACTION && nspire_osd_kind(ARB_REPLUG_TEXT) == OSD_ACTION &&
        nspire_osd_kind("The link cable keeps resetting: swap its ends") == OSD_ACTION &&
        nspire_osd_kind("Controller stopped. Plug it in again.") == OSD_ACTION,
        "messages: the ones that ask something are ACTION");
  CHECK(nspire_osd_kind("USB device in: connecting...") == OSD_WAIT && nspire_osd_kind("Starting the USB device...") == OSD_WAIT,
        "messages: the waits");
  CHECK(nspire_osd_kind("Saved to slot 3") == OSD_INFO && nspire_osd_kind("Volume 70%") == OSD_INFO &&
        nspire_osd_kind("Controller connected (Xbox 360)") == OSD_INFO, "messages: information");
  /* the times: info 2.5 s, an action 5 s */
  memset(&l, 0, sizeof(l));
  osd_line_post(&l, "Saved to slot 3", 1000);
  CHECK(osd_line_visible(&l, 3400) && !osd_line_visible(&l, 3600), "messages: information 2.5 s");
  osd_line_post(&l, ARB_REPLUG_TEXT, 10000);
  CHECK(osd_line_visible(&l, 14900) && !osd_line_visible(&l, 15100), "messages: an action 5 s");
  /* a newer, less important one waits for the action's time, then shows */
  memset(&l, 0, sizeof(l));
  osd_line_post(&l, PADSTATUS_TOAST_TEXT, 0);
  CHECK(osd_line_post(&l, "Volume 70%", 1000) == 0 && !strcmp(osd_line_visible(&l, 1000), PADSTATUS_TOAST_TEXT),
        "messages: an information doesn't push out an action before its time");
  CHECK(osd_line_visible(&l, 4900) && !strcmp(osd_line_visible(&l, 4900), PADSTATUS_TOAST_TEXT) &&
        osd_line_visible(&l, 5000) && !strcmp(osd_line_visible(&l, 5000), "Volume 70%"),
        "messages: ...it shows once the action's time is up");
  /* an action replaces an action; the state over: taken down at once */
  osd_line_post(&l, ARB_REPLUG_TEXT, 6000);
  CHECK(!strcmp(osd_line_visible(&l, 6000), ARB_REPLUG_TEXT), "messages: a newer action shows at once");
  osd_line_withdraw(&l, ARB_REPLUG_TEXT);
  CHECK(!osd_line_visible(&l, 6100), "messages: withdrawn when its state changed");
  /* a wait stays while it is kept up, an information after it at once */
  memset(&l, 0, sizeof(l));
  osd_line_post(&l, "USB device in: connecting...", 0);
  for (k = 1; k <= 20; k++)
    osd_line_extend(&l, k * 500);
  CHECK(osd_line_visible(&l, 12000) != NULL, "messages: a wait stays while it lasts (10 s)");
  CHECK(osd_line_post(&l, "Controller connected (Xbox 360)", 12000) == 1, "messages: the wait's outcome shows at once");
  /* an old waiting one is dropped, not shown late */
  memset(&l, 0, sizeof(l));
  osd_line_post(&l, ARB_REPLUG_TEXT, 0);
  osd_line_post(&l, "Saved to slot 1", 500);
  CHECK(osd_line_visible(&l, 5000) && !strcmp(osd_line_visible(&l, 5000), "Saved to slot 1"),
        "messages: the waiting one shows after the action (4.5 s old)");
  memset(&l, 0, sizeof(l));
  osd_line_post(&l, ARB_REPLUG_TEXT, 0);
  osd_line_post(&l, "Saved to slot 1", 0);
  osd_line_withdraw(&l, ARB_REPLUG_TEXT);
  CHECK(!osd_line_visible(&l, 5100), "messages: one waiting longer than 5 s is dropped");
  /* the Controller page's top: two lines, plain */
  for (i = 0; i < PV_COUNT; i++)
  {
    const char *p = nspire_padstatus_plain((enum pad_verdict)i);
    CHECK(p[0] && ui_wrap_lines(UI_W - 24, 2, p, &ui_font_small_strong) <= 2, "page top: %s: two lines at most",
          nspire_padstatus_name((enum pad_verdict)i));
    for (k = 0; k < sizeof(tech) / sizeof(tech[0]); k++)
      CHECK(!strstr(p, tech[k]), "page top: %s: no technical word (%s)", nspire_padstatus_name((enum pad_verdict)i), tech[k]);
  }
}

/* R11w part 2: the flap hint only while Link is picked (diag_tests.py has
 * the Link-off half on gpsp-host). The cable's plain end flapping to empty
 * four times, 1.5 s apart. */
static void flap_rounds(u32 bits)
{
  int k;

  for (k = 0; k < 4; k++)
  {
    unit_usb_plugged = 1;
    unit_usb_bits_set = bits;
    arb_frames(90);
    unit_usb_plugged = 0;
    unit_usb_bits_set = 0;
    arb_frames(8);
  }
  unit_usb_plugged = 1;
  unit_usb_bits_set = bits;
  arb_frames(10);
}

/* R11w part 2: the flap hint only for R2's shape (the cable's plain end
 * with a device flapping to nothing), not the adapter's end (a controller
 * pulled and put back), not an empty CX II port's off-and-on */
static void test_r11w_flap(void)
{
  comeup_quiet();
  arb_setup(0);
  flap_rounds(0x20000100u);   /* the adapter's end, a device connected */
  CHECK(!strstr(unit_osd_log, "swap"), "flap: the adapter's end: no toast (%s)", unit_osd_log);
  comeup_quiet();
  arb_setup(0);
  flap_rounds(0x20000031u);   /* a CX II's empty port */
  CHECK(!strstr(unit_osd_log, "swap"), "flap: an empty port: no toast (%s)", unit_osd_log);
  comeup_quiet();
  arb_setup(0);
  flap_rounds(0x20000020u);   /* the plain end, a device on it */
  CHECK(strstr(unit_osd_log, "The link cable keeps resetting: swap its ends") != NULL, "flap: the plain end: the toast (%s)",
        unit_osd_log);
  comeup_quiet();
}

/* fast17: On-chip memory's crash marker (gpsp_sram.run) says when
 * its run started; a clean exit after that (the exit marker's "clean at
 * clock", an older build's run between) makes it stale: not a crash. */
static void put_text(const char *name, const char *text)
{
  char path[700];
  FILE *f;
  nspire_save_file(path, sizeof(path), name);
  f = fopen(path, "w");
  if (f)
  {
    fputs(text, f);
    fclose(f);
  }
}

static int saves_has(const char *name)
{
  char path[700];
  FILE *f;
  nspire_save_file(path, sizeof(path), name);
  f = fopen(path, "rb");
  if (f)
    fclose(f);
  return f != NULL;
}

static void test_sram_marker(void)
{
  int was = host_is_cx2;

  host_is_cx2 = 1;
  put_text("gpsp_link_run.txt.tns", "clean at clock 200 s, build x\n");
  put_text("gpsp_sram.run", "gpSP runs its hot code ...\nstarted at clock 100 s\n");
  nspire_file_list_start();
  nspire_sram_code = 1;
  CHECK(nspire_sram_start() == 0 && nspire_sram_code == 1 && !saves_has("gpsp_sram.run"),
        "sram: a marker from before a clean exit is stale: gone, On-chip memory stays on (%u)", nspire_sram_code);
  put_text("gpsp_sram.run", "gpSP runs its hot code ...\nstarted at clock 300 s\n");
  nspire_file_list_start();
  nspire_sram_code = 1;
  CHECK(nspire_sram_start() == 1 && nspire_sram_code == 0 && !saves_has("gpsp_sram.run"),
        "sram: a marker after the last clean exit is a crash: off (%u)", nspire_sram_code);
  put_text("gpsp_sram.run", "gpSP runs its hot code ...\n");
  nspire_file_list_start();
  nspire_sram_code = 1;
  CHECK(nspire_sram_start() == 1 && nspire_sram_code == 0, "sram: a marker without its clock is a crash");
  put_text("gpsp_sram.tns", "an r16 marker\n");
  nspire_file_list_start();
  nspire_sram_code = 1;
  CHECK(nspire_sram_start() == 1 && nspire_sram_code == 0 && !saves_has("gpsp_sram.tns"),
        "sram: r16's gpsp_sram.tns is a crash, and goes");
  nspire_sram_code = 0;
  host_is_cx2 = was;
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    fprintf(stderr, "usage: frontend_tests DIR\n");
    return 2;
  }
  snprintf(dir, sizeof(dir), "%s", argv[1]);
  snprintf(unit_documents_dir, sizeof(unit_documents_dir), "%s/", dir);
  nspire_save_dir_make();

  printf("turning frames\n");
  test_turn_raw();
  test_turn_1x();
  test_turn_rects();
  printf("key bindings\n");
  test_keys();
  printf("settings files\n");
  test_config();
  test_config_versions();
  test_pace_rule_mark();
  printf("settings in the program file\n");
  test_settings_in_program();
  printf("slot keys\n");
  test_slot_keys();
  printf("brightness\n");
  test_brightness();
  printf("file list order\n");
  test_game_list();
  printf("turbo\n");
  test_turbo();
  printf("the link's device\n");
  test_link();
  printf("linking on its own\n");
  test_usbport();
  test_usb_cycle();
  test_arbiter();
  test_arbiter_audio();
  test_arbiter_drive();
  test_arbiter_hub();
  test_arbiter_cycles();
  test_comeup_messages();
  test_r11w_flap();
  test_stuck_host();
  test_link_watch();
  printf("the Controller status and the pad log\n");
  test_padstatus();
  test_r11w_messages();
  test_padlog();
  printf("the other's menu, after its link\n");
  test_link_menu_stale();
  printf("the hello and the link two GBAs choose\n");
  test_link_choice();
  printf("link speed and the fast transport's off switch\n");
  test_link_speed();
  printf("Fire Emblem's cable\n");
  test_link_fe();
  printf("MultiSio's cable\n");
  test_link_msio();
  printf("the other side's garbage (fuzz)\n");
  test_link_fuzz();
  printf("cpu speed and freeze marker\n");
  test_clock();
  test_sram_marker();
  test_cpu_speed_auto();
  printf("R12: the CX II's time base\n");
  test_time_base();
  test_tick_follow();
  printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
