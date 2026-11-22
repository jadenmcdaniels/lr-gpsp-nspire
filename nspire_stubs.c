/* Globals and small stubs normally provided by libretro/libretro.c */

#include "common.h"
#include "main.h"
#include "cpu.h"
#include "gba_memory.h"
#include "nspire_frameskip.h"
#include "nspire_gui_video.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>

int dynarec_enable = 1;
/* Default 1 = official file (legacy standalone behavior before menu option). */
u32 nspire_bios_choice = 1;
/* Libretro gpsp_sprlim default "disabled": 0=hardware cap, 1=no cap. */
u32 nspire_sprlim_choice = 0;
/* Libretro gpsp_rtc: 0=auto, 1=enabled, 2=disabled. */
u32 nspire_rtc_choice = 0;
/* Libretro gpsp_frame_mixing: 0=off, 1=on. */
u32 nspire_frame_mix_choice = 0;
/* 0 = range D-clean of LCD buffer; 1 = full clean_dcache before display swap. */
u32 nspire_lcd_cache_clean_full = 0;
/* 1 = draw FPS overlay on presented game frames. */
u32 nspire_fps_overlay = 0;
/* 1 = skip PPU BLDCNT blending / brightness passes (performance; inaccurate). */
u32 nspire_gba_blend_off = 0;
/* 0 = partial SMC (default); 1 = classic — flush_dynarec_caches on SMC + full
 * PC-relative block linking from RAM code (same emits as ROM blocks). */
u32 nspire_dynarec_ram_policy = 0;
/* Runtime ROM page-cache target in MiB (2..32). */
u32 nspire_rom_buffer_size_choice = 8;
boot_mode selected_boot_mode = boot_game;
int sprite_limit = 1;
u32 num_skipped_frames = 0;
u32 skip_next_frame = 0;

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;

u32 netplay_num_clients = 0;
u32 netplay_client_id = 0;

void game_name_ext(char *src, char *buffer, char *extension)
{
  (void)extension;
  if (buffer && src)
    strcpy(buffer, src);
}

u32 quick_save_slot = 0;

gpsp_gui_cheat_entry cheats[16];
u32 num_cheats = 0;

/* r19: the choices that only decided speed are made for the player, and their tiles are gone (the fast
 * choice is the default and has no tile, so the settings stay few; an instability it brings is fixed in
 * the code, not given a tile to turn the fast choice off). Whatever a settings file
 * says (files from older builds' menus can have frame mixing on and the smoothed picture): automatic
 * frame skip, no frame mixing, the fast handling of code in RAM and fast forward without a top speed. Two are
 * fixed the other way, for the picture: Fit is drawn smoothed, and Transparency stays ON: without it fog, water and fades are simply missing, which is a wrong
 * picture, not a slower one. Not here: CPU speed, Fast memory and the link's speed, which turn themselves down
 * after a freeze and must stay down (nspire_clock.c, nspire_sram.c, nspire_link.c).
 * The PC build keeps what a file says unless GPSP_HOST_FAST is set: its tests of the frame skip's other modes
 * and of fast forward's top speeds go through settings files. */
int nspire_fast_settings = 1;

void nspire_config_fast(void)
{
#if defined(NSPIRE_HOST)
  {
    static int asked;
    if (!asked)
    {
      asked = 1;
      nspire_fast_settings = getenv("GPSP_HOST_FAST") != NULL;
    }
  }
#endif
  if (!nspire_fast_settings)
    return;
  /* (frame skip is the player's again: after a time fixed to Auto, its setting came back. More has its
   * tile, Auto or Off.) */
  nspire_frame_mix_choice = 0;
  nspire_dynarec_ram_policy = 0;
  nspire_gba_blend_off = 0;
  nspire_ff_max = NSPIRE_FF_UNLIMITED;
  /* (Fit is the smoothed one, always: on the calculator the sharp one looked like a picture that should
   * have been smoothed) */
  if (screen_scale == scaled_raw)
    screen_scale = scaled_aspect;
}

void nspire_emulator_options_apply(void)
{
  static u32 last_mix = (u32)-1;
  static u32 last_ram_policy;
  static u32 ram_policy_seen;
  static u32 last_block_reuse;
  static u32 block_reuse_seen;

  nspire_config_fast();
  if (nspire_rom_buffer_size_choice < 2)
    nspire_rom_buffer_size_choice = 2;
  else if (nspire_rom_buffer_size_choice > 32)
    nspire_rom_buffer_size_choice = 32;

  sprite_limit = (nspire_sprlim_choice == 0) ? 1 : 0;
  if (last_mix != nspire_frame_mix_choice)
  {
    last_mix = nspire_frame_mix_choice;
    nspire_video_mix_reset();
  }

  if (!ram_policy_seen)
  {
    ram_policy_seen = 1;
    last_ram_policy = nspire_dynarec_ram_policy;
  }
  else if (last_ram_policy != nspire_dynarec_ram_policy)
  {
    last_ram_policy = nspire_dynarec_ram_policy;
#ifdef HAVE_DYNAREC
    /* Flushing before init_emitter / init_bios_hooks can crash the device. */
    if (bios_swi_entrypoint)
      flush_dynarec_caches();
#endif
  }
}

void nspire_dynarec_ram_policy_menu_hook(void)
{
  nspire_emulator_options_apply();
}

int nspire_rtc_force_value(void)
{
  static const int map[] = {
    FEAT_AUTODETECT,
    FEAT_ENABLE,
    FEAT_DISABLE
  };

  return map[nspire_rtc_choice % 3];
}

