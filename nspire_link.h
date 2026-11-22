/* GBA link: the link cable or the wireless adapter between two gpSPs.
 *
 * gpSP's core already plays the other GBA's part (libretro's netplay code:
 * serial_proto.c for the Pokemon and Advance Wars cables, rfu.c for the
 * wireless adapter). It sends packets with netpacket_send; nspire_link.c
 * carries them to the other gpSP and hands it the ones that come back.
 * Under that, the cable (link_cable_*) is the calculators' USB
 * (nspire_link_usb.c) or, on the PC, a socket to another gpsp-host
 * (tests/host/host_link.c). */
#ifndef NSPIRE_LINK_H
#define NSPIRE_LINK_H

#include <stdint.h>

enum link_state
{
  LINK_OFF,         /* not linked */
  LINK_LOOKING,     /* looking for the other gpSP (the game goes on) */
  LINK_LINKED       /* the two games run frame for frame */
};

/* Starts looking for the other gpSP (the game goes on meanwhile); linking
 * happens at a frame's start once it's there. */
void nspire_link_connect(void);
/* Unplugs: the other side hears it, and both games go on alone. */
void nspire_link_disconnect(void);
/* gpSP is closing (quit): unplugs, lets the transport go, writes the log. */
void nspire_link_shutdown(void);
/* R11w (2026-11-15): the same within ms (the exit's USB budget): the waits
 * for the service's task are cut there, and if a callback is still in a read
 * or write then, StopService (which waits for it without a deadline) is not
 * called: -1 (the exit restarts the calculator); else 0. */
int nspire_link_shutdown_within(unsigned ms);
enum link_state nspire_link_state(void);
/* Another calculator (or a computer) is on the cable, or a link is up: the
 * USB port is the link's (a USB gamepad driver keeps off it then). */
int nspire_link_cable_present(void);
/* R3: 1 when a file write harms no link (off, the other gpSP's cable not seen,
 * no link ended within a beat): the pad log's writes. */
int nspire_link_quiet_for_writes(void);
/* R9: the device on the cable may be the other calculator (the link has it, or
 * a fresh NavNet look lists a node): no power restart by hand under it. */
int nspire_link_calc_on_cable(void);
/* The cable's ends are likely the wrong way round (the port keeps resetting,
 * or the arbiter gave up on a calculator on the cable): the card says so. */
int nspire_link_swap_hint(void);
/* SEAM-2/3: why a bus power cycle at the host's end could harm a link now
 * (0: it can't). Asked by the arbiter at the frame start, after the watch:
 * not while a link is up or the cable's seen, not unless the watch ran this
 * frame and made at least 2 looks since change_at (the port's last change,
 * a tick), the last one CYCLE_LOOK_MS after it, and (look 1) a fresh look
 * taken here, last, lists no node. */
const char *nspire_link_cycle_veto(uint32_t change_at, int look);
/* What the menu and messages say about the link, e.g. "Linked: player 1". */
const char *nspire_link_status(void);
/* The protocol the two speak while linked (nspire_link.c's hello). */
int nspire_link_protocol(void);
/* The main menu's Link line's help: the status, and what picking it does. */
extern char nspire_link_help[];
/* Whether gpSP links this game, and how (serial_mode's name), or NULL. */
const char *nspire_link_device_name(void);

/* Around each game frame (nspire_host_main.c run_frame). */
void nspire_link_frame_begin(void);
void nspire_link_frame_end(void);
/* The menu opened (1) or closed (0): the other side waits meanwhile and
 * says why instead of giving up on this one. */
void nspire_link_menu(int open);

/* Link setting (Settings, Emulator page): which device is plugged in. The
 * numbers are what the settings file keeps (nspire_config.c word 68). */
enum
{
  LINK_DEVICE_AUTO,       /* what gpSP's game list says (gba_over.h) */
  LINK_DEVICE_WIRELESS,   /* wireless adapter */
  LINK_DEVICE_CABLE,      /* link cable: the game's stand-in if it has one */
  LINK_DEVICE_EXACT,      /* link cable, wire for wire for any game */
  LINK_DEVICES
};
extern uint32_t nspire_link_device;
/* Link speed (Settings, Emulator page; nspire_link_speed.c): Auto tries the
 * fast transport (nspire_link_fast.c) when both sides can, Compatible
 * links over NavNet only. Kept in the settings file's word 68, bit 8
 * (older builds read that word % LINK_DEVICES: the same device). */
enum { LINK_SPEED_AUTO, LINK_SPEED_COMPATIBLE, LINK_SPEEDS };
extern uint32_t nspire_link_speed;
/* The menu changed it: Auto again turns the fast transport on after a crash. */
void nspire_link_speed_changed(void);
/* Reads the fast transport's marker once a run (the first log write):
 * the last run died with it in use, then it's off (a log line, a toast). */
void nspire_link_speed_check(void);
/* 1 while it's off after a crash (the menu shows it). */
int nspire_link_fast_crashed(void);
/* Why the fast transport isn't tried now, or NULL (it may be). */
const char *nspire_link_fast_off_why(void);
int nspire_link_fast_allowed(void);
/* The marker: in use from just before a bring-up to the link's end. */
/* 0 if the marker couldn't be written (or it's off): not tried then. */
int nspire_link_fast_begin(void);
void nspire_link_fast_end(void);
/* The PC tests': the marker read again at the next check (a new run). */
void nspire_link_speed_reread(void);
/* Cable transfers since starting, started (player 1) or answered (player
 * 2): the PC tests' report counts them. And a sum over their start cycles
 * and words, the same on both GBAs and at any link speed when they swap
 * the same words at the same cycles (the report shows it). */
extern uint32_t nspire_link_transfers;
extern uint32_t nspire_link_transfer_sum;
/* The PC tests' (gpsp-host --stand-in CODE:MODE): plays a game's cable
 * with a stand-in that isn't in nspire_link.c's table; < 0 if it's wrong. */
int nspire_link_try_stand_in(const char *code_and_mode);
/* The serial_mode for the game just loaded under the setting. */
void nspire_link_apply_device(void);
/* The setting changed (menu): unlinks, and plugs in the new device. */
void nspire_link_device_changed(void);

/* The link's log (gpsp_link.txt.tns in the saves folder): on the
 * calculators nothing else shows what the cable did. A line, with the ms
 * since Link was picked; the transport adds its calls' results. Never from
 * the OS's task (nspire_link_usb.c's service). */
void nspire_link_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int nspire_link_log_started(void);
/* Writes the log if it has something new: leaving the game, closing gpSP
 * (nspire_link.c also does at a link's start and end, between frames). */
void nspire_link_log_flush(void);
/* R11t: flushes held because a sound adapter played (none is written then). */
unsigned nspire_link_log_audio_held(void);
/* Asks for the log to be written at the next frame's start (between
 * frames, where the write costs nothing the link waits on): the pad glue's
 * events. Never writes here. */
void nspire_link_log_request_flush(void);
/* Every on-screen toast (nspire_osd_message) into the log. */
void nspire_link_log_toast(const char *text);
/* "this calculator is USB host (adapter end)" or "...device (cable end)",
 * from nspire_usb_bits. */
const char *nspire_usb_role_text(uint32_t bits);
/* The gamepad glue's os_time setting, for the log's header. */
void nspire_link_log_pad_setting(int os_time);
/* The PC's gpsp-host, quitting while linked without unlinking: the link's
 * figures so far into the log, and the log written. */
void nspire_link_log_close(void);
/* EXIT-3 (nspire_exit.c): the exit's lines ('\n' between them) into the log,
 * if this run has one, which is written then; and the clean-exit marker
 * (gpsp_link_run.txt.tns) with them, always: one write each, last. */
void nspire_link_exit_close(const char *lines);
/* EXIT-R4: one line written after the hand-back (gpsp_exit_after.txt.tns),
 * with the marker's clock: the next run's log pairs the two. */
void nspire_link_exit_after(const char *line);

/* The frontend's (nspire_host_main.c): the screen while the other side is
 * waited for (ms so far; whether it's in its menu), and esc to unlink
 * (pressed since the wait's first slice, waited_ms WAIT_SLICE_MS). */
void nspire_link_waiting(unsigned waited_ms, int other_in_menu);
int nspire_link_wait_cancelled(unsigned waited_ms);

/* ---- The cable: nspire_link_usb.c, or host_link.c on the PC ---- */

/* The most frames of delay the two may agree on (nspire_link.c's
 * delay_phase), and the longest message: the transport's buffers hold
 * that many frames' messages in flight (nspire_link_usb.c's RING). */
#define LINK_DELAY_MAX 15
/* A wait for the other side this long shows "Waiting for the other GBA"
 * (nspire_link_waiting); a shorter one shows nothing. */
#define LINK_WAIT_SHOW_MS 2000
#define LINK_MSG_MAX   2048

/* Starts looking for the other side; < 0 if this calculator can't. On its
 * own (automatic: the watch found the other, nobody picked Link): only
 * both ways up counts as ready, never one channel (where the game itself
 * would write, and wait), and a side that never connects back is given up
 * on (link_cable_ready < 0). */
int link_cable_open(int automatic);
/* 1 once the two are connected, 0 not yet, < 0 if it failed. */
int link_cable_ready(void);
/* Sends one message whole; < 0 if the cable broke. */
int link_cable_send(const void *msg, unsigned len);
/* One message into buf, waiting up to wait_ms for it: its length, 0 if
 * nothing came, < 0 if the cable broke. */
int link_cable_receive(void *buf, unsigned size, unsigned wait_ms);
void link_cable_close(void);
/* No link up: watches for the other calculator's gpSP (the service up, a
 * look now and then, the OS let run). 2: it's there (its connection came,
 * or this side's reached it; held for link_cable_open), 1: a calculator or
 * computer is on the cable, 0: nothing, < 0: this one can't link. Connects
 * only if may_connect. plugged: what the USB port's controller says
 * (nspire_usb_plugged: 1 something's on it, so the OS's tasks get the CPU
 * at least once a frame to set it up; -1 can't tell, so in spare time and
 * at least every 250 ms). Not called with nothing on the port. fresh: the
 * port changed less than 10 s ago (P2: a tick each frame then, while
 * something is set up; later only when it's near, or every 100 ms). */
int link_cable_watch(int may_connect, int plugged, int fresh);
/* SEAM-2: NavNet looks made so far (any caller), and one look now (not
 * connecting; the watch has started the service): the nodes it found. */
unsigned link_cable_looks(void);
unsigned link_cable_look_now(void);
/* A CX's USB controller with its bus access off (nspire_usb_gated: its OS
 * does that with nothing on the port, which then reads "can't tell"): no
 * watching and no OS time in spare time, but a tick at least every 250 ms
 * and a look (not connecting) now and then, so that the OS can turn the
 * controller on again when a cable comes. 0, or < 0: this one can't link. */
int link_cable_gated(void);
/* Nodes the last look found (0: none, the OS hasn't enumerated one). */
unsigned link_cable_last_nodes(void);
/* DETECT (2026-11-11): the USB port just changed (something plugged in, or
 * its controller came up): the next watch or ready looks at once, then
 * often (LINK_HURRY_LOOK_MS, slower if a look costs more) for
 * LINK_HURRY_MS, the OS given a little more time per frame until a node is
 * listed; then the usual rate. */
#define LINK_HURRY_MS       10000
#define LINK_HURRY_LOOK_MS  250
void link_cable_hurry(void);
/* Where the set-up is, for the link's log and its messages: LINK_UP_NODE a
 * look listed a node, LINK_UP_OUT this side's channel to the other's
 * service, LINK_UP_SVC the other's connection came to this side's service
 * (its callback ran: the OS starts it at the first data, the knock),
 * LINK_UP_READY ready. */
#define LINK_UP_NODE  1
#define LINK_UP_OUT   2
#define LINK_UP_SVC   4
#define LINK_UP_READY 8
unsigned link_cable_progress(void);
/* Watching stopped (nothing on the port, a game with no link): lets go of
 * what it held, this side's channel to the other and the other's
 * connection, so neither side takes it for a link. */
void link_cable_unwatch(void);
/* A linked frame's end: what this side queued goes out within frames
 * frames (a game with no spare time gives the OS none, and its messages
 * would wait for its next wait for the other side). */
void link_cable_frame_done(unsigned frames);
/* What the transport holds back (small messages wait to go together in one
 * write, nspire_link_usb.c) goes now: after a message that must not wait
 * (hello, menu, BYE). */
void link_cable_flush(void);
/* LNK-2: that, and what the OS's task sends (two channels with the OS's
 * sleep) gone too, the OS let run up to 300 ms: before gpSP's menu, which
 * gives the OS no time. */
void link_cable_push(void);
/* A line on the transport's work so far, for the link's log. */
void link_cable_stats(char *text, unsigned size);
/* R3: a short line for the crash ring each second: the service's task's
 * writes and turns since the last, the longest write, what it's in now and
 * for how long, its stack's lowest KB unchanged or not, what's held. */
void link_cable_second(char *text, unsigned size);
/* gpSP is closing: the transport lets go of everything it holds (the
 * calculators' NavNet service, which stays up between links). */
void link_cable_shutdown(void);
/* R11w: the same with its waits cut at ms (0: as link_cable_shutdown); -1
 * if a callback is still in a read or write then (StopService not called). */
int link_cable_shutdown_within(unsigned ms);
/* A number that's different on the two sides (it picks player 1), and a
 * new one each call (a hello that tied rolls again). */
uint32_t link_cable_nonce(void);
/* Why it failed, for the message, or NULL. */
const char *link_cable_error(void);
/* The fast transport (nspire_link_fast.h): what this side can be, for the
 * hello (0: none), and, after both hellos, bringing it up with the other
 * side's (both sides call it at once): 1 if the link runs on it now, 0 if
 * NavNet carries on (the log says why). Either way the link's messages go
 * through link_cable_send/receive as before. */
uint32_t link_cable_fast_offer(void);
/* refused: the arbiter's reason this side may not take the USB controller
 * now (nspire_arb_fast_refusal), or NULL; the other side is told. */
int link_cable_go_fast(uint32_t other_offer, const char *refused);
/* 1 while the fast transport runs on the controller; and letting go of it
 * now (the arbiter's word), NavNet carrying the link from there. */
int link_cable_fast_up(void);
void link_cable_fast_drop(const char *why);
/* LINK2: 1 if a linked frame gives the OS its spare time (OS_WANT_LINK:
 * nspire_wait_until's idle sleeps), as NavNet needs (its messages move only
 * in the OS's tasks); 0 while the fast transport carries the link: nothing
 * of the link's is in the OS then (polled DMA, the USB line masked), the
 * cable's pull and a fall back are its own register checks, and on the CX
 * CAS such a tick cost 50-363 ms each (r6 logs, 2026-11-11). */
int link_cable_linked_os(void);
/* Why the last give back left the USB controller not as the OS had it, or
 * NULL (link_fast_damage): the off switch keeps the fast transport off. */
const char *link_cable_fast_damage(void);
/* 1 once after the fast transport fell back to NavNet mid-link (what was
 * sent goes on, nothing lost): nspire_link.c measures the round trip again
 * and raises the delay. */
int link_cable_fell_back(void);

#endif
