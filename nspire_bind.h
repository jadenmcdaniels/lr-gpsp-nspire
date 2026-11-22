/* What the Controls screen can bind, apart from how it is drawn
 * (nspire_controls.c draws it with ui_controls_draw). A calculator key or a
 * controller button is bound to nothing or to one action: a GBA button
 * pressed (A, B, L, R, Start, Select, the D-pad), the same button as turbo
 * (ACTION_TURBO_*: pressed and let go over and over while held), or a
 * hotkey. Each row is one GBA button with its turbo, or one hotkey; each
 * action has two places (KEY_SLOTS), one view (the calculator's keys, or a
 * controller's buttons) shows the ones of its kind. The bindings are
 * key_bindings (nspire_keys.h). */
#ifndef NSPIRE_BIND_H
#define NSPIRE_BIND_H

#include <stddef.h>
#include "nspire_keys.h"

struct bind_row
{
  unsigned char action;       /* ACTION_A, ACTION_MENU ... */
  unsigned char has_turbo;    /* a GBA button: TURBO_OF(action) is its turbo */
  const char *name;           /* "A", "Fast forward" (the marks and icons are the screen's) */
  unsigned char pad_default;  /* the controller button that does it while bound to nothing */
};

extern const struct bind_row bind_rows[];
extern const int bind_row_count;

/* The row of an action (a turbo's is its button's), or -1. */
int bind_row_of(int action);
/* "A", "Turbo A", "Fast forward". */
const char *bind_action_name(int action);
/* Whether b is for the view: all its keys are a controller's (pad), or
 * all the calculator's. */
int bind_in_view(key_binding b, int pad);
/* What a view shows for action, into out (KEY_SLOTS of them): its bindings
 * of the view's kind; in a controller's view a plain button bound to no
 * controller button shows the one that does it by default (nspire_input.c
 * pad_jobs). Returns how many. */
int bind_shown(int action, int pad, key_binding out[KEY_SLOTS]);
/* The action other than 'except' that the view shows b on, or -1. */
int bind_owner(key_binding b, int pad, int except);
/* Puts b on action, in the view's kind of key: it takes the place of the
 * one of that kind (a button has one controller button; a calculator key
 * replaces the first of its kind), else an empty place, else the second. */
void bind_set(int action, key_binding b, int pad);
/* Takes b off every other action (a key does one thing). */
void bind_take(int action, key_binding b);
/* Takes the view's kind of key off action. */
void bind_clear(int action, int pad);
/* A binding in words: the calculator's key names ("shift+S"), or the
 * controller's ("Bottom", "L1"; with bind_pad_family set, the pad's own
 * names: "Circle", "LB", "ZR", "Options"). */
void bind_text(key_binding b, int pad, char *text, size_t size);

/* R10b (2026-11-15): the controller's family (nspire_usbpad_glue.h
 * PAD_FAMILY_*), whose own button names bind_text gives: 0 (none, or a
 * generic USB gamepad) the positions. The Controls screen sets it from the
 * pad that is in. Which button does what doesn't change with it: the
 * defaults are by position on every pad (the right face button is A, the
 * bottom one B, L1 L, R1 R). */
extern int bind_pad_family;
/* 1 while the pad that is in has layout=labels (its bottom and right, left
 * and top face buttons swapped): the names are the buttons' that send them. */
extern int bind_pad_labels;
/* A controller button's own name in a family ("Cross", "B", "ZL"), never NULL. */
const char *bind_pad_name(unsigned key, int family);
/* A face button's side ("right", "bottom", "top", "left"), else NULL. */
const char *bind_pad_side(unsigned key);

#endif
