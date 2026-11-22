/* The menus' drawing kit: a 320x240 landscape frame in 5:6:5 (the LCD's own
 * format), turned onto the LCD by flip_screen_ui. Smooth text from pre-drawn
 * glyphs (nspire_ui_font.h), icons (nspire_ui_icons.h), flat rectangles,
 * pictures scaled down by area averaging.
 *
 * The look ("plain", docs/ui/spec.txt): a
 * white page, black type in one monospaced face (IBM Plex Mono, see
 * docs/fonts/README.md) at one size, things shown as a grid of pictures (a picture with
 * a short name under it), tabs instead of menus, nothing decorated, no
 * notes. The picked thing is a black block with its type in white; the one
 * colour on a screen is the picked game's picture. The kit still has the
 * earlier looks' tools (the lit ground, leaning bands, giant numbers, a
 * theme's colours): the plain look leaves them unused or flat. */
#ifndef NSPIRE_UI_H
#define NSPIRE_UI_H

#include "common.h"

#define UI_W 320
#define UI_H 240

#define UI_RGB(r, g, b) ((u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

/* ---- Colour: a theme ---- */
struct ui_theme
{
  const char *name;
  u16 bg;          /* the ground at its bottom (its darkest), and the band under the lines */
  u16 bg2;         /* the ground at its top: the field's colour */
  u16 raised;      /* a tile: empty pictures, a question's block, the resting button */
  u16 line;        /* the bracket, tracks, key chips, the keypad's keys */
  u16 muted;       /* values, facts, what is off */
  u16 text2;       /* a sentence under a title */
  u16 text;        /* text */
  u16 accent;      /* the picked line's block */
  u16 ink;         /* text on it */
  u16 ink2;        /* a value on it */
  u16 red;         /* what destroys something: its word, and its block when picked */
  u16 on_red;      /* text on that block */
  u16 detail;      /* what is in the back: the ground's own colour, a step off it */
  u16 mark;        /* what is live: progress, the picked slot's frame, a link that is up */
  u16 label;       /* the line that goes with a big word (when, how many, which slot) */
  u16 second;      /* the second colour: the stripe along a cut, the tab before a title, a picture's block */
  u16 shadow;      /* the big words' hard shadow on the ground */
};

#define UI_THEMES 1
extern const struct ui_theme ui_themes[UI_THEMES];
extern struct ui_theme ui_theme;         /* the one in use */
extern const char *const ui_theme_names[UI_THEMES];
/* n: 0 to UI_THEMES - 1 (anything else is 0) */
void ui_theme_set(unsigned n);

#define UI_BG       (ui_theme.bg)
#define UI_RAISED   (ui_theme.raised)
#define UI_LINE     (ui_theme.line)
#define UI_MUTED    (ui_theme.muted)
#define UI_TEXT2    (ui_theme.text2)
#define UI_TEXT     (ui_theme.text)
#define UI_ACCENT   (ui_theme.accent)
#define UI_ON_ACCENT (ui_theme.ink)
#define UI_FOCUS    (ui_theme.accent)    /* the focused line: a block of the accent */
#define UI_FOCUS_INK (ui_theme.ink)      /* text on it */
#define UI_FOCUS_INK2 (ui_theme.ink2)    /* facts and values on it */
#define UI_RED      (ui_theme.red)
#define UI_DETAIL   (ui_theme.detail)
#define UI_MARK     (ui_theme.mark)
#define UI_LABEL    (ui_theme.label)
#define UI_SECOND   (ui_theme.second)
#define UI_SHADOW   (ui_theme.shadow)
/* older names, for the screens drawn outside nspire_ui_screens.c */
#define UI_SURFACE  UI_RAISED
#define UI_TEXT3    UI_MUTED
#define UI_WARN     UI_TEXT

/* ---- The grid: one for every screen (pixels; text y is a baseline) ----
 * (r16, docs/ui/spec.txt "2. GRID"). Every screen is three parts, always in the same
 * places:
 *   the header   its title on UI_HEAD_BASE at the left margin, underlined
 *                (rows UI_HEAD_LINE, +1); where it is opened from before it,
 *                a note and the battery at the right (ui_header_from);
 *   the body     from UI_TOP, in lines of UI_LINE_H (a line's text on its top
 *                + UI_LINE_BASE); a cell of a settings page is two lines
 *                (UI_CELL); at most UI_LINES lines, so the body ends by
 *                UI_TOP + UI_LINES * UI_LINE_H, a gap over the band;
 *   the band     rows UI_FOOT_Y-239, its one line on UI_FOOT_BASE: what just
 *                happened (ui_footer_status), else the picked thing's one
 *                sentence (ui_footer_help), else the keys that aren't obvious
 *                (ui_footer). The paused menu's row of choices is its band.
 * Across: UI_MARGIN on both sides; two columns of UI_COL_W, UI_GUTTER apart
 * (the second from UI_COL2); three of UI_COL3_W (the game list). No screen
 * places anything by its own numbers but these. */
#define UI_MARGIN   8           /* the one left edge: everything starts here (a tile, a line, the tabs), and ends as far from the right edge */
#define UI_GUTTER   8           /* between two columns */
#define UI_COL_W    ((UI_W - 2 * UI_MARGIN - UI_GUTTER) / 2)   /* 142: a column of two */
#define UI_COL2     (UI_MARGIN + UI_COL_W + UI_GUTTER)          /* 164: the second column */
#define UI_COL3_W   ((UI_W - 2 * UI_MARGIN) / 3 + 1)            /* 97: a column of three */
#define UI_HEAD_BASE 18         /* the top row's baseline: the tabs, or the path of the page you are in */
#define UI_HEAD_Y    5          /* the picked tab's block: rows UI_HEAD_Y to UI_HEAD_Y + UI_LINE_H - 1 */
#define UI_HEAD_LINE 24         /* the 2-pixel underline under the title (the tab you are on) */
#define UI_TOP      36          /* the body's first line (a settings page's first hairline) */
#define UI_LINE_H   18          /* a line of the body: a list's line, the picked one's block */
#define UI_LINE_BASE 13         /* a line's baseline under its top */
#define UI_CELL     (2 * UI_LINE_H)   /* a settings page's cell: its name over its value; its block 2 shorter */
#define UI_LINES    10          /* the body's lines at most: it ends on row 216 */
#define UI_FOOT_Y   222         /* the band: what just happened, the picked thing's sentence, or the keys */
#define UI_FOOT_BASE 235        /* its baseline */
/* the names the screens had before the grid (the same places) */
#define UI_HEAD_H   UI_TOP
#define UI_ROW_H    UI_LINE_H
#define UI_ROW_BASE UI_LINE_BASE
#define UI_ROWS     UI_LINES
#define UI_HELP_BASE (UI_FOOT_Y - 4)   /* the last line over the band (the Controller page's raw words) */

/* Characters drawn from control codes */
#define UI_ELLIPSIS "\x01"
#define UI_DOT      "\x02"   /* middle dot, between facts */
#define UI_LEFT     "\x03"   /* single angle quotes */
#define UI_RIGHT    "\x04"
#define UI_ARROW_L  "\x05"
#define UI_ARROW_U  "\x06"
/* A space that wrapped text doesn't break at (Latin-1's), as in "4\xa0MB":
 * drawn as a space. */
#define UI_NBSP     "\xa0"
#define UI_ARROW_R  "\x07"
#define UI_ARROW_D  "\x08"

/* ---- Type ---- */

struct ui_glyph
{
  u16 bits;       /* first byte of its rows in the font's bits (a font is under 64 KB) */
  s8 x, y;        /* top-left from the pen (y from the baseline) */
  u8 w, h;
};

struct ui_kern
{
  u16 pair;       /* left << 8 | right */
  s16 adjust;     /* 1/64 pixel */
};

struct ui_font
{
  const struct ui_glyph *glyphs;   /* [char][phase] */
  const u8 *bits;                  /* 4 bits a pixel, rows padded to bytes */
  const u16 *advance;              /* 1/64 pixel */
  const struct ui_kern *kern;
  u16 kern_count;
  u8 ascent, descent;
  u8 phases;                       /* positions between pixels its glyphs are drawn at (1 or 2) */
};

/* The big words: heavy italic capitals 18 high (no small letters: they are
 * always set in capitals), in four widths from narrow to wide. */
extern const struct ui_font ui_font_d62, ui_font_d75, ui_font_d100, ui_font_d125;
#define ui_font_large ui_font_d75                   /* a title, a name set as it comes */
extern const struct ui_font ui_font_body;           /* 13 px: what is read or chosen */
extern const struct ui_font ui_font_body_strong;    /* 13 px medium: the same, emphasised */
extern const struct ui_font ui_font_small;          /* 12 px: a note */
extern const struct ui_font ui_font_small_strong;   /* 12 px medium: labels (ui_label), keys, the speed readout */
extern const struct ui_font ui_font_osd;            /* 11 px medium: in-game OSD messages */
/* the names the other screens use */
#define ui_font_title ui_font_large
#define ui_font_bold  ui_font_body_strong
#define ui_font_caps  ui_font_small_strong
#define ui_font_num   ui_font_small_strong

/* ---- Icons (nspire_ui_icons.h: UI_ICON_PLAY ...) ---- */
#include "nspire_ui_icons.h"
/* size 16 or 12; x, y its top-left */
void ui_icon(int x, int y, int id, int size, u16 color);
/* ui18: a Settings page's glyph (UI_GLYPH_SOUND ...: the page's tile, as a game's picture is its tile in the
 * store), UI_GLYPH_SIZE square, drawn from strokes as the icons are, one heavier stroke for all */
void ui_page_glyph(int x, int y, int id, u16 color);

/* The frame being drawn (nspire_landscape). */
extern u16 *ui_fb;
/* What the frame on the screen shows, for the PC tests: "main:2" (the main
 * menu, its third line picked), "Graphics:0", "list:5", "folders:1"; ""
 * when it isn't one of these screens. */
extern char ui_shown[32];
/* The battery in the header: 0-100, -1 when not known (not drawn); and
 * whether it charges. */
extern int ui_battery;
extern int ui_charging;

void ui_begin(void);
/* Shows the frame (flip_screen_ui). While ui_defer_present is set it shows
 * nothing: a screen drawn as the ground of a dialog, which presents both. */
extern int ui_defer_present;
void ui_present(void);

void ui_fill(u16 color);
/* The same around the rectangle (x, y, w, h; x and w even) that something opaque covers next. */
void ui_fill_around(u16 color, int x, int y, int w, int h);
/* The ground every screen starts with: the field's colour running from
 * bright at the top to dark at the bottom (dithered: the screen has 32
 * levels a colour), and in it, tone on tone, a wide leaning band and two
 * thin ones. */
void ui_ground(void);
/* Everything that leans leans as the big words do, 1 in 5: how far a row
 * is to the right of where the shape is at the screen's bottom. */
#define UI_LEAN(y) ((UI_H - 1 - (y)) / 5)
/* A leaning band w wide between rows y0 and y1 (its left edge at x on the
 * bottom row), alpha 0 (none) to 16 (all). */
void ui_lean(int x, int w, int y0, int y1, u16 color, int alpha);
/* The cut down the paused menu: the lines to its left, the game to its
 * right. Its x on row y. */
#define UI_CUT_BOTTOM 112
#define UI_CUT(y) (UI_CUT_BOTTOM + UI_LEAN(y))
#define UI_CUT_STRIPE 3          /* what runs along it: a red stripe this wide */
/* The paused menu's ground: left of the cut (all of it: everywhere), and
 * the stripe along the cut. */
void ui_ground_cut(int all);
void ui_rect(int x, int y, int w, int h, u16 color);
/* alpha: 0 (none) to 16 (all) */
void ui_rect_alpha(int x, int y, int w, int h, u16 color, int alpha);
/* r: 0-3 (the system's corners are 2) */
void ui_rrect(int x, int y, int w, int h, int r, u16 color);
void ui_rrect_alpha(int x, int y, int w, int h, int r, u16 color, int alpha);
/* A 1-pixel outline. */
void ui_rframe(int x, int y, int w, int h, int r, u16 color);
void ui_hline(int x, int y, int w, u16 color);
/* A filled circle, centre and radius in half pixels (smooth edge). */
void ui_disc(int cx2, int cy2, int r2, u16 color);

/* Text with its baseline at y; returns the x after it. */
int ui_text(int x, int y, const char *s, const struct ui_font *f, u16 color);
int ui_text_right(int right, int y, const char *s, const struct ui_font *f, u16 color);
int ui_text_center(int center, int y, const char *s, const struct ui_font *f, u16 color);
int ui_text_width(const char *s, const struct ui_font *f);
/* Text is only drawn between x0 and x1 (a name that scrolls); reset with
 * ui_clip(0, UI_W). */
void ui_clip(int x0, int x1);
/* s cut with an ellipsis to fit max_w; returns out. */
char *ui_fit(const char *s, const struct ui_font *f, int max_w, char *out, size_t size);
/* Draws s in lines of at most max_w (breaking at spaces; '\n' counts as a
 * space), at most max_lines; returns how many lines it took. */
int ui_wrap(int x, int y, int line_h, int max_w, int max_lines, const char *s, const struct ui_font *f,
            u16 color);
/* The same, each line centered on center. */
int ui_wrap_center(int center, int y, int line_h, int max_w, int max_lines, const char *s,
                   const struct ui_font *f, u16 color);
/* How many lines ui_wrap would take for s, without drawing it. */
int ui_wrap_lines(int max_w, int max_lines, const char *s, const struct ui_font *f);

/* ---- The three voices ---- */

/* s in capitals (ASCII), in one of a few buffers used in turn: good until
 * six more calls. */
const char *ui_caps(const char *s);
/* Extra space after each character of the text drawn next, in 1/64 pixel
 * (0: none). ui_text_width counts it. */
void ui_tracking(int extra64);
/* A big word (in capitals), baseline y; returns the x after it. */
int ui_big(int x, int y, const char *s, u16 color);
int ui_big_width(const char *s);
/* The same made width wide, as a poster makes a short word as wide as a
 * long one: set in the widest of the four cuts that fits, its letters then
 * spaced out to the width (by 8 pixels each at most: a word that would
 * need more stays shorter). A word too long for the narrowest cut is
 * squeezed a little (to 0.8 at most), then cut ("..."). Returns the x
 * after it. */
int ui_big_wide(int x, int y, const char *s, u16 color, int width);
/* The width a big word takes in the narrowest cut (what ui_big_wide can
 * squeeze into width * 5 / 4). */
int ui_big_narrow_width(const char *s);
/* A big number (or word) in the widest cut; returns the x after it. */
int ui_big_number(int x, int y, const char *s, u16 color);
/* While on, the big words (ui_big, ui_big_wide, ui_big_number, a header's
 * title) carry a hard shadow, 2 down and right: for words on the ground,
 * not for words on a block. */
void ui_shadow(int on);
/* A label: small bold capitals, spaced out. Returns the x after it. */
int ui_label(int x, int y, const char *s, u16 color);
int ui_label_right(int right, int y, const char *s, u16 color);
int ui_label_width(const char *s);
/* The screen's name very large (the big words, three times the size), tone
 * on tone, in the back: its right end at right, baseline y, only between
 * rows y0 and y1. Drawn before what goes over it. */
void ui_backdrop_word(const char *s, int right, int y, int y0, int y1);
/* A word or number very large (the big words' widest cut, scale times the
 * size: 2 to 6), with crisp edges: its right end at right, baseline y, only
 * between rows y0 and y1 and left of x1. outline: only its outline, as a
 * jersey's number is drawn. */
void ui_giant(const char *s, int right, int y, int scale, int outline, u16 color, int y0, int y1, int x1);

/* ---- Components ---- */

/* The header: the title at the left margin, underlined as the tab you are
 * on, an optional note and the battery at the right; all labels (small
 * capitals), the one size of the rest of the screen, as the game list's
 * tabs and the paused menu's top line. */
void ui_header(const char *title, const char *note);
/* The same with where the screen is, before its title: "SETTINGS  PICTURE"
 * (from: in grey, or NULL). */
void ui_header_from(const char *from, const char *title, const char *note);
/* The battery as the header has it (the charge and its icon), its right
 * edge at right; returns its left edge. Nothing (right) when not known. */
int ui_header_battery(int right);
/* A 4x4 square: "on", "linked", a mark on a key. */
void ui_led(int x, int y, u16 color);
/* "gpSP", baseline y; returns the x after it. */
int ui_wordmark(int x, int y);
/* The focused line: a flat block of the accent. Returns the colour its
 * text takes (UI_FOCUS_INK). */
u16 ui_focus(int x, int y, int w, int h);
/* The same for a line of the paused menu: from the left edge to the cut. */
u16 ui_focus_cut(int y, int h);
/* Draw a key name at the text baseline and return its right edge. */
int ui_key_hint(int x, int baseline, const char *key);
/* The keys that aren't obvious, in the band under the lines: pairs of key
 * and what it does, from the left margin while they fit. enter and esc are
 * never said (they are the same on every screen: enter does, esc goes back). */
void ui_footer(const char *const *pairs, int count);
/* The band's left: a message instead (what just happened), in capitals. */
void ui_footer_status(const char *text, u16 color);
/* The band: the picked thing's one sentence (a setting the label and value
 * can't say all of), in black, as it is written. */
void ui_footer_help(const char *text);
/* The keys' pairs as the band has them (a chip, its word in capitals), one
 * under the other from baseline y, UI_LINE_H apart, while above the band: a
 * manager's actions beside its list. Returns the baseline after the last. */
int ui_keys_column(int x, int y, const char *const *pairs);

/* ---- Widgets of the grid (r16) ---- */

/* r19: the top row of every top-level screen: a word in each corner (names[0] at the left margin,
 * the last at the right one) and the selector between them, centred ("SAVES GAMES" with a game paused, "GAMES"
 * without). focus: the arrows are on the row: every word black, word `on` a black block with its type in white;
 * else the arrows are in the body under it: word `on` black, the others grey (the body's picked thing has the
 * block: one block on a screen, where you are). */
void ui_nav(const char *const *names, int count, int on, int focus);
/* r19: where a page is, as a path at the top left ("SETTINGS / PICTURE", "SETTINGS / SYSTEM / ABOUT"): the
 * steps before the last in grey, the last black; a note at the right (or NULL). The steps come as one string,
 * '/' between them. */
void ui_path(const char *path, const char *note);
/* Tabs, as the game list's GAMES and FOLDERS: labels 12 apart on baseline
 * y, the one that is on (on: its index) black and underlined, the others in
 * grey. Returns the x after the last. */
int ui_tabs(int x, int y, const char *const *names, int count, int on);
/* A line of text w wide at x (baseline y), the one rule for a text too long for its place, on every screen: cut
 * with "..." , except while picked, where after a second it runs to the left a pixel at a time and comes round
 * again from the right (r19; ms: how long it has been picked; ui_scroll_after). Returns 1 when it is too long
 * and picked (the screen draws it again while it moves). */
int ui_line_text(int x, int y, int w, const char *s, const struct ui_font *f, u16 color, int picked,
                 unsigned ms);
/* How far a text has run after ms of being picked, 0 to period - 1 (period: its width and the gap before it comes
 * round): it waits 1 s, runs at 30 px a second, and waits again each time it is back at its start. */
int ui_scroll_after(unsigned ms, int period);
/* r19: the band of a screen of items: left, what the picked one is (its facts: "4 SAVES . 2 H"); right, before
 * the battery, the keys that aren't obvious (pairs: key, what it does), as many as fit, the first ones first. */
void ui_band(const char *left, const char *const *pairs, int count);
/* r19: a few words at the band's right end, before the battery ("12/640"): after the band's own words. */
void ui_footer_note(const char *text);
/* A line of a list, the whole width, its top at y: its name at the left
 * margin, a value (a fact, in grey; NULL for none) at the right margin; the
 * picked one a black block with its type in white. heading: a section's
 * name in grey capitals instead (never picked). Returns 1 as ui_line_text. */
int ui_list_line(int y, const char *name, const char *value, int picked, int heading, unsigned ms);
/* A signal's strength as four plain bars (2 wide, 1 apart, 3 to 9 high, standing on baseline y), its right
 * edge at right: bars (0-4) filled, the rest a 1-pixel stub. Returns its left edge. */
#define UI_SIGNAL_W 11
int ui_signal(int right, int y, int bars, u16 color);
/* < > around a value, chevrons 5x9 with their tops at y. */
void ui_chevrons(int left, int right, int y, u16 color);
/* A thin progress bar, 4 high: done of total. */
void ui_progress(int x, int y, int w, u32 done, u32 total);
/* A scroll position along the right edge, between y0 and y1: first of
 * count, shown at a time. */
void ui_scrollbar(int x, int y0, int y1, int first, int shown, int count);

/* Pictures in 5:6:5. */
void ui_blit(int x, int y, const u16 *src, int w, int h, int pitch);
/* With a 1-pixel hairline around it (the picture's edge on a dark ground). */
void ui_picture(int x, int y, const u16 *src, int w, int h, int pitch);
/* Area-averaged scaling down (or the same size). */
void ui_scale(u16 *dst, int dw, int dh, const u16 *src, int sw, int sh, int pitch);

/* The paused game, kept when the menu opens (r16: as it was, 240 x 160, rows
 * 240 apart: the paused menu shows it pixel for pixel). */
void ui_backdrop_make(const u16 *frame, int pitch);
void ui_backdrop_clear(void);
/* The white ground and the paused game in its middle. */
void ui_backdrop_draw(void);
/* The paused game's picture (240 x 160, pitch 240), or NULL when there's none. */
const u16 *ui_backdrop_picture(void);
/* A picture (sw x sh, rows pitch apart) scaled to dw x dh at x, y; grey: in
 * black and white (a game that isn't picked). */
void ui_picture_fit(int x, int y, int dw, int dh, const u16 *src, int sw, int sh, int pitch, int grey);

/* A game's file name made readable: "Legend of Zelda, The - The Minish Cap
 * (USA) (Rev 1).zip.tns" -> title "The Legend of Zelda: The Minish Cap",
 * tags "USA, Rev 1" (kept apart, not shown on the list). */
void ui_game_title(const char *file_name, char *title, size_t title_size, char *tags, size_t tags_size);

/* When something happened, in words: then and now in seconds (UTC, as the
 * file system gives them). style 0 (the game list): "Just now", "5 min ago",
 * "2 hours ago", "Yesterday", "3 days ago", "Last week", "Oct 14"; style 1
 * (the slot strip, narrow): "Now", "5 min", "2 h", "Mon", "Oct 14". "" when then isn't known (0), is before 2020 or is after now. */
void ui_when(long then, long now, int style, char *out, size_t size);

#endif
