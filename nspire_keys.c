/* Key names and bindings (nspire_keys.h); the keypad itself is read in
 * nspire_input.c. */

#include <stdio.h>
#include <string.h>

#include "nspire_keys.h"

/* gpSP's names, by key number (1 + the bit in the keypad registers). The
 * arrows (113-120) have none: they always move. The last line is the USB
 * gamepad's word (nspire_keys.h KEY_PAD_*): its D-pad bits have none
 * either, as they press the arrows. */
char *nspire_key_strings[NUM_KEYS + 1] =
{
    "NONE",
	"return", "enter", NULL, "(-)", "space", "Z", "Y", "0", "?!", "home", NULL, NULL, NULL, NULL, NULL, NULL,
    "X", "W", "V", "3", "U", "T", "S", "1", "pi", "trig", "10^x", NULL, NULL, NULL, NULL, NULL,
    "R", "Q", "P", "6", "O", "N", "M", "4", "EE", "x^2", NULL, NULL, NULL, NULL, NULL, NULL,
    "L", "K", "J", "9", "I", "H", "G", "7", "/", "e^x", NULL, NULL, NULL, NULL, NULL, NULL,
    "F", "E", "D", NULL, "C", "B", "A", "=", "*", "^", NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, "var", "-", ")", ".", "(", "5", "cat", "frac", "del", "scratch", NULL, NULL, NULL, NULL, NULL,
    "flag", "click", "+", "doc", "2", "menu", "8", "esc", NULL, "tab", NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, "shift", "ctrl", ",", NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, "Pad B", "Pad A", "Pad Y", "Pad X", "Pad L1", "Pad R1", "Pad L2", "Pad R2",
    "Pad Select", "Pad Start", "Pad Home", "Pad L3", "Pad R3", "Pad Aux", NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

/* ---- Turbo ---- */

/* A button held on a key bound to its turbo is pressed and let go over
 * and over, speed (1-30) times a second of the game: frame n of the hold
 * (the game's frames, so fast forward doesn't change what the game sees) is
 * down while n * speed / 30 is even. The first frame is always down, so a
 * tap is an ordinary press. 60 frames are a whole number of presses at
 * every speed, so the count starts over there. A key bound to the plain
 * button held at the same time keeps the button down (the two are ORed);
 * the turbo's count goes on under it. */
const unsigned short gba_button_bits[GBA_BUTTONS] =
  { 0x040, 0x080, 0x020, 0x010, 0x001, 0x002, 0x200, 0x100, 0x008, 0x004 };
static unsigned char turbo_frame[GBA_BUTTONS];   /* frames since this button's turbo went down */

unsigned int nspire_turbo_buttons(unsigned int plain, unsigned int turbo)
{
  unsigned int speed = nspire_turbo_speed % TURBO_SPEEDS + 1;
  int i;

  for (i = 0; i < GBA_BUTTONS; i++)
  {
    unsigned int bit = gba_button_bits[i];
    if (!(turbo & bit))
    {
      turbo_frame[i] = 0;
      continue;
    }
    if (!(((unsigned int)turbo_frame[i] * speed / 30) & 1))
      plain |= bit;
    if (++turbo_frame[i] >= 60)
      turbo_frame[i] = 0;
  }
  return plain;
}

/* The old choices' buttons (A 1, B 2, L 4, R 8): off, all, then each one,
 * the pairs, the threes (the old "Turbo" setting's order). */
static const unsigned char old_turbo_masks[OLD_TURBO_CHOICES] =
  { 0, 15, 1, 2, 4, 8, 3, 12, 5, 6, 9, 10, 7, 11, 13, 14 };

unsigned int keys_old_turbo_mask(unsigned int choice)
{
  return old_turbo_masks[choice % OLD_TURBO_CHOICES];
}

void keys_move_turbo(key_binding bindings[NUM_ACTIONS][KEY_SLOTS], int button, int to_turbo)
{
  key_binding *from = bindings[to_turbo ? button : TURBO_OF(button)];
  key_binding *to = bindings[to_turbo ? TURBO_OF(button) : button];
  int s, t;

  for (s = 0; s < KEY_SLOTS; s++)
  {
    if (!from[s].key)
      continue;
    for (t = 0; t < KEY_SLOTS && to[t].key; t++)
      ;
    if (t == KEY_SLOTS)
      break;   /* no room: the rest stay where they are */
    to[t] = from[s];
    from[s].key = from[s].with = 0;
  }
}

int keys_turbo_from_old(key_binding bindings[NUM_ACTIONS][KEY_SLOTS], unsigned int old_mask,
                        const key_binding old_turbo_key[KEY_SLOTS])
{
  static const unsigned char old_buttons[4] = { ACTION_A, ACTION_B, ACTION_L, ACTION_R };
  /* the controller button that did each while bound to nothing (nspire_input.c pad_jobs) */
  static const unsigned char pad_jobs[4] = { KEY_PAD_A, KEY_PAD_B, KEY_PAD_L1, KEY_PAD_R1 };
  int i, made = 0;

  memset(bindings[FIRST_TURBO], 0, sizeof(key_binding) * GBA_BUTTONS * KEY_SLOTS);
  for (i = 0; i < 4; i++)
  {
    int button = old_buttons[i], t, s, put = 0, own_pad = 0;
    if (old_mask & (1u << i))
    {
      for (s = 0; s < KEY_SLOTS; s++)
        own_pad |= bindings[button][s].key >= KEY_PAD_FIRST || bindings[button][s].with >= KEY_PAD_FIRST;
      keys_move_turbo(bindings, button, 1);
      /* its controller button by default repeated too: it's the turbo's
       * now (bound, it no longer does the plain button) */
      if (!own_pad && !keys_key_used(bindings, pad_jobs[i], NUM_ACTIONS))
        for (s = 0; s < KEY_SLOTS; s++)
          if (!bindings[TURBO_OF(button)][s].key)
          {
            bindings[TURBO_OF(button)][s].key = pad_jobs[i];
            break;
          }
      for (s = 0; s < KEY_SLOTS; s++)
        made += bindings[TURBO_OF(button)][s].key != 0;
      continue;
    }
    /* The Turbo key held, the button's own key pressed: a combination, of
     * keys of one kind (the calculator's, or a controller's). */
    for (t = 0; t < KEY_SLOTS; t++)
    {
      key_binding turbo = old_turbo_key[t];
      if (!turbo.key || turbo.with)
        continue;
      for (s = 0; s < KEY_SLOTS && put < KEY_SLOTS; s++)
      {
        key_binding own = bindings[button][s];
        if (!own.key || own.with || own.key == turbo.key ||
            (own.key >= KEY_PAD_FIRST) != (turbo.key >= KEY_PAD_FIRST))
          continue;
        bindings[TURBO_OF(button)][put].key = own.key;
        bindings[TURBO_OF(button)][put].with = turbo.key;
        put++;
        made++;
      }
    }
  }
  return made;
}

/* ---- Bindings ---- */

key_binding key_bindings[NUM_ACTIONS][KEY_SLOTS];

const char *const action_names[NUM_ACTIONS] =
{
  "D-pad up", "D-pad down", "D-pad left", "D-pad right",
  "A", "B", "L", "R", "Start", "Select",
  "Menu", "Fast forward", "Load state", "Save state", "Save+Exit",
  "Load newest state", "Next slot", "Previous slot", "FPS counter", "Restart game",
  "Brightness up", "Brightness down",
  "Turbo up", "Turbo down", "Turbo left", "Turbo right", "Turbo A", "Turbo B", "Turbo L", "Turbo R",
  "Turbo Start", "Turbo Select"
};

/* gpSP's layout, plus the new hotkeys, which start without a key except
 * the slots, ) next and ( previous as in PocketSNES, and brightness, + and
 * - (as the OS has them with ctrl); turbo A, B, L and R are = held with
 * their key (= was the Turbo key, as in PocketSNES: what an old settings
 * file with the Turbo key on = turns into). */
static const key_binding default_bindings[NUM_ACTIONS] =
{
  { KEY_8, 0 }, { KEY_5, 0 }, { KEY_4, 0 }, { KEY_6, 0 }, { KEY_DEL, 0 }, { KEY_VAR, 0 },
  { KEY_SCRATCH, 0 }, { KEY_DOC, 0 }, { KEY_SHIFT, 0 }, { KEY_CTRL, 0 },
  { KEY_MENU, 0 }, { KEY_F, 0 }, { KEY_L, 0 }, { KEY_S, 0 }, { KEY_Q, 0 },
  { 0, 0 }, { KEY_RPAREN, 0 }, { KEY_LPAREN, 0 }, { 0, 0 }, { 0, 0 },
  { KEY_PLUS, 0 }, { KEY_MINUS, 0 },
  { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
  { KEY_DEL, KEY_EQUALS }, { KEY_VAR, KEY_EQUALS }, { KEY_SCRATCH, KEY_EQUALS }, { KEY_DOC, KEY_EQUALS },
  { 0, 0 }, { 0, 0 }
};

void keys_set_defaults(key_binding bindings[NUM_ACTIONS][KEY_SLOTS])
{
  int i;
  memset(bindings, 0, sizeof(key_binding) * NUM_ACTIONS * KEY_SLOTS);
  for (i = 0; i < NUM_ACTIONS; i++)
    bindings[i][0] = default_bindings[i];
}

/* A binding uses key (as its key or the one held with it). */
static int uses(key_binding b, unsigned key)
{
  return key && (b.key == key || b.with == key);
}

void keys_drop_clashing_defaults(key_binding bindings[NUM_ACTIONS][KEY_SLOTS], int first)
{
  int i, j, slot;

  for (i = first; i < NUM_ACTIONS; i++)
    for (j = 0; j < first; j++)
      for (slot = 0; slot < KEY_SLOTS; slot++)
        if (default_bindings[i].key && !default_bindings[i].with &&
            uses(bindings[j][slot], default_bindings[i].key))
          memset(bindings[i], 0, sizeof(bindings[i]));
}

void keys_add_default_if_free(key_binding bindings[NUM_ACTIONS][KEY_SLOTS], int action)
{
  int i, slot;
  unsigned key = default_bindings[action].with ? 0 : default_bindings[action].key;

  if (!key)
    return;
  for (i = 0; i < NUM_ACTIONS; i++)
    for (slot = 0; slot < KEY_SLOTS; slot++)
      if (bindings[i][slot].key == key || bindings[i][slot].with == key ||
          (i == action && bindings[i][slot].key))
        return;
  bindings[action][0].key = (unsigned char)key;
  bindings[action][0].with = 0;
}

int keys_key_used(key_binding bindings[NUM_ACTIONS][KEY_SLOTS], unsigned key, int end)
{
  int i, slot;
  for (i = 0; i < end && i < NUM_ACTIONS; i++)
    for (slot = 0; slot < KEY_SLOTS; slot++)
      if (uses(bindings[i][slot], key))
        return 1;
  return 0;
}

const char *key_name(unsigned key)
{
  return key <= NUM_KEYS ? nspire_key_strings[key] : NULL;
}

int key_is_reserved(unsigned key)
{
  return (key >= KEY_UP && key <= KEY_LEFTUP) || key == KEY_CLICK;
}

int key_is_bindable(unsigned key)
{
  return key > 0 && key <= NUM_KEYS && key_name(key) && !key_is_reserved(key);
}

const char *binding_name(key_binding b, char *text, size_t size)
{
  const char *key = key_name(b.key), *with = key_name(b.with);

  if (!b.key || !key)
    snprintf(text, size, "none");
  else if (b.with && with)
    snprintf(text, size, "%s+%s", with, key);
  else
    snprintf(text, size, "%s", key);
  return text;
}

unsigned binding_pack(key_binding b)
{
  return b.key | ((unsigned)b.with << 8);
}

key_binding binding_unpack(unsigned packed)
{
  key_binding b = { 0, 0 };
  unsigned key = packed & 0xFF, with = (packed >> 8) & 0xFF;

  if (key_is_bindable(key))
  {
    b.key = (unsigned char)key;
    if (with != key && key_is_bindable(with))
      b.with = (unsigned char)with;
  }
  return b;
}

