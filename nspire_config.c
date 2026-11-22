/* gpSP's settings: the ones for all games, kept inside the program file
 * (nspire_selfstore.h) with a copy, gpsp.cfg.tns, in the saves folder
 * (nspire_paths.h), and a game's own settings and keys, "<ROM
 * name>.settings.tns" in the saves folder. They're read from the program
 * file, else from the copy (a new _gpSP.tns copied over the old one), else
 * from gpsp.cfg.tns or gpsp_qol.cfg.tns (its name before) next to gpSP
 * (builds before the saves folder; settings files next to gpSP move into
 * the saves folder at start, so its folder has only the program and the
 * games). Older builds' gpsp.cfg.tns has an older format; word 37 tells
 * which. The copy is only read, never written.
 *
 * gpsp.cfg.tns is gpSP's binary file of 32-bit words. Words 0-36 are as
 * gpSP has always written them (older, shorter files still load; word 36
 * is reserved: it held the removed video renderer choice, now 0); then:
 *   2       a save's OS calls (since 2026-11-21, final/r19-bank: nspire_audio.c
 *           "A save's OS calls"): SAVE_CALL_MARK in the top byte, the
 *           longest call the saves learned in ms in the low 16 bits; 0 until
 *           something was learned (older builds write 0 and never read it).
 *           Read eased a quarter, as a save with short calls eases it
 *   22      Bluetooth (since 2026-11-17, nspire_bt_glue.h): BT_MARK in the top
 *           byte, bit 0 on, bits 4-5 the sound's output (0 automatic, 1 the
 *           USB adapter, 2 Bluetooth), bits 8-9 the headphones' codec (0
 *           automatic, 1 mono, 2 stereo). Written only when not the
 *           defaults (on, automatic, automatic): a default file stays as it
 *           was; older builds never read it and write 0 (the defaults)
 *   37      "QOL3", marking what follows ("QOL2" files lack word 66)
 *   38-57   the key bindings of actions 0-19 (nspire_keys.h): the first
 *           binding in the low 16 bits, the second in the high 16 bits,
 *           each key | held key << 8
 *   58-61   auto-increment, save on exit, load on start, CPU speed
 *   62      the FPS counter (0 off, 1 on, 2 detailed)
 *   63      screen brightness (0 the calculator's own, 1-10)
 *   64-65   the key bindings of actions 20-21 (brightness up, down)
 *   66      compress saves (1, the default) or not
 *   67      fast forward's top speed in tenths (11-100: 1.1x-10.0x), 0
 *           max, no top speed (the default, and what older files have)
 *   68      what's in the link port (nspire_link.h: 0 auto, 1 the
 *           wireless adapter, 2 the link cable, 3 the link cable exact)
 *   69      0 (older builds: the old Turbo key's bindings, as 38-57)
 *   70      turbo: bits 8-15 the speed (presses per second - 1), bit 31
 *           set (files without it: 15 a second); bit 30 set: words 72-81
 *           hold the turbo bindings. Bits 0-3: the old turbo choice the
 *           keys were turned from (keys_old_turbo_mask's number; 0 none),
 *           kept for games whose own files still have the old scheme
 *   71      autosave: bits 0-3 how often (nspire_frontend.h's choice, 0
 *           off), bit 4 to a new slot (else the picked one), bit 31 set
 *           (files without it, whose word 71 was 0: off, a new slot);
 *           bit 5: saves on the USB drive (nspire_saves_on, nspire_places.h;
 *           older files have 0 there: the calculator);
 *           bit 12: 1 when Settings > Saving > Confirm delete is Off (0 by
 *           default: older builds write 0 and read it as before)
 *   72-81   the turbo bindings of the GBA buttons (actions 22-31, Turbo
 *           up to Turbo Select), as 38-57 (since 2026-11-12; files of 72
 *           words have the old turbo: words 69-70 are turned into turbo
 *           bindings, keys_turbo_from_old)
 *
 * Files from older builds, and from the first QoL build ("QOL1", without
 * words 63-65), start with the 320x214 picture: older builds showed it
 * whatever the scaling setting said, and the first QoL build took their
 * setting as it was. Its CPU speeds were normal, 432, 456 and 480 MHz.
 * Files before QOL3 get the slot keys, ) and (, where those keys are free.
 *
 * A game's own file is text, "name=value" lines, written only while the
 * game has its own settings or keys ("Settings for" / "Keys for: this
 * game"); "version=2" marks the ones written since the QOL1 build,
 * "version=3" the ones with the slot keys' defaults, and "version=4" the
 * ones with turbo bindings (key_turbo_a ...; before, a "turbo" choice and
 * a "key_turbo" Turbo key, turned into bindings when read). */

#include "common.h"
#include "gui.h"
#include "nspire.h"
#include "nspire_gui_video.h"
#include "nspire_link.h"
#include "nspire_frontend.h"
#include "nspire_keys.h"
#include "nspire_states.h"
#include "nspire_clock.h"
#include "nspire_brightness.h"
#include "nspire_paths.h"
#include "nspire_selfstore.h"
#include "nspire_frameskip.h"
#include "nspire_audio.h"
#include "nspire_bt_glue.h"
#include "nspire_sram.h"
#include "nspire_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CONFIG_WORDS 82
#define OLD_CONFIG_WORDS 72   /* before the turbo bindings (2026-11-12) */
#define QOL_MAGIC  0x334C4F51u   /* "QOL3" */
#define QOL2_MAGIC 0x324C4F51u   /* "QOL2", before word 66 and the slot keys */
#define QOL1_MAGIC 0x314C4F51u   /* "QOL1", the first QoL build's */
/* 4: turbo bindings (key_turbo_a ...), no turbo= line; 5: the pace rule's
 * mark (PACE_RULE_MARK): a frameskip_type of Off in a file before 5 is
 * Automatic once */
#define GAME_FILE_VERSION 5
#define GAME_FILE_END "end=1"   /* R6: a whole game file's last line */
/* Actions before brightness up/down, in words 38-57; then those two, in
 * words 64-65; the turbo actions in words 72-81. */
#define FIRST_ACTIONS 20
#define QOL2_ACTIONS 22
#define TURBO_WORD 72
/* Word 70: the old turbo choice (bits 0-3), the speed (bits 8-15), this
 * mark, and the turbo bindings' mark. */
#define TURBO_MARK 0x80000000u
#define TURBO_BINDINGS_MARK 0x40000000u
/* Word 71: autosave (bits 0-3), to a new slot (bit 4), and this mark. */
#define AUTOSAVE_MARK 0x80000000u
/* Top byte of word 26: gpSP's newer frameskip encoding (auto/manual/off). */
#define FS_V2_MARK 0xFF000000u
/* Word 26 bit 0 (2026-11-15, final/pace): written by every build since the
 * pace rule (nspire_frameskip.c). A file without it that says frameskip Off
 * gets Automatic, the rule, once (on calculators where older builds stored
 * Off as the old default's choice, not a player's); the next write has the
 * mark, so an Off chosen after that stays Off. The word's other bits were 0;
 * older builds read only its top byte. A game's own file: GAME_FILE_VERSION 5. */
#define PACE_RULE_MARK 0x00000001u
/* Word 69: nspire_sram_code (0 or 1), marked with 0x5A in its top byte (since
 * turbo bindings moved to words 72-81, older builds wrote 0 there). */
#define SRAM_MARK 0x5A000000u
/* Word 69 bit 8 (2026-11-17, final/speed5): written by a build where On-chip
 * memory is on by default (it works on the CX II, so it is used fully
 * there). A file without it holds an older build's value, where off was the
 * default and so what nearly every file says: it is read as on, once; the
 * next write has the mark, so an off chosen after that stays off. On a CX it
 * is never on (nspire_sram_start), and a run with it on that didn't end
 * turns it off and saves that (the marker file). */
#define SRAM_ON_MARK 0x00000100u

u32 nspire_auto_increment = 1;
u32 nspire_save_on_exit = 1;
u32 nspire_autosave = 0;            /* off */
u32 nspire_autosave_new_slot = 1;   /* nothing overwritten */
/* Saving's "Saves on" (nspire_places.h: PLACE_CALCULATOR 0, PLACE_DRIVE 1;
 * 2026-11-17): kept here, bit 5 of the autosave word, and in a game's own
 * settings. (weak: the menus' nspire_devices.c has it; a build without the
 * menus, the PC's unit tests, has this one.) */
u32 nspire_saves_on __attribute__((weak));
#define SAVES_DRIVE_BIT 0x20u
/* The menus' colours (nspire_ui.h's themes): bits 8-10 of the autosave
 * word when THEME_MARK is set there, so the settings keep their size and
 * a build without themes reads the word as before. */
#define THEME_MARK 0x40000000u
u32 nspire_ui_theme = 0;
u32 nspire_load_on_start = 1;
u32 nspire_cpu_speed = CPU_SPEED_AUTO;
u32 nspire_compress_saves = 1;
/* ui18: Settings > Saving > Confirm delete (1: ask, the default; 0: off, bit 12
 * of word 71, 0 in older files). */
u32 nspire_confirm_delete = 1;
/* (final/r19-bank) the longest OS call of a save learned, ms (word 2): nspire_audio.c's bank */
u32 nspire_save_call_ms;
#define SAVE_CALL_MARK 0x5C000000u
#define CONFIRM_DELETE_OFF 0x1000u
u32 nspire_audio_enabled = 1, nspire_audio_volume = NSPIRE_VOLUME_DEFAULT;
u32 nspire_audio_mono = 0, nspire_audio_buffer = 0;
#define AUDIO_MARK 0xA5000000u
/* r19: the volume's 21 steps in word 3's bits 16-20, said to be there by bit 21; bits 4-7 keep the old eleven
 * steps (half of it, rounded) for a build before r19 that reads the file; a file without bit 21 has only those:
 * its volume is twice that. */
#define VOLUME20_MARK 0x00200000u
/* Bluetooth (word 22): nspire_bt_glue.h's settings. */
unsigned nspire_bt_enabled = 1, nspire_audio_output = NSPIRE_OUTPUT_AUTO, nspire_bt_codec = NSPIRE_BT_CODEC_AUTO;
#define BT_MARK 0xB7000000u
/* Fast forward's top speed: the Graphics menu's index (nspire_frameskip.h);
 * the files keep tenths (word 67). */
u32 nspire_ff_max = NSPIRE_FF_UNLIMITED;
/* Turbo (nspire_keys.h): 15 presses a second. */
u32 nspire_turbo_speed = TURBO_SPEED_DEFAULT;
/* The old turbo choice the settings for all games had (word 70 bits 0-3),
 * for a game's own file of the old scheme (nspire_config_open_game). */
static u32 old_turbo_choice;
/* The menu's "Settings for" and "Keys for": 0 all games, 1 this game. */
u32 nspire_settings_for;
u32 nspire_keys_for;

void nspire_cheats_open(const char *rom_path) __attribute__((weak));
void nspire_cheats_close(void) __attribute__((weak));

extern u32 update_backup_flag;
extern u32 global_enable_analog;
extern u32 analog_sensitivity_level;

/* The settings a game can have its own of. */
static const struct
{
  const char *name;
  u32 *value;
  u32 count;
} settings[] =
{
  { "audio_enabled", &nspire_audio_enabled, 2 },
  { "volume20", &nspire_audio_volume, NSPIRE_VOLUME_MAX + 1 },   /* (r19; "audio_volume", 0-10, is read as twice it) */
  { "audio_mono", &nspire_audio_mono, 2 },
  { "audio_buffer", &nspire_audio_buffer, 3 },
  { "screen_scale", &screen_scale, 4 },
  { "frameskip_type", &current_frameskip_type, 3 },
  { "frameskip_value", &frameskip_value, 100 },
  { "frameskip_random", &random_skip, 2 },
  { "show_fps", &nspire_fps_overlay, 3 },
  { "frame_mixing", &nspire_frame_mix_choice, 2 },
  { "gba_blending_off", &nspire_gba_blend_off, 2 },
  { "no_sprite_limit", &nspire_sprlim_choice, 2 },
  { "rtc", &nspire_rtc_choice, 3 },
  { "bios", &nspire_bios_choice, 2 },
  { "dynarec_smc", &nspire_dynarec_ram_policy, 2 },
  { "write_saves", &update_backup_flag, 2 },
  { "auto_increment", &nspire_auto_increment, 2 },
  { "save_on_exit", &nspire_save_on_exit, 2 },
  { "autosave", &nspire_autosave, AUTOSAVE_CHOICES },
  { "autosave_new_slot", &nspire_autosave_new_slot, 2 },
  { "saves_on_drive", &nspire_saves_on, 2 },
  { "load_on_start", &nspire_load_on_start, 2 },
  { "compress_saves", &nspire_compress_saves, 2 },
  { "confirm_delete", &nspire_confirm_delete, 2 },
  { "fast_forward_max", &nspire_ff_max, NSPIRE_FF_CHOICES },
  { "cpu_speed", &nspire_cpu_speed, CPU_SPEEDS },
  { "turbo_speed", &nspire_turbo_speed, TURBO_SPEEDS },
};

#define NUM_SETTINGS (int)(sizeof(settings) / sizeof(settings[0]))

static const char *const action_ids[NUM_ACTIONS] =
{
  "up", "down", "left", "right", "a", "b", "l", "r", "start", "select",
  "menu", "fast_forward", "load_state", "save_state", "save_exit",
  "load_newest", "next_slot", "previous_slot", "show_fps", "restart",
  "brighter", "dimmer",
  "turbo_up", "turbo_down", "turbo_left", "turbo_right", "turbo_a", "turbo_b", "turbo_l", "turbo_r",
  "turbo_start", "turbo_select"
};

/* What the old turbo turned into, in the link log (gpsp_link.txt.tns):
 * once, the first start of this build on settings of the old scheme. */
static void log_turbo_migration(const char *where, u32 choice, const key_binding old_key[KEY_SLOTS], int made)
{
  char k0[24], k1[24];
  nspire_link_log("keys: %s of the old turbo: choice %u (buttons %x), Turbo key %s / %s: %d turbo bindings",
                  where, (unsigned)choice, keys_old_turbo_mask(choice), binding_name(old_key[0], k0, sizeof(k0)),
                  binding_name(old_key[1], k1, sizeof(k1)), made);
}

/* The first QoL build's CPU speeds (normal, 432, 456, 480 MHz) in today's
 * list (nspire_clock.h); its normal was only the default, so it is auto. */
static u32 cpu_speed_from_qol1(u32 old)
{
  static const u32 now[4] = { CPU_SPEED_AUTO, 3, 5, 7 };
  return now[old % 4];
}

/* The CPU speed in word 61 and a game's cpu_speed line. Auto (the default)
 * is 0; a speed chosen in the menu, or normal after a freeze, is
 * CPU_SPEED_CHOSEN + its index. Builds before auto wrote the index alone,
 * with 0 (normal) the default: that 0 is auto now, any other is kept. The
 * mark is 1000, a multiple of 10, so those builds (index % 10) read a
 * chosen speed as it is and auto as normal (0x100 would read as 468 MHz). */
#define CPU_SPEED_CHOSEN 1000u

static u32 cpu_speed_word(u32 speed)
{
  speed %= CPU_SPEEDS;
  return speed == CPU_SPEED_AUTO ? 0 : CPU_SPEED_CHOSEN + speed;
}

static u32 cpu_speed_from_word(u32 word)
{
  if (word >= CPU_SPEED_CHOSEN)
    return (word - CPU_SPEED_CHOSEN) % CPU_SPEEDS;
  return word % 10 ? word % 10 : CPU_SPEED_AUTO;
}

/* The values for all games, while a game with its own is open. */
static u32 global_values[NUM_SETTINGS];
static key_binding global_keys[NUM_ACTIONS][KEY_SLOTS];
static int game_open, own_settings, own_keys;
/* The open game's file, kept from when it opened: by the time it closes,
 * gamepak_filename may already name the next game. */
static char game_file[600];

static void capture(u32 *values)
{
  int i;
  for (i = 0; i < NUM_SETTINGS; i++)
    values[i] = *settings[i].value;
}

static void apply(const u32 *values)
{
  int i;
  for (i = 0; i < NUM_SETTINGS; i++)
    *settings[i].value = values[i] % settings[i].count;
}

/* CONFIG_FILE (gpsp.cfg.tns) and OLD_CONFIG_FILE (gpsp_qol.cfg.tns, its
 * name before) are in nspire_paths.h. Older builds also wrote a
 * gpsp.cfg.tns, in an older format; the format is told by word 37. */

/* A settings file next to gpSP (builds before the saves folder). */
static void program_folder_path(char *path, const char *name)
{
  sprintf(path, "%s/%s", main_path, name);
}

/* R6: a settings file is written as "<its name>.tmp" and takes the name
 * once whole (the old one removed first: the calculator's rename doesn't
 * replace a file); written in place, a reset mid-write left half a file.
 * Before a read: a .tmp beside the file is a write that didn't finish
 * (dropped); a .tmp alone is the file if whole(it) says so. */
static void settings_tmp(char *tmp, size_t size, const char *path)
{
  snprintf(tmp, size, "%s.tmp", path);
}

static void recover_settings(const char *path, int (*whole)(const char *))
{
  char tmp[620];
  struct stat st;

  settings_tmp(tmp, sizeof(tmp), path);
  if (stat(tmp, &st) != 0)
    return;
  if (stat(path, &st) != 0 && whole(tmp) && rename(tmp, path) == 0)
    return;
  remove(tmp);
}

/* The .tmp written (ok) takes the file's name, or is dropped. */
static int settings_replace(const char *tmp, const char *path, int ok)
{
  if (!ok)
  {
    remove(tmp);
    return 0;
  }
  remove(path);
  return rename(tmp, path) == 0;
}

static int whole_settings_file(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0 && (st.st_size == CONFIG_WORDS * 4 || st.st_size == OLD_CONFIG_WORDS * 4);
}

/* A whole settings file into o (at most CONFIG_WORDS words): its size, or
 * -1 if it isn't there or is bigger than any settings file. */
static long read_settings_file(const char *path, u32 *o)
{
  long size;
  FILE *f;

  recover_settings(path, whole_settings_file);
  f = fopen(path, "rb");

  if (!f)
    return -1;
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 0 || size > CONFIG_WORDS * 4 || fread(o, 1, (size_t)size, f) != (size_t)size)
    size = -1;
  fclose(f);
  return size;
}

/* The settings came from the program file (not from a copy or an older
 * file). */
static int settings_in_program;

int nspire_config_in_program(void)
{
  return settings_in_program;
}

/* The settings' bytes, from the first place that has them (see the top of
 * this file). */
static long read_settings(u32 *o)
{
  static const char *const names[] = { CONFIG_FILE, OLD_CONFIG_FILE };
  char path[600];
  long size = selfstore_read(nspire_program_path, o, CONFIG_WORDS * 4);
  unsigned i;

  settings_in_program = size >= 0;
  if (size >= 0)
    return size;
  /* The copy in the saves folder (an older build's file is moved in at
   * start and takes its name, nspire_tidy_save_folder), then next to gpSP
   * (builds before the saves folder). */
  for (i = 0; i < 2; i++)
  {
    nspire_save_file(path, sizeof(path), names[i]);
    if ((size = read_settings_file(path, o)) >= 0)
      return size;
  }
  for (i = 0; i < 2; i++)
  {
    program_folder_path(path, names[i]);
    if ((size = read_settings_file(path, o)) >= 0)
      return size;
  }
  return -1;
}

static void clamp_rom_buffer_size(void)
{
  if (nspire_rom_buffer_size_choice < 2)
    nspire_rom_buffer_size_choice = 2;
  else if (nspire_rom_buffer_size_choice > 32)
    nspire_rom_buffer_size_choice = 32;
}

s32 nspire_config_load(void)
{
  u32 o[CONFIG_WORDS];
  long size;
  u32 words, i;
  int qol = 0;   /* 0: an older build's file, 1: QOL1, 2: QOL2, 3: this build's */

  memset(o, 0, sizeof(o));
  size = read_settings(o);
  /* Sizes gpSP's builds wrote (92 = keys, 96 = +BIOS, 112 = +frameskip,
   * 124 = +emulator options, 128 = +ROM buffer, 132 = +LCD cache, 136 = +FPS,
   * 140 = +blending, 144 = +dynarec SMC, 148 = +renderer, 152 = older
   * renderer), and this one's. Word 36 was the video renderer (0 video.cc,
   * 1 legacy; removed): it is not read, and written as 0. A game file's
   * "renderer=" line is ignored the same way (no setting has that name). */
  if (!(size == 92 || (size >= 96 && size <= 152 && size % 4 == 0) || size == OLD_CONFIG_WORDS * 4 ||
        size == CONFIG_WORDS * 4))
    return -1;
  words = (u32)size / 4;

  /* Word 3 held sound settings in old ports. A mark distinguishes the
   * USB settings from those older values without growing the file. */
  nspire_audio_enabled = 1; nspire_audio_volume = NSPIRE_VOLUME_DEFAULT;
  nspire_audio_mono = 0; nspire_audio_buffer = 0;
  if ((o[3] & 0xFF000000u) == AUDIO_MARK) {
    nspire_audio_enabled = o[3] & 1;
    if (o[3] & VOLUME20_MARK)
      nspire_audio_volume = ((o[3] >> 16) & 31) % (NSPIRE_VOLUME_MAX + 1);
    else
      nspire_audio_volume = ((o[3] >> 4) & 15) % 11 * 2;
    nspire_audio_mono = (o[3] >> 8) & 1;
    nspire_audio_buffer = ((o[3] >> 12) & 3) % 3;
  }
  nspire_bt_enabled = 1;
  nspire_audio_output = NSPIRE_OUTPUT_AUTO;
  nspire_bt_codec = NSPIRE_BT_CODEC_AUTO;
  if ((o[22] & 0xFF000000u) == BT_MARK) {
    nspire_bt_enabled = o[22] & 1;
    nspire_audio_output = ((o[22] >> 4) & 3) % NSPIRE_OUTPUTS;
    nspire_bt_codec = ((o[22] >> 8) & 3) % NSPIRE_BT_CODECS;
  }
  screen_scale = o[0] % 4;
  screen_filter = o[1] % 2;
  update_backup_flag = o[4] % 2;
  nspire_save_call_ms = (o[2] & 0xFF000000u) == SAVE_CALL_MARK ? (o[2] & 0xFFFF) - (o[2] & 0xFFFF) / 4 : 0;
  global_enable_analog = o[5] % 2;
  analog_sensitivity_level = o[6] % 8;

  /* gpSP's single key per action, for the first 15. */
  keys_set_defaults(key_bindings);
  for (i = 0; i < 15; i++)
  {
    key_binding b = binding_unpack(o[7 + i] & 0xFF);
    key_bindings[i][0] = b;
  }

  nspire_bios_choice = words >= 24 ? o[23] % 2 : 1;
  if (words >= 28)
  {
    if ((o[26] & FS_V2_MARK) == FS_V2_MARK)
      current_frameskip_type = o[24] % 3;
    else
    {
      static const u32 old_types[] = { no_frameskip, auto_frameskip, auto_frameskip, manual_frameskip };
      current_frameskip_type = old_types[o[24] % 4];
    }
    if (current_frameskip_type == no_frameskip && (o[26] & (FS_V2_MARK | PACE_RULE_MARK)) != (FS_V2_MARK | PACE_RULE_MARK))
      current_frameskip_type = auto_frameskip;   /* once: PACE_RULE_MARK */
    frameskip_value = o[25] > 99 ? 99 : o[25];
    if (frameskip_value < 1 && current_frameskip_type == auto_frameskip)
      frameskip_value = 4;
    random_skip = o[27] % 2;
  }
  if (words >= 31)
  {
    nspire_sprlim_choice = o[28] % 2;
    nspire_rtc_choice = o[29] % 3;
    nspire_frame_mix_choice = o[30] % 2;
  }
  nspire_rom_buffer_size_choice = words >= 32 ? o[31] : 8;
  nspire_lcd_cache_clean_full = words >= 33 ? o[32] % 2 : 0;
  nspire_fps_overlay = words >= 34 ? o[33] % 2 : 0;
  nspire_gba_blend_off = words >= 35 ? o[34] % 2 : 0;
  nspire_dynarec_ram_policy = words >= 36 ? o[35] % 2 : 0;
  /* Word 36 was the renderer (1 = legacy), which ignored this setting: the picture
   * that user saw had full blending, so keep that. */
  if (words >= 37 && o[36] == 1)
    nspire_gba_blend_off = 0;

  if (words >= 63 && (o[37] == QOL_MAGIC || o[37] == QOL2_MAGIC || o[37] == QOL1_MAGIC))
  {
    qol = o[37] == QOL1_MAGIC || words < 67 ? 1 : o[37] == QOL2_MAGIC ? 2 : 3;
    for (i = 0; i < FIRST_ACTIONS; i++)
    {
      key_bindings[i][0] = binding_unpack(o[38 + i] & 0xFFFF);
      key_bindings[i][1] = binding_unpack(o[38 + i] >> 16);
    }
    nspire_auto_increment = o[58] % 2;
    nspire_save_on_exit = o[59] % 2;
    nspire_load_on_start = o[60] % 2;
    nspire_cpu_speed = qol >= 2 ? cpu_speed_from_word(o[61]) : cpu_speed_from_qol1(o[61]);
    nspire_fps_overlay = o[62] % 3;
  }
  nspire_compress_saves = qol == 3 ? o[66] % 2 : 1;
  nspire_ff_max = qol == 3 && o[67] >= 11 && o[67] <= 100 ? o[67] - 11 : NSPIRE_FF_UNLIMITED;
  nspire_link_device = qol == 3 ? o[68] % LINK_DEVICES : LINK_DEVICE_AUTO;
  /* Link speed in bit 8 (older builds take the word % LINK_DEVICES). */
  nspire_link_speed = qol == 3 ? (o[68] >> 8) & 1 : LINK_SPEED_AUTO;
  if (qol >= 2)
  {
    nspire_brightness = o[63] % (BRIGHTNESS_LEVELS + 1);
    for (i = FIRST_ACTIONS; i < QOL2_ACTIONS; i++)
    {
      key_bindings[i][0] = binding_unpack(o[64 + i - FIRST_ACTIONS] & 0xFFFF);
      key_bindings[i][1] = binding_unpack(o[64 + i - FIRST_ACTIONS] >> 16);
    }
  }
  else
  {
    /* See the top of this file. */
    screen_scale = scaled_raw;
    keys_drop_clashing_defaults(key_bindings, qol ? FIRST_ACTIONS : 15);
  }
  if (qol < 3)
  {
    keys_add_default_if_free(key_bindings, ACTION_NEXT_SLOT);
    keys_add_default_if_free(key_bindings, ACTION_PREVIOUS_SLOT);
  }
  nspire_turbo_speed = qol == 3 && (o[70] & TURBO_MARK) ? ((o[70] >> 8) & 0xFF) % TURBO_SPEEDS : TURBO_SPEED_DEFAULT;
  old_turbo_choice = qol == 3 && (o[70] & TURBO_MARK) ? (o[70] & 0xF) : 0;
  if (qol == 3 && words >= CONFIG_WORDS && (o[70] & TURBO_BINDINGS_MARK))
    for (i = 0; i < GBA_BUTTONS; i++)
    {
      key_bindings[FIRST_TURBO + i][0] = binding_unpack(o[TURBO_WORD + i] & 0xFFFF);
      key_bindings[FIRST_TURBO + i][1] = binding_unpack(o[TURBO_WORD + i] >> 16);
    }
  else
  {
    /* The old turbo (see the top of this file): its choice and its Turbo
     * key (a file from before turbo: = where it's free) become bindings. */
    key_binding old_key[KEY_SLOTS];
    int made;
    memset(old_key, 0, sizeof(old_key));
    if (qol == 3 && (o[70] & TURBO_MARK))
    {
      old_key[0] = binding_unpack(o[69] & 0xFFFF);
      old_key[1] = binding_unpack(o[69] >> 16);
    }
    else if (!keys_key_used(key_bindings, KEY_EQUALS, FIRST_TURBO))
      old_key[0].key = KEY_EQUALS;
    made = keys_turbo_from_old(key_bindings, keys_old_turbo_mask(old_turbo_choice), old_key);
    log_turbo_migration("settings", old_turbo_choice, old_key, made);
  }
  nspire_sram_code = (qol == 3 && words >= CONFIG_WORDS &&
                      (o[70] & TURBO_BINDINGS_MARK) &&
                      (o[69] & 0xFF000000u) == SRAM_MARK && (o[69] & SRAM_ON_MARK)) ? (o[69] & 1) : 1;

  if (qol == 3 && (o[71] & AUTOSAVE_MARK))
  {
    nspire_autosave = (o[71] & 0xF) % AUTOSAVE_CHOICES;
    nspire_autosave_new_slot = (o[71] >> 4) & 1;
    nspire_saves_on = (o[71] & SAVES_DRIVE_BIT) != 0;
    nspire_ui_theme = o[71] & THEME_MARK ? ((o[71] >> 8) & 7) % UI_THEMES : 0;
    nspire_confirm_delete = (o[71] & CONFIRM_DELETE_OFF) == 0;
  }
  else
  {
    nspire_autosave = 0;
    nspire_autosave_new_slot = 1;
    nspire_saves_on = 0;
    nspire_ui_theme = 0;
    nspire_confirm_delete = 1;
  }
  ui_theme_set(nspire_ui_theme);

  clamp_rom_buffer_size();
  nspire_emulator_options_apply();
  return 0;
}

static int write_global_file(void)
{
  char path[600], tmp[620];
  u32 o[CONFIG_WORDS];
  u32 i;
  FILE *f;
  int ok;

  memset(o, 0, sizeof(o));
  o[0] = screen_scale;
  o[1] = screen_filter;
  o[3] = AUDIO_MARK | (nspire_audio_enabled & 1) | VOLUME20_MARK |
         ((nspire_audio_volume % (NSPIRE_VOLUME_MAX + 1)) << 16) |
         ((((nspire_audio_volume % (NSPIRE_VOLUME_MAX + 1)) + 1) / 2) << 4) |
         ((nspire_audio_mono & 1) << 8) | ((nspire_audio_buffer % 3) << 12);
  if (!nspire_bt_enabled || nspire_audio_output % NSPIRE_OUTPUTS || nspire_bt_codec % NSPIRE_BT_CODECS)
    o[22] = BT_MARK | (nspire_bt_enabled & 1) | (nspire_audio_output % NSPIRE_OUTPUTS) << 4 |
            (nspire_bt_codec % NSPIRE_BT_CODECS) << 8;
  o[4] = update_backup_flag;
  if (nspire_save_call_ms)
    o[2] = SAVE_CALL_MARK | (nspire_save_call_ms > 0xFFFF ? 0xFFFF : nspire_save_call_ms);
  o[5] = global_enable_analog;
  o[6] = analog_sensitivity_level;
  for (i = 0; i < 15; i++)
    o[7 + i] = key_bindings[i][0].with ? 0 : key_bindings[i][0].key;
  clamp_rom_buffer_size();
  o[23] = nspire_bios_choice % 2;
  o[24] = current_frameskip_type % 3;
  o[25] = frameskip_value;
  o[26] = FS_V2_MARK | PACE_RULE_MARK;
  o[27] = random_skip % 2;
  o[28] = nspire_sprlim_choice % 2;
  o[29] = nspire_rtc_choice % 3;
  o[30] = nspire_frame_mix_choice % 2;
  o[31] = nspire_rom_buffer_size_choice;
  o[32] = nspire_lcd_cache_clean_full % 2;
  o[33] = nspire_fps_overlay ? 1 : 0;
  o[34] = nspire_gba_blend_off % 2;
  o[35] = nspire_dynarec_ram_policy % 2;
  o[36] = 0;   /* reserved: the removed video renderer choice (old files have 0 or 1) */
  o[37] = QOL_MAGIC;
  for (i = 0; i < NUM_ACTIONS; i++)
    o[i < FIRST_ACTIONS ? 38 + i : i < QOL2_ACTIONS ? 64 + i - FIRST_ACTIONS : TURBO_WORD + i - FIRST_TURBO] =
        binding_pack(key_bindings[i][0]) | (binding_pack(key_bindings[i][1]) << 16);
  o[58] = nspire_auto_increment;
  o[59] = nspire_save_on_exit;
  o[60] = nspire_load_on_start;
  o[61] = cpu_speed_word(nspire_cpu_speed);
  o[62] = nspire_fps_overlay;
  o[63] = nspire_brightness;
  o[66] = nspire_compress_saves % 2;
  o[67] = nspire_ff_max < NSPIRE_FF_UNLIMITED ? nspire_ff_max + 11 : 0;
  o[68] = nspire_link_device % LINK_DEVICES | (nspire_link_speed % LINK_SPEEDS) << 8;
  o[69] = SRAM_MARK | SRAM_ON_MARK | (nspire_sram_code & 1);
  o[70] = TURBO_MARK | TURBO_BINDINGS_MARK | (old_turbo_choice % OLD_TURBO_CHOICES) |
          ((nspire_turbo_speed % TURBO_SPEEDS) << 8);
  o[71] = AUTOSAVE_MARK | (nspire_autosave % AUTOSAVE_CHOICES) | ((nspire_autosave_new_slot & 1) << 4) |
          (nspire_saves_on ? SAVES_DRIVE_BIT : 0) | (!nspire_confirm_delete ? CONFIRM_DELETE_OFF : 0);
  if (nspire_ui_theme % UI_THEMES)   /* the first theme writes the word as builds without themes do */
    o[71] |= THEME_MARK | ((nspire_ui_theme % UI_THEMES) << 8);

  /* The copy in the saves folder, then the program file. */
  nspire_save_dir_make();
  nspire_save_file(path, sizeof(path), CONFIG_FILE);
  /* EXIT-3: the same words as the file has: no write, remove and rename
   * (three of the CX's slow file operations at every quit). */
  {
    u32 old[CONFIG_WORDS];
    ok = 0;
    f = fopen(path, "rb");
    if (f)
    {
      ok = fread(old, 4, CONFIG_WORDS, f) == CONFIG_WORDS && fgetc(f) == EOF && !memcmp(old, o, sizeof(o));
      fclose(f);
    }
  }
  if (!ok)
  {
    settings_tmp(tmp, sizeof(tmp), path);
    f = fopen(tmp, "wb");
    if (f)
    {
      ok = fwrite(o, 4, CONFIG_WORDS, f) == CONFIG_WORDS;
      if (fclose(f) != 0)
        ok = 0;
    }
    ok = settings_replace(tmp, path, ok);
  }
  /* Then the program file's copy (summed: nspire_selfstore.c). */
  if (selfstore_write(nspire_program_path, o, sizeof(o)))
    ok = 1;
  return ok ? 0 : -1;
}

/* ---- A game's own settings and keys ---- */

static void game_file_path(const char *rom_path, char *path, size_t size)
{
  char base[512];
  nspire_game_base(rom_path, base, sizeof(base));
  snprintf(path, size, "%s.settings.tns", base);
}

static void write_game_file(void)
{
  char tmp[620];
  FILE *f;
  int i, slot, ok;

  if (!game_open)
    return;
  if (!own_settings && !own_keys)
  {
    remove(game_file);
    return;
  }
  settings_tmp(tmp, sizeof(tmp), game_file);
  f = fopen(tmp, "w");
  if (!f)
    return;
  fprintf(f, "version=%d\n", GAME_FILE_VERSION);
  if (own_settings)
  {
    fprintf(f, "own_settings=1\n");
    for (i = 0; i < NUM_SETTINGS; i++)
      fprintf(f, "%s=%u\n", settings[i].name, (unsigned)(settings[i].value == &nspire_cpu_speed ?
              cpu_speed_word(nspire_cpu_speed) : *settings[i].value));
  }
  if (own_keys)
  {
    fprintf(f, "own_keys=1\n");
    for (i = 0; i < NUM_ACTIONS; i++)
      for (slot = 0; slot < KEY_SLOTS; slot++)
        fprintf(f, "key_%s%s=%u\n", action_ids[i], slot ? "_2" : "",
                binding_pack(key_bindings[i][slot]));
  }
  /* R6: the last line: a .tmp that has it is whole (recover_settings). */
  ok = fprintf(f, "%s\n", GAME_FILE_END) > 0;
  ok = fclose(f) == 0 && ok;
  settings_replace(tmp, game_file, ok);
}

static int whole_game_file(const char *path)
{
  char line[200], last[200] = "";
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  while (fgets(line, sizeof(line), f))
    memcpy(last, line, sizeof(last));
  fclose(f);
  return !strncmp(last, GAME_FILE_END, strlen(GAME_FILE_END));
}

void nspire_config_open_game(const char *rom_path)
{
  char line[200];
  u32 values[NUM_SETTINGS];
  unsigned char in_file[NUM_SETTINGS];
  key_binding keys[NUM_ACTIONS][KEY_SLOTS];
  /* The old turbo (version 3 and before): its "turbo" line, its Turbo key. */
  key_binding old_key[KEY_SLOTS];
  u32 old_choice = 0;
  int has_old_choice = 0, has_old_key = 0;
  int has_settings = 0, has_keys = 0, version = 1, i;
  FILE *f;

  nspire_config_close_game();
  capture(global_values);
  memcpy(global_keys, key_bindings, sizeof(global_keys));
  memcpy(values, global_values, sizeof(values));
  memcpy(keys, key_bindings, sizeof(keys));
  memset(in_file, 0, sizeof(in_file));
  memset(old_key, 0, sizeof(old_key));
  game_open = 1;
  own_settings = own_keys = 0;

  game_file_path(rom_path, game_file, sizeof(game_file));
  recover_settings(game_file, whole_game_file);
  f = fopen(game_file, "r");
  if (f)
  {
    while (fgets(line, sizeof(line), f))
    {
      char *value = strchr(line, '=');
      unsigned long number;
      if (!value)
        continue;
      *value++ = 0;
      number = strtoul(value, NULL, 10);
      if (!strcmp(line, "version"))
        version = (int)number;
      else if (!strcmp(line, "own_settings"))
        has_settings = number != 0;
      else if (!strcmp(line, "own_keys"))
        has_keys = number != 0;
      else if (!strcmp(line, "turbo"))
      {
        old_choice = (u32)(number % OLD_TURBO_CHOICES);
        has_old_choice = 1;
      }
      else if (!strcmp(line, "key_turbo") || !strcmp(line, "key_turbo_2"))
      {
        old_key[line[9] ? 1 : 0] = binding_unpack((unsigned)number);
        has_old_key = 1;
      }
      else if (!strncmp(line, "key_", 4))
      {
        size_t len = strlen(line + 4);
        int second = len > 2 && !strcmp(line + 4 + len - 2, "_2");
        for (i = 0; i < NUM_ACTIONS; i++)
          if (strlen(action_ids[i]) == len - (second ? 2 : 0) &&
              !strncmp(line + 4, action_ids[i], strlen(action_ids[i])))
            keys[i][second] = binding_unpack((unsigned)number);
      }
      else if (!strcmp(line, "audio_volume"))
      {
        /* (a game's file from before r19: eleven steps; a "volume20" line after it wins) */
        for (i = 0; i < NUM_SETTINGS; i++)
          if (settings[i].value == &nspire_audio_volume && !in_file[i])
            values[i] = (u32)number % 11 * 2;
      }
      else
        for (i = 0; i < NUM_SETTINGS; i++)
          if (!strcmp(line, settings[i].name))
          {
            values[i] = settings[i].value == &nspire_cpu_speed ? cpu_speed_from_word((u32)number) :
                        (u32)number % settings[i].count;
            in_file[i] = 1;
          }
    }
    fclose(f);
  }

  if (has_settings && version < 5)
  {
    /* The pace rule's once (PACE_RULE_MARK): a game's own Off from before
     * it is Automatic; the file is written as version 5 when the game
     * closes, so an Off chosen after stays. */
    int j;
    for (i = 0; i < NUM_SETTINGS; i++)
      if (settings[i].value == &current_frameskip_type && values[i] == no_frameskip)
      {
        values[i] = auto_frameskip;
        /* a value of 0 is "never skip" for automatic: 4, as the file for all games gets */
        for (j = 0; j < NUM_SETTINGS; j++)
          if (settings[j].value == &frameskip_value && values[j] < 1)
            values[j] = 4;
      }
  }
  if (has_settings && version < 3)
  {
    /* Written by the first QoL build: see the top of this file. */
    for (i = 0; i < NUM_SETTINGS; i++)
      if (settings[i].value == &screen_scale)
        values[i] = scaled_raw;
      else if (settings[i].value == &nspire_cpu_speed && in_file[i] && values[i] != CPU_SPEED_AUTO)
        values[i] = cpu_speed_from_qol1(values[i]);
  }
  if (has_settings)
  {
    own_settings = 1;
    apply(values);
  }
  if (has_keys && version < 3)
  {
    keys_add_default_if_free(keys, ACTION_NEXT_SLOT);
    keys_add_default_if_free(keys, ACTION_PREVIOUS_SLOT);
  }
  if (version < 4)
  {
    /* The old turbo (2026-11-12). The game's own keys: its turbo
     * choice (else the one all games had) and its Turbo key (a file from
     * before turbo: = where free) become its turbo bindings. Keys for all
     * games, a turbo choice of its own: its own keys, the buttons its
     * choice and that of all games differ on moved over. */
    u32 choice = has_settings && has_old_choice ? old_choice : old_turbo_choice;
    if (has_keys)
    {
      int made;
      if (!has_old_key && !keys_key_used(keys, KEY_EQUALS, FIRST_TURBO))
        old_key[0].key = KEY_EQUALS;
      made = keys_turbo_from_old(keys, keys_old_turbo_mask(choice), old_key);
      log_turbo_migration("a game's own keys", choice, old_key, made);
    }
    else if (has_settings && has_old_choice &&
             keys_old_turbo_mask(choice) != keys_old_turbo_mask(old_turbo_choice))
    {
      static const unsigned char old_buttons[4] = { ACTION_A, ACTION_B, ACTION_L, ACTION_R };
      unsigned game = keys_old_turbo_mask(choice), all = keys_old_turbo_mask(old_turbo_choice);
      for (i = 0; i < 4; i++)
        if ((game ^ all) & (1u << i))
          keys_move_turbo(keys, old_buttons[i], (game >> i) & 1);
      has_keys = 1;
      nspire_link_log("keys: a game's own turbo choice %u (all games: %u): its own keys now",
                      (unsigned)choice, (unsigned)old_turbo_choice);
    }
  }
  if (has_keys)
  {
    own_keys = 1;
    memcpy(key_bindings, keys, sizeof(keys));
  }
  /* A raised CPU speed froze this game last time (nspire_clock.c). */
  if (nspire_clock_crashed_rom[0] && !strcmp(nspire_clock_crashed_rom, rom_path))
  {
    nspire_cpu_speed = 0;
    nspire_clock_crashed_rom[0] = 0;
    write_game_file();
  }
  nspire_settings_for = (u32)own_settings;
  nspire_keys_for = (u32)own_keys;
  clamp_rom_buffer_size();
  nspire_emulator_options_apply();
  /* r19: the game's cheat file (nspire_cheats.c; weak: a build without the menus has none) */
  if (nspire_cheats_open)
    nspire_cheats_open(rom_path);
}

static void close_game(int write)
{
  if (!game_open)
    return;
  if (nspire_cheats_close)
    nspire_cheats_close();
  if (write)
    write_game_file();
  if (own_settings)
    apply(global_values);
  else
    capture(global_values);
  if (own_keys)
    memcpy(key_bindings, global_keys, sizeof(global_keys));
  else
    memcpy(global_keys, key_bindings, sizeof(global_keys));
  game_open = own_settings = own_keys = 0;
  nspire_settings_for = nspire_keys_for = 0;
  nspire_emulator_options_apply();
}

void nspire_config_close_game(void)
{
  close_game(1);
}

void nspire_config_drop_game(void)
{
  close_game(0);
}

/* The menu changed "Settings for": this game's own starts from the values in
 * use, and going back to all games drops them. */
void nspire_config_settings_for_changed(void)
{
  if (!game_open)
  {
    nspire_settings_for = 0;
    return;
  }
  if (nspire_settings_for && !own_settings)
  {
    capture(global_values);
    own_settings = 1;
  }
  else if (!nspire_settings_for && own_settings)
  {
    own_settings = 0;
    apply(global_values);
    nspire_emulator_options_apply();
  }
}

void nspire_config_keys_for_changed(void)
{
  if (!game_open)
  {
    nspire_keys_for = 0;
    return;
  }
  if (nspire_keys_for && !own_keys)
  {
    memcpy(global_keys, key_bindings, sizeof(global_keys));
    own_keys = 1;
  }
  else if (!nspire_keys_for && own_keys)
  {
    own_keys = 0;
    memcpy(key_bindings, global_keys, sizeof(global_keys));
  }
}

/* Writes the settings for all games, and the open game's own. */
s32 nspire_config_save(void)
{
  u32 live[NUM_SETTINGS];
  key_binding live_keys[NUM_ACTIONS][KEY_SLOTS];
  s32 result;

  /* What isn't the game's own belongs to all games. */
  if (!(game_open && own_settings))
    capture(global_values);
  if (!(game_open && own_keys))
    memcpy(global_keys, key_bindings, sizeof(global_keys));

  capture(live);
  memcpy(live_keys, key_bindings, sizeof(live_keys));
  apply(global_values);
  memcpy(key_bindings, global_keys, sizeof(global_keys));
  result = write_global_file();
  apply(live);
  memcpy(key_bindings, live_keys, sizeof(live_keys));

  write_game_file();
  return result;
}
