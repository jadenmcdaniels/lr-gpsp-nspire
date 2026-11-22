/* The menus' screens: see nspire_ui_screens.h. Places are in pixels of the
 * 320x240 frame; text y is its baseline. The design ("plain",
 * docs/ui/spec.txt) in one paragraph: black on white, one monospaced face
 * at one size; the games a grid of pictures with their names under them;
 * the paused menu a picture in the middle with the choices in one row
 * along the bottom; a settings page a spec sheet of two columns. The
 * picked thing is a black block with its type in white. Every place is the
 * grid's (nspire_ui.h: the header, the body from UI_TOP in lines of
 * UI_LINE_H, the band); a text too long for its place is cut or, picked,
 * scrolls (ui_line_text), the same everywhere. */

#include "nspire_ui_screens.h"
#include "nspire_keys.h"
#include "nspire_states.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define X0 UI_MARGIN            /* the left edge */
#define X1 (UI_W - UI_MARGIN)   /* text ends */
#define TOP UI_TOP              /* the body's first line */
#define RH UI_LINE_H
#define BASE UI_LINE_BASE
#define LINE_Y(n) (TOP + (n) * RH)          /* the body's line n: its top */
#define LINE_BASE(n) (LINE_Y(n) + BASE)     /* its baseline */

/* ---- Shared parts ---- */

/* A picture's place when there's none (yet): white (r16: no outline; the grid's alignment says where it is). */
static void empty_picture(int x, int y, int w, int h)
{
  (void)x;
  (void)y;
  (void)w;
  (void)h;
}

/* s in lines of at most w, at most max_lines, the first baseline y, lh
 * apart; a text too long has its last line cut ("..."): a name is as long
 * as it is, that isn't a fault. Returns the last line's baseline. */
static int words(int x, int y, int lh, int w, int max_lines, const char *s, const struct ui_font *f, u16 color)
{
  char rest[200], fit[200];
  int lines = 0;

  snprintf(rest, sizeof(rest), "%s", s);
  while (lines < max_lines - 1 && ui_text_width(rest, f) > w)
  {
    /* the most words that fit one line */
    char *cut = NULL, *sp = rest;
    while ((sp = strchr(sp + 1, ' ')) != NULL)
    {
      int n;
      *sp = 0;
      n = ui_text_width(rest, f);
      *sp = ' ';
      if (n > w)
        break;
      cut = sp;
    }
    if (!cut)
      break;
    *cut = 0;
    ui_text(x, y + lines++ * lh, rest, f, color);
    memmove(rest, cut + 1, strlen(cut + 1) + 1);
  }
  ui_text(x, y + lines * lh, ui_fit(rest, f, w, fit, sizeof(fit)), f, color);
  return y + lines * lh;
}

/* A label centred on cx. */
static void label_center(int cx, int y, const char *s, u16 color)
{
  ui_label(cx - ui_label_width(s) / 2, y, s, color);
}

/* A line of a list in the plain cut: the picked one's block from x to
 * x + w, its text cut out of it. label may be NULL (the caller draws it).
 * Returns the colour text on it takes; *ink2 the colour of a value. */
static u16 line(int x, int w, int y, const char *label, int label_w, int focused, u16 *ink2)
{
  char fit[160];
  u16 fg = UI_TEXT;
  *ink2 = UI_TEXT;
  if (focused)
  {
    fg = ui_focus(x, y, w, RH);
    *ink2 = UI_FOCUS_INK2;
  }
  if (label)
    ui_text(x + X0, y + BASE, ui_fit(label, &ui_font_small, label_w, fit, sizeof(fit)), &ui_font_small, fg);
  return fg;
}

/* ---- The items (r19): a grid of pictures, each with one name under it ----
 * Three by two tiles: a picture (96 x 64, a state's at 2/3) and its name on one line under it, centred, in
 * capitals; nothing else on a tile (until r19 a second line of facts and a plate two lines high). The picked one
 * is in its own colours and its name is white on a black block as wide as the tile's plate; the others are black
 * and white prints. A name too long is cut ("..."); picked, it runs round (ui_line_text). What the picked item is
 * (how many saves, when; a save's age and size) is the band's left; the keys that aren't obvious its right.
 * The tiles fill the width: x 8, 112, 216. The games' grid has the folders after the games: a folder is its name
 * with a slash, no picture. v shows the items a line each. */

#define G_PW UI_TILE_W    /* a picture */
#define G_PH UI_TILE_H
#define G_STEP (G_PW + 8)   /* a column: x 8, 112, 216 */
#define G_X(c) (8 + (c) * G_STEP)
#define G_Y0 30           /* the first row's picture; the second's G_RSTEP lower */
#define G_RSTEP 96
#define G_BLOCK (G_PH + 1)    /* the name's block under the picture: rows G_BLOCK to G_BLOCK + RH - 1 from its top */
#define G_NAME (G_BLOCK + BASE)   /* the name's baseline */
#define G_PLATE 4         /* the picked one's block reaches this far either side of its picture */

/* A game without a picture: its initial alone in the picture's place (r16: no frame). */
static void initial_tile(int x, int y, int w, int h, const char *title)
{
  char s[2] = { title[0], 0 };
  if (!strncmp(title, "The ", 4))
    s[0] = title[4];
  ui_text_center(x + w / 2, y + h / 2 + 4, s, &ui_font_small_strong, UI_TEXT);
}

/* A tile's name under its picture at x, y: centred; picked: white on the block. Returns 1 while it runs (picked,
 * too long). */
static int tile_name(int x, int y, const char *name, int picked, unsigned ms)
{
  const struct ui_font *f = &ui_font_small_strong;
  u16 ink = picked ? UI_FOCUS_INK : UI_TEXT;
  int w = ui_text_width(name, f);
  if (picked)
    ui_rect(x - G_PLATE, y + G_BLOCK, G_PW + 2 * G_PLATE, RH, UI_ACCENT);
  if (w > G_PW && !picked)
  {
    /* too long, not picked: cut ("..."), and what is left centred as the others are */
    char fit[200];
    name = ui_fit(name, f, G_PW, fit, sizeof(fit));
    ui_text(x + (G_PW - ui_text_width(name, f)) / 2, y + G_NAME, name, f, ink);
    return 0;
  }
  if (w <= G_PW)
  {
    ui_text(x + (G_PW - w) / 2, y + G_NAME, name, f, ink);
    return 0;
  }
  return ui_line_text(x, y + G_NAME, G_PW, name, f, ink, picked, ms);
}

/* A folder's name as an item's: "HOMEBREW/"; the way up: "BACK". */
static void folder_name(const char *dir, char *out, size_t size)
{
  if (!strcmp(dir, ".."))
    snprintf(out, size, "Back");
  else if (!strcmp(dir, "USB drive"))
    snprintf(out, size, "%s", dir);   /* (r19: "USB drive", not "USB drive/": DRIVE_LABEL, not a folder's name) */
  else
    snprintf(out, size, "%s/", dir);
}

static void grid_cell(const struct ui_list_view *v, int r)
{
  const struct ui_list_game *g = &v->rows[r];
  const struct ui_list_details *d = v->details;
  int focused = r == v->picked_row;             /* it has the block */
  int here = focused || r == v->marked_row;     /* the item the grid is on (in colour; with the arrows on the top
                                                   row nothing has the block) */
  int x = G_X(r % 3), y = G_Y0 + (r / 3) * G_RSTEP;
  const u16 *pic = g->picture;
  char title[160], tags[96];

  if (g->folder)
  {
    folder_name(g->file, title, sizeof(title));
    ui_list_scrolls |= tile_name(x, y, title, focused, v->picked_ms);
    return;
  }
  ui_game_title(g->file, title, sizeof(title), tags, sizeof(tags));
  if (here && d && d->ready)
    pic = d->has_picture ? d->picture : NULL;
  if (pic)
    ui_picture_fit(x, y, G_PW, G_PH, pic, 144, 96, 144, !here);
  else if (g->looked_up || (here && d && d->ready))
    initial_tile(x, y, G_PW, G_PH, title);
  if (focused && v->moving)
  {
    /* being moved: the arrows on a white chip in its middle, so they read over any picture */
    ui_rect(x + G_PW / 2 - 18, y + G_PH / 2 - 9, 36, 16, UI_BG);
    ui_text_center(x + G_PW / 2, y + G_PH / 2 + 4, UI_ARROW_L " " UI_ARROW_R, &ui_font_small_strong, UI_TEXT);
  }
  ui_list_scrolls |= tile_name(x, y, title, focused && !v->moving, v->picked_ms);
}

/* The items a line each (v): the name at the left, a game's facts at the right, the picked line a black block. */
static void list_lines(const struct ui_list_view *v)
{
  int r;
  for (r = 0; r < UI_LIST_LINES && v->rows[r].file; r++)
  {
    char title[160], tags[96], facts[40];
    const struct ui_list_game *g = &v->rows[r];
    if (g->folder)
    {
      folder_name(g->file, title, sizeof(title));
      facts[0] = 0;
    }
    else
    {
      ui_game_title(g->file, title, sizeof(title), tags, sizeof(tags));
      snprintf(facts, sizeof(facts), "%s%s%s", g->facts, g->facts[0] && g->age[0] ? " " UI_DOT " " : "", g->age);
    }
    ui_list_scrolls |= ui_list_line(LINE_Y(r), title, facts, r == v->picked_row && !v->moving, 0, v->picked_ms);
    if (r == v->picked_row && v->moving)
      ui_text_right(X1, LINE_BASE(r), UI_ARROW_U " " UI_ARROW_D, &ui_font_small_strong, UI_TEXT);
  }
  ui_scrollbar(UI_W - 2, TOP, TOP + UI_LIST_LINES * RH, v->first, UI_LIST_LINES, v->items);
}

/* r19: the games (and, after them, the folders) under the top row (ui_nav: the same row as the menu's, GAMES one
 * of its words). With the arrows on the row nothing under it has the block; the item the grid is on stays in
 * colour. The band: what just happened; else what the picked item is at the left (a game's saves and when, a
 * folder's place) and the keys that aren't obvious at the right. */
void ui_list_draw(const struct ui_list_view *v)
{
  const char *keys[12];
  char left[96] = "";
  int r, n = 0;
  const struct ui_list_game *p = v->picked_row >= 0 ? &v->rows[v->picked_row] : NULL;

  ui_begin();
  ui_fill(UI_BG);
  ui_list_scrolls = 0;
  ui_nav(v->tabs, v->tab_count, v->tab_on, v->nav_focus);

  if (v->lines)
    list_lines(v);
  else
  {
    for (r = 0; r < UI_LIST_ROWS && v->rows[r].file; r++)
      grid_cell(v, r);
    /* more than a screen: where these are among them (a row of three a step), at the screen's right edge */
    ui_scrollbar(UI_W - 2, G_Y0, G_Y0 + G_RSTEP + G_BLOCK + RH, v->first / 3, UI_LIST_ROWS / 3, (v->items + 2) / 3);
  }
  if (!v->games && v->items <= 3)
  {
    /* nothing to play here: where games go, under the folders' one row */
    ui_text_center(UI_W / 2, LINE_BASE(6), "No games here", &ui_font_small_strong, UI_TEXT);
    ui_text_center(UI_W / 2, LINE_BASE(7), "Copy game.gba.tns into this folder", &ui_font_small, UI_TEXT);
  }

  if (p && p->folder)
  {
    /* a folder: where it leads */
    const char *path = v->path ? v->path : "";
    while (strlen(path) > 30)
      path++;
    if (!strcmp(p->file, ".."))
      snprintf(left, sizeof(left), "Up from %s%s", path != v->path && v->path ? UI_ELLIPSIS : "", path);
    else
      snprintf(left, sizeof(left), "Folder");
  }
  else if (p)
    snprintf(left, sizeof(left), "%s%s%s", p->facts, p->facts[0] && p->age[0] ? " " UI_DOT " " : "", p->age);
  if (v->moving)
  {
    keys[n++] = "!enter";
    keys[n++] = "Put it here";
  }
  else if (p && !p->folder)
  {
    /* r19: three keys in the band beside every game were too many; now only the one a player needs
     * (its saves). V (a line a game) and tab (moves it) work as before and are in the README. */
    keys[n++] = "S";
    keys[n++] = "Saves";
  }
  if (v->status && v->status[0])
    ui_footer_status(v->status, UI_TEXT);
  else
    ui_band(left, keys, n / 2);

  if (v->nav_focus)
    snprintf(ui_shown, sizeof(ui_shown), "main:%d", v->tab_on);
  else
    snprintf(ui_shown, sizeof(ui_shown), "list:%d", v->picked_row >= 0 ? v->first + v->picked_row : -1);
  ui_present();
}

/* ---- Saves (r19): a game's states and its own save, the same grid ----
 * The paused game's are the top row's SAVES (v->tabs): first RESUME (the game as it stands) and SAVE (a "+": enter
 * saves), then its own save and each state, the newest first. Another game's (from the games' grid, S) is a page
 * of its own, its path at the top. A marked tile says so in its name. The band: what just happened; else what the
 * picked tile is and the keys. */
void ui_saves_draw(const struct ui_saves_view *v)
{
  int r;
  const struct ui_save_tile *picked = NULL;

  ui_begin();
  ui_fill(UI_BG);
  ui_menu_scrolls = 0;

  if (v->tabs)
    ui_nav(v->tabs, v->tab_count, v->tab_on, v->nav_focus);
  else
  {
    /* a page of its own: where this is, as a path: the game, SAVES and, with a drive in, which place's (tab
     * switches them: the band says so). (Until r19 the places were tabs at the header's right, where the path
     * ran into them.) */
    char path[200];
    snprintf(path, sizeof(path), "%s/Saves%s%s", v->game ? v->game : "", v->place_count > 1 ? "/" : "",
             v->place_count > 1 ? v->places[v->place] : "");
    ui_path(path, v->note);
  }

  if (!v->count)
    ui_wrap_center(UI_W / 2, LINE_BASE(4), RH, X1 - X0, 2, v->empty, &ui_font_small, UI_TEXT);
  for (r = 0; r < UI_SAVES_TILES && v->first + r < v->count; r++)
  {
    const struct ui_save_tile *t = &v->tiles[r];
    int here = v->first + r == v->picked, on = here && !v->nav_focus;
    int x = G_X(r % 3), y = G_Y0 + (r / 3) * G_RSTEP;
    char name[40];
    if (here)
      picked = t;
    if (t->action == UI_SAVE_RESUME && ui_backdrop_picture())
      ui_picture_fit(x, y, G_PW, G_PH, ui_backdrop_picture(), 240, 160, 240, !here);
    else if (t->action == UI_SAVE_NEW)
      ui_text_center(x + G_PW / 2, y + G_PH / 2 + 4, "+", &ui_font_small_strong, UI_TEXT);
    else if (t->picture)
      ui_picture_fit(x, y, G_PW, G_PH, t->picture, G_PW, G_PH, G_PW, !here);
    /* a marked one says so in its name ("SLOT 4 MARKED"; "#123 MARKED" where that is too long) */
    if (t->marked && strlen(t->name) > 7 && !strncmp(t->name, "Slot ", 5))
      snprintf(name, sizeof(name), "#%s marked", t->name + 5);
    else
      snprintf(name, sizeof(name), "%s%s", t->name, t->marked ? " marked" : "");
    ui_menu_scrolls |= tile_name(x, y, name, on, v->picked_ms);
  }
  ui_scrollbar(UI_W - 2, G_Y0, G_Y0 + G_RSTEP + G_BLOCK + RH, v->first / 3, UI_SAVES_TILES / 3, (v->count + 2) / 3);

  if (v->status && v->status[0])
    ui_footer_status(v->status, v->warn ? UI_RED : UI_TEXT);
  else
  {
    int n = 0;
    while (v->keys && v->keys[2 * n])
      n++;
    ui_band(v->nav_focus || !picked ? "" : picked->band, v->keys, v->nav_focus ? 0 : n);
  }
  if (v->tabs && v->nav_focus)
    snprintf(ui_shown, sizeof(ui_shown), "main:%d", v->tab_on);
  else
    snprintf(ui_shown, sizeof(ui_shown), "saves:%d", v->picked);
  ui_present();
}

/* ---- The save manager and the ROM manager ---- */

#define MG_W (UI_COL2 - UI_GUTTER)   /* the list: x 0-155, its block from the screen's edge; beside it the */
#define MG_PX UI_COL2                /* second column: the picked one's picture, name, facts, keys */
#define MG_PW G_PW
#define MG_PH G_PH

void ui_manager_draw(const struct ui_manager_view *v)
{
  static const char *const keys[] = { "!enter", "Mark", "A", "Mark all" };
  char fit[160];
  int r, y;

  ui_begin();
  ui_ground();
  ui_header(v->title, v->subtitle);

  if (!v->count)
    ui_wrap(X0, LINE_BASE(0), RH, X1 - X0, 4, v->empty, &ui_font_small, UI_TEXT2);   /* the drive's summary is long */
  for (r = 0; r < UI_MANAGER_ROWS && v->first + r < v->count; r++)
  {
    const struct ui_manager_row *row = &v->rows[r];
    int y0 = LINE_Y(r), focused = v->first + r == v->picked;
    char right_text[32];
    int size_w;
    u16 fg2, fg = line(4, MG_W - 4, y0, NULL, 0, focused, &fg2);
    int room;
    snprintf(right_text, sizeof(right_text), "%s%s", row->marked ? "+ " : "", row->size);
    size_w = ui_label_width(right_text);
    room = MG_W - X0 - size_w - 12;
    /* too long: cut, the picked one scrolls in its place (ui_line_text, as everywhere) */
    ui_line_text(X0, y0 + BASE, room, row->label, &ui_font_small, fg, focused, v->picked_ms);
    ui_label(MG_W - 6 - size_w, y0 + BASE, right_text, fg2);
  }
  ui_scrollbar(MG_W, TOP, TOP + UI_MANAGER_ROWS * RH, v->first, UI_MANAGER_ROWS, v->count);

  if (v->count)
  {
    /* the second column, on the body's lines: the picture (96x64), the name (one or two lines), its facts
     * on one line, then what the keys do to it, a key a line, in the band's form */
    if (v->picture)
      ui_picture(MG_PX, TOP, v->picture, MG_PW, MG_PH, MG_PW);
    else
      empty_picture(MG_PX, TOP, MG_PW, MG_PH);
    if (!v->picture && v->pic_empty && v->pic_empty[0])
      label_center(MG_PX + MG_PW / 2, TOP + MG_PH / 2 + 4, v->pic_empty, UI_TEXT);
    y = LINE_BASE(4);
    y = words(MG_PX, y, RH - 2, X1 - MG_PX, 2, v->head, &ui_font_small_strong, UI_TEXT) + RH;
    /* its facts on one line ("14 KB . Compressed"); the first alone when both don't fit */
    fit[0] = 0;
    for (r = 0; r < 2; r++)
      if (v->facts[r][0])
        snprintf(fit + strlen(fit), sizeof(fit) - strlen(fit), "%s%s", fit[0] ? " " UI_DOT " " : "", v->facts[r]);
    if (fit[0])
    {
      if (ui_label_width(fit) <= X1 - MG_PX)
        ui_label(MG_PX, y, fit, UI_TEXT);
      else
        ui_line_text(MG_PX, y, X1 - MG_PX, ui_caps(v->facts[0]), &ui_font_small_strong, UI_TEXT, 0, 0);
      y += RH;
    }
    ui_keys_column(MG_PX, y + 2, v->actions);
  }

  if (v->status && v->status[0])
    ui_footer_status(v->status, v->warn ? UI_RED : UI_TEXT);
  else
    ui_footer(keys, v->count ? 2 : 0);   /* (nothing to mark in an empty list) */
  if (v->percent >= 0)
    ui_progress(0, UI_FOOT_Y - 6, UI_W, (u32)v->percent, 100);
  snprintf(ui_shown, sizeof(ui_shown), "%s:%d", v->name, v->picked);
  ui_present();
}

/* ---- The speed test's results ---- */

#define SPEED_BAR_W 292
#define SPEED_BAR_MS 250   /* the bar's width: 25 ms, in tenths */

/* A part of the bar: tenths of ms from at, n long. */
static void time_segment(int x, int y, unsigned at, unsigned n, u16 color)
{
  int x0 = x + (int)(SPEED_BAR_W * (at > SPEED_BAR_MS ? SPEED_BAR_MS : at) / SPEED_BAR_MS);
  int x1 = x + (int)(SPEED_BAR_W * (at + n > SPEED_BAR_MS ? SPEED_BAR_MS : at + n) / SPEED_BAR_MS);
  if (x1 > x0)
    ui_rect(x0, y, x1 - x0, 4, color);
}

void ui_speed_draw(const struct ui_speed_view *v)
{
  char text[96];
  int i, y = TOP;

  ui_begin();
  ui_ground();
  if (v->mhz)
    snprintf(text, sizeof(text), "CPU %u MHz", v->mhz);
  else
    text[0] = 0;   /* the clock isn't known (the PC): no note */
  ui_header(v->stopped ? "Speed test (stopped)" : "Speed test", text);

  /* a part every two and a half lines: its name and frames a second, the bar of a frame's time, the times */
  for (i = 0; i < v->count; i++, y += 5 * RH / 2)
  {
    const struct ui_speed_part *p = &v->parts[i];
    const struct ui_font *nf = &ui_font_small;
    char fit[96];
    int num_x, room;
    snprintf(text, sizeof(text), "%u.%u", p->game_fps / 10, p->game_fps % 10);
    num_x = X1 - ui_text_width("fps", &ui_font_small_strong) - 5 - ui_text_width(text, &ui_font_small_strong);
    ui_text(num_x, y + BASE, text, &ui_font_small_strong, UI_TEXT);
    ui_text_right(X1, y + BASE, "fps", &ui_font_small_strong, UI_TEXT);
    room = num_x - 10 - X0;
    ui_text(X0, y + BASE, ui_fit(p->name, nf, room, fit, sizeof(fit)), nf, UI_TEXT);
    if (p->fast_forward)
    {
      snprintf(text, sizeof(text), "%u.%u pictures shown a second", p->drawn_fps / 10, p->drawn_fps % 10);
      ui_text(X0, y + RH + BASE + 4, ui_fit(text, &ui_font_small, X1 - X0, fit, sizeof(fit)), &ui_font_small, UI_TEXT);
      continue;
    }
    /* where a frame's time goes, against the 16.7 ms a frame has */
    ui_rect(X0, y + 20, SPEED_BAR_W, 4, UI_RAISED);
    time_segment(X0, y + 20, 0, p->cpu_ms, UI_TEXT);
    time_segment(X0, y + 20, p->cpu_ms, p->gfx_ms, UI_MUTED);
    time_segment(X0, y + 20, p->cpu_ms + p->gfx_ms, p->out_ms, UI_LINE);
    ui_rect(X0 + SPEED_BAR_W * 167 / SPEED_BAR_MS, y + 18, 1, 8, UI_MARK);
    /* a frame's milliseconds, each after its name and a colon, as the speed readout has them: 43 characters,
     * 284 pixels, inside the bar's 292 */
    snprintf(text, sizeof(text), "game: %u.%u " UI_DOT " drawing: %u.%u " UI_DOT " showing: %u.%u ms",
             p->cpu_ms / 10, p->cpu_ms % 10, p->gfx_ms / 10, p->gfx_ms % 10, p->out_ms / 10, p->out_ms % 10);
    ui_text(X0, y + RH + BASE + 4, ui_fit(text, &ui_font_small, X1 - X0, fit, sizeof(fit)), &ui_font_small, UI_TEXT);
  }

  /* where the numbers went: the band's what-happened */
  ui_footer_status("Saved to gpsp_results.txt", UI_TEXT);
  snprintf(ui_shown, sizeof(ui_shown), "speed:%d", v->count);
  ui_present();
}

/* ---- A question or a message ---- */

#define BAR_BASE_DIALOG UI_FOOT_BASE

void ui_dialog_draw(const struct ui_dialog_view *v)
{
  /* r16, one thing on the screen: the question alone on the white, its words where a page's are (its title in
   * capitals on the body's second line, what happens under it), its answers in the band as the paused menu's
   * choices are (the picked one a black block the band's height). Until r16 a card over the dimmed screen. */
  const struct ui_font *f = &ui_font_small_strong;
  char fit[96];
  int x, i;
  const char *answers[2] = { v->first, v->second };

  ui_fill(UI_BG);
  ui_text(X0, LINE_BASE(1), ui_fit(ui_caps(v->title), &ui_font_small_strong, X1 - X0, fit, sizeof(fit)),
          &ui_font_small_strong, UI_TEXT);
  ui_wrap(X0, LINE_BASE(3), RH, X1 - X0, 5, v->text, &ui_font_small, UI_TEXT);
  for (i = 0, x = X0; i < 2 && answers[i]; i++)
  {
    int w = ui_text_width(ui_caps(answers[i]), f);
    if (i == v->picked)
      ui_rect(x - 4, UI_FOOT_Y, w + 7, UI_H - UI_FOOT_Y, UI_ACCENT);
    x = ui_text(x, BAR_BASE_DIALOG, ui_caps(answers[i]), f, i == v->picked ? UI_FOCUS_INK : UI_TEXT) + 16;
  }
  snprintf(ui_shown, sizeof(ui_shown), "dialog:%d", v->picked);
  ui_present();
}

/* ---- The top-level screen (r19) ----
 * One screen, as the site is one page: the top row (ui_nav) has a word in each corner (SETTINGS, QUIT) and the
 * selector between them (GAMES; SAVES GAMES with a game paused); under it the picked word's things; the band at
 * the bottom. GAMES is the games' grid (ui_list_draw) and SAVES the paused game's saves (ui_saves_draw): both
 * draw this same row. Here: SETTINGS' grid of pages, and QUIT's one line. Left and right move along the row;
 * down or enter goes into what is under it, up or esc comes back (gui.c). One black block on the screen: the
 * word while the arrows are on the row, else the picked thing under it. */

#define P_Y TOP
#define T_Y (P_Y + 32)          /* a picture in the body's middle (the loading screen's): its top (row 68) */

static void main_menu(const struct ui_menu_view *v)
{
  const char *names[UI_MAX_ROWS];
  int i;

  ui_fill(UI_BG);
  for (i = 0; i < v->count; i++)
    names[i] = v->rows[i].label;
  ui_nav(names, v->count, v->picked, v->nav_focus);

  /* under the row: what the picked word says (Quit; Settings is a page drawn with this row, settings_page) */
  if (v->help && v->help[0])
    ui_wrap_center(UI_W / 2, LINE_BASE(4), RH, X1 - X0, 2, v->help, &ui_font_small, UI_TEXT);

  /* the band: what the last choice did */
  if (v->status && v->status[0])
    ui_footer_status(v->status, UI_TEXT);
}

/* The game waits for the other calculator: what's going on, in text. */
void ui_link_wait_draw(int other_in_menu)
{
  static const char *const keys[] = { "!esc", "Unlink" };
  const char *head = other_in_menu ? "The other player is in the menu" : "Waiting for the other GBA";
  int cx = UI_W / 2;

  ui_begin();
  ui_ground();
  ui_header("Link", NULL);
  ui_text_center(cx, LINE_BASE(4), ui_caps(head), &ui_font_small_strong, UI_TEXT);
  ui_wrap_center(cx, LINE_BASE(5), RH, X1 - X0, 3,
                 other_in_menu ? "The game goes on when they leave it."
                               : "The game goes on when the other calculator has caught up.",
                 &ui_font_small, UI_TEXT2);
  ui_footer(keys, 1);
  snprintf(ui_shown, sizeof(ui_shown), "link_wait:%d", other_in_menu);
  ui_present();
}

/* ---- Settings as tiles (ui18; nspire_ui_screens.h) ----
 * The store's grid with words where its pictures are: a tile is its name (small strong capitals) over its value
 * (small capitals, the exact same font size); 2 or 3 columns, the store's: 8 from the edges and between them
 * (x 8 / 112 / 216, 96 wide; x 8 / 164, 148 wide), a wide tile the whole 304; the picked one the store's plate. */
#define T_VFONT (&ui_font_small)          /* the value: same font size as name */
#define T_NFONT (&ui_font_small_strong)   /* the name: bold capitals, as the store's tile title */
#define T_NAME 12                         /* the name's baseline under the tile's top */
#define T_VALUE 24                        /* the value's baseline (5 over the 29 px plate's bottom, as the store's) */
#define T_ONE 18                          /* a tile with a name and no value: its baseline, the tile's middle */
#define T_EDIT (2 * ui_text_width("<", T_VFONT))         /* what the chevrons take beside a value */
#define T_ARROW 10                        /* the arrow after the name of a tile that opens something, and its gap */

/* A tile's width in cols columns, span of them. */
static int tile_w(int cols, int span)
{
  int cell = (UI_W - 16 - (cols - 1) * 8) / cols;
  return span * cell + (span - 1) * 8;
}

static int tile_x(int cols, int col)
{
  return 8 + col * (tile_w(cols, 1) + 8);
}

int ui_tile_fit(const char *name, const char *value, int editable)
{
  int nw = ui_text_width(ui_caps(name), T_NFONT), vw = ui_text_width(ui_caps(value), T_VFONT) + (editable ? T_EDIT : 0);
  int cols;
  for (cols = 3; cols > 1; cols--)
    if (nw <= tile_w(cols, 1) - T_ARROW && vw <= tile_w(cols, 1))
      break;
  return cols;
}

int ui_tiles_place(const u8 *flags, int count, int cols, u8 *row, u8 *col, u8 *span)
{
  int r = 0, c = 0, i;
  for (i = 0; i < count; i++)
  {
    if (flags[i] & UI_TILE_WIDE)
    {
      /* a row of its own */
      if (c)
        r++;
      row[i] = (u8)r;
      col[i] = 0;
      span[i] = (u8)cols;
      r++;
      c = 0;
      continue;
    }
    row[i] = (u8)r;
    col[i] = (u8)c;
    span[i] = 1;
    if (++c == cols)
    {
      r++;
      c = 0;
    }
  }
  return c ? r + 1 : r;
}

int ui_tile_step(const u8 *flags, int count, int cols, int picked, int dx, int dy)
{
  u8 row[UI_TILES_MAX], col[UI_TILES_MAX], span[UI_TILES_MAX];
  int rows, k, tries, r, c;
  if (count <= 0 || picked < 0 || picked >= count)
    return picked;
  if (count > UI_TILES_MAX)
    count = UI_TILES_MAX;
  if (dx)
  {
    /* the one before or after, in order (across rows), wrapping */
    for (k = picked, tries = 0; tries < count; tries++)
    {
      k = (k + dx + count) % count;
      if (!(flags[k] & UI_TILE_FIXED))
        return k;
    }
    return picked;
  }
  if (!dy)
    return picked;
  rows = ui_tiles_place(flags, count, cols, row, col, span);
  r = row[picked];
  c = col[picked];
  for (tries = 0; tries < rows; tries++)
  {
    int best = -1, best_d = 99;
    r = (r + dy + rows) % rows;
    if (r == row[picked])
      break;
    /* in that row: the tile over this column (a wide one is over every column), else the nearest */
    for (k = 0; k < count; k++)
      if (row[k] == r && !(flags[k] & UI_TILE_FIXED))
      {
        int d = c < col[k] ? col[k] - c : c >= col[k] + span[k] ? c - (col[k] + span[k] - 1) : 0;
        if (d < best_d)
        {
          best = k;
          best_d = d;
        }
      }
    if (best >= 0)
      return best;
  }
  return picked;
}

/* One tile, its top at y: its name, then its value; picked: the plate, the words white; editing: the value
 * between chevrons; opens: an arrow after its name. Returns 1 while a text in it scrolls (picked, too long). */
static int tile(int x, int y, int w, const char *name, const char *value, int picked, int editing, int opens,
                unsigned ms)
{
  /* r19: its two lines centred in it, as an item's name is under its picture */
  u16 ink = picked ? UI_FOCUS_INK : UI_TEXT;
  char line[96];
  int nw, vw;
  if (picked)
    ui_rect(x - G_PLATE, y, w + 2 * G_PLATE, UI_TILE_BLOCK, UI_ACCENT);
  /* (r19: no arrow after the name of a tile that opens a page or a screen: a name without a value says it) */
  (void)opens;
  snprintf(line, sizeof(line), "%s", name);
  nw = ui_text_width(line, T_NFONT);
#if defined(NSPIRE_HOST)
  /* a page's columns are picked so that no name or value is ever cut (ui_tile_fit): the PC build says so when one
   * is (the PC tests fail a run that says anything on stderr) */
  if (nw > w || ui_text_width(value, T_VFONT) + (editing ? T_EDIT : 0) > w)
    fprintf(stderr, "gpsp-host: text cut short: %s: %s\n", name, value);
#endif
  if (!value[0])
  {
    /* r19: a tile that only opens something (KEYS, MORE): its name alone, in the tile's middle */
    if (nw <= w)
    {
      ui_text(x + (w - nw) / 2, y + T_ONE, line, T_NFONT, ink);
      return 0;
    }
    return ui_line_text(x, y + T_ONE, w, line, T_NFONT, ink, picked, ms);
  }
  if (nw <= w)
    ui_text(x + (w - nw) / 2, y + T_NAME, line, T_NFONT, ink);
  else
    ui_line_text(x, y + T_NAME, w, line, T_NFONT, ink, picked, ms);
  /* its value; being changed: between chevrons */
  snprintf(line, sizeof(line), editing ? "<%s>" : "%s", value);
  vw = ui_text_width(line, T_VFONT);
  if (vw <= w)
  {
    ui_text(x + (w - vw) / 2, y + T_VALUE, line, T_VFONT, ink);
    return 0;
  }
  return ui_line_text(x, y + T_VALUE, w, line, T_VFONT, ink, picked, ms);
}

/* ---- A page that is a list (nspire_ui_screens.h) ---- */

/* The flags of a list page's tiles (its first v->tiles lines). */
static int page_tile_flags(const struct ui_list_page_view *v, u8 *flags)
{
  int i, n = v->tiles < v->count ? v->tiles : v->count;
  if (n > UI_TILES_MAX)
    n = UI_TILES_MAX;
  for (i = 0; i < n; i++)
    flags[i] = (u8)(v->lines[i].tile | (v->lines[i].fixed ? UI_TILE_FIXED : 0));
  return n;
}

static int page_tile_rows(const struct ui_list_page_view *v)
{
  u8 flags[UI_TILES_MAX], row[UI_TILES_MAX], col[UI_TILES_MAX], span[UI_TILES_MAX];
  int n = page_tile_flags(v, flags);
  return n ? ui_tiles_place(flags, n, v->cols > 1 ? v->cols : 2, row, col, span) : 0;
}

int ui_list_page_lines(const struct ui_list_page_view *v)
{
  int rows = page_tile_rows(v);
  if (rows > UI_TILE_ROWS)
    rows = UI_TILE_ROWS;
  return rows ? (UI_FOOT_Y - 6 - (TOP + rows * UI_TILE_PITCH)) / RH : UI_LINES;
}

int ui_list_page_step(const struct ui_list_page_view *v, int picked, int dx, int dy)
{
  u8 flags[UI_TILES_MAX];
  int n = page_tile_flags(v, flags), k, tries;
  if (picked < n)
  {
    u8 row[UI_TILES_MAX], col[UI_TILES_MAX], span[UI_TILES_MAX];
    int rows = ui_tiles_place(flags, n, v->cols > 1 ? v->cols : 2, row, col, span);
    if (dy > 0 && row[picked] == rows - 1)
    {
      /* down from the tiles' last row: the list's first line */
      for (k = n; k < v->count; k++)
        if (!v->lines[k].heading && !v->lines[k].fixed)
          return k;
    }
    else if (dy < 0 && row[picked] == 0 && v->count > n)
    {
      /* up from the first row: the list's last line */
      for (k = v->count - 1; k >= n; k--)
        if (!v->lines[k].heading && !v->lines[k].fixed)
          return k;
    }
    return ui_tile_step(flags, n, v->cols > 1 ? v->cols : 2, picked, dx, dy);
  }
  if (!dy)
    return picked;
  for (k = picked, tries = 0; tries < v->count; tries++)
  {
    k += dy;
    if (k >= v->count)
      k = 0;
    if (k < 0)
      k = v->count - 1;
    if (k < n)
    {
      /* into the tiles: up, their last row's first; down (past the list's end), the first */
      u8 row[UI_TILES_MAX], col[UI_TILES_MAX], span[UI_TILES_MAX];
      int rows = ui_tiles_place(flags, n, v->cols > 1 ? v->cols : 2, row, col, span), j;
      for (j = 0; j < n; j++)
        if (!(flags[j] & UI_TILE_FIXED) && (dy > 0 || row[j] == rows - 1))
          return j;
      continue;
    }
    if (!v->lines[k].heading && !v->lines[k].fixed)
      return k;
  }
  return picked;
}

int ui_list_page_draw(const struct ui_list_page_view *v)
{
  int i, n, top = TOP, lines = UI_LINES, end, x0 = X0, x1 = X1;

  ui_begin();
  ui_fill(UI_BG);
  ui_list_page_scrolls = 0;
  ui_header_from(v->from, v->title, v->note);
  /* (eight lines: the Controller page's sentence for a hub with nothing on it is six) */
  if (!v->count && v->empty)
    ui_wrap(X0, LINE_BASE(0), RH, X1 - X0, 8, v->empty, &ui_font_small, UI_TEXT);
  /* ui18: the tiles first, in their rows; the list under them */
  {
    u8 flags[UI_TILES_MAX], row[UI_TILES_MAX], col[UI_TILES_MAX], span[UI_TILES_MAX];
    int cols = v->cols > 1 ? v->cols : 2, rows;
    int first = 0;
    n = page_tile_flags(v, flags);
    rows = n ? ui_tiles_place(flags, n, cols, row, col, span) : 0;
    /* more rows than the screen's: a row at a time, to keep the picked one shown */
    if (rows > UI_TILE_ROWS)
    {
      if (v->picked >= 0 && v->picked < n && row[v->picked] >= UI_TILE_ROWS)
        first = row[v->picked] - UI_TILE_ROWS + 1;
      rows = UI_TILE_ROWS;
      ui_scrollbar(UI_W - 2, TOP, TOP + (UI_TILE_ROWS - 1) * UI_TILE_PITCH + UI_TILE_BLOCK, first, UI_TILE_ROWS,
                   ui_tiles_place(flags, n, cols, row, col, span));
    }
    for (i = 0; i < n; i++)
      if (row[i] >= first && row[i] < first + UI_TILE_ROWS)
        ui_menu_scrolls |= tile(tile_x(cols, col[i]), TOP + (row[i] - first) * UI_TILE_PITCH, tile_w(cols, span[i]),
                                v->lines[i].name, v->lines[i].kind, i == v->picked, 0, flags[i] & UI_TILE_OPENS,
                                v->picked_ms);
    if (rows)
    {
      top = TOP + rows * UI_TILE_PITCH;
      lines = (UI_FOOT_Y - 6 - top) / RH;
    }
  }
  end = top;
  /* (ui18: a list under tiles starts and ends where they do, 8 from the screen's edges; alone, on the margins) */
  if (n)
  {
    x0 = G_X(0);
    x1 = UI_W - G_X(0);
  }
  for (i = 0; i < lines && n + v->first + i < v->count; i++)
  {
    const struct ui_page_line *l = &v->lines[n + v->first + i];
    int y = top + i * RH, picked = n + v->first + i == v->picked && !l->heading;
    u16 ink = picked ? UI_FOCUS_INK : UI_TEXT, ink2 = picked ? UI_FOCUS_INK : UI_TEXT;
    int right = x1 - (v->count - n > lines ? 6 : 0), room;
    end = y + RH;

    if (l->heading)
    {
      ui_label(x0, y + BASE, l->name, UI_MUTED);
      continue;
    }
    if (picked)
      ui_rect(G_PLATE, y, UI_W - 2 * G_PLATE, RH, UI_FOCUS);   /* (ui18: as wide as the tiles' plates) */
    if (l->signal >= 0 && l->signal <= 4)
    {
      char strength[] = "0/4";
      strength[0] += l->signal;
      right -= ui_text_width(strength, &ui_font_small);
      ui_text(right, y + BASE, strength, &ui_font_small, ink);
      right -= 8;
    }
    if (l->kind[0])
    {
      int kw = ui_text_width(l->kind, &ui_font_small);
      if (kw > (x1 - x0) / 2)
        kw = (x1 - x0) / 2;   /* at most half the line: cut, or scrolling while picked */
      ui_menu_scrolls |= ui_line_text(right - kw, y + BASE, kw, l->kind, &ui_font_small, ink2, picked, v->picked_ms);
      right -= kw + 12;
    }
    room = right - x0;
    ui_list_page_scrolls |= ui_line_text(x0, y + BASE, room, l->name, &ui_font_small, ink, picked, v->picked_ms);
  }
  /* (at UI_W - 2 as every other bar: a pixel of ground between it and the picked line's block, which ends at 315;
   * the Bluetooth page with more devices than lines showed them touching) */
  ui_scrollbar(UI_W - 2, top, top + lines * RH, v->first, lines, v->count - n);
  if (v->status && v->status[0])
    ui_footer_status(v->status, UI_TEXT);
  else if (v->help && v->help[0])
    ui_footer_help(v->help);
  else
  {
    int n = 0;
    while (v->keys && v->keys[2 * n])
      n++;
    ui_footer(v->keys, n);
  }
  snprintf(ui_shown, sizeof(ui_shown), "%s:%d", v->name ? v->name : "list", v->picked);
  ui_present();
  return end;
}

/* r19: Settings > Cheats (nspire_ui_screens.h): a line a code, its name at the left and ON or OFF at the right
 * (OFF in grey), the picked line the black block; more than the screen's lines scroll, a bar at the right. */
int ui_cheats_scrolls;
void ui_cheats_draw(const struct ui_cheats_view *v)
{
  int i, vw = ui_text_width("OFF", &ui_font_small_strong);
  ui_begin();
  ui_fill(UI_BG);
  ui_cheats_scrolls = 0;
  ui_header_from("Settings", "Cheats", v->note);
  for (i = 0; i < v->shown; i++)
  {
    int y = TOP + i * RH, picked = v->first + i == v->picked;
    int right = X1 - (v->count > UI_CHEAT_LINES ? 6 : 0);
    const char *value = v->on[i] ? "On" : "Off";
    if (picked)
      ui_rect(G_PLATE, y, UI_W - 2 * G_PLATE, RH, UI_FOCUS);
    ui_text(right - ui_text_width(ui_caps(value), &ui_font_small_strong), y + BASE, value, &ui_font_small_strong,
            picked ? UI_FOCUS_INK : v->on[i] ? UI_TEXT : UI_MUTED);
    ui_cheats_scrolls |= ui_line_text(X0, y + BASE, right - vw - 12 - X0, v->names[i], &ui_font_small,
                                      picked ? UI_FOCUS_INK : UI_TEXT, picked, v->picked_ms);
  }
  /* (at UI_W - 2 as the grids' bars: a pixel of ground between it and the picked line's block, which ends at 315) */
  ui_scrollbar(UI_W - 2, TOP, TOP + UI_CHEAT_LINES * RH, v->first, UI_CHEAT_LINES, v->count);
  if (v->status && v->status[0])
    ui_footer_status(v->status, UI_TEXT);
  else if (v->help && v->help[0])
    ui_footer_help(v->help);
  snprintf(ui_shown, sizeof(ui_shown), "cheats:%d", v->picked);
  ui_present();
}

/* r19: which build this is, on the About page: the release, the commit it was built from and that commit's
 * date (tools/build_id.sh writes nspire_build_id.h; a build without it says the release alone). */
#if __has_include("nspire_build_id.h")
#include "nspire_build_id.h"
#endif
#define GPSP_RELEASE "r19"
#ifdef NSPIRE_BUILD_GIT
#define GPSP_BUILD_TEXT GPSP_RELEASE " " UI_DOT " " NSPIRE_BUILD_GIT
#define GPSP_BUILD_DATE NSPIRE_BUILD_DATE
#else
#define GPSP_BUILD_TEXT GPSP_RELEASE
#define GPSP_BUILD_DATE ""
#endif

static void about_page(void)
{
  /* a label's worth each: what it is, which build, whose it is, who to thank */
  static const char *const lines[][2] = {
    { "GPSP", "GAME BOY ADVANCE EMULATOR" },
    { "BUILD", GPSP_BUILD_TEXT },
    { "", GPSP_BUILD_DATE },
    { "FOR", "TI-NSPIRE CX, CX II" },
    { "CORE", "LIBRETRO GPSP" },
    { "PORT", "ANDYMCCA" },
    { "BUILT BY", "JADENMCDANIELS" },
    { "THANKS", "EXOPHASE " UI_DOT " DAVID G.F." },
    { "", "CALC84MANIAC " UI_DOT " VOGTINATOR" },
    { "", "NOTAZ " UI_DOT " LIBRETRO" },
  };
  size_t i;
  int y = LINE_BASE(0);
  for (i = 0; i < sizeof(lines) / sizeof(lines[0]); i++, y += RH)
  {
    ui_text(X0, y, lines[i][0], &ui_font_small, UI_TEXT);
    ui_text(X0 + 64, y, lines[i][1], &ui_font_small, UI_TEXT);
  }
}

int ui_menu_scrolls;


/* A settings page (ui18): its options as tiles (above: "Settings as tiles"), the picked one the store's plate,
 * its value between chevrons while it is being changed. More rows than the screen's four scroll a row at a time
 * to keep the picked one shown. (r16: a list, a line each, the name at the left and the value at the right;
 * until r16 a sheet of two columns of cells, a grey name in capitals over its value.) */
static void settings_page(const struct ui_menu_view *v)
{
  u8 flags[UI_TILES_MAX] = { 0 }, row[UI_TILES_MAX] = { 0 }, col[UI_TILES_MAX], span[UI_TILES_MAX];
  int i, first, rows, count = v->count < UI_TILES_MAX ? v->count : UI_TILES_MAX, cols = v->cols > 1 ? v->cols : 2;

  ui_fill(UI_BG);
  if (v->nav)
    ui_nav((const char **)v->nav, v->nav_count, v->nav_on, v->nav_focus);
  else
    ui_header_from(v->from, v->title, NULL);

  if (v->about)
  {
    about_page();
    return;
  }

  for (i = 0; i < count; i++)
    flags[i] = v->rows[i].tile;
  rows = ui_tiles_place(flags, count, cols, row, col, span);
  first = count && v->picked >= 0 && row[v->picked] >= UI_TILE_ROWS ? row[v->picked] - UI_TILE_ROWS + 1 : 0;
  for (i = 0; i < count; i++)
  {
    const struct ui_row *r = &v->rows[i];
    int picked = i == v->picked;
    const char *value;
    if (row[i] < first || row[i] >= first + UI_TILE_ROWS)
      continue;
    if (r->kind == UI_ROW_SWITCH)
      value = r->on ? "On" : "Off";
    else if (r->kind == UI_ROW_PICK || r->value[0])
      value = r->value;
    else
      value = "";   /* (a screen or a page: its name alone, in the tile's middle) */
    ui_menu_scrolls |= tile(tile_x(cols, col[i]), TOP + (row[i] - first) * UI_TILE_PITCH, tile_w(cols, span[i]),
                            r->label, value, picked,
                            picked && v->editing && (r->kind == UI_ROW_PICK || r->kind == UI_ROW_SWITCH),
                            r->kind == UI_ROW_SUBMENU, v->picked_ms);
  }
  ui_scrollbar(UI_W - 2, TOP, TOP + (UI_TILE_ROWS - 1) * UI_TILE_PITCH + UI_TILE_BLOCK, first, UI_TILE_ROWS, rows);

  /* the band: what just happened (the Sound page: the adapter), else the picked tile's one sentence, where it
   * has one (r16: only where its name and value can't say what it does) */
  if (v->status && v->status[0])
    ui_footer_status(v->status, UI_TEXT);
  else if (v->help && v->help[0])
    ui_footer_help(v->help);
}

void ui_menu_draw(const struct ui_menu_view *v)
{
  ui_begin();
  ui_menu_scrolls = 0;
  if (v->page < 0)
    main_menu(v);
  else
    settings_page(v);
  snprintf(ui_shown, sizeof(ui_shown), "%s:%d", v->page < 0 ? "main" : v->tabs[v->page], v->picked);
  if (v->page >= 0 && v->nav && v->nav_focus)
    snprintf(ui_shown, sizeof(ui_shown), "main:%d", v->nav_on);   /* (the arrows on the row, over Settings) */
  {
    /* the PC tests' report is words separated by spaces: "Games_and_saves:1" */
    char *c;
    for (c = ui_shown; *c; c++)
      if (*c == ' ')
        *c = '_';
  }
  ui_present();
}

/* ---- Starting a game ---- */

void ui_loading_draw(const struct ui_loading_view *v)
{
  static const char *const keys_cancel[] = { "Q", "Cancel" };
  const char *keys_more[2];
  char fit[160];
  int px = (UI_W - STATE_PIC_W) / 2, y;

  ui_begin();
  ui_ground();
  ui_header(v->what, NULL);
  if (!v->bar)
  {
    /* it stopped: the game's name, and what happened in words */
    y = words(X0, LINE_BASE(0), RH, X1 - X0, 2, ui_caps(v->title), &ui_font_small_strong, UI_TEXT) + 2 * RH;
    if (v->line)
      ui_wrap(X0, y, RH, X1 - X0, 5, v->line, &ui_font_small, UI_TEXT);
  }
  else
  {
    /* as the paused menu has a game: its 96x64 picture in the middle, its name under it (one line, cut as every
     * name), then the bar, and how far on the body's last line */
    if (v->picture)
      ui_picture_fit(G_X(1), T_Y, G_PW, G_PH, v->picture, STATE_PIC_W, STATE_PIC_H, STATE_PIC_W, 0);
    else
      initial_tile(G_X(1), T_Y, G_PW, G_PH, v->title);
    ui_text_center(UI_W / 2, T_Y + G_NAME, ui_fit(v->title, &ui_font_small_strong, X1 - X0, fit, sizeof(fit)),
                   &ui_font_small_strong, UI_TEXT);
    ui_progress(px, T_Y + G_NAME + 12, STATE_PIC_W, v->done, v->total);
    y = LINE_BASE(UI_LINES - 1);
    if (v->line)
      ui_label(px, y, v->line, UI_TEXT);
    if (v->fact)
      ui_label(px + STATE_PIC_W - ui_label_width(v->fact), y, v->fact, UI_TEXT);
  }
  if (v->cancel)
    ui_footer(keys_cancel, 1);
  else if (v->more)
  {
    keys_more[0] = "any key";
    keys_more[1] = v->more;
    ui_footer(keys_more, 1);
  }
  ui_present();
}

/* ---- Keys (r19): the list of what can be bound, and the calculator's keypad with every binding where it is ----
 * Left, the buttons and gpSP's own keys, a line each: its name, its key at the right ("NONE" in grey when it
 * has none). Right, the keypad of the TI-Nspire CX and CX II as it is laid out (the touchpad in the middle of
 * the top, the letters at the bottom), drawn in type alone: a key that does nothing is its own name in grey; a
 * key that is bound is what it does, in black ("A", "ST", an arrow); the picked line's key is the black block.
 * (Until r18 the keys were outlined and carried icons; r18 dropped the drawing; r19 shows the
 * keypad again, so that each binding is seen where its key is.) Keys are gpSP's numbers (nspire_keys.h). */
#define KP_W 168
#define KP_X (X1 - KP_W)   /* its right edge on the right margin */
#define KP_Y TOP
#define KL_W (KP_X - 12)   /* the list's lines end here (their block 4 further) */

struct kp_key
{
  u8 key, x, y, w, h;
  const char *cap;        /* its own name, shown when nothing is on it */
};

static const struct kp_key kp_keys[] = {
  /* top: esc, scratchpad, tab | home, doc, menu */
  { 104, 0, 0, 24, 13, "esc" }, { 91, 0, 15, 24, 13, "pad" }, { 106, 0, 30, 24, 13, "tab" },
  { 10, 144, 0, 24, 13, "on" }, { 100, 144, 15, 24, 13, "doc" }, { 102, 144, 30, 24, 13, "menu" },
  /* under the touchpad */
  { 122, 0, 46, 24, 13, "ctrl" }, { 121, 30, 46, 32, 13, "shift" }, { 82, 106, 46, 32, 13, "var" },
  { 90, 144, 46, 24, 13, "del" },
  /* left: = trig, ^ x2, e^x 10^x, ( ) */
  { 72, 0, 62, 11, 13, "=" }, { 26, 13, 62, 11, 13, NULL }, { 74, 0, 77, 11, 13, "^" }, { 42, 13, 77, 11, 13, NULL },
  { 58, 0, 92, 11, 13, NULL }, { 27, 13, 92, 11, 13, NULL }, { 86, 0, 107, 11, 13, "(" }, { 84, 13, 107, 11, 13, ")" },
  /* right: templates catalog, * /, + -, enter */
  { 89, 144, 62, 11, 13, NULL }, { 88, 157, 62, 11, 13, NULL }, { 73, 144, 77, 11, 13, "*" },
  { 57, 157, 77, 11, 13, "/" }, { 99, 144, 92, 11, 13, "+" }, { 83, 157, 92, 11, 13, "-" },
  { 2, 144, 107, 24, 13, "ent" },
  /* the numbers */
  { 56, 30, 62, 32, 13, "7" }, { 103, 68, 62, 32, 13, "8" }, { 52, 106, 62, 32, 13, "9" },
  { 40, 30, 77, 32, 13, "4" }, { 87, 68, 77, 32, 13, "5" }, { 36, 106, 77, 32, 13, "6" },
  { 24, 30, 92, 32, 13, "1" }, { 101, 68, 92, 32, 13, "2" }, { 20, 106, 92, 32, 13, "3" },
  { 8, 30, 107, 32, 13, "0" }, { 85, 68, 107, 32, 13, "." }, { 4, 106, 107, 32, 13, "-" },
};

/* the letters: four rows of nine from y 123, 14 apart (keys 16 x 13, 19 apart) */
static const u8 kp_letters[4][9] = {
  { 41, 71, 70, 69, 67, 66, 65, 55, 9 },
  { 25, 54, 53, 51, 50, 49, 39, 38, 97 },
  { 123, 37, 35, 34, 33, 23, 22, 21, 1 },
  { 0, 19, 18, 17, 7, 6, 5, 5, 0 },
};
static const char *const kp_letter_caps[4][9] = {
  { "EE", "A", "B", "C", "D", "E", "F", "G", "?" },
  { NULL, "H", "I", "J", "K", "L", "M", "N", NULL },
  { ",", "O", "P", "Q", "R", "S", "T", "U", NULL },
  { NULL, "V", "W", "X", "Y", "Z", "space", NULL, NULL },
};

/* How a key is drawn: its own name; what is bound to it; the picked line's (the block). */
enum { KS_NONE, KS_MARK, KS_LIT };

static void kp_draw_key(int x, int y, int w, int h, int state, const struct ui_control_row *row, const char *cap)
{
  x += KP_X;
  y += KP_Y;
  if (state == KS_LIT)
    ui_rect(x, y, w, h, UI_ACCENT);
  if (row && state != KS_NONE)
  {
    /* what it does, as long as fits the key ("START", "ST"); on a key too narrow for either, the key's own
     * name, in the black of a key that does something (the list says what) */
    const char *m = row->mark;
    if (m && ui_text_width(m, &ui_font_small_strong) > w + 2)
    {
      m = row->mark_short ? row->mark_short : m;
      if (ui_text_width(m, &ui_font_small_strong) > w && cap)
        m = cap;
    }
    if (m)
      ui_text_center(x + w / 2 + (w & 1), y + 10, m, &ui_font_small_strong, state == KS_LIT ? UI_ON_ACCENT : UI_TEXT);
  }
  else if (cap && ui_text_width(cap, &ui_font_small) <= w + 6)
    ui_text_center(x + w / 2 + (w & 1), y + 10, cap, &ui_font_small, UI_MUTED);
}

/* The row whose binding is on key k (its key, or the key held with it), the picked one first. */
static int row_on_key(const struct ui_controls_view *v, int k)
{
  int i;
  if (!k)
    return -1;
#define ON_KEY(r) ((r)->key == k || (r)->with == k || (r)->key2 == k)
  if (v->picked < v->count && ON_KEY(&v->rows[v->picked]) && !v->hint)
    return v->picked;
  for (i = 0; i < v->count; i++)
    if (ON_KEY(&v->rows[i]) && (!v->hint || v->rows[i].basic))
      return i;
#undef ON_KEY
  return -1;
}

static int key_state(const struct ui_controls_view *v, int k, const struct ui_control_row **row)
{
  int r = row_on_key(v, k);
  *row = r >= 0 ? &v->rows[r] : NULL;
  if (r < 0)
    return KS_NONE;
  return r == v->picked && !v->hint ? KS_LIT : KS_MARK;
}

/* The touchpad: its four edges are the arrows, always (they move in every game and menu): black, the picked
 * direction's the block. */
static void kp_touchpad(const struct ui_controls_view *v)
{
  static const char *const arrows[4] = { UI_ARROW_U, UI_ARROW_D, UI_ARROW_L, UI_ARROW_R };
  static const s8 at[4][2] = { { 54, 0 }, { 54, 30 }, { 34, 15 }, { 74, 15 } };   /* each arrow's cell, 20 x 13 */
  int i, lit = -1;
  if (!v->hint && v->picked < 4 && v->picked < v->count)
    lit = v->picked;
  for (i = 0; i < 4; i++)
  {
    int x = KP_X + 30 + at[i][0] - 10, y = KP_Y + at[i][1];
    if (i == lit)
      ui_rect(x, y, 20, 13, UI_ACCENT);
    ui_text_center(x + 10, y + 10, arrows[i], &ui_font_small_strong, i == lit ? UI_ON_ACCENT : UI_TEXT);
  }
}

static void kp_draw(const struct ui_controls_view *v)
{
  const struct ui_control_row *row;
  size_t i;
  int r, c;

  kp_touchpad(v);
  for (i = 0; i < sizeof(kp_keys) / sizeof(kp_keys[0]); i++)
  {
    const struct kp_key *k = &kp_keys[i];
    int st = key_state(v, k->key, &row);
    kp_draw_key(k->x, k->y, k->w, k->h, st, row, k->cap);
  }
  for (r = 0; r < 4; r++)
    for (c = 0; c < 9; c++)
    {
      int k = kp_letters[r][c], w = 16, st;
      if (!k || (c == 7 && r == 3))
        continue;
      if (c == 6 && r == 3)
        w = 35;   /* space: two keys wide */
      st = key_state(v, k, &row);
      kp_draw_key(c * 19, 123 + r * 14, w, 13, st, row, kp_letter_caps[r][c]);
    }
}

/* A line of the list: the name, its key at the right; w: where the line ends. */
static void keys_line(const struct ui_controls_view *v, int i, int y, int w)
{
  const struct ui_control_row *row = &v->rows[i];
  int focused = i == v->picked && !v->hint, none, rw;
  u16 fg = UI_TEXT;
  char text[64], fit[64];
  const char *k1 = row->key_text && row->key_text[0] ? row->key_text : "none";

  if (focused)
  {
    ui_rect(4, y, w, RH, UI_ACCENT);
    fg = UI_FOCUS_INK;
  }
  none = !strcmp(k1, "none");
  if (focused && v->capturing)
    snprintf(text, sizeof(text), "%s", v->pad ? "Press button" : "Press key");
  else if (row->key_text2 && row->key_text2[0])
    snprintf(text, sizeof(text), "%s / %s", k1, row->key_text2);
  else if (none)
  {
    /* r19: a clearer label than "NONE" for a key not set. "NO KEY", in grey: it fits beside every
     * name of twelve letters ("RESTART GAME"; "NOT SET" didn't). A line whose name leaves no room even for it
     * ("BRIGHTNESS DOWN" beside the keypad, once its key is cleared) has nothing at its right instead: a name is
     * never cut for it. */
    snprintf(text, sizeof(text), "No key");
    if (ui_text_width(ui_caps(row->name), &ui_font_small_strong) > w - 8 - ui_text_width("NO KEY", &ui_font_small) - 6)
      text[0] = 0;
  }
  else
    snprintf(text, sizeof(text), "%s", k1);
  rw = ui_text_width(ui_caps(text), &ui_font_small);
  ui_text(4 + w - 4 - rw, y + BASE, text, &ui_font_small, none && !focused ? UI_MUTED : fg);
  ui_text(X0, y + BASE, ui_fit(row->name, &ui_font_small_strong, w - 8 - rw - 6, fit, sizeof(fit)),
          &ui_font_small_strong, fg);
}

void ui_controls_draw(const struct ui_controls_view *v)
{
  static const char *const keys_capture[] = { "!esc", "Cancel" };
  static const char *const keys_hint[] = { "!enter", "Play" };
  static const char *const keys_list[] = { "!enter", "Set", "del", "Clear" };
  static const char *const keys_list_turbo[] = { "!enter", "Set", "del", "Clear", "tab", "Turbo" };
  static const char *const keys_list_pad[] = { "!enter", "Set", "del", "Clear", UI_ARROW_L UI_ARROW_R, "Controller" };
  static const char *const keys_list_kp[] = { "!enter", "Set", "del", "Clear", UI_ARROW_L UI_ARROW_R, "Calculator" };
  const int rows_shown = UI_LINES;
  /* the calculator's keys beside its keypad; a controller's, the whole width */
  const int w = v->pad ? UI_W - 8 : KL_W;
  const char *note = v->hint ? NULL : !v->pad_connected ? NULL :
                     v->pad ? (v->pad_name && v->pad_name[0] ? v->pad_name : "Controller") : "Calculator";
  char text[128], fit[96];
  int i, y;

  ui_begin();
  ui_fill(UI_BG);
  if (v->hint)
    ui_header("Keys", NULL);
  else
    ui_header_from("Settings", "Keys", note);

  if (v->hint)
  {
    /* before the first game: the keys it is played with, where they are */
    y = TOP;
    for (i = 0; i < v->count; i++)
      if (v->rows[i].basic == 1)
      {
        keys_line(v, i, y, w);
        y += RH;
      }
    kp_draw(v);
    ui_footer(keys_hint, 1);
    snprintf(ui_shown, sizeof(ui_shown), "keys_hint");
    ui_present();
    return;
  }

  if (v->conflict_row >= 0)
  {
    /* the key is another button's: say which, and ask */
    const char *now = v->target_name ? v->target_name : v->rows[v->picked].name;
    const char *other = v->conflict_name ? v->conflict_name : v->rows[v->conflict_row].name;
    snprintf(text, sizeof(text), "%s is %s", v->new_key_text, other);
    ui_text(X0, LINE_BASE(1), ui_fit(text, &ui_font_small_strong, X1 - X0, fit, sizeof(fit)), &ui_font_small_strong,
            UI_TEXT);
    snprintf(text, sizeof(text), "Use it for %s? %s will have no %s.", now, other,
             v->pad ? "button" : "key");
    ui_wrap(X0, LINE_BASE(3), RH, X1 - X0, 4, text, &ui_font_small, UI_TEXT);
    snprintf(text, sizeof(text), "Use for %s", now);
    /* the answers in the band, as every question's */
    {
      const char *answers[2] = { "Keep", text };
      int k, x = X0;
      ui_rect(0, UI_FOOT_Y, UI_W, UI_H - UI_FOOT_Y, UI_BG);
      for (k = 0; k < 2; k++)
      {
        int aw = ui_text_width(answers[k], &ui_font_small_strong);
        if (k == v->conflict_choice)
          ui_rect(x - 4, UI_FOOT_Y, aw + 7, UI_H - UI_FOOT_Y, UI_ACCENT);
        x = ui_text(x, UI_FOOT_BASE, answers[k], &ui_font_small_strong,
                    k == v->conflict_choice ? UI_FOCUS_INK : UI_TEXT) + 16;
      }
    }
    snprintf(ui_shown, sizeof(ui_shown), "keys_clash:%d", v->conflict_choice);
    ui_present();
    return;
  }

  /* the buttons, then Reset keys */
  for (i = v->first; i <= v->count && i < v->first + rows_shown; i++)
  {
    int ry = TOP + (i - v->first) * RH;
    if (i == v->count)
    {
      u16 fg = UI_TEXT;
      if (i == v->picked)
      {
        ui_rect(4, ry, w, RH, UI_ACCENT);
        fg = UI_FOCUS_INK;
      }
      ui_text(X0, ry + BASE, "Reset keys", &ui_font_small_strong, fg);
      continue;
    }
    keys_line(v, i, ry, w);
  }
  ui_scrollbar(4 + w + 2, TOP, TOP + rows_shown * RH, v->first, rows_shown, v->count + 1);
  if (!v->pad)
    kp_draw(v);

  /* the band: while a key is waited for, what for (or why the last one didn't count); else the picked line's
   * keys */
  if (v->capturing)
  {
    if (v->note && v->note[0])
      ui_footer_status(v->note, UI_TEXT);
    else
    {
      const char *name = v->target_name ? v->target_name : v->picked < v->count ? v->rows[v->picked].name : "";
      snprintf(text, sizeof(text), v->pad ? "Press a button for %s" : "Press a key for %s", name);
      ui_footer_status(text, UI_TEXT);
    }
    (void)keys_capture;
  }
  else if (v->band && v->band[0])
    ui_footer_status(v->band, UI_TEXT);
  else if (v->picked >= v->count)
    ui_footer(keys_list, 0);
  else if (v->pad_connected)
    ui_footer(v->pad ? keys_list_kp : keys_list_pad, 3);
  else if (v->rows[v->picked].turbo_text)
  {
    /* a GBA button: tab sets the key that presses it over and over; said with it when it has one */
    const char *pairs[6];
    const char *t = v->rows[v->picked].turbo_text;
    memcpy(pairs, keys_list_turbo, sizeof(pairs));
    if (strcmp(t, "none"))
    {
      snprintf(text, sizeof(text), "Turbo %s", t);
      pairs[5] = text;
    }
    ui_footer(pairs, 3);
  }
  else
    ui_footer(keys_list, 2);
  snprintf(ui_shown, sizeof(ui_shown), v->capturing && v->turbo ? "keys_turbo:%d" : "keys:%d", v->picked);
  ui_present();
}
