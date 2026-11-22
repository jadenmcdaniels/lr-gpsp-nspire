/* USB gamepads in gpSP (nspire_usbpad_glue.c), on nspire-usbpad, a sibling
 * repository (../nspire-usbpad: its usbpad.h, and its calculator library
 * build/nspire/libusbpad.a, which the Makefile builds and links). Without
 * it the build has no pads: the glue's calls do nothing (NSPIRE_NO_USBPAD).
 *
 * The keys layer (nspire_input.c) is the only caller. A pad's buttons are
 * keys in a fifth key word (nspire_keys.h KEY_PAD_*), its D-pad and left
 * stick press the arrows; both pads drive player 1 (the GBA has one). */
#ifndef NSPIRE_USBPAD_GLUE_H
#define NSPIRE_USBPAD_GLUE_H

#include <stdint.h>

/* Once, from init_input(): reads the pad settings file, hands nspire-usbpad
 * gpSP's OS sleep and USB port check, starts its driver, and registers
 * usbpad_shutdown() with atexit(), since quit() (nspire_host_main.c) ends
 * in exit(0) and isn't this branch's to change. */
void nspire_usbpad_start(void);

/* The pads' buttons, from keys_poll(): bit n is key KEY_PAD_FIRST + n
 * (usbpad.h's bits, both pads ORed, each pad's face buttons in the layout
 * the settings file gives it), bits 0-3 the D-pad (up, down, left,
 * right). Polls the driver at most 80 times a second, as it times its
 * steps in polls (about 60 a second; without a timer, by the RTC's
 * seconds), and lets the OS run when it asks to (a pad attaching, and now
 * and then while one is in). Cheap: under a microsecond with no pad. */
uint32_t nspire_usbpad_keys(void);
/* gpSP posted a message of its own now, not at a key (a state written over
 * frames: nspire_host_main.c save_job_frame): the toasts wait its time. */
void nspire_usbpad_message_posted(void);

/* Each game frame (nspire_input_frame), with the hotkeys it found: what the
 * driver has to say (connected, disconnected, plug in again, needs power,
 * not supported) as a message over the game, one a second at most, none
 * that a newer state makes wrong. Menus don't show those messages, so they
 * wait there for the game; gpSP's own messages (after a hotkey, or as the
 * game starts or resumes) keep their 2 s first. */
void nspire_usbpad_frame(uint32_t hotkeys);

/* The name of the controller that is in ("Xbox 360", "DualSense"; a
 * Bluetooth one's "DualSense (Bluetooth)" when no USB one is), or NULL while
 * none is (the Controls screen's controller view). */
const char *nspire_usbpad_name(void);

/* R10b: the family of the controller that is in, for its buttons' own names
 * on the Controls screen (nspire_bind.h bind_pad_family): PAD_FAMILY_NONE
 * for none, or a generic USB gamepad (its buttons by position). */
enum { PAD_FAMILY_NONE, PAD_FAMILY_XBOX360, PAD_FAMILY_XBOX, PAD_FAMILY_DS4, PAD_FAMILY_DUALSENSE, PAD_FAMILY_SWITCH,
       PAD_FAMILIES };
int nspire_usbpad_family(void);
/* 1 if that controller's face buttons are by their labels (layout=labels in
 * the pad settings file: bottom and right swapped, left and top), 0 by position. */
int nspire_usbpad_labels(void);
/* (Bluetooth, nspire_bt_glue.c) the USB controllers in now: the Bluetooth
 * ones are numbered after them; and the pad settings file's layout for a
 * controller by its USB ids (a Bluetooth pad's are its kind's): 1 by its
 * labels. 0 without the pad driver. */
int nspire_usbpad_pads(void);
unsigned nspire_usbpad_slots(void);
const char *nspire_usbpad_slot_name(int slot);
int nspire_usbpad_layout_labels(uint16_t vid, uint16_t pid);

/* From get_gui_input (the menus, the file list): writes the log's pad lines
 * to the file if there are new ones, at most once in 2 s (a reset in the
 * OS's USB task there would otherwise leave them in memory). Not in a game
 * (its frames' starts write the log). */
void nspire_usbpad_menu_flush(void);

/* R11w (2026-11-15): the exit. nspire_usbpad_exit_log: the pad lines and
 * the pad log (its milestones, the driver's block) written, from quit()
 * before any USB shutdown. nspire_usbpad_stop_within: the driver's shutdown
 * within ms (its waits bounded by it, and a callback still inside gpSP
 * waited for no longer): 0 done, -1 not (the caller restarts the
 * calculator: nspire_exit_usb_timed_out). Writes no file. Both once; the
 * atexit handler does both for an exit() that didn't come through quit(). */
void nspire_usbpad_exit_log(void);
int nspire_usbpad_stop_within(unsigned ms);
/* (r19-usb-exit) After nspire_usbpad_exit_log: the port quiet as the pad
 * driver's shutdown takes it (nothing on it, gated, or the cable's plain
 * end, the same for a second) and no device ever offered to the OS's USB
 * drivers in this run: 1, the sound's and the drive's shutdowns need not
 * wait for a callback of theirs (none ever ran, none can start). */
int nspire_usbpad_exit_untouched(void);

/* Hub (2026-11-16): a USB hub is on the adapter (the OS offered one, or a
 * second device came while one was attached), until the port's device
 * leaves its wires: no power restart of the port then (it cuts everything
 * on the hub), and what is plugged into the hub comes and goes by the OS's
 * offers and detaches, never by the port's lines. busy: the OS's USB task is
 * at work on one of its devices now (an offer, an attach or a detach within
 * the last 0.3 s, or a driver's callback inside the OS). 0 without the pad
 * driver (weak defaults: nspire_arbiter.c). */
int nspire_usbhub_present(void);
int nspire_usbhub_busy(void);
/* (2026-11-18, Hub 5) the port's power was restarted under a dead hub and
 * its devices are not back yet: a drive on it is coming back. */
int nspire_usbhub_recovering(void);
/* (2026-11-18) the hub's devices may be dead now: a drive on it is left alone. */
int nspire_usbhub_doubtful(void);
/* (2026-11-18) A hub recovery is in the air (for Bluetooth's toasts): 1 from a
 * fall of the power under the hub or a dead hub's restart due or under way,
 * until 5 s after the hub's devices are offered again, the recovery gives
 * up or the hub is pulled; 0 otherwise, and where none of it runs. */
int nspire_hub_recovering(void);
/* The OS's USB has stopped (nspire_usbpad_glue.c watch_stuck): it enables
 * the port for each device plugged in and never offers one again, until
 * the OTG adapter comes out. quit() restarts the calculator then (it froze
 * after a clean exit in that state). 0 without the pad driver. */
/* (2026-11-17) the OS's USB kept awake for this run: 7 its code is changed
 * (nspire-usbshim usbshim_awake.h), -1 not the OS build whose code is known,
 * 0 not asked yet (no adapter was in). */
int nspire_usbpad_awake(void);
int nspire_usbpad_os_stuck(void);
/* ...and nothing has been offered since it was found so (the adapter out
 * ends os_stuck, so that the next plug is judged anew; only an offer ends
 * this): quit's restart. */
int nspire_usbpad_os_dead(void);
/* The OS offered the device on the port and it is neither a controller nor
 * a calculator (every offer of it answered 0, its try ended: R11u's "not a
 * controller"): the link has nothing to watch for on it. */
int nspire_usbpad_other_device(void);

/* The heartbeat's pad line (nspire_link.c, every 10 s of the log's life):
 * the pad's reports, errors and drops, while a pad is in. 1 = text written. */
int nspire_usbpad_heartbeat(char *line, unsigned size);

/* The OS's tick for a pad, while the OS is wanted (nspire_usbpad_keys; a
 * pad's reports come only while the OS runs). Timer ticks (NSPIRE_TICK_HZ,
 * 32768 a second) since the last sleep -> what to do:
 * USBPAD_TICK_NONE, USBPAD_TICK_NEAR (nspire_os_sleep_near with
 * nspire_os_near_wait(since, USBPAD_TICK_FROM, USBPAD_TICK_FORCE)) or
 * USBPAD_TICK_SLEEP (nspire_os_sleep, forced). Attaching: forced every
 * USBPAD_TICK_FORCE. Otherwise near tries from USBPAD_TICK_FROM, forced
 * at USBPAD_TICK_FORCE: the longest gap between a pad's reports stays the
 * 33 ms it was before P2 (2026-11-11 R1: P2 forced at 66 ms, and on a CX
 * the near tries began at 33 ms, past the 8 ticks (24 ms) the OS tick's
 * phase is known for, so they could never fire). 1/60 s is inside that
 * on both (CX 2.98 ms tick: 24 ms; CX II 10.25 ms: 82 ms). tests/link_usb
 * models it ("pad's tick"). */
#define USBPAD_TICK_FROM  (32768u / 60)
#define USBPAD_TICK_FORCE (32768u / 30)
enum { USBPAD_TICK_NONE, USBPAD_TICK_NEAR, USBPAD_TICK_SLEEP };
static inline int nspire_usbpad_tick_rule(uint32_t since, int attach)
{
  if (since >= USBPAD_TICK_FORCE)
    return USBPAD_TICK_SLEEP;
  if (attach || since < USBPAD_TICK_FROM)
    return USBPAD_TICK_NONE;
  return USBPAD_TICK_NEAR;
}

#endif
