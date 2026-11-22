/* Bluetooth in gpSP (nspire_bt_glue.c), on nspire-bt, a sibling repository
 * (../nspire-bt: its include/nspire_bt.h and bt_port.h, and its calculator
 * library build/nspire/libnspirebt.a, which the Makefile builds and links):
 * Bluetooth controllers and headphones through a USB Bluetooth dongle on
 * the calculator's USB port (directly or behind a hub). Without it
 * (NSPIRE_NO_BT) the calls below do nothing and the Bluetooth page says the
 * build has none.
 *
 * Where it runs:
 *  - nspire_bt_glue_start from init_input (the setting on), with the other
 *    USB drivers, before the first come-up of the port;
 *  - nspire_bt_glue_poll from keys_poll: a game frame's keys and every menu
 *    pass (BTstack's run loop, the pads' state, the events);
 *  - nspire_bt_os_time from the pacing code (nspire_frameskip.c, beside
 *    nspire_audio_os_time, before the frame's wait): the CPU's interrupts
 *    on for a moment when the dongle has transfers that complete in them
 *    (an interrupt window, as the pad glue's), and the OS's task time only
 *    while the dongle comes or goes, out of the waits (riding the sound's
 *    sleeps while a USB sound adapter streams). The one function another
 *    pacing scheme moves;
 *  - the key store's file written only from a menu pass (never mid-game);
 *  - nspire_bt_glue_stop_within from quit() inside nspire_exit_usb_left_ms,
 *    nspire_bt_glue_write_log after the USB shutdowns (gpsp_bt.txt.tns). */
#ifndef NSPIRE_BT_GLUE_H
#define NSPIRE_BT_GLUE_H

#include <stdint.h>

/* The settings (gpsp.cfg.tns word 22, nspire_config.c). */
extern unsigned nspire_bt_enabled;      /* 0 off, 1 on (default) */
extern unsigned nspire_audio_output;    /* NSPIRE_OUTPUT_*: where the game's sound goes */
extern unsigned nspire_bt_codec;        /* NSPIRE_BT_CODEC_* */
enum { NSPIRE_OUTPUT_AUTO, NSPIRE_OUTPUT_USB, NSPIRE_OUTPUT_BT, NSPIRE_OUTPUTS };
/* (bt10: High, joint stereo bitpool 53: the codec loses 6-12 dB less than at 35 on the games' sound, 69 media
 * packets a second instead of 43: nspire-bt tests/profiles/quality) */
enum { NSPIRE_BT_CODEC_AUTO, NSPIRE_BT_CODEC_MONO, NSPIRE_BT_CODEC_STEREO, NSPIRE_BT_CODEC_HIGH, NSPIRE_BT_CODECS };

/* 1 if this build has Bluetooth. */
int nspire_bt_built(void);

/* Once, from init_input: starts Bluetooth if the setting is on. */
void nspire_bt_glue_start(void);
/* The setting changed (the Bluetooth page): on starts it (or turns the
 * radio on again), off turns the radio off (the dongle stays the driver's). */
void nspire_bt_glue_apply(void);
/* The headphones' codec setting changed (the Sound page): only that (a
 * Bluetooth that has failed stays as it is). */
void nspire_bt_glue_codec(void);
/* From init_input after nspire_usbpad_start(): the exit's handler for an
 * exit() that didn't come through quit(), run before the pad driver's. */
void nspire_bt_glue_atexit(void);
/* The key store's file written now if it changed: quit() calls it before
 * its USB shutdowns (the stop writes no file). */
void nspire_bt_glue_flush(void);

/* Nothing to do (no dongle, nothing pending): what every per-frame call
 * site tests first, two words read and one compare, no call. nspire_bt_wake
 * is the port's idle word (bt_port_idle_word: set by the dongle's attach in
 * the OS's USB task), a word of zeros while Bluetooth isn't started;
 * nspire_bt_live is the glue's own (pads, headphones, toasts, the OS's
 * time wanted). */
extern const volatile uint32_t *nspire_bt_wake;
extern unsigned nspire_bt_live;
#define NSPIRE_BT_IDLE() (!(*nspire_bt_wake | nspire_bt_live))

/* From keys_poll unless NSPIRE_BT_IDLE(). in_game: a game frame's keys
 * (nspire_input_frame); else a menu pass, where the key store is written
 * when it changed and the interrupt window / OS time are given here (no
 * frame loop places them). Returns the Bluetooth pads' buttons (usbpad.h's
 * bits, both slots ORed, each pad's face buttons in its layout as the USB
 * pads': the pad settings file). */
uint32_t nspire_bt_glue_poll(int in_game);
/* Each game frame unless NSPIRE_BT_IDLE(), with its hotkeys: the toasts (connected, disconnected,
 * headphones, dongle) over the game, one a second at most; gpSP's own
 * message after a hotkey keeps its 2 s first. */
void nspire_bt_glue_frame(uint32_t hotkeys);

/* The OS's time for the dongle, at a frame's end before its wait (see the
 * top): due the next frame's time (timer ticks), began when the frame just
 * done started (0 not known), paced 0 in fast forward (frames back to back).
 * 1 if it slept in the OS. Called only while nspire_bt_live. */
int nspire_bt_os_time(uint32_t due, uint32_t began, int paced);

/* The game's sound to Bluetooth headphones (nspire_audio.c): each frame
 * before the game runs, mode as nspire_audio_frame_begin's (0 pause, 1
 * normal, 2 fast forward) and whether a USB sound adapter streams: 1 if the
 * headphones take the sound this frame (they stream and the Output setting
 * lets them have it), then nspire_bt_glue_audio_write after the frame.
 * Called only while nspire_bt_live (else: 0). */
int nspire_bt_glue_audio_frame(int mode, int usb_streaming);
void nspire_bt_glue_audio_write(const int16_t *lr, unsigned frames);
/* bt10: the mixer's frames, read in place (nspire_bt.h nspire_bt_audio_write_mix: words L | R << 16 of
 * 12-bit samples in two spans; NSPIRE_BT_MIX_CLEAR (1) clears them, NSPIRE_BT_MIX_MONO (2) mixes L and R) */
void nspire_bt_glue_audio_write_mix(uint32_t *w1, unsigned n1, uint32_t *w2, unsigned n2, unsigned flags);
/* The headphones' delay to the ear, ms (their report, else an estimate), 0 while none stream: the USB
 * adapter's sound is held back by it, less its own, while both play (nspire_audio.c). */
unsigned nspire_bt_glue_audio_delay_ms(void);
/* Headphones connected and streaming (whatever the Output setting). */
int nspire_bt_glue_headphones(void);
/* For the Sound page's status: "Bluetooth: WH-1000XM4, 44.1 kHz stereo" or NULL. */
const char *nspire_bt_glue_audio_summary(void);
/* The streaming headphones' name ("headphones" when they gave none), NULL while none stream. */
const char *nspire_bt_glue_headphones_name(void);

/* The Bluetooth pads for the Controls screen and the Controller page: the
 * first one's name ("DualSense (Bluetooth)") and kind (usbpad.h's
 * USBPAD_KIND_*), NULL / 0 while none is in; how many are in. */
const char *nspire_bt_glue_pad_name(void);
int nspire_bt_glue_pad_kind(void);
int nspire_bt_glue_pad_count(void);
/* Bluetooth slot 0 or 1: its pad's kind's name ("DualSense", "Switch Pro",
 * "Bluetooth gamepad"), NULL while the slot is empty (the Controller page). */
const char *nspire_bt_glue_slot_name(int slot);
/* ...and its number ("Controller N": after the highest USB slot in use), 0 while empty. */
int nspire_bt_glue_slot_number(int slot);
/* One line for the Controller page ("Bluetooth: P2 DualSense", "Bluetooth:
 * plug the USB dongle in"...), NULL while Bluetooth is off. */
const char *nspire_bt_glue_status(void);
/* The dongle is attached and the driver's (the pad glue never power-
 * restarts the port under it; the arbiter counts it as a working device). */
int nspire_bt_usb_active(void);
/* (2026-11-18) HCI events and ACL packets the dongle brought in so far (a hub's sign of life). */
uint32_t nspire_bt_usb_traffic(void);

/* The pad log's Bluetooth lines (nspire_padlog.c, at each write): text,
 * '\n' between lines; the length. */
int nspire_bt_glue_log_text(char *out, unsigned size);

/* The exit (quit, nspire_host_main.c): the stop within ms (0 done, also with
 * USB memory left to the OS; -1 a USB callback still inside gpSP: the caller
 * restarts the calculator), then the Bluetooth log (gpsp_bt.txt.tns). */
int nspire_bt_glue_stop_within(unsigned ms);
void nspire_bt_glue_write_log(void);

/* The last thing that happened (a connect, a failure: its toast's words),
 * with a number that changes with each: the Bluetooth page's band shows it
 * (nspire_bt_page.c, the ops behind ui7's page, nspire_bluetooth.h). */
const char *nspire_bt_glue_note(unsigned *seq);
/* ...and that event itself (its toast's makings), 0 while there is none. */
int nspire_bt_glue_note_event(int *type, int *player, int *kind, int *err, char *name, unsigned size);
/* The words of an event's toast (NSPIRE_BT_EV_*; player 1.. a controller's
 * number; name the device's): OSD_TEXT_MAX (63) characters at the most,
 * whatever the name. */
int nspire_bt_glue_toast_text(int type, int player, int kind, int err, const char *name, char *out, unsigned size);

#endif
