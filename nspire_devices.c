/* The menus' side of the USB drive and Bluetooth (r16, docs/ui/spec.txt):
 * saves on a USB drive (nspire_places.h) and Bluetooth through a USB dongle
 * (nspire_bluetooth.h): what the menus show and what the keys do, against
 * functions the USB drive code and the Bluetooth glue fill in. Until they
 * do, the pointers are NULL and nothing of either shows. On the PC
 * (NSPIRE_HOST), GPSP_HOST_DRIVE_SAVES=1 and GPSP_HOST_BT=1 give stand-ins
 * with made-up devices and saves, so the screens can be drawn and tried
 * (tests/host/ui_wired.py, scenes "drive_saves" and "bluetooth"). */

#include "common.h"
#include "input.h"
#include "nspire.h"
#include "nspire_frontend.h"
#include "nspire_audio.h"
#include "nspire_keys.h"
#include "nspire_dialog.h"
#include "nspire_ui_screens.h"
#include "nspire_places.h"
#include "nspire_bluetooth.h"
#include "nspire_states.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Saves in two places ---- */

const char *const nspire_place_names[PLACES] = { "Calculator", "USB drive" };
u32 nspire_saves_on;
const struct nspire_place_ops *nspire_drive_saves;

int nspire_places_ready(void)
{
  return nspire_drive_saves && nspire_drive_saves->ready && nspire_drive_saves->ready();
}

/* ---- Bluetooth ---- */

const struct nspire_bt_ops *nspire_bt;

void nspire_bt_toast_text(char *out, unsigned size, int kind, int player, int connected)
{
  const char *how = connected ? "connected" : "disconnected";
  if (kind == BT_CONTROLLER && player > 0)
    snprintf(out, size, "Controller %d %s%s", player, how, connected ? " (Bluetooth)" : "");
  else if (kind == BT_CONTROLLER)
    snprintf(out, size, "Controller %s%s", how, connected ? " (Bluetooth)" : "");
  else if (kind == BT_HEADPHONES)
    snprintf(out, size, "Headphones %s", how);
  else
    snprintf(out, size, "Bluetooth device %s", how);
}

void nspire_bt_toast(int kind, int player, int connected)
{
  char text[64];
  nspire_bt_toast_text(text, sizeof(text), kind, player, connected);
  nspire_osd_message("%s", text);
}

#define BT_MAX 40   /* (r19-bt: everything named that a search hears; the page shows UI_PAGE_LINES of them) */

/* r19-bt: the word for what a device says it is (the library's kind), none when nothing says */
static const char *bt_what_word(int what)
{
  static const char *const w[] = { "", "Controller", "Headphones", "Speaker", "Phone", "Computer", "TV",
                                   "Keyboard", "Mouse" };
  return what > 0 && what < (int)(sizeof(w) / sizeof(w[0])) ? w[what] : "";
}

const char *nspire_bt_summary(void)
{
  static char text[32];
  struct bt_device d[BT_MAX];
  int n, i, connected = 0;
  if (!nspire_bt)
    return "";
  if (nspire_bt->present && !nspire_bt->present())
    return "No dongle";
  if (!nspire_bt->on())
    return "Off";
  n = nspire_bt->devices(d, BT_MAX);
  for (i = 0; i < n; i++)
    connected += d[i].state == BT_CONNECTED;
  if (!connected)
    return "On";
  snprintf(text, sizeof(text), "On, %d connected", connected);
  return text;
}

/* A signal worth a word: the lowest two of its four steps (under -75 dBm: nspire_bt_page.c signal_bars). r19
 * showed every one as "3/4", "1/4" and so on, and the menu did not say what that meant:
 * now a normal signal says nothing and a weak one "WEAK" after what the device is
 * ("CONTROLLER . WEAK"), the line's one other fact. Not on a connected one (its signal is the search's,
 * from before it connected). */
#define BT_WEAK_BARS 1

static int bt_weak(const struct bt_device *d)
{
  return d->signal >= 0 && d->signal <= BT_WEAK_BARS && d->state != BT_CONNECTED;
}

static int bt_is_paired(const struct bt_device *d)
{
  return d->state != BT_FOUND && d->state != BT_PAIRING;
}

/* r19-bt, the order (as a phone's list): paired first, then what gpSP can use (controllers, headphones), then
 * the rest; each the stronger first (a signal not known last) */
static int bt_group(const struct bt_device *d)
{
  return bt_is_paired(d) ? 0 : d->kind != BT_OTHER ? 1 : 2;
}

static int bt_before(const struct bt_device *a, const struct bt_device *b)
{
  if (bt_group(a) != bt_group(b))
    return bt_group(a) < bt_group(b);
  return (a->rssi ? a->rssi : -128) > (b->rssi ? b->rssi : -128);
}

/* The page's lines: the switch, search, PAIRED and its devices, FOUND and its devices. which[] says what each
 * line is: -1 the switch, -2 search, -3 a heading, else the device's index. ui18: the switch and search are the
 * page's tiles (v->tiles), the store's, side by side; the devices stay a list under them, a line each: a name
 * is as long as its maker made it ("Example Soundbar Remote Control 001"), and a list of what was heard is read
 * down, "WEAK" beside one whose signal is (bt_weak). r19-bt: the order of bt_before; UI_PAGE_LINES in all,
 * the weakest of the rest left out first when there are more, a paired one never. */
static int bt_lines(struct ui_list_page_view *v, int *which, struct bt_device *d, int n)
{
  int count = 0, i, j, pass, any, room, n_paired = 0, n_found;
  int on = nspire_bt->on();
  int order[BT_MAX];

  memset(v->lines, 0, sizeof(v->lines));
  v->cols = 2;
  snprintf(v->lines[count].name, sizeof(v->lines[0].name), "Bluetooth");
  {
    /* (r19-bt: a switch the dongle's start-up or its stop hasn't caught up with yet says so) */
    int sw = nspire_bt->switching ? nspire_bt->switching() : 0;
    snprintf(v->lines[count].kind, sizeof(v->lines[0].kind), "%s",
             on ? (sw == 1 ? "Starting" : "On") : (sw == 2 ? "Stopping" : "Off"));
  }
  v->lines[count].signal = -1;
  which[count++] = -1;
  v->tiles = count;
  if (!on)
    return count;
  snprintf(v->lines[count].name, sizeof(v->lines[0].name), "Search");
  snprintf(v->lines[count].kind, sizeof(v->lines[0].kind), "%s",
           nspire_bt->searching() ? "Searching" : "Search again");
  v->lines[count].signal = -1;
  which[count++] = -2;
  v->tiles = count;
  if (n > BT_MAX)
    n = BT_MAX;
  for (i = 0; i < n; i++)
  {
    for (j = i; j > 0 && bt_before(&d[i], &d[order[j - 1]]); j--)
      order[j] = order[j - 1];
    order[j] = i;
    n_paired += bt_is_paired(&d[i]);
  }
  n_found = n - n_paired;
  /* the lines left for devices: the tiles' and the headings' taken */
  room = UI_PAGE_LINES - count - (n_paired > 0) - (n_found > 0);
  if (n_paired > room)
    n_paired = room;
  if (n_found > room - n_paired)
    n_found = room - n_paired;
  for (pass = 0; pass < 2; pass++)
  {
    int want = pass ? n_found : n_paired;
    any = 0;
    for (i = 0; i < n && want > 0; i++)
    {
      const struct bt_device *e = &d[order[i]];
      const char *word;
      if (bt_is_paired(e) != !pass)
        continue;
      if (!any)
      {
        snprintf(v->lines[count].name, sizeof(v->lines[0].name), pass ? "Found" : "Paired");
        v->lines[count].heading = 1;
        v->lines[count].signal = -1;
        which[count++] = -3;
        any = 1;
      }
      want--;
      snprintf(v->lines[count].name, sizeof(v->lines[0].name), "%s", e->name[0] ? e->name : e->address);
      /* (the driver keeps 31 characters of a name: one that long was cut there: said so, "...") */
      if (strlen(e->name) == 31)
        snprintf(v->lines[count].name, sizeof(v->lines[0].name), "%s" UI_ELLIPSIS, e->name);
      word = e->state == BT_CONNECTED ? "Connected" : e->state == BT_CONNECTING ? "Connecting" :
             e->state == BT_PAIRING ? "Pairing" : e->state == BT_PAIRED ? "Paired" : bt_what_word(e->what);
      snprintf(v->lines[count].kind, sizeof(v->lines[0].kind), "%s%s", word,
               !bt_weak(e) ? "" : word[0] ? " " UI_DOT " Weak" : "Weak");
      v->lines[count].signal = -1;   /* (the list page's "N/4": not drawn) */
      which[count++] = order[i];
    }
  }
  return count;
}

static void bt_forget_ground(void *arg)
{
  ui_list_page_draw((const struct ui_list_page_view *)arg);
}

#if defined(NSPIRE_HOST)
/* The PC tests read the page's words (tests/host/bt_tests.py: host_bt.c writes them to out/pad.txt as "bt page
 * ..." lines): its lines ("name=kind/signal", UI_DOT as "."), the picked one, and the band (what just happened,
 * else the picked line's sentence, else its keys), each time they change. */
void host_bt_page_said(const char *what) __attribute__((weak));

static void bt_page_said(const struct ui_list_page_view *v)
{
  static char last_list[600], last_band[200], last_tile[260];
  char list[600], band[200];
  int i, n = 0;
  char *c;

  if (!host_bt_page_said)
    return;
  list[0] = 0;
  for (i = v->tiles; i < v->count && n < (int)sizeof(list) - 1; i++)
    if (!v->lines[i].heading)
      n += snprintf(list + n, sizeof(list) - (size_t)n, "%s%s=%s/%d", n ? "; " : "", v->lines[i].name,
                    v->lines[i].kind, v->lines[i].signal);
  if (v->status && v->status[0])
    snprintf(band, sizeof(band), "status %s", v->status);
  else if (v->help && v->help[0])
    snprintf(band, sizeof(band), "help %s", v->help);
  else
  {
    n = snprintf(band, sizeof(band), "keys");
    for (i = 0; v->keys && v->keys[2 * i] && n < (int)sizeof(band) - 1; i++)
      n += snprintf(band + n, sizeof(band) - (size_t)n, " %s %s", v->keys[2 * i] + (v->keys[2 * i][0] == '!'),
                    v->keys[2 * i + 1]);
  }
  for (c = list; *c; c++)
    if (*c == UI_DOT[0])
      *c = '.';
  if (strcmp(list, last_list))
  {
    char line[640];
    snprintf(line, sizeof(line), "list %s", list);
    host_bt_page_said(line);
    snprintf(last_list, sizeof(last_list), "%s", list);
  }
  {
    /* (r19-bt: and when the picked line's own words change: the switch's STARTING / STOPPING) */
    char line[260];
    snprintf(line, sizeof(line), "picked %s=%s, %s", v->lines[v->picked].name, v->lines[v->picked].kind, band);
    for (c = line; *c; c++)
      if (*c == UI_DOT[0])
        *c = '.';
    if (strcmp(band, last_band) || (v->picked < v->tiles && strcmp(line, last_tile)))
      host_bt_page_said(line);
    snprintf(last_band, sizeof(last_band), "%s", band);
    snprintf(last_tile, sizeof(last_tile), "%s", line);
  }
}
#else
#define bt_page_said(v) ((void)0)
#endif

void nspire_bluetooth_page(void)
{
  static const unsigned char own_keys[] = { KEY_DEL, 0 };
  static const char *const keys_switch[] = { NULL };
  static const char *const keys_search[] = { NULL };
  static const char *const keys_found[] = { "!enter", "Pair", NULL };
  static const char *const keys_paired[] = { "!enter", "Connect", "del", "Forget", NULL };
  static const char *const keys_connected[] = { "!enter", "Disconnect", "del", "Forget", NULL };
  /* r19-bt: a misclick stopped with enter again (a pairing or a connect picked by mistake is
   * stopped by pressing enter on it a second time) */
  static const char *const keys_pairing[] = { "!enter", "Stop", NULL };
  static const char *const keys_connecting[] = { "!enter", "Stop", "del", "Forget", NULL };
  static struct ui_list_page_view v;
  struct bt_device d[BT_MAX];
  int which[UI_PAGE_LINES];
  int picked = 0, n;
  u32 picked_at = nspire_ticks(), search_ended = nspire_ticks();
  int search_on = 1;   /* (0: the player stopped the search: not started again until asked) */
  char last_shown[32] = "";
  const unsigned char *menu_keys = nspire_gui_own_keys;

  if (!nspire_bt)
    return;
  nspire_audio_pause();
  keys_wait_release();
  nspire_gui_own_keys = own_keys;
  if (nspire_bt->on() && !nspire_bt->searching())
    nspire_bt->search(1);   /* the page opened: look for what is around */
  for (;;)
  {
    gui_action_type a;
    const char *status = nspire_bt->status ? nspire_bt->status() : NULL;
    int w;

    /* r19-bt (a pad put in pairing mode after the page's one search had ended was never heard): the page searches on while it is open, as a phone does, a second after each search ends; not
     * while something is being paired or connected (the radio's for it), nor after the player stopped it */
    {
      int k, busy = 0;
      n = nspire_bt->on() ? nspire_bt->devices(d, BT_MAX) : 0;
      for (k = 0; k < n; k++)
        busy |= d[k].state == BT_PAIRING || d[k].state == BT_CONNECTING;
      if (nspire_bt->searching() || busy)
        search_ended = nspire_ticks();
      else if (search_on && nspire_bt->on() && nspire_ticks() - search_ended > NSPIRE_TICK_HZ)
      {
        nspire_bt->search(2);
        search_ended = nspire_ticks();
      }
    }

    memset(&v, 0, sizeof(v));
    v.from = "Settings";
    v.title = "Bluetooth";
    v.name = "bluetooth";
    n = nspire_bt->devices(d, BT_MAX);
    v.count = bt_lines(&v, which, d, n);
    if (picked >= v.count)
      picked = v.count - 1;
    while (picked > 0 && v.lines[picked].heading)
      picked--;
    v.picked = picked;
    /* (the list under the tiles scrolls to keep the picked line shown) */
    {
      int lines = ui_list_page_lines(&v), at = picked - v.tiles;
      v.first = at >= lines ? at - lines + 1 : 0;
    }
    w = which[picked];
    v.keys = w == -1 ? keys_switch : w == -2 ? keys_search :
             d[w].state == BT_FOUND ? keys_found :
             d[w].state == BT_PAIRING ? (nspire_bt->stop ? keys_pairing : keys_found) :
             d[w].state == BT_CONNECTING ? (nspire_bt->stop ? keys_connecting : keys_paired) :
             d[w].state == BT_CONNECTED ? keys_connected : keys_paired;
    nspire_gui_own_keys = (w >= 0 && d[w].state != BT_FOUND && d[w].state != BT_PAIRING) ? own_keys : NULL;
    /* (r19-bt: enter tries any device: the band says what enter does, and the outcome when it is known) */
    if (w >= 0 && nspire_bt->why_not)
      v.help = nspire_bt->why_not(d[w].address);   /* (r19-bt: an LE controller this dongle can't connect) */
    v.status = status;
    v.picked_ms = ui_list_page_scrolls ? (unsigned)((nspire_ticks() - picked_at) * 1000 / NSPIRE_TICK_HZ) : 0;
    ui_list_page_draw(&v);
    bt_page_said(&v);
    if (strcmp(last_shown, ui_shown))
    {
      snprintf(last_shown, sizeof(last_shown), "%s", ui_shown);
      picked_at = nspire_ticks();
    }

    update_at_vblank();
    a = get_gui_input();
    if (a == CURSOR_BACK || a == CURSOR_EXIT)
      break;
    if (a == CURSOR_UP || a == CURSOR_DOWN || a == CURSOR_LEFT || a == CURSOR_RIGHT)
    {
      /* ui18: the tiles in two dimensions, down from them into the list, up and down along it */
      int k = ui_list_page_step(&v, picked, a == CURSOR_LEFT ? -1 : a == CURSOR_RIGHT ? 1 : 0,
                                a == CURSOR_UP ? -1 : a == CURSOR_DOWN ? 1 : 0);
      if (k != picked)
        picked_at = nspire_ticks();
      picked = k;
    }
    else if (a == CURSOR_SELECT)
    {
      if (w == -1)
        nspire_bt->set_on(!nspire_bt->on());
      else if (w == -2)
      {
        search_on = !nspire_bt->searching();
        nspire_bt->search(search_on);
      }
      else if (d[w].state == BT_FOUND)
        nspire_bt->pair(d[w].address);
      else if (d[w].state == BT_PAIRED || d[w].state == BT_CONNECTED)
        nspire_bt->connect(d[w].address, d[w].state != BT_CONNECTED);
      else if ((d[w].state == BT_PAIRING || d[w].state == BT_CONNECTING) && nspire_bt->stop)
        nspire_bt->stop(d[w].address);   /* (found or paired again, as before; enter again starts it again) */
    }
    else if (a == CURSOR_CLEAR && w >= 0 && d[w].state != BT_FOUND && d[w].state != BT_PAIRING)
    {
      struct ui_dialog_view q;
      char title[64];
      memset(&q, 0, sizeof(q));
      snprintf(title, sizeof(title), "Forget %s?", d[w].name[0] ? d[w].name : d[w].address);
      q.title = title;
      q.text = "It has to be paired again to connect.";
      q.first = "No, keep it";
      q.second = "Forget";
      q.destructive = 1;
      if (nspire_ask(&q, bt_forget_ground, &v) == 1)
        nspire_bt->forget(d[w].address);
    }
  }
  if (nspire_bt->searching())
    nspire_bt->search(0);
  nspire_gui_own_keys = menu_keys;
  keys_wait_release();
}

/* ---- The PC's stand-ins ---- */

#if defined(NSPIRE_HOST)

/* Saves on a drive: three made-up slots (no pictures), each an hour older than the one after it. */
static int mock_ready(void) { return 1; }
static unsigned mock_removed;   /* (UI17: the Saves place's drive tab deletes) */
static int mock_exists(int slot) { return slot >= 0 && slot < 3 && !(mock_removed >> slot & 1); }
static int mock_next(void) { return 3; }
/* (the calculator's own slots' pictures stand in for the drive's: what is kept, no file read) */
static int mock_picture(int slot, u16 *pic) { return states_picture_soon(slot, pic, 0) == 1; }
static long mock_time(int slot) { return (long)time(NULL) - (3 - slot) * 3600L; }
static int mock_save(int slot) { nspire_osd_message("Saved to slot %d (USB drive)", slot + 1); return 1; }
static int mock_load(int slot) { return mock_exists(slot) ? -1 : 0; }
static int mock_copy(int from, int slot, int *to)
{
  (void)from;
  (void)slot;
  *to = 3;
  return 1;
}
static int mock_remove(const char *rom_path, int slot)
{
  (void)rom_path;
  if (!mock_exists(slot))
    return 0;
  mock_removed |= 1u << slot;
  return 1;
}
static const struct nspire_place_ops mock_places = { mock_ready, mock_exists, mock_next, mock_picture, mock_time,
                                                     mock_save, mock_load, mock_copy, mock_remove };

/* Bluetooth: two controllers (a DualSense, paired and connected; a Switch Pro Controller), earbuds,
 * an LE device heard by its address only, and one with a long name. */
static struct bt_device mock_devices[] = {
  { "DualSense Wireless Controller", "A0:5A:5D:11:22:33", BT_CONTROLLER, BT_CONNECTED, 4, 0, BT_WHAT_CONTROLLER, -50 },
  { "Pro Controller", "98:B6:E9:44:55:66", BT_CONTROLLER, BT_FOUND, 3, 0, BT_WHAT_CONTROLLER, -60 },
  { "Example Buds", "2C:BE:EB:77:88:99", BT_HEADPHONES, BT_FOUND, 1, 0, BT_WHAT_HEADPHONES, -80 },
  { "", "44:16:22:5A:7B:01", BT_CONTROLLER, BT_FOUND, 4, 1, BT_WHAT_CONTROLLER, -52 },
  { "Example TV Set Model 0001", "AA:BB:CC:00:00:05", BT_OTHER, BT_FOUND, 2, 0, BT_WHAT_TV, -66 },
  { "Example Soundbar Remote Control 001", "AA:BB:CC:00:00:07", BT_OTHER, BT_FOUND, 2, 1, BT_WHAT_NONE, -71 },
};
static int mock_bt_on = 1, mock_bt_searching;
static char mock_bt_said[64];
static int mock_present(void) { return 1; }
static int mock_on(void) { return mock_bt_on; }
static void mock_set_on(int on) { mock_bt_on = on; }
static int mock_searching(void) { return mock_bt_searching; }
static void mock_search(int start) { mock_bt_searching = start; }
static int mock_devices_get(struct bt_device *out, int max)
{
  int n = (int)(sizeof(mock_devices) / sizeof(mock_devices[0]));
  if (n > max)
    n = max;
  memcpy(out, mock_devices, (size_t)n * sizeof(*out));
  return n;
}
static struct bt_device *mock_find(const char *address)
{
  unsigned i;
  for (i = 0; i < sizeof(mock_devices) / sizeof(mock_devices[0]); i++)
    if (!strcmp(mock_devices[i].address, address))
      return &mock_devices[i];
  return NULL;
}
static int mock_pair(const char *address)
{
  struct bt_device *d = mock_find(address);
  if (!d)
    return 0;
  d->state = BT_PAIRING;
  snprintf(mock_bt_said, sizeof(mock_bt_said), "Pairing %s...", d->name[0] ? d->name : d->address);
  return 1;
}
static int mock_connect(const char *address, int on)
{
  struct bt_device *d = mock_find(address);
  if (d)
    d->state = on ? BT_CONNECTED : BT_PAIRED;
  return d != NULL;
}
static int mock_forget(const char *address)
{
  struct bt_device *d = mock_find(address);
  if (d)
    d->state = BT_FOUND;
  return d != NULL;
}
static int mock_stop(const char *address)
{
  struct bt_device *d = mock_find(address);
  if (!d || (d->state != BT_PAIRING && d->state != BT_CONNECTING))
    return 0;
  d->state = d->state == BT_PAIRING ? BT_FOUND : BT_PAIRED;
  snprintf(mock_bt_said, sizeof(mock_bt_said), "Stopped.");
  return 1;
}
static const char *mock_status(void) { return mock_bt_said[0] ? mock_bt_said : NULL; }
static const struct nspire_bt_ops mock_bt = { mock_present, mock_on, mock_set_on, mock_searching, mock_search,
                                              mock_devices_get, mock_pair, mock_connect, mock_forget, mock_status,
                                              mock_stop };

static void __attribute__((constructor)) devices_mocks(void)
{
  const char *e = getenv("GPSP_HOST_DRIVE_SAVES");
  if (e && *e == '1')
    nspire_drive_saves = &mock_places;
  e = getenv("GPSP_HOST_BT");
  if (e && *e == '1')
    nspire_bt = &mock_bt;
}

#endif
