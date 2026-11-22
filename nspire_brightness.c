/* Screen brightness: see nspire_brightness.h.
 *
 * The CX II's OS sets the LCD backlight at 0x90130014, from 0 (brightest) to
 * 225 (darkest). An older CX's backlight is its LCD contrast at 0x900F0020,
 * higher brighter: 0x11A to 0x1CE is the range Hackspire gives for it (0x174
 * normal; 0x100 is off, so the dimmest level stays above that). gpSP's
 * levels spread over either range in equal steps, and the value the OS had
 * is written back when gpSP closes. A register that reads outside its range
 * isn't what it seems, and the backlight is left alone. */

#include "common.h"
#include "nspire.h"
#include "nspire_hw.h"
#include "nspire_frontend.h"
#include "nspire_brightness.h"

#include <os.h>
#include <stdio.h>

#define DARKEST 225u
#define CX_DIMMEST  0x11Au
#define CX_BRIGHTEST 0x1CEu

u32 nspire_brightness;

static int have_backlight;   /* the register was read and looks right */
static u32 backlight_reg;    /* HW_BACKLIGHT (CX II) or HW_CX_BACKLIGHT */
static u32 os_value;         /* the calculator's own */
static int changed;          /* gpSP wrote the backlight */

u32 nspire_brightness_value(u32 level)
{
  if (level < 1)
    level = 1;
  if (level > BRIGHTNESS_LEVELS)
    level = BRIGHTNESS_LEVELS;
  return (BRIGHTNESS_LEVELS - level) * DARKEST / (BRIGHTNESS_LEVELS - 1);
}

u32 nspire_brightness_level(u32 value)
{
  if (value > DARKEST)
    value = DARKEST;
  return BRIGHTNESS_LEVELS - (value * (BRIGHTNESS_LEVELS - 1) + DARKEST / 2) / DARKEST;
}

u32 nspire_brightness_cx_value(u32 level)
{
  if (level < 1)
    level = 1;
  if (level > BRIGHTNESS_LEVELS)
    level = BRIGHTNESS_LEVELS;
  return CX_DIMMEST + (level - 1) * (CX_BRIGHTEST - CX_DIMMEST) / (BRIGHTNESS_LEVELS - 1);
}

u32 nspire_brightness_cx_level(u32 value)
{
  if (value < CX_DIMMEST)
    value = CX_DIMMEST;
  if (value > CX_BRIGHTEST)
    value = CX_BRIGHTEST;
  return 1 + ((value - CX_DIMMEST) * (BRIGHTNESS_LEVELS - 1) + (CX_BRIGHTEST - CX_DIMMEST) / 2) /
             (CX_BRIGHTEST - CX_DIMMEST);
}

static u32 value_for(u32 level)
{
  return is_cx2 ? nspire_brightness_value(level) : nspire_brightness_cx_value(level);
}

static u32 level_for(u32 value)
{
  return is_cx2 ? nspire_brightness_level(value) : nspire_brightness_cx_level(value);
}

void nspire_brightness_init(void)
{
  u32 value;

  have_backlight = changed = 0;
  backlight_reg = is_cx2 ? HW_BACKLIGHT : HW_CX_BACKLIGHT;
  value = HW_READ(backlight_reg);
  /* Something other than the OS's 0..255 (CX II), or than a lit backlight
   * up to 0x1FF (older CX), means the register isn't what it seems: then
   * gpSP leaves the backlight alone. */
  if (is_cx2 ? value > 255 : value <= 0x100 || value > 0x1FF)
    return;
  os_value = value;
  have_backlight = 1;
}

static void write_backlight(u32 value)
{
  HW_WRITE(backlight_reg, value);
  changed = 1;
}

void nspire_brightness_apply(void)
{
  if (nspire_brightness > BRIGHTNESS_LEVELS)
    nspire_brightness = 0;
  if (!have_backlight)
    return;
  if (nspire_brightness)
    write_backlight(value_for(nspire_brightness));
  else if (changed)
    write_backlight(os_value);
}

void nspire_brightness_step(int delta)
{
  s32 level;

  if (!have_backlight)
  {
    nspire_osd_message("Brightness: not on this calculator");
    return;
  }
  level = nspire_brightness ? (s32)nspire_brightness : (s32)level_for(os_value);
  level += delta;
  if (level < 1)
    level = 1;
  if (level > BRIGHTNESS_LEVELS)
    level = BRIGHTNESS_LEVELS;
  nspire_brightness = (u32)level;
  nspire_brightness_apply();
  nspire_osd_message("Brightness %d/%d", (int)level, BRIGHTNESS_LEVELS);
}

void nspire_brightness_report(char *text, u32 size)
{
  if (have_backlight && is_cx2)
    snprintf(text, size, "backlight register %u at start (0 brightest, 225 darkest), setting %u",
             (unsigned)os_value, (unsigned)nspire_brightness);
  else if (have_backlight)
    snprintf(text, size, "CX backlight (900F0020) %03x at start (11a dimmest, 1ce brightest), setting %u",
             (unsigned)os_value, (unsigned)nspire_brightness);
  else
    snprintf(text, size, "backlight register not used (it read %03x, out of range)",
             (unsigned)HW_READ(backlight_reg));
}

void nspire_brightness_restore(void)
{
  if (have_backlight && changed)
  {
    HW_WRITE(backlight_reg, os_value);
    changed = 0;
  }
}
