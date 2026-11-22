/* PAD3: the pad log, gpsp_pad.txt.tns (nspire_padlog.h). */
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "nspire.h"
#include "nspire_os.h"
#include "nspire_paths.h"
#include "nspire_link.h"
#include "nspire_arbiter.h"
#include "nspire_padlog.h"
#include "nspire_bt_glue.h"
/* (r19-usb, nspire_exit.c; weak: the unit tests' builds have no exit) */
const char *nspire_exit_trail_last_run(void) __attribute__((weak));
#include "nspire_padstatus.h"
#include "nspire_audio.h"

#if __has_include("nspire_build_id.h")
#include "nspire_build_id.h"
#else
#define NSPIRE_BUILD_GIT    "unknown"
#define NSPIRE_BUILD_DATE   "unknown"
#define NSPIRE_BUILD_PADS   0
#define NSPIRE_BUILD_DRIVER "unknown"
#endif

/* 32 bytes, word aligned (an ARM926 faults on an unaligned word). */
struct entry
{
  u32 t;            /* nspire_ticks */
  u8 kind, a, b, c;   /* kind | PL_MARK: an interesting entry (R2) */
  uint32_t w[NSPIRE_USB_RAW];
};
#define PL_MARK 0x80
/* R10b: an entry of a try that ended with nothing working (kept: push drops
 * the oldest entry without it first). */
#define PL_KEEP 0x40
#define PL_KIND(e) ((e)->kind & ~(PL_MARK | PL_KEEP))
typedef char entry_is_32_bytes[sizeof(struct entry) == 32 ? 1 : -1];
typedef char kinds_fit_under_the_flags[PL_BT < PL_KEEP ? 1 : -1];   /* the last kind */

/* nspire_audio_os.c (built with USB audio only): its declines of a pad's
 * audio interface. A weak reference: NULL without it. */
unsigned nspire_audio_declined(uint32_t *ids, unsigned *ifno) __attribute__((weak));
static unsigned audio_declined_seen;
/* R11u: nspire_drive_os.c (absent without the drive driver: weak) */
unsigned nspire_drive_kept(uint32_t *ids, unsigned *ifno, int *err) __attribute__((weak));
static unsigned drive_kept_seen;
/* (2026-11-17) and the mass storage interfaces the OS offered its driver */
unsigned nspire_drive_offers(uint32_t *ids, unsigned *ifno, unsigned *why, unsigned *counts) __attribute__((weak));
static unsigned drive_offers_seen;

static struct entry ring[PADLOG_RING];
static unsigned head, count;     /* the next slot, entries kept */
static unsigned lost;            /* dropped from the ring's far end */
static u32 t0;
static int t0_set;
static unsigned news;            /* entries since the last write */
static int interesting;          /* something besides the controller's idle off-and-on */
static int try_due, failing, written, exited;
static int with_diag;           /* R11w: the exit's writes: the driver's diag block too */
static unsigned writes;
static unsigned attempts;        /* R8: writes tried, opened or not (the timers' and the limit's count) */
static u32 written_at, sampled_at;
static int sampled;
/* The last sample: its meaningful bits, and whether it said "nothing". */
static u32 last_sig;
static int last_nothing = 1, last_kind = -1;
static u32 wires_counter, wires_gone;
static int wires_on;
static int link_was = -1;
static enum padlog_try last_try;
static const char *asked;       /* nspire_padlog_request's reason, until written */

static u32 ms_since(u32 at) { return (u32)((u64)(nspire_ticks() - at) * 1000 / NSPIRE_TICK_HZ); }

static void start(void)
{
  if (!t0_set)
  {
    t0_set = 1;
    t0 = nspire_ticks();
  }
}

/* R2: one entry is interesting (besides the controller's idle off-and-on). */
static void mark(struct entry *e)
{
  e->kind |= PL_MARK;
  interesting = 1;
}

static unsigned kept;             /* R10b: entries with PL_KEEP in the ring */

#define AT(i) (&ring[(head + PADLOG_RING - count + (i)) % PADLOG_RING])

/* R10b: the ring is full: the oldest entry without PL_KEEP goes (the ones
 * older than it move up one), or the oldest of all if every one has it.
 * Afterwards ring[head] is free. Only while full: a shift of at most the
 * ring (8 KB). */
static void drop_one(void)
{
  unsigned i, victim = 0;

  for (i = 0; i < count && (AT(i)->kind & PL_KEEP); i++)
    ;
  if (i < count)
    victim = i;
  if (AT(victim)->kind & PL_KEEP)
    kept--;
  for (i = victim; i > 0; i--)
    *AT(i) = *AT(i - 1);
  lost++;
}

static struct entry *push(enum padlog_kind kind)
{
  struct entry *e;

  start();
  if (count == PADLOG_RING)
  {
    drop_one();
    count--;
  }
  e = &ring[head];
  head = (head + 1) % PADLOG_RING;
  count++;
  memset(e, 0, sizeof(*e));
  e->t = nspire_ticks();
  e->kind = (u8)kind;
  news++;
  /* (A port entry counts once something is plugged in: nspire_padlog_poll.) */
  if (kind != PL_IDLE && kind != PL_LINK && kind != PL_PORT && kind != PL_VERDICT)
    mark(e);
  return e;
}

static struct entry *newest(void)
{
  return count ? &ring[(head + PADLOG_RING - 1) % PADLOG_RING] : NULL;
}

void nspire_padlog_add(enum padlog_kind kind, int a, int b, int c, uint32_t w0, uint32_t w1, uint32_t w2)
{
  struct entry *e = push(kind);

  e->a = (u8)a;
  e->b = (u8)b;
  e->c = (u8)c;
  e->w[0] = w0;
  e->w[1] = w1;
  e->w[2] = w2;
  if (kind == PL_VERDICT && a != PV_NOTHING && a != PV_UNREADABLE)
    mark(e);
}

void nspire_padlog_add6(enum padlog_kind kind, int a, int b, int c, const uint32_t w[6])
{
  struct entry *e = push(kind);

  e->a = (u8)a;
  e->b = (u8)b;
  e->c = (u8)c;
  memcpy(e->w, w, sizeof(e->w));
}

unsigned nspire_padlog_kept(void)
{
  return kept;
}

/* ---- R11w: the milestones (nspire_padlog.h) ---- */

struct mstone
{
  struct entry e;
  u32 repeat, last_t;
};
static struct mstone ms_first[PADLOG_MS_FIRST], ms_ring[PADLOG_MS_RING];
static unsigned ms_first_n, ms_head, ms_count, ms_lost;
static struct mstone *ms_last;
static const char *exit_step_text;

static int ms_same(const struct entry *a, const struct entry *b)
{
  /* (a trace record's sequence and poll, w3 and w4, don't count) */
  return a->kind == b->kind && a->a == b->a && a->b == b->b && a->c == b->c && a->w[0] == b->w[0] &&
         a->w[1] == b->w[1] && a->w[2] == b->w[2] && (a->kind == PL_TRACE || a->w[3] == b->w[3]);
}

static void ms_push(const struct entry *e)
{
  struct mstone *m;

  start();
  if (ms_last && ms_same(&ms_last->e, e))
  {
    ms_last->repeat++;
    ms_last->last_t = e->t;
    return;
  }
  if (ms_first_n < PADLOG_MS_FIRST)
    m = &ms_first[ms_first_n++];
  else
  {
    if (ms_count == PADLOG_MS_RING)
      ms_lost++;
    else
      ms_count++;
    m = &ms_ring[ms_head];
    ms_head = (ms_head + 1) % PADLOG_MS_RING;
  }
  m->e = *e;
  m->repeat = 0;
  m->last_t = e->t;
  ms_last = m;
  /* (written then, as any entry that says something; not for the exit's
   * own marks and counters alone: a quit with nothing plugged in writes no
   * pad log, as before) */
  if (e->kind != PL_EXIT && e->kind != PL_MS_COUNTERS)
  {
    interesting = 1;
    news++;
  }
}

void nspire_padlog_milestone6(enum padlog_kind kind, int a, int b, int c, const uint32_t w[6])
{
  struct entry e;

  memset(&e, 0, sizeof(e));
  e.t = nspire_ticks();
  e.kind = (u8)kind;
  e.a = (u8)a;
  e.b = (u8)b;
  e.c = (u8)c;
  memcpy(e.w, w, sizeof(e.w));
  ms_push(&e);
}

void nspire_padlog_milestone(enum padlog_kind kind, int a, int b, int c, uint32_t w0, uint32_t w1, uint32_t w2)
{
  uint32_t w[6] = { w0, w1, w2, 0, 0, 0 };

  nspire_padlog_milestone6(kind, a, b, c, w);
}

unsigned nspire_padlog_milestones(void)
{
  return ms_first_n + ms_count;
}

void nspire_padlog_exit_mark(int code, uint32_t value)
{
  nspire_padlog_milestone(PL_EXIT, code, 0, 0, value, 0, 0);
}

/* R10b: a try ended with nothing working: its entries (back to the try
 * before's end, the last PADLOG_KEEP_TRY of them; its own end entry, just
 * added, included) are kept, the oldest kept ones let go past
 * PADLOG_KEEP_MAX (the last run's link log lost R10a's failed tries). */
static void keep_try(void)
{
  unsigned i, n = 0;

  for (i = count; i > 0 && n < PADLOG_KEEP_TRY; i--)
  {
    struct entry *e = AT(i - 1);

    if (i < count && PL_KIND(e) == PL_TRY_END)
      break;
    if (!(e->kind & PL_KEEP))
    {
      e->kind |= PL_KEEP;
      kept++;
    }
    n++;
  }
  for (i = 0; i < count && kept > PADLOG_KEEP_MAX; i++)
    if (AT(i)->kind & PL_KEEP)
    {
      AT(i)->kind &= ~PL_KEEP;
      kept--;
    }
}

/* The bits that mean something (the rest moves on its own: frame numbers,
 * interrupt status the OS clears). R11w: while the port is enabled, not
 * the lines' state (PORTSC 11:10): a connected device's bus traffic moves
 * them (R11w 192.2-199.1 s: 0000000d / 0000080d / 0000040d more than thirty
 * times in 7 s, a line each, 350 older events pushed out). */
static u32 signature(int kind, int plugged, const uint32_t *w0)
{
  u32 s = (u32)kind << 28 | (u32)(plugged + 1) << 26;
  uint32_t w[NSPIRE_USB_RAW];

  memcpy(w, w0, sizeof(w));
  if ((kind == 2 || kind == 1) && (w[4] & 4))
    w[4] &= ~0xC00u;
  if (kind == 2)
    /* OTGCSR 16-21 (ID, role, VBUS, sessions) and 4-5 (bus request, drop); USBCMD run; PORTSC 0, 2, 10-11 */
    return s | (w[0] >> 16 & 0x3F) | (w[0] & 0x30) << 2 | (w[2] & 1) << 8 | (w[4] & 0xC05) << 9 |
           (w[4] >> 7 & 3) << 21;   /* PAD4: PORTSC 7 suspend, 8 reset */
  if (kind == 1)
    /* OTGSC 5, 8-12; USBMODE 0-1; USBCMD run; PORTSC 0, 2, 10-12 (PP), 7-8 */
    return s | (w[0] >> 8 & 0x1F) | (w[0] >> 5 & 1) << 5 | (w[1] & 3) << 6 | (w[2] & 1) << 8 |
           (w[4] & 0x1C05) << 9 | (w[4] >> 7 & 3) << 23;
  return s;
}

/* R11w: what a "port:" line's words say (nspire_padstatus_decode), as a number. */
static u32 port_key(int kind, int plugged, const uint32_t *w)
{
  struct pad_status s;

  memset(&s, 0, sizeof(s));
  s.kind = kind;
  s.plugged = plugged;
  memcpy(s.raw, w, sizeof(s.raw));
  nspire_padstatus_decode(&s);
  return (u32)s.role | (u32)(s.power != 0) << 4 | (u32)(s.connected != 0) << 5 | (u32)(s.lines != 0) << 6 |
         (u32)(s.enabled != 0) << 7 | (u32)(s.resetting != 0) << 8 | (u32)(s.suspended != 0) << 9 |
         (u32)(s.running != 0) << 10;
}

void nspire_padlog_poll(void)
{
  uint32_t w[NSPIRE_USB_RAW];
  u32 sig;
  int kind, plugged, nothing, link, on;

  start();
  if (nspire_audio_declined)
  {
    uint32_t ids = 0;
    unsigned ifno = 0, n = nspire_audio_declined(&ids, &ifno);

    if (n != audio_declined_seen)
    {
      audio_declined_seen = n;
      nspire_padlog_add(PL_AUDIO, 0, 0, 0, ids, ifno, n);
    }
  }
  if (nspire_drive_offers)
  {
    uint32_t ids = 0;
    unsigned ifno = 0, why = 0, counts = 0, n;

    n = nspire_drive_offers(&ids, &ifno, &why, &counts);
    if (n != drive_offers_seen)
    {
      drive_offers_seen = n;
      /* (b = 1: an offer's line, not a kept attach's; a = why, c = interface) */
      nspire_padlog_add(PL_DRIVE_KEPT, (int)why, 1, (int)ifno, ids, counts, n);
      nspire_padlog_milestone(PL_DRIVE_KEPT, (int)why, 1, (int)ifno, ids, counts, n);
    }
  }
  if (nspire_drive_kept)
  {
    uint32_t ids = 0;
    unsigned ifno = 0, n;
    int err = 0;

    n = nspire_drive_kept(&ids, &ifno, &err);
    if (n != drive_kept_seen)
    {
      drive_kept_seen = n;
      nspire_padlog_add(PL_DRIVE_KEPT, err, 0, 0, ids, ifno, n);
    }
  }
  if (sampled && ms_since(sampled_at) < PADLOG_SAMPLE_MS)
    return;
  sampled = 1;
  sampled_at = nspire_ticks();
  link = nspire_link_state();
  if (link != link_was)
  {
    if (link_was >= 0)
      nspire_padlog_add(PL_LINK, link, link_was, 0, 0, 0, 0);
    link_was = link;
  }
  plugged = nspire_usb_plugged();
  kind = nspire_usb_raw(w);
  /* Nothing plugged in reads as the controller's own empty state (0), or
   * not at all (a CX II's OS switches it off and on, ~120 ms every 1.5 s:
   * the R9F logs; a CX's gate): only the bits' "nothing" is noted. */
  nothing = plugged == 0 || (plugged < 0 && (!kind || nspire_usb_gated()));
  sig = signature(kind, plugged, w);
  if (sig == last_sig && kind == last_kind)
    return;
  /* A device onto the wires at the adapter's end: J or K on the lines, or connected. */
  on = !nothing && ((kind == 2 && !(w[0] & (1u << 21))) || (kind == 1 && !(w[0] & (1u << 8)))) &&
       ((w[4] & 1) || (w[4] >> 10 & 3));
  if (on && !wires_on)
    wires_counter++;
  if (!on && wires_on)
    wires_gone++;
  wires_on = on;
  if (nothing && last_nothing && last_kind >= 0)
  {
    /* The idle off-and-on: one line with a count. (r19-usb) And what each
     * reading said: the controller readable with its ID pin at the device's
     * end (plugged 0: no OTG adapter in the port, whatever is on the other
     * end of the cable), or the controller off (unreadable). r19's CX II
     * with a controller on a cable (958921d):
     * nothing but these for 899 s, the page between "empty" and "off". */
    struct entry *e = newest();
    int readable = plugged == 0 && kind != 0;
    if (e && PL_KIND(e) == PL_IDLE)
    {
      e->w[0]++;
      e->w[readable ? 1 : 2]++;
      e->t = nspire_ticks();
    }
    else
      nspire_padlog_add(PL_IDLE, 0, 0, 0, 1, readable ? 1 : 0, readable ? 0 : 1);
  }
  else
  {
    /* R11w: the same words as the line before (only bits the words don't
     * show moved): counted on it, not repeated */
    struct entry *e = newest();
    if (e && PL_KIND(e) == PL_PORT && e->a == (u8)kind && e->b == (u8)(plugged + 1) &&
        port_key(e->a, (int)e->b - 1, e->w) == port_key(kind, plugged, w))
    {
      if (e->c < 255)
        e->c++;
    }
    else
    {
      e = push(PL_PORT);
      e->a = (u8)kind;
      e->b = (u8)(plugged + 1);
      memcpy(e->w, w, sizeof(w));
      if (!nothing)
        mark(e);
    }
  }
  last_sig = sig;
  last_kind = kind;
  last_nothing = nothing;
}

uint32_t nspire_padlog_wires_counter(void)
{
  return wires_counter;
}

uint32_t nspire_padlog_wires_gone(void)
{
  return wires_gone;
}

void nspire_padlog_try_end(enum padlog_try why)
{
  nspire_padlog_add(PL_TRY_END, why, 0, 0, 0, 0, 0);
  if (why != PL_TRY_PAD && why != PL_TRY_NOT_PAD)   /* (R11u: nothing failed there) */
    keep_try();
  last_try = why;
  try_due = 1;
}

void nspire_padlog_request(const char *why)
{
  asked = why;
}

void nspire_padlog_failing(int on)
{
  failing = on;
}

/* ---- The text ---- */

typedef void (*emit_fn)(void *ctx, const char *line);

static const char *owner_name(int o)
{
  return nspire_arb_owner_name((enum arb_owner)o);
}

static const char *link_name(int s)
{
  return s == LINK_OFF ? "off" : s == LINK_LOOKING ? "looking for the other GBA" : s == LINK_LINKED ? "linked" : "?";
}

static const char *try_name(int why)
{
  static const char *const n[] = { "?", "a controller attached", "the OS offered nothing in 12 s",
                                   "the arbiter gave up (stage 4)", "the device was unplugged with nothing working",
                                   "the driver gave up on it (power, unsupported, failed)",
                                   "the device is not a controller (left alone; nothing failed)",
                                   "the adapter's power came up after it was held down (what drew it down was taken "
                                   "off, or the port recovered): nothing on it had worked" };
  return (unsigned)why < sizeof(n) / sizeof(n[0]) ? n[why] : "?";
}

static const char *event_name(int type)
{
  static const char *const n[] = { "?", "attach (connected)", "detach (disconnected)", "replug hint",
                                   "may need more power", "not supported", "stopped answering" };
  return (unsigned)type < sizeof(n) / sizeof(n[0]) ? n[type] : "?";
}

/* R10b: without the pad glue (unit tests, no pads), the driver's numbers. */
void nspire_usbpad_trace_words(int step, int task, int port, const uint32_t w[6], char *out, unsigned size)
  __attribute__((weak, noipa));
void nspire_usbpad_trace_words(int step, int task, int port, const uint32_t w[6], char *out, unsigned size)
{
  snprintf(out, size, "bring-up #%lu: %c p%d %04lx:%04lx step %d ep %02lx err %ld arg %lu", (unsigned long)w[3],
           task ? task : '?', port, (unsigned long)(w[0] >> 16), (unsigned long)(w[0] & 0xFFFF), step,
           (unsigned long)(w[2] >> 16 & 0xFF), (long)(int32_t)w[1], (unsigned long)(w[2] & 0xFFFF));
}

void nspire_usbpad_fail_words(uint32_t facts, uint32_t err, char *out, unsigned size) __attribute__((weak, noipa));
void nspire_usbpad_fail_words(uint32_t facts, uint32_t err, char *out, unsigned size)
{
  snprintf(out, size, "step %lu, ep %02lx, error %ld", (unsigned long)(facts & 0xFF), (unsigned long)(facts >> 8 & 0xFF),
           (long)(int32_t)err);
}

static void port_words(const struct entry *e, char *out, unsigned size)
{
  struct pad_status s;
  char raw[200];

  memset(&s, 0, sizeof(s));
  s.kind = e->a;
  s.plugged = (int)e->b - 1;
  memcpy(s.raw, e->w, sizeof(s.raw));
  nspire_padstatus_decode(&s);
  nspire_padstatus_raw_text(&s, raw, sizeof(raw));
  if (s.role == PS_ROLE_NONE)
    snprintf(out, size, "port: nothing plugged in | %s", raw);
  else if (s.role == PS_ROLE_UNKNOWN)
    snprintf(out, size, "port: can't tell (plugged %d) | %s", s.plugged, raw);
  else if (s.role == PS_ROLE_DEVICE)
    snprintf(out, size, "port: device end (cable's plain end), %s, controller %s | %s",
             s.power ? "B session" : "no session", s.running ? "running" : "stopped", raw);
  else
    snprintf(out, size, "port: host end (adapter), power %s, %s, %s%s%s, host controller %s | %s", s.power ? "on" : "OFF",
             s.connected ? "connected" : s.lines ? "a device on the wires (not connected)" : "nothing on the wires",
             s.enabled ? "enabled" : "not enabled", s.resetting ? ", in reset" : "", s.suspended ? ", SUSPENDED" : "",
             s.running ? "running" : "STOPPED", raw);
}

static void entry_text(const struct entry *e, char *line, unsigned size)
{
  unsigned n = (unsigned)snprintf(line, size, "%7lu ", (unsigned long)((u64)(e->t - t0) * 1000 / NSPIRE_TICK_HZ));
  char *p = line + (n < size ? n : size - 1);
  unsigned left = n < size ? size - n : 1;

  switch (PL_KIND(e))
  {
  case PL_PORT:
    port_words(e, p, left);
    if (e->c)
    {
      size_t k = strlen(p);
      if (k < left)
        snprintf(p + k, left - k, " (and %u more like it: the lines' state moving)", (unsigned)e->c);
    }
    break;
  case PL_IDLE:
    /* (r19-usb) gpSP writes nothing to the port with no adapter's end seen:
     * the arbiter cycles only an unclaimed device at the host's end, the
     * awake and transceiver changes wait for the adapter (watch_awake,
     * watch_phy): the off and on is the OS's own power-down of an idle port */
    snprintf(p, left, "port: nothing plugged in; the controller went off and on %lu time(s) (the OS's, up to this time): "
             "%lu reading(s) of the ID pin at the device's end (NO OTG ADAPTER in the port), %lu with the controller off; "
             "gpSP wrote nothing to the port", (unsigned long)e->w[0], (unsigned long)e->w[1], (unsigned long)e->w[2]);
    break;
  case PL_OFFER:
  {
    int score = (int8_t)e->a;
    char port[24] = "";
    /* hub (2026-11-16): the port of the hub (or the calculator's own, 1) the
     * device is on, where the driver read it */
    if (e->w[1] >> 24)
      snprintf(port, sizeof(port), ", port %lu", (unsigned long)(e->w[1] >> 24));
    snprintf(p, left, "OS offer #%lu: %04lx:%04lx %s %02lx/%02lx/%02lx -> %s (%d)%s", (unsigned long)e->w[2],
             (unsigned long)(e->w[0] >> 16), (unsigned long)(e->w[0] & 0xFFFF), e->b ? "interface" : "device",
             (unsigned long)(e->w[1] >> 16 & 0xFF), (unsigned long)(e->w[1] >> 8 & 0xFF), (unsigned long)(e->w[1] & 0xFF),
             score > 0 ? "taken" : score == 0 ? "not a controller the driver knows" :
             score == -2 ? "refused: a TI calculator" : "refused", score, port);
    break;
  }
  case PL_EVENT:
  {
    char facts[96] = "";

    if (e->w[1] & PL_EVENT_FACTS)
    {
      char words[80];

      nspire_usbpad_fail_words(e->w[1], e->w[2], words, sizeof(words));
      snprintf(facts, sizeof(facts), "; failed at %s", words);
    }
    snprintf(p, left, "driver: %s, slot %d, %04lx:%04lx, kind %u%s", event_name(e->a), (int)e->c - 1,
             (unsigned long)(e->w[0] >> 16), (unsigned long)(e->w[0] & 0xFFFF), (unsigned)e->b, facts);
    break;
  }
  case PL_TRACE:
    nspire_usbpad_trace_words(e->a, e->b, (int)e->c - 1, e->w, p, left);
    break;
  case PL_HC:
    if (e->a == 2)
      snprintf(p, left, "pad glue: the OS's USB has STOPPED: %lu plugs in a row were enabled and never offered, the "
               "port's connect change never taken (USBCMD %08lx PORTSC %08lx); the adapter out and in again restarts "
               "it; quit restarts the calculator", (unsigned long)e->w[0], (unsigned long)e->w[1], (unsigned long)e->w[2]);
    else if (e->a == 3)
      snprintf(p, left, "pad glue: the OS offers devices again: its USB is back");
    /* (2026-11-17) the OS's sleep of what nobody uses: its code changed for
     * the run (b: the changes in, as bits, or -1), the transceiver found
     * asleep and woken (b: a device was waiting on it), the bus suspended
     * under what is plugged in */
    else if (e->a == 4)
      snprintf(p, left, (int)e->w[0] == 7 ? "awake: the OS's USB is kept awake for this run (3 words of its code "
               "changed, put back at exit): a hub with nothing on it stays on, a pulled device leaves the port working" :
               (int)e->w[0] < 0 ? "awake: not the OS build whose code is known: nothing changed (it suspends a hub "
               "with nothing on it, and what nobody uses)" : "awake: the OS's code changed only in part (bits %lx)",
               (unsigned long)e->w[0]);
    else if (e->a == 5)
      snprintf(p, left, "awake: the OS's code is put back as it was (the exit)");
    else if (e->a == 6)
      snprintf(p, left, "pad glue: the USB transceiver was asleep with %s (the OS's suspend left it so; USBCMD %08lx "
               "PORTSC %08lx): woken (%lu)%s", e->b ? "a device waiting on it" : "nothing on the port",
               (unsigned long)e->w[1], (unsigned long)e->w[2], (unsigned long)e->w[0],
               e->b ? "; the port's power restart starts the device over" : "");
    else if (e->a == 8)
      snprintf(p, left, "pad glue: the controller's reports come in interrupt windows (the first after %lu windows): no "
               "OS tick of its own, a task sleep every 250 ms", (unsigned long)e->w[0]);
    else if (e->a == 9)
      snprintf(p, left, "pad glue: %lu interrupt windows brought no report and %lu task sleeps did: the pad's tick every "
               "33 ms as before", (unsigned long)e->w[0], (unsigned long)e->w[1]);
    else if (e->a == 7)
      snprintf(p, left, "pad glue: the OS holds what is plugged in suspended (nobody used it: USBCMD %08lx PORTSC "
               "%08lx); the port's power restart starts it over with gpSP's drivers", (unsigned long)e->w[1],
               (unsigned long)e->w[2]);
    else if (e->a)
      snprintf(p, left, "pad glue: the OS's host controller STOPPED %lu ms after the OS offered this device and no pad "
               "came of it (USBCMD %08lx PORTSC %08lx): the port is free again, the come-up gives the OS its time",
               (unsigned long)e->w[0], (unsigned long)e->w[1], (unsigned long)e->w[2]);
    else
      snprintf(p, left, "pad glue: the OS's host controller runs again, %lu ms after it stopped (USBCMD %08lx PORTSC %08lx)",
               (unsigned long)e->w[0], (unsigned long)e->w[1], (unsigned long)e->w[2]);
    break;
  case PL_AUDIO:
    snprintf(p, left, "audio: left %04lx:%04lx interface %lu alone: a game controller, the pad driver's (gpSP's USB "
             "audio declined %lu interface(s) so far)", (unsigned long)(e->w[0] >> 16), (unsigned long)(e->w[0] & 0xFFFF),
             (unsigned long)e->w[1], (unsigned long)e->w[2]);
    break;
  case PL_DRIVER_ATTACH:
    snprintf(p, left, "attach #%lu: %04lx:%04lx step %u in %02x out %02x error %ld",
             (unsigned long)e->w[2], (unsigned long)(e->w[0] >> 16), (unsigned long)(e->w[0] & 0xffff),
             (unsigned)e->a, (unsigned)e->b, (unsigned)e->c, (long)(int32_t)e->w[1]);
    break;
  case PL_DRIVER_OUT:
  {
    char refusal[8], completion[8];
    if (e->b == 255)
      strcpy(refusal, "none");
    else
      snprintf(refusal, sizeof(refusal), "%u", (unsigned)e->b);
    if (e->c == 255)
      strcpy(completion, "none");
    else
      snprintf(completion, sizeof(completion), "%u", (unsigned)e->c);
    snprintf(p, left, "output: %04lx:%04lx endpoint %02x errors %lu refusal %s completion %s completed %lu",
             (unsigned long)(e->w[0] >> 16), (unsigned long)(e->w[0] & 0xffff), (unsigned)e->a,
             (unsigned long)e->w[1], refusal, completion, (unsigned long)e->w[2]);
    break;
  }
  case PL_OWNER:
    snprintf(p, left, "owner: %s (was %s)", owner_name(e->a), owner_name(e->b));
    break;
  case PL_STAGE:
  {
    static const char *const st[] = { "wait for the OS's offer", "power cycle", "wait again", "direct read", "gave up" };
    snprintf(p, left, "arbiter: stage %u (%s)", (unsigned)e->a, e->a < 5 ? st[e->a] : "?");
    break;
  }
  case PL_CYCLE:
    snprintf(p, left, "%s power restart %u -> %ld (%s)", e->b == 1 ? "user:" : e->b == 2 ? "link:" :
             e->b == 3 ? "hub recovery:" : "arbiter:", (unsigned)e->a, (long)(int32_t)e->w[0],
             (int32_t)e->w[0] == 1 ? "power back" : (int32_t)e->w[0] == 0 ? "power not back" :
             (int32_t)e->w[0] == -1 ? "nothing to restart" :
             (int32_t)e->w[0] == -5 ? "not made: the OS is on the device, or its host controller is stopped with it connected" :
             "can't here");
    break;
  case PL_LINK:
    snprintf(p, left, "link: %s (was %s)", link_name(e->a), link_name(e->b));
    break;
  case PL_VERDICT:
    snprintf(p, left, "status: %s: %s", nspire_padstatus_name((enum pad_verdict)e->a),
             nspire_padstatus_cause((enum pad_verdict)e->a));
    break;
  case PL_TRY_END:
    snprintf(p, left, "try ended: %s", try_name(e->a));
    break;
  case PL_USER:
    if (e->a == PL_USER_PAGE || e->a == PL_USER_PAGE_LEFT)
      snprintf(p, left, "user: %s the Controller status page (%s)", e->a == PL_USER_PAGE ? "opened" : "left",
               nspire_padstatus_name((enum pad_verdict)e->w[0]));
    else if (e->a == PL_USER_OS_TIME)
      snprintf(p, left, "user: gave the OS %lu ms (until its host controller runs, 30 s at most): %lu OS sleeps, the host "
               "controller %s", (unsigned long)e->w[2], (unsigned long)e->w[0], e->w[1] ? "ran" : "never ran");
    else
      snprintf(p, left, "user: %u", (unsigned)e->a);
    break;
  case PL_DRIVE:
    snprintf(line + n, size - n, "drive state=%d detail=%d generation/commands=%lu epoch/MB=%lu",
             e->a, e->b, (unsigned long)e->w[0], (unsigned long)e->w[1]);
    break;
  case PL_TOAST:
    snprintf(p, left, "toast: \"%s\"", PADSTATUS_TOAST_TEXT);
    break;
  case PL_ATTACH_OS:
    snprintf(p, left, "pad glue: %lu forced OS ticks, %lu OS sleeps in all", (unsigned long)e->w[0],
             (unsigned long)e->w[1]);
    break;
  case PL_COMEUP_BEGIN:
    snprintf(p, left, "come-up: a device came onto the adapter's wires%s (the plug: the times below are from here)",
             e->a ? " (there at gpSP's start)" : "");
    break;
  case PL_STEP:
  {
    static const char *const st[PADSTATUS_STEPS] = { "the OS's host controller runs", "the device is connected",
                                                     "the OS resets the port", "the port is enabled",
                                                     "the OS offered it", "the driver attached it",
                                                     "the first report (buttons) came" };
    snprintf(p, left, "come-up: +%lu ms: %s (OS sleeps +%lu, PORTSC %08lx)", (unsigned long)e->w[0],
             e->a < PADSTATUS_STEPS ? st[e->a] : "?", (unsigned long)e->w[1], (unsigned long)e->w[2]);
    break;
  }
  case PL_BURST:
  {
    static const char *const why[] = { "?", "its time was up", "the controller works", "the come-up ended",
                                       "a key was pressed", "the OS was done with the port (enabled, nothing offered)",
                                       "the OS can't be let run",
                                       "the host controller didn't start in 15 s (the most it waits for it)" };
    snprintf(p, left, "come-up: the OS ran alone %lu ms (%lu OS sleeps, the %s %s%s): %s; the host controller %s",
             (unsigned long)e->w[0], (unsigned long)e->w[1], e->b ? "game" : "menus", e->b ? "stood still" : "waited",
             e->c ? ", waiting for the host controller to start" : "",
             e->a < sizeof(why) / sizeof(why[0]) ? why[e->a] : "?", e->w[2] ? "runs" : "is STOPPED");
    break;
  }
  case PL_COMEUP:
  {
    static const char *const how[] = { "?", "the controller works", "the driver gave its answer (see above)",
                                       "it is the other calculator (the link's)", "the device left",
                                       "the link has the port", "another driver has the port",
                                       "nothing answered in 60 s",
                                       "the OS enabled the port and offered nothing (the arbiter's restart is next)",
                                       "the port never latched the device as connected, even after the restart",
                                       "the device is not a controller (left alone, no more OS time for it)",
                                       "it is a USB hub (what is plugged into it comes by the OS's offers)" };
    snprintf(p, left, "come-up: ended after %lu ms (OS sleeps +%lu%s): %s", (unsigned long)e->w[0], (unsigned long)e->w[1],
             e->c ? "" : ", no burst", e->a < sizeof(how) / sizeof(how[0]) ? how[e->a] : "?");
    break;
  }
  case PL_TIMER:
  {
    static const char *const st[PADSTATUS_STEPS] = { "the host controller runs", "the device is connected",
                                                     "the OS resets the port", "the port is enabled", "offered",
                                                     "attached", "first report" };
    char rate[48];

    if (e->w[2])
      snprintf(rate, sizeof(rate), "%lu.%lu counts a ms", (unsigned long)(e->w[2] / 10), (unsigned long)(e->w[2] % 10));
    else
      snprintf(rate, sizeof(rate), "%s", e->w[0] & 0x80 ? "not counted" : "not running");
    snprintf(p, left, "timers (%s): 900c0020 control %02lx, load %08lx, %s; select 900c0080 %08lx; 900c0000 control "
             "%02lx, gpSP's count %lu.%lu a ms", e->a < PADSTATUS_STEPS ? st[e->a] : "?", (unsigned long)e->w[0],
             (unsigned long)e->w[1], rate, (unsigned long)e->w[3], (unsigned long)e->w[4], (unsigned long)(e->w[5] / 10),
             (unsigned long)(e->w[5] % 10));
    break;
  }
  case PL_HUB:
  {
    static const char *const why[] = { "?", "the OS was done", "its time was up", "a key was pressed",
                                       "the OS can't be let run", "the hub left" };
    if (e->a == PL_HUB_IN)
      snprintf(p, left, "hub: a USB hub is on the port (%04lx:%04lx): no power restart while it is in; what is plugged "
               "into it comes and goes by the OS's offers, not by the port's lines", (unsigned long)(e->w[0] >> 16),
               (unsigned long)(e->w[0] & 0xFFFF));
    else if (e->a == PL_HUB_OUT)
      snprintf(p, left, "hub: the USB hub left the port");
    else if (e->a == PL_HUB_PORTS)
    {
      /* c connected, e enabled, s suspended, o over-current, r in reset,
       * p powered, d the OS has a device for it, ! a change it has not
       * taken; "-": nothing */
      static const char letters[] = "cesorpd!";
      char ports[8 * 11 + 1];
      unsigned n = (e->c & 15u) < 8 ? (e->c & 15u) : 8, i, k, o = 0;

      for (i = 0; i < n; i++)
      {
        unsigned b = (i < 4 ? e->w[0] >> (8 * i) : e->w[1] >> (8 * (i - 4))) & 0xFF;

        o += (unsigned)snprintf(ports + o, sizeof(ports) - o, "%s%u ", i ? ", " : "", i + 1);
        if (!b)
          ports[o++] = '-';
        for (k = 0; k < 8; k++)
          if (b & (1u << k))
            ports[o++] = letters[k];
        ports[o] = 0;
      }
      snprintf(p, left, "hub: its ports as the OS sees them: %s (c connected, e enabled, s suspended, o over-current, r "
               "reset, p powered, d the OS has the device, ! a change in the hub's last answer); the hub driver %s%s, %u "
               "active; %lu "
               "look(s) asked of the OS so far%s%s", ports, e->b & 1 ? "runs" : "does not run",
               e->b & 2 ? ", SUSPENDED" : "", (unsigned)(e->c >> 4), (unsigned long)e->w[2],
               e->b & 4 ? "; A DEVICE ON IT DOES NOT START (connected, never made a device)" : "",
               e->b & 8 ? "; THE HUB REPORTS TOO MUCH CURRENT" : "");
    }
    else if (e->a == PL_HUB_NEWS && e->b == 2)
      snprintf(p, left, "hub: a device waits on a port of the hub (connected, the OS has made no device of it yet; "
               "%lu so far): the OS's tick on every poll until it has", (unsigned long)e->w[0]);
    else if (e->a == PL_HUB_NEWS && e->b)
      snprintf(p, left, "hub: the OS's \"a hub has news\" flag stays set through the OS's ticks (its explore task "
               "does not run): no more ticks for it until it clears");
    else if (e->a == PL_HUB_NEWS)
      snprintf(p, left, "hub: a port of the hub changed (the OS's hub driver has news, %lu so far): the OS's tick on "
               "every poll until it has found what came or went", (unsigned long)e->w[0]);
    else if (e->a == PL_HUB_DEAD)
      snprintf(p, left, "hub: the port under the hub is no longer the OS's working port (%s; USBCMD %08lx PORTSC "
               "%08lx): the hub as the OS had it is gone, the port's own recovery takes over",
               e->w[1] & 4 ? "the OS holds it suspended, its controller stopped" : "connected and not enabled",
               (unsigned long)e->w[0], (unsigned long)e->w[1]);
    else if (e->a == PL_HUB_BURST)
      snprintf(p, left, "hub: the OS ran alone %lu ms (%lu OS sleeps, the %s %s) for a device on the hub (%lu offer(s)): "
               "%s", (unsigned long)e->w[0], (unsigned long)e->w[1], e->b ? "game" : "menus",
               e->b ? "stood still" : "waited", (unsigned long)e->w[2], e->c < sizeof(why) / sizeof(why[0]) ? why[e->c] : "?");
    else if (e->a == PL_HUB_LEFT)
      snprintf(p, left, "hub: a device left the hub (the driver's detach #%lu): what was said of it is forgotten",
               (unsigned long)e->w[0]);
    else if (e->a == PL_HUB_BLIP && e->b == PL_HUBP_FELL)
      snprintf(p, left, "hub: the port's power FELL under the hub (OTGCSR %08lx OTGISR %08lx): a hub on the "
               "calculator's power browns out when a device on it draws too much (a plug-in)",
               (unsigned long)e->w[0], (unsigned long)e->w[1]);
    else if (e->a == PL_HUB_BLIP && e->b == PL_HUBP_BACK)
      snprintf(p, left, "hub: the port's power is back (seen off for %lu ms between two looks: not its length); the OS "
               "was told nothing of it (no connect change): watching whether the hub's devices still answer",
               (unsigned long)e->w[2]);
    else if (e->a == PL_HUB_BLIP && e->b == PL_HUBP_ALIVE)
      snprintf(p, left, "hub: after the power fell its devices still answer (data came in): left as it is");
    else if (e->a == PL_HUB_BLIP)
      snprintf(p, left, "hub: after the power fell the OS took it itself (%lu offer(s), %lu detach(es) since): "
               "left to it", (unsigned long)e->w[0], (unsigned long)e->w[1]);
    else if (e->a == PL_HUB_RECOVER)
    {
      const char *why = e->b == PL_HUBR_WHY_POWER ? "its power fell and nothing on it answers since" :
                        "the OS's explore of the hub has not run";
      if (e->c == PL_HUBR_RESTART)
        snprintf(p, left, "hub: DEAD (%s): recovery #%lu, the port's power restarted -> %ld (%s): the OS sees the hub "
                 "leave and come back, and offers what is on it again", why, (unsigned long)e->w[1],
                 (long)(int32_t)e->w[0], (int32_t)e->w[0] == 1 ? "power back" : "NOT made or power not back");
      else if (e->c == PL_HUBR_BACK)
        snprintf(p, left, "hub: recovery: the hub is offered again %lu ms after the restart%s", (unsigned long)e->w[0],
                 e->w[1] ? " (late: after \"still dead\" was said)" : "");
      else if (e->c == PL_HUBR_ALIVE)
        snprintf(p, left, "hub: looked dead (%s), not cut: %s", why,
                 e->w[0] == 1 ? "its devices brought data in or the OS worked on one since" :
                 e->w[0] == 2 ? "the hub answers a request" : "the hub can't be asked and nothing on it says it is dead");
      else if (e->c == PL_HUBR_CANCELLED)
        snprintf(p, left, "hub: the recovery %s ends: %s", e->w[1] ? "under way" : "due",
                 e->w[0] == 1 ? "the hub left the port" : e->w[0] == 2 ? "another hub came" :
                 "the port has no hub any more");
      else if (e->c == PL_HUBR_OFFERED)
        snprintf(p, left, "hub: recovery: %lu device(s) on the hub offered again within %lu ms of the restart",
                 (unsigned long)e->w[0], (unsigned long)e->w[1]);
      else if (e->c == PL_HUBR_STILL_DEAD)
        snprintf(p, left, "hub: recovery FAILED: the hub was not offered again within %lu ms of the restart (the OS's "
                 "USB may have stopped): take the hub out and put it back (still watched for)", (unsigned long)e->w[0]);
      else if ((int32_t)e->w[1] < 0)
        snprintf(p, left, "hub: DEAD (%s), the restart refused now (%ld: the link or its cable): tried again in 5 s",
                 why, (long)(int32_t)e->w[1]);
      else
        snprintf(p, left, "hub: DEAD (%s), no recovery made (%lu so far; restart's answer %ld): take the hub out and "
                 "put it back", why, (unsigned long)e->w[0], (long)(int32_t)e->w[1]);
    }
    else if (e->a == PL_HUB_POOL)
      snprintf(p, left, "driver: TI's stale \"being aborted\" mark taken off %lu free transfer block(s) of the OS "
               "(%lu so far): left by a sound adapter's abort earlier in this OS's uptime; a transfer in such a block "
               "can't be aborted (the OS's task sleeps for good)", (unsigned long)e->w[0], (unsigned long)e->w[1]);
    else
      snprintf(p, left, "hub: no power restart (it would cut every device on the hub)");
    break;
  }
  case PL_SOUND:
  {
    static const char *const st[] = { "off", "waiting", "setting up", "ready", "streaming", "failed", "gone", "stopped" };   /* ua_os.h's UA_* */
    snprintf(p, left, "sound: %s (was %s), %04lx:%04lx, %lu completions so far%s%s; abort marks taken off %lu, "
             "not as expected %lu; stops that aborted nothing %lu",
             e->a < sizeof(st) / sizeof(st[0]) ? st[e->a] : "?", e->b < sizeof(st) / sizeof(st[0]) ? st[e->b] : "?",
             (unsigned long)(e->w[0] >> 16), (unsigned long)(e->w[0] & 0xFFFF), (unsigned long)e->w[1],
             e->w[2] & 2 ? ", attached" : "", e->w[2] & 1 ? ", its transfers or callbacks still the OS's" : "",
             (unsigned long)(e->w[2] >> 8 & 0xFF), (unsigned long)(e->w[2] >> 16 & 0xFF),
             (unsigned long)(e->w[2] >> 24 & 0xFF));
    break;
  }
  case PL_BT:
    if (e->a == PL_BT_STATE)
      snprintf(p, left, "bluetooth: %s%s, state %lu",
               e->b == 1 ? "on" : e->b == 2 ? "off" : e->b == 3 ? "failed" : "the start failed",
               e->b == 3 && (e->c & 0xFFFF) == 0xFFF7 ? " (the dongle was pulled out)" : "", (unsigned long)e->w[0]);
    else if (e->a == PL_BT_PAD)
      snprintf(p, left, "bluetooth: controller %s, slot %d, kind %lu, address ..:%02lx:%02lx:%02lx",
               e->b ? "connected" : "disconnected", e->c, (unsigned long)e->w[0], (unsigned long)(e->w[1] >> 16 & 0xFF),
               (unsigned long)(e->w[1] >> 8 & 0xFF), (unsigned long)(e->w[1] & 0xFF));
    else if (e->a == PL_BT_AUDIO)
      snprintf(p, left, "bluetooth: headphones %s, address ..:%02lx:%02lx:%02lx", e->b ? "connected" : "disconnected",
               (unsigned long)(e->w[1] >> 16 & 0xFF), (unsigned long)(e->w[1] >> 8 & 0xFF), (unsigned long)(e->w[1] & 0xFF));
    else if (e->a == PL_BT_STOP)
      snprintf(p, left, "bluetooth: stopped (%s) in %lu ms", e->b == 0 ? "clean" : e->b == 1 ? "USB memory left to the OS"
               : "A CALLBACK STILL INSIDE GPSP", (unsigned long)e->w[0]);
    else
      snprintf(p, left, "bluetooth: %d", e->a);
    break;
  case PL_DRIVE_KEPT:
    if (e->b == 1)
    {
      /* usbms_platform.h USBMS_WHY_* */
      static const char *const why[] = {
        "took it", "left it (the offer's own word unreadable)", "left it (the offer's arguments unreadable)",
        "left it (no interface in the offer)", "left it (no device behind the interface)",
        "left it (its descriptors unreadable)", "left it (a TI device)", "left it (not mass storage)",
        "left it (the driver is shutting down)", "left it (a drive is in already: one at a time)",
        "left it (no transfer memory free)" };

      snprintf(p, left, "drive: the OS offered the drive driver %04lx:%04lx interface %u (mass storage, %lu so far): it "
               "%s; its matches %lu, attaches %lu (%lu failed)", (unsigned long)(e->w[0] >> 16),
               (unsigned long)(e->w[0] & 0xFFFF), (unsigned)e->c, (unsigned long)e->w[2],
               why[(unsigned)e->a < sizeof(why) / sizeof(why[0]) ? (unsigned)e->a : 1], (unsigned long)(e->w[1] & 0xFF),
               (unsigned long)(e->w[1] >> 8 & 0xFF), (unsigned long)(e->w[1] >> 16 & 0xFF));
      break;
    }
    snprintf(p, left, "drive: %04lx:%04lx interface %lu: the USB drive driver could not use it (its attach said %d); kept "
             "quietly instead of refused (TI's OS offers nothing more after a refused attach), %lu so far",
             (unsigned long)(e->w[0] >> 16), (unsigned long)(e->w[0] & 0xFFFF), (unsigned long)e->w[1], (int)(int8_t)e->a,
             (unsigned long)e->w[2]);
    break;
  case PL_NOT_PAD:
    snprintf(p, left, "driver: not a controller: %04lx:%04lx interface %u (%02lx/%02lx/%02lx), kept quietly (no pipe, no "
             "OS time; the OS goes on offering), %lu so far", (unsigned long)(e->w[0] >> 16),
             (unsigned long)(e->w[0] & 0xFFFF), (unsigned)e->a, (unsigned long)(e->w[1] >> 16 & 0xFF),
             (unsigned long)(e->w[1] >> 8 & 0xFF), (unsigned long)(e->w[1] & 0xFF), (unsigned long)e->w[2]);
    break;
  case PL_MS_COUNTERS:
    snprintf(p, left, "driver counters: %lu offers, %lu attaches, %lu detaches; OUT packets sent from the attach %lu, "
             "from the main task %lu, from the IN completion (the OS's USB task) %lu",
             (unsigned long)e->w[0], (unsigned long)e->w[1], (unsigned long)e->w[2], (unsigned long)e->w[3],
             (unsigned long)e->w[4], (unsigned long)e->w[5]);
    break;
  case PL_EXIT:
    switch (e->a)
    {
    case PL_EXIT_LOGGED:
      snprintf(p, left, "exit: the files are written; the USB shutdowns come next (%lu pad(s) in)", (unsigned long)e->w[0]);
      break;
    case PL_EXIT_USB_BEGIN:
      snprintf(p, left, "exit: the USB shutdowns begin (watchdog backstop: %s)", e->w[0] ? "armed" : "not armed");
      break;
    case PL_EXIT_USB_DONE:
      snprintf(p, left, "exit: the USB shutdowns done in %lu ms", (unsigned long)e->w[0]);
      break;
    case PL_EXIT_TIMEOUT:
      snprintf(p, left, "exit: USB shutdown timed out at %s (%lu ms into the USB shutdowns): the calculator restarts",
               exit_step_text ? exit_step_text : "?", (unsigned long)e->w[0]);
      break;
    default:
      snprintf(p, left, "exit: %u", (unsigned)e->a);
    }
    break;
  case PL_POWER:
    if (e->a == 1)
      snprintf(p, left, "port: the adapter's power was turned on and stays off (%lu ms; OTGCSR or OTGSC %08lx: asked "
               "for, below a session's level): what is on the adapter draws more than the port gives (a controller "
               "charging its battery), or the port's supply failed", (unsigned long)e->w[0], (unsigned long)e->w[1]);
    else if (e->a == 2)
      snprintf(p, left, "port: the adapter's power came up after %lu ms held down", (unsigned long)e->w[0]);
    else
      snprintf(p, left, "port: the adapter's power came on %lu ms after the adapter was seen (%lu OS sleeps): the OS's own "
               "start of the port", (unsigned long)e->w[0], (unsigned long)e->w[1]);
    break;
  case PL_MS_INPUT:
    if (e->a == 2)
      snprintf(p, left, "pad: %04lx:%04lx (slot %lu) attached %lu ms ago and has sent NO REPORT: pressed, it does not "
               "work (the bring-up lines before this say how far it got)", (unsigned long)(e->w[0] >> 16),
               (unsigned long)(e->w[0] & 0xFFFF), (unsigned long)e->w[1], (unsigned long)e->w[2]);
    else
      snprintf(p, left, "pad: the first report of %04lx:%04lx (slot %lu)", (unsigned long)(e->w[0] >> 16),
               (unsigned long)(e->w[0] & 0xFFFF), (unsigned long)e->w[1]);
    break;
  default:
    snprintf(p, left, "? %u", (unsigned)e->kind);
  }
}

/* R11w: the milestones' lines, oldest first, each with its repeats. */
static void ms_line(const struct mstone *m, char *line, unsigned size)
{
  unsigned n;

  entry_text(&m->e, line, size);
  n = (unsigned)strlen(line);
  if (m->repeat && n < size)
    snprintf(line + n, size - n, " [x%lu more, the last at %lu ms]", (unsigned long)m->repeat,
             (unsigned long)((u64)(m->last_t - t0) * 1000 / NSPIRE_TICK_HZ));
}

static void produce(emit_fn emit, void *ctx, const char *why)
{
  char line[800], sys[300];
  char label[PADSTATUS_LINES][PADSTATUS_LABEL], value[PADSTATUS_LINES][PADSTATUS_VALUE];
  struct pad_status s;
  unsigned i;

  snprintf(line, sizeof(line), "gpSP build %s %s; gamepads: %s; pad log written %s at %lu ms (write %u)", NSPIRE_BUILD_GIT,
           NSPIRE_BUILD_DATE, NSPIRE_BUILD_PADS ? "built, driver " NSPIRE_BUILD_DRIVER : "not built", why,
           (unsigned long)ms_since(t0), writes + 1);
  emit(ctx, line);
  nspire_os_diag_system(sys, sizeof(sys));
  emit(ctx, sys);
  /* R11t: the timer block's shared clock select (the OS's value, gpSP's
   * write); R12: on a CX II the time base's lines after it (its mode and
   * why, the timer blocks as found, the speeds measured and refined) */
  nspire_timer_clock_text(line, sizeof(line));
  emit(ctx, line);
  for (i = 1; nspire_timer_header_text((int)i, line, sizeof(line)); i++)
    emit(ctx, line);
  /* (speed9a) which rule gives the OS tick's phase (its timer, or r15's from
   * the last wake) and why: the pad's ticks are taken by it */
  nspire_os_tick_rule_report(line, sizeof(line));
  emit(ctx, line);
  /* (r19-usb) the last run's exit, when it never reached its hand-back
   * (nspire_exit.c's trail on the disk: the step it froze in) */
  if (nspire_exit_trail_last_run && nspire_exit_trail_last_run()[0])
  {
    snprintf(line, sizeof(line), "LAST RUN: %s", nspire_exit_trail_last_run());
    emit(ctx, line);
  }
  nspire_padstatus_read(&s);
  nspire_padstatus_lines(&s, label, value);
  snprintf(line, sizeof(line), "Now: %s", nspire_padstatus_cause(nspire_padstatus_verdict(&s)));
  emit(ctx, line);
  for (i = 0; i < PADSTATUS_LINES; i++)
  {
    snprintf(line, sizeof(line), "  %.*s: %.*s", PADSTATUS_LABEL - 1, label[i], PADSTATUS_VALUE - 1, value[i]);
    emit(ctx, line);
  }
  nspire_padstatus_raw_text(&s, sys, sizeof(sys));
  snprintf(line, sizeof(line), "  raw: %s", sys);
  emit(ctx, line);
  snprintf(line, sizeof(line), "Events, oldest first (ms since gpSP started; %u kept, %u older dropped; %u held for tries "
           "that failed):", count, lost, kept);
  emit(ctx, line);
  for (i = 0; i < count; i++)
  {
    entry_text(&ring[(head + PADLOG_RING - count + i) % PADLOG_RING], line, sizeof(line));
    emit(ctx, line);
  }
  /* R11w: the milestones, and at exit the driver's whole block */
  snprintf(line, sizeof(line), "Milestones (offers, attaches, detaches, aborts / frees / closes, OUT packets and the "
           "task that sent them, the exit; never pushed out by port lines; the first %u kept, then the latest %u, "
           "%u dropped between):", ms_first_n, ms_count, ms_lost);
  emit(ctx, line);
  for (i = 0; i < ms_first_n; i++)
  {
    ms_line(&ms_first[i], line, sizeof(line));
    emit(ctx, line);
  }
  if (ms_lost)
  {
    snprintf(line, sizeof(line), "  (%u milestone(s) dropped here)", ms_lost);
    emit(ctx, line);
  }
  for (i = 0; i < ms_count; i++)
  {
    ms_line(&ms_ring[(ms_head + PADLOG_MS_RING - ms_count + i) % PADLOG_MS_RING], line, sizeof(line));
    emit(ctx, line);
  }
  if (nspire_usbpad_counters_text(line, sizeof(line)) > 0)
    emit(ctx, line);
  {
    /* Bluetooth's lines (nspire_bt_glue.c): its state, the dongle, counters */
    static char bt[1200];
    char *l = bt, *end;

    if (nspire_bt_glue_log_text(bt, sizeof(bt)) > 0)
      for (; *l; l = end + 1)
      {
        end = strchr(l, '\n');
        if (end)
          *end = 0;
        snprintf(line, sizeof(line), "%.700s", l);
        emit(ctx, line);
        if (!end)
          break;
      }
  }
  if (with_diag)
  {
    static char diag[6000];
    char *l = diag, *end;

    emit(ctx, "The driver's diag at exit (its counters, its latest trace records, the OS's offers):");
    if (nspire_usbpad_diag_text(diag, sizeof(diag)) > 0)
      for (; *l; l = end + 1)
      {
        end = strchr(l, '\n');
        if (end)
          *end = 0;
        snprintf(line, sizeof(line), "  %.700s", l);
        emit(ctx, line);
        if (!end)
          break;
      }
  }
}

/* Weak defaults (unit tests, no pads, no Bluetooth): no text. */
int nspire_bt_glue_log_text(char *out, unsigned size) __attribute__((weak, noipa));
int nspire_bt_glue_log_text(char *out, unsigned size)
{
  if (size)
    out[0] = 0;
  return 0;
}

int nspire_usbpad_counters_text(char *out, unsigned size) __attribute__((weak, noipa));
int nspire_usbpad_counters_text(char *out, unsigned size)
{
  if (size)
    out[0] = 0;
  return 0;
}

int nspire_usbpad_diag_text(char *out, unsigned size) __attribute__((weak, noipa));
int nspire_usbpad_diag_text(char *out, unsigned size)
{
  if (size)
    out[0] = 0;
  return 0;
}

static void emit_file(void *ctx, const char *line)
{
  fputs(line, (FILE *)ctx);
  fputc('\n', (FILE *)ctx);
}

struct buf
{
  char *out;
  unsigned size, len;
};

static void emit_buf(void *ctx, const char *line)
{
  struct buf *b = ctx;
  unsigned n = (unsigned)strlen(line);

  if (b->len + n + 2 > b->size)
    return;
  memcpy(b->out + b->len, line, n);
  b->len += n;
  b->out[b->len++] = '\n';
  b->out[b->len] = 0;
}

unsigned nspire_padlog_text(char *out, unsigned size, const char *why)
{
  struct buf b = { out, size, 0 };

  if (size)
    out[0] = 0;
  produce(emit_buf, &b, why);
  return b.len;
}

/* R2: whether the ring still holds an entry that says something (the idle
 * off-and-on alone may have pushed every one out). */
static int ring_marked(void)
{
  unsigned i;

  for (i = 0; i < count; i++)
    if (ring[(head + PADLOG_RING - count + i) % PADLOG_RING].kind & PL_MARK)
      return 1;
  return 0;
}

static void write_file(const char *why)
{
  char path[600], prev[600];
  FILE *f;

  /* R2: a ring with nothing in it but the idle line never replaces a file
   * written from one that had a try in it. */
  if (!ring_marked() && !nspire_padlog_milestones())
    return;
  /* R8: a failed open counts as a try and starts the timers too (a full
   * storage isn't asked again every frame). */
  attempts++;
  written_at = nspire_ticks();
  nspire_save_file(path, sizeof(path), PADLOG_FILE);
  if (!written)
  {
    /* The last run's file is kept once, as _prev. */
    written = 1;
    nspire_save_file(prev, sizeof(prev), PADLOG_PREV);
    remove(prev);
    rename(path, prev);
  }
  f = fopen(path, "w");
  if (!f)
    return;
  nspire_log_buffered(f);   /* (r19-usb-exit: 16 KB to the OS a call, not 1 KB) */
  produce(emit_file, f, why);
  fclose(f);
  writes++;
  news = 0;
  written_at = nspire_ticks();
}


void nspire_padlog_write_point(void)
{
  if (!interesting || exited || attempts >= PADLOG_WRITES_MAX || !nspire_link_quiet_for_writes())
    return;
  /* R11t: a file write stalls the USB callbacks 100-290 ms (the music
   * player's log) and a playing sound adapter has ~48 ms queued: while one
   * plays, the lines stay in the ring; only a try that ends in a failure is
   * written. The rest is written once the adapter is gone, or at exit. */
  if (nspire_usbaudio_active() && !(try_due && last_try != PL_TRY_PAD && last_try != PL_TRY_NOT_PAD))
    return;   /* (R11u: a device that is not a controller is not a failure either) */
  if (try_due && (!attempts || ms_since(written_at) >= PADLOG_TRY_GAP_MS))
  {
    try_due = 0;
    asked = NULL;
    write_file(try_name(last_try));
    return;
  }
  if (asked && news)
  {
    const char *why = asked;
    asked = NULL;
    write_file(why);
    return;
  }
  if (failing && news && (!attempts || ms_since(written_at) >= PADLOG_TIMER_MS))
    write_file("on the timer (something plugged in, nothing working)");
}

void nspire_padlog_exit(void)
{
  if (!interesting || exited)
    return;
  exited = 1;
  with_diag = 1;
  write_file("at exit, before the USB shutdowns");
}

/* R11w: written past the exit's write: the one line that says why the
 * calculator restarts (the step's words are a string that lives on). */
void nspire_padlog_exit_timeout(const char *step, uint32_t ms)
{
  exit_step_text = step;
  nspire_padlog_exit_mark(PL_EXIT_TIMEOUT, ms);
  exited = 1;
  with_diag = 1;
  write_file("at exit: USB shutdown timed out, the calculator restarts");
}

unsigned nspire_padlog_writes(void)
{
  return writes;
}

unsigned nspire_padlog_count(void)
{
  return count;
}

void nspire_padlog_reset(void)
{
  head = count = lost = news = writes = attempts = 0;
  kept = 0;
  interesting = try_due = failing = written = exited = 0;
  with_diag = 0;
  ms_first_n = ms_head = ms_count = ms_lost = 0;
  ms_last = NULL;
  exit_step_text = NULL;
  asked = NULL;
  t0_set = sampled = 0;
  last_sig = 0;
  last_nothing = 1;
  last_kind = -1;
  wires_counter = wires_gone = 0;
  wires_on = 0;
  link_was = -1;
}
