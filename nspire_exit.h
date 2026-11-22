/* The way out of gpSP (nspire_exit.c, EXIT-1..3, 2026-11-11).
 *
 * Every way out (Exit gpSP, Save+Exit, the quit hotkey, an error exit, a
 * failed load, main's return, the PC's script end) goes through exit(),
 * and nspire_exit_start's handler, registered first, runs last of gpSP's:
 * after quit() (nspire_host_main.c) and after the pad glue's stop(). It
 * waits a little for the keys to be let go, writes the log's last lines
 * and the clean-exit marker (gpsp_link_run.txt.tns: the exit's steps in ms,
 * the hardware words at start and at exit), gives the timer back, and then,
 * as the very last thing, nspire_os_hand_back (nspire_os.h): the keypad
 * line, the keypad controller and the LCD's cursor as they were, by
 * register writes; nothing after it masks anything, and only EXIT-R4's
 * small file (gpsp_exit_after.txt.tns: the words read back), written when
 * no key is down, lets the OS run. */
#ifndef NSPIRE_EXIT_H
#define NSPIRE_EXIT_H

/* main's first act: the hardware words noted, the OS's keypad line off
 * (R6), the hand-back registered with atexit. */
void nspire_exit_start(void);
/* EXIT-R1: gpSP is on its way out (quit chosen, quit(), an exit handler):
 * the exit's clock starts here (the first call only). */
void nspire_exit_begin(void);
/* EXIT-3: the time since the last step (or since the exit began) goes to
 * 'name' (a string that lives on); nothing before nspire_exit_begin (a game
 * left for another from the menu is not an exit). In memory only: written
 * with the marker at the end. */
void nspire_exit_step(const char *name);
/* The steps so far, "name ms, ...; total ms", for a log line. */
void nspire_exit_steps_text(char *text, unsigned size);

/* R11w (2026-11-15): the USB shutdowns at the exit, bounded. The R11w run
 * hung there (the pad log written, the clean marker never: keys dead, a
 * reset). Every file is written before them (quit(), nspire_host_main.c).
 * nspire_exit_usb_begin: their clock starts (EXIT_USB_MS for all of them)
 * and the watchdog backstop is armed (EXIT_BACKSTOP_MS: a hang inside an
 * OS call, where no wait of ours can act). nspire_exit_usb_left_ms: what
 * is left of it (0 once over; EXIT_USB_MS before begin). nspire_exit_usb_end:
 * all done, the backstop off. nspire_exit_usb_timed_out: a shutdown didn't
 * finish within its limit: the code the OS can still call must not be
 * unloaded and the keys must not be left dead, so: "exit: USB shutdown
 * timed out at <step>" into the pad log and the link log (written), a
 * plain message for two seconds, and the calculator restarts
 * (nspire_os_restart). Never returns on the calculator. */
#define EXIT_USB_MS      4000
#define EXIT_BACKSTOP_MS 12000
#define EXIT_MESSAGE_MS  2000
#define EXIT_USB_TEXT1   "USB did not shut down."
#define EXIT_USB_TEXT2   "The calculator restarts now."
void nspire_exit_usb_begin(void);
unsigned nspire_exit_usb_left_ms(void);
void nspire_exit_usb_end(void);
void nspire_exit_usb_timed_out(const char *step);

/* (r19-usb, 2026-11-21) The CX II's night run froze on the quitting screen:
 * its pad log said "the USB shutdowns come next", its marker still
 * "running", the Bluetooth live log not removed: somewhere in the USB
 * shutdowns, and nothing said where.
 * nspire_exit_trail: before each step of the way out, one line appended to
 * EXIT_TRAIL_FILE in the saves folder and the file closed again ("exit step
 * N at T ms: <what>"), so that a freeze or a reset leaves the step it was in
 * on the disk; the hand-back removes the file. nspire_exit_trail_check (at
 * the start, the saves folder made): a trail left by the last run is read,
 * its last line kept for the pad log's header and the link log
 * (nspire_exit_trail_last_run, "" when the last run ended), the file
 * removed. Not a .tns name: the OS never lists it.
 * The USB shutdowns' sleeps: every wait in them that lets the OS run sleeps
 * through nspire_os_sleep (the sound's, Bluetooth's, the pad driver's and
 * the drive's drivers alike), and from nspire_exit_usb_begin to _end each
 * sleep asks nspire_exit.c first: once the shutdowns have run EXIT_USB_MS
 * + EXIT_USB_GRACE_MS by gpSP's clock (or, with gpSP's timer not running,
 * EXIT_USB_SLEEPS_MAX sleeps), a wait that still sleeps is one that would
 * never end (a callback that never comes, a device that never answers): its
 * step is timed out (the trail says which wait, then the restart above). A
 * wait inside an OS call, which never comes back to a sleep of gpSP's, can't
 * be cut so: the trail's last line names its step. */
#define EXIT_TRAIL_FILE     "gpsp_exit.run"
#define EXIT_USB_GRACE_MS   2000
#define EXIT_USB_SLEEPS_MAX 3000
void nspire_exit_trail(const char *what);
void nspire_exit_trail_check(void);
const char *nspire_exit_trail_last_run(void);

/* 2026-11-11: the picture's log lines (the LCD at a game's first frame,
 * after the last menu, at the quit), for the exit marker; empty without a
 * game (nspire_host_main.c). */
void nspire_picture_log_text(char *text, u32 size);

#endif
