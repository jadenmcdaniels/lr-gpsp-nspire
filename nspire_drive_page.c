/* The USB folder is virtual: the calculator's current directory is never
 * changed, and selected names keep their usb:/ prefix through loading. */
#include "common.h"
#include "input.h"
#include "nspire.h"
#include "nspire_frontend.h"
#include "nspire_keys.h"
#include "nspire_drive.h"
#include "nspire_places.h"
#include "nspire_ui_screens.h"
#include <stdlib.h>
#include <string.h>

static void free_names(char **names, unsigned count)
{
  for (unsigned i = 0; i < count; i++) free(names[i]);
  free(names);
}

/* The folder the page was last left in, while the same drive stays opened
 * (its epoch): a game picked three folders down is three enters away once. */
static char last_where[512];
static unsigned last_epoch = ~0u;

static void draw(char **games, unsigned ng, char **dirs, unsigned nd, const char *where,
                 unsigned pick, unsigned top, const char *note)
{
  static const char *const browse[] = { "enter", "Open / play", "esc", "Back", NULL };
  static const char *const again[] = { "enter", "Try again", "esc", "Back", NULL };
  const char *const *actions = nspire_drive_state() == DRIVE_ERROR || nspire_drive_state() == DRIVE_NOMEM ? again : browse;
  struct ui_manager_view v;
  memset(&v, 0, sizeof(v));
  v.title = DRIVE_LABEL;
  v.name = "usb";
  snprintf(v.subtitle, sizeof(v.subtitle), "%s", where);
  v.count = ng + nd;
  v.first = top;
  v.picked = pick;
  v.empty = nspire_drive_state() == DRIVE_READY ? "No GBA games or folders here" : nspire_drive_summary();
  for (unsigned i = 0; i < UI_MANAGER_ROWS && top + i < ng + nd; i++)
  {
    unsigned n = top + i;
    snprintf(v.rows[i].label, sizeof(v.rows[i].label), "%s%s",
             n < nd ? dirs[n] : games[n - nd], n < nd ? "/" : "");
  }
  v.pic_empty = "";
  if (pick < nd + ng)
    snprintf(v.head, sizeof(v.head), "%s", pick < nd ? dirs[pick] : games[pick - nd]);
  /* (Settings, Saving, Saves on: nspire_places.h) */
  snprintf(v.facts[0], sizeof(v.facts[0]), nspire_saves_on == PLACE_DRIVE ? "Saves on USB drive" : "Saves on calculator");
  snprintf(v.facts[1], sizeof(v.facts[1]), "Game kept in memory");
  v.actions = actions;
  v.status = note;
  v.percent = -1;
  ui_manager_draw(&v);
}

int nspire_drive_browser(char *result, unsigned size)
{
  char where[512] = "/", note[128] = "";
  char **games = NULL, **dirs = NULL;
  unsigned ng = 0, nd = 0, pick = 0, top = 0, seen = ~0u;
  int refresh = 1, chosen = 0;
  nspire_drive_start();
  nspire_drive_poll();
  if (nspire_drive_state() == DRIVE_READY && last_epoch == nspire_drive_epoch() && last_where[0])
  {
    snprintf(where, sizeof(where), "%s", last_where);
    seen = last_epoch;
  }
  keys_wait_release();
  for (;;)
  {
    /* Draw before mounting: reads can take several seconds. */
    draw(games, ng, dirs, nd, where, pick, top, note);
    nspire_drive_menu_tick();
    if (seen != nspire_drive_epoch())
    {
      seen = nspire_drive_epoch();
      strcpy(where, "/");
      refresh = 1;
      pick = top = 0;
      note[0] = 0;
    }
    if (refresh)
    {
      free_names(games, ng); free_names(dirs, nd);
      games = dirs = NULL; ng = nd = 0;
      if (nspire_drive_state() == DRIVE_READY &&
          nspire_drive_list(where, &games, &ng, &dirs, &nd))
      {
        /* (a remembered folder that is gone: the drive's top, once) */
        if (strcmp(where, "/"))
        {
          strcpy(where, "/");
          continue;
        }
        snprintf(note, sizeof(note), "Couldn't read this folder. Replug the drive and try again.");
      }
      refresh = 0;
    }
    update_at_vblank();
    gui_action_type action = get_gui_input();
    if (action == CURSOR_EXIT) break;
    if (action == CURSOR_BACK || action == CURSOR_LEFT)
    {
      char *slash = strrchr(where, '/');
      if (!slash || slash == where)
      {
        if (!strcmp(where, "/")) break;
        strcpy(where, "/");
      }
      else *slash = 0;
      pick = top = 0; refresh = 1; note[0] = 0;
    }
    /* a drive that is in and could not be opened (or not for lack of memory: the menus' caches
     * given up first): another try on enter */
    else if (action == CURSOR_SELECT && (nspire_drive_state() == DRIVE_ERROR || nspire_drive_state() == DRIVE_NOMEM))
    {
      nspire_drive_retry();
      note[0] = 0;
    }
    else if (action == CURSOR_UP && pick) pick--;
    else if (action == CURSOR_DOWN && pick + 1 < ng + nd) pick++;
    else if (action == CURSOR_SELECT && pick < ng + nd)
    {
      const char *name = pick < nd ? dirs[pick] : games[pick - nd];
      unsigned n = strlen(where), len = strlen(name);
      int slash = n && where[n - 1] != '/';
      if (n + slash + len + 1 > sizeof(where) || (pick >= nd && 4 + n + slash + len + 1 > size))
        snprintf(note, sizeof(note), "This path is too long. Move the game to a shorter folder.");
      else if (pick < nd)
      {
        if (slash) where[n++] = '/';
        strcpy(where + n, name);
        pick = top = 0; refresh = 1; note[0] = 0;
      }
      else
      {
        snprintf(result, size, "usb:%s%s%s", where, slash ? "/" : "", name);
        nspire_skip_state_load = key_down(KEY_SHIFT);
        chosen = 1;
        break;
      }
    }
    if (pick < top) top = pick;
    if (pick >= top + UI_MANAGER_ROWS) top = pick - UI_MANAGER_ROWS + 1;
  }
  free_names(games, ng); free_names(dirs, nd);
  if (nspire_drive_state() == DRIVE_READY)
  {
    snprintf(last_where, sizeof(last_where), "%s", where);
    last_epoch = nspire_drive_epoch();
  }
  keys_wait_release();
  return chosen;
}
