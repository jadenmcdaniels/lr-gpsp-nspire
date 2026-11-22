/* The USB port observer (S1): one place that says what is on the port.
 *
 * It reads only nspire_usb_plugged / nspire_usb_bits / nspire_usb_gated
 * (nspire_os.c), at most once in 8 frames (nspire_usbport_poll, from the
 * link's frame start and the gamepad glue), takes no OS time and writes no
 * file (its log lines ask for the write at the next frame start, as the pad
 * glue's do). With the port EMPTY a poll is a counter and nothing more.
 *
 * Nothing decides by it yet: the link and the pad glue still use their own
 * checks; later steps (S2 on) read this instead. */
#ifndef NSPIRE_USBPORT_H
#define NSPIRE_USBPORT_H

#include <stdint.h>

enum port_state
{
  PORT_EMPTY,        /* nothing on the port */
  PORT_DEVICE_END,   /* the cable's end: the other side is the host (a calculator's A end, a PC) */
  PORT_HOST_END,     /* the adapter's end: this calculator is the host (pad, drive, calculator...) */
  PORT_UNKNOWN       /* can't tell: a CX's controller gated off, registers stale, role bit unknown */
};

/* What the OS side reported for what's on the port (nspire_usbport_os_report,
 * called by whoever sees it: the link's node watch, the pad glue's events). It
 * is forgotten when the port changes. */
enum port_os
{
  PORT_OS_NONE,           /* nothing reported */
  PORT_OS_NODE_SERVICE,   /* a NavNet node with gpSP's service */
  PORT_OS_NODE_NO_SERVICE,/* a node without it */
  PORT_OS_OFFER,          /* a device match offered by the OS: class and vendor id say what */
  PORT_OS_DRIVE           /* a mass storage drive */
};

/* Takes a sample every 8th call (and no sooner than 122 ms after the last),
 * the call being cheap otherwise. */
void nspire_usbport_poll(void);

enum port_state nspire_usbport_state(void);
/* The bits (nspire_usb_bits) at the last sample; 0 while EMPTY. */
uint32_t nspire_usbport_bits(void);
/* ms since the port last changed (since the start before any change). */
uint32_t nspire_usbport_age_ms(void);
/* Changes seen so far (a state or a bits change; the start's first reading
 * counts if something is on the port). */
uint32_t nspire_usbport_changed_counter(void);
/* Samples taken so far (the tests count them). */
uint32_t nspire_usbport_samples(void);
uint32_t nspire_usbport_max_gap_ms(void);   /* S8: the longest time between two looks */

void nspire_usbport_os_report(enum port_os what, unsigned cls, unsigned vid);
enum port_os nspire_usbport_os(void);
unsigned nspire_usbport_os_class(void);
unsigned nspire_usbport_os_vid(void);

/* Called at each change of state (not of the bits alone), from nspire_usbport_poll. */
typedef void (*nspire_usbport_edge_fn)(enum port_state from, enum port_state to);
int nspire_usbport_on_edge(nspire_usbport_edge_fn fn);   /* 0 ok, -1 full (4 at most) */

const char *nspire_usbport_name(enum port_state s);
/* Forgets everything (the tests). */
void nspire_usbport_reset(void);

#endif
