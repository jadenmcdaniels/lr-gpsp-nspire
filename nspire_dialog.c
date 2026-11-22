/* Questions and messages over a screen (ui_dialog_draw, nspire_ui_screens.c):
 * the screen behind is drawn once (not shown by itself), the dialog over it,
 * and nothing is redrawn until a key does something. */

#include "common.h"
#include "input.h"
#include "nspire.h"
#include "nspire_frontend.h"
#include "nspire_keys.h"
#include "nspire_link.h"
#include "nspire_ui_screens.h"

#include <string.h>

#include "nspire_dialog.h"

/* The first time the screen behind is dimmed; the next ones draw the dialog
 * alone over it (a button picked), or redraw the ground first (ground). */
static void draw_over(struct ui_dialog_view *d, void (*ground)(void *), void *arg)
{
  if (ground)
  {
    ui_defer_present = 1;
    ground(arg);
    ui_defer_present = 0;
    d->no_dim = 0;
  }
  ui_dialog_draw(d);
  d->no_dim = 1;
}

/* ui18: a question has no keys of its own: the keys bound to the GBA's A and B (a calculator key the screen
 * under it keeps for itself, del in the Saves place, too) and a controller's are enter and esc on it (before,
 * only the enter key worked on the question that deletes for good); and an answer is taken only once every
 * key that was down when it came (the del or the enter that asked it, held or repeating) has been let go. */
int nspire_ask(struct ui_dialog_view *d, void (*ground)(void *), void *arg)
{
  int redraw = 1, answer, armed = 0;
  const unsigned char *own = nspire_gui_own_keys;

  nspire_gui_own_keys = NULL;
  d->no_dim = 0;
  for (;;)
  {
    gui_action_type a;
    if (redraw)
      draw_over(d, ground, arg);
    redraw = 0;
    update_at_vblank();
    a = get_gui_input();
    if (!any_key_down())
      armed = 1;
    if (a == CURSOR_NONE || (a == CURSOR_SELECT && !armed))
      continue;
    redraw = 1;
    if (d->second && (a == CURSOR_LEFT || a == CURSOR_RIGHT || a == CURSOR_UP || a == CURSOR_DOWN))
      d->picked ^= 1;
    else if (a == CURSOR_SELECT)
    {
      d->no_dim = 0;
      nspire_link_log("ui: question \"%s\": %s", d->title, d->picked ? d->second : d->first);
      answer = d->picked;
      break;
    }
    else if (a == CURSOR_BACK || a == CURSOR_EXIT)
    {
      d->no_dim = 0;
      nspire_link_log("ui: question \"%s\": esc", d->title);
      answer = -1;
      break;
    }
  }
  nspire_gui_own_keys = own;
  return answer;
}
