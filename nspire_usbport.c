/* The USB port observer: see nspire_usbport.h. */
#include <stdio.h>
#include "common.h"
#include "nspire_usbport.h"
#include "nspire_link.h"
#include "nspire_os.h"
#include "nspire.h"

#define EVERY_CALLS 8
#define MIN_TICKS   4000   /* 122 ms: two callers (link, pad glue) share the 8 */
#define EDGES       4

static enum port_state state;
static uint32_t bits, changed_at, changes, samples, calls, sampled_at, max_gap;
static int started;
static enum port_os os_what;
static unsigned os_cls, os_vid;
static nspire_usbport_edge_fn edges[EDGES];

/* SEAM-13: ticks to ms exactly (32768 / 1000 truncated to 32 ran 2.4% short). */
static uint32_t ms_of(uint32_t ticks) { return (uint32_t)((uint64_t)ticks * 1000 / NSPIRE_TICK_HZ); }

const char *nspire_usbport_name(enum port_state s)
{
  static const char *const names[] = {"EMPTY", "DEVICE_END", "HOST_END", "UNKNOWN"};

  return (unsigned)s < 4 ? names[s] : "?";
}

/* nspire_usb_bits' role bit: a CX II's OTGCSR bit 21 (bit 5 here), a CX's
 * OTGSC bit 8 (bit 0): 1 the device's end (as nspire_link.c's usb_is_host). */
static enum port_state classify(uint32_t *out_bits)
{
  int plugged = nspire_usb_plugged();
  uint32_t b;

  *out_bits = 0;
  if (plugged == 0)
    return PORT_EMPTY;
  if (plugged < 0)
    return PORT_UNKNOWN;   /* nspire_usb_gated() says why (link_cable_gated handles it) */
  b = nspire_usb_bits();
  *out_bits = b;
  switch (b >> 28)
  {
  case 2:
    return b & 0x20 ? PORT_DEVICE_END : PORT_HOST_END;
  case 1:
    return b & 0x01 ? PORT_DEVICE_END : PORT_HOST_END;
  }
  return PORT_UNKNOWN;
}

void nspire_usbport_poll(void)
{
  enum port_state now_state;
  uint32_t b, now, gap;
  int i;

  if (++calls < EVERY_CALLS)
    return;
  now = nspire_ticks();
  if (started && now - sampled_at < MIN_TICKS)
    return;
  calls = 0;
  gap = started ? now - sampled_at : 0;
  if (gap > max_gap)
    max_gap = gap;
  sampled_at = now;
  samples++;
  now_state = classify(&b);
  if (!started)
  {
    started = 1;
    changed_at = now;
    state = PORT_EMPTY;
    /* Nothing on the port at the start: no line (a game played without the
     * cable leaves the last log as it is, nspire_link.c). */
  }
  if (now_state == state && b == bits)
    return;
  changes++;
  nspire_link_log("seamless: port %s (bits %08x) was %s (bits %08x) for %u ms; change %u; seen %u ms after the last look; %s", nspire_usbport_name(now_state),
                  (unsigned)b, nspire_usbport_name(state), (unsigned)bits,
                  (unsigned)ms_of(now - changed_at), (unsigned)changes, (unsigned)ms_of(gap),
                  now_state == PORT_UNKNOWN && nspire_usb_gated() ? "controller gated off (can't tell)" :
                  nspire_usb_role_text(b));
  nspire_link_log_request_flush();
  {
    enum port_state from = state;

    state = now_state;
    bits = b;
    changed_at = now;
    os_what = PORT_OS_NONE;
    os_cls = os_vid = 0;
    if (from != now_state)
      for (i = 0; i < EDGES; i++)
        if (edges[i])
          edges[i](from, now_state);
  }
}

enum port_state nspire_usbport_state(void) { return state; }
uint32_t nspire_usbport_bits(void) { return bits; }
uint32_t nspire_usbport_age_ms(void) { return ms_of(nspire_ticks() - changed_at); }
uint32_t nspire_usbport_changed_counter(void) { return changes; }
uint32_t nspire_usbport_samples(void) { return samples; }
/* The longest time between two looks: what a plug-in waits for to be seen. */
uint32_t nspire_usbport_max_gap_ms(void) { return ms_of(max_gap); }

void nspire_usbport_os_report(enum port_os what, unsigned cls, unsigned vid)
{
  static const char *const names[] = {"none", "node with gpSP's service", "node without it", "match offer", "drive"};

  if (what == os_what && cls == os_cls && vid == os_vid)
    return;
  os_what = what;
  os_cls = cls;
  os_vid = vid;
  nspire_link_log("seamless: OS side: %s (class %u, vid %04x) %u ms after the port change", names[what < 5 ? what : 0], cls,
                  vid, (unsigned)nspire_usbport_age_ms());
  nspire_link_log_request_flush();
}

enum port_os nspire_usbport_os(void) { return os_what; }
unsigned nspire_usbport_os_class(void) { return os_cls; }
unsigned nspire_usbport_os_vid(void) { return os_vid; }

int nspire_usbport_on_edge(nspire_usbport_edge_fn fn)
{
  int i;

  for (i = 0; i < EDGES; i++)
    if (!edges[i])
    {
      edges[i] = fn;
      return 0;
    }
  return -1;
}

void nspire_usbport_reset(void)
{
  int i;

  state = PORT_EMPTY;
  bits = changed_at = changes = samples = calls = sampled_at = max_gap = 0;
  started = 0;
  os_what = PORT_OS_NONE;
  os_cls = os_vid = 0;
  for (i = 0; i < EDGES; i++)
    edges[i] = 0;
}
