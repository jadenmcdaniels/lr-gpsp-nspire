#include <stdio.h>
#include "common.h"
#include "nspire_arbiter.h"
#include "nspire_usbport.h"
#include "nspire_link.h"
#include "nspire_os.h"
#include "nspire.h"
#include "nspire_frontend.h"
#include "nspire_padlog.h"
#include "nspire_padstatus.h"

/* What the other owners say (weak: a build without that driver has none).
 * They were nspire_link.c's. */
int nspire_usbpad_active(void) __attribute__((weak, noipa));
int nspire_usbpad_active(void) { return 0; }
/* The pad driver's own USB mode (Direct): NavNet isn't touched, no link. */
int nspire_usbpad_owns_usb(void) __attribute__((weak, noipa));
int nspire_usbpad_owns_usb(void) { return 0; }
/* A USB flash drive's driver (another branch) while a drive's in. */
int nspire_usbms_active(void) __attribute__((weak, noipa));
int nspire_usbms_active(void) { return 0; }
/* An audio endpoint is claimed during setup, even before it streams. Its
 * OS transfers may also still own the controller after an unplug: the sink
 * keeps owns_usb true until their callbacks and pipe have retired. */
int nspire_usbaudio_active(void) __attribute__((weak, noipa));
int nspire_usbaudio_active(void) { return 0; }
int nspire_usbaudio_owns_usb(void) __attribute__((weak, noipa));
int nspire_usbaudio_owns_usb(void) { return 0; }

/* The Bluetooth dongle attached and its driver's (nspire_bt_glue.c). */
int nspire_bt_usb_active(void) __attribute__((weak, noipa));
int nspire_bt_usb_active(void) { return 0; }

/* (the sound adapter's, or the Bluetooth dongle's: a working device of
 * gpSP's other than a pad or a drive) */
static int audio_claimed(void)
{
  return nspire_usbaudio_active() || nspire_usbaudio_owns_usb() || nspire_bt_usb_active();
}

/* SEAM-8: the pad glue's word on what the OS offered since the port last
 * changed (a match, an attach, a refusal, an unsupported device): then it is
 * being handled, and nothing here cuts its power (weak: never). */
int nspire_usbpad_offered(void) __attribute__((weak, noipa));
int nspire_usbpad_offered(void) { return 0; }
/* PAD4: the pad glue's point between frames (its come-up: the OS's time in
 * one go, the restart when due); weak: none. */
void nspire_usbpad_frame_point(void) __attribute__((weak, noipa));
void nspire_usbpad_frame_point(void) {}
/* PAD4: the OS has the device and works on it (the glue's come-up): the last
 * resort's "Unplug and replug" waits (a replug starts the OS's wait over). */
int nspire_usbpad_coming_up(void) __attribute__((weak, noipa));
int nspire_usbpad_coming_up(void) { return 0; }

/* Hub (2026-11-16, the glue): a USB hub is on the adapter. A power restart
 * of the port cuts the power of everything on the hub (a sound adapter
 * playing, a drive, a controller in use), and what the stages below read
 * off the port's lines (one device came, the OS's offer of it, it left) a
 * hub holds still for as long as it is in: none of them runs then, and
 * nobody restarts the port (weak: never). */
int nspire_usbhub_present(void) __attribute__((weak, noipa));
int nspire_usbhub_present(void) { return 0; }

static enum arb_owner owner;
static unsigned stage, changes;
static u32 stage_at;           /* the tick the stage began */
static u32 port_counter;       /* nspire_usbport_changed_counter at the last tick */
static enum port_state port_was;
static u32 change_at;          /* the port's last change that wasn't our cycle's: the stages' clock */
static nspire_arb_hook_fn hooks[5];
/* A session: from the port's first non-EMPTY reading until it reads EMPTY
 * for ARB_SESSION_END_MS (an adapter or cable pulled out: its ID pin; the
 * host's end never reads EMPTY in a cycle), or UNKNOWN with a CX's
 * controller gated off as long (SEAM-14: a CX gates it as soon as the port
 * is empty) unless that gate may be our own cycle's (its quiet window, or a
 * PP still to put back). Its cycles, toasts and claims last that long:
 * owner flips and bits changes don't give new ones (SEAM-6). */
static int in_session, empty_seen;
static int handled;            /* SEAM-15: the pad driver said the OS offered the device: no cycle, not asked again */
static u32 empty_at;
static unsigned cycles;        /* power cycles this session (S4) */
static int cycled;             /* a cycle happened this session (SEAM-13: not cycle_at == 0) */
static u32 cycle_at;           /* the tick of the last one's end */
static unsigned refused;       /* the stages' last cycle found nothing to cycle (-1) */
static int link_cycled;        /* the last cycle was the link's own (nspire_arb_link_cycle) */
static unsigned stopped;       /* no further cycle: can't here (-2), or VBUS didn't come back (0) */
static unsigned told;          /* S7: the toasts shown this session (bits) */
static unsigned claimed;       /* a pad, a link or a drive owned the port since it last went through EMPTY/UNKNOWN */
static unsigned connected_seen, enabled_seen;   /* SEAM-10: the port's bits 0x100 / 0x400 this session */
static const char *veto_said;  /* the last reason no cycle was made (logged when it changes) */
static u32 wires_counter;      /* PAD3: nspire_padlog_wires_counter at the last tick */
static u32 wires_gone;         /* R1: nspire_padlog_wires_gone at the last tick */
static int gone_real;          /* R1: a device left the wires outside a cycle's quiet window since the last came */
static unsigned session_cycles;   /* R1: cycles this session over all its devices (ARB_SESSION_CYCLES_MAX) */
static int claimed_since_cycle;   /* PAD4: a pad (link, drive) had the port since the last cycle */
#define TOLD_START 1
#define TOLD_GAVE_UP 2

static u32 ms_of(u32 ticks) { return (u32)((u64)ticks * 1000 / NSPIRE_TICK_HZ); }
static u32 since_ms(u32 at) { return ms_of(nspire_ticks() - at); }

static enum arb_owner classify(void)
{
  enum port_state p = nspire_usbport_state();
  enum port_os os = nspire_usbport_os();

  if (nspire_link_cable_present() || os == PORT_OS_NODE_SERVICE || os == PORT_OS_NODE_NO_SERVICE)
    return ARB_LINK;
  if (nspire_usbpad_active())
    return ARB_PAD;
  if (nspire_usbpad_owns_usb() || nspire_usbms_active() || audio_claimed() ||
      os == PORT_OS_OFFER || os == PORT_OS_DRIVE)
    return ARB_OTHER;
  return p == PORT_EMPTY || p == PORT_UNKNOWN ? ARB_NONE : ARB_WAIT;
}

static int swap_hint;   /* the last resort said to swap the cable's ends */

int nspire_arb_swap_hint(void)
{
  return swap_hint;
}

/* R1: what one device's try uses up (its cycles, the last resort's toast). */
static void device_reset(void)
{
  cycles = 0;
  cycled = link_cycled = 0;
  refused = stopped = told = 0;
  swap_hint = 0;
  connected_seen = enabled_seen = 0;
}

static void session_reset(void)
{
  device_reset();
  session_cycles = 0;
  gone_real = claimed_since_cycle = 0;
  claimed = 0;
  handled = 0;
  veto_said = 0;
}

/* Why a power cycle may not be made now (0: it may). Only from the frame
 * start (nspire_arb_frame), once everything else says a cycle is due: the
 * last check takes a fresh NavNet look (nspire_link_cycle_veto). */
static const char *veto(void)
{
  const char *why;

  /* the cheap checks first: this runs each frame while a cycle is due */
  if (nspire_usbhub_present())
    why = "a USB hub is in (a restart would cut every device on it)";
  else if (nspire_arb_link_blocked() || nspire_link_cable_present())
    why = "the port is claimed";
  else if (nspire_usbport_os() != PORT_OS_NONE)
    why = "the OS offered the device to a driver";
  else if (handled)
    why = "the OS offered the device to a driver";
  else if (!(why = nspire_link_cycle_veto(change_at, 0)))
  {
    /* The pad driver's diag text: once it says so, latched until the port
     * goes through EMPTY/UNKNOWN (SEAM-15: not a 900-byte text each frame
     * while an adapter whose attach failed stays in). */
    if (nspire_usbpad_offered())
    {
      handled = 1;
      why = "the OS offered the device to a driver";
    }
    else
      why = nspire_link_cycle_veto(change_at, 1);   /* a NavNet look, last */
  }
  if (why && why != veto_said)
  {
    nspire_link_log("seamless: no power cycle yet: %s (%u ms after the port change)", why, (unsigned)since_ms(change_at));
    nspire_link_log_request_flush();
  }
  veto_said = why;
  return why;
}

/* PAD4 (R10a's CX II, its hardware log): where a power restart
 * helps. A device on the wires the port never latched as connected (PORTSC
 * 00000800, 0000088a): both such restarts latched it within 0.26 s. A port
 * enabled with the host controller running and nothing offered (S4's
 * device, there before the driver registered: the OS is done with it).
 * Not while the OS works on a connected device (connected, not enabled, or
 * in reset): the stages' two restarts at 3.3 s and 8.1 s set the Switch
 * Pro back each time (connected at 640353 and 643875 and 648699, the OS's
 * reset only 9.8 s after a connect left alone); nor on a stopped host
 * controller with the device connected: none of the restarts there started
 * it (423609: 62 s later; 745684, 766120: 38 s and 17 s). A controller
 * that doesn't read (the unit tests' stubs): as before. 1 it helps, 0 not
 * now (the OS is on it), -1 nothing on the wires (nspire_usb_reenumerate
 * says "nothing to cycle" itself, as before: the swap hint's evidence). */
/* (2026-11-17) The pad glue woke the transceiver under a device that
 * waited on it (connected, never enabled: nspire_usbcycle.c): for
 * ARB_PHY_HINT_MS that device is not one the OS works on, and a restart is
 * what starts it over. */
#define ARB_PHY_HINT_MS 8000
static int phy_hint;
static u32 phy_hint_at;

void nspire_arb_phy_woken(void)
{
  phy_hint = 1;
  phy_hint_at = nspire_ticks();
}

static int restart_fits(void)
{
  uint32_t w[NSPIRE_USB_RAW];
  int kind = nspire_usb_raw(w);

  if (!kind)
    return 1;
  if (!(w[4] & 1))
    return w[4] & 0xC00 ? 1 : -1;
  /* (2026-11-17) A CX II's port with its suspend bit and a device connected,
   * not in reset: the OS is not at work on it. It suspended the device
   * (nobody used it: what was plugged in before gpSP's drivers were there),
   * or the device came onto a transceiver an earlier suspend left asleep
   * and can never be enabled (nspire_usbcycle.c nspire_usb_phy_wake). The
   * restart is the way out of both: its drop wakes the transceiver, and the
   * OS starts over with the device and gpSP's drivers. (The restarts PAD4
   * saw do nothing on a stopped host controller were made with the
   * transceiver left asleep.) */
  if (kind == 2 && (w[4] & 0x80) && !(w[4] & 0x100))
    return 1;
  if (phy_hint && since_ms(phy_hint_at) >= ARB_PHY_HINT_MS)
    phy_hint = 0;
  if (kind == 2 && phy_hint && !(w[4] & 4) && !(w[4] & 0x100))
    return 1;
  return (w[4] & 4) && (w[2] & 1) && !(w[4] & 0x100);
}

static int restart_helps(void)
{
  return restart_fits() == 1;
}

/* S4: the controller side's bus power cycle, at most ARB_CYCLES_MAX a
 * session. Nothing but an unclaimed device at
 * the host's end gets here (classify), and veto() said yes. */
static int none_left(void)
{
  return cycles >= ARB_CYCLES_MAX || stopped || refused;
}

/* from_link: the link's own (nspire_arb_link_cycle). Its "nothing to
 * cycle" (-1: the cable left between its look and the cycle) is not the
 * stages' evidence for a cable the wrong way round: no refused latch, which
 * would also stop the stages' cycles for whatever comes next this session
 * (F4). */
static int cycle(int from_link)
{
  int r;

  if (none_left())
    return -1;
  cycles++;
  session_cycles++;
  claimed_since_cycle = 0;
  r = nspire_usb_reenumerate((int)cycles);
  nspire_padlog_add(PL_CYCLE, (int)cycles, from_link ? 2 : 0, 0, (u32)r, 0, 0);
  link_cycled = from_link && r > 0;
  if (r == -1)
  {
    nspire_link_log("seamless: cycle %u refused (nothing connected at the host's end: nothing to cycle)", cycles);
    if (!from_link)
      refused = 1;
  }
  else if (r < 0)
  {
    nspire_link_log("seamless: cycle %u refused (the port's registers say it can't be done safely)", cycles);
    stopped = 1;
  }
  else
  {
    cycled = 1;
    cycle_at = nspire_ticks();
    if (r == 0)
      stopped = 1;   /* VBUS didn't come back, or gated off in the drop: no second */
  }
  nspire_link_log_request_flush();
  return r < -1 ? -2 : r;
}

static void set_stage(unsigned s)
{
  static const char *const names[] = { "wait for the OS's offer", "power cycle", "wait again", "direct read", "gave up" };

  stage = s;
  stage_at = nspire_ticks();
  nspire_link_log("seamless: stage %u (%s), %u ms after the port change", s, names[s < 5 ? s : 0],
                  (unsigned)since_ms(change_at));
  nspire_link_log_request_flush();
  nspire_padlog_add(PL_STAGE, (int)s, 0, 0, 0, 0, 0);
  if (s == ARB_STAGE_GAVE_UP)
    nspire_padlog_try_end(PL_TRY_GAVE_UP);
  if (hooks[s])
    hooks[s]();
  else if (s == ARB_STAGE_CYCLE && restart_fits() != 0)
    cycle(0);
  else if (s == ARB_STAGE_CYCLE && !none_left())
  {
    /* PAD4: the OS is on the device (or its stopped controller): no restart */
    nspire_link_log("seamless: no power cycle: the port says the OS is working on the device (connected, not enabled or "
                    "in reset), or its host controller is stopped with the device connected");
    nspire_padlog_add(PL_CYCLE, (int)cycles + 1, 0, 0, (u32)-5, 0, 0);
  }
  /* S7: the last resort, once a session. Nothing to cycle, or a port that
   * was connected but never enabled (a CX host end with a calculator on the
   * cable: its OS never enumerates, R2): the cable's ends are likely
   * the wrong way round (SEAM-10). Not after something owned the port since
   * it last went through EMPTY (S9, SEAM-5): the pad or drive was pulled out
   * of an adapter that stayed in, nothing is wrong. */
  /* R11w part 2 (2026-11-15): "swap the cable" came with controllers (seen
   * on a calculator; the R11w pad log 245420-252289: a controller pulled out
   * of an adapter that stayed in, the restart found nothing on the wires,
   * `refused`, and this said to swap the cable). Now: nothing on the wires
   * (refused, never connected): nothing is said (there is no device to act
   * on; the Controller page says the adapter is empty); else the plain
   * replug, the plug's one message that asks something
   * (nspire_padstatus_may_ask). The swap advice is the link's alone: it is
   * right only while the user is linking, and then the link owns the port
   * (classify: ARB_LINK while LINK_LOOKING or a calculator's node is seen),
   * so these stages never run to here: the link's own hint speaks ("Unplug
   * the link cable and plug it in again", the flap hint). Deliberately, this
   * never says to swap and nspire_arb_swap_hint() stays 0 (part 2:
   * a linking-only swap here was unreachable; now it says so;
   * frontend_tests test_arbiter_cycles' "linking" row checks it). */
  if (s == ARB_STAGE_GAVE_UP && !(told & TOLD_GAVE_UP))
  {
    int nothing = refused && !connected_seen;
    int waiting = nspire_usbpad_coming_up();   /* PAD4: the OS still works on the device */

    told |= TOLD_GAVE_UP;
    swap_hint = 0;
    if (waiting)
    {
      nspire_link_log("seamless: no last resort: the OS has the device and is still bringing it up (the pad's come-up)");
      nspire_link_log_request_flush();
    }
    else if (!claimed && !nothing && nspire_padstatus_may_ask())
    {
      nspire_padstatus_asked();
      nspire_osd_message(ARB_REPLUG_TEXT);
    }
    else if (!claimed)
    {
      nspire_link_log("seamless: no last resort said: %s", nothing ? "nothing on the wires (no device to act on)" :
                      "this plug's message was said already");
      nspire_link_log_request_flush();
    }
  }
}

/* Bookkeeping only: who owns the port, its session, its last change. No
 * stage moves and nothing touches the port (this runs wherever the keys are
 * read: menus, the file list, inside the emulated frame: SEAM-12). */
void nspire_arb_tick(void)
{
  enum arb_owner now = classify();
  enum port_state ps = nspire_usbport_state();
  u32 counter = nspire_usbport_changed_counter();
  int quiet = nspire_arb_cycling();   /* our own cycle moves the bits */
  int gate_gone;

  nspire_usb_power_restore();
  /* SEAM-14: a CX gated off with nothing in reads UNKNOWN, not EMPTY. Our
   * own cycle's gate (its quiet window, PP still to put back) never ends a
   * session (SEAM-6). */
  gate_gone = ps == PORT_UNKNOWN && nspire_usb_gated() && !quiet && !nspire_usb_power_restore_pending();
  if (ps == PORT_EMPTY || gate_gone)
  {
    if (!empty_seen)
    {
      empty_seen = 1;
      empty_at = nspire_ticks();
    }
    else if (in_session && since_ms(empty_at) >= ARB_SESSION_END_MS)
    {
      /* the first hardware test: does a CX gate only with nothing in? */
      nspire_link_log("seamless: session ended: port %s %u ms (%u cycles)", gate_gone ? "gated" : "EMPTY",
                      (unsigned)since_ms(empty_at), cycles);
      nspire_link_log_request_flush();
      in_session = 0;
      session_reset();
    }
  }
  else
  {
    empty_seen = 0;
    in_session = 1;
    if (ps == PORT_HOST_END)
    {
      connected_seen |= (nspire_usbport_bits() & 0x100) != 0;
      enabled_seen |= (nspire_usbport_bits() & 0x400) != 0;
    }
  }
  /* PAD3 (F2): a device onto the wires of an adapter that stayed in (its
   * bits may not change: the OS's host controller stopped, R9F) is a new
   * one: the claim of what was there before (the link's calculator) and
   * the OS's old offer don't hold for it (they held off every cycle and
   * every toast for the controller after a link, R9F's CX II). */
  /* R1 (2026-11-11): a device that left the wires outside a cycle's
   * quiet window was pulled out (a cycle's own drop is inside it), so the
   * next one onto them is a new try: its own two cycles and its own last
   * resort, not the ones the device before used up (four pads in turn in
   * one adapter that never reads EMPTY: pads 2-4 got neither). */
  if (owner == ARB_PAD || owner == ARB_LINK || owner == ARB_OTHER)
    claimed_since_cycle = 1;
  if (nspire_padlog_wires_gone() != wires_gone)
  {
    wires_gone = nspire_padlog_wires_gone();
    /* PAD4: a pad that attached after the cycle and then left was pulled
     * out, quiet window or not (a quick swap within 3 s of the restart kept
     * the next pad from its own try) */
    if (!quiet || claimed_since_cycle)
      gone_real = 1;
  }
  if (nspire_padlog_wires_counter() != wires_counter)
  {
    wires_counter = nspire_padlog_wires_counter();
    if (ps == PORT_HOST_END && owner != ARB_PAD && owner != ARB_LINK && owner != ARB_OTHER && (!quiet || claimed_since_cycle))
    {
      claimed = handled = 0;
      veto_said = 0;
      /* PAD4 (R10a 397138 -> 402487): an adapter put in empty got "nothing
       * to restart" (-1), which latched `refused`, and the 8BitDo on its
       * wires 3.8 s later got no restart at all: a device onto the wires that
       * no device left before (the first after an empty adapter) is a new
       * try as well. */
      if ((gone_real || refused) && session_cycles < ARB_SESSION_CYCLES_MAX)
        device_reset();
      stage = ARB_STAGE_WAIT_OFFER;
      stage_at = change_at = nspire_ticks();
    }
    gone_real = 0;
  }
  if (counter != port_counter)
  {
    port_counter = counter;
    /* Through EMPTY or UNKNOWN: whatever claimed the port is gone with it.
     * A bits change at the host's end (a device out of an adapter that
     * stays in) keeps the claim (SEAM-5). */
    if (ps != PORT_HOST_END || port_was != PORT_HOST_END)
      claimed = handled = 0;
    port_was = ps;
    if (!quiet)
    {
      stage = ARB_STAGE_WAIT_OFFER;
      stage_at = change_at = nspire_ticks();
    }
  }
  if (now != owner)
  {
    nspire_link_log("seamless: port owner %s (was %s); port %s", nspire_arb_owner_name(now), nspire_arb_owner_name(owner),
                    nspire_usbport_name(ps));
    nspire_link_log_request_flush();
    nspire_padlog_add(PL_OWNER, now, owner, 0, 0, 0, 0);
#ifdef NSPIRE_HOST
    /* The PC tests' untrimmed trail (pad.txt): the link log keeps only its
     * last 3 KB before a link (DETECT log_begin), which may not reach back
     * to the run's first owners. */
    {
      extern void host_usbpad_note(const char *text) __attribute__((weak));
      char note[64];

      if (host_usbpad_note)
      {
        snprintf(note, sizeof(note), "arbiter: port owner %s (was %s)", nspire_arb_owner_name(now),
                 nspire_arb_owner_name(owner));
        host_usbpad_note(note);
      }
    }
#endif
    owner = now;
    changes++;
    if (!quiet)
    {
      stage = ARB_STAGE_WAIT_OFFER;   /* the wait for an offer starts when nobody claims */
      stage_at = nspire_ticks();
    }
  }
  if (owner == ARB_PAD || owner == ARB_LINK || owner == ARB_OTHER)
    claimed = 1;
  /* The fast transport holds the controller only as long as this says so:
   * it lets go at the first tick that doesn't (NavNet carries the link). */
  if (link_cable_fast_up())
  {
    const char *why = nspire_arb_fast_refusal();
    if (why)
    {
      nspire_link_log("seamless: the fast link lets go of the USB controller: %s", why);
      nspire_link_log_request_flush();
      link_cable_fast_drop(why);
    }
  }
}

int nspire_arb_link_cycle(void)
{
  /* A stale calculator report can keep LINK precedence while another
   * driver attaches or drains. Its live resource claim must win here too. */
  if (owner != ARB_LINK || nspire_usbport_state() != PORT_HOST_END || nspire_arb_link_blocked() ||
      nspire_usbhub_present())
    return -2;
  if (none_left())
    return -3;
  return cycle(1);
}

/* F1: the link's own cycle is over for the fast transport once the
 * port is enabled again with the host controller running (the OS reset the
 * new connect and enumerates; NavNet's node comes after that): without this
 * the hello, ~2 s after the cycle (r6: 1.7 s from port enabled to the fast
 * decision), fell in ARB_CYCLE_QUIET_MS and the whole link ran on NavNet.
 * A CX's PP still to put back, or a port not (yet) enabled, still refuses. */
static int link_cycle_done(void)
{
  return link_cycled && nspire_usbport_state() == PORT_HOST_END && (nspire_usbport_bits() & 0x400) &&
         nspire_usb_running() == 1;
}

const char *nspire_arb_fast_refusal(void)
{
  static char why[40];

  if (owner != ARB_LINK)
  {
    snprintf(why, sizeof(why), "the port's owner is %s", nspire_arb_owner_name(owner));
    return why;
  }
  if ((nspire_arb_cycling() && !link_cycle_done()) || nspire_usb_power_restore_pending())
    return "a bus power cycle was just made";
  /* (hub: its take-over masks the USB interrupt line: every device on a hub
   * would stand still, claimed by a driver or not) */
  if (nspire_usbhub_present())
    return "a USB hub is in";
  if (nspire_usbpad_active())
    return "a controller is attached";
  if (nspire_usbpad_owns_usb())
    return "the pad driver's Direct mode has the port";
  if (nspire_usbms_active())
    return "a USB drive is in";
  if (audio_claimed())
    return "USB audio has the port";
  return 0;
}

/* The frame start's tick (nspire_link_frame_begin, after the link's watch):
 * the bookkeeping, then the stages for an unclaimed device at the host's
 * end. Only here is a power cycle made (SEAM-12). */
void nspire_arb_frame(void)
{
  u32 ms;

  nspire_arb_tick();
  nspire_usbpad_frame_point();   /* PAD4: the come-up's OS time in one go, its restart (the glue) */
  if (owner != ARB_WAIT || nspire_usbport_state() != PORT_HOST_END)
    return;
  /* Hub: no stage is the hub's (they are one device's try on the port's
   * lines: its restarts, "Starting the USB device...", the last resort's
   * "Unplug the USB device and plug it in again"). Said once in the logs. */
  if (nspire_usbhub_present())
  {
    if (veto() && stage != ARB_STAGE_WAIT_OFFER)
    {
      stage = ARB_STAGE_WAIT_OFFER;
      stage_at = nspire_ticks();
    }
    return;
  }
  ms = since_ms(stage_at);
  if (stage >= ARB_STAGE_WAIT_AGAIN && cycles && cycled && !(told & TOLD_START) && since_ms(change_at) >= ARB_CYCLE2_AT_MS)
  {
    told |= TOLD_START;   /* S7: the second try is under way (or the first, late) */
    if (!claimed)
      nspire_osd_message("Starting the USB device...");
  }
  if (stage == ARB_STAGE_WAIT_OFFER && ms >= ARB_STAGE0_MS)
  {
    /* No cycle can be made (none left, or can't here): on to the next stages
     * at once. One can: only once every guard says it harms nothing. */
    if (hooks[ARB_STAGE_CYCLE] || none_left() || restart_fits() == 0 || !veto())
      set_stage(ARB_STAGE_CYCLE);
  }
  if (stage == ARB_STAGE_CYCLE)
    set_stage(ARB_STAGE_WAIT_AGAIN);
  else if (stage == ARB_STAGE_WAIT_AGAIN)
  {
    int second = !hooks[ARB_STAGE_CYCLE] && cycles == 1 && cycled && !stopped && !refused;

    /* The second cycle at the port's age 6 s and ARB_CYCLE_GAP_MS after the
     * first, the guards asked again; then a wait for its offer. */
    /* PAD4: and only where a restart helps (restart_helps): never into the
     * OS's work on the device the first one brought back */
    if (second && !restart_helps())
      second = 0;
    if (second && since_ms(change_at) >= ARB_CYCLE2_AT_MS && since_ms(cycle_at) >= ARB_CYCLE_GAP_MS && !veto())
      cycle(0);
    else if (ms >= ARB_STAGE2_MS && !second && (!cycled || cycles < 2 || since_ms(cycle_at) >= ARB_CYCLE2_WAIT_MS))
      set_stage(ARB_STAGE_DIRECT);
  }
  if (stage == ARB_STAGE_DIRECT)
    set_stage(ARB_STAGE_GAVE_UP);
}

/* PAD3: the Controller page's "Restart USB power": a cycle by hand, as the
 * stages' (the same bookkeeping: its quiet window, the link's flap count
 * not fed), outside their two-a-session limit; refused while the port is
 * the link's, a controller's or another driver's. */
int nspire_arb_manual_cycle(void)
{
  int r;

  nspire_arb_tick();
  if (owner == ARB_LINK || owner == ARB_PAD || owner == ARB_OTHER || nspire_link_state() != LINK_OFF)
    return -3;
  /* R9: not under the other calculator (R9F's session 1: it was
   * on a stopped host controller, the link off, unidentified). */
  if (nspire_link_calc_on_cable())
  {
    nspire_link_log("seamless: no power restart by hand: a NavNet look lists a node (the other calculator)");
    nspire_link_log_request_flush();
    return -4;
  }
  r = nspire_usb_reenumerate(9);
  nspire_padlog_add(PL_CYCLE, 9, 1, 0, (u32)r, 0, 0);
  nspire_link_log("seamless: power restart by hand (the Controller page): %d", r);
  nspire_link_log_request_flush();
  if (r >= 0)
  {
    cycled = 1;
    cycle_at = nspire_ticks();
  }
  /* R10a (PAD3 x LINK2): only the link's own cycle may end its quiet window
   * early for the fast transport (link_cycle_done); a cycle by hand never. */
  link_cycled = 0;
  /* what comes now is a new try: the stages, the claim and the old offers start over */
  claimed = handled = 0;
  veto_said = 0;
  stage = ARB_STAGE_WAIT_OFFER;
  stage_at = change_at = nspire_ticks();
  return r;
}

/* (2026-11-18, Hub 5) The pad glue's recovery of a hub gone dead under the
 * OS (its power fell, or the OS's explore no longer runs): the one power
 * restart a hub in is allowed (the veto's "a USB hub is in" is what this
 * lifts: everything on the hub is dead already). The manual cycle's
 * bookkeeping; refused while the link is up or looking, or a NavNet look
 * lists the other calculator. -3 not now, -4 a calculator, else
 * nspire_usb_reenumerate's result. Blocks ~0.8 s. */
int nspire_arb_hub_cycle(void)
{
  int r;

  nspire_arb_tick();
  /* (2026-11-18) what the other restarts' veto says of the port
   * itself, not of what is on the hub (all of it is dead: the glue checked
   * that right before, the hub's own answer last): the link, its cable, a
   * restart just made, the pad driver's Direct mode, or a CX's power
   * still to put back */
  if (nspire_link_state() != LINK_OFF || nspire_link_cable_present() || nspire_usbpad_owns_usb() ||
      nspire_arb_cycling() || nspire_usb_power_restore_pending())
    return -3;
  if (nspire_link_calc_on_cable())
    return -4;
  r = nspire_usb_reenumerate(10);
  nspire_padlog_add(PL_CYCLE, 10, 3, 0, (u32)r, 0, 0);
  nspire_link_log("seamless: power restart for the dead hub (the pad glue's recovery): %d", r);
  nspire_link_log_request_flush();
  if (r >= 0)
  {
    cycled = 1;
    cycle_at = nspire_ticks();
  }
  link_cycled = 0;
  claimed = handled = 0;
  veto_said = 0;
  stage = ARB_STAGE_WAIT_OFFER;
  stage_at = change_at = nspire_ticks();
  return r;
}

/* PAD4: the come-up's restart (nspire_usbpad_glue.c): a device on the
 * wires the port never latched, CU_NOCONN_MS after the plug, nothing
 * offered. The stages' bookkeeping (the device's cycles, the quiet window,
 * the pad log's "arbiter:" line), at once instead of after stage 0 and the
 * link watch's looks: nothing connected can be a listed calculator; a
 * fresh NavNet look is asked all the same (one that lists a node: no). -3
 * not now, else nspire_usb_reenumerate's. Between frames or in the menus. */
int nspire_arb_comeup_cycle(void)
{
  int r;

  nspire_arb_tick();
  if (owner != ARB_WAIT || nspire_usbport_state() != PORT_HOST_END || nspire_link_state() != LINK_OFF ||
      nspire_link_cable_present() || nspire_arb_link_blocked() || nspire_usbport_os() != PORT_OS_NONE || handled ||
      nspire_arb_own_drop() || cycles || none_left() || !restart_helps() || nspire_usbhub_present())
    return -3;
  if (nspire_usbpad_offered())
  {
    handled = 1;
    return -3;
  }
  if (nspire_link_calc_on_cable())
    return -3;
  r = cycle(0);
  /* the stages go on from here as after their own first cycle (the second
   * one only where a restart still helps, then the last resort) */
  stage = ARB_STAGE_WAIT_AGAIN;
  stage_at = nspire_ticks();
  return r;
}

enum arb_owner nspire_arb_owner(void) { return owner; }
unsigned nspire_arb_stage(void) { return stage; }
uint32_t nspire_arb_stage_ms(void) { return since_ms(stage_at); }
unsigned nspire_arb_changes(void) { return changes; }
unsigned nspire_arb_cycles(void) { return cycles; }
int nspire_arb_cycling(void) { return cycled && since_ms(cycle_at) < ARB_CYCLE_QUIET_MS; }
/* PAD4: the port's changes now are our last cycle's own (its quiet window,
 * and nothing had the port since it): not a device's coming or going. */
int nspire_arb_own_drop(void) { return nspire_arb_cycling() && !claimed_since_cycle; }

const char *nspire_arb_owner_name(enum arb_owner o)
{
  static const char *const n[] = { "NONE", "WAIT", "PAD", "LINK", "OTHER" };
  return o <= ARB_OTHER ? n[o] : "?";
}

const char *nspire_arb_link_refusal(void)
{
  if (nspire_usbpad_owns_usb())
    return "The USB port is the gamepad's (USB driver: Direct)";
  /* A pad's in the port (the cable can't be): NavNet would be opened on
   * its adapter, and the pad's driver refuses new pads while a link is up. */
  if (nspire_usbpad_active())
    return "The USB port has a controller in it";
  if (nspire_usbms_active())
    return "The USB port has a drive in it";
  if (nspire_bt_usb_active())
    return "The USB port has the Bluetooth dongle";
  if (audio_claimed())
    return "The USB port is in use for sound";
  return 0;
}

int nspire_arb_link_blocked(void)
{
  return nspire_usbpad_owns_usb() || nspire_usbpad_active() || nspire_usbms_active() || audio_claimed();
}

int nspire_arb_set_hook(unsigned s, nspire_arb_hook_fn fn)
{
  if (s != ARB_STAGE_CYCLE && s != ARB_STAGE_DIRECT)
    return -1;
  hooks[s] = fn;
  return 0;
}

void nspire_arb_reset(void)
{
  owner = ARB_NONE;
  stage = changes = 0;
  session_reset();
  in_session = empty_seen = 0;
  port_counter = 0;
  port_was = PORT_EMPTY;
  wires_counter = nspire_padlog_wires_counter();
  wires_gone = nspire_padlog_wires_gone();
  change_at = stage_at = nspire_ticks();
  hooks[1] = hooks[3] = 0;
}
