/* PAD3 (2026-11-11): the Controller status page (More settings > Controller
 * status): what this calculator's USB port, the OS and the controller
 * driver are doing, live, in plain words (nspire_padstatus.c), so a user
 * with no computer can read the cause off the screen. One screen, built
 * from the menus' kit (nspire_ui.h); expected to be restyled when the new
 * menus are merged.
 *
 * enter: restart the port's bus power (the arbiter's power cycle, by hand);
 * right: let the OS run until its host controller runs, 30 s at most (does
 * it start with time alone? R4: R9F's and r5's came up after +1280 and +1362
 * OS sleeps, 46-53 s, and not by +1000: 5 s could not tell); esc: back. Both say what came of it, and the pad log
 * (nspire_padlog.c) records them with the registers before and after. */
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "input.h"
#include "nspire.h"
#include "nspire_frontend.h"
#include "nspire_keys.h"
#include "nspire_os.h"
#include "nspire_ui_screens.h"
#include "nspire_bluetooth.h"
#include "nspire_usbpad_glue.h"
#include "nspire_arbiter.h"
#include "nspire_padlog.h"
#include "nspire_padstatus.h"
#include "nspire_bt_glue.h"
#include "nspire_drive.h"

#define OS_TIME_MS 30000   /* R4: 5 s was ~500 sleeps at most, under the threshold the logs suggest */
#define REFRESH_MS 250

/* Nine detail rows, followed by three lines of register values. */
#define LABEL_X UI_MARGIN
#define VALUE_X (UI_MARGIN + 84)
#define RIGHT   (UI_W - UI_MARGIN)
#define DETAIL_H 12

#ifdef NSPIRE_HOST
void host_usbpad_note(const char *text) __attribute__((weak));
#endif

/* R11w part 2: the page's top words: a state that asks the user to do
 * something is said once it has held PADSTATUS_STABLE_MS (a pulled adapter
 * passes through "the plain end" for a moment: R11w 419287) */
static enum pad_verdict top_v = PV_COUNT;
static u32 top_at;

static const char *top_words(enum pad_verdict v)
{
  /* (r19-usb) the controller off is the port with nothing in it too (a CX
   * II's OS powers an idle port down and up again: r19's gpsp_pad_prev, the
   * two in turn a few times a second): one state, not two taking turns */
  if (v == PV_UNREADABLE)
    v = PV_NOTHING;
  if (v != top_v)
  {
    top_v = v;
    top_at = nspire_ticks();
  }
  if ((nspire_padstatus_failing(v) || v == PV_DEVICE_END) &&
      (u64)(nspire_ticks() - top_at) * 1000 < (u64)PADSTATUS_STABLE_MS * NSPIRE_TICK_HZ)
    return PADSTATUS_CHECKING;
  return nspire_padstatus_plain(v);
}

/* (r19-usb) what the list view shows in words: its empty sentence and the one under the list (the tests) */
static char said_now[300];

/* The PC tests read the page from pad.txt, each time it says something new (either view). */
static void host_note(const struct pad_status *s, enum pad_verdict v, const char *note)
{
#ifdef NSPIRE_HOST
  if (host_usbpad_note)
  {
    static char said[1200];
    char label[PADSTATUS_LINES][PADSTATUS_LABEL], value[PADSTATUS_LINES][PADSTATUS_VALUE];
    char text[1200];
    unsigned n = (unsigned)snprintf(text, sizeof(text), "page: %s |", nspire_padstatus_name(v));
    int i;
    nspire_padstatus_lines(s, label, value);
    for (i = 0; i < PADSTATUS_LINES && n < sizeof(text); i++)
      n += (unsigned)snprintf(text + n, sizeof(text) - n, " %s: %s |", label[i], value[i]);
    if (n < sizeof(text))
      n += (unsigned)snprintf(text + n, sizeof(text) - n, " note: %s", note && note[0] ? note : "-");
    if (n < sizeof(text))
      n += (unsigned)snprintf(text + n, sizeof(text) - n, " | top: %s", top_words(v));
    if (n < sizeof(text) && nspire_bt_glue_status())
      n += (unsigned)snprintf(text + n, sizeof(text) - n, " | bt: %s", nspire_bt_glue_status());
    if (n < sizeof(text) && said_now[0])
      snprintf(text + n, sizeof(text) - n, " | said: %s", said_now);
    if (strcmp(text, said))
    {
      strcpy(said, text);
      host_usbpad_note(text);
    }
  }
#else
  (void)s;
  (void)v;
  (void)note;
#endif
}

/* The Details view: the port, the OS and the driver, live, the registers' raw words (whoever reads them off the
 * screen: a hardware log, a bug report). */
static void draw_details(const struct pad_status *s, enum pad_verdict v, const char *note)
{
  /* ("!enter": said in the band, where enter otherwise isn't: here it does something of its own) */
  static const char *const keys[] = { "!enter", "Restart USB power", UI_ARROW_R, "Give the OS 30 s", "esc", "Back" };
  char label[PADSTATUS_LINES][PADSTATUS_LABEL], value[PADSTATUS_LINES][PADSTATUS_VALUE];
  char raw[200], fit[100];
  char *word;
  int i, y = UI_TOP + UI_LINE_BASE;   /* the body's first line */

  nspire_padstatus_lines(s, label, value);
  ui_begin();
  ui_fill(UI_BG);
  ui_header_from("Settings/Controller", "Details", note);
  /* R11w part 2: the top says in plain words what is plugged in, whether it
   * works and what to do (a state that asks something only once it has
   * held PADSTATUS_STABLE_MS: before, "Checking the USB port..."); the
   * technical lines below (the cause's technical words are the pad log's) */
  if (nspire_drive_state() >= DRIVE_ATTACHED)
  {
    char drive[128];
    snprintf(drive, sizeof(drive), "Drive: %s", nspire_drive_summary());
    ui_wrap(LABEL_X, y, UI_LINE_H, RIGHT - LABEL_X, 2, drive, &ui_font_small_strong, UI_TEXT);
  }
  else if (nspire_bt_usb_active() && !nspire_usbpad_pads() && nspire_bt_glue_status())
    /* the Bluetooth dongle on the port, no USB controller: its word
     * ("Bluetooth: P1 DualSense", "Bluetooth: on, no devices") */
    ui_wrap(LABEL_X, y, UI_LINE_H, RIGHT - LABEL_X, 2, nspire_bt_glue_status(), &ui_font_small_strong, UI_TEXT);
  else
    ui_wrap(LABEL_X, y, UI_LINE_H, RIGHT - LABEL_X, 2, top_words(v), &ui_font_small_strong, UI_TEXT);
  y = UI_TOP + 2 * UI_LINE_H + 8;
  for (i = 0; i < PADSTATUS_LINES; i++, y += DETAIL_H)
  {
    ui_text(LABEL_X, y, label[i], &ui_font_small, UI_MUTED);
    ui_text(VALUE_X, y, ui_fit(value[i], &ui_font_small, RIGHT - VALUE_X, fit, sizeof(fit)), &ui_font_small,
            UI_TEXT);
  }
  /* Keep each register and its value together when wrapping the three lines. */
  nspire_padstatus_raw_text(s, raw, sizeof(raw));
  if (s->kind == 1 || s->kind == 2)
    for (word = raw; (word = strchr(word, ' ')) != NULL; )
    {
      *word++ = '=';
      word = strchr(word, ' ');
      if (!word)
        break;
      word++;
    }
  ui_wrap(LABEL_X, UI_HELP_BASE - 26, 13, RIGHT - LABEL_X, 3, raw, &ui_font_small, UI_MUTED);
  ui_footer(keys, 3);
  snprintf(ui_shown, sizeof(ui_shown), "controller:%s", nspire_padstatus_name(v));
  ui_present();
  said_now[0] = 0;
  host_note(s, v, note);
}

/* The page. r19: its four tiles (Controller: None, USB port: Empty, USB power: Restart, Details) were too much
 * for a calculator with nothing plugged in: four things for one controller, and the port's tile kept changing on
 * its own while nothing was done. So: ONE list,
 * a line a controller that is connected (its name, and USB or Bluetooth); with none, one sentence saying how to
 * get one. The port's state is not shown as a word any more (it changes while the port is looked at, and "Empty"
 * and "Controller is off" took turns); what the port asks of the player is still said in plain words, once it
 * has been the same for PADSTATUS_STABLE_MS. The two things to do are keys, said in the band: enter restarts
 * the port's power, D shows the details (the registers, for a report). */
static struct ui_list_page_view p;   /* the page as last drawn */
static void draw(const struct pad_status *s, enum pad_verdict v, const char *note)
{
  static const char *const keys[] = { "!enter", "Restart USB", "D", "Details", NULL };
  const char *top = top_words(v);
  char hub_words[240];
  int n = 0, sentence, end, k;

  memset(&p, 0, sizeof(p));
  p.from = "Settings";
  p.title = "Controller";
  p.note = note;
  p.name = "controller";
  /* USB's by their slots, then Bluetooth's (their toasts and lights number them in this order) */
  for (k = 0; k < 2 && n < 6; k++)
    if (nspire_usbpad_slot_name(k))
    {
      snprintf(p.lines[n].name, sizeof(p.lines[0].name), "%s", nspire_usbpad_slot_name(k));
      snprintf(p.lines[n].kind, sizeof(p.lines[0].kind), "USB");
      p.lines[n].signal = -1;
      p.lines[n++].fixed = 1;
    }
  for (k = 0; k < 2 && n < 6; k++)
    if (nspire_bt_glue_slot_name(k))
    {
      snprintf(p.lines[n].name, sizeof(p.lines[0].name), "%s", nspire_bt_glue_slot_name(k));
      snprintf(p.lines[n].kind, sizeof(p.lines[0].kind), "Bluetooth");
      p.lines[n].signal = -1;
      p.lines[n++].fixed = 1;
    }
  p.count = n;
  p.picked = -1;
  /* (r19-usb) No adapter's end seen (nothing, or the controller off: the ID
   * pin of every reading at the device's end): whatever is on the other end of
   * the cable, this calculator is not the host. A CX II log (r19, 958921d)
   * with a controller on a cable said "Plug one into the USB port" for 899 s. */
  if (!n && (v == PV_NOTHING || v == PV_UNREADABLE))
    p.empty = nspire_bt ? "No controller: no OTG adapter is in the USB port. A controller needs one (a plain cable "
                          "won't work), or pair one in Settings > Bluetooth." :
                          "No controller: no OTG adapter is in the USB port. Put the adapter in this calculator and the "
                          "controller in the adapter (a plain cable won't work).";
  else if (!n)
    p.empty = nspire_bt ? "No controller. Plug one into the USB port, or pair one in Settings > Bluetooth." :
                          "No controller. Plug one into the USB port.";
  /* (r19-usb) A hub with no controller on it: what a controller plugged in
   * and never listed means. A CX II log (r19, 926fa73): no port of the hub
   * read connected for a controller in any of the looks gpSP asked (every
   * 2 s) over minutes, while a Bluetooth dongle put back on another port
   * read connected at once: the hub never saw those controllers on its
   * wires. */
  if (v == PV_HUB && !n)
  {
    snprintf(hub_words, sizeof(hub_words), "%s %s", top,
             "One plugged in and still not listed after a few seconds isn't seen by the hub: use a data cable (not "
             "charge-only), or a hub with its own power.");
    top = hub_words;
  }
  /* what to do, when the port asks something: under the list, in plain words, once it has stopped changing */
  sentence = v != PV_WORKING && v != PV_NOTHING && v != PV_UNREADABLE && v != PV_LINKED && strcmp(top, PADSTATUS_CHECKING) &&
             (u64)(nspire_ticks() - top_at) * 1000 >= (u64)PADSTATUS_STABLE_MS * NSPIRE_TICK_HZ;
  /* (r19's screenshots: with no controller the port's own sentence is THE sentence: under "No
   * controller. Plug one into the USB port" it read as two instructions, the first one wrong for the case) */
  if (sentence && !n && nspire_drive_state() < DRIVE_ATTACHED)
  {
    p.empty = top;
    sentence = 0;
  }
  p.keys = keys;
  ui_defer_present = 1;
  end = ui_list_page_draw(&p);
  ui_defer_present = 0;
  if (!n)
    end = UI_TOP + 3 * UI_LINE_H;   /* (under the sentence above) */
  if (sentence && nspire_drive_state() < DRIVE_ATTACHED && (UI_FOOT_Y - 4 - end) / UI_LINE_H > 0)
    ui_wrap(UI_MARGIN, end + UI_LINE_H + UI_LINE_BASE, UI_LINE_H, RIGHT - LABEL_X, (UI_FOOT_Y - 4 - end) / UI_LINE_H - 1,
            top, &ui_font_small, UI_TEXT);
  else
    sentence = 0;
  snprintf(said_now, sizeof(said_now), "%s%s%s", p.empty && !n ? p.empty : "", p.empty && !n && sentence ? " / " : "",
           sentence ? top : "");
  snprintf(ui_shown, sizeof(ui_shown), "controller:%s", nspire_padstatus_name(v));
  ui_present();
  host_note(s, v, note);
}

static u32 ms_since(u32 at)
{
  return (u32)((u64)(nspire_ticks() - at) * 1000 / NSPIRE_TICK_HZ);
}

static void snapshot(struct pad_status *s, enum pad_verdict *v)
{
  keys_poll();   /* the pad driver's poll (its events, the arbiter's tick) runs here too */
  nspire_padstatus_read(s);
  *v = nspire_padstatus_verdict(s);
}

/* Right: the OS alone until its host controller runs, OS_TIME_MS at most
 * (its tasks run in our sleeps), the page live meanwhile; any key ends it
 * sooner. The pad log gets the sleeps and the time it took. */
static void give_os_time(char *note, unsigned size)
{
  struct pad_status s;
  enum pad_verdict v;
  u32 at = nspire_ticks(), drawn = 0, sleeps = nspire_os_sleeps();
  uint32_t w[NSPIRE_USB_RAW];
  unsigned steps = 0, elapsed;
  int ran = 0, first = 1;

  keys_wait_release();
  at = nspire_ticks();
  /* The fallback timer advances in frame waits, not in OS sleeps. Bound
   * this action by sleeps as well and keep checking keys when it stalls. */
  while ((!nspire_timer_runs() || ms_since(at) < OS_TIME_MS) && steps < OS_TIME_MS / 10)
  {
    if (!nspire_os_sleep())
      break;
    steps++;
    keys_poll();
    if (any_key_down())
      break;
    /* USBCMD's run bit from the words the page shows (w[2] for both kinds) */
    if (nspire_usb_raw(w) && (w[2] & 1))
    {
      ran = 1;
      break;
    }
    if (first || ms_since(drawn) >= REFRESH_MS || steps % (REFRESH_MS / 10) == 0)
    {
      char text[48];
      first = 0;
      drawn = nspire_ticks();
      snapshot(&s, &v);
      elapsed = nspire_timer_runs() ? ms_since(at) : steps * 10;
      snprintf(text, sizeof(text), "the OS runs: %u s left",
               elapsed < OS_TIME_MS ? (OS_TIME_MS - elapsed + 999) / 1000 : 0);
      draw_details(&s, v, text);
    }
  }
  keys_wait_release();
  elapsed = nspire_timer_runs() ? ms_since(at) : steps * 10;
  nspire_padlog_add(PL_USER, PL_USER_OS_TIME, 0, 0, nspire_os_sleeps() - sleeps, (u32)ran, elapsed);
  snprintf(note, size, "after %u s: the host controller %s", (elapsed + 500) / 1000, ran ? "ran" : "never ran");
}

void nspire_controller_page(void)
{
  struct pad_status s;
  enum pad_verdict v;
  char note[64] = "";
  u32 drawn = 0;
  static const unsigned char own_keys[] = { KEY_D, 0 };
  const unsigned char *menu_keys = nspire_gui_own_keys;
  int first = 1, details = 0;

  keys_wait_release();
  nspire_gui_own_keys = own_keys;
  snapshot(&s, &v);
  nspire_padlog_add(PL_USER, PL_USER_PAGE, 0, 0, (u32)v, 0, 0);
  for (;;)
  {
    gui_action_type a;

    if (first || ms_since(drawn) >= REFRESH_MS)
    {
      first = 0;
      drawn = nspire_ticks();
      nspire_padstatus_read(&s);
      v = nspire_padstatus_verdict(&s);
      if (details)
        draw_details(&s, v, note);
      else
        draw(&s, v, note);
    }
    update_at_vblank();
    a = get_gui_input();   /* the pad driver is polled in it */
    if (nspire_drive_menu_tick())
      first = 1;
    if (a == CURSOR_BACK || a == CURSOR_EXIT)
    {
      if (!details)
        break;
      details = 0;   /* esc in Details: back to the page */
      first = 1;
    }
    else if (!details && key_pressed(KEY_D))
    {
      details = 1;
      first = 1;
    }
    else if (a == CURSOR_SELECT)
    {
      int r;
      if (details)
        draw_details(&s, v, "restarting the USB power...");
      else
        draw(&s, v, "restarting the USB power...");
      r = nspire_arb_manual_cycle();
      snprintf(note, sizeof(note), "%s", r == 1 ? "power restarted" : r == 0 ? "power restart: no power back" :
               r == -1 ? "nothing to restart (no device)" : r == -3 ? "not now: the port is in use" :
               r == -4 ? "not now: the other calculator is on it" :
               "can't restart here");
      first = 1;
    }
    else if (a == CURSOR_RIGHT && details)
    {
      /* (R4: the OS alone until its host controller runs; in Details, where its band says it) */
      give_os_time(note, sizeof(note));
      first = 1;
    }
  }
  nspire_padstatus_read(&s);
  nspire_padlog_add(PL_USER, PL_USER_PAGE_LEFT, 0, 0, (u32)nspire_padstatus_verdict(&s), 0, 0);
  nspire_padlog_request("the Controller page was closed");
  nspire_padlog_write_point();
  nspire_gui_own_keys = menu_keys;
  keys_wait_release();
}
