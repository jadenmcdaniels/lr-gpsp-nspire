/* What is drawn over the game, in the bands beside a 3:2 picture (13 rows
 * at the top and at the bottom; nspire_video_present.c draws a band only
 * when its text changes, so this costs nothing a frame). r16: in the menus'
 * type, case and spacing (docs/ui/spec.txt "3. TYPE"): IBM Plex Mono Medium in
 * capitals, white on the black band (the menus' black on white, turned over
 * for the dark around a game), no chip, no icon:
 *   top left   the speed ("FPS: 60", "FPS: 60  DRAWN: 45"), flush left;
 *   top right  the link and the controller as text;
 *   bottom     a message (a toast), at the left corner: one rule for every one. Its
 *              time on screen is its reading time (OSD_READ_MS), at least
 *              its kind's; one too long for the band's one line shows in two
 *              parts, cut at a space, each for its share of that time.
 * Drawn in 5:6:5 and turned to gpSP's 5:5:5 BGR on a landscape LCD. */

#include "common.h"
#include "nspire_frontend.h"
#include "nspire_ui.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

extern int nspire_lcd_portrait;

/* ---- R11w part 2 (2026-11-15): the message line's one rule ----
 * (nspire_frontend.h). A message stays on long enough to be read: one
 * shown only briefly is easily missed. */

int nspire_osd_kind(const char *text)
{
  static const char *const asks[] = { "Unplug", "unplug", "Plug ", "plug it", "Swap", "swap", "Put the", "Try ",
                                      "try a", "start gpSP there", "Settings > Controller", "restart the game",
                                      "Take the", NULL };
  size_t n = strlen(text);
  int i;

  if (n > 3 && !strcmp(text + n - 3, "..."))
    return OSD_WAIT;
  for (i = 0; asks[i]; i++)
    if (strstr(text, asks[i]))
      return OSD_ACTION;
  return OSD_INFO;
}

unsigned nspire_osd_hold_ms(int kind)
{
  return kind == OSD_ACTION ? OSD_ACTION_MS : kind == OSD_WAIT ? OSD_WAIT_MS : OSD_INFO_MS;
}

int nspire_osd_may_replace(int shown_kind, unsigned shown_ms, int new_kind)
{
  if (shown_ms >= nspire_osd_hold_ms(shown_kind))
    return 1;
  return new_kind == OSD_ACTION || shown_kind != OSD_ACTION;
}

/* r16: a toast is up for as long as it takes to read: a second, and 70 ms a character (about 14 a second, in
 * capitals), at least its kind's time (information and waits 2.5 s, what asks for something 5 s). "Saved to
 * slot 3" 2.5 s; a 50-character one 4.5 s, or 5 s when it asks for something. One too long for the band's one
 * line shows in two parts, each for its own reading time. */
#define OSD_READ_MS(chars) (1000u + 70u * (unsigned)(chars))

/* The band's one line: 316 pixels of the menu type. */
#define OSD_LINE_W 316

/* Where a toast too long for the band's line is cut in two (the space between the parts), 0 when it fits: at
 * the end of a sentence or a clause (". ", ": ", "; ", ", ") where both parts fit, else at the space nearest its
 * middle where both fit. (The band is 13 rows, nspire_video_present.c's: two lines at once don't fit it.) */
static size_t split_at(const char *text)
{
  char part[OSD_TEXT_MAX];
  size_t n = strlen(text), i, best = 0, best_clause = 0;
  long best_off = 1L << 20, clause_off = 1L << 20;
  if (nspire_osd_width(text) <= OSD_LINE_W)
    return 0;
  for (i = 1; i + 1 < n; i++)
  {
    long off;
    if (text[i] != ' ')
      continue;
    snprintf(part, sizeof(part), "%.*s", (int)i, text);
    if (nspire_osd_width(part) > OSD_LINE_W)
      break;
    if (nspire_osd_width(text + i + 1) > OSD_LINE_W)
      continue;
    off = (long)i * 2 - (long)n;
    if (off < 0)
      off = -off;
    if (strchr(".:;,", text[i - 1]) && off < clause_off)
    {
      clause_off = off;
      best_clause = i;
    }
    if (off < best_off)
    {
      best_off = off;
      best = i;
    }
  }
  return best_clause ? best_clause : best;
}

static unsigned hold_for(const char *text, int kind)
{
  size_t n = strlen(text), cut = split_at(text);
  unsigned read = cut ? OSD_READ_MS(cut) + OSD_READ_MS(n - cut - 1) : OSD_READ_MS(n), least = nspire_osd_hold_ms(kind);
  return read > least ? read : least;
}

static void line_show(struct osd_line *l, const char *text, int kind, unsigned now_ms)
{
  snprintf(l->text, sizeof(l->text), "%s", text);
  l->kind = kind;
  l->on = 1;
  l->at_ms = now_ms;
  l->until_ms = now_ms + hold_for(l->text, kind);
}

int osd_line_post(struct osd_line *l, const char *text, unsigned now_ms)
{
  int kind = nspire_osd_kind(text);

  (void)osd_line_visible(l, now_ms);
  if (l->on && !strcmp(l->text, text))
  {
    l->until_ms = now_ms + hold_for(l->text, kind);   /* the same again: kept up, not a new one */
    return 1;
  }
  if (!l->on || nspire_osd_may_replace(l->kind, now_ms - l->at_ms, kind))
  {
    line_show(l, text, kind, now_ms);
    return 1;
  }
  snprintf(l->pend, sizeof(l->pend), "%s", text);   /* the newest waits */
  l->pend_kind = kind;
  l->pend_on = 1;
  l->pend_ms = now_ms;
  return 0;
}

/* A toast too long for the band's one line, in two parts (split_at): the first for its reading time, then the
 * second for the rest of the toast's. What fits is shown as it is. */
static const char *line_part(const struct osd_line *l, unsigned now_ms)
{
  static char part[OSD_TEXT_MAX];
  size_t cut = split_at(l->text), n = strlen(l->text);
  unsigned total = l->until_ms - l->at_ms, first_ms;

  if (!cut)
    return l->text;   /* it fits (or one word is longer than the line: cut when drawn) */
  /* the toast's time (longer when its kind's is, or it was kept up) shared as the two parts' reading times */
  first_ms = (unsigned)((u64)total * OSD_READ_MS(cut) / (OSD_READ_MS(cut) + OSD_READ_MS(n - cut - 1)));
  if (now_ms - l->at_ms < first_ms)
    snprintf(part, sizeof(part), "%.*s", (int)cut, l->text);
  else
  {
    snprintf(part, sizeof(part), "%s", l->text + cut + 1);
    /* a text the line couldn't keep whole (OSD_TEXT_MAX: nspire_osd_message cut it there) ends at its last
     * whole word, with "...", never in a word */
    if (n == OSD_TEXT_MAX - 1 && strrchr(part, ' '))
      snprintf(strrchr(part, ' '), sizeof(part) - (size_t)(strrchr(part, ' ') - part), UI_ELLIPSIS);
  }
  return part;
}

const char *osd_line_visible(struct osd_line *l, unsigned now_ms)
{
  if (l->on && (int)(now_ms - l->until_ms) >= 0)
    l->on = 0;
  if (!l->on && l->pend_on)
  {
    l->pend_on = 0;
    if (now_ms - l->pend_ms <= OSD_PENDING_MS)
      line_show(l, l->pend, l->pend_kind, now_ms);
  }
  return l->on ? line_part(l, now_ms) : NULL;
}

void osd_line_extend(struct osd_line *l, unsigned now_ms)
{
  if (osd_line_visible(l, now_ms))
  {
    unsigned until = now_ms + nspire_osd_hold_ms(l->kind == OSD_ACTION ? OSD_WAIT : l->kind);
    if ((int)(until - l->until_ms) > 0)
      l->until_ms = until;
  }
}

void osd_line_withdraw(struct osd_line *l, const char *text)
{
  if (l->on && (!text || !strcmp(l->text, text)))
  {
    l->on = 0;
    l->until_ms = l->at_ms;
  }
  if (l->pend_on && (!text || !strcmp(l->pend, text)))
    l->pend_on = 0;
}

u32 nspire_osd_width(const char *text)
{
  return (u32)ui_text_width(text, &ui_font_small_strong);
}

/* 5:6:5 -> gpSP's 5:5:5 BGR, from x for w pixels of every row. */
static void to_landscape(u16 *band, u32 x, u32 w)
{
  u32 row, col;
  if (nspire_lcd_portrait)
    return;
  for (row = 0; row < OSD_BAND_H; row++)
    for (col = x; col < x + w && col < 320; col++)
    {
      u32 p = band[row * 320 + col], g = (p >> 5) & 63;
      band[row * 320 + col] = (u16)(((g & 1) << 15) | ((p & 31) << 10) | ((g >> 1) << 5) | (p >> 11));
    }
}

/* Text in white, x its left; stops before max_x. Its dark edge is the band
 * (or, over a picture that fills the screen, the black plate) it sits on:
 * nothing is drawn over the game's own pixels. */
#define OSD_WHITE 0xFFFFu
#define OSD_RED   UI_RGB(0xFF, 0x44, 0x38)

static void plate_text(u32 x, const char *text, u32 max_x)
{
  ui_clip(0, (int)max_x);
  ui_text((int)x, OSD_FONT_BASE, text, &ui_font_small_strong, OSD_WHITE);
  ui_clip(0, UI_W);
}

/* Every word over the game in capitals, as the menus' labels are ("FPS: 60  DRAWN: 54", "SAVED TO SLOT 3"):
 * the frontend keeps its words as written (the PC tests and the logs read them), they are set in capitals
 * here. (r16: messages too; until then only the readout.) */
static const char *readout_caps(const char *text)
{
  return ui_caps(text);
}

/* The readout's left edge: flush with the screen's (the glyphs' own side bearing is the gap). */
#define OSD_LEFT_X 1

/* Text with its left edge at x on a plate of width 320 (the band's rows
 * are OSD_BAND_H high); stops before max_x. The color is ignored: what is
 * over the game is white. */
void nspire_osd_text(u16 *band, u32 x, const char *text, u16 color, u32 max_x)
{
  u16 *saved = ui_fb;
  (void)color;
  ui_fb = band;
  text = readout_caps(text);
  plate_text(x, text, max_x);
  ui_fb = saved;
  to_landscape(band, x > 1 ? x - 1 : 0, nspire_osd_width(text) + 3);
}

void nspire_osd_band(u16 *band, const char *left, u16 left_color, const char *centered, u16 centered_color,
                     const char *right, u16 right_color)
{
  u16 *saved = ui_fb;
  int left_end = 316;   /* where the left text has to stop: before the chip, before the icons */
  (void)left_color;
  (void)centered_color;
  (void)right_color;
  memset(band, 0, 320 * OSD_BAND_H * sizeof(u16));
  ui_fb = band;
  if (centered && centered[0])
  {
    /* the toast: its words in capitals, white on the band, at its left corner as the readout is at the top's
     * (r16: small, anchored, precise; one line: osd_line_visible gives a long one in two parts); a single word
     * longer than the line ends in "..." rather than in a letter. (Over a picture that fills the screen the
     * plate under it is nspire_video_present.c's, the pinned code's: there it stays in the middle.) */
    char fit[OSD_TEXT_MAX + 8];
    const char *text = ui_caps(centered);
    int tw = ui_text_width(text, &ui_font_small_strong), x;
    if (tw > OSD_LINE_W)
    {
      text = ui_fit(text, &ui_font_small_strong, OSD_LINE_W, fit, sizeof(fit));
      tw = ui_text_width(text, &ui_font_small_strong);
    }
    x = OSD_LEFT_X;
    ui_text(x, OSD_FONT_BASE, text, &ui_font_small_strong, OSD_WHITE);
    left_end = x - 8;
  }
  if (right && right[0])
  {
    int edge = UI_W - 4;
    if (strstr(right, "battery"))
    {
      edge -= ui_text_width("LOW", &ui_font_small_strong);
      ui_text(edge, OSD_FONT_BASE, "LOW", &ui_font_small_strong, OSD_RED);
      edge -= 6;
    }
    if (strstr(right, "link") || !strncmp(right, "Linked", 6))
    {
      edge -= ui_text_width("LINK", &ui_font_small_strong);
      ui_text(edge, OSD_FONT_BASE, "LINK", &ui_font_small_strong, OSD_WHITE);
      edge -= 6;
    }
    if (strstr(right, "pad"))
    {
      edge -= ui_text_width("PAD", &ui_font_small_strong);
      ui_text(edge, OSD_FONT_BASE, "PAD", &ui_font_small_strong, OSD_WHITE);
      edge -= 6;
    }
    if (edge < left_end)
      left_end = edge;
  }
  if (left && left[0] && left_end > OSD_LEFT_X)
    plate_text(OSD_LEFT_X, readout_caps(left), (u32)left_end);
  ui_fb = saved;
  to_landscape(band, 0, 320);
}
