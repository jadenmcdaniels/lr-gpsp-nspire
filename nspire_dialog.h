/* Questions and messages over a screen (nspire_dialog.c). */
#ifndef NSPIRE_DIALOG_H
#define NSPIRE_DIALOG_H

#include "nspire_ui_screens.h"

/* Shows d over the screen that ground(arg) draws (NULL: over what is on the
 * screen already, which the dialog dims once), waits for enter or esc (menu): the picked button (0 the first, 1 the
 * second), -1 for esc (or the menu key): nothing was chosen. A message with
 * one button returns 0 or -1. */
int nspire_ask(struct ui_dialog_view *d, void (*ground)(void *), void *arg);

#endif
