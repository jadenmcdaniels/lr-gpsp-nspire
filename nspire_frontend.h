/* What the Nspire frontend's files share: messages and counters over the
 * picture (nspire_video_present.c), frame timing (nspire_host_main.c), and
 * the settings the QoL features added (gui.c). */
#ifndef NSPIRE_FRONTEND_H
#define NSPIRE_FRONTEND_H

#include <stddef.h>

/* ---- Text over the picture ---- */

enum { OSD_WHITE, OSD_GOOD, OSD_WARN, OSD_BAD };

/* A message at the bottom of the screen for two seconds. */
void nspire_osd_message(const char *format, ...) __attribute__((format(printf, 1, 2)));
void nspire_osd_clear(void);
/* DETECT: the message on screen now (NULL: none), and that one kept two
 * more seconds from now (a progress message kept up while something comes
 * up: no new toast, nothing logged). */
const char *nspire_osd_visible(void);
void nspire_osd_extend(void);
u16 nspire_osd_color(int which);
/* R11w part 2 (2026-11-15): the message line's one rule (nspire_osd.c).
 * A message's kind from its words: OSD_WAIT ends in "...", OSD_ACTION asks
 * the user to do something (Unplug, Plug, Swap, Put, Try, ... Controller
 * status), else OSD_INFO. Its time on screen: INFO 2.5 s, WAIT 2.5 s from
 * the last keep (nspire_osd_extend), ACTION 5 s, or less if its state
 * changed (nspire_osd_withdraw). A newer, less important message (any but an
 * ACTION over an ACTION) doesn't push one out before its time: it waits
 * (one, at most OSD_PENDING_MS old) and shows when the line is free. */
enum { OSD_INFO, OSD_WAIT, OSD_ACTION };
#define OSD_INFO_MS    2500
#define OSD_WAIT_MS    2500
#define OSD_ACTION_MS  5000
#define OSD_PENDING_MS 5000   /* (an action's time: what waited for it shows after it) */
#define OSD_TEXT_MAX   64
int nspire_osd_kind(const char *text);
unsigned nspire_osd_hold_ms(int kind);
int nspire_osd_may_replace(int shown_kind, unsigned shown_ms, int new_kind);
struct osd_line
{
  char text[OSD_TEXT_MAX];
  int kind, on;
  unsigned at_ms, until_ms;
  char pend[OSD_TEXT_MAX];
  int pend_kind, pend_on;
  unsigned pend_ms;
};
/* The line's state machine (times in ms): post (1 shown now, 0 waiting),
 * what is shown now (NULL none), keep a shown one up, take one down. */
int osd_line_post(struct osd_line *l, const char *text, unsigned now_ms);
const char *osd_line_visible(struct osd_line *l, unsigned now_ms);
void osd_line_extend(struct osd_line *l, unsigned now_ms);
void osd_line_withdraw(struct osd_line *l, const char *text);
/* nspire_video_present.c's line: a message whose state is over (a
 * controller connected after "USB not working"...) taken down now. */
void nspire_osd_withdraw(const char *text);
/* The FPS counter ("58/60") and its color, and the detailed line that
 * replaces it; empty strings show nothing. Set by the main loop. */
extern char nspire_fps_text[64];
extern u32 nspire_fps_full;   /* full speed's game frames a second: 60, fast forward's top speed (0 at max) */
extern u16 nspire_fps_color;
extern char nspire_perf_text[80];

/* Text in gpSP's 6x10 font (font.h) into any 16-bit buffer, without the
 * menu's clipping; it stops before max_x. Returns the x after the text. */
#define OSD_FONT_W 6
#define OSD_FONT_H 10
/* The bands over the picture (nspire_osd.c): 13 rows are what the screen
 * has beside a 3:2 picture. */
#define OSD_BAND_H 13
#define OSD_FONT_BASE 10   /* the text's baseline in the band */
u32 nspire_osd_width(const char *text);
void nspire_osd_text(u16 *band, u32 x, const char *text, u16 color, u32 max_x);
void nspire_osd_band(u16 *band, const char *left, u16 left_color, const char *centered, u16 centered_color,
                     const char *right, u16 right_color);

void nspire_present_frame(void);
void nspire_video_mix_reset(void);
/* Before a frame that will be drawn: with interframe blending on the
 * blended pictures, the core draws into the other of two frame buffers, so
 * the one drawn last stays as the previous frame (nothing is copied). */
void nspire_video_frame_start(void);
/* The pictures blended since the last call and how many game frames apart
 * (frameskip: the last drawn frame), one line for the log; counts from 0
 * again. nspire_blend_gap: the last picture's gap (0: not blended). */
void nspire_video_blend_report(char *text, u32 size);
extern u32 nspire_blend_gap;
/* Something other than a game frame went into the screen buffers (a menu):
 * the text bands are drawn again. */
void nspire_present_forget_bands(void);

/* nspire_blend.c: the blended pictures, in one pass, turned (portrait LCDs).
 * src is 240x160, GBA_SCREEN_PITCH pixels a line; with prev (the previous
 * frame, only read), frames are blended with it (interframe blending). */
void nspire_blend_turn_aspect(u16 *dst, const u16 *src, const u16 *prev);
void nspire_blend_aspect_landscape(u16 *dst, const u16 *src, const u16 *prev);
void nspire_blend_turn_full(u16 *dst, const u16 *src, const u16 *prev);
/* Their parts (speed10, nspire_reuse.c's pictures of the groups that
 * changed): groups first to first + n - 1 (source lines 3 first on), the
 * last source line. */
void nspire_blend_aspect_groups(u16 *dst, const u16 *src, const u16 *prev, u32 first, u32 n);
void nspire_blend_aspect_last(u16 *dst, const u16 *src, const u16 *prev);
void nspire_blend_landscape_groups(u16 *dst, const u16 *src, const u16 *prev, u32 first, u32 n);
void nspire_blend_landscape_last(u16 *dst, const u16 *src, const u16 *prev);
/* The 3:2 picture (portrait: nspire_blend_turn_aspect, else
 * nspire_blend_aspect_landscape) without the groups dst holds already
 * (nspire_reuse.c); nspire_reuse_pictures_forget: every screen buffer's
 * groups unknown (they were written by something else). */
void nspire_reuse_picture(u16 *dst, const u16 *src, const u16 *prev, int portrait);
void nspire_reuse_pictures_forget(void);

/* nspire_turn.c, besides the ones in nspire.h */
void nspire_turn_1x(u16 *dst, const u16 *src, u32 src_pitch);
void nspire_turn_rect_565(u16 *dst, const u16 *src, u32 src_pitch,
                          u32 x0, u32 y0, u32 w, u32 h);

/* ---- Timing ---- */

/* Ticks (nspire_ticks) spent drawing scanlines, counted while
 * nspire_perf_on is set (nspire_render_time.c). */
extern u32 nspire_render_ticks;
extern int nspire_perf_on;

/* Starts frame timing over (after the menu, loading, saving...). */
void nspire_reset_timing(void);

/* ---- Settings added for the QoL features (gui.c) ---- */

void nspire_save_pick(int slot);   /* the Save line's own pick (-1: none) */
extern u32 nspire_auto_increment;   /* saving goes to a new slot */
extern u32 nspire_save_on_exit;     /* leaving a game saves a state first */
extern u32 nspire_load_on_start;    /* starting a game loads its newest state */
/* Autosave (nspire_host_main.c): a state every nspire_autosave_seconds[
 * nspire_autosave] seconds of play (0: off), to a new slot after the
 * highest or to the picked slot (nspire_autosave_new_slot). */
#define AUTOSAVE_CHOICES 10
extern u32 nspire_autosave;
extern u32 nspire_autosave_new_slot;
/* The menus' colours: one of nspire_ui.h's UI_THEMES themes (Settings, Picture, Colours). */
extern u32 nspire_ui_theme;
extern const u32 nspire_autosave_seconds[AUTOSAVE_CHOICES];
extern const char *const nspire_autosave_names[AUTOSAVE_CHOICES];
/* The time to the next autosave starts over (a save, a load, a new game). */
void nspire_autosave_restart(void);
extern u32 nspire_cpu_speed;        /* 0 normal, then the raised speeds */
extern u32 nspire_compress_saves;   /* new states and in-game saves are compressed */
/* ui18: Settings > Saving > Confirm delete: 1 (the default) the Saves place asks before it deletes states; 0 it
 * deletes them at once (the game's own save always asks). In the settings file as a bit of word 71
 * (nspire_config.c CONFIRM_DELETE_OFF): older builds read the file as before. */
extern u32 nspire_confirm_delete;

/* The loading screen (nspire_loading.c): the game's name and a bar for its
 * ROM (gba_memory.c reports through nspire_rom_progress), a line for what
 * comes after, or why it didn't load (waits for a key). */
void nspire_loading_start(const char *rom_path);
void nspire_loading_note(const char *text);
void nspire_loading_end(void);
/* fast17: the ROM load's parts (reads, inflate, polls, the bar) in
 * gpsp_states_time.txt.tns, after "ROM load ..." */
void nspire_loading_rom_note(void);
void nspire_loading_failed(const char *why);
/* A zip that didn't load because it doesn't fit in memory (gba_memory.c's
 * nspire_rom_load_need_mb) becomes a .gbz of it, once, on the loading
 * screen with its bar: 1 when it did (gbz_path has it; the zip is gone,
 * its place in the folder's order is the .gbz's), 0 when it can't or
 * didn't (nspire_rom_load_error says why; nothing changed), -1 when Q
 * cancelled it (nothing changed). */
int nspire_loading_zip_to_gbz(const char *zip_path, char *gbz_path, size_t size);
/* Q on the loading screen: a new press since loading started (polls the
 * keys). Once it's said yes it keeps saying yes until the next start. */
int nspire_loading_cancelled(void);
/* The ROM load stopped because the player cancelled (gba_memory.c). */
extern int nspire_rom_load_cancelled;
/* A game that stopped (nspire_bad_jump, cpu.h): its name and why, on the
 * loading screen's layout, until a key is pressed. */
void nspire_game_stopped(const char *why);
/* Why the last ROM didn't load, when gba_memory.c can say (else NULL). */
extern const char *nspire_rom_load_error;
/* Set when a .gbz page wouldn't read or unpack during play (ZIP-2): the game
 * is stopped after the frame. */
extern u32 nspire_rom_page_damaged;
/* It was a zip too big to unpack: the MB it needs and the MB free (else 0). */
extern u32 nspire_rom_load_need_mb, nspire_rom_load_free_mb;

/* The Saves place (UI17, nspire_save_manager.c): a game's states and its own
 * save as a grid; load, mark, compress, uncompress, delete, copy to or from
 * a USB drive. rom_path: the game's (NULL: the paused game's); *slot: the
 * slot to pick first (the paused game's; -1 none). 1: a state was picked
 * to load (*slot; *on_drive: the USB drive's), 0: left with esc. */
int nspire_saves_place(const char *rom_path, int *slot, int *on_drive);
/* r19: the paused game's saves as the top row's SAVES (tabs: the row's words, on: which one this is; nav 1: the
 * arrows start on the row; 0: on its first tile, Resume; 2: on the tile it was last left on). Tiles: Resume,
 * Save, the game's own save, each state. Returns how it was left: */
enum
{
  SAVES_TAB_ESC,              /* esc or the menu key, on the row or in the tiles: back to the game */
  SAVES_TAB_LEFT,             /* an arrow off the row's word: the word before, or after */
  SAVES_TAB_RIGHT,
  SAVES_TAB_RESUME,           /* enter on Resume */
  SAVES_TAB_SAVE,             /* Save (*slot -1), or s on a state (*slot: over it); *on_drive: the place shown */
  SAVES_TAB_LOAD,             /* enter on a state: *slot, *on_drive */
  SAVES_TAB_LOAD_GAME_SAVE    /* enter on the game's own save */
};
int nspire_saves_tab(const char *const *tabs, int count, int on, int nav, int *slot, int *on_drive);
/* What the place's band says when it opens next (once): what a save or load from it just did. */
void nspire_saves_status(const char *text);
/* Two paths of one ROM file (separators and case aside). */
int nspire_same_rom_path(const char *a, const char *b);
/* The folder's games, compressing them into .zip files and back
 * (nspire_rom_manager.c; main menu > ROM manager). */
void nspire_rom_manager(void);
/* Closes the running game's ROM file if the whole ROM is in memory
 * (gba_memory.c): 1 then, 0 if the game still reads pieces of it. */
int nspire_rom_file_release(void);

/* Set when the file browser started a game with shift+enter: its newest
 * state isn't loaded that time. */
extern int nspire_skip_state_load;
/* UI17: the slot the next game's start loads (the Saves place's pick; on_drive: the USB drive's), -1: as
 * Resume last save says. nspire_start_game puts it back to -1. */
extern int nspire_start_slot, nspire_start_slot_on_drive;
/* Its picture for the loading screen (STATE_PIC_W x STATE_PIC_H), and whether there is one (-1: not set, the
 * newest state's is shown); nspire_loading_start takes it once. */
extern u16 *nspire_start_picture;
extern int nspire_start_picture_has;

/* The game's own settings and keys (nspire_config.c): opened before its ROM
 * loads, written with the settings and when it closes. */
void nspire_config_open_game(const char *rom_path);
void nspire_config_close_game(void);
/* As close, without writing the game's settings file (a cancelled start). */
void nspire_config_drop_game(void);
s32 nspire_config_load(void);
s32 nspire_config_save(void);
/* The last load found the settings in the program file. */
int nspire_config_in_program(void);
/* The menu's "Settings for" / "Keys for" (0 all games, 1 this game), and
 * what to do when they change. */
extern u32 nspire_settings_for;
extern u32 nspire_keys_for;
void nspire_config_settings_for_changed(void);
void nspire_config_keys_for_changed(void);

/* Loads and starts a game: its own settings, the ROM, the in-game save and
 * its newest state if wanted. Returns 0 if the ROM couldn't be loaded. */
int nspire_start_game(const char *rom_path);
/* The last nspire_start_game returned 0 because Q cancelled it. */
extern int nspire_start_cancelled;
/* Before leaving a game (another game, exit): saves a state if wanted,
 * writes the in-game save and the game's settings. */
void nspire_leave_game(void);
/* How the game list was left without a game (load_file returned -1): 1 by esc (which says "Quit" on
 * the first screen), 0 by the menu key (the menu without a game). */
extern int nspire_list_left_by_esc;
/* What leaving does about saving: 1 save, -1 don't (the quit question's answer), 0 Save when I quit. */
extern int nspire_exit_save;
/* Places other branches fill in; the menus work without them. Cheats' "Add
 * a code" (weak: absent, the page says where codes come from); the link's
 * speed (Advanced > Emulator: 0 Auto, 1 Compatible). */
void nspire_cheats_add_code(void) __attribute__((weak));
extern uint32_t nspire_link_speed;   /* nspire_link_speed.c (nspire_link.h) */
/* The Controls screen: the keys on a drawing of the keypad, or of the
 * controller (nspire_controls.c). */
void nspire_controls_screen(void);
/* PAD3: the Controller status page (nspire_padpage.c): the USB port, the
 * OS and the controller driver, live, in plain words. */
void nspire_controller_page(void);
/* The first start's "The keys" screen (nspire_controls.c), once: before the
 * first game's first frame. */
void nspire_controls_hint(void);
/* The PC tests switch the first-run hints off (host_platform.c, unless
 * GPSP_HOST_HINTS=1); the calculator has no such function: always on. */
int nspire_first_run_hints(void) __attribute__((weak));
/* Leaves the game, writes the settings and exits gpSP. */
void nspire_quit_game(void);

/* Save state helpers used by the menu and the hotkeys; they show messages. */
int nspire_save_state_now(void);
int nspire_load_state_slot(int slot);
/* The slot the next save goes to, or -1 if every slot is used. */
int nspire_save_target(void);

/* The speed test (Graphics options). */
void nspire_speed_test(void);

#endif
