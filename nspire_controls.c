/* The Controls screen: each GBA button and gpSP key on a drawing of the
 * keypad (or of the controller), the one picked lit; enter, then the new
 * key; on a GBA button tab, then the key for its turbo; del takes the
 * row's keys off; a key another button has is asked about; Reset to
 * defaults asks first; esc leaves, changing nothing it hasn't been told
 * to. What can be bound is nspire_bind.h's (the model); this file is the
 * screen's loop, and ui_controls_draw (nspire_ui_screens.c) draws it. The
 * keys are key_bindings (nspire_keys.h), saved with the settings when the
 * menu closes. */

#include "common.h"
#include "input.h"
#include "nspire.h"
#include "nspire_frontend.h"
#include "nspire_dialog.h"
#include "nspire_keys.h"
#include "nspire_bind.h"
#include "nspire_ui_screens.h"
#include "nspire_usbpad_glue.h"

#include <stdio.h>
#include <string.h>

/* How the model's rows (nspire_bind.c bind_rows, in that order) look: the
 * mark on a key and the icon. */
static const struct
{
  const char *mark, *mark_short;   /* r19: words only (until r18 an icon for gpSP's own keys) */
} looks[] = {
  { UI_ARROW_U, NULL }, { UI_ARROW_D, NULL }, { UI_ARROW_L, NULL }, { UI_ARROW_R, NULL },
  { "A", NULL }, { "B", NULL }, { "L", NULL }, { "R", NULL },
  { "START", "ST" }, { "SEL", "SE" },
  { "MENU", "MN" }, { "FAST", "FF" }, { "SAVE", "SV" },
  { "LOAD", "LD" }, { "LAST", "LN" }, { "QUIT", "QT" },
  { "SL+", "S+" }, { "SL-", "S-" }, { "FPS", "FP" },
  { "RST", "RS" }, { "LT+", "B+" }, { "LT-", "B-" },
};
#define ROWS (int)(sizeof(looks) / sizeof(looks[0]))

static struct ui_control_row view_rows[ROWS];
/* Each row's texts: its two keys, then its turbo's. */
static char key_texts[ROWS][3][40];
/* R10b: a controller button in words for the help line. */
static char key_words[ROWS][48];

static const char *words_for(int i, key_binding b, int action)
{
  const char *side = bind_pad_side(b.key);

  if (b.with)
    return NULL;
  if (action == ACTION_MENU && b.key == KEY_PAD_HOME)
    snprintf(key_words[i], sizeof(key_words[i]), "%s, or %s and %s", bind_pad_name(KEY_PAD_HOME, bind_pad_family),
             bind_pad_name(KEY_PAD_SELECT, bind_pad_family), bind_pad_name(KEY_PAD_START, bind_pad_family));
  else if (side && bind_pad_family)
    snprintf(key_words[i], sizeof(key_words[i]), "the %s button (%s)", bind_pad_name(b.key, bind_pad_family), side);
  else if (side)
    snprintf(key_words[i], sizeof(key_words[i]), "the %s button", side);
  else
    return NULL;
  return key_words[i];
}

/* The view's rows from the model (nspire_bind.h): the view's kind of keys
 * of each button and of its turbo. */
static void fill_rows(int pad)
{
  int i;
  for (i = 0; i < ROWS && i < bind_row_count; i++)
  {
    struct ui_control_row *r = &view_rows[i];
    key_binding shown[KEY_SLOTS];
    int n, s, action = bind_rows[i].action;
    memset(r, 0, sizeof(*r));
    r->name = bind_rows[i].name;
    r->mark = looks[i].mark;
    r->mark_short = looks[i].mark_short;
    r->icon = -1;
    key_texts[i][0][0] = key_texts[i][1][0] = key_texts[i][2][0] = 0;
    n = bind_shown(action, pad, shown);
    r->key = shown[0].key;
    r->with = shown[0].with;
    r->key2 = shown[1].key;
    for (s = 0; s < n; s++)
      bind_text(shown[s], pad, key_texts[i][s], sizeof(key_texts[i][s]));
    if (!n)
      /* a controller's D-pad and left stick always move */
      snprintf(key_texts[i][0], sizeof(key_texts[i][0]), pad && action < 4 ? "D-pad" : "none");
    r->key_text = key_texts[i][0];
    r->key_text2 = key_texts[i][1][0] ? key_texts[i][1] : NULL;
    if (pad && n == 1)
      r->key_words = words_for(i, shown[0], action);
    if (bind_rows[i].has_turbo)
    {
      n = bind_shown(TURBO_OF(action), pad, shown);
      r->tkey = shown[0].key;
      r->twith = shown[0].with;
      if (n)
        bind_text(shown[0], pad, key_texts[i][2], sizeof(key_texts[i][2]));
      else
        snprintf(key_texts[i][2], sizeof(key_texts[i][2]), "none");
      r->turbo_text = key_texts[i][2];
    }
  }
}

static gui_action_type input(void)
{
  gui_action_type a = get_gui_input();
  if (a == CURSOR_BACK)
    a = CURSOR_EXIT;
  return a;
}

static void ground(void *view)
{
  ui_controls_draw((struct ui_controls_view *)view);
}

/* "Reset all keys?": 1 for Reset (No is focused first). */
static int ask_reset(struct ui_controls_view *v)
{
  struct ui_dialog_view d;
  memset(&d, 0, sizeof(d));
  d.title = "Reset all keys?";
  d.text = "Every button gets the key gpSP came with.";
  d.first = "No, keep mine";
  d.second = "Reset";
  d.destructive = 1;
  return nspire_ask(&d, ground, v) == 1;
}

/* Waits for the key for 'action' and puts it there, asking first if
 * another action has it. esc or an arrow change nothing. */
static void capture_for(struct ui_controls_view *v, int action, char *note, size_t note_size)
{
  key_binding b;
  int other;

  note[0] = 0;
  v->target_name = bind_action_name(action);
  v->turbo = IS_TURBO(action);
  for (;;)
  {
    v->capturing = 1;
    ui_controls_draw(v);
    v->capturing = 0;
    b = key_capture();
    if (!b.key || (b.key == KEY_ESC && !b.with))
      break;   /* esc or an arrow: nothing changes */
    if (bind_in_view(b, v->pad))
      break;
    /* the other kind of key: say so, and go on waiting */
    snprintf(note, note_size, v->pad ? "Use a button on the controller." : "Use a key on the calculator.");
  }
  note[0] = 0;
  if (b.key && bind_in_view(b, v->pad))
  {
    other = bind_owner(b, v->pad, action);
    if (other < 0)
      bind_set(action, b, v->pad);
    else
    {
      char text[24];
      const unsigned char *own = nspire_gui_own_keys;
      nspire_gui_own_keys = NULL;
      bind_text(b, v->pad, text, sizeof(text));
      v->conflict_row = bind_row_of(other);
      v->conflict_name = bind_action_name(other);
      v->new_key = b.key;
      v->new_key_text = text;
      v->conflict_choice = 0;
      for (;;)
      {
        gui_action_type c;
        ui_controls_draw(v);
        do
        {
          update_at_vblank();
          c = input();
        } while (c == CURSOR_NONE);
        if (c == CURSOR_UP || c == CURSOR_DOWN || c == CURSOR_LEFT || c == CURSOR_RIGHT)
          v->conflict_choice ^= 1;
        else if (c == CURSOR_EXIT || (c == CURSOR_SELECT && !v->conflict_choice))
          break;
        else if (c == CURSOR_SELECT)
        {
          bind_take(action, b);
          bind_set(action, b, v->pad);
          break;
        }
      }
      nspire_gui_own_keys = own;
      v->conflict_row = -1;
      v->conflict_name = NULL;
      v->new_key_text = NULL;
    }
  }
  v->target_name = NULL;
  v->turbo = 0;
}

/* The keys this screen keeps for itself (nspire_gui_own_keys): tab sets a
 * button's turbo, del takes a row's keys off. */
static const unsigned char own_keys[] = { KEY_TAB, KEY_DEL, 0 };

void nspire_controls_screen(void)
{
  struct ui_controls_view v;
  const int visible = UI_LINES;
  char note[64] = "";
  int redraw = 1;
  const unsigned char *menu_keys = nspire_gui_own_keys;

  memset(&v, 0, sizeof(v));
  v.rows = view_rows;
  v.count = ROWS;
  v.conflict_row = -1;
  v.note = note;
  nspire_gui_own_keys = own_keys;
  for (;;)
  {
    gui_action_type a;
    int was_connected = v.pad_connected, row_turbo;
    v.pad_connected = nspire_usbpad_name() != NULL;
    if (!v.pad_connected)
      v.pad = 0;
    v.pad_name = v.pad ? nspire_usbpad_name() : NULL;
    /* R10b: the pad's own button names (by position for a generic one) */
    bind_pad_family = v.pad ? nspire_usbpad_family() : PAD_FAMILY_NONE;
    bind_pad_labels = v.pad && nspire_usbpad_labels();
    fill_rows(v.pad);
    if (v.picked < v.first)
      v.first = v.picked;
    if (v.picked >= v.first + visible)
      v.first = v.picked - visible + 1;
    /* drawn when a key did something (or a controller came or went), not every pass */
    if (redraw || was_connected != v.pad_connected)
      ui_controls_draw(&v);
    redraw = 0;
    update_at_vblank();
    nspire_gui_own_keys = v.picked < ROWS ? own_keys : NULL;
    a = input();
    row_turbo = v.picked < ROWS && bind_rows[v.picked].has_turbo;
    if (a == CURSOR_NONE && key_pressed(KEY_TAB) && row_turbo)
    {
      /* tab, then the key: the picked button's turbo */
      capture_for(&v, TURBO_OF(bind_rows[v.picked].action), note, sizeof(note));
      redraw = 1;
      continue;
    }
    if (a != CURSOR_NONE)
      redraw = 1;
    switch (a)
    {
    case CURSOR_UP:
      v.picked = (v.picked + ROWS) % (ROWS + 1);
      break;
    case CURSOR_DOWN:
      v.picked = (v.picked + 1) % (ROWS + 1);
      break;
    case CURSOR_LEFT:
    case CURSOR_RIGHT:
      if (v.pad_connected)
        v.pad ^= 1;
      break;
    case CURSOR_EXIT:
      nspire_gui_own_keys = menu_keys;
      bind_pad_family = PAD_FAMILY_NONE;
      bind_pad_labels = 0;
      return;
    case CURSOR_CLEAR:
      /* del: the picked button has no key of this view's kind, nor its turbo */
      if (v.picked < ROWS)
      {
        bind_clear(bind_rows[v.picked].action, v.pad);
        if (row_turbo)
          bind_clear(TURBO_OF(bind_rows[v.picked].action), v.pad);
      }
      break;
    case CURSOR_SELECT:
      if (v.picked == ROWS)
      {
        if (ask_reset(&v))
          keys_set_defaults(key_bindings);
        break;
      }
      capture_for(&v, bind_rows[v.picked].action, note, sizeof(note));
      break;
    default:
      break;
    }
  }
}

/* The first-run hint: the basic keys on the drawing, before the game. */
void nspire_controls_hint(void)
{
  static struct ui_control_row hint[ROWS + 1];
  static char moves[32];
  struct ui_controls_view v;
  int i, n = 0;

  fill_rows(0);
  memset(hint, 0, sizeof(hint));
  snprintf(moves, sizeof(moves), "%s %s %s %s", view_rows[0].key_text, view_rows[2].key_text,
           view_rows[1].key_text, view_rows[3].key_text);
  hint[n].name = "Move";
  hint[n].key_text = moves;
  hint[n++].basic = 1;
  for (i = 0; i < ROWS; i++)
    if (i < 4 || bind_rows[i].action == ACTION_A || bind_rows[i].action == ACTION_B ||
        bind_rows[i].action == ACTION_START || bind_rows[i].action == ACTION_SELECT ||
        bind_rows[i].action == ACTION_MENU)
    {
      hint[n] = view_rows[i];
      hint[n].tkey = hint[n].twith = 0;   /* the basic keys only: no turbo */
      hint[n].turbo_text = NULL;
      hint[n++].basic = i < 4 ? 2 : 1;
    }
  memset(&v, 0, sizeof(v));
  v.rows = hint;
  v.count = n;
  v.conflict_row = -1;
  v.hint = 1;
  const unsigned char *own = nspire_gui_own_keys;
  nspire_gui_own_keys = NULL;
  keys_wait_release();   /* the key that started the game doesn't end the screen */
  ui_controls_draw(&v);
  for (;;)
  {
    gui_action_type a;
    update_at_vblank();
    nspire_gui_own_keys = v.picked < ROWS ? own_keys : NULL;
    a = input();
    if (a == CURSOR_SELECT || a == CURSOR_EXIT || a == CURSOR_BACK)
      break;
  }
  nspire_gui_own_keys = own;
}
