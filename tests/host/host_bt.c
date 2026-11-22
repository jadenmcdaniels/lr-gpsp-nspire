/* nspire-bt's API (nspire_bt.h, and what gpSP uses of bt_port.h) on the
 * PC, for gpsp-host: a simulated Bluetooth dongle, its two pad slots,
 * headphones and what a search finds, driven by the script. The library
 * itself is nspire-bt's (its own tests: tests/transport, tests/profiles);
 * this checks gpSP's glue (nspire_bt_glue.c), its page (nspire_bt_page.c),
 * the sound's routing (nspire_audio.c) and the exit.
 *
 * Script lines (host_hw.c passes "bt ..." here):
 *   bt dongle in|out       the dongle plugged in (Bluetooth on: ON after 0.3 s)
 *                          or pulled out (pads and headphones gone: FAILED
 *                          event "dongle unplugged", NO_DONGLE)
 *   bt firmware missing    the dongle's firmware file isn't there (the port's
 *                          problem; Bluetooth still comes on, on ROM code)
 *   bt fail                the dongle gave up (the port's FAILED)
 *   bt hung                the port's watchdog finds the dongle hung (no
 *                          detach): pads and headphones gone, "failed" with
 *                          the port's hung 1 (restarting it)
 *   bt hung back|giveup    its restart works (ON again) or not (FAILED, hung
 *                          2: "replug it")
 *   bt dongle reoffer      a hub power-cycled after a brown-out offers the
 *                          dongle again (no detach for the old one): its
 *                          links end ("failed", the port STARTING), ON again
 *                          0.5 s later, the last headphones called again
 *   bt hub recovering on|off   hub17's nspire_hub_recovering as the glue sees
 *                          it (a hub being restarted: "unplugged" and "replug
 *                          it" not shown)
 *   bt nomem               the dongle given up for want of memory (its rings:
 *                          29 KB; the port's problem "not enough memory ...")
 * GPSP_HOST_BT_NOMEM=1: the start itself refused for want of memory (the USB
 * slots, 13 KB).
 *   bt keys MASK [EXTRA]   the Bluetooth pads report the script's pad keys
 *                          ANDed with MASK (hex), ORed with EXTRA: a USB pad
 *                          and a Bluetooth one told apart
 *   bt pad in [KIND] [NAME]   a paired pad turned on: into the first free
 *                          slot (KIND ds4, dualsense (default), switchpro,
 *                          hid); its buttons are the script's pad keys
 *   bt pad out [N]         slot N's pad (1 the default) goes
 *   bt headphones in [NAME] / bt headphones out
 *   bt audio SPEED RING AHEAD [SINK]   what the headphones' stats say from
 *                          now (the rate's factor, their ring and lead ms,
 *                          their delay report; default 1.0 123 0 0)
 *   bt near TYPE RSSI NAME a device a search finds (TYPE pad, headphones,
 *                          other, unknown, -le after it for an LE one: pad-le;
 *                          NAME the rest of the line, none: a nameless one).
 *                          r19-bt: TYPE may be what it says it is (its kind):
 *                          speaker, phone, computer, tv, keyboard, mouse (not
 *                          a controller or headphones)
 *   bt connect ok|fail     what a connect does (ok: a pad pairs and comes in,
 *                          headphones connect, other: "not a controller or
 *                          headphones"; fail: "Couldn't connect")
 *   bt connect hold|release  (r19-bt) a connect stays under way (its device
 *                          PAIRING / CONNECTING) until stopped (nspire_bt_cancel)
 *                          or released (then it ends as connect ok|fail says)
 *   bt paired headphones NAME  (r19-bt) the last headphones, paired, off: not
 *                          connected
 *   bt reconnect           (r19-bt) the library's automatic reconnect of the last
 *                          headphones under way (their attempt AUTO): held open
 *                          until the player's connect stops it first (as the
 *                          library: one attempt at a time), the player stops it
 *                          (held: no reconnect until asked), or "bt reconnect
 *                          fail" (it ends, quietly: no event)
 *   bt le drop [NAME]      (r19-bt) a connected LE controller's link lost (the
 *                          first one, or NAME's): its slot freed, still bonded
 *   bt le advertise [NAME] a bonded LE controller advertising (turned on): the
 *                          library's whitelist connects it (pad in), unless it
 *                          was forgotten, the player stopped it (held), a
 *                          player's connect runs, or both slots are taken
 *   bt le refuse           the dongle refuses LE connects: LE off for the run
 *                          (nspire_bt_le_problem), LE controllers refused
 * LE controllers (bt near pad-le ...) connect as the Classic ones: paired
 * (bonded) at their first connect, the Xbox Wireless Controller as kind GIP
 * ("Xbox One"); an LE device that is not a controller is tried and let go
 * (EV_NOT_SUPPORTED, as a Classic one), nothing paired.
 * One attempt at a time, as the library: a connect asked while one runs waits
 * its turn (its device's attempt ASKED); nspire_bt_cancel stops one (running
 * or waiting): its device as before (found, or paired), held, no event.
 *   bt stuck on|off        the stop leaves a USB callback inside gpSP
 *   bt event TYPE NAME     the library posts an event of that type (on, off,
 *                          failed, unplugged, firmware, pad, padout, refused,
 *                          headphones, headphonesout, paired, failedconnect,
 *                          notsupported, restarted: a connect ended by the
 *                          dongle's restart) with that device name: its toast
 * GPSP_HOST_BT=1 (ui_wired's bluetooth scene): the dongle in from the start,
 * the reference DualSense paired and connected, and what a search hears: a
 * Switch Pro Controller, earbuds, an LE device known by its address only,
 * one with a long name. A connect takes a second (pairing, then connected).
 * What gpSP did goes into out/pad.txt as "bt ..." lines: start, power,
 * search, connect, forget, disconnect, flush, the sound's modes as they
 * change ("bt audio mode N"), codec, volume, player lights, the stop, and
 * the polls' and the interrupt windows' counts at the stop. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nspire_bt.h"
#include "bt_port.h"

double host_hw_now(void);
const char *host_hw_out_dir(void);
uint32_t host_hw_pad_keys(void);

static void bt_log(const char *format, ...)
{
  char path[4200];
  va_list ap;
  FILE *f;

  snprintf(path, sizeof(path), "%s/pad.txt", host_hw_out_dir());
  f = fopen(path, "a");
  if (!f)
    return;
  fprintf(f, "%.3f bt ", host_hw_now());
  va_start(ap, format);
  vfprintf(f, format, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}

/* ---- the simulation ---- */
static int state = NSPIRE_BT_OFF, started, dongle, dongles_seen, firmware_missing, failed, connect_fails, stuck, dead;
static int hung, hung_fires, hung_backs, hp_recall, nomem;
static char le_problem[48];             /* bt le refuse */
static int hub_recovering_now;      /* bt hub recovering on|off */

/* the glue's stand-in for hub17's nspire_hub_recovering (weak there) */
int host_bt_hub_recovering(void)
{
  return hub_recovering_now;
}
static uint32_t heap_now_b, heap_peak_b;
static uint32_t keys_mask = 0x3FFFFu, keys_extra;
/* the port's log (bt_port_log): its latest lines, for the glue's live log */
#define PLOG_N 64
static char plog[PLOG_N][112];
static uint32_t plog_n;
static double on_at = -1;
static volatile uint32_t idle_word;     /* the port's: nonzero while a dongle is in or news is pending */
static struct usbpad_state pads[NSPIRE_BT_PADS];
static char pad_names[NSPIRE_BT_PADS][32];
static int audio_on, audio_mode = -1;
static double st_speed = 1.0;
static unsigned st_ring = 123, st_ahead, st_sink;
static char hp_name[32];
static unsigned volume = 256;
static int codec_mono = -1;
static unsigned long polls, writes, frames_written;
static int want_flush;

#define EVQ 16
static struct nspire_bt_event evq[EVQ];
static unsigned ev_rd, ev_wr;

static void post(int type, int player, int kind, int dev_type, const uint8_t *addr, const char *name, int err)
{
  struct nspire_bt_event *e = &evq[ev_wr++ % EVQ];
  memset(e, 0, sizeof(*e));
  e->type = (uint8_t)type;
  e->player = (int8_t)player;
  e->kind = (uint8_t)kind;
  e->dev_type = (uint8_t)dev_type;
  if (addr)
    memcpy(e->addr, addr, 6);
  e->err = (int16_t)err;
  if (name)
    snprintf(e->name, sizeof(e->name), "%s", name);
  idle_word = 1;
}

/* devices: the ones "near" (a search finds them) and the paired */
#define NDEV 48
struct dev
{
  struct nspire_bt_device d;
  int near, found;
  double heard_at;   /* (r19-bt: a search again soon keeps what was heard in the last 15 s, as the library) */
};
static struct dev devs[NDEV];
static double switching_until;   /* (r19-bt: "bt starting S" / "bt stopping S") */
static int switching_kind;
static int ndev;
static double scan_until = -1, scan_from;
static struct nspire_bt_device scan_list[NDEV], paired_list[NDEV];
static int scan_n, paired_n;

static int type_rank(int t)
{
  return t == NSPIRE_BT_DEV_PAD ? 0 : t == NSPIRE_BT_DEV_HEADPHONES ? 1 : t == NSPIRE_BT_DEV_UNKNOWN ? 2 : 3;
}

static void lists(void)
{
  int i, j;

  scan_n = paired_n = 0;
  for (i = 0; i < ndev; i++)
  {
    if (devs[i].found)
      scan_list[scan_n++] = devs[i].d;
    if (devs[i].d.paired)
      paired_list[paired_n++] = devs[i].d;
  }
  /* pads, headphones, unknown, other; the stronger first */
  for (i = 1; i < scan_n; i++)
    for (j = i; j > 0; j--)
    {
      struct nspire_bt_device *a = &scan_list[j - 1], *b = &scan_list[j], t;
      if (type_rank(a->type) < type_rank(b->type) || (type_rank(a->type) == type_rank(b->type) && a->rssi >= b->rssi))
        break;
      t = *a;
      *a = *b;
      *b = t;
    }
}

static struct dev *dev_by_addr(const uint8_t *a)
{
  int i;
  for (i = 0; i < ndev; i++)
    if (!memcmp(devs[i].d.addr, a, 6))
      return &devs[i];
  return NULL;
}

static struct dev *dev_add(int type, const char *name)
{
  struct dev *v;
  if (ndev >= NDEV)
    return NULL;
  v = &devs[ndev];
  memset(v, 0, sizeof(*v));
  v->d.addr[0] = 0xA0;
  v->d.addr[1] = 0x10;
  v->d.addr[5] = (uint8_t)(ndev + 1);
  v->d.type = (uint8_t)type;
  v->d.kind = (uint8_t)(type == NSPIRE_BT_DEV_PAD ? NSPIRE_BT_KIND_CONTROLLER :
                        type == NSPIRE_BT_DEV_HEADPHONES ? NSPIRE_BT_KIND_HEADPHONES : NSPIRE_BT_KIND_NONE);
  snprintf(v->d.name, sizeof(v->d.name), "%s", name);
  ndev++;
  return v;
}

/* the attempts: pend[0] runs (it ends at pending_at, or never while held open), the rest wait their turn */
#define NPEND 4
static struct dev *pend[NPEND];
static int n_pend, connect_hold;
static double pending_at;
static struct dev *auto_dev;            /* the reconnect's (bt reconnect), while under way */
static void connect_now(struct dev *v);

static void set_attempt(struct dev *v, int attempt)
{
#if NSPIRE_BT_API_LEVEL >= 2
  v->d.attempt = (uint8_t)attempt;
#else
  (void)v;
  (void)attempt;
#endif
}

static void set_held(struct dev *v, int held)
{
#if NSPIRE_BT_API_LEVEL >= 2
  v->d.held = (uint8_t)held;
#else
  (void)v;
  (void)held;
#endif
}

static void pend_remove(int i)
{
  if (i < 0 || i >= n_pend)
    return;
  memmove(&pend[i], &pend[i + 1], (size_t)(n_pend - i - 1) * sizeof(pend[0]));
  n_pend--;
  if (i == 0 && n_pend)
    pending_at = host_hw_now() + 1.0;   /* (the next one's turn) */
}

/* the dongle gone, the radio off: nothing under way */
static void attempts_off(void)
{
  int i;
  for (i = 0; i < n_pend; i++)
    set_attempt(pend[i], 0);
  if (auto_dev)
    set_attempt(auto_dev, 0);
  n_pend = 0;
  auto_dev = NULL;
}

static int pend_find(const struct dev *v)
{
  int i;
  for (i = 0; i < n_pend; i++)
    if (pend[i] == v)
      return i;
  return -1;
}

static int kind_of(const char *w)
{
  if (!strcmp(w, "ds4"))
    return USBPAD_KIND_DS4;
  if (!strcmp(w, "switchpro"))
    return USBPAD_KIND_SWITCHPRO;
  if (!strcmp(w, "hid"))
    return USBPAD_KIND_HID;
  return USBPAD_KIND_DUALSENSE;
}

static int pad_in(int kind, const char *name, const uint8_t *addr)
{
  int s;
  for (s = 0; s < NSPIRE_BT_PADS; s++)
    if (!pads[s].connected)
      break;
  if (s == NSPIRE_BT_PADS)
  {
    post(NSPIRE_BT_EV_PAD_REFUSED, -1, kind, NSPIRE_BT_DEV_PAD, addr, name, 0);
    return -1;
  }
  memset(&pads[s], 0, sizeof(pads[s]));
  pads[s].connected = 1;
  pads[s].kind = (uint8_t)kind;
  pads[s].vid = kind == USBPAD_KIND_SWITCHPRO ? 0x057e : 0x054c;
  pads[s].pid = kind == USBPAD_KIND_SWITCHPRO ? 0x2009 : kind == USBPAD_KIND_DS4 ? 0x05c4 : 0x0ce6;
  snprintf(pad_names[s], sizeof(pad_names[s]), "%s", name);
  post(NSPIRE_BT_EV_PAD_CONNECTED, s, kind, NSPIRE_BT_DEV_PAD, addr, name, 0);
  bt_log("pad in slot %d kind %d %s", s, kind, name);
  bt_port_log("P%d: %s, BR", s + 1, name);   /* (the library's own lines, as the port's log has them) */
  return s;
}

static void pad_out(int s)
{
  if (s < 0 || s >= NSPIRE_BT_PADS || !pads[s].connected)
    return;
  memset(&pads[s], 0, sizeof(pads[s]));
  post(NSPIRE_BT_EV_PAD_DISCONNECTED, s, 0, NSPIRE_BT_DEV_PAD, NULL, pad_names[s], 0);
  bt_log("pad out slot %d", s);
  bt_port_log("P%d: disconnected", s + 1);
}

static void audio_in(const char *name, const uint8_t *addr)
{
  audio_on = 1;
  snprintf(hp_name, sizeof(hp_name), "%s", name);
  post(NSPIRE_BT_EV_AUDIO_CONNECTED, -1, 0, NSPIRE_BT_DEV_HEADPHONES, addr, name, 0);
  bt_log("headphones in %s", name);
  bt_port_log("A2DP stream started: %s", name);
}

static void audio_out(void)
{
  if (!audio_on)
    return;
  audio_on = 0;
  post(NSPIRE_BT_EV_AUDIO_DISCONNECTED, -1, 0, NSPIRE_BT_DEV_HEADPHONES, NULL, hp_name, 0);
  bt_log("headphones out");
}

static void dongle_out(void)
{
  int s;
  for (s = 0; s < NSPIRE_BT_PADS; s++)
    pad_out(s);
  audio_out();
  if (state == NSPIRE_BT_ON)
    post(NSPIRE_BT_EV_FAILED, -1, 0, 0, NULL, NULL, NSPIRE_BT_ENODONGLE);
  if (state != NSPIRE_BT_RADIO_OFF && state != NSPIRE_BT_OFF)
    state = NSPIRE_BT_NO_DONGLE;
  scan_until = -1;
  on_at = -1;
  attempts_off();
}

static void fatal_now(void);

void host_bt_script(const char *line)
{
  char w1[32] = "", w2[32] = "", w3[32] = "";
  const char *rest;

  sscanf(line, "%31s %31s %31s", w1, w2, w3);
  if (!strcmp(w1, "dongle") && !strcmp(w2, "reoffer"))
  {
    int s;
    hp_recall = audio_on;
    for (s = 0; s < NSPIRE_BT_PADS; s++)
      pad_out(s);
    audio_out();
    if (state == NSPIRE_BT_ON)
      post(NSPIRE_BT_EV_FAILED, -1, 0, 0, NULL, NULL, 0);
    if (hung == 2 && !dead)
      failed = 0;
    hung = 0;
    dongles_seen++;
    state = NSPIRE_BT_STARTING;
    on_at = host_hw_now() + 0.5;
    idle_word = 1;
    bt_port_log("usb: the dongle was offered again (no detach for the old one: 1 so far)");
    bt_log("script: dongle reoffer");
  }
  else if (!strcmp(w1, "dongle"))
  {
    dongle = !strcmp(w2, "in");
    dongles_seen += dongle;
    bt_log("script: dongle %s", dongle ? "in" : "out");
    if (dongle)
    {
      idle_word = 1;
      if (started && (state == NSPIRE_BT_NO_DONGLE || state == NSPIRE_BT_STARTING || state == NSPIRE_BT_FAILED))
      {
        state = NSPIRE_BT_STARTING;
        on_at = host_hw_now() + 0.3;
      }
    }
    else
    {
      dongle_out();
      if ((hung == 2 || nomem == 2) && !dead)
        failed = 0;   /* (a given-up dongle unplugged: its problem with it, as the port) */
      hung = 0;
      if (nomem == 2)
        nomem = 0;
    }
  }
  else if (!strcmp(w1, "firmware"))
    firmware_missing = !strcmp(w2, "missing");
  else if (!strcmp(w1, "nomem"))
  {
    int s;
    nomem = 2;
    failed = 1;
    for (s = 0; s < NSPIRE_BT_PADS; s++)
      pad_out(s);
    audio_out();
    state = NSPIRE_BT_FAILED;
    bt_port_log("memory: no 28704 bytes for the USB rings (Bluetooth holds 12832, its peak 12832)");
    post(NSPIRE_BT_EV_FAILED, -1, 0, 0, NULL, NULL, NSPIRE_BT_EPORT);
  }
  else if (!strcmp(w1, "keys"))
  {
    keys_mask = (uint32_t)strtoul(w2, NULL, 16);
    keys_extra = w3[0] ? (uint32_t)strtoul(w3, NULL, 16) : 0;
  }
  else if (!strcmp(w1, "hung") && !w2[0])
  {
    int s;
    hung = 1;
    hung_fires++;
    bt_port_log("watchdog: the dongle stopped answering: an ACL OUT transfer did not complete in 2000 ms");
    for (s = 0; s < NSPIRE_BT_PADS; s++)
      pad_out(s);
    audio_out();
    if (state == NSPIRE_BT_ON)
      post(NSPIRE_BT_EV_FAILED, -1, 0, 0, NULL, NULL, 0);
    state = NSPIRE_BT_STARTING;
    bt_log("script: hung");
  }
  else if (!strcmp(w1, "hung") && !strcmp(w2, "back"))
  {
    hung = 0;
    hung_backs++;
    state = NSPIRE_BT_ON;
    bt_port_log("watchdog: the dongle is back 2600 ms after it (160 polls), firmware uploaded again");
    post(NSPIRE_BT_EV_ON, -1, 0, 0, NULL, NULL, 0);
    bt_log("script: hung: back");
  }
  else if (!strcmp(w1, "hub") && !strcmp(w2, "recovering"))
  {
    /* (hub17's nspire_hub_recovering as the glue sees it: "bt hub recovering on|off") */
    hub_recovering_now = !strcmp(w3, "on");
    bt_log("script: hub recovering %s", hub_recovering_now ? "on" : "off");
  }
  else if (!strcmp(w1, "hung") && !strcmp(w2, "giveup"))
  {
    hung = 2;
    failed = 1;
    state = NSPIRE_BT_FAILED;
    bt_port_log("watchdog: no restart (it was restarted once since it came): given up");
    post(NSPIRE_BT_EV_FAILED, -1, 0, 0, NULL, NULL, NSPIRE_BT_EPORT);
    bt_log("script: hung: given up");
  }
  else if (!strcmp(w1, "fail"))
  {
    failed = 1;
    hung = 0;
    if (started)
    {
      dongle_out();
      state = NSPIRE_BT_FAILED;
      post(NSPIRE_BT_EV_FAILED, -1, 0, 0, NULL, NULL, NSPIRE_BT_EPORT);
    }
  }
  else if (!strcmp(w1, "pad") && !strcmp(w2, "in"))
  {
    struct dev *v;
    char name[32];
    rest = strstr(line, "in") + 2;
    rest += strspn(rest, " ");
    rest += strcspn(rest, " ");
    rest += strspn(rest, " ");
    snprintf(name, sizeof(name), "%s", *rest ? rest : "Wireless Controller");
    v = dev_add(NSPIRE_BT_DEV_PAD, name);
    if (v)
    {
      v->d.paired = 1;
      v->d.connected = 1;
      v->d.pad_kind = (uint8_t)kind_of(w3[0] ? w3 : "dualsense");
      if (pad_in(v->d.pad_kind, name, v->d.addr) < 0)
        v->d.connected = 0;
    }
  }
  else if (!strcmp(w1, "pad") && !strcmp(w2, "out"))
  {
    int s = w3[0] ? atoi(w3) - 1 : 0, i;
    if (s >= 0 && s < NSPIRE_BT_PADS && pads[s].connected)
      for (i = 0; i < ndev; i++)
        if (devs[i].d.connected && devs[i].d.type == NSPIRE_BT_DEV_PAD && !strcmp(devs[i].d.name, pad_names[s]))
        {
          devs[i].d.connected = 0;
          break;
        }
    pad_out(s);
  }
  else if (!strcmp(w1, "headphones") && !strcmp(w2, "in"))
  {
    struct dev *v;
    rest = strstr(line, "in") + 2;
    rest += strspn(rest, " ");
    v = dev_add(NSPIRE_BT_DEV_HEADPHONES, *rest ? rest : "WH-1000XM4");
    if (v)
    {
      v->d.paired = v->d.connected = v->d.last = 1;
      audio_in(v->d.name, v->d.addr);
    }
  }
  else if (!strcmp(w1, "audio") && w2[0])
  {
    unsigned r = 123, a = 0, k = 0;
    double sp = 1.0;
    if (sscanf(line, " audio %lf %u %u %u", &sp, &r, &a, &k) >= 1)
    {
      st_speed = sp;
      st_ring = r;
      st_ahead = a;
      st_sink = k;
    }
  }
  else if (!strcmp(w1, "headphones") && !strcmp(w2, "out"))
  {
    int i;
    for (i = 0; i < ndev; i++)
      if (devs[i].d.type == NSPIRE_BT_DEV_HEADPHONES)
        devs[i].d.connected = 0;
    audio_out();
  }
  else if (!strcmp(w1, "near"))
  {
    struct dev *v;
    int le = strstr(w2, "-le") != NULL;   /* (pad-le ...: an LE device) */
    static const char *const kinds[] = { "", "pad", "headphones", "speaker", "phone", "computer", "tv", "keyboard",
                                         "mouse" };
    int t = !strncmp(w2, "pad", 3) ? NSPIRE_BT_DEV_PAD : !strncmp(w2, "headphones", 10) ? NSPIRE_BT_DEV_HEADPHONES :
            !strncmp(w2, "unknown", 7) ? NSPIRE_BT_DEV_UNKNOWN : NSPIRE_BT_DEV_OTHER;
    int k, kind = 0;
    for (k = 1; k < (int)(sizeof(kinds) / sizeof(kinds[0])); k++)
      if (!strncmp(w2, kinds[k], strlen(kinds[k])))
        kind = k;
    rest = line;
    rest += strspn(rest, " ");
    rest += strcspn(rest, " ");
    rest += strspn(rest, " ");
    rest += strcspn(rest, " ");
    rest += strspn(rest, " ");
    rest += strcspn(rest, " ");
    rest += strspn(rest, " ");
    v = dev_add(t, rest);
    if (v)
    {
      v->near = 1;
      v->d.le = (uint8_t)le;
      v->d.rssi = (int8_t)atoi(w3);
      v->d.cod = t == NSPIRE_BT_DEV_PAD ? 0x002508 : t == NSPIRE_BT_DEV_HEADPHONES ? 0x240404 : 0x5A020C;
      if (kind)
        v->d.kind = (uint8_t)kind;   /* (NSPIRE_BT_KIND_*: the same order) */
    }
  }
  else if (!strcmp(w1, "connect") && (!strcmp(w2, "hold") || !strcmp(w2, "release")))
  {
    connect_hold = !strcmp(w2, "hold");
    if (!connect_hold && n_pend)
      pending_at = host_hw_now();   /* (the one under way ends now) */
  }
  else if (!strcmp(w1, "connect"))
    connect_fails = !strcmp(w2, "fail");
  else if (!strcmp(w1, "paired") && !strcmp(w2, "headphones"))
  {
    struct dev *v;
    rest = strstr(line, "headphones") + 10;
    rest += strspn(rest, " ");
    v = dev_add(NSPIRE_BT_DEV_HEADPHONES, *rest ? rest : "Example Headphones");
    if (v)
    {
      v->d.paired = v->d.last = 1;
      v->d.cod = 0x240404;
    }
  }
  else if (!strcmp(w1, "le") && !strcmp(w2, "refuse"))
  {
    snprintf(le_problem, sizeof(le_problem), "the dongle refuses LE connections");
    bt_log("script: le refuse");
  }
  else if (!strcmp(w1, "le") && (!strcmp(w2, "drop") || !strcmp(w2, "advertise")))
  {
    struct dev *v = NULL;
    int i, s;
    rest = strstr(line, w2) + strlen(w2);
    rest += strspn(rest, " ");
    for (i = 0; i < ndev; i++)
      if (devs[i].d.le && devs[i].d.type == NSPIRE_BT_DEV_PAD && (!*rest || !strcmp(devs[i].d.name, rest)) &&
          (!strcmp(w2, "drop") ? devs[i].d.connected : !devs[i].d.connected))
      {
        v = &devs[i];
        break;
      }
    if (!v)
      bt_log("le %s: no such LE controller", w2);
    else if (!strcmp(w2, "drop"))
    {
      for (s = 0; s < NSPIRE_BT_PADS; s++)
        if (pads[s].connected && !strcmp(pad_names[s], v->d.name))
        {
          pad_out(s);
          break;
        }
      v->d.connected = 0;
      bt_log("le drop %s (still bonded: waited for)", v->d.name);
    }
    else
    {
#if NSPIRE_BT_API_LEVEL >= 2
      int held = v->d.held;
#else
      int held = 0;
#endif
      const char *why = !v->d.paired ? "not bonded" : held ? "held" : le_problem[0] ? "LE off" :
                        n_pend ? "a player's connect runs" : nspire_bt_pad_count() >= NSPIRE_BT_PADS ? "no slot" : NULL;
      if (why)
        bt_log("le advertise %s: not taken (%s)", v->d.name, why);
      else
      {
        bt_log("le back %s (the whitelist)", v->d.name);
        if (pad_in(v->d.pad_kind ? v->d.pad_kind : USBPAD_KIND_HID, v->d.name, v->d.addr) >= 0)
          v->d.connected = 1;
      }
    }
  }
  else if (!strcmp(w1, "reconnect") && !strcmp(w2, "fail"))
  {
    if (auto_dev)
    {
      bt_log("reconnect %s failed (no event)", auto_dev->d.name);
      bt_port_log("reconnect %s failed, 0x04", auto_dev->d.name);
      set_attempt(auto_dev, 0);
      auto_dev = NULL;
    }
  }
  else if (!strcmp(w1, "reconnect"))
  {
    int i;
    for (i = 0; i < ndev; i++)
      if (devs[i].d.last && devs[i].d.type == NSPIRE_BT_DEV_HEADPHONES && !devs[i].d.connected)
        break;
    if (i < ndev && (state == NSPIRE_BT_ON) && !n_pend
#if NSPIRE_BT_API_LEVEL >= 2
        && !devs[i].d.held
#endif
        )
    {
      auto_dev = &devs[i];
      set_attempt(auto_dev, 2);
      bt_log("reconnect %s", auto_dev->d.name);
      bt_port_log("reconnect %s", auto_dev->d.name);
    }
    else
      bt_log("reconnect: nothing to reconnect now");
  }
  else if (!strcmp(w1, "stuck"))
    stuck = !strcmp(w2, "on");
  else if (!strcmp(w1, "fatal"))
    fatal_now();   /* (an assert with no guarded call to return to: the config's backstop) */
  else if (!strcmp(w1, "assert"))
  {
    /* a BTstack assert under the library's guard: the dongle given up */
    int s;
    dead = failed = 1;
    for (s = 0; s < NSPIRE_BT_PADS; s++)
      pad_out(s);
    audio_out();
    state = NSPIRE_BT_FAILED;
    post(NSPIRE_BT_EV_FAILED, -1, 0, 0, NULL, NULL, NSPIRE_BT_EPORT);
    bt_log("script: assert: the dongle given up");
  }
  else if (!strcmp(w1, "starting") || !strcmp(w1, "stopping"))
  {
    switching_kind = !strcmp(w1, "starting") ? 1 : 2;
    switching_until = host_hw_now() + atof(w2);
  }
  else if (!strcmp(w1, "startup"))
  {
    bt_port_start_step("USB: its first requests (GET_DESCRIPTOR, the request limit: 5 s at most)");
    bt_port_start_step("the firmware files read (file I/O, 4 KB a poll)");
    bt_port_start_step("BTstack power on: HCI Reset, the chipset's start (requests)");
    bt_port_start_step("the firmware upload (Realtek Download commands, one a poll)");
  }
  else if (!strcmp(w1, "startup-done"))
    bt_port_start_step("up: BTstack WORKING");
  else if (!strcmp(w1, "event"))
  {
    static const struct { const char *w; int type, err; } t[] = {
      { "on", NSPIRE_BT_EV_ON, 0 }, { "off", NSPIRE_BT_EV_OFF, 0 }, { "failed", NSPIRE_BT_EV_FAILED, NSPIRE_BT_EPORT },
      { "unplugged", NSPIRE_BT_EV_FAILED, NSPIRE_BT_ENODONGLE }, { "pad", NSPIRE_BT_EV_PAD_CONNECTED, 0 },
      { "padout", NSPIRE_BT_EV_PAD_DISCONNECTED, 0 }, { "refused", NSPIRE_BT_EV_PAD_REFUSED, 0 },
      { "headphones", NSPIRE_BT_EV_AUDIO_CONNECTED, 0 }, { "headphonesout", NSPIRE_BT_EV_AUDIO_DISCONNECTED, 0 },
      { "paired", NSPIRE_BT_EV_PAIRED, 0 }, { "failedconnect", NSPIRE_BT_EV_CONNECT_FAILED, 4 },
      { "notsupported", NSPIRE_BT_EV_NOT_SUPPORTED, 0 },
      { "restarted", NSPIRE_BT_EV_CONNECT_FAILED, NSPIRE_BT_ERESTART } };
    unsigned i;
    rest = line;
    rest += strspn(rest, " ");
    rest += strcspn(rest, " ");
    rest += strspn(rest, " ");
    rest += strcspn(rest, " ");
    rest += strspn(rest, " ");
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++)
      if (!strcmp(w2, t[i].w))
        post(t[i].type, t[i].type == NSPIRE_BT_EV_PAD_CONNECTED || t[i].type == NSPIRE_BT_EV_PAD_DISCONNECTED ? 1 : -1,
             USBPAD_KIND_DUALSENSE, 0, NULL, rest, t[i].err);
  }
  else
  {
    fprintf(stderr, "gpsp-host: unknown bt line: %s\n", line);
    exit(2);
  }
  lists();
}

static int preload_due;

/* GPSP_HOST_BT=1: the reference devices (ui_wired's bluetooth scene) */
static struct dev *near_dev(const char *addr, int type, int rssi, int le, const char *name)
{
  struct dev *v = dev_add(type, name);
  unsigned a[6], i;
  if (!v)
    return NULL;
  if (sscanf(addr, "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6)
    for (i = 0; i < 6; i++)
      v->d.addr[i] = (uint8_t)a[i];
  v->near = 1;
  v->d.rssi = (int8_t)rssi;
  v->d.le = (uint8_t)le;
  return v;
}

static void __attribute__((unused)) near_kind(const char *addr, int rssi, int le, int kind, const char *name)
{
  struct dev *v = near_dev(addr, NSPIRE_BT_DEV_OTHER, rssi, le, name);
  if (v)
    v->d.kind = (uint8_t)kind;
}

static void __attribute__((constructor)) host_bt_preload(void)
{
  const char *e = getenv("GPSP_HOST_BT");
  if (!e || strcmp(e, "1"))
    return;
  dongle = dongles_seen = 1;
  preload_due = 1;
  near_dev("98:B6:E9:44:55:66", NSPIRE_BT_DEV_PAD, -60, 0, "Pro Controller");
  /* (r19-bt: the earbuds and the LE device far off: their lines say WEAK, as distant devices do) */
  near_dev("2C:BE:EB:77:88:99", NSPIRE_BT_DEV_HEADPHONES, -80, 0, "Example Buds");
  /* (r19-bt, a phone's list): every named device with what it says it
   * is; a nameless LE one only when it shows the HID service (a pad in pairing mode, by its address); seven
   * lines in all, the page's under its tiles (ui_wired's w_bluetooth_phone shows a TV, a phone, a computer) (a list that scrolls draws its bar against the picked line's
   * block: nspire_ui_screens.c) */
  near_dev("AA:BB:CC:00:00:06", NSPIRE_BT_DEV_UNKNOWN, -79, 1, "");
  near_dev("44:16:22:5A:7B:01", NSPIRE_BT_DEV_PAD, -62, 1, "");
  near_dev("AA:BB:CC:00:00:07", NSPIRE_BT_DEV_OTHER, -71, 1, "Example Soundbar Remote Control 001");
}

/* nspire_devices.c's page, its words as they change (r19-bt: bt_tests reads them) */
void host_bt_page_said(const char *what)
{
  bt_log("page %s", what);
}

/* ---- bt_port.h ---- */
static bt_port_config_t port_cfg;

static void fatal_now(void)
{
  dead = failed = 1;
  bt_log("script: fatal: an assert outside a guarded call");
  if (port_cfg.fatal)
    port_cfg.fatal("Bluetooth stopped: an internal check failed (hci.c:1)");
}

int bt_port_dead(void)
{
  return dead;
}

int bt_port_tlv_dirty(void)
{
  return want_flush;
}

const volatile uint32_t *bt_port_idle_word(void)
{
  return &idle_word;
}

int bt_port_usb_active(void)
{
  return started && dongle;   /* (a given-up dongle is still attached: the glue asks the state too) */
}

enum bt_port_state bt_port_state(void)
{
  if (!started)
    return BT_PORT_OFF;
  if (failed)
    return BT_PORT_FAILED;
  if (!dongle)
    return BT_PORT_NO_DONGLE;
  return state == NSPIRE_BT_ON ? BT_PORT_READY : state == NSPIRE_BT_RADIO_OFF ? BT_PORT_IDLE : BT_PORT_STARTING;
}

const char *bt_port_status(void)
{
  return !dongle ? "Bluetooth: plug the USB dongle in (unplug and replug it if it is in)" : "Bluetooth ready";
}

const char *bt_port_problem(void)
{
  return failed && nomem == 1 ? "not enough memory for Bluetooth (13 KB)" :
         failed && nomem == 2 ? "not enough memory for the dongle (29 KB)" :
         failed && hung == 2 ? "the dongle stopped answering: replug it" :
         failed ? "the dongle does not finish its start-up (3 tries)" :
         firmware_missing ? "firmware file missing: /documents/zgpSP_files/bt/rtl8761bu_fw.bin.tns" : "";
}

void bt_port_info(bt_port_info_t *out)
{
  memset(out, 0, sizeof(*out));
  out->vid = dongle ? 0x2357 : 0;
  out->pid = dongle ? 0x0604 : 0;
  out->chipset = 1;
  out->firmware = firmware_missing ? 3 : dongle && state == NSPIRE_BT_ON ? 1 : 0;
  out->dongles_seen = (uint32_t)dongles_seen;
  out->hung = (uint8_t)hung;
  heap_now_b = !started ? 0 : 12832u + (dongles_seen && !nomem ? 28704u : 0);
  if (heap_now_b + (dongles_seen && !nomem ? 74716u : 0) > heap_peak_b)
    heap_peak_b = heap_now_b + (dongles_seen && !nomem ? 74716u : 0);
  out->heap_now = heap_now_b;
  out->heap_peak = heap_peak_b;
  out->mem_fail_what = (uint8_t)nomem;
  out->mem_fail_bytes = nomem == 1 ? 12832u : nomem == 2 ? 28704u : 0;
  out->watchdog_fires = (uint32_t)hung_fires;
  out->recoveries = (uint32_t)hung_backs;
}

void bt_port_log(const char *fmt, ...)
{
  va_list ap;
  char line[100];
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  snprintf(plog[plog_n++ % PLOG_N], sizeof(plog[0]), "[%4u.%03u] %s", (unsigned)host_hw_now(),
           (unsigned)(host_hw_now() * 1000) % 1000, line);
  bt_log("log %s", line);
}

uint32_t bt_port_log_count(void)
{
  return plog_n;
}

int bt_port_log_fwrite(void *file)
{
  uint32_t i, from = plog_n > PLOG_N ? plog_n - PLOG_N : 0;
  fprintf((FILE *)file, "[   0.000] bt_port: start (host)\n[   0.001] usb: driver registered\n");
  for (i = from; i < plog_n; i++)
    fprintf((FILE *)file, "%s\n", plog[i % PLOG_N]);
  return 0;
}

const char *bt_port_log_recent(unsigned back)
{
  return back < PLOG_N && back < plog_n ? plog[(plog_n - 1 - back) % PLOG_N] : "";
}

void bt_port_diag(char *buf, size_t size)
{
  snprintf(buf, size, "host bt: dongle %d state %d\n", dongle, state);
}

size_t bt_port_log_text(char *buf, size_t size)
{
  int n = snprintf(buf, size, "[   0.000] bt_port: start (host)\n[   0.001] usb: driver registered\n");
  return n < 0 ? 0 : (size_t)n;
}

/* ---- nspire_bt.h ---- */
int nspire_bt_start(const bt_port_config_t *cfg)
{
  if (cfg)
    port_cfg = *cfg;
  if (getenv("GPSP_HOST_BT_NOMEM"))
  {
    nomem = 1;   /* (the port's USB slots: the start refused) */
    failed = 1;
    state = NSPIRE_BT_FAILED;
    bt_log("start refused: not enough memory");
    return NSPIRE_BT_EPORT;
  }
  started = 1;
  bt_log("start (data %s, log level %u, keep awake %u)", cfg && cfg->data_dir ? cfg->data_dir : "-",
         cfg ? cfg->log_level : 0, cfg ? cfg->keep_usb_awake : 0);
  if (failed)
  {
    state = NSPIRE_BT_FAILED;
    return 0;
  }
  state = dongle ? NSPIRE_BT_STARTING : NSPIRE_BT_NO_DONGLE;
  if (dongle)
    on_at = host_hw_now() + 0.3;
  idle_word = dongle;
  return 0;
}

int nspire_bt_power(int on)
{
  if (!started)
    return NSPIRE_BT_ESTATE;
  bt_log("power %s", on ? "on" : "off");
  if (on)
  {
    if (state == NSPIRE_BT_RADIO_OFF)
    {
      state = dongle ? NSPIRE_BT_STARTING : NSPIRE_BT_NO_DONGLE;
      if (dongle)
        on_at = host_hw_now() + 0.3;
      idle_word = 1;
    }
    return 0;
  }
  if (state != NSPIRE_BT_RADIO_OFF)
  {
    int s;
    for (s = 0; s < NSPIRE_BT_PADS; s++)
      pad_out(s);
    audio_out();
    attempts_off();
    lists();
    state = NSPIRE_BT_RADIO_OFF;
    post(NSPIRE_BT_EV_OFF, -1, 0, 0, NULL, NULL, 0);
  }
  return 0;
}

/* As the library's stop spends its time: the polite part (the links'
 * disconnects: a third of ms, polls and OS ticks, not under 600 ms or after
 * an assert), the port's (BTstack off, USB closed: ~60 ms of OS ticks);
 * "stuck": the whole of ms waiting for a callback, then -1. Each OS tick is
 * the glue's sleep_tick (nspire_os_sleep: the model's time moves). */
/* (r19-bt) "bt starting S" / "bt stopping S": the switch waits S seconds for the dongle's start-up or its stop
 * (nspire_bt_switching); "bt startup": the bring-up's markers, as the port makes them */
int nspire_bt_switching(void)
{
  return host_hw_now() < switching_until ? switching_kind : 0;
}

void bt_port_start_step(const char *what)
{
  static unsigned n;
  char line[160];
  snprintf(line, sizeof(line), "bt start: step %u: %s", ++n, what);
  bt_log("%s", line);
  if (port_cfg.stop_step)
    port_cfg.stop_step(line);
}

/* (r19-bt) the stop's step markers, as the library's: logged, and handed to the glue (its live log) */
void bt_port_stop_step(const char *what)
{
  static unsigned n;
  char line[160];
  if (!what)
  {
    n = 0;
    return;
  }
  snprintf(line, sizeof(line), "bt stop: step %u: %s", ++n, what);
  bt_log("%s", line);
  if (port_cfg.stop_step)
    port_cfg.stop_step(line);
}

int nspire_bt_stop_within(unsigned ms)
{
  unsigned ticks = 0, i, polite = 0;
  int links = audio_on || nspire_bt_pad_count();

  bt_log("stop within %u ms (polls %lu, sound frames written %lu)", ms, polls, frames_written);
  bt_port_stop_step(NULL);
  bt_port_stop_step("the links dropped (disconnects, the scan stopped; waited for ms / 3 at most)");
  if (links && ms >= 600 && !dead)
  {
    polite = ms / 3 / 10;
    if (polite > 8)
      polite = 8;   /* (the disconnects done: a few ticks) */
    for (i = 0; i < polite; i++, ticks++)
      if (port_cfg.sleep_tick)
        port_cfg.sleep_tick();
  }
  bt_port_stop_step("USB: callbacks inside the OS waited for, the driver retired");
  for (i = 0; i < (stuck ? ms / 10 : 6); i++, ticks++)
    if (port_cfg.sleep_tick)
      port_cfg.sleep_tick();
  bt_log("stopped: %u OS ticks (%u polite), %s", ticks, polite, stuck ? "a callback still inside" : "clean");
  started = 0;
  state = NSPIRE_BT_OFF;
  idle_word = 0;
  return stuck ? -1 : 0;
}

int nspire_bt_stop_pending(void)
{
  return stuck;
}

void nspire_bt_stop(void)
{
  nspire_bt_stop_within(500);
}

int nspire_bt_state(void)
{
  return state;
}

int nspire_bt_poll(void)
{
  int r = 0, i;
  double now = host_hw_now();

  if (!started)
    return 0;
  polls++;
  if (state == NSPIRE_BT_STARTING && on_at >= 0 && now >= on_at && dongle)
  {
    state = NSPIRE_BT_ON;
    on_at = -1;
    bt_port_log("hci: state WORKING");
    post(NSPIRE_BT_EV_ON, -1, 0, 0, NULL, NULL, 0);
    if (hp_recall)
    {
      hp_recall = 0;   /* (the profiles call the last headphones again) */
      audio_in(hp_name, NULL);
    }
  }
  if (scan_until >= 0)
  {
    if (now >= scan_from + 0.5)
      for (i = 0; i < ndev; i++)
        if (devs[i].near)
        {
          devs[i].heard_at = now;
          if (!devs[i].found)
          {
            devs[i].found = 1;
            lists();
          }
        }
    if (now >= scan_until)
    {
      scan_until = -1;
      post(NSPIRE_BT_EV_SCAN_DONE, -1, 0, 0, NULL, NULL, 0);
    }
  }
  if (n_pend && now >= pending_at && !connect_hold)
  {
    struct dev *v = pend[0];
    pend_remove(0);
    set_attempt(v, 0);
    connect_now(v);
  }
  if (preload_due && state == NSPIRE_BT_ON)
  {
    preload_due = 0;
    host_bt_script("pad in dualsense DualSense Wireless Controller");
    devs[ndev - 1].d.addr[0] = 0xA0; devs[ndev - 1].d.addr[1] = 0x5A; devs[ndev - 1].d.addr[2] = 0x5D;
    devs[ndev - 1].d.addr[3] = 0x11; devs[ndev - 1].d.addr[4] = 0x22; devs[ndev - 1].d.addr[5] = 0x33;
    devs[ndev - 1].d.rssi = -50;
    lists();
  }
  for (i = 0; i < NSPIRE_BT_PADS; i++)
    if (pads[i].connected)
      pads[i].buttons = ((host_hw_pad_keys() & keys_mask) | keys_extra) & 0x3FFFFu;
  if (ev_rd != ev_wr)
    r |= NSPIRE_BT_EVENTS;
  if (want_flush)
    r |= NSPIRE_BT_WANT_FLUSH;
  if (audio_on)
    r |= NSPIRE_BT_AUDIO;
  if (dongle && state != NSPIRE_BT_OFF)
    r |= NSPIRE_BT_WANT_IRQ;
  if (state == NSPIRE_BT_STARTING)
    r |= NSPIRE_BT_WANT_TASK;
  /* the port's word: quiet once no dongle is in and nothing is pending */
  idle_word = dongle || ev_rd != ev_wr || state == NSPIRE_BT_STARTING;
  return r;
}

int nspire_bt_flush(void)
{
  bt_log("flush");
  want_flush = 0;
  return 0;
}

int nspire_bt_next_event(struct nspire_bt_event *ev)
{
  if (ev_rd == ev_wr)
    return 0;
  *ev = evq[ev_rd++ % EVQ];
  return 1;
}

const char *nspire_bt_kind_name(int kind)
{
  switch (kind)
  {
  case USBPAD_KIND_DS4: return "DualShock 4";
  case USBPAD_KIND_DUALSENSE: return "DualSense";
  case USBPAD_KIND_SWITCHPRO: return "Switch Pro";
  case USBPAD_KIND_GIP: return "Xbox One";
  default: return "Bluetooth gamepad";
  }
}

const char *nspire_bt_type_name(int type)
{
  return type == NSPIRE_BT_DEV_PAD ? "Controller" : type == NSPIRE_BT_DEV_HEADPHONES ? "Headphones" :
         type == NSPIRE_BT_DEV_OTHER ? "Other" : "Unknown";
}

/* (the library's own texts, nbt_core.c) */
int nspire_bt_event_text(const struct nspire_bt_event *ev, char *buf, unsigned size)
{
  const char *who = ev->name[0] ? ev->name : "A0:00:00:00:00:01";
  int n;
  switch (ev->type)
  {
  case NSPIRE_BT_EV_ON: n = snprintf(buf, size, "Bluetooth on"); break;
  case NSPIRE_BT_EV_OFF: n = snprintf(buf, size, "Bluetooth off"); break;
  case NSPIRE_BT_EV_FAILED:
    n = ev->err == NSPIRE_BT_ENODONGLE ? snprintf(buf, size, "Bluetooth: dongle unplugged")
        : snprintf(buf, size, "Bluetooth: %s", bt_port_problem()[0] ? bt_port_problem() : "the dongle stopped answering");
    break;
  case NSPIRE_BT_EV_PAD_CONNECTED:
    n = snprintf(buf, size, "Controller %d: %s (Bluetooth)", ev->player + 1, nspire_bt_kind_name(ev->kind));
    break;
  case NSPIRE_BT_EV_PAD_DISCONNECTED: n = snprintf(buf, size, "Controller %d disconnected", ev->player + 1); break;
  case NSPIRE_BT_EV_PAD_REFUSED: n = snprintf(buf, size, "Two controllers already: %s not used", who); break;
  case NSPIRE_BT_EV_AUDIO_CONNECTED: n = snprintf(buf, size, "Headphones: %s", who); break;
  case NSPIRE_BT_EV_AUDIO_DISCONNECTED: n = snprintf(buf, size, "Headphones disconnected"); break;
  case NSPIRE_BT_EV_PAIRED: n = snprintf(buf, size, "Paired: %s", who); break;
  case NSPIRE_BT_EV_CONNECT_FAILED: n = snprintf(buf, size, "Couldn't connect to %s", who); break;
  case NSPIRE_BT_EV_NOT_SUPPORTED: n = snprintf(buf, size, "%s: not a controller or headphones", who); break;
  case NSPIRE_BT_EV_SCAN_DONE: n = snprintf(buf, size, "Bluetooth: %d devices found", scan_n); break;
  default: n = snprintf(buf, size, "Bluetooth"); break;
  }
  return n < 0 ? 0 : n >= (int)size ? (int)size - 1 : n;
}

const char *nspire_bt_status(void)
{
  static char text[96];
  int i, n;

  switch (state)
  {
  case NSPIRE_BT_OFF: return "Bluetooth: off";
  case NSPIRE_BT_STARTING: return "Bluetooth: starting";
  case NSPIRE_BT_RADIO_OFF: return "Bluetooth: radio off";
  case NSPIRE_BT_STOPPING: return "Bluetooth: stopping";
  case NSPIRE_BT_FAILED: snprintf(text, sizeof(text), "Bluetooth: %s", bt_port_problem()); return text;
  case NSPIRE_BT_NO_DONGLE: return bt_port_status();
  default: break;
  }
  n = snprintf(text, sizeof(text), "Bluetooth:");
  for (i = 0; i < NSPIRE_BT_PADS; i++)
    if (pads[i].connected)
      n += snprintf(text + n, sizeof(text) - (unsigned)n, "%s P%d %s", n > 10 ? "," : "", i + 1,
                    nspire_bt_kind_name(pads[i].kind));
  if (audio_on)
    n += snprintf(text + n, sizeof(text) - (unsigned)n, "%s headphones %s", n > 10 ? "," : "", hp_name);
  if (n <= 10)
    snprintf(text + n, sizeof(text) - (unsigned)n, " on, no devices");
  return text;
}

int nspire_bt_diag(char *buf, unsigned size)
{
  return snprintf(buf, size, "host bt: polls %lu, writes %lu\n", polls, writes);
}

/* search, connect */
int nspire_bt_scan_start(unsigned seconds)
{
  int i;
  if (state != NSPIRE_BT_ON)
    return NSPIRE_BT_ESTATE;
  for (i = 0; i < ndev; i++)
    if (!devs[i].found || host_hw_now() - devs[i].heard_at > 15)
      devs[i].found = 0;
  scan_from = host_hw_now();
  scan_until = scan_from + seconds;
  lists();
  bt_log("search %u s", seconds);
  return 0;
}

void nspire_bt_scan_stop(void)
{
  if (scan_until >= 0)
    bt_log("search stopped");
  scan_until = -1;
}

int nspire_bt_scanning(void)
{
  return scan_until >= 0;
}

int nspire_bt_scan_count(void)
{
  return scan_n;
}

const struct nspire_bt_device *nspire_bt_scan_result(int i)
{
  return i >= 0 && i < scan_n ? &scan_list[i] : NULL;
}

int nspire_bt_connect(const uint8_t addr[6])
{
  struct dev *v = dev_by_addr(addr);
  if (state != NSPIRE_BT_ON)
    return NSPIRE_BT_ESTATE;
  if (!v)
    return NSPIRE_BT_ENOTFOUND;
  if (v->d.le && le_problem[0])
  {
    /* (the library: LE off on this dongle; r19-bt: any other LE device is tried, looked at for a HID service
     * before any pairing, and let go with EV_NOT_SUPPORTED when it has none) */
    bt_log("connect %s: refused (%s)", v->d.name[0] ? v->d.name : "(address only)", le_problem);
    return NSPIRE_BT_ENOTSUP;
  }
  set_held(v, 0);
  if (auto_dev == v)
  {
    auto_dev = NULL;            /* (the reconnect under way is the player's now) */
    bt_log("reconnect %s: the player's now", v->d.name);
  }
  else if (auto_dev)
  {
    /* one attempt at a time: the reconnect stops first (the library cancels its page) */
    bt_log("reconnect %s: stopped for the player's connect", auto_dev->d.name);
    set_attempt(auto_dev, 0);
    auto_dev = NULL;
  }
  bt_log("connect %s", v->d.name[0] ? v->d.name : "(address only)");
  set_attempt(v, 1);
  lists();
  if (pend_find(v) >= 0)
    return 0;
  if (n_pend == NPEND)
    return NSPIRE_BT_ENOMEM;
  if (n_pend)
    bt_log("connect %s: waits its turn", v->d.name);
  pend[n_pend++] = v;
  if (n_pend == 1)
    pending_at = host_hw_now() + 1.0;   /* (a page, a pairing, the profiles: a second) */
  return 0;
}

#if NSPIRE_BT_API_LEVEL >= 2
const char *nspire_bt_le_problem(void)
{
  return le_problem;
}

int nspire_bt_cancel(const uint8_t addr[6])
{
  struct dev *v = dev_by_addr(addr);
  int i;
  if (state != NSPIRE_BT_ON)
    return NSPIRE_BT_ESTATE;
  if (!v)
    return NSPIRE_BT_ENOTFOUND;
  i = pend_find(v);
  if (i < 0 && auto_dev != v)
    return NSPIRE_BT_ENOTFOUND;
  bt_log("cancel %s (%s)", v->d.name[0] ? v->d.name : "(address only)",
         auto_dev == v ? "the reconnect" : i == 0 ? "under way" : "waiting its turn");
  if (auto_dev == v)
    auto_dev = NULL;
  pend_remove(i);
  set_attempt(v, 0);
  set_held(v, 1);               /* (found, or paired, as before: no pairing was made yet) */
  lists();
  return 0;
}
#endif

static void connect_now(struct dev *v)
{
  const uint8_t *addr = v->d.addr;
  if (connect_fails)
  {
    post(NSPIRE_BT_EV_CONNECT_FAILED, -1, 0, v->d.type, addr, v->d.name, 4);
    return;
  }
  if (v->d.type == NSPIRE_BT_DEV_PAD)
  {
    if (!v->d.paired)
      post(NSPIRE_BT_EV_PAIRED, -1, 0, v->d.type, addr, v->d.name, 0);
    v->d.paired = 1;
    want_flush = 1;
    v->d.pad_kind = (uint8_t)(strstr(v->d.name, "Pro Controller") ? USBPAD_KIND_SWITCHPRO :
                              strstr(v->d.name, "DUALSHOCK") ? USBPAD_KIND_DS4 :
                              strstr(v->d.name, "Xbox") ? USBPAD_KIND_GIP :
                              v->d.le ? USBPAD_KIND_HID : USBPAD_KIND_DUALSENSE);
    if (v->d.le)
      bt_log("le paired %s (bonded), kind %d", v->d.name, v->d.pad_kind);
    if (pad_in(v->d.pad_kind, v->d.name, addr) >= 0)
      v->d.connected = 1;
  }
  else if (v->d.type == NSPIRE_BT_DEV_HEADPHONES)
  {
    if (!v->d.paired)
      post(NSPIRE_BT_EV_PAIRED, -1, 0, v->d.type, addr, v->d.name, 0);
    v->d.paired = v->d.connected = v->d.last = 1;
    want_flush = 1;
    audio_in(v->d.name, addr);
  }
  else
    post(NSPIRE_BT_EV_NOT_SUPPORTED, -1, 0, v->d.type, addr, v->d.name, 0);
  lists();
}

int nspire_bt_disconnect(const uint8_t addr[6])
{
  struct dev *v = dev_by_addr(addr);
  int s;
  if (!v)
    return NSPIRE_BT_ENOTFOUND;
  bt_log("disconnect %s", v->d.name);
  if (v->d.type == NSPIRE_BT_DEV_HEADPHONES)
    audio_out();
  else
    for (s = 0; s < NSPIRE_BT_PADS; s++)
      if (pads[s].connected && !strcmp(pad_names[s], v->d.name))
        pad_out(s);
  v->d.connected = 0;
  lists();
  return 0;
}

int nspire_bt_forget(const uint8_t addr[6])
{
  struct dev *v = dev_by_addr(addr);
  if (!v)
    return NSPIRE_BT_ENOTFOUND;
  if (v->d.connected)
    nspire_bt_disconnect(addr);
  if (pend_find(v) >= 0)
    pend_remove(pend_find(v));
  if (auto_dev == v)
    auto_dev = NULL;
  set_attempt(v, 0);
  bt_log("forget %s", v->d.name);
  v->d.paired = 0;
  want_flush = 1;
  lists();
  return 0;
}

unsigned nspire_bt_features(void)
{
  return NSPIRE_BT_FEAT_LE_SCAN | NSPIRE_BT_FEAT_LE_CONNECT;   /* (r19-bt: gpSP's library: full LE, no AVRCP) */
}

int nspire_bt_paired_count(void)
{
  return paired_n;
}

const struct nspire_bt_device *nspire_bt_paired(int i)
{
  return i >= 0 && i < paired_n ? &paired_list[i] : NULL;
}

/* pads */
const struct usbpad_state *nspire_bt_pad(int slot)
{
  static const struct usbpad_state none;
  return slot >= 0 && slot < NSPIRE_BT_PADS ? &pads[slot] : &none;
}

int nspire_bt_pad_count(void)
{
  int i, n = 0;
  for (i = 0; i < NSPIRE_BT_PADS; i++)
    n += pads[i].connected;
  return n;
}

const char *nspire_bt_pad_name(int slot)
{
  return slot >= 0 && slot < NSPIRE_BT_PADS && pads[slot].connected ? pad_names[slot] : "";
}

int nspire_bt_set_pad_config(const struct usbpad_config *cfg)
{
  (void)cfg;
  return 0;
}

int nspire_bt_pad_set_player(int slot, int player)
{
  bt_log("lights slot %d player %d", slot, player);
  return 0;
}

int nspire_bt_pad_rumble(int slot, uint8_t strong, uint8_t weak)
{
  (void)slot; (void)strong; (void)weak;
  return 0;
}

/* sound */
int nspire_bt_audio_active(void)
{
  return audio_on;
}

void nspire_bt_audio_frame_begin(int mode)
{
  if (mode != audio_mode)
    bt_log("audio mode %d", mode);
  audio_mode = mode;
}

unsigned nspire_bt_audio_write(const int16_t *lr, unsigned frames)
{
  (void)lr;
  writes++;
  frames_written += frames;
  return frames;
}

unsigned nspire_bt_audio_write_mix(uint32_t *w1, unsigned n1, uint32_t *w2, unsigned n2, unsigned flags)
{
  (void)w1; (void)w2; (void)flags;
  writes++;
  frames_written += n1 + n2;
  return n1 + n2;
}

void nspire_bt_audio_set_input_rate(uint32_t hz)
{
  bt_log("input rate %u", (unsigned)hz);
}

void nspire_bt_audio_set_volume(unsigned vol256)
{
  if (vol256 != volume)
    bt_log("volume %u", vol256);
  volume = vol256;
}

void nspire_bt_audio_set_latency(unsigned ms)
{
  (void)ms;
}

static unsigned codec_bitpool;
void nspire_bt_audio_set_codec(int mono, unsigned bitpool)
{
  if (mono != codec_mono || bitpool != codec_bitpool)
    bt_log("codec %s bitpool %u", mono ? "mono" : "joint stereo", bitpool);
  codec_mono = mono;
  codec_bitpool = bitpool;
}

void nspire_bt_audio_stats(struct nspire_bt_audio_stats *s)
{
  memset(s, 0, sizeof(*s));
  if (audio_on)
  {
    s->rate = 44100;
    s->channels = codec_mono == 1 ? 1 : 2;
    s->bitpool = codec_bitpool ? (uint8_t)codec_bitpool : 35;
    s->target_ms = 123;
    s->ring_ms = st_ring;
    s->ahead_ms = st_ahead;
    s->sink_delay_ms = st_sink;
    s->speed = (uint32_t)(st_speed * 65536.0 + 0.5);
  }
}
