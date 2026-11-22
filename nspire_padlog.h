/* PAD3 (2026-11-11): the pad log, gpsp_pad.txt.tns in gpSP's files folder,
 * a file of its own that the link log's trimming can't touch. A ring of the
 * last PADLOG_RING events kept as numbers; text only when written (a try's
 * end, every PADLOG_TIMER_MS at most while something at the adapter's end
 * doesn't work and the ring has news, the Controller page closing, exit;
 * never while linked; only at nspire_padlog_write_point). */
#ifndef NSPIRE_PADLOG_H
#define NSPIRE_PADLOG_H

#include <stdint.h>

#define PADLOG_FILE      "gpsp_pad.txt.tns"
#define PADLOG_PREV      "gpsp_pad_prev.txt.tns"
#define PADLOG_RING      256
#define PADLOG_SAMPLE_MS 100
#define PADLOG_TIMER_MS  10000
#define PADLOG_TRY_GAP_MS 2000
#define PADLOG_WRITES_MAX 120

enum padlog_kind
{
  PL_PORT = 1, PL_IDLE, PL_OFFER, PL_EVENT, PL_OWNER, PL_STAGE, PL_CYCLE, PL_LINK, PL_VERDICT,
  PL_TRY_END, PL_USER, PL_TOAST, PL_ATTACH_OS,
  /* PAD4: the come-up (nspire_usbpad_glue.c): its start (a: at gpSP's
   * start), each step (a: CU_*, w0 ms after the plug, w1 OS sleeps since,
   * w2 PORTSC), the OS's time in one go (a: PL_BURST_*, b: in a game, w0 ms,
   * w1 OS sleeps, w2 the host controller running at its end; R11t: c, it
   * waited for the host controller), its end (a:
   * PL_CU_*, b: at start, c: a burst ran, w0 ms, w1 OS sleeps, w2 the burst's ms) */
  PL_COMEUP_BEGIN, PL_STEP, PL_BURST, PL_COMEUP, PL_DRIVER_ATTACH, PL_DRIVER_OUT, PL_DRIVE,
  /* R10b (2026-11-15, driver API 6): the driver's bring-up trace, a record
   * an entry (a: USBPAD_STEP_*, b: the task 'U' 'M' 'C', c: driver port + 1;
   * w0 vid << 16 | pid, w1 the error (int32: > 0 the OS's status, < 0
   * USBPAD_E*), w2 ep << 16 | arg, w3 seq, w4 the driver's poll, w5 records
   * lost before it (low half) and routine ones not logged (high half)); the
   * OS's host controller after an offer that brought no pad (a: 1 found
   * stopped, 0 running again; w0 ms since the offer / the halt, w1 USBCMD,
   * w2 PORTSC). PL_EVENT of a failed bring-up: w1 step | ep << 8 |
   * PL_EVENT_FACTS, w2 the error. */
  PL_TRACE, PL_HC,
  /* 2026-11-15 (F1): gpSP's USB audio left a game controller's audio
   * interface to the pad driver (nspire_audio_os.c): w0 vid << 16 | pid,
   * w1 the interface, w2 how many so far. */
  PL_AUDIO,
  /* R12 (2026-11-15): the timer block at a come-up's step (a: the step,
   * PADSTATUS_STEPS' index: connected or reset), six words
   * (nspire_timer_usb_probe): 900C0020's control and load, its counts a ms
   * x 10 (0: not counted), the select 900C0080, 900C0000's control and
   * gpSP's count's speed a ms x 10. */
  PL_TIMER,
  /* R11u (2026-11-15): the driver kept an interface quietly as not a
   * controller (a: bInterfaceNumber; w0 vid << 16 | pid, w1 class << 16 |
   * subclass << 8 | protocol, w2 how many so far). */
  PL_NOT_PAD,
  /* R11u: the USB drive driver's attach would have refused a device its
   * match took; gpSP's wrapper kept it quietly instead (nspire_drive_os.c:
   * w0 vid << 16 | pid, w1 the interface, w2 how many so far; a: the real
   * attach's error). */
  PL_DRIVE_KEPT,
  /* R11w (2026-11-15): the milestones' own (nspire_padlog_milestone): the
   * driver's counters at a detach's end and at exit (w0 matches, w1
   * attaches, w2 detaches, w3 OUTs sent from the attach, w4 from the main
   * task (fallbacks included), w5 from the IN completion); the exit's USB
   * shutdowns (a: PL_EXIT_*, w0 ms or the watchdog's counts a ms); a pad's
   * input (a: 1 its first report; w0 vid << 16 | pid, w1 the slot). */
  PL_MS_COUNTERS, PL_EXIT, PL_MS_INPUT,
  /* Hub (2026-11-16): a USB hub on the port (nspire_usbpad_glue.c). a:
   * PL_HUB_IN (w0 vid << 16 | pid), PL_HUB_OUT, PL_HUB_BURST (the OS alone
   * for a device on it: b in a game, c why it ended (PL_HUBB_*), w0 ms, w1
   * OS sleeps, w2 the offers it brought), PL_HUB_LEFT (a device left a port
   * of it: w0 the pad driver's detaches so far), PL_HUB_NO_CYCLE (a power
   * restart that was due and not made: it would cut every device on it).
   * And the USB sound adapter's driver (nspire_audio.c): a its state
   * (UA_*), b the state before, w0 vid << 16 | pid, w1 the stream's
   * completions so far, w2 bit 0 its callbacks or transfers are still the
   * OS's, bit 1 attached. */
  PL_HUB, PL_SOUND,
  /* Bluetooth (nspire_bt_glue.c): a PL_BT_*. PL_BT_STATE: b the library's
   * event (NSPIRE_BT_EV_ON 1, OFF 2, FAILED 3; 0 the start failed), c its
   * err (0xFFFF the start), w0 its state after. PL_BT_PAD / PL_BT_AUDIO: b 1
   * connected, 0 disconnected, c the pad's slot, w0 its kind, w1 the
   * address's last three bytes. PL_BT_STOP: b the stop's verdict (0 clean,
   * 1 USB memory left to the OS, 2 a callback still inside gpSP), w0 ms. */
  PL_BT,
  /* (r19-usb) The adapter's power (nspire_usbpad_glue.c watch_power): a 1
   * turned on and still off PADSTATUS_STABLE_MS later (w0 ms, w1 OTGCSR / a
   * CX's OTGSC), 2 it came up after being held down (w0 ms), 3 it came on
   * by the OS's own start (w0 ms after the adapter was seen, w1 OS sleeps). */
  PL_POWER,
  /* R11w: "port:" lines whose words say the same as the line before (the
   * lines' state moving on an enabled port: R11w 192.2-199.1 s) are not
   * repeated: the line before counts them (its c) */
};
enum padlog_bt { PL_BT_STATE = 1, PL_BT_PAD, PL_BT_AUDIO, PL_BT_STOP };
enum padlog_exit { PL_EXIT_LOGGED = 1, PL_EXIT_USB_BEGIN, PL_EXIT_USB_DONE, PL_EXIT_TIMEOUT };
#define PL_EVENT_FACTS 0x1000000u
/* Entries of a try that ended with nothing working are kept: the ring drops
 * other entries first (at most PADLOG_KEEP_MAX kept, the newest tries'; the
 * last PADLOG_KEEP_TRY entries of each). */
#define PADLOG_KEEP_MAX 160
#define PADLOG_KEEP_TRY 96
enum padlog_comeup { PL_CU_WORKS = 1, PL_CU_DRIVER, PL_CU_CALC, PL_CU_GONE, PL_CU_LINK, PL_CU_OTHER, PL_CU_TIMEOUT,
                     PL_CU_IDLE, PL_CU_NOCONN, PL_CU_NOT_PAD, PL_CU_HUB /* a hub: its devices come by the OS's offers */ };
enum padlog_hub { PL_HUB_OUT = 0, PL_HUB_IN, PL_HUB_BURST, PL_HUB_LEFT, PL_HUB_NO_CYCLE,
                  PL_HUB_POOL /* w0 stale abort marks taken off the OS's free transfer blocks now, w1 so far */,
                  PL_HUB_DEAD /* (2026-11-17) the port under the hub no longer the OS's working port: w0 USBCMD, w1 PORTSC */,
                  PL_HUB_NEWS /* (2026-11-17) a port of the hub changed, by the OS's own flag: w0 the times so far */,
                  PL_HUB_PORTS /* (2026-11-17) the hub's ports as the OS's hub driver sees them: b the driver's
                                  running | suspended << 1 (| 4: a port's device does not start, | 8: over-current),
                                  c the ports' number | its active children << 4, w0 / w1 a byte a port (1-4, 5-8:
                                  USBPAD_OS_HUBP_*), w2 the looks gpSP asked the OS for so far */,
                  PL_HUB_BLIP /* (2026-11-18) the port's power under the hub: b PL_HUBP_* (w0 OTGCSR, w1 OTGISR at
                                 the fall; w2 ms it was off at the return) */,
                  PL_HUB_RECOVER /* (2026-11-18) the dead hub's recovery: b why (PL_HUBR_WHY_*), c PL_HUBR_* step,
                                    w0..w2 the step's numbers */ };
enum padlog_hub_power { PL_HUBP_FELL = 0, PL_HUBP_BACK, PL_HUBP_ALIVE, PL_HUBP_OS_TOOK };
enum padlog_hub_recover_why { PL_HUBR_WHY_POWER = 1, PL_HUBR_WHY_WEDGE };
enum padlog_hub_recover { PL_HUBR_RESTART = 0 /* w0 the restart's result, w1 recoveries so far */,
                          PL_HUBR_BACK /* w0 ms from the restart to the hub's offer, w1 1: after "still dead" */,
                          PL_HUBR_OFFERED /* w0 devices offered again, w1 ms from the restart */,
                          PL_HUBR_STILL_DEAD /* w0 ms waited for the hub's offer */,
                          PL_HUBR_NOT_MADE /* w0 recoveries so far (the limit), w1 the restart's refusal (< 0:
                                              refused now, tried again) */,
                          PL_HUBR_ALIVE /* not cut after all: w0 1 data came in or the OS worked on a device, 2 the
                                           hub answers a request, 3 the hub can't be asked and nothing says dead */,
                          PL_HUBR_CANCELLED /* the recovery ends: w0 1 the hub left, 2 another hub, 3 no hub on the
                                               port (empty, the cable's device end, nothing to restart); w1 the
                                               wait it was in */ };
enum padlog_hub_burst { PL_HUBB_DONE = 1, PL_HUBB_TIME, PL_HUBB_KEY, PL_HUBB_NO_SLEEP, PL_HUBB_GONE };
enum padlog_burst { PL_BURST_TIME = 1, PL_BURST_WORKS, PL_BURST_ENDED, PL_BURST_KEY, PL_BURST_IDLE, PL_BURST_NO_SLEEP,
                    PL_BURST_HC_WAIT };
enum padlog_try { PL_TRY_PAD = 1, PL_TRY_NO_OFFER, PL_TRY_GAVE_UP, PL_TRY_UNPLUGGED, PL_TRY_FAILED,
                  PL_TRY_NOT_PAD /* R11u: not a controller, nothing failed */,
                  PL_TRY_POWER_UP /* (r19-usb) the adapter's power came up after it was held down */ };
enum padlog_user { PL_USER_PAGE = 1, PL_USER_CYCLE, PL_USER_OS_TIME, PL_USER_PAGE_LEFT };

void nspire_padlog_add(enum padlog_kind kind, int a, int b, int c, uint32_t w0, uint32_t w1, uint32_t w2);
/* The same with all six words (PL_TRACE). */
void nspire_padlog_add6(enum padlog_kind kind, int a, int b, int c, const uint32_t w[6]);
/* The entries kept for failed tries now (the tests). */
unsigned nspire_padlog_kept(void);
/* The pad glue's words for the driver's numbers (weak defaults here: the
 * numbers): a trace entry (PL_TRACE's fields) and a failed bring-up's facts
 * (PL_EVENT's w1, w2), each one line without a newline. */
void nspire_usbpad_trace_words(int step, int task, int port, const uint32_t w[6], char *out, unsigned size);
void nspire_usbpad_fail_words(uint32_t facts, uint32_t err, char *out, unsigned size);
void nspire_padlog_poll(void);
uint32_t nspire_padlog_wires_counter(void);
/* The same for a device leaving the wires (the glue: what's offered from
 * then on is the next device's). */
uint32_t nspire_padlog_wires_gone(void);
void nspire_padlog_try_end(enum padlog_try why);
void nspire_padlog_request(const char *why);
void nspire_padlog_failing(int failing);
void nspire_padlog_write_point(void);
void nspire_padlog_exit(void);

/* R11w (2026-11-15): the milestones, a ring of their own that the event
 * list's port lines can't displace: every OS offer and its answer, every
 * attach and its result, every detach (begin, end, what was in flight),
 * every abort / free / close and its status, every OUT with its task and
 * its completion, a pad's first report, the driver's counters at each
 * detach and at exit, the exit's USB steps. The first PADLOG_MS_FIRST are
 * kept for good, then the latest PADLOG_MS_RING; one just like the one
 * before (its numbers but the record's sequence) is counted on that one.
 * Written into the pad log with every write (exits and failures included),
 * after the events; at exit the driver's whole diag block follows. */
#define PADLOG_MS_FIRST 32
#define PADLOG_MS_RING  192
void nspire_padlog_milestone(enum padlog_kind kind, int a, int b, int c, uint32_t w0, uint32_t w1, uint32_t w2);
void nspire_padlog_milestone6(enum padlog_kind kind, int a, int b, int c, const uint32_t w[6]);
unsigned nspire_padlog_milestones(void);   /* kept now (tests) */
/* The exit's marks (PL_EXIT_*) as milestones; the timed-out USB shutdown's
 * (its step's words live on) also written at once, past the exit's write. */
void nspire_padlog_exit_mark(int code, uint32_t value);
void nspire_padlog_exit_timeout(const char *step, uint32_t ms);
/* The pad glue's text for the log (weak: none without it): the driver's
 * counters as one line, and its whole diag block (lines). */
int nspire_usbpad_counters_text(char *out, unsigned size);
int nspire_usbpad_diag_text(char *out, unsigned size);
unsigned nspire_padlog_text(char *out, unsigned size, const char *why);
unsigned nspire_padlog_writes(void);
unsigned nspire_padlog_count(void);
void nspire_padlog_reset(void);

#endif
