/* Letting the calculator's OS run its own tasks (nspire_os.c).
 *
 * gpSP runs in the OS's main task and never waits on the OS, so the OS's
 * lower-priority tasks (the USB host's, NavNet's) don't get the CPU while a
 * game runs: interrupts alone don't run them. A cable plugged in after gpSP
 * started was never set up, a service's connection waited for its callback,
 * and every NavNet message took ~100 ms (the OS's 100 Hz tick). Sleeping one
 * OS tick in the OS's own TCC_Sleep (Nucleus) lets them run.
 *
 * Also what the link's log says about the calculator, all read only: the
 * OS, the USB port's state by its controller's registers, the screen's and
 * the interrupts'. Nothing here writes a register but the watchdog's
 * control (nspire_os_watchdog_off, only if the OS turned it on). */
#ifndef NSPIRE_OS_H
#define NSPIRE_OS_H

#include <stdint.h>

/* gpSP runs with the CPSR's IRQ mask (its I bit) set, as both calculators'
 * logs show: interrupts come only where it's cleared, around each OS
 * sleep. These clear it (FIQs and the mode left as they are) and put the
 * CPSR's control byte back as it was. (On the PC: nothing.) */
static inline uint32_t nspire_irqs_unmask(void)
{
#ifdef __arm__
  uint32_t old, on;
  __asm__ volatile("mrs %0, cpsr\n\tbic %1, %0, #0x80\n\tmsr cpsr_c, %1" : "=r"(old), "=r"(on) : : "memory");
  return old;
#elif defined(NSPIRE_HOST)
  /* gpsp-host: the OS's interrupt handlers get the CPU (its keypad
   * handler, if its line is on: tests/host/host_platform.c). */
  void host_os_irq_window(void);
  host_os_irq_window();
  return 0;
#else
  return 0;
#endif
}
static inline void nspire_irqs_restore(uint32_t old)
{
#ifdef __arm__
  __asm__ volatile("msr cpsr_c, %0" : : "r"(old) : "memory");
#else
  (void)old;
#endif
}

/* Finds TCC_Sleep in the OS's code (once; ~0.1 s) and times the OS tick:
 * 1 if it can sleep (not if the OS's tick, its timer or interrupt, is off:
 * a sleep would never come back). */
int nspire_os_ready(void);
/* (r19-usb-exit) The first nspire_os_ready()'s time (timer ticks: the scan
 * and the tick's probe), 0 before it; *at (if not null): when it ended. */
uint32_t nspire_os_ready_cost(uint32_t *at);
/* (r19-usb-exit) 1: nspire_os_sleep returns 0 at once, counted, until 0
 * again (the quit's drive shutdown with nothing ever on the port). */
void nspire_os_sleep_hold(int on);
uint32_t nspire_os_sleeps_held(void);
/* (r19-usb) Called first by every nspire_os_sleep while set: the exit's USB
 * shutdowns set it (nspire_exit.c: a wait that sleeps on past their time is
 * cut there, wherever it is: in gpSP or in a driver's library). */
extern void (*nspire_os_sleep_guard)(void);
/* Sleeps one OS tick (10 ms at most): 1 if it did. */
int nspire_os_sleep(void);
/* (final/r19-pace) called as each sleep wakes, while set: fast forward's
 * picture waiting for the LCD flipped as soon as the LCD has the last one,
 * not a frame later (nspire_frameskip.c ff_flush) */
extern void (*nspire_os_wake_hook)(void);
/* Sleeps a tick only if the OS's next tick comes before 'deadline' (timer
 * ticks, nspire_ticks), with room to spare: a wait that costs nothing. 1 if
 * it did. */
int nspire_os_sleep_before(uint32_t deadline);
/* P2: sleeps a tick only if the OS's next tick is known (a wake within
 * the last 8 ticks says where they fall) and at most 'max_wait' timer ticks
 * away: a short wait, for a game with no spare time. 1 if it did.
 * NSPIRE_OS_NEAR (0): a tenth of the OS's tick, at least 16 timer ticks
 * (CX II 33, 1 ms; CX 16, 0.5 ms). */
#define NSPIRE_OS_NEAR 0
int nspire_os_sleep_near(uint32_t max_wait);
/* P2: the wait a near sleep may take 'since' ticks after the last, between
 * a try's first chance ('from') and its forced sleep ('until'): the least
 * (NSPIRE_OS_NEAR's) growing to it plus half a tick. A fixed 1 ms let frames
 * whose length beats against the tick's miss it every time (5.7 ms frames:
 * 88 ms a second waited in tests/link_usb's model; 27 with this). */
uint32_t nspire_os_near_wait(uint32_t since, uint32_t from, uint32_t until);
/* 2026-11-15: timer ticks from now to the OS's next tick in *ticks, the
 * tick's length in *period, and 1 if where the ticks fall is known (as
 * sleep_near: a wake within the last 8 ticks), else 0 with the most it can
 * be (a whole tick). Sleeps nowhere. (speed9a) With the tick's timer read
 * (nspire_os_tick_timer): known always, the middle of what its count says. */
int nspire_os_next_tick_in(uint32_t *ticks, uint32_t *period);
/* (2026-11-17, final/speed9a) The tick's phase from
 * its own timer (the SP804 at 0x900D0000, read only): nspire_os_ready's
 * probe (nspire_os_tick_probe, also for tests) decides; 1 if it is read,
 * 0 if r15's rule (from the last wake) stands. */
int nspire_os_tick_probe(void);
int nspire_os_tick_timer(void);
/* By the timer: the earliest and the latest the tick can come (timer ticks
 * from now); the counts to it (d: within ((d - 1) c, d c], c a count of the
 * timer) if read, 0 if it can't be read now. */
uint32_t nspire_os_tick_bracket(uint32_t *earliest, uint32_t *latest);
/* The counts to the tick at which it is at least 'need' timer ticks away
 * and less than a count more (ceil(need / c) + 1); 0 without the timer. */
uint32_t nspire_os_tick_need(uint32_t need);
/* The lines' check (nspire_lines.c os_watch, inline, while
 * nspire_os_watching): nspire_os_watch_line is called only when
 * *reg - 1 < value, reg the tick timer's count register (a word that never
 * is, without the timer's rule). nspire_os_watch_arm(d): from d counts to a
 * tick on (0: never). */
struct nspire_os_watch { volatile const uint32_t *reg; uint32_t value; };
extern struct nspire_os_watch nspire_os_watch_at;
void nspire_os_watch_arm(uint32_t counts);
/* By the timer: how late a wake comes after its tick (timer ticks: the OS's
 * own time then), a running average of the sleeps'; 0 before any. */
uint32_t nspire_os_wake_late(void);
/* One line for the logs: which rule gives the tick's phase, and why. */
void nspire_os_tick_rule_report(char *text, unsigned size);
/* The longest from a sleep's wake to the next sleep (timer ticks) since the
 * last call, and starts anew. */
uint32_t nspire_os_gap_longest(void);
/* P2: timer ticks slept so far outside nspire_wait_until's idle wait (the
 * OS's time taken from the game; the perf line's "os"), and in *count (if
 * not null) those sleeps. nspire_os_idle is set while in that wait. */
uint32_t nspire_os_busy_slept(uint32_t *count);
extern int nspire_os_idle;
/* Ticks since a sleep last returned (the OS had the CPU then). */
uint32_t nspire_os_since_sleep(void);
/* Sleeps so far (the plug-in timeline in the link's log counts them). */
uint32_t nspire_os_sleeps(void);
/* R3: the longest sleep (ticks) since the last call, and starts anew. A
 * sleep is a tick at most (10 ms): a longer one is the OS busy. */
uint32_t nspire_os_sleep_longest(void);
/* Who wants gpSP to let the OS run in its spare time, a bit each: the link
 * (something on the USB port while no link is up, looking for the other
 * calculator, or linked), the USB gamepad's driver and the USB flash
 * drive's (other branches, nspire_usbpad* and nspire_usbms*: the OS's USB
 * task has to run while a drive is read). Each sets and clears only its
 * own bit; nonzero is wanted. Never otherwise, as the OS's dimming,
 * power-down and brightness keys then act during the game. UNVERIFIED on
 * a calculator: whether the OS dims the screen or powers down (3 minutes
 * without a key, by default: gpSP reads the keys itself, so the OS sees
 * none) during a long game with a USB gamepad, which wants the OS every
 * frame, or a long link. The link's heartbeat logs the backlight and the
 * OS's busy cursor (nspire_os_diag_screen) and the last key's time. */
#define OS_WANT_LINK 1u
#define OS_WANT_PAD  2u
#define OS_WANT_MS   4u
#define OS_WANT_AUDIO 8u
#define OS_WANT_BT    16u   /* the Bluetooth dongle coming or going (nspire_bt_glue.c) */
extern unsigned nspire_os_wanted;
/* Sets (on) or clears this one's bit of nspire_os_wanted. */
static inline void nspire_os_want(unsigned who, int on)
{
  nspire_os_wanted = (nspire_os_wanted & ~who) | (on ? who : 0u);
}
/* A line for the link's log: how it was found, the tick, the sleeps. */
void nspire_os_report(char *text, unsigned size);
/* Where Nucleus keeps its current task's pointer (TCC_Sleep's literal), or
 * 0: the NavNet service's callback reads its own task from it. */
extern uint32_t nspire_os_task_var;
/* A task's stack words as ND Screen Mirror found them on a CX II's OS 6.2
 * (+0x24 and +0x28 its bounds, +0x2c its stack pointer as saved when it
 * last stopped; not known on other OSes, so only logged), if task is one
 * ("TASK" at +0x0c): 1, else 0. */
int nspire_os_task_stack(uint32_t task, uint32_t words[3]);

/* The USB port by its controller's OTG status (read only): 0 nothing on
 * it (the controller's own unplugged state: the device end, no session),
 * 1 something (a cable to the other calculator or a computer, the OTG
 * adapter), -1 can't tell (a controller this doesn't know, its clock off,
 * a read of all zeros or ones). */
int nspire_usb_plugged(void);
/* 1 on a CX whose OS has turned the bus access to the USB controller off
 * (0x900B0018 bit 5; its registers read stale: nspire_usb_plugged is -1):
 * as it does with nothing on the port, and on again once something's
 * plugged in (with the OS let run: the CX CAS's log, 2026-11-08, saw it
 * come on with no OS sleep, in NavNet's looks). 0 otherwise. */
int nspire_usb_gated(void);
/* A controller-side bus power cycle (S4, nspire_usbcycle.c): drop the bus
 * power, ~300 ms of OS ticks, raise it, wait up to 500 ms for VBUS valid, so
 * the OS enumerates what is on the port again. Blocks about 0.8 s
 * (sleeping, IRQs masked only around each register write). Returns -1 when
 * there's nothing to cycle (the device's end, nothing connected), -2 when it
 * can't be done safely here (a controller it doesn't know or gated, not in
 * host mode, no OS sleep, a timer that doesn't run), else 0 (VBUS not valid
 * at the end, or gated off in the drop: no further cycle) or 1 (valid). The
 * caller (the arbiter) has checked that no link, pad or drive has the port.
 * Logs "reenum: cycle N ...". */
int nspire_usb_reenumerate(int attempt);
/* (2026-11-17, nspire_usbcycle.c) A CX II's transceiver left asleep by the
 * OS's suspend: 1 asleep at the host's end, 0 not, -1 not this controller;
 * and its wake (1 woken, 0 nothing to do or not now: the OS holds a
 * suspended device on the port, or resets it). */
int nspire_usb_phy_asleep(void);
int nspire_usb_phy_wake(void);
/* The controller nspire_usbcycle.c drives: 2 a CX II's FOTG210, 1 a CX's
 * ChipIdea, 0 neither (gated off, or its reads are stale). */
int nspire_usb_kind(void);
/* A CX's controller gated off in a cycle's drop left PP 0: put back at the
 * first call after the gate opens, if the ID pin still says the host's end
 * (the arbiter's every tick; a compare when nothing's pending). 1 if it did. */
int nspire_usb_power_restore(void);
/* 1 while that PP is still to put back (the arbiter: the gate is ours). */
int nspire_usb_power_restore_pending(void);
/* The status bits the plug-in timeline compares (0 if it can't tell). */
uint32_t nspire_usb_bits(void);
/* The controller's run bit (USBCMD bit 0): 1, 0 stopped, -1 can't tell. */
int nspire_usb_running(void);
/* PAD3: the controller's raw words, read only (the pad log, the Controller
 * page). A CX II's FOTG210: OTGCSR (0x80), OTGISR (0x84), USBCMD (0x10),
 * USBSTS (0x14), PORTSC (0x30), the global interrupt status (0xC0). A CX's
 * ChipIdea: OTGSC (0x1A4), USBMODE (0x1A8), USBCMD (0x140), USBSTS (0x144),
 * PORTSC1 (0x184), USBINTR (0x148). Returns the kind (nspire_usb_kind: 2,
 * 1, or 0 with every word 0: gated off or not readable). In both PORTSC
 * words bit 0 is connected, bit 2 enabled, bits 11:10 the lines (0 SE0:
 * nothing pulls them up, 1 K, 2 J: a device's pull-up). */
#define NSPIRE_USB_RAW 6
int nspire_usb_raw(uint32_t w[NSPIRE_USB_RAW]);
/* Lines for the link's log: the OS (its id, as Ndless knows it) and the
 * model; the USB controller's role and port; the screen, the interrupts
 * and the timers, and the real-time clock. */
void nspire_os_diag_system(char *text, unsigned size);
/* At gpSP's start: a CX II's power controller words then (the diag line has them). */
void nspire_os_note_start(void);
void nspire_os_diag_usb(char *text, unsigned size);
void nspire_os_diag_screen(char *text, unsigned size);
/* R3, short, for the link's ring each second: the watchdog (control, load,
 * count, "due" if its interrupt's raised, "locked"), the backlight, the
 * OS's busy cursor and the interrupt controller's enables. */
void nspire_os_diag_watch(char *text, unsigned size);
/* R3: the watchdog off again if the OS turned it on (Ndless turns it off
 * when a program starts and notes the OS turns it on again): 1 if it was
 * on, else 0 (nothing written). Done after each nspire_os_sleep. */
int nspire_os_watchdog_off(void);
/* Times it was found on and turned off. */
uint32_t nspire_os_watchdog_offs(void);

/* R11w (2026-11-15): the exit's backstop and the restart (nspire_exit.c).
 * nspire_os_backstop_arm: the SP805 watchdog set to reset the calculator
 * after about ms (its count's speed measured against gpSP's timer first:
 * the load is that many counts for half of ms, the reset coming at its
 * second time-out); nspire_os_watchdog_off leaves it alone while armed.
 * Returns the counts a ms it measured (0: it doesn't count, not armed).
 * It covers a hang inside an OS call (an abort that never returns), where
 * gpSP's own bounded waits can't act. nspire_os_backstop_disarm: off again
 * (watchdog control 0), as gpSP keeps it outside the exit.
 * nspire_os_restart: restarts the calculator, Ndless's own way
 * (resources/utils.c ut_calc_reboot, its uninstaller's restart on every
 * model): interrupts off, 2 written to 0x900A0008 (the CPU reset), and if
 * that doesn't take, the watchdog unlocked, loaded with 0x1000 and set to
 * reset (control 3), then a spin. Called only with every file closed.
 * Never returns on the calculator. */
uint32_t nspire_os_backstop_arm(uint32_t ms);
void nspire_os_backstop_disarm(void);
int nspire_os_backstop_armed(void);
void nspire_os_restart(void);

/* R6: the OS's keys. After a linked trade the CX CAS's file browser, once
 * gpSP had quit, pressed del, right and enter on its own and deleted
 * gpSP's program file: the keys pressed in the game had reached the OS.
 * Its keypad interrupt (16 at the interrupt controller, a PL190 at
 * 0xDC000000; Firebird's INT_KEYPAD) is handled whenever interrupts are on:
 * in every OS sleep (nspire_os_sleep), in the OS's other tasks while a
 * NavNet call or a file write waits, in the pad driver's ticks. The OS
 * queues a key event for each change and nobody takes them while gpSP
 * runs in the OS's main task; the document browser gets them all after
 * exit. So the line is off at the controller from gpSP's start to its
 * exit, whatever the CPU's mask (nspire_exit.c).
 * nspire_os_keypad_irq: off (0) or on (1): returns 1 if it was on.
 * nspire_os_keypad_keep_off: off again if something turned it on while
 * gpSP has it (1 if it did; counted).
 * EXIT-1 (2026-11-11): R6 also took the OS's pending events at exit with
 * Ndless's get_event; on both calculators no key worked after quitting
 * (the pointer moved, the CX II's busy clock stayed). The one place in
 * the exit where the main task let the OS run after the line was back was
 * that drain: if get_event waits when there's no event (the symptoms say
 * so; not proven), the main task sat in it and took and dropped every key
 * and click. Gone: nothing at exit lets the OS run once its keypad is
 * back. */
int nspire_os_keypad_irq(int on);
int nspire_os_keypad_keep_off(void);
uint32_t nspire_os_keypad_retaken(void);
/* EXIT-1: the keypad line, the keypad controller's words and the LCD's
 * cursor control and place as gpSP starts (main's first act); EXIT-R5: on a
 * CX II the cursor is then put off the panel (back in the hand-back). */
void nspire_os_hw_note_start(void);
/* EXIT-2: after the OS ran (each sleep, NavNet calls): the keypad line off
 * again and the OS's busy cursor (the LCD's hardware cursor) off, if on. */
void nspire_os_after_os_time(void);
uint32_t nspire_os_cursor_offs(void);
/* EXIT-1: the start's words, the words now, and what the hand-back will
 * write, as text for the clean-exit marker (written before the hand-back:
 * the log is closed by then). */
void nspire_os_hand_back_text(char *text, unsigned size);
/* EXIT-R4: the same words read back after the hand-back, for the file
 * written after it (gpsp_exit_after.txt.tns). */
void nspire_os_after_hand_back_text(char *text, unsigned size);
/* QUIT6: the interrupt controller's lines now (register reads only): those
 * raised and enabled (taken by the CPU as soon as the OS unmasks its
 * interrupts: a source left raised that the OS doesn't clear is taken for
 * ever, a freeze), all raised, all enabled; 'when' names the moment. */
void nspire_os_irq_text(char *text, unsigned size, const char *when);
/* EXIT-1: the very last thing gpSP does (nspire_exit.c): the keypad line on,
 * the keypad controller's control, size and interrupt enables and the LCD's
 * cursor control as read at start, by register writes only; from then on
 * nspire_os_sleep refuses (counted), nothing masks the line again. */
void nspire_os_hand_back(void);
int nspire_os_handed_back(void);

#endif
