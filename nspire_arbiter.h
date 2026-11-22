/* Who has the USB port (S3): one state machine over the port observer
 * (nspire_usbport.h) that the link and the gamepad glue ask, in place of
 * their mutual refusals. The rules, in one place:
 *
 *   a link that is up (or the other gpSP's node seen)   -> ARB_LINK
 *   else the OS offered a pad class (pad claim)           -> ARB_PAD (the
 *        link stops watching, as nspire_usbpad_active did)
 *   else the pad's own USB mode, a drive or audio         -> ARB_OTHER
 *   else something on the port nobody claims              -> ARB_WAIT
 *   else (port EMPTY, or can't tell)                      -> ARB_NONE
 *
 * A pad -> calculator or calculator -> pad swap needs no menu: the claim of
 * the one that left is gone (its detach, the link's end) before the other is
 * classified, and a changed port restarts the stage. In ARB_WAIT on the
 * host's end the stage runs (nspire_arb_frame only): 0 wait for the OS's
 * offer (ARB_STAGE0_MS, and until the link's watch has looked long enough
 * to have seen a calculator: nspire_link_cycle_veto), 1 power cycle
 * (nspire_usb_reenumerate; a hook replaces it), 2 wait again (ARB_STAGE2_MS),
 * 3 direct read (hook, empty until S6), 4 gave up. No file is written here
 * (log lines ask for a flush at the next frame start). A tick is a few
 * compares. */
#ifndef NSPIRE_ARBITER_H
#define NSPIRE_ARBITER_H

enum arb_owner { ARB_NONE, ARB_WAIT, ARB_PAD, ARB_LINK, ARB_OTHER };

#define ARB_STAGE0_MS 1500
#define ARB_STAGE2_MS 4000
/* S4: power cycles per port change, the 2nd at the port's age 6 s, a wait of
 * 2.5 s after it for the offer; the port's own changes in the 3 s after a
 * cycle are its doing. */
#define ARB_CYCLES_MAX      2
#define ARB_CYCLE2_AT_MS    6000
#define ARB_CYCLE2_WAIT_MS  2500
#define ARB_CYCLE_QUIET_MS  3000
/* SEAM-2: the second cycle at least this long after the first's end. */
#define ARB_CYCLE_GAP_MS    4500
/* SEAM-6: a session (its cycles and toasts) ends when the port reads EMPTY
 * this long (the adapter's or the cable's ID pin: a real unplug; the host's
 * end never reads EMPTY in a cycle; a CX's gate reads UNKNOWN). */
#define ARB_SESSION_END_MS  700
/* R1: a new device onto an adapter that stays in gets its own cycles, up to
 * this many in all a session (no loop if a cycle's own drop is ever taken
 * for an unplug: a slow pad back after the quiet window). */
#define ARB_SESSION_CYCLES_MAX 8
/* The last resort's words (R11w part 2: plain, at most 50 characters; it
 * never says to swap the cable: that is the link's, while linking). */
#define ARB_REPLUG_TEXT "Unplug the USB device and plug it in again"
enum { ARB_STAGE_WAIT_OFFER, ARB_STAGE_CYCLE, ARB_STAGE_WAIT_AGAIN, ARB_STAGE_DIRECT, ARB_STAGE_GAVE_UP };

/* What the pad driver says (its library's, or the weak default: never). */
int nspire_usbpad_active(void);
int nspire_usbpad_owns_usb(void);
int nspire_usbms_active(void);
/* Audio: an attached endpoint, or transfers still draining after detach.
 * Either prevents link takeover and automatic/manual bus power cycles. */
int nspire_usbaudio_active(void);
int nspire_usbaudio_owns_usb(void);
/* The pad glue: the OS offered a device to a driver since the port was last
 * empty (asked only right before a power cycle). */
int nspire_usbpad_offered(void);
/* Hub (2026-11-16, the pad glue): a USB hub is on the adapter: no power
 * restart of the port by anyone, no stage (nspire_usbpad_glue.h). */
int nspire_usbhub_present(void);

/* The bookkeeping (owner, session, the port's changes): wherever the keys
 * are read (the pad glue). Never a stage, never the port touched. */
void nspire_arb_tick(void);
/* The frame start's (nspire_link_frame_begin, after the link's watch): the
 * tick, then the stages; the only place a power cycle is made, and only once
 * nspire_link_cycle_veto and the OS's offers say it harms nothing. */
void nspire_arb_frame(void);
enum arb_owner nspire_arb_owner(void);
unsigned nspire_arb_stage(void);
/* PAD3: ms since the stage began (the Controller toast waits while stage 0 may still cycle). */
uint32_t nspire_arb_stage_ms(void);
unsigned nspire_arb_cycles(void);
/* 1 in the ARB_CYCLE_QUIET_MS after a power cycle: the port's changes then are its own. */
int nspire_arb_cycling(void);
/* PAD4: as nspire_arb_cycling, but not once a pad (link, drive) had the port
 * after the cycle: then a device leaving or coming is a real one. */
int nspire_arb_own_drop(void);
/* 1 after the last resort said to swap the cable's ends (until the session
 * starts over). R11w part 2: always 0 (the arbiter never says to swap: the
 * link owns the port while linking; kept for the page and the tests). */
int nspire_arb_swap_hint(void);
const char *nspire_arb_owner_name(enum arb_owner o);

/* PAD3: a bus power restart asked for on the Controller page: -3 not now
 * (the link, a controller or a drive has the port, or the link is up or
 * looking), -4 a NavNet look lists the other calculator (R9), else
 * nspire_usb_reenumerate's result. Blocks ~0.8 s. */
int nspire_arb_manual_cycle(void);
/* PAD4: the pad come-up's power restart at once (nspire_usbpad_glue.c): a
 * device on the wires the port never latched as connected. -3 not now, else
 * nspire_usb_reenumerate's result. Blocks ~0.8 s. */
int nspire_arb_comeup_cycle(void);
/* (2026-11-18) The pad glue's recovery of a dead hub: the port's power
 * restarted although a hub is in (everything on it is dead already). -3 not
 * now (the link), -4 a NavNet look lists the other calculator, else
 * nspire_usb_reenumerate's result. Blocks ~0.8 s. */
int nspire_arb_hub_cycle(void);
/* (2026-11-17) the glue woke the CX II's transceiver under a device that
 * waited on it: a power restart of that device is in order for a while */
void nspire_arb_phy_woken(void);

/* Why the link may not start now: 0 if it may, else the text the Link line
 * shows (the pad driver's USB mode, or a controller in the port). */
const char *nspire_arb_link_refusal(void);
/* 1 while the link must not watch the cable (a pad, a drive or the pad
 * driver's own USB mode has the port). */
int nspire_arb_link_blocked(void);

/* Why the link's fast transport (nspire_link_fast.c: gpSP driving the USB
 * controller itself) may not hold the controller now, or 0 if it may: only
 * while the owner is LINK, no bus power cycle is under way or just made
 * (ARB_CYCLE_QUIET_MS, or a CX's PP still to put back), and no pad, drive
 * or pad Direct mode has a claim. Asked before the take-over; each tick
 * makes a fast transport that is up let go once it says no. */
const char *nspire_arb_fast_refusal(void);

/* LINK2: Link picked (the owner LINK) at a host's end whose controller never
 * ran with a device connected (r5, r6: the CX II's OS never started it; a
 * new connect did): one bus power cycle as the stages' (S4), counted in the
 * session's ARB_CYCLES_MAX. From the frame start only (SEAM-12). 1 a cycle
 * was made and VBUS came back; 0 it came back without VBUS; -1 nothing to
 * cycle (nothing connected at the host's end any more: no latch,
 * F4); -2 can't here (the port's registers, the fast link, no timer); -3
 * none left this session. Its quiet window (nspire_arb_cycling) stops
 * refusing the fast transport once the port is enabled with the controller
 * running (F1). */
int nspire_arb_link_cycle(void);

/* A stage's hook (1 or 3), called once when the stage is entered; 0 ok. */
typedef void (*nspire_arb_hook_fn)(void);
int nspire_arb_set_hook(unsigned stage, nspire_arb_hook_fn fn);

/* Owner changes so far (the tests count them), and a reset for them. */
unsigned nspire_arb_changes(void);
void nspire_arb_reset(void);

#endif
