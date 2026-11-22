/* What the Controls screen can bind (nspire_bind.h): the rows, and putting
 * keys on actions. No drawing here: nspire_controls.c draws it. */

#include <stdio.h>
#include <string.h>

#include "nspire_bind.h"
#include "nspire_usbpad_glue.h"

/* A GBA button's row has its turbo too; the hotkeys follow. */
const struct bind_row bind_rows[] =
{
  { ACTION_UP, 1, "Up", 0 },
  { ACTION_DOWN, 1, "Down", 0 },
  { ACTION_LEFT, 1, "Left", 0 },
  { ACTION_RIGHT, 1, "Right", 0 },
  { ACTION_A, 1, "A", KEY_PAD_A },
  { ACTION_B, 1, "B", KEY_PAD_B },
  { ACTION_L, 1, "L", KEY_PAD_L1 },
  { ACTION_R, 1, "R", KEY_PAD_R1 },
  { ACTION_START, 1, "Start", KEY_PAD_START },
  { ACTION_SELECT, 1, "Select", KEY_PAD_SELECT },
  { ACTION_MENU, 0, "Menu", KEY_PAD_HOME },
  { ACTION_FAST_FORWARD, 0, "Fast forward", 0 },
  { ACTION_SAVE_STATE, 0, "Save state", 0 },
  { ACTION_LOAD_STATE, 0, "Load state", 0 },
  { ACTION_LOAD_NEWEST, 0, "Load newest", 0 },
  { ACTION_QUIT, 0, "Save & quit", 0 },
  { ACTION_NEXT_SLOT, 0, "Next slot", 0 },
  { ACTION_PREVIOUS_SLOT, 0, "Prev slot", 0 },
  { ACTION_SHOW_FPS, 0, "FPS counter", 0 },
  { ACTION_RESTART, 0, "Restart game", 0 },
  /* (r19: named Brightness up and Brightness down, clearer than Brighter and Dimmer) */
  { ACTION_BRIGHTER, 0, "Brightness up", 0 },
  { ACTION_DIMMER, 0, "Brightness down", 0 },
};
const int bind_row_count = (int)(sizeof(bind_rows) / sizeof(bind_rows[0]));

/* Every action is a row's, or a row's turbo. */
typedef char every_action_has_a_row[GBA_BUTTONS + (END_HOTKEYS - FIRST_HOTKEY) ==
                                    sizeof(bind_rows) / sizeof(bind_rows[0]) &&
                                    FIRST_TURBO + GBA_BUTTONS == NUM_ACTIONS ? 1 : -1];

int bind_row_of(int action)
{
  int i;
  if (action < 0 || action >= NUM_ACTIONS)
    return -1;
  action = BUTTON_OF(action);
  for (i = 0; i < bind_row_count; i++)
    if (bind_rows[i].action == action)
      return i;
  return -1;
}

const char *bind_action_name(int action)
{
  static const char *const turbo_names[GBA_BUTTONS] =
  {
    "Turbo Up", "Turbo Down", "Turbo Left", "Turbo Right", "Turbo A", "Turbo B", "Turbo L", "Turbo R",
    "Turbo Start", "Turbo Select"
  };
  int row = bind_row_of(action);
  if (row < 0)
    return "?";
  return IS_TURBO(action) ? turbo_names[action - FIRST_TURBO] : bind_rows[row].name;
}

static int is_pad_key(unsigned key)
{
  return key >= KEY_PAD_FIRST;
}

int bind_in_view(key_binding b, int pad)
{
  if (!b.key)
    return 0;
  return is_pad_key(b.key) == !!pad && (!b.with || is_pad_key(b.with) == !!pad);
}

/* A controller button in any binding. */
static int pad_key_bound(unsigned key)
{
  return keys_key_used(key_bindings, key, NUM_ACTIONS);
}

/* An action with a controller button of its own (nspire_input.c: its
 * default one does nothing then). */
static int has_pad_binding(int action)
{
  int s;
  for (s = 0; s < KEY_SLOTS; s++)
    if (is_pad_key(key_bindings[action][s].key) || is_pad_key(key_bindings[action][s].with))
      return 1;
  return 0;
}

int bind_shown(int action, int pad, key_binding out[KEY_SLOTS])
{
  int n = 0, s, row = bind_row_of(action);

  memset(out, 0, sizeof(key_binding) * KEY_SLOTS);
  if (row < 0)
    return 0;
  for (s = 0; s < KEY_SLOTS; s++)
    if (bind_in_view(key_bindings[action][s], pad))
      out[n++] = key_bindings[action][s];
  if (pad && !n && !IS_TURBO(action) && bind_rows[row].pad_default && !has_pad_binding(action) &&
      !pad_key_bound(bind_rows[row].pad_default))
    out[n++].key = bind_rows[row].pad_default;
  return n;
}

int bind_owner(key_binding b, int pad, int except)
{
  key_binding shown[KEY_SLOTS];
  int a, s, n;

  if (!b.key)
    return -1;
  for (a = 0; a < NUM_ACTIONS; a++)
  {
    if (a == except)
      continue;
    n = bind_shown(a, pad, shown);
    for (s = 0; s < n; s++)
      if (shown[s].key == b.key && shown[s].with == b.with)
        return a;
  }
  return -1;
}

void bind_set(int action, key_binding b, int pad)
{
  key_binding *k = key_bindings[action];
  int s, put = -1;
  for (s = 0; s < KEY_SLOTS; s++)
    if (bind_in_view(k[s], pad))
    {
      if (put < 0)
        put = s;
      else
        k[s].key = k[s].with = 0;   /* a second one of its kind goes */
    }
  if (put < 0)
    for (s = 0; s < KEY_SLOTS && put < 0; s++)
      if (!k[s].key)
        put = s;
  if (put < 0)
    put = KEY_SLOTS - 1;
  k[put] = b;
}

void bind_take(int action, key_binding b)
{
  int i, s;
  for (i = 0; i < NUM_ACTIONS; i++)
    if (i != action)
      for (s = 0; s < KEY_SLOTS; s++)
        if (key_bindings[i][s].key == b.key && key_bindings[i][s].with == b.with)
          key_bindings[i][s].key = key_bindings[i][s].with = 0;
}

void bind_clear(int action, int pad)
{
  int s;
  for (s = 0; s < KEY_SLOTS; s++)
    if (bind_in_view(key_bindings[action][s], pad))
      key_bindings[action][s].key = key_bindings[action][s].with = 0;
}

int bind_pad_family;

/* Each family's names for KEY_PAD_B .. KEY_PAD_AUX (usbpad.h's bits: the
 * bottom, right, left and top face buttons, L1 R1 L2 R2, Select, Start,
 * Home, L3 R3, Aux), as printed on the pads: by position, Xbox 360, Xbox
 * One / Series, DualShock 4, DualSense, Switch Pro. */
#define PAD_NAMES 14
static const char *const pad_names[PAD_FAMILIES][PAD_NAMES] =
{
  { "Bottom", "Right", "Left", "Top", "L1", "R1", "L2", "R2", "Select", "Start", "Home", "L3", "R3", "Aux" },
  { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start", "Guide", "LS", "RS", "Aux" },
  { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "View", "Menu", "Xbox", "LS", "RS", "Share" },
  { "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "Share", "Options", "PS", "L3", "R3", "Touchpad" },
  { "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "Create", "Options", "PS", "L3", "R3", "Touchpad" },
  { "B", "A", "Y", "X", "L", "R", "ZL", "ZR", "Minus", "Plus", "Home", "LS", "RS", "Capture" },
};
typedef char pad_names_follow_the_keys[KEY_PAD_B - KEY_PAD_FIRST == 4 && KEY_PAD_A == KEY_PAD_B + 1 &&
                                       KEY_PAD_Y == KEY_PAD_B + 2 && KEY_PAD_X == KEY_PAD_B + 3 &&
                                       KEY_PAD_L1 == KEY_PAD_B + 4 && KEY_PAD_SELECT == KEY_PAD_B + 8 &&
                                       KEY_PAD_AUX == KEY_PAD_B + PAD_NAMES - 1 ? 1 : -1];

int bind_pad_labels;

/* The physical face button behind a pad key: with layout=labels the glue
 * swaps bottom and right, left and top (nspire_usbpad_glue.c pad_keys). */
static unsigned physical(unsigned key)
{
  if (!bind_pad_labels)
    return key;
  switch (key)
  {
  case KEY_PAD_B: return KEY_PAD_A;
  case KEY_PAD_A: return KEY_PAD_B;
  case KEY_PAD_Y: return KEY_PAD_X;
  case KEY_PAD_X: return KEY_PAD_Y;
  default: return key;
  }
}

const char *bind_pad_name(unsigned key, int family)
{
  if (family < 0 || family >= PAD_FAMILIES)
    family = PAD_FAMILY_NONE;
  key = physical(key);
  if (key >= KEY_PAD_B && key < KEY_PAD_B + PAD_NAMES)
    return pad_names[family][key - KEY_PAD_B];
  return key_name(key) ? key_name(key) : "?";
}

const char *bind_pad_side(unsigned key)
{
  switch (physical(key))
  {
  case KEY_PAD_A: return "right";
  case KEY_PAD_B: return "bottom";
  case KEY_PAD_X: return "top";
  case KEY_PAD_Y: return "left";
  default: return NULL;
  }
}

void bind_text(key_binding b, int pad, char *text, size_t size)
{
  if (!pad)
    binding_name(b, text, size);
  else if (b.with)
    snprintf(text, size, "%s+%s", bind_pad_name(b.with, bind_pad_family), bind_pad_name(b.key, bind_pad_family));
  else
    snprintf(text, size, "%s", bind_pad_name(b.key, bind_pad_family));
}
