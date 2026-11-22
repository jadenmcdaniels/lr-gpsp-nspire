/* The Bluetooth page's data and actions behind ui7's page (nspire_bluetooth.h:
 * struct nspire_bt_ops; the page itself, its keys and its words are
 * nspire_devices.c's): what is paired and what a search heard, pairing,
 * connecting, forgetting, the switch, and the band's line (what is going
 * on: a pairing, its outcome, the dongle missing, its firmware missing,
 * Bluetooth stopped). nspire_bt_glue.c installs them at start. */
#include "common.h"
#include "nspire.h"
#include "nspire_bluetooth.h"
#include "nspire_bt_glue.h"
#include "nspire_usbpad_glue.h"
#include <stdio.h>
#include <string.h>

#ifndef NSPIRE_NO_BT

#include "nspire_bt.h"
#include "bt_port.h"
#include "nspire_ui.h"      /* (ui_label_width: the band's line fits its place) */

#define NOTE_MS     6000    /* the outcome of what was asked stays on the band this long */
#define WAIT_MS     30000   /* a pairing or a connect with no word from the library: given up on */
#define SEARCH_S    10      /* a search: Classic inquiry and LE scan */

static struct
{
  uint8_t addr[6];          /* a pairing or a connect asked for */
  int pairing;              /* 1 pairing (not paired yet), 0 connecting */
  uint32_t at;
  int active;
  char line[96];            /* the band's line: what was asked, its outcome */
  uint32_t line_at;
  unsigned note_seen;
} pg;

static uint32_t now_ms(void)
{
  return (uint32_t)((uint64_t)nspire_ticks() * 1000u / NSPIRE_TICK_HZ);
}

static void addr_text(const uint8_t *a, char *out, unsigned size)
{
  snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
}

static int addr_parse(const char *s, uint8_t *a)
{
  unsigned v[6], i;
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
    return 0;
  for (i = 0; i < 6; i++)
    a[i] = (uint8_t)v[i];
  return 1;
}

/* fmt with one %s, the name cut ("...") until the line fits the band (ui7's: UI_W - 2 x UI_MARGIN of its type) */
static void fit_line(char *out, unsigned size, const char *fmt, const char *name)
{
  char cut[48];
  int n = (int)strlen(name);

  snprintf(out, size, fmt, name);
  while (ui_label_width(out) > UI_W - 2 * UI_MARGIN && n > 4)
  {
    n--;
    snprintf(cut, sizeof(cut), "%.*s...", n, name);
    snprintf(out, size, fmt, cut);
  }
}

static void say(const char *fmt, const char *name)
{
  fit_line(pg.line, sizeof(pg.line), fmt, name);
  pg.line_at = now_ms();
}

static const struct nspire_bt_device *find(const uint8_t *a)
{
  int i;
  for (i = 0; i < nspire_bt_paired_count(); i++)
    if (!memcmp(nspire_bt_paired(i)->addr, a, 6))
      return nspire_bt_paired(i);
  for (i = 0; i < nspire_bt_scan_count(); i++)
    if (!memcmp(nspire_bt_scan_result(i)->addr, a, 6))
      return nspire_bt_scan_result(i);
  return NULL;
}

static void name_of(const struct nspire_bt_device *d, const uint8_t *a, char *out, unsigned size)
{
  if (d && d->name[0])
    snprintf(out, size, "%.36s", d->name);
  else
    addr_text(a, out, size);
}

static int ready(void)
{
  return nspire_bt_enabled && nspire_bt_state() == NSPIRE_BT_ON;
}

/* ---- the ops ---- */

/* the dongle is in (with Bluetooth off by the setting: 1, the page shows its switch, not "No dongle") */
static int op_present(void)
{
  return !nspire_bt_enabled || nspire_bt_usb_active() || nspire_bt_state() == NSPIRE_BT_ON;
}

static int op_on(void)
{
  return nspire_bt_enabled != 0;
}

static void op_set_on(int on)
{
  nspire_bt_enabled = !!on;
  nspire_bt_glue_apply();
  pg.active = 0;
  pg.line[0] = 0;
}

static int op_searching(void)
{
  return ready() && nspire_bt_scanning();
}

static void op_search(int start)
{
  if (!start)
  {
    if (nspire_bt_scanning())
      nspire_bt_scan_stop();
    return;
  }
  /* (the page opens with a search, or Search again: what happened before is old news; 2: the page searching on
   * by itself, r19-bt: the band keeps what it says) */
  if (start != 2)
  {
    (void)nspire_bt_glue_note(&pg.note_seen);
    pg.line[0] = 0;
  }
  if (!ready())
    return;   /* (the band says why: op_status) */
  if (nspire_bt_scan_start(SEARCH_S) != 0)
    say("%s", "Can't search now. Try again in a moment.");
}

static int signal_bars(int rssi)
{
  if (!rssi)
    return -1;
  return rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
}

/* A connect or pairing under way to it: r19-bt's library says so for each device (the player's, waiting its
 * turn or running, and an automatic reconnect: one at a time); an older one only knew the page's own. */
static int attempting(const struct nspire_bt_device *d)
{
#if NSPIRE_BT_API_LEVEL >= 2
  return d->attempt != NSPIRE_BT_ATTEMPT_NONE;
#else
  return pg.active && !memcmp(pg.addr, d->addr, 6);
#endif
}

static void fill(struct bt_device *o, const struct nspire_bt_device *d)
{
  memset(o, 0, sizeof(*o));
  snprintf(o->name, sizeof(o->name), "%s", d->name);
  addr_text(d->addr, o->address, sizeof(o->address));
  o->kind = d->type == NSPIRE_BT_DEV_PAD ? BT_CONTROLLER : d->type == NSPIRE_BT_DEV_HEADPHONES ? BT_HEADPHONES : BT_OTHER;
  o->state = d->connected ? BT_CONNECTED : d->paired ? BT_PAIRED : BT_FOUND;
  if (!d->connected && attempting(d))
#if NSPIRE_BT_API_LEVEL >= 2
    o->state = d->paired ? BT_CONNECTING : BT_PAIRING;   /* (a pairing done, the connect going on: CONNECTING) */
#else
    o->state = pg.pairing ? BT_PAIRING : BT_CONNECTING;
#endif
  o->signal = (s8)signal_bars(d->rssi);
  o->le = d->le;
  o->rssi = d->rssi;
#if NSPIRE_BT_API_LEVEL >= 3
  o->what = d->kind <= NSPIRE_BT_KIND_MOUSE ? d->kind : BT_WHAT_NONE;   /* (the same order) */
#else
  o->what = o->kind == BT_CONTROLLER ? BT_WHAT_CONTROLLER : o->kind == BT_HEADPHONES ? BT_WHAT_HEADPHONES : BT_WHAT_NONE;
#endif
}

/* r19-bt: listed as a phone lists them: a nameless LE device only when it shows the HID service or a gamepad
 * appearance (the library's type: a controller), nameless beacons not */
static int listed(const struct nspire_bt_device *d)
{
  return d->name[0] || !d->le || d->paired || d->connected || d->type == NSPIRE_BT_DEV_PAD;
}

static int op_devices(struct bt_device *out, int max)
{
  int n = 0, paired, i, j;

  if (!ready())
    return 0;
  for (i = 0; i < nspire_bt_paired_count() && n < max; i++)
    fill(&out[n++], nspire_bt_paired(i));
  paired = n;
  for (i = 0; i < nspire_bt_scan_count() && n < max; i++)
  {
    const struct nspire_bt_device *d = nspire_bt_scan_result(i);
    int dup = 0;
    for (j = 0; j < paired; j++)
      if (!memcmp(nspire_bt_paired(j)->addr, d->addr, 6))
      {
        dup = 1;
        if (d->rssi)
          out[j].signal = (s8)signal_bars(d->rssi);   /* (a paired one the search heard: its signal) */
      }
    if (!dup && listed(d))
      fill(&out[n++], d);
  }
  return n;
}

/* An LE controller that can't connect: a build without LE connects, or a dongle on which LE does not work
 * this run (the library's nspire_bt_le_problem: the rest of Bluetooth goes on Classic only). NULL: it can. */
static const char *le_why_not(const struct nspire_bt_device *d)
{
  if (!d->le || d->type != NSPIRE_BT_DEV_PAD)
    return NULL;
  if (!(nspire_bt_features() & NSPIRE_BT_FEAT_LE_CONNECT))
    return "This build can't connect LE controllers.";
#if NSPIRE_BT_API_LEVEL >= 2
  if (nspire_bt_le_problem()[0])
    return "This dongle can't connect LE controllers.";
#endif
  return NULL;
}

static const char *op_why_not(const char *address)
{
  uint8_t a[6];
  const struct nspire_bt_device *d;
  if (!addr_parse(address, a) || !ready() || !(d = find(a)) || d->connected || attempting(d))
    return NULL;
  return le_why_not(d);
}

static int start_connect(const char *address, int pairing)
{
  uint8_t a[6];
  char name[40];
  int r;

  if (!addr_parse(address, a) || !ready())
    return 0;
  if (nspire_bt_scanning())
    nspire_bt_scan_stop();   /* (the radio's for the connection now) */
  name_of(find(a), a, name, sizeof(name));
  r = nspire_bt_connect(a);
  if (r == NSPIRE_BT_ENOMEM)
  {
    const struct nspire_bt_device *d = find(a);
    say("%s", d && d->type == NSPIRE_BT_DEV_HEADPHONES ? "Other headphones are in use." : "Two controllers are in already.");
    return 0;
  }
  if (r == NSPIRE_BT_ENOTSUP)
  {
    /* r19-bt: LE controllers connect; an LE device that is not one is not paired with */
    const struct nspire_bt_device *d = find(a);
    const char *why = d ? le_why_not(d) : NULL;
    say("%s", why ? why : d && d->le ? "This dongle can't connect LE devices." : "Only controllers and headphones connect.");
    return 0;
  }
  if (r != 0)
  {
    say("Couldn't connect to %s.", name);
    return 0;
  }
  memcpy(pg.addr, a, 6);
  pg.pairing = pairing;
  pg.at = now_ms();
  pg.active = 1;
#if NSPIRE_BT_API_LEVEL >= 2
  /* r19-bt: the line says PAIRING or CONNECTING while the library tries (waiting its turn or paging), and the
   * band its key, "ENTER STOP" (a misclick stopped by enter again); its outcome is the band's then */
  pg.line[0] = 0;
  (void)name;
#else
  say(pairing ? "Pairing %s..." : "Connecting %s...", name);
#endif
  return 1;
}

static int op_pair(const char *address)
{
  return start_connect(address, 1);
}

static int op_connect(const char *address, int on)
{
  uint8_t a[6];
  char name[40];

  if (on)
    return start_connect(address, 0);
  if (!addr_parse(address, a))
    return 0;
  name_of(find(a), a, name, sizeof(name));
  if (nspire_bt_disconnect(a) != 0)
  {
    say("Couldn't disconnect %s.", name);
    return 0;
  }
  say("Disconnected %s.", name);
  return 1;
}

#if NSPIRE_BT_API_LEVEL >= 2
/* enter on a device PAIRING or CONNECTING: the library stops it honestly (the page cancelled, a link it
 * brought up dropped, a pairing it made dropped: found, or paired, as before) and holds it (no automatic
 * reconnect) until it is asked again or calls by itself */
static int op_stop(const char *address)
{
  uint8_t a[6];

  if (!addr_parse(address, a) || !ready())
    return 0;
  if (nspire_bt_cancel(a) != 0)
    return 0;
  if (pg.active && !memcmp(pg.addr, a, 6))
    pg.active = 0;
  (void)nspire_bt_glue_note(&pg.note_seen);   /* (what came before the stop is old news) */
  say("%s", "Stopped.");
  return 1;
}
#endif

static int op_forget(const char *address)
{
  uint8_t a[6];
  char name[40];

  if (!addr_parse(address, a))
    return 0;
  name_of(find(a), a, name, sizeof(name));
  if (nspire_bt_forget(a) != 0)
  {
    say("Couldn't forget %s.", name);
    return 0;
  }
  if (pg.active && !memcmp(pg.addr, a, 6))
    pg.active = 0;
  say("Forgot %s.", name);
  return 1;
}

/* What Bluetooth itself is like when it isn't simply working (NULL then). */
static const char *state_line(void)
{
  bt_port_info_t in;
  int st;

  if (!nspire_bt_enabled)
    return NULL;
  bt_port_info(&in);
  st = nspire_bt_state();
  if (bt_port_state() == BT_PORT_FAILED || st == NSPIRE_BT_FAILED)
    return in.firmware == 3 ? "The dongle's firmware file is missing." :
           in.firmware == 4 ? "The dongle's firmware didn't load." : "Bluetooth stopped: the dongle failed.";
  switch (st)
  {
  case NSPIRE_BT_OFF:
    return "Bluetooth couldn't start.";
  case NSPIRE_BT_NO_DONGLE:
    /* (r19-bt: a hub's devices are set up one by one by the OS, the dongle a few seconds after the hub) */
    if (nspire_usbhub_present())
      return "A hub is in: its devices take up to 15 s.";
    return in.dongles_seen ? "Dongle unplugged. Plug it in again." : "Plug a USB Bluetooth dongle in.";
  case NSPIRE_BT_STARTING:
    return in.chipset == 1 ? "Starting the dongle (about 3 s)..." : "Starting the dongle...";
  case NSPIRE_BT_STOPPING:
    return "Bluetooth is stopping...";
  default:
    break;
  }
  if (in.firmware == 3)
    return "The dongle's firmware file is missing.";
  return NULL;   /* (the Search line says "Searching"; a picked line's own word shows) */
}

static const char *op_status(void)
{
  unsigned seq;
  const char *note = nspire_bt_glue_note(&seq);

  /* the library's word on what was asked (or on something else that happened) */
  if (seq != pg.note_seen)
  {
    pg.note_seen = seq;
    if (note[0])
    {
      int type, player, kind, err;
      char name[40], text[96];
      snprintf(pg.line, sizeof(pg.line), "%s", note);
      /* (the toast's words, its name cut to the band's place) */
      if (nspire_bt_glue_note_event(&type, &player, &kind, &err, name, sizeof(name)))
      {
        int n = (int)strlen(name);
        nspire_bt_glue_toast_text(type, player, kind, err, name, text, sizeof(text));
        while (ui_label_width(text) > UI_W - 2 * UI_MARGIN && n > 4)
        {
          char cut[48];
          n--;
          snprintf(cut, sizeof(cut), "%.*s...", n, name);
          nspire_bt_glue_toast_text(type, player, kind, err, cut, text, sizeof(text));
        }
        snprintf(pg.line, sizeof(pg.line), "%s", text);
        /* r19-bt: a device tried and found to be neither (enter never refuses for a label): one sentence */
        if (type == NSPIRE_BT_EV_NOT_SUPPORTED)
          snprintf(pg.line, sizeof(pg.line), "%s", "Not a controller or headphones.");
      }
      pg.line_at = now_ms();
      pg.active = 0;
    }
  }
  /* (no word from the library long after it stopped trying: said here; while it still tries, waiting its turn
   * or paging, its line keeps PAIRING / CONNECTING) */
  if (pg.active && now_ms() - pg.at > WAIT_MS)
  {
    const struct nspire_bt_device *d = find(pg.addr);
    if (!d || !attempting(d))
    {
      pg.active = 0;
      say("%s", pg.pairing ? "No answer. Is it in pairing mode?" : "No answer. Is it on?");
    }
  }
  if (pg.line[0] && (pg.active || now_ms() - pg.line_at < NOTE_MS))
    return pg.line;
  return state_line();
}

#if NSPIRE_BT_API_LEVEL >= 3
static int op_switching(void)
{
  return nspire_bt_enabled || nspire_bt_state() != NSPIRE_BT_OFF ? nspire_bt_switching() : 0;
}
#endif

static const struct nspire_bt_ops ops = {
  op_present, op_on, op_set_on, op_searching, op_search, op_devices, op_pair, op_connect, op_forget, op_status,
#if NSPIRE_BT_API_LEVEL >= 2
  op_stop,
#else
  NULL,
#endif
  op_why_not,
#if NSPIRE_BT_API_LEVEL >= 3
  op_switching
#else
  NULL
#endif
};

void nspire_bt_page_install(void)
{
  unsigned seq;
  (void)nspire_bt_glue_note(&seq);
  pg.note_seen = seq;
  nspire_bt = &ops;
}

#else

void nspire_bt_page_install(void) {}

#endif
