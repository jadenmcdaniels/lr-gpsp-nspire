/* Calculator keys and what they're bound to.
 *
 * Keys are numbered 1 + their bit in the keypad registers at 0x900E0010
 * (gpSP's numbering, shared with PocketSNES's settings files); 0 is no key.
 * Each action has two bindings, and a binding is a key or a combination
 * ('key' pressed while 'with' is held). One key may be in several actions'
 * bindings. */
#ifndef NSPIRE_KEYS_H
#define NSPIRE_KEYS_H

#include <stddef.h>

/* The GBA buttons, then the hotkeys, then the GBA buttons again as turbo
 * (the button pressed and let go over and over while the key is held:
 * nspire_turbo_buttons). The first 15 are gpSP's old gamepad_config_map
 * entries. A key may be bound to a button and another key to its turbo:
 * "del = A" and "= = Turbo A" side by side. */
enum nspire_action
{
  ACTION_UP,
  ACTION_DOWN,
  ACTION_LEFT,
  ACTION_RIGHT,
  ACTION_A,
  ACTION_B,
  ACTION_L,
  ACTION_R,
  ACTION_START,
  ACTION_SELECT,
  ACTION_MENU,
  ACTION_FAST_FORWARD,
  ACTION_LOAD_STATE,
  ACTION_SAVE_STATE,
  ACTION_QUIT,            /* "Save+Exit": saves a state if that's on, then leaves */
  ACTION_LOAD_NEWEST,
  ACTION_NEXT_SLOT,
  ACTION_PREVIOUS_SLOT,
  ACTION_SHOW_FPS,
  ACTION_RESTART,
  ACTION_BRIGHTER,        /* screen brightness (CX II) */
  ACTION_DIMMER,
  ACTION_TURBO_UP,        /* ACTION_UP's turbo; then each GBA button's, in their order */
  ACTION_TURBO_DOWN,
  ACTION_TURBO_LEFT,
  ACTION_TURBO_RIGHT,
  ACTION_TURBO_A,
  ACTION_TURBO_B,
  ACTION_TURBO_L,
  ACTION_TURBO_R,
  ACTION_TURBO_START,
  ACTION_TURBO_SELECT,
  NUM_ACTIONS
};

#define FIRST_HOTKEY ACTION_MENU
/* The hotkeys are FIRST_HOTKEY to END_HOTKEYS - 1 (a bit each in a u32). */
#define END_HOTKEYS ACTION_TURBO_UP
#define FIRST_TURBO ACTION_TURBO_UP
#define GBA_BUTTONS 10            /* ACTION_UP to ACTION_SELECT */
#define TURBO_OF(action) (FIRST_TURBO + (action))
#define IS_TURBO(action) ((action) >= FIRST_TURBO && (action) < FIRST_TURBO + GBA_BUTTONS)
/* An action that presses a GBA button (plain or turbo). */
#define IS_BUTTON(action) ((action) < GBA_BUTTONS || IS_TURBO(action))
/* The GBA button an action presses (plain or turbo): ACTION_UP to ACTION_SELECT. */
#define BUTTON_OF(action) (IS_TURBO(action) ? (action) - FIRST_TURBO : (action))
#define KEY_SLOTS 2

#define KEY_ENTER   2
#define KEY_0       8
#define KEY_Q       34
#define KEY_S       23
#define KEY_V       19
#define KEY_M       39
#define KEY_U       21
#define KEY_T       22
#define KEY_A       71
#define KEY_C       69
#define KEY_D       67
#define KEY_L       49
#define KEY_F       65
#define KEY_1       24
#define KEY_3       20
#define KEY_4       40
#define KEY_5       87
#define KEY_6       36
#define KEY_7       56
#define KEY_9       52
#define KEY_2       101
#define KEY_8       103
#define KEY_VAR     82
#define KEY_RPAREN  84
#define KEY_LPAREN  86
#define KEY_MINUS   83
#define KEY_DEL     90
#define KEY_SCRATCH 91
#define KEY_CLICK   98
#define KEY_PLUS    99
#define KEY_DOC     100
#define KEY_MENU    102
#define KEY_ESC     104
#define KEY_TAB     106
#define KEY_EQUALS  72
#define KEY_UP      113
#define KEY_RIGHT   115
#define KEY_DOWN    117
#define KEY_LEFT    119
#define KEY_LEFTUP  120
#define KEY_SHIFT   121
#define KEY_CTRL    122

/* A USB gamepad's buttons (nspire_usbpad_glue.h) are keys too, a fifth
 * word after the keypad's four: key 129 + the button's bit in
 * nspire-usbpad's usbpad.h (USBPAD_SOUTH is bit 4: key 133), both pads'
 * together. The D-pad's bits (0-3) have no key: the pad's D-pad and left
 * stick press the arrows. The face buttons are named by where they are on
 * a Nintendo pad (Pad A the right one, Pad B the bottom one), unless the
 * pad settings file says "labels" for a pad (then Pad A is the bottom one,
 * as on an Xbox pad's labels). */
#define KEY_PAD_FIRST  129
#define KEY_PAD_B      133   /* bottom: Xbox A, PlayStation cross */
#define KEY_PAD_A      134   /* right: Xbox B, circle */
#define KEY_PAD_Y      135   /* left: Xbox X, square */
#define KEY_PAD_X      136   /* top: Xbox Y, triangle */
#define KEY_PAD_L1     137
#define KEY_PAD_R1     138
#define KEY_PAD_L2     139
#define KEY_PAD_R2     140
#define KEY_PAD_SELECT 141   /* Back, View, Share, Create, minus */
#define KEY_PAD_START  142   /* Start, Menu, Options, plus */
#define KEY_PAD_HOME   143   /* Guide, PS, Home */
#define KEY_PAD_L3     144
#define KEY_PAD_R3     145
#define KEY_PAD_AUX    146   /* Capture, touchpad click, mic */
#define KEY_WORDS   5
#define NUM_KEYS    (KEY_WORDS * 32)

typedef struct
{
  unsigned char key;
  unsigned char with;
} key_binding;

extern key_binding key_bindings[NUM_ACTIONS][KEY_SLOTS];
extern const char *const action_names[NUM_ACTIONS];

void keys_set_defaults(key_binding bindings[NUM_ACTIONS][KEY_SLOTS]);
/* For settings from builds that had only actions 0 to first - 1: the newer
 * actions keep their default keys only where no older one uses that key. */
void keys_drop_clashing_defaults(key_binding bindings[NUM_ACTIONS][KEY_SLOTS], int first);
/* For settings written before an action had a default key: gives it that
 * key if the action has none and no action uses the key. */
void keys_add_default_if_free(key_binding bindings[NUM_ACTIONS][KEY_SLOTS], int action);
/* Whether an action before 'end' has key in a binding (as its key or the
 * one held with it). */
int keys_key_used(key_binding bindings[NUM_ACTIONS][KEY_SLOTS], unsigned key, int end);
/* A key's name, or NULL for a key that doesn't exist. */
const char *key_name(unsigned key);
/* Arrows and the touchpad click can't be bound: they always move. */
int key_is_reserved(unsigned key);
int key_is_bindable(unsigned key);
/* "ctrl", "shift+S" or "none". */
const char *binding_name(key_binding b, char *text, size_t size);
/* A binding as one number for the settings files: key | with << 8. */
unsigned binding_pack(key_binding b);
key_binding binding_unpack(unsigned packed);

/* Reads the keypad (nspire_input.c). */
void keys_poll(void);
int key_down(unsigned key);
int key_pressed(unsigned key);   /* went down at the last poll */
int any_key_down(void);
/* LNK-1: a keypad key down now, read from the keypad itself; the frame's
 * keys (key_down, key_pressed) stay as they are. */
int key_down_now(unsigned key);
/* Any keypad key down now, the same way: what stops the menus' look-ups of
 * files (states_yield) so a key isn't kept waiting. */
int keys_down_now(void);
int binding_held(key_binding b);
/* A hotkey fires on a binding that was just pressed. A single key that is
 * part of a combination held for another hotkey doesn't fire: with S on
 * "Save state" and shift+S on "Load state", shift+S only loads. */
int hotkey_pressed(int action);
/* R11w part 2: the times (73) and divide (57) keys change the sound's volume
 * in a game while a sound adapter is in, unless the user bound the key to
 * something (then the binding wins): 1 if key is free for that. */
int nspire_volume_key_free(unsigned key);

/* Waits for a key or a combination for the key configuration: hold one key
 * and press another for a combination. Returns no key (key 0) if an arrow or
 * the touchpad click is pressed first, which cancels. */
key_binding key_capture(void);
/* Waits until no key is down. */
void keys_wait_release(void);
/* R6: a key down on the keypad, the touchpad pressed or the on key, as the
 * hardware has them (nspire_exit.c's wait before the OS gets its keypad). */
int keys_raw_down(void);
/* The keys down now count as up until they're released: a key that started
 * a game or closed a menu doesn't reach the game. */
void keys_mask_held(void);
/* Turbo (nspire_keys.c): a key bound to a button's turbo (ACTION_TURBO_*)
 * presses that button and lets it go over and over while it's held, at
 * 1 + nspire_turbo_speed (0-29) presses a second. */
#define TURBO_SPEEDS 30
#define TURBO_SPEED_DEFAULT 14   /* 15 presses a second */
extern unsigned int nspire_turbo_speed;
/* The GBA's bit (REG_P1: A 1, B 2, Select 4, Start 8, Right 0x10, Left 0x20,
 * Up 0x40, Down 0x80, R 0x100, L 0x200) for ACTION_UP to ACTION_SELECT. */
extern const unsigned short gba_button_bits[GBA_BUTTONS];
/* A frame's GBA buttons: 'plain' (held on keys bound to the buttons) as
 * they are, ORed with 'turbo' (held on keys bound to the buttons' turbo)
 * pressed and let go: each turbo button's frame n since its turbo key went
 * down is down while n * speed / 30 is even (the game's frames). */
unsigned int nspire_turbo_buttons(unsigned int plain, unsigned int turbo);
/* The old turbo (builds before 2026-11-12, nspire_config.c): a choice of
 * buttons that always repeated, and a Turbo key, held with a button to make
 * it repeat. The choice's buttons (A 1, B 2, L 4, R 8) for its number. */
#define OLD_TURBO_CHOICES 16
unsigned int keys_old_turbo_mask(unsigned int choice);
/* Turns the old turbo into turbo bindings in 'bindings' (whose turbo
 * actions are cleared first): a button in old_mask (A 1, B 2, L 4, R 8)
 * has its keys moved to its turbo (its keys were turbo all the time), and
 * its default controller button (Pad A for A ...) bound to the turbo where
 * it had no controller button of its own and that one is free; every
 * other one of A, B, L, R gets its turbo on the Turbo key held with its key
 * ("= held, del pressed" was turbo A), as far as two slots go. Returns how
 * many bindings it made. */
int keys_turbo_from_old(key_binding bindings[NUM_ACTIONS][KEY_SLOTS], unsigned int old_mask,
                        const key_binding old_turbo_key[KEY_SLOTS]);
/* The bindings of 'button' (ACTION_A ...) and its turbo, swapped over
 * where one side has none: to_turbo moves the plain ones to the turbo. */
void keys_move_turbo(key_binding bindings[NUM_ACTIONS][KEY_SLOTS], int button, int to_turbo);
/* The menus: keys a screen uses for its own job (0-terminated), which the
 * keys bound to A and B (Enter and Esc in menus) leave alone. */
extern const unsigned char *nspire_gui_own_keys;

/* ui18: GBA A/B in menus and waits (calc binding not in taken/own_keys, or default pad button). */
int nspire_gui_button_pressed(int action);
int nspire_gui_button_down(int action);
int nspire_gui_button_down_now(int action);

/* How many times the held direction has repeated in menus (get_gui_input). */
unsigned gui_repeat_count(void);

#endif
