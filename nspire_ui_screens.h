/* The menus' screens, drawn with the kit (nspire_ui.h): the game list, the
 * in-game menu beside the paused game, the settings pages, questions, the
 * loading screen. gui.c keeps what the menus do and fills in these views;
 * this only draws them. Fields marked "new" are the design's (the previews
 * fill them; zero draws what gui.c gives today). */
#ifndef NSPIRE_UI_SCREENS_H
#define NSPIRE_UI_SCREENS_H

#include "nspire_ui.h"

/* ---- The game list (UI17: the store) ----
 * The games as a shop shows what it sells: a grid of three by two tiles, each its game's latest save's picture
 * (96 x 64), its name and its facts under it ("4 SAVES · 2 H", "NOT PLAYED"); the picked one in colour with its
 * name and facts in a black plate under it, the others black and white prints. v (list) shows the games a line
 * each instead, ten to a screen (the name, its facts at the right): for a long list. */

#define UI_LIST_ROWS 6      /* games on the screen at once: the grid, three by two */
#define UI_LIST_LINES 10    /* the same as lines (v): the body's ten */
#define UI_LIST_MAX UI_LIST_LINES
#define UI_TILE_W 96        /* a tile's picture: 2/3 of a state's (STATE_PIC_W x STATE_PIC_H) */
#define UI_TILE_H 64
#define UI_LIST_NAME_W UI_TILE_W   /* a name's room under its picture: longer ends in "..." (the picked one scrolls) */
#define UI_FOLDER_ROWS UI_LIST_ROWS   /* folders on the screen at once (gui.c scrolls both by UI_LIST_ROWS) */
#define UI_MANAGER_ROWS 9   /* the ROM manager's lines (UI_LINE each, from UI_TOP) */

struct ui_list_game
{
  const char *file;       /* file name, NULL past the end */
  const u16 *picture;     /* its latest save's picture (144 x 96), NULL while not looked up or when none */
  int looked_up;          /* 1: picture and facts are all there is (NULL: never saved) */
  char facts[24];         /* "4 saves", "Not played", or "" while not looked up */
  char age[12];           /* when the last one was ("2 h", "Wed", "Nov 9"), or "" */
  u8 folder;              /* r19: a folder among the items (file: its name; ".." the way up): no picture */
};

/* The picked game's details, looked up when it stays picked (gui.c): shown
 * beside the names. */
struct ui_list_details
{
  int ready;
  int states, newest;     /* newest: slot, -1 for none */
  int has_picture;
  u16 picture[144 * 96];  /* STATE_PIC_W x STATE_PIC_H: its latest save's */
  long bytes;
  char facts[32];         /* when it was last saved ("2 days ago"), "Not played yet", or "" */
};

struct ui_list_view
{
  const char *folder;           /* "gba" */
  struct ui_list_game rows[UI_LIST_MAX];
  int picked_row;               /* among rows, -1 when on the folders */
  int marked_row;               /* the game picked while on the folders, or -1 */
  int games;                    /* in the folder */
  int first;                    /* the first row's place in the list */
  int moving;                   /* the picked game is being moved */
  int lines;                    /* UI17: the games a line each (v), not the grid */
  const struct ui_list_details *details;
  /* folders, shown instead of the details while picked */
  const char *const *dirs;
  int dir_count, dir_first, dir_picked;   /* dir_picked -1: not on them */
  const char *status;           /* what the last key did, or "" */
  unsigned picked_ms;           /* how long the picked game has been picked: its name scrolls when too long
                                   (ui_line_text); 0 while it isn't (ui_list_scrolls) */
  int resume;                   /* the list was opened from a game's menu: esc is "Back", not "Quit" */
  int has_usb;                  /* USB drive browser is built and available (u opens it) */
  /* r19: the list is two of the top row's tabs (Games, Folders), under the same row as every top-level screen */
  const char *const *tabs;      /* the row's tabs */
  int tab_count, tab_on;        /* how many, and the one this is */
  int nav_focus;                /* the arrows are on the row: nothing under it has the block */
  int folders;                  /* (unused since the folders are items of the grid) */
  const char *path;             /* where this folder is ("/documents/gba"): the band says it on a folder's tile */
  int items;                    /* r19: the games and, after them, the folders: all the grid's items */
};

void ui_list_draw(const struct ui_list_view *v);
/* Whether the last screen drawn (a list, a menu, a list page) has a picked name too long for its place: it
 * scrolls with picked_ms, so the screen is drawn again while it does. One flag for all of them (one screen is
 * drawn at a time). */
extern int ui_menu_scrolls;
#define ui_list_scrolls ui_menu_scrolls
#define ui_list_page_scrolls ui_menu_scrolls

/* ---- The save manager and the ROM manager ---- */

struct ui_manager_row
{
  char label[64];
  char size[16];
  u8 marked;
};

struct ui_manager_view
{
  const char *title;            /* "Save manager" */
  const char *name;             /* one word for it, for the tests (ui_shown) */
  char subtitle[64];            /* "17 files, 1.2 MB" */
  struct ui_manager_row rows[UI_MANAGER_ROWS];
  int count, first, picked;     /* the list's length, its first row, picked */
  int marked;                   /* how many are marked */
  const char *empty;            /* said when there's nothing */
  /* beside it: the picked one (or the marked ones) */
  const u16 *picture;           /* UI_TILE_W x UI_TILE_H, or NULL */
  const char *pic_empty;
  char head[96];
  char facts[2][32];            /* its facts, shown on one line: "14 KB", "Compressed" */
  const char *const *actions;   /* key, what it does, ...; NULL-ended */
  const char *status;           /* what the last key did, or "" */
  int warn;                     /* the status asks something (delete?) */
  int percent;                  /* work under way, 0-100, or -1 */
  unsigned picked_ms;           /* how long the picked line has been picked: a name too long scrolls (motion, spec 5) */
};

void ui_manager_draw(const struct ui_manager_view *v);

/* ---- The Saves place (UI17, nspire_save_manager.c): a game's states and its own save ----
 * The same grid as the store: each state a tile, its picture, "SLOT 7" and when, its size (and "packed" when
 * compressed); the game's own save the first tile. Marked tiles have a black box at their picture's corner.
 * The header: the game, then SAVES; at the right the totals, or the places as tabs ("tab CALCULATOR  USB
 * DRIVE") while a drive can hold saves. The band: what just happened, else the keys. */
#define UI_SAVES_TILES 6

struct ui_save_tile
{
  const u16 *picture;     /* UI_TILE_W x UI_TILE_H, or NULL */
  char name[16];          /* "Slot 7", "Game save" */
  char when[12];          /* "2 h", "Mon", "Now", or "" */
  char facts[24];         /* "13 KB packed", "416 KB" */
  u8 marked;
  u8 game_save;           /* the game's own save (no picture: its word instead) */
  u8 action;              /* r19: not a save: UI_SAVE_RESUME (the paused game, its picture as it stands) or
                             UI_SAVE_NEW (a "+" where a picture is: enter saves) */
  char band[48];          /* r19: what the band says while it is picked ("2 H AGO . 13 KB") */
};
#define UI_SAVE_RESUME 1
#define UI_SAVE_NEW 2

struct ui_saves_view
{
  const char *game;             /* its name, the header's first words */
  char note[40];                /* "7 saves, 1.2 MB", "3 marked, 39 KB" (without places) */
  const char *places[2];        /* the places as tabs (place_count 2), or none */
  int place_count, place;
  struct ui_save_tile tiles[UI_SAVES_TILES];
  int count, first, picked;     /* all of them, tiles[0]'s place among them, the picked one's */
  const char *empty;            /* said when there is nothing */
  const char *const *keys;      /* the band's keys, pairs, NULL-ended (those that fit are said, in order) */
  const char *status;           /* what just happened, or "" */
  int warn;
  unsigned picked_ms;           /* a name too long scrolls */
  /* r19: the paused game's saves are a tab of the top row (SAVES): the row instead of the path */
  const char *const *tabs;      /* the row's words, or NULL: a page of its own (the path at the top) */
  int tab_count, tab_on;
  int nav_focus;                /* the arrows are on the row: no tile has the block */
};

void ui_saves_draw(const struct ui_saves_view *v);

/* ---- The speed test's results ---- */

struct ui_speed_part
{
  const char *name;
  int fast_forward;
  /* tenths: frames a second, and ms a frame */
  unsigned game_fps, drawn_fps, cpu_ms, gfx_ms, out_ms;
};

struct ui_speed_view
{
  unsigned mhz;
  int stopped;
  struct ui_speed_part parts[4];
  int count;
};

void ui_speed_draw(const struct ui_speed_view *v);

/* ---- A question or a message over the screen behind it ---- */

struct ui_dialog_view
{
  const char *title;            /* "Replace saved state 3?" */
  const char *text;             /* what happens, what to do: 1-3 short lines */
  const char *first, *second;   /* buttons ("Cancel", "Replace"); second may be NULL */
  int picked;                   /* 0: first (the safe one, by default) */
  int warn;                     /* something went wrong: the warning icon before the title */
  int destructive;              /* new: the second button destroys something (in red) */
  int no_dim;                   /* the screen behind is dimmed already (a redraw of the dialog alone) */
};
/* Draws over what is in the frame (the screen it asks about). */
void ui_dialog_draw(const struct ui_dialog_view *v);

/* ---- Menus ---- */

enum ui_row_kind
{
  UI_ROW_ACTION,      /* does something */
  UI_ROW_SUBMENU,     /* opens a page */
  UI_ROW_PICK,        /* a value picked with left/right */
  UI_ROW_SWITCH       /* off or on */
};

struct ui_row
{
  char label[48];
  char value[40];         /* PICK */
  u8 kind;
  u8 on;                  /* SWITCH */
  u8 gap_before;          /* a divider above it (main menu) */
  u8 tile;                /* ui18: UI_TILE_WIDE, UI_TILE_FIXED (a settings page's tiles) */
  u8 glyph;               /* ui18: the paused menu's Settings: the page's glyph (UI_GLYPH_*) */
};

#define UI_MAX_ROWS 32

/* ---- Settings as tiles (ui18) ----
 * Every settings page's options as the store's tiles (r17's game grid): each its name small (capitals) over its
 * value large (the large capitals), in a grid of as many columns as fit every value the page can show whole (3
 * or 2, the store's x 8 / 112 / 216 or 8 / 164); a tile whose name or value would be cut there takes a row of its
 * own (UI_TILE_WIDE). The picked one is the store's: its words white on a black plate 4 px wider than the tile;
 * a value being changed is between chevrons. A tile that opens a page or a screen has an arrow after its name.
 * Rows of UI_TILE_PITCH from UI_TOP, UI_TILE_ROWS of them on the screen; more scroll a row at a time (a bar at
 * the right edge, as the store's).
 * The arrows: left and right the tile before or after, in order (across rows, wrapping, as the Saves place's);
 * up and down the row above or below, the same column (a shorter row: its nearest), wrapping (as the store's);
 * tiles that say something (UI_TILE_FIXED) are passed. */
#define UI_TILE_WIDE  1     /* takes a row of its own: its name or a value doesn't fit a column */
#define UI_TILE_FIXED 2     /* says something, never picked (the Controller page's "USB port") */
#define UI_TILE_OPENS 4     /* opens a page or a screen: an arrow after its name */
#define UI_TILE_PITCH 45    /* a row of tiles: a tile 29 high (UI_TILE_BLOCK), 16 between */
#define UI_TILE_BLOCK 29    /* a tile: its words, the picked one's plate */
#define UI_TILE_ROWS 4      /* on the screen at once: UI_TOP to row 216 */
#define UI_TILES_MAX UI_MAX_ROWS
/* The most columns (3, 2, or 1: only a row of its own) a tile with this name and value fits whole; editable: the
 * value is changed in place (its chevrons beside it). */
int ui_tile_fit(const char *name, const char *value, int editable);
/* Where tile i is: its row, its column, how many columns it takes; returns the rows. */
int ui_tiles_place(const u8 *flags, int count, int cols, u8 *row, u8 *col, u8 *span);
/* The tile picked after an arrow from picked: dx -1 / 1 (left, right), or dy -1 / 1 (up, down); flags[i] the
 * tiles' UI_TILE_* (count of them, in cols columns). */
int ui_tile_step(const u8 *flags, int count, int cols, int picked, int dx, int dy);

/* new: a save slot in the in-game menu's carousel (Save, Load) */
struct ui_slot
{
  const u16 *picture;     /* STATE_PIC_W x STATE_PIC_H, or NULL */
  char name[16];          /* "Slot 3", "New", "In-game save" */
  char when[16];          /* right of name on the tile, or "" */
  char when_long[20];     /* caption under the strip: "2 min ago", "Yesterday" */
  char facts[20];         /* second line on the tile ("2 min ago", "Slot 4", "Battery save") */
  u8 game_save;           /* 1: the game's own in-game save (UI_ICON_SAVE) */
};

/* new: the Trade / Link card's states */
enum ui_link_state
{
  UI_LINK_OFF = 1,        /* nothing plugged in */
  UI_LINK_PAD,            /* a controller is plugged in */
  UI_LINK_FOUND,          /* the other calculator answers */
  UI_LINK_LINKED,
  UI_LINK_SWAP,           /* the cable's ends the wrong way round */
  UI_LINK_LOOKING         /* this one waits for the other to pick it too */
};

struct ui_menu_view
{
  int page;                     /* -1: the main menu, else a settings page */
  const char *const *tabs;      /* tabs[page]: the page's name, for the tests (ui_shown) */
  int tab_count;                /* 0: pages have no tabs; the name is in title */
  const char *title;            /* the game (main menu), or the page's name */
  struct ui_row rows[UI_MAX_ROWS];
  int count, picked;
  const char *help;             /* the picked line's ('\n' is a space) */
  const char *status;           /* what the last action did, or "" */
  int editing;                  /* the picked line is being changed */
  int about;                    /* the About page: credits, no rows */
  /* main menu: the picture beside it, and what it is */
  int no_game;
  const u16 *picture;           /* STATE_PIC_W x STATE_PIC_H, or NULL */
  const char *pic_empty;        /* said instead when there's none */
  char pic_head[48];
  char pic_sub[64];
  int version;                  /* changes with what picture points at */
  /* ui18 part 2: Save and Load's stationary carousel of save tiles */
  struct ui_slot slots[3];      /* visible tiles slots[0..slot_count-1] */
  int slot_count;               /* 0..3 tiles drawn */
  int slot_picked;              /* index in slots[] currently highlighted */
  int slot_first;               /* index of slots[0] in the full carousel */
  int slot_total;               /* total tiles in the carousel */
  int strip_focus;              /* 1: arrows move in the strip, 0: on the tabs */
  int nav_focus;                /* r19: the arrows are on the top row's tabs (rows[]: picked has the block); 0: in
                                   the body under them (its picked thing has it) */
  int link;                     /* the Trade / Link card (enum ui_link_state), 0 none */
  int player;                   /* linked: 1 or 2 */
  char link_note[96];           /* what the link last said (why it couldn't start or stopped), or "" */
  /* the first line with another game picked on it (up/down); title stays the paused game's */
  int other;                    /* 0: the paused game; else which of the others (1: the last one played before it) */
  const u16 *other_picture;     /* its latest save's picture (STATE_PIC_W x STATE_PIC_H), or NULL */
  char other_title[160];        /* its name */
  int other_count;              /* how many others there are */
  char other_facts[32];         /* "2 days ago", "Not played yet" */
  /* a page: the page it is opened from ("Settings"), said before its title; NULL for none */
  const char *from;
  /* ui18: a page's tiles: in cols columns (2 or 3; rows[].tile says which take a row of their own) */
  int cols;
  /* ms since the picked cell (or its value) last changed: a name or value too long for its cell scrolls in it
   * (ui_scroll_after); 0 while nothing does (ui_menu_scrolls), so an unchanged page isn't drawn again */
  unsigned picked_ms;
  /* r19: Settings, the one screen under the top row: a page drawn with the row where a page has its path
   * (nav: the row's words, nav_count of them, nav_on the page's own; nav_focus 1: the arrows are on the row and
   * no tile is picked, picked -1). NULL: a page with a path. */
  const char *const *nav;
  int nav_count, nav_on;
  /* r16: Save and Load where saves can be in two places (a USB drive): the places as tabs
   * over the strip (places[place] on; place_count < 2: none), and the keys there, at its right ("c", "Copy to
   * USB drive"; NULL-ended, at most one pair shown beside the tabs' own "tab") */
  const char *places[2];
  int place_count, place;
  const char *place_keys[3];
  /* UI17/ui18: Save and Load show carousel chips (m All saves, l Load, s Save) under the strip */
  int saves_key;
};

void ui_menu_draw(const struct ui_menu_view *v);
/* Whether the last page drawn has a picked cell whose text is too long for it (and scrolls with picked_ms). */
extern int ui_menu_scrolls;

/* The game is waiting for the other calculator (nspire_link_waiting): the
 * cable, what is going on, esc to unlink. */
void ui_link_wait_draw(int other_in_menu);

/* ---- A page that is a list (r16): the Bluetooth page, and any page of things found or kept ----
 * The header (from, title, note); the lines from UI_TOP, the whole width: each its name at the left (cut "...",
 * the picked one scrolling: ui_line_text), what it is (kind: "Controller", "Headphones", "Other", or a value
 * such as "On", "Connected"; grey) and, for something heard over the air, its signal as a score out of four
 * (signal 0-4; -1: none) at the right margin; a section's name in grey capitals (heading: never picked, up
 * and down pass it). The picked line a black block. Past UI_LINES lines it scrolls (first) with a scrollbar
 * on the right edge. The band: what just happened (status), else the picked line's one sentence (help), else
 * its keys (keys: pairs, NULL-ended; "!enter" says enter where it does something worth saying). Example
 * (the Bluetooth page's mock, tests/host/ui_wired.py scene "bluetooth"):
 *   BLUETOOTH                           On          <- a switch: enter flips it
 *   SEARCH                              Searching   <- a thing to do: enter starts or stops it
 *   PAIRED                                          <- heading
 *   DualSense Wireless Controller   Connected  ||||
 *   FOUND                                           <- heading
 *   Pro Controller                 Controller  |||.
 *   Example Buds                   Headphones  ||..
 *   AA:BB:CC:00:00:06                   Other  |...  <- a name not heard yet: its address
 */
#define UI_PAGE_LINES 24

struct ui_page_line
{
  char name[48];          /* its name, or its address until the name arrives */
  char kind[48];          /* what it is or what state it is in ("Controller", "Connected", "On"), or "" (at most
                             half the line: longer is cut, or scrolls while picked) */
  s8 signal;              /* 0-4 bars, -1 none */
  u8 heading;             /* a section's name: not picked, up/down pass it */
  u8 fixed;               /* a line that says something (the Controller page's "USB port  Working"): drawn as
                             the others, never picked (the page's up/down pass it) */
  u8 tile;                /* ui18: a tile (the page's first `tiles` lines): UI_TILE_WIDE, UI_TILE_OPENS */
};

struct ui_list_page_view
{
  const char *from, *title, *note;   /* the header: "SETTINGS  BLUETOOTH", a note at the right (or NULL) */
  const char *name;                  /* one word for it, for the tests (ui_shown "bluetooth:3") */
  struct ui_page_line lines[UI_PAGE_LINES];
  int count, picked, first;          /* first: the first line shown (the page scrolls it to keep picked shown) */
  const char *const *keys;           /* the band's keys for the picked line, NULL-ended pairs */
  const char *status;                /* what just happened, or NULL/"" */
  const char *help;                  /* the picked line's one sentence, or NULL */
  const char *empty;                 /* said in the body when count is 0 */
  unsigned picked_ms;                /* a name too long scrolls (ui_line_text) */
  /* ui18: the first `tiles` lines are tiles (a settings page's: name over value, in cols columns), the rest
   * the list under them (Bluetooth's devices); first then counts the list's lines only */
  int tiles, cols;
};

/* Returns the row under what it drew (the tiles' and the lines'): a page's own words go there. */
int ui_list_page_draw(const struct ui_list_page_view *v);
/* ui18: the line picked after an arrow on such a page (dx left/right, dy up/down): among the tiles as
 * ui_tile_step, down from their last row into the list, up from its first line to them; in the list up and
 * down, passing headings and fixed lines (left and right do nothing there). */
int ui_list_page_step(const struct ui_list_page_view *v, int picked, int dx, int dy);
/* The lines of the list under the tiles on the screen at once. */
int ui_list_page_lines(const struct ui_list_page_view *v);

/* ---- r19: Settings > Cheats: a line a code, its switch at the right (nspire_cheats.c) ---- */
#define UI_CHEAT_LINES UI_LINES
struct ui_cheats_view
{
  const char *note;                    /* the header's, at its right: "3 ON" */
  const char *names[UI_CHEAT_LINES];   /* the lines on the screen: shown of them, from the code `first` */
  u8 on[UI_CHEAT_LINES];
  int shown, first, picked, count;     /* picked: among all count codes */
  const char *status, *help;           /* the band: what just happened, else a sentence */
  unsigned picked_ms;                  /* a name too long scrolls while picked */
};
void ui_cheats_draw(const struct ui_cheats_view *v);
extern int ui_cheats_scrolls;

/* ---- Controls: the keys on a drawing of the calculator or the controller ---- */

struct ui_control_row
{
  const char *name;       /* "A", "Fast forward" */
  const char *mark;       /* its label on the key ("A", "START") */
  const char *mark_short; /* when the label doesn't fit the key ("ST") */
  int icon;               /* UI_ICON_* on the key when mark is NULL */
  u8 key, with;           /* its key (nspire_keys.h numbers; a combination: with held); 0 none */
  u8 key2;                /* its second key, or 0 */
  const char *key_text;   /* "del", "shift+S", "none" */
  u8 basic;               /* the first-run hint: 1 in its legend and on the drawing, 2 drawing only */
  const char *key_text2;  /* the second key's text, or NULL */
  u8 tkey, twith;         /* a GBA button's turbo key (and the key held with it); 0 none */
  const char *turbo_text; /* its turbo's key, "none", or NULL: a hotkey (no turbo) */
  const char *key_words;  /* R10b: the controller's button in words for the help line ("the Circle button
                             (right)"; the menu row: "PS, or Share and Options"), or NULL */
};

struct ui_controls_view
{
  int pad;                      /* the controller's view (keys are KEY_PAD_*) */
  int pad_connected;            /* a controller is in: left/right turn the views */
  const char *pad_name;         /* its name ("Xbox 360"), or NULL */
  const struct ui_control_row *rows;
  int count;                    /* "Reset to defaults" comes after them */
  int picked, first;            /* picked == count: Reset to defaults */
  int capturing;                /* waiting for the key for rows[picked] */
  int conflict_row;             /* -1, or the row that already has the key pressed */
  u8 new_key;                   /* the key pressed */
  const char *new_key_text;
  int conflict_choice;          /* 0: keep (focused first), 1: use it */
  int hint;                     /* the first-run hint: the basic keys only, no list */
  const char *band;             /* r19: the band's words for the picked row (its turbo key), or NULL */
  const char *help;             /* the one line under the list (NULL: said from the rows) */
  const char *note;             /* shown instead while capturing: why that key didn't count, or NULL */
  int turbo;                    /* capturing: the key is for the picked button's turbo */
  const char *target_name;      /* capturing: what the key is for ("A", "Turbo A"), NULL: the row's name */
  const char *conflict_name;    /* the action that has the key ("Turbo B"), NULL: conflict_row's name */
};

void ui_controls_draw(const struct ui_controls_view *v);

/* ---- Starting a game: loading, or making a .gbz of a zip ---- */

struct ui_loading_view
{
  const char *what;             /* "Starting", "Couldn't start it" */
  const char *title, *tags;     /* the game */
  const u16 *picture;           /* the state it resumes from, or NULL */
  int bar;                      /* a bar: done of total */
  u32 done, total;
  const char *line;             /* under it, or NULL */
  int warn;                     /* the line says what went wrong */
  const char *fact;             /* new: "Saved 4.2 MB" beside the bar, or NULL */
  const char *more;             /* "Back to the list" (any key), or NULL */
  int cancel;                   /* Q cancels */
};

void ui_loading_draw(const struct ui_loading_view *v);

#endif
