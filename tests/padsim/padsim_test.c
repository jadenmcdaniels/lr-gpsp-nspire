/* R10b (2026-11-15): gpSP's pad glue with the REAL nspire-usbpad (its five
 * classes and Backend A, from USBPAD_DIR/src) on the driver's own fake TI
 * USB stack (USBPAD_DIR/tests/os_fake: NetBSD's probing order, TI's buffer
 * descriptors and status codes, and its checks on how it is used: control
 * transfers only from attach, no freed pipe or transfer touched, nothing
 * left open after a detach). The pads are the driver's corpus: real
 * descriptors (Xbox 360, 8BitDo Ultimate 2 as R10a saw it, Xbox Series,
 * DualShock 4 v2, DualSense, Switch Pro) and real input reports (Android
 * CTS, Linux xpad captures). Around them gpSP's port watch, arbiter,
 * Controller status and pad log (the frontend_tests objects), and the
 * calculator's USB registers as they read for each step (host_stubs.c's).
 *
 * "In a game": each frame reads the keys (the glue polls the driver),
 * gives the OS its spare time (the frame's sleep), shows the glue's
 * messages and then makes the point between frames (the arbiter's tick and
 * the come-up's OS time in one go). What reaches gpSP is the fifth key word
 * (KEY_PAD_*, usbpad.h's bits): pad_tests.py shows those reach the GBA.
 *
 *   padsim_test WORKDIR
 *
 * Checked: every family plugged in during a game attaches within 1 s of
 * virtual time, its bottom and right face buttons arrive as Pad B / Pad A
 * (the defaults by position: GBA B and A), its family's button names, and
 * nothing is held after its unplug; a pad whose bring-up fails (the OS
 * refuses its pipes) is named on the Controller page with its step, the pad
 * log has the step, endpoint, error and the driver's trace, and a good pad
 * plugged next works at once; ten unplug-and-plug rounds across families
 * leave no pipe or transfer open and no violation of the OS's rules; R10a's
 * halt (a device nobody claims: the fake OS halts its host controller)
 * is logged and the come-up gives the OS its time.
 * R11u (2026-11-15): the fake's default rule is TI's (an attach that fails
 * after a nonzero match is a breach, nothing offered after it until the
 * adapter comes out): every round checks none happened; R11u's sound card
 * (whole, and its HID interface alone) is no failure ("Not a controller",
 * no toast, its try and come-up ended as such), and every family plugged
 * in after it with the adapter kept in connects within 1 s. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "common.h"
#include "nspire.h"
#include "nspire_keys.h"
#include "nspire_usbpad_glue.h"
#include "nspire_padstatus.h"
#include "nspire_padlog.h"
#include "nspire_arbiter.h"
#include "nspire_usbport.h"
#include "nspire_bind.h"
#include "usbpad.h"
#include "fake_os.h"

#include "corpus/cfg_x360.h"
#include "corpus/cfg_r10a.h"
#include "corpus/gip_real.h"
#include "corpus/gip_reports.h"
#include "corpus/cfg_sony_real.h"
#include "corpus/sony_cts.h"
#include "corpus/cfg_switchpro.h"
#include "corpus/switchpro_cts.h"
#include "corpus/cfg_keyboard.h"
#include "corpus/cfg_soundcard.h"

/* host_stubs.c's calculator */
extern u32 unit_ticks;
extern int unit_usb_plugged;
extern uint32_t unit_usb_bits_set;
extern int unit_usb_raw_kind;
extern uint32_t unit_usb_raw[6];
extern void (*unit_os_sleep_hook)(void);
extern char unit_documents_dir[600];
extern char unit_osd_log[2048];
extern long unit_os_sleeps;

static int checks, failures;
#define CHECK(cond, ...)                                                                                               \
  do                                                                                                                   \
  {                                                                                                                    \
    checks++;                                                                                                          \
    if (!(cond))                                                                                                       \
    {                                                                                                                  \
      failures++;                                                                                                      \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                      \
      printf(__VA_ARGS__);                                                                                             \
      printf("\n");                                                                                                    \
    }                                                                                                                  \
  } while (0)

/* ---- What the glue needs of nspire_os.c (gpsp-host's host_platform.c) ---- */

static u32 slept_at;
uint32_t nspire_os_since_sleep(void) { return unit_ticks - slept_at; }
/* (speed9a) the pad log's OS gap: measured at each sleep in nspire_os.c */
uint32_t nspire_os_gap_longest(void) { return 0; }
/* (speed9a) the pad log's line on the OS tick's phase */
void nspire_os_tick_rule_report(char *text, unsigned size) { snprintf(text, size, "OS tick: (padsim)"); }
uint32_t nspire_os_near_wait(uint32_t since, uint32_t from, uint32_t force)
{
  (void)from;
  return since < force ? force - since : 0;
}
int nspire_os_sleep_near(uint32_t max_wait)
{
  (void)max_wait;
  return nspire_os_sleep();
}
int keys_raw_down(void) { return 0; }
void nspire_reset_timing(void) {}
/* (2026-11-17, final/speed8) the frame loop's pad ticks: none here, the
 * glue's own rule as before (the keys are read without a game's frames) */
int nspire_frameskip_os_keys(u32 force) { (void)force; return 0; }
void nspire_frameskip_os_none(void) {}
int nspire_os_keypad_keep_off(void) { return 0; }
void nspire_os_after_os_time(void) { }   /* (2026-11-17) the pad's interrupt window (nspire_usbpad_glue.c pad_irq_window) */
void nspire_exit_begin(void) {}
void nspire_exit_step(const char *what) { (void)what; }
/* R11w: the exit's USB budget and its way out (nspire_exit.c's), counted */
static int exit_timeouts;
static const char *exit_timeout_step;
unsigned nspire_exit_usb_left_ms(void) { return 4000; }
void nspire_exit_usb_timed_out(const char *step)
{
  exit_timeouts++;
  exit_timeout_step = step;
}
void host_os_irq_window(void) {}

/* The OS's tick: its USB task runs (inline), the clock moves a tick. */
static void os_tick(void)
{
  slept_at = unit_ticks;
  fos_sleep();
}

/* ---- The USB registers, by what is on the port (a CX II's FOTG210) ---- */

enum { WIRES_EMPTY, WIRES_PAD, WIRES_HALTED };

static void wires(int what)
{
  unit_usb_plugged = 1;   /* the OTG adapter stays in */
  unit_usb_raw_kind = 2;
  memset(unit_usb_raw, 0, sizeof(unit_usb_raw));
  unit_usb_raw[0] = 1u << 19;   /* OTGCSR: the host's end, bus power on */
  switch (what)
  {
  case WIRES_EMPTY:
    unit_usb_bits_set = 0x20000000u;
    unit_usb_raw[2] = 0x00080031u;
    break;
  case WIRES_PAD:
    unit_usb_bits_set = 0x20000500u;   /* PORTSC connected and enabled as its bits 8 and 10 */
    unit_usb_raw[2] = 0x00080031u;
    unit_usb_raw[4] = 0x00000005u;
    break;
  case WIRES_HALTED:   /* R10a 685064 */
    unit_usb_bits_set = 0x20000500u;
    unit_usb_raw[2] = 0x00080000u;
    unit_usb_raw[4] = 0x0000088du;
    break;
  }
}

/* ---- A game's frame ---- */

static u32 keys;
static int dev = -1;
static const uint8_t *streaming;
static unsigned streaming_len;

static void frame(void)
{
  unit_ticks += 32768 / 60;
  keys = nspire_usbpad_keys();
  /* the pad streams its report; the frame's spare time is the OS's */
  if (dev >= 0 && streaming)
    fos_report(dev, streaming, streaming_len);
  nspire_os_sleep();
  nspire_usbpad_frame(0);
  nspire_arb_frame();
}

static void frames(int n)
{
  while (n-- > 0)
    frame();
}

static double ms_since(u32 t) { return (double)(u32)(unit_ticks - t) * 1000.0 / 32768.0; }

/* ---- The pads ---- */

struct fam
{
  const char *name;   /* usbpad_kind_name */
  int kind, family;
  struct fos_spec spec;
  const uint8_t *idle, *south, *east, *first;
  unsigned len, first_len;
  const char *south_name, *east_name;   /* bind_pad_name of Pad B and Pad A */
};

static const uint8_t x_idle[20] = { 0x00, 0x14 };
static const uint8_t x_south[20] = { 0x00, 0x14, 0x00, 0x10 };   /* A */
static const uint8_t x_east[20] = { 0x00, 0x14, 0x00, 0x20 };    /* B */

static const struct fam fams[] = {
  { "Xbox 360", USBPAD_KIND_XINPUT, PAD_FAMILY_XBOX360,
    { CORPUS_X360_VID, CORPUS_X360_PID, 0x0114, corpus_x360_cfg, sizeof corpus_x360_cfg, NULL, 0, 0, 0xFF },
    x_idle, x_south, x_east, NULL, 20, 0, "A", "B" },
  /* R10a's 8BitDo Ultimate 2: it must keep working exactly as it does */
  { "Xbox 360", USBPAD_KIND_XINPUT, PAD_FAMILY_XBOX360,
    { 0x2dc8, 0x310b, 0x0114, corpus_u2_r10a_cfg, sizeof corpus_u2_r10a_cfg, corpus_u2_r10a_if2_rdesc,
      sizeof corpus_u2_r10a_if2_rdesc, 0, 0x00 },
    x_idle, x_south, x_east, NULL, 20, 0, "A", "B" },
  { "Xbox One", USBPAD_KIND_GIP, PAD_FAMILY_XBOX,
    { CORPUS_GIP_SERIES_VID, CORPUS_GIP_SERIES_PID, CORPUS_GIP_SERIES_REL, corpus_gip_series_cfg,
      sizeof corpus_gip_series_cfg, NULL, 0, 0, 0xFF },
    corpus_gip_pdp_reports[0].b, corpus_gip_pdp_reports[15].b, corpus_gip_pdp_reports[16].b, corpus_gip_announce, 18,
    sizeof corpus_gip_announce, "A", "B" },
  { "DualShock 4", USBPAD_KIND_DS4, PAD_FAMILY_DS4,
    { 0x054c, CORPUS_DS4V2_REAL_PID, CORPUS_DS4V2_REAL_REL, corpus_ds4v2_cfg, sizeof corpus_ds4v2_cfg, NULL, 0, 0, 0 },
    cts_ds4[1].b, cts_ds4[0].b, cts_ds4[2].b, NULL, 64, 0, "Cross", "Circle" },
  { "DualSense", USBPAD_KIND_DUALSENSE, PAD_FAMILY_DUALSENSE,
    { 0x054c, CORPUS_DUALSENSE_REAL_PID, CORPUS_DUALSENSE_REAL_REL, corpus_dualsense_cfg, sizeof corpus_dualsense_cfg,
      NULL, 0, 0, 0 },
    cts_dualsense[1].b, cts_dualsense[0].b, cts_dualsense[2].b, NULL, 64, 0, "Cross", "Circle" },
  { "Switch Pro", USBPAD_KIND_SWITCHPRO, PAD_FAMILY_SWITCH,
    { CORPUS_SWPRO_VID, CORPUS_SWPRO_PID, 0x0200, corpus_swpro_cfg, sizeof corpus_swpro_cfg, NULL, 0, 0, 0 },
    cts_swpro[1].b, cts_swpro[0].b, cts_swpro[2].b, NULL, 64, 0, "B", "A" },
};
#define FAMS (int)(sizeof(fams) / sizeof(fams[0]))

static unsigned connected_toasts(void)
{
  unsigned n = 0;
  const char *p = unit_osd_log;

  while ((p = strstr(p, "Controller connected")) != NULL)
  {
    n++;
    p++;
  }
  return n;
}

/* Plugs f in during the game; frames until it attaches (max 3 s). The ms
 * from the plug to its attach, or -1. */
static double plug(const struct fam *f)
{
  u32 t0 = unit_ticks;
  int i;

  wires(WIRES_PAD);
  streaming = NULL;
  dev = fos_plug(&f->spec);
  for (i = 0; i < 180 && !usbpad_count(); i++)
    frame();
  if (!usbpad_count())
    return -1;
  if (f->first)
    fos_report(dev, f->first, f->first_len);
  return ms_since(t0);
}

static void unplug(void)
{
  if (dev >= 0)
    fos_unplug(dev);
  dev = -1;
  streaming = NULL;
  wires(WIRES_EMPTY);
  frames(30);
}

static void hold(const uint8_t *rep, unsigned len, int n)
{
  streaming = rep;
  streaming_len = len;
  frames(n);
}

static const char *log_text(void)
{
  static char text[96 * 1024];

  nspire_padlog_text(text, sizeof(text), "padsim");
  return text;
}

/* One family: plugged in, its two face buttons, its names, unplugged with a
 * button held. */
static void one_pad(const struct fam *f, const char *round)
{
  double ms = plug(f);
  struct pad_driver_view d;

  CHECK(ms >= 0 && ms <= 1000, "%s %s (%04x:%04x): attached %.0f ms after the plug (<= 1000)", round, f->name, f->spec.vid,
        f->spec.pid, ms);
  if (ms < 0)
  {
    unplug();
    return;
  }
  printf("  %s %s %04x:%04x: attached %.0f ms after the plug\n", round, f->name, f->spec.vid, f->spec.pid, ms);
  CHECK(usbpad_state(0)->kind == f->kind && !strcmp(nspire_usbpad_name(), f->name), "%s %s: kind %d, name %s", round,
        f->name, usbpad_state(0)->kind, nspire_usbpad_name());
  hold(f->south, f->len, 8);
  CHECK(keys == USBPAD_SOUTH, "%s %s: the bottom face button is Pad B (GBA B): keys %05x", round, f->name, (unsigned)keys);
  hold(f->east, f->len, 8);
  CHECK(keys == USBPAD_EAST, "%s %s: the right face button is Pad A (GBA A): keys %05x", round, f->name, (unsigned)keys);
  hold(f->idle, f->len, 8);
  CHECK(keys == 0, "%s %s: let go: keys %05x", round, f->name, (unsigned)keys);
  CHECK(nspire_usbpad_family() == f->family && !strcmp(bind_pad_name(KEY_PAD_B, nspire_usbpad_family()), f->south_name) &&
        !strcmp(bind_pad_name(KEY_PAD_A, nspire_usbpad_family()), f->east_name),
        "%s %s: its button names (family %d: %s, %s)", round, f->name, nspire_usbpad_family(),
        bind_pad_name(KEY_PAD_B, nspire_usbpad_family()), bind_pad_name(KEY_PAD_A, nspire_usbpad_family()));
  nspire_usbpad_status(&d);
  CHECK(d.pads == 1 && !strstr(d.detail, "didn't") && !strstr(d.detail, "failed"), "%s %s: the page's Driver word: %s",
        round, f->name, d.detail);
  CHECK(fos_stats()->halted == 0, "%s %s: the OS's host never halted", round, f->name);
  /* unplugged with B held: let go at once */
  hold(f->south, f->len, 4);
  fos_unplug(dev);
  dev = -1;
  streaming = NULL;
  wires(WIRES_EMPTY);
  frames(3);
  CHECK(keys == 0 && usbpad_count() == 0, "%s %s: unplugged with B held: let go (keys %05x, %d pads)", round, f->name,
        (unsigned)keys, usbpad_count());
  frames(30);
  CHECK(fos_stats()->pipes_open == 0 && fos_stats()->xfers_open == 0, "%s %s: nothing left open (%u pipes, %u xfers)",
        round, f->name, fos_stats()->pipes_open, fos_stats()->xfers_open);
  CHECK(fos_stats()->violations == 0, "%s %s: no violation of the OS's rules: %s", round, f->name, fos_stats()->violation);
  /* R11u: TI's rule, the fake's default: no attach refused after a match
   * (a breach stops every later offer until the adapter comes out) */
  CHECK(fos_stats()->breaches == 0 && fos_stats()->not_offered == 0, "%s %s: no refused attach (%u breaches, %u not offered)",
        round, f->name, fos_stats()->breaches, fos_stats()->not_offered);
}

static void test_each_family(void)
{
  int i;
  unsigned toasts = connected_toasts();

  printf("each family plugged in during a game\n");
  for (i = 0; i < FAMS; i++)
    one_pad(&fams[i], "family");
  CHECK(connected_toasts() - toasts == (unsigned)FAMS, "each family: a connected message each (%u)",
        connected_toasts() - toasts);
}

/* R10a's Switch Pro as it might still go: the OS refuses the pipes (open
 * NOT SUPPORTED, every transfer refused). The driver keeps it, quiet; gpSP
 * names it and the step, and the next pad works at once. */
static void test_fail_then_good(void)
{
  struct fos_spec bad = fams[5].spec;
  struct pad_driver_view d;
  const char *text;
  u32 t0;
  int i, saw = 0;
  unsigned refused0 = fos_stats()->attach_failed;

  printf("a pad whose bring-up fails, then a good one\n");
  fos_r10a_halt(1);   /* the R10a model: a device left without a driver halts the host for good */
  bad.flags = FOS_PIPE_UNSUPPORTED | FOS_OUT_REFUSE;
  wires(WIRES_PAD);
  dev = fos_plug(&bad);
  for (i = 0; i < 120; i++)
  {
    frame();
    if (strstr(unit_osd_log, "Switch Pro didn't start"))
      saw = 1;
  }
  CHECK(saw, "fail: the message names the pad: %s", unit_osd_log);
  CHECK(usbpad_count() == 0 && fos_stats()->attach_failed == refused0 && fos_stats()->halted == 0,
        "fail: kept quietly, no attach refused, the host not halted (%d, %u, %u)", usbpad_count(),
        fos_stats()->attach_failed - refused0, fos_stats()->halted);
  nspire_usbpad_status(&d);
  CHECK(!strncmp(d.detail, "Switch Pro: ", 12) && strstr(d.detail, "(") && strlen(d.detail) < sizeof(d.detail) - 1,
        "fail: the page's Driver line names the pad and the step: %s", d.detail);
  CHECK(nspire_usbport_os() == PORT_OS_OFFER && nspire_arb_owner() == ARB_OTHER,
        "fail: while it is in, the port is its (no power cycle for it): os %d holder %d", nspire_usbport_os(), nspire_arb_owner());
  text = log_text();
  CHECK(strstr(text, "driver: not supported, slot -1, 057e:2009, kind 6; failed at ") != NULL,
        "fail: the pad log's event has the step, endpoint and error:\n%s", text);
  CHECK(strstr(text, "U p0 057e:2009 claim: ok") && strstr(text, "U p0 057e:2009 in_open ep81 64B packets: OS 11 not supported") &&
        strstr(text, "U p0 057e:2009 quiet after in_open: OS 11 not supported"),
        "fail: the driver's trace in the pad log:\n%s", text);
  printf("  the page: %s\n", d.detail);
  fos_unplug(dev);
  dev = -1;
  wires(WIRES_EMPTY);
  frames(30);
  CHECK(nspire_usbport_os() == PORT_OS_NONE && nspire_arb_owner() != ARB_OTHER, "fail: unplugged: the port is free (os %d holder %d)",
        nspire_usbport_os(), nspire_arb_owner());
  nspire_usbpad_status(&d);
  CHECK(!strstr(d.detail, "Switch Pro"), "fail: the failure goes with it: %s", d.detail);
  CHECK(fos_stats()->pipes_open == 0 && fos_stats()->xfers_open == 0, "fail: its pipes closed at the detach (%u, %u)",
        fos_stats()->pipes_open, fos_stats()->xfers_open);
  t0 = unit_ticks;
  one_pad(&fams[4], "after the failure");
  CHECK(ms_since(t0) < 3000, "after the failure: the DualSense's round in %.0f ms", ms_since(t0));
  fos_r10a_halt(0);
}

/* Ten rounds over the families (the 8BitDo first), a failing pad in round
 * 6, nothing left open, no violation. */
static void test_rounds(void)
{
  int r;
  char name[32];

  printf("ten unplug-and-plug rounds across the families\n");
  for (r = 0; r < 10; r++)
  {
    snprintf(name, sizeof(name), "round %d", r + 1);
    if (r == 5)
    {
      struct fos_spec bad = fams[3].spec;   /* a DS4 whose IN pipe the OS won't open */

      bad.flags = FOS_PIPE_UNSUPPORTED;
      wires(WIRES_PAD);
      dev = fos_plug(&bad);
      frames(90);
      CHECK(usbpad_count() == 0, "round 6: the failing DS4 holds no slot");
      unplug();
    }
    one_pad(&fams[(r + 1) % FAMS], name);
  }
  CHECK(fos_stats()->violations == 0, "rounds: no violation: %s", fos_stats()->violation);
}

/* R11u (2026-11-15): the session gpSP started with a USB sound card in the
 * adapter (0d8c:0014: if0-if2 audio, if3 its volume keys). R11u's driver
 * took if3 at match, refused its attach (not a gamepad) and TI's OS offered
 * nothing more for 705 s: six pads plugged in after it, the adapter kept
 * in, each "the OS enabled the port and offered nothing". On the fake (TI's
 * rule is its default) that refusal is a breach and every pad below would
 * be not offered. Now: the card, whole (match says 0 to all four: a HID
 * interface beside audio ones) and its HID interface alone (taken, its
 * report descriptor read, kept quietly), is not a controller: no "didn't
 * start", no "USB not working", the page says not_a_pad / "Not a
 * controller"; then every family swapped in, each connected within 1 s. */
static void r11u_card(int hid_only, const char *what)
{
  struct fos_spec card;
  struct pad_status s;
  struct pad_driver_view d;
  const char *text;
  enum pad_verdict v = PV_NOTHING;
  int i;

  memset(&card, 0, sizeof(card));
  card.vid = CORPUS_SND_VID;
  card.pid = CORPUS_SND_PID;
  card.rel = CORPUS_SND_REL;
  card.cfg = hid_only ? corpus_snd_hidonly_cfg : corpus_snd_cfg;
  card.cfg_len = hid_only ? sizeof corpus_snd_hidonly_cfg : sizeof corpus_snd_cfg;
  card.rdesc = corpus_snd_rdesc;
  card.rdesc_len = sizeof corpus_snd_rdesc;
  unit_osd_log[0] = 0;
  wires(WIRES_PAD);
  dev = fos_plug(&card);
  for (i = 0; i < 60 * 15; i++)   /* 15 s in the game: past the 12 s wait, the toasts' 3 s and 20 s */
    frame();
  CHECK(fos_stats()->breaches == 0 && fos_stats()->attach_failed == 0, "%s: no refused attach (%u)", what,
        fos_stats()->breaches);
  CHECK(!strstr(unit_osd_log, PADSTATUS_TOAST_TEXT) && !strstr(unit_osd_log, "didn't start") &&
        !strstr(unit_osd_log, "not supported"), "%s: no failure message: %s", what, unit_osd_log);
  nspire_padstatus_read(&s);
  v = nspire_padstatus_verdict(&s);
  CHECK(v == PV_NOT_A_PAD && !nspire_padstatus_failing(v), "%s: the page's verdict: %s", what, nspire_padstatus_name(v));
  nspire_usbpad_status(&d);
  CHECK(hid_only ? (d.not_pad && !strcmp(d.detail, "Not a controller (0d8c:0014 if 3)")) : (!d.not_pad && d.score == 0),
        "%s: the page's Driver word: '%s' (not_pad %d, score %d)", what, d.detail, d.not_pad, d.score);
  text = log_text();
  {
    /* this card's part of the log: from its come-up's start on */
    const char *p = text, *q;

    while ((q = strstr(p + 1, "come-up: a device came onto the adapter's wires")) != NULL)
      p = q;
    text = p;
  }
  CHECK(strstr(text, "try ended: the device is not a controller (left alone; nothing failed)") != NULL,
        "%s: the pad log ends its try as no failure", what);
  CHECK(strstr(text, "the device is not a controller (left alone, no more OS time for it)") != NULL,
        "%s: the come-up ends: not a controller", what);
  CHECK(!strstr(text, "no pad came in") && !strstr(text, "try ended: the OS offered nothing"),
        "%s: no 12 s wait ran out", what);
  if (failures)
    printf("%s\n", text + (strlen(text) > 2500 ? strlen(text) - 2500 : 0));
  if (hid_only)
    CHECK(strstr(text, "driver: not a controller: 0d8c:0014 interface 3 (03/00/00), kept quietly") &&
          strstr(text, "U p0 0d8c:0014 quiet after start: not supported (-8)"),
          "%s: the pad log has the driver's quiet keep:\n%s", what, text + (strlen(text) > 4000 ? strlen(text) - 4000 : 0));
  else
    CHECK(strstr(text, "OS offer #") && strstr(text, "0d8c:0014 interface 03/00/00 -> not a controller the driver knows (0)"),
          "%s: the pad log has the card's offers:\n%s", what, text + (strlen(text) > 4000 ? strlen(text) - 4000 : 0));
  CHECK(fos_stats()->pipes_open == 0 && fos_stats()->xfers_open == 0, "%s: nothing open for it (%u, %u)", what,
        fos_stats()->pipes_open, fos_stats()->xfers_open);
  unplug();
  for (i = 0; i < FAMS; i++)
    one_pad(&fams[i], what);
}

static void test_r11u_soundcard(void)
{
  printf("R11u: a USB sound card first, then every family with the adapter kept in\n");
  r11u_card(0, "after the sound card");
  r11u_card(1, "after the sound card's HID interface");
  CHECK(fos_stats()->not_offered == 0, "R11u: every pad was offered (%u not)", fos_stats()->not_offered);
}

/* R10a fact 3 on the fake's model: a device nobody claims (a keyboard: not
 * a game controller, the attach is the OS's) and the OS halts its host
 * controller. The glue logs it with the registers, frees the port and
 * gives the OS its time in one go (UNVERIFIED that it helps: the fake's
 * model never starts again). */
static void test_halt(void)
{
  struct fos_spec kb = { CORPUS_KBD_VID, CORPUS_KBD_PID, 0x0110, corpus_kbd_cfg, sizeof corpus_kbd_cfg, NULL, 0, 0, 0 };
  const char *text;
  long sleeps;
  int i;

  printf("R10a's halt: a device nobody claims\n");
  fos_r10a_halt(1);
  wires(WIRES_PAD);
  dev = fos_plug(&kb);
  for (i = 0; i < 30 && !fos_stats()->halted; i++)
    frame();
  CHECK(fos_stats()->halted == 1, "halt: the fake OS halted its host (%u)", fos_stats()->halted);
  wires(WIRES_HALTED);
  sleeps = unit_os_sleeps;
  frames(120);
  text = log_text();
  CHECK(strstr(text, "pad glue: the OS's host controller STOPPED") && strstr(text, "USBCMD 00080000 PORTSC 0000088d"),
        "halt: the pad log says so, with the registers:\n%s", text + (strlen(text) > 3000 ? strlen(text) - 3000 : 0));
  CHECK(nspire_usbport_os() == PORT_OS_NONE && nspire_arb_owner() != ARB_OTHER, "halt: the port is free (os %d holder %d)",
        nspire_usbport_os(), nspire_arb_owner());
  CHECK(unit_os_sleeps - sleeps >= 100, "halt: the come-up gave the OS its time (%ld OS ticks)", unit_os_sleeps - sleeps);
  fos_unplug(dev);
  dev = -1;
  wires(WIRES_EMPTY);
  frames(30);
  CHECK(fos_stats()->violations == 0, "halt: no violation: %s", fos_stats()->violation);
  fos_r10a_halt(0);
}

/* R11w (2026-11-15): the hardware run's Switch Pro: attached, played,
 * unplugged with an OUT in flight, and the OS offered nothing more to any
 * driver. Both readings as the fake's rules: (A) its abort's callback late
 * (FOS_ABORT_LATE: a free or a close before it is a violation), (B) no
 * usbd_transfer from the main task while reports flow (out_main_flowing,
 * limit 0). Then every other family and the sound card work; ten rounds.
 * The pad log keeps the story in its milestones (never pushed out by port
 * lines) and the driver's counters. */
static void swpro_hazard(const char *round)
{
  struct fos_spec sp = fams[5].spec;
  double ms;
  int i;

  sp.flags = FOS_OUT_HOLD | FOS_ABORT_LATE;   /* its 80 02 stays in flight */
  wires(WIRES_PAD);
  streaming = NULL;
  dev = fos_plug(&sp);
  for (i = 0; i < 180 && !usbpad_count(); i++)
    frame();
  ms = usbpad_count() ? ms_since(unit_ticks) : -1;
  CHECK(usbpad_count() == 1, "%s: the Switch Pro attached (%.0f)", round, ms);
  hold(fams[5].south, fams[5].len, 40);   /* played: B held */
  CHECK(keys == USBPAD_SOUTH, "%s: the Switch Pro's B arrives: %05x", round, (unsigned)keys);
  fos_unplug(dev);   /* with the 80 02 in flight */
  dev = -1;
  streaming = NULL;
  wires(WIRES_EMPTY);
  frames(30);
  CHECK(usbpad_count() == 0 && keys == 0, "%s: the Switch Pro gone, let go", round);
  CHECK(fos_stats()->free_in_flight == 0 && fos_stats()->close_in_flight == 0,
        "%s: (A) nothing freed or closed before its callback (%u, %u)", round, fos_stats()->free_in_flight,
        fos_stats()->close_in_flight);
  CHECK(fos_stats()->violations == 0, "%s: no violation: %s", round, fos_stats()->violation);
}

static void test_r11w_unplug_out_in_flight(void)
{
  const char *text;
  int r;
  unsigned flowing0 = fos_stats()->out_main_flowing;

  printf("R11w: the Switch Pro unplugged with an OUT in flight, then every family and the sound card\n");
  swpro_hazard("R11w");
  for (r = 0; r < 5; r++)
    one_pad(&fams[r], "after the R11w Switch Pro");
  text = log_text();
  CHECK(strstr(text, "Milestones (") != NULL, "R11w: the pad log has its milestones");
  CHECK(strstr(text, "U p0 057e:2009 detach ep01 OUT in flight ") != NULL,
        "R11w: the detach's milestone says the OUT was in flight:\n%s", text + (strlen(text) > 6000 ? strlen(text) - 6000 : 0));
  CHECK(strstr(text, "abort ep01 OUT in flight, NO callback yet: ok") != NULL,
        "R11w: the abort's milestone: no callback inside it");
  CHECK(strstr(text, "057e:2009 detach_end IN kept OUT kept") != NULL, "R11w: the detach's end: kept for later");
  CHECK(strstr(text, "057e:2009 retire ep01 OUT: ok") != NULL && strstr(text, "057e:2009 retire ep81 IN: ok") != NULL,
        "R11w: retired once the callbacks came");
  CHECK(strstr(text, "driver counters: ") != NULL && strstr(text, "Driver now: ") != NULL,
        "R11w: the driver's counters in the milestones and now");
  for (r = 0; r < 10; r++)
  {
    char name[40];

    snprintf(name, sizeof(name), "R11w round %d", r + 1);
    swpro_hazard(name);
    one_pad(&fams[r % 5], name);
  }
  r11u_card(1, "after ten R11w rounds (the card's HID interface)");
  r11u_card(0, "after ten R11w rounds (the whole card)");
  CHECK(fos_stats()->out_main_flowing == flowing0, "R11w: (B) no usbd_transfer from the main task while reports flow "
        "(%u)", fos_stats()->out_main_flowing - flowing0);
  CHECK(fos_stats()->out_from_usb > 0, "R11w: OUTs from the USB task (%u)", fos_stats()->out_from_usb);
  CHECK(fos_stats()->not_offered == 0 && fos_stats()->breaches == 0, "R11w: every device offered (%u not, %u breaches)",
        fos_stats()->not_offered, fos_stats()->breaches);
}

/* R11w: a connected, enabled device's lines' state moving (R11w 192.2-199.1
 * s: PORTSC 0000000d / 0000080d / 0000040d more than thirty times in 7 s, a
 * "port:" line each; 350 older events pushed out): no port line for those,
 * and the milestones untouched. */
static void test_r11w_port_flood(void)
{
  static const uint32_t portsc[3] = { 0x0000000du, 0x0000080du, 0x0000040du };
  const char *text;
  unsigned n0, n1, ms0;
  int i;

  printf("R11w: the lines' state of an enabled port moving: no flood\n");
  wires(WIRES_PAD);
  frames(10);
  n0 = nspire_padlog_count();
  ms0 = nspire_padlog_milestones();
  for (i = 0; i < 60; i++)   /* 7 s: a change every ~115 ms */
  {
    unit_usb_raw[4] = portsc[i % 3];
    frames(7);
  }
  n1 = nspire_padlog_count();
  CHECK(n1 - n0 <= 2, "flood: %u port line(s) for 60 changes of the lines' state (<= 2)", n1 - n0);
  CHECK(nspire_padlog_milestones() == ms0, "flood: the milestones untouched (%u -> %u)", ms0, nspire_padlog_milestones());
  text = log_text();
  CHECK(strstr(text, "PORTSC 0000080d") == NULL || strstr(text, "PORTSC 0000040d") == NULL,
        "flood: not every state a line");
  wires(WIRES_EMPTY);
  frames(30);
}

int main(int argc, char **argv)
{
  const char *work = argc > 1 ? argv[1] : "build/padsim";

  mkdir(work, 0777);
  snprintf(unit_documents_dir, sizeof(unit_documents_dir), "%s/", work);
  unit_os_sleep_hook = os_tick;
  wires(WIRES_EMPTY);
  if (fos_init(FOS_INLINE) != 0)
  {
    printf("padsim: fos_init failed\n");
    return 1;
  }
  nspire_usbpad_start();
  frames(60);
  CHECK(usbpad_status()[0] && strcmp(usbpad_status(), "No controller") == 0, "the driver runs: %s", usbpad_status());
  test_each_family();
  test_fail_then_good();
  test_rounds();
  test_r11u_soundcard();   /* (before test_halt: the fake's R10a halt lasts until the adapter comes out) */
  test_r11w_unplug_out_in_flight();
  test_r11w_port_flood();
  CHECK(exit_timeouts == 0, "no exit timed out (%d, %s)", exit_timeouts, exit_timeout_step ? exit_timeout_step : "-");
  test_halt();
  printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
