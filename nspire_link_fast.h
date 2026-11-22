/* The fast transport under the link's cable (nspire_link_fast.c): raw bulk
 * transfers on an endpoint of our own, polled, instead of NavNet.
 *
 * nspire_link_usb.c keeps NavNet as it was and hands a link to this file
 * once both hellos said they can (EXT_FAST, nspire_link.c), calling in
 * from its link_cable_* only where noted. Control messages between the two
 * transports go over NavNet as messages starting LINK_FAST_CTRL, 0x67,
 * 'F': nspire_link.c never sends that type, and a gpSP that
 * doesn't offer the fast transport never gets one. */
#ifndef NSPIRE_LINK_FAST_H
#define NSPIRE_LINK_FAST_H

#include <stdint.h>

/* The hello's EXT_FAST value: what this calculator can be on the fast
 * transport now, and the transport's version in bits 8-15. */
#define LINK_FAST_CAN_HOST   1u   /* USB host (adapter end): drives its own async queue heads */
#define LINK_FAST_CAN_DEVICE 2u   /* USB device (cable end), a ChipIdea: an endpoint of its own */
#define LINK_FAST_VERSION    1u
#define LINK_FAST_CTRL       0xF0 /* a transport's control message's first byte */

/* What NavNet gives the fast transport (nspire_link_usb.c): one message out
 * (written before it returns, as far as the OS lets), one in (control
 * messages never come back through it: they go to link_fast_control), and
 * whether this side's NavNet writes are all out. */
struct link_fast_nav
{
  int (*send)(const void *msg, unsigned len);
  int (*receive)(void *buf, unsigned size, unsigned wait_ms);
  int (*writes_out)(void);
};

/* The hello's offer (0: none, e.g. a CX II at the device end). */
uint32_t link_fast_offer(void);
/* After both hellos, both sides at once: brings the fast transport up with
 * the other side's offer, or leaves NavNet as it is (a log line says why).
 * 1 if it's up, 0 not. Up to ~9 s (the host: READY 5 s, SYN and ACK 4 s).
 * refused: the USB port's arbiter (nspire_arbiter.c) says this side may
 * not take the controller now (why), or NULL: the other side, if it tries,
 * is told FAIL at once and both stay on NavNet. */
int link_fast_start(uint32_t other_offer, const struct link_fast_nav *nav, const char *refused);
/* 1 while the transport runs on the controller (up: NavNet idle beside it). */
int link_fast_up(void);
/* 1 while any word of the controller's, or the interrupt line, is ours
 * (a bus power cycle must not be made then). */
int link_fast_holds(void);
/* Why the controller wasn't given back as found at the last give back
 * (the OS's schedule address or enable, VIC bit 8, the endpoint's control;
 * a schedule that wouldn't stop), or NULL. Cleared when a link starts with
 * nothing held. The off switch (nspire_link_speed.c) keeps the fast
 * transport off after it, as after a crash. */
const char *link_fast_damage(void);
/* Lets go now (the arbiter's word: the port's owner changed, a pad, a
 * power cycle): BYE to the other side, everything back, RESUME on NavNet,
 * which carries the link from there. Nothing if it isn't up. */
void link_fast_drop(const char *why);
/* 1 while link_cable_send/receive go here (up, or falling back: the
 * other's RESUME not come yet, or messages kept from the start). */
int link_fast_owns(void);
int link_fast_send(const void *msg, unsigned len);
/* A message, 0 none in wait_ms, < 0 broken, or LINK_FAST_NAVNET: NavNet
 * carries the link now (it fell back), read it there. */
#define LINK_FAST_NAVNET (-1000)
int link_fast_receive(void *buf, unsigned size, unsigned wait_ms);
/* A NavNet message is the fast transport's control message (LINK_FAST_CTRL,
 * 0x67, 'F', op): only once this link tried it (until link_fast_stop). */
int link_fast_is_control(const uint8_t *msg, unsigned len);
/* A control message that came over NavNet. */
void link_fast_control(const uint8_t *msg, unsigned len);
/* 1 once after a fall back mid-link has resumed on NavNet. */
int link_fast_fell_back(void);
/* Polls (sends what waits), at a frame's end and on a flush. */
void link_fast_poll(void);
/* The link's over (close, shutdown): every register and word put back. */
void link_fast_stop(const char *why);
/* For the ring's line each second: "; fast up, N/M packets this second"
 * (empty if it never ran). */
void link_fast_second(char *text, unsigned size);
/* For the log's transport line (empty if it never ran). */
void link_fast_stats(char *text, unsigned size);

#endif
