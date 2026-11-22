/* Ndless keypad input for libretro gpSP core (replaces libretro/input.c):
 * the keypad, key bindings (nspire_keys.h), the GBA buttons and hotkeys of a
 * frame, and menu input with key repeat. */

#include "common.h"
#include "input.h"
#include "serial.h"
#include "gba_memory.h"
#include "gui.h"
#include "nspire.h"
#include "nspire_gui_video.h"
#include "nspire_keys.h"
#include "nspire_hw.h"
#include "nspire_brightness.h"
#include "nspire_usbpad_glue.h"
#include "nspire_bt_glue.h"
#include "nspire_drive.h"
#include "nspire_audio.h"
#include "nspire_os.h"
#include "nspire_frameskip.h"
#include "nspire_frontend.h"

#include <os.h>

bool libretro_supports_bitmasks    = false;
bool libretro_supports_ff_override = false;
bool libretro_ff_enabled           = false;
bool libretro_ff_enabled_prev      = false;

unsigned turbo_period      = TURBO_PERIOD_MIN;
unsigned turbo_pulse_width = TURBO_PULSE_WIDTH_MIN;
unsigned turbo_a_counter   = 0;
unsigned turbo_b_counter   = 0;

u32 global_enable_analog = 0;
u32 analog_sensitivity_level = 4;

static u32 old_key = 0;

void retro_set_input_state(retro_input_state_t cb) { (void)cb; }

void set_fastforward_override(bool fastforward) { (void)fastforward; }

static void trigger_key(u32 key)
{
  u32 p1_cnt = read_ioreg(REG_P1CNT);

  if ((p1_cnt >> 14) & 0x01)
  {
    u32 key_intersection = (p1_cnt & key) & 0x3FF;

    if (p1_cnt >> 15)
    {
      if (key_intersection == (p1_cnt & 0x3FF))
      {
        flag_interrupt(IRQ_KEYPAD);
        check_and_raise_interrupts();
      }
    }
    else
    {
      if (key_intersection)
      {
        flag_interrupt(IRQ_KEYPAD);
        check_and_raise_interrupts();
      }
    }
  }
}

/* ---- The keypad ---- */

#define TOUCHPAD_KEYS_MASK 0xFF00FFFD

/* The keypad's four words, then the USB gamepad's (nspire_keys.h). */
static u32 current_keys[KEY_WORDS];
static u32 new_keys[KEY_WORDS];
/* Keys that count as up until released (keys_mask_held). */
static u32 masked_keys[KEY_WORDS];

/* A pad's D-pad (and its left stick) presses the arrows: its up, down,
 * left and right bits (usbpad.h's 0-3) as the touchpad's arrows below. */
static const u32 pad_arrows[4] = { 0x00010000, 0x00100000, 0x00400000, 0x00040000 };

/* OUT-04: touchpad_scan is an I2C read of 11 bytes through the OS (Ndless
 * libndls/touchpad.c), every game frame: 180 a second in fast forward. A
 * game frame's poll in fast forward (with the timer) reuses the last scan's
 * arrows for 1/60 s, so the touchpad is read 60 times a real second, as in
 * normal play; the keypad's words and the USB pad still every frame. Normal
 * play, menus and waits: a scan every poll, as before. */
static int frame_poll;            /* keys_poll called by nspire_input_frame */
static int touchpad_kept;         /* touchpad_last/touchpad_at hold a fast forward scan */
static u32 touchpad_last, touchpad_at;

static u32 touchpad_arrows(void)
{
  touchpad_report_t report;
  u32 touchpad_pressed;

  touchpad_scan(&report);
  switch ((tpad_arrow_t)report.arrow)
  {
  case TPAD_ARROW_UP:
    touchpad_pressed = 0x00010000;
    break;
  case TPAD_ARROW_UPRIGHT:
    touchpad_pressed = 0x00050000;
    break;
  case TPAD_ARROW_RIGHT:
    touchpad_pressed = 0x00040000;
    break;
  case TPAD_ARROW_RIGHTDOWN:
    touchpad_pressed = 0x00140000;
    break;
  case TPAD_ARROW_DOWN:
    touchpad_pressed = 0x00100000;
    break;
  case TPAD_ARROW_DOWNLEFT:
    touchpad_pressed = 0x00500000;
    break;
  case TPAD_ARROW_LEFT:
    touchpad_pressed = 0x00400000;
    break;
  case TPAD_ARROW_LEFTUP:
    touchpad_pressed = 0x00410000;
    break;
  case TPAD_ARROW_CLICK:
    touchpad_pressed = 0x00000002;
    break;
  default:
    touchpad_pressed = 0x00000000;
    break;
  }
  return touchpad_pressed;
}

/* Header line of gpsp_perf.txt.tns: what a scan costs on this calculator. */
static char keys_line[96];

u32 nspire_keys_probe(void)
{
  u32 start = nspire_ticks(), t;
  int i;

  for (i = 0; i < 16; i++)
    (void)touchpad_arrows();
  t = nspire_ticks() - start;
  if (!nspire_timer_runs() || !t)
    snprintf(keys_line, sizeof(keys_line), "keys: touchpad scan not timed (16 scans, %u ticks)", (unsigned)t);
  else
    snprintf(keys_line, sizeof(keys_line), "keys: touchpad scan %u us (16 scans, %u ticks)",
             (unsigned)((u64)t * 1000000u / NSPIRE_TICK_HZ / 16), (unsigned)t);
  return nspire_ticks() - start;
}

const char *nspire_keys_line(void)
{
  return keys_line[0] ? keys_line : "keys: touchpad scan not measured";
}

void keys_poll(void)
{
  u32 touchpad_pressed, pad;
  int i;

  nspire_audio_poll();

  /* R6: if the OS turned its keypad interrupt on again, off (one read;
   * nothing before gpSP took it, or after the hand-back). */
  nspire_os_keypad_keep_off();
  if (frame_poll && (nspire_pace == PACE_FAST_FORWARD || !synchronize_flag) && nspire_timer_runs())
  {
    u32 now = nspire_ticks();
    if (touchpad_kept && now - touchpad_at < NSPIRE_TICK_HZ / 60)
      touchpad_pressed = touchpad_last;
    else
    {
      touchpad_pressed = touchpad_last = touchpad_arrows();
      touchpad_at = now;
      touchpad_kept = 1;
    }
  }
  else
  {
    touchpad_pressed = touchpad_arrows();
    touchpad_kept = 0;
  }
  pad = nspire_usbpad_keys();
  /* Bluetooth's pads ORed in, its run loop; nothing (a compare) without a dongle */
  if (!NSPIRE_BT_IDLE())
    pad |= nspire_bt_glue_poll(frame_poll);
  nspire_drive_poll();
  for (i = 0; i < 4; i++)
    if (pad & (1u << i))
      touchpad_pressed |= pad_arrows[i];

  new_keys[0] = ~current_keys[0];
  new_keys[1] = ~current_keys[1];
  new_keys[2] = ~current_keys[2];
  new_keys[3] = ~current_keys[3];
  new_keys[4] = ~current_keys[4];

  new_keys[0] &= (current_keys[0] = HW_READ(HW_KEYPAD) | (on_key_pressed() ? 0x200 : 0));
  new_keys[1] &= (current_keys[1] = HW_READ(HW_KEYPAD + 4));
  new_keys[2] &= (current_keys[2] = HW_READ(HW_KEYPAD + 8));
  new_keys[3] &= (current_keys[3] =
                      (HW_READ(HW_KEYPAD + 12) & TOUCHPAD_KEYS_MASK) | touchpad_pressed);
  new_keys[4] &= (current_keys[4] = pad & ~0xFu);
  if (masked_keys[0] | masked_keys[1] | masked_keys[2] | masked_keys[3] | masked_keys[4])
  {
    for (i = 0; i < KEY_WORDS; i++)
    {
      masked_keys[i] &= current_keys[i];   /* released: counts again */
      current_keys[i] &= ~masked_keys[i];
      new_keys[i] &= ~masked_keys[i];
    }
  }
}

void keys_mask_held(void)
{
  int i;
  keys_poll();
  for (i = 0; i < KEY_WORDS; i++)
  {
    masked_keys[i] |= current_keys[i];
    current_keys[i] = new_keys[i] = 0;
  }
}

int key_down(unsigned key)
{
  return key && key <= NUM_KEYS && (current_keys[(key - 1) >> 5] >> ((key - 1) & 0x1F)) & 1;
}

int key_pressed(unsigned key)
{
  return key && key <= NUM_KEYS && (new_keys[(key - 1) >> 5] >> ((key - 1) & 0x1F)) & 1;
}

/* LNK-1: for the link's waits, which run inside a frame: a keys_poll there
 * would take the edges (key_pressed) the next frame's hotkeys look for. */
int key_down_now(unsigned key)
{
  if (!key || key > 4 * 32)
    return 0;
  key--;
  return (HW_READ(HW_KEYPAD + 4 * (key >> 5)) >> (key & 0x1F)) & 1;
}

int keys_down_now(void)
{
  return (HW_READ(HW_KEYPAD) | HW_READ(HW_KEYPAD + 4) | HW_READ(HW_KEYPAD + 8) |
          (HW_READ(HW_KEYPAD + 12) & TOUCHPAD_KEYS_MASK)) != 0;
}

int any_key_down(void)
{
  return (current_keys[0] | current_keys[1] | current_keys[2] | current_keys[3] | current_keys[4]) != 0;
}

/* R6: the keys as the hardware has them, none of gpSP's masks: the keypad's
 * four words (all 0 with no key down, as Ndless's any_key_pressed reads
 * them), the touchpad pressed, the on key. nspire_exit.c waits on it. */
int keys_raw_down(void)
{
  touchpad_report_t report;

  if (HW_READ(HW_KEYPAD) | HW_READ(HW_KEYPAD + 4) | HW_READ(HW_KEYPAD + 8) | HW_READ(HW_KEYPAD + 12))
    return 1;
  if (on_key_pressed())
    return 1;
  memset(&report, 0, sizeof(report));
  touchpad_scan(&report);
  return report.pressed != 0;
}

void keys_wait_release(void)
{
  do
  {
    keys_poll();
    nspire_wait_until(nspire_ticks() + NSPIRE_TICK_HZ / 100);
  } while (any_key_down());
}

int binding_held(key_binding b)
{
  return b.key && key_down(b.key) && (!b.with || key_down(b.with));
}

static int binding_pressed(key_binding b)
{
  return binding_held(b) && (key_pressed(b.key) || (b.with && key_pressed(b.with)));
}

static int key_is_bound(unsigned key)
{
  int i, slot;
  for (i = 0; i < NUM_ACTIONS; i++)
    for (slot = 0; slot < KEY_SLOTS; slot++)
      if (key_bindings[i][slot].key == key || key_bindings[i][slot].with == key)
        return 1;
  return 0;
}

/* A USB gamepad's keys that are bound to nothing do a console's jobs, one
 * button each, no doubles: the right face button (Pad A) is A, the bottom
 * one (Pad B) B, L1 is L, R1 is R, Start and Select theirs (the D-pad and
 * the left stick are the arrows); every other pad key does nothing until
 * bound; in the menus A and B are Enter and Esc then, as the keys bound
 * to A and B are. Home, or Select and Start held together for 0.5 s (for
 * pads without Home), opens the menu and leaves it. */
static const unsigned char pad_jobs[][2] =
{
  { KEY_PAD_A, ACTION_A }, { KEY_PAD_B, ACTION_B }, { KEY_PAD_L1, ACTION_L }, { KEY_PAD_R1, ACTION_R },
  { KEY_PAD_START, ACTION_START }, { KEY_PAD_SELECT, ACTION_SELECT }
};

/* An action that has a pad key of its own (bound on the Controls screen):
 * its default pad key does nothing then, so a GBA button never has two
 * controller buttons (UI part 2). */
static int has_pad_binding(int action)
{
  int slot;
  for (slot = 0; slot < KEY_SLOTS; slot++)
    if (key_bindings[action][slot].key >= KEY_PAD_FIRST || key_bindings[action][slot].with >= KEY_PAD_FIRST)
      return 1;
  return 0;
}

/* A pad key bound to nothing whose job is action's, down or just pressed
 * (test: key_down or key_pressed). */
static int pad_job(int action, int (*test)(unsigned))
{
  unsigned i;

  if (!current_keys[KEY_WORDS - 1] || has_pad_binding(action))
    return 0;
  for (i = 0; i < sizeof(pad_jobs) / sizeof(pad_jobs[0]); i++)
    if (pad_jobs[i][1] == action && test(pad_jobs[i][0]) && !key_is_bound(pad_jobs[i][0]))
      return 1;
  return 0;
}

/* The keys of the GBA buttons' combinations (plain or turbo) held this
 * frame: a single key bound to a button doesn't count while it's part of
 * one ("= held, del pressed" is turbo A, not A as well). Filled by
 * find_held_combos, once a frame. */
static unsigned char combo_keys[2 * 2 * GBA_BUTTONS * KEY_SLOTS];
static int num_combo_keys;

static void find_held_combos(void)
{
  int i, slot;
  num_combo_keys = 0;
  for (i = 0; i < NUM_ACTIONS; i++)
    if (IS_BUTTON(i))
      for (slot = 0; slot < KEY_SLOTS; slot++)
      {
        key_binding b = key_bindings[i][slot];
        if (b.with && binding_held(b))
        {
          combo_keys[num_combo_keys++] = b.key;
          combo_keys[num_combo_keys++] = b.with;
        }
      }
}

static int key_in_button_combo(unsigned key)
{
  int i;
  for (i = 0; i < num_combo_keys; i++)
    if (combo_keys[i] == key)
      return 1;
  return 0;
}

/* A GBA button's action (plain or turbo) held this frame (after
 * find_held_combos). */
static int action_held(int action)
{
  int slot;
  for (slot = 0; slot < KEY_SLOTS; slot++)
  {
    key_binding b = key_bindings[action][slot];
    if (binding_held(b) && (b.with || !key_in_button_combo(b.key)))
      return 1;
  }
  return pad_job(action, key_down);
}

/* Select and Start held together: 0 not, 1 for less than 0.5 s (since
 * pad_combo_at), 2 the menu's been asked for (once a hold). */
static int pad_combo;
static u32 pad_combo_at;

/* The pad asks for the menu, or to leave it: Home just pressed, or Select
 * and Start held together for 0.5 s, keys bound to nothing. */
static int pad_menu_pressed(void)
{
  if (has_pad_binding(ACTION_MENU))
  {
    pad_combo = 0;
    return 0;
  }
  if (key_pressed(KEY_PAD_HOME) && !key_is_bound(KEY_PAD_HOME))
    return 1;
  if (!key_down(KEY_PAD_SELECT) || !key_down(KEY_PAD_START) || key_is_bound(KEY_PAD_SELECT) ||
      key_is_bound(KEY_PAD_START))
  {
    pad_combo = 0;
    return 0;
  }
  if (!pad_combo)
  {
    pad_combo = 1;
    pad_combo_at = nspire_ticks();
  }
  if (pad_combo == 1 && nspire_ticks() - pad_combo_at >= NSPIRE_TICK_HZ / 2)
  {
    pad_combo = 2;
    return 1;
  }
  return 0;
}

static int key_in_held_combo(unsigned key)
{
  int i, slot;
  for (i = FIRST_HOTKEY; i < END_HOTKEYS; i++)
    for (slot = 0; slot < KEY_SLOTS; slot++)
    {
      key_binding b = key_bindings[i][slot];
      if (b.with && (b.key == key || b.with == key) && binding_held(b))
        return 1;
    }
  return 0;
}

int hotkey_pressed(int action)
{
  int slot;
  for (slot = 0; slot < KEY_SLOTS; slot++)
  {
    key_binding b = key_bindings[action][slot];
    if (binding_pressed(b) && (b.with || !key_in_held_combo(b.key)))
      return 1;
  }
  return 0;
}

key_binding key_capture(void)
{
  key_binding b = { 0, 0 };
  unsigned first = 0, key;

  keys_wait_release();
  /* The first key pressed, then either another one while it's held (a
   * combination) or its release (a single key). An arrow or the touchpad
   * click gives up and returns no key. */
  while (!first)
  {
    /* Paced, as keys_wait_release is: the pad driver and the OS's sleeps
     * count time, which (without a timer) only waits move. */
    nspire_wait_until(nspire_ticks() + NSPIRE_TICK_HZ / 100);
    keys_poll();
    for (key = 1; key <= NUM_KEYS && !first; key++)
      if (key_pressed(key))
      {
        if (key_is_reserved(key))
        {
          keys_wait_release();
          return b;
        }
        if (key_is_bindable(key))
          first = key;
      }
  }
  for (;;)
  {
    nspire_wait_until(nspire_ticks() + NSPIRE_TICK_HZ / 100);
    keys_poll();
    if (!key_down(first))
    {
      b.key = (unsigned char)first;
      break;
    }
    for (key = 1; key <= NUM_KEYS; key++)
      if (key != first && key_pressed(key) && key_is_bindable(key))
      {
        b.with = (unsigned char)first;
        b.key = (unsigned char)key;
        break;
      }
    if (b.key)
      break;
  }
  keys_wait_release();
  return b;
}

/* ---- A frame of the game ---- */

/* R11w part 2 (2026-11-15): with a sound adapter in, the times key makes
 * the game louder and the divide key quieter, one of Settings'
 * 21 steps (nspire_audio_volume 0-20) a press, "Volume 14/20" for a
 * moment. The setting is Settings' Volume, saved as it is (the
 * settings written when the menu closes and at the quit). A key the user
 * bound to anything keeps its binding: the hotkey is off for that key. */
#define KEY_TIMES  73
#define KEY_DIVIDE 57

int nspire_volume_key_free(unsigned key)
{
  return !key_is_bound(key);
}

#ifdef NSPIRE_HOST
/* gpsp-host has no USB audio: its tests say an adapter is in (host_hw.c) */
int host_audio_adapter(void) __attribute__((weak));
#endif

static int adapter_in(void)
{
#ifdef NSPIRE_HOST
  if (host_audio_adapter && host_audio_adapter())
    return 1;
#endif
  return nspire_usbaudio_active();
}

static void volume_keys(void)
{
  int step = 0;
  unsigned v;

  if (!adapter_in())
    return;
  if (key_pressed(KEY_TIMES) && nspire_volume_key_free(KEY_TIMES))
    step = 1;
  else if (key_pressed(KEY_DIVIDE) && nspire_volume_key_free(KEY_DIVIDE))
    step = -1;
  if (!step)
    return;
  v = nspire_audio_enabled ? nspire_audio_volume % (NSPIRE_VOLUME_MAX + 1) : 0;
  if (step > 0 && v < NSPIRE_VOLUME_MAX)
    v++;
  else if (step < 0 && v > 0)
    v--;
  nspire_audio_volume = v;
  nspire_audio_enabled = 1;   /* (r19: no switch beside the volume: 0 is off) */
  nspire_osd_message("Volume %u/%u", v, (unsigned)NSPIRE_VOLUME_MAX);
}


/* Reads the keys for a frame: sets the GBA's buttons and returns the hotkeys
 * just pressed, one bit per action (1 << ACTION_MENU, ...). */
u32 nspire_input_frame(void)
{
  u32 gba_buttons = 0, turbo = 0, hotkeys = 0;
  int i;

  frame_poll = 1;
  keys_poll();
  frame_poll = 0;
  find_held_combos();

  /* The arrows are the D-pad; then each button's keys, and its turbo's. (r19: 7 9 1 3 do nothing unless
   * the player binds them: until r19 they were the diagonals, whatever the Keys page said; by default only
   * 8 4 5 6 are bound, and 7 9 1 3 stay blank unless the player binds them.) */
  if (key_down(KEY_UP)) gba_buttons |= BUTTON_UP;
  if (key_down(KEY_DOWN)) gba_buttons |= BUTTON_DOWN;
  if (key_down(KEY_LEFT)) gba_buttons |= BUTTON_LEFT;
  if (key_down(KEY_RIGHT)) gba_buttons |= BUTTON_RIGHT;
  for (i = 0; i < GBA_BUTTONS; i++)
  {
    if (action_held(i))
      gba_buttons |= gba_button_bits[i];
    if (action_held(TURBO_OF(i)))
      turbo |= gba_button_bits[i];
  }
  gba_buttons = nspire_turbo_buttons(gba_buttons, turbo);
  /* Opposite directions together are neither. */
  if ((gba_buttons & (BUTTON_UP | BUTTON_DOWN)) == (BUTTON_UP | BUTTON_DOWN))
    gba_buttons &= ~(BUTTON_UP | BUTTON_DOWN);
  if ((gba_buttons & (BUTTON_LEFT | BUTTON_RIGHT)) == (BUTTON_LEFT | BUTTON_RIGHT))
    gba_buttons &= ~(BUTTON_LEFT | BUTTON_RIGHT);

  if (serial_mode == SERIAL_MODE_GBP)
  {
    if (frame_counter > 20 && frame_counter < 100)
      gba_buttons = (frame_counter % 3) ? 0x3FF : 0x30F;
  }

  if ((gba_buttons | old_key) != old_key)
    trigger_key(gba_buttons);
  old_key = gba_buttons;
  write_ioreg(REG_P1, (~old_key) & 0x3FF);

  for (i = FIRST_HOTKEY; i < END_HOTKEYS; i++)
    if (hotkey_pressed(i) || pad_job(i, key_pressed))
      hotkeys |= 1u << i;
  /* Without a key for the menu, the menu key opens it, so it can't get lost. */
  if (!key_bindings[ACTION_MENU][0].key && !key_bindings[ACTION_MENU][1].key &&
      key_pressed(KEY_MENU))
    hotkeys |= 1u << ACTION_MENU;
  if (pad_menu_pressed())
    hotkeys |= 1u << ACTION_MENU;
  volume_keys();
  nspire_usbpad_frame(hotkeys);
  if (!NSPIRE_BT_IDLE())
    nspire_bt_glue_frame(hotkeys);
  nspire_drive_frame();
  return hotkeys;
}

/* The buttons the game sees while it's paused (menus, loading). */
void nspire_input_release_all(void)
{
  old_key = 0;
  write_ioreg(REG_P1, 0x3FF);
}

/* Here, before the file list, so a pad plugged in then works there. The
 * pads' usbpad_shutdown() runs at exit (atexit, nspire_usbpad_glue.h). */
void init_input(void)
{
  nspire_bt_glue_start();   /* (registered before the pads' first come-up of the port) */
  nspire_usbpad_start();
  nspire_bt_glue_atexit();  /* (its exit handler runs before the pad driver's) */
  nspire_drive_start();
  nspire_audio_start();
}

bool input_check_savestate(const u8 *src)
{
  const u8 *p = bson_find_document(src, "input");
  return (p && bson_contains_key(p, "prevkey", BSON_TYPE_INT32));
}

bool input_read_savestate(const u8 *src)
{
  const u8 *p = bson_find_document(src, "input");
  if (p)
    return bson_read_int32(p, "prevkey", &old_key);
  return false;
}

unsigned input_write_savestate(u8 *dst)
{
  u8 *wbptr1, *startp = dst;
  bson_start_document(dst, "input", wbptr1);
  bson_write_int32(dst, "prevkey", old_key);
  bson_finish_document(dst, wbptr1);
  return (unsigned int)(dst - startp);
}

/* ---- Menus ---- */

static gui_action_type held_direction = CURSOR_NONE;
static u32 repeat_at;
static unsigned repeats;

const unsigned char *nspire_gui_own_keys;

/* A key the menus already use for something else: it keeps that job. */
static int menu_key_taken(unsigned key)
{
  static const unsigned char taken[] =
    { KEY_TAB, KEY_MENU, KEY_ESC, KEY_ENTER, KEY_CLICK, KEY_SHIFT,
      KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, 0 };
  const unsigned char *k;
  int slot;

  if (key_is_reserved(key))
    return 1;
  for (k = taken; *k; k++)
    if (*k == key)
      return 1;
  for (k = nspire_gui_own_keys; k && *k; k++)
    if (*k == key)
      return 1;
  for (slot = 0; slot < KEY_SLOTS; slot++)
    if (key_bindings[ACTION_MENU][slot].key == key ||
        key_bindings[ACTION_BRIGHTER][slot].key == key ||
        key_bindings[ACTION_DIMMER][slot].key == key)
      return 1;
  return 0;
}

/* A binding of action (A or B) whose test passes, on keys the menus leave free. */
static int menu_button(int action, int (*test)(key_binding))
{
  int slot;
  for (slot = 0; slot < KEY_SLOTS; slot++)
  {
    key_binding b = key_bindings[action][slot];
    if (test(b) && !menu_key_taken(b.key) && (!b.with || !menu_key_taken(b.with)))
      return 1;
  }
  return 0;
}

static int binding_down_now(key_binding b)
{
  return b.key && b.key < KEY_PAD_FIRST && key_down_now(b.key) &&
         (!b.with || (b.with < KEY_PAD_FIRST && key_down_now(b.with)));
}

int nspire_gui_button_pressed(int action)
{
  return menu_button(action, binding_pressed) || pad_job(action, key_pressed);
}

int nspire_gui_button_down(int action)
{
  return menu_button(action, binding_held) || pad_job(action, key_down);
}

int nspire_gui_button_down_now(int action)
{
  return menu_button(action, binding_down_now);
}

unsigned gui_repeat_count(void)
{
  return repeats;
}

/* Arrows (or 8 2 5 4 6, in every menu: no number key starts or picks
 * anything) move and repeat while held: after 0.4 s, 15 times a second; a pad's D-pad and stick are the
 * arrows. The brightness keys work here too. */
gui_action_type get_gui_input(void)
{
  gui_action_type direction = CURSOR_NONE;
  u32 now;

  keys_poll();
  nspire_usbpad_menu_flush();

  if (hotkey_pressed(ACTION_BRIGHTER))
    nspire_brightness_step(1);
  if (hotkey_pressed(ACTION_DIMMER))
    nspire_brightness_step(-1);

  if (key_pressed(KEY_CLICK) || key_pressed(KEY_ENTER))
    return CURSOR_SELECT;
  if (key_pressed(KEY_ESC))
    return CURSOR_BACK;
  if (key_pressed(KEY_MENU) || hotkey_pressed(ACTION_MENU) || pad_menu_pressed())
    return CURSOR_EXIT;
  /* The keys bound to A and B are Enter and Esc too, and so are the pad's
   * keys doing their jobs. */
  if (nspire_gui_button_pressed(ACTION_A))
    return CURSOR_SELECT;
  if (nspire_gui_button_pressed(ACTION_B))
    return CURSOR_BACK;
  if (key_pressed(KEY_DEL))
    return CURSOR_CLEAR;

  if (key_down(KEY_UP) || key_down(KEY_8))
    direction = CURSOR_UP;
  else if (key_down(KEY_DOWN) || key_down(KEY_5) || key_down(KEY_2))
    direction = CURSOR_DOWN;
  else if (key_down(KEY_RIGHT) || key_down(KEY_6))
    direction = CURSOR_RIGHT;
  else if (key_down(KEY_LEFT) || key_down(KEY_4))
    direction = CURSOR_LEFT;

  now = nspire_ticks();
  if (direction != held_direction)
  {
    held_direction = direction;
    repeat_at = now + NSPIRE_TICK_HZ * 2 / 5;
    repeats = 0;
    return direction;
  }
  if (direction != CURSOR_NONE && (s32)(now - repeat_at) >= 0)
  {
    repeat_at = now + NSPIRE_TICK_HZ / 15;
    repeats++;
    return direction;
  }
  return CURSOR_NONE;
}
