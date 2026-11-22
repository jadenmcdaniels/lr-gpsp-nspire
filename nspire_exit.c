/* The way out of gpSP (nspire_exit.h): EXIT-1..3, 2026-11-11.
 *
 * The first hardware run of the seamless build (SEAM1, f29402d) on a CX II
 * and a CX CAS: after quitting, no key and no click worked on either (the
 * touchpad still moved the pointer) until a reset; the CX II's busy clock
 * stayed; the CX CAS took long to quit. Both had written the clean marker,
 * which quit() wrote first (nspire_link_shutdown), so it said nothing about
 * what came after. What came after, in the order it ran: quit()'s steps,
 * the pad glue's stop() (atexit; the driver's shutdown lets the OS run with
 * its keypad line still off), then R6's give-back (atexit, registered
 * first): a wait for the keys, the keypad line on, and Ndless's get_event
 * called until it said there was nothing, up to 4096 times. With the line
 * back, that call was the only place where gpSP's task let the OS run; if
 * get_event waits for an event when there is none (what the symptoms
 * point to: the pointer is the OS's, its keypad interrupt handled, its
 * events never acted on), the main task sat in that loop, taking and
 * dropping every key and click, and the busy clock stayed because the OS's
 * main task never got back to its own loop. The line itself was on: the
 * touchpad's interrupt is the keypad's line 16 too (Firebird keypad.cpp),
 * and the pointer moved.
 *
 * Now: no get_event at all; the hand-back is register writes only and the
 * last thing; after it nspire_os_sleep refuses (counted in the marker) and
 * nothing masks the line again. The marker, written right before it, has
 * the exit's steps in ms and the words at start and at exit. */

#include "common.h"
#include "nspire.h"
#include "nspire_exit.h"
#include "nspire_keys.h"
#include "nspire_link.h"
#include "nspire_os.h"
#include "nspire_padlog.h"
#include "nspire_paths.h"
#include "nspire_ui.h"
#include "nspire_frontend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXIT_STEPS 40
/* (r19-usb-exit) Each step's OS sleeps too (their count, and the ms the
 * non-idle ones took), and the first look for the OS's sleep when it fell
 * in the step (nspire_os_ready_cost: the scan and the tick's probe, ~0.5 s
 * on a CX): which of a step's ms were the OS's, which gpSP's own work or
 * its file calls. */
static struct
{
  const char *name;
  u32 ms, sleeps, slept_ms, ready_ms;
} steps[EXIT_STEPS];
static unsigned step_count;
static u32 step_mark, exit_began, step_sleeps, step_slept;
static int exiting, started;

static u32 ticks_ms(u32 ticks)
{
  return (u32)((u64)ticks * 1000 / NSPIRE_TICK_HZ);
}

/* EXIT-R1: the clock starts on the way out only. nspire_leave_game also
 * runs when another game is picked from the menu (gui.c menu_load); its
 * steps then went into the exit's: after a switch the quit's "quit chosen"
 * carried the minutes since it, and after ~6 switches the 32 slots were
 * full and the real quit's steps were dropped. */
void nspire_exit_begin(void)
{
  if (exiting)
    return;
  exiting = 1;
  exit_began = step_mark = nspire_ticks();
  step_count = 0;
  step_sleeps = nspire_os_sleeps();
  step_slept = nspire_os_busy_slept(NULL);
}

void nspire_exit_step(const char *name)
{
  u32 now, sleeps, slept, ready;
  uint32_t ready_at;   /* (uint32_t: unsigned long on the calculator, u32 unsigned int) */

  if (!exiting)
    return;   /* not on the way out (a game switch): nothing kept */
  now = nspire_ticks();
  sleeps = nspire_os_sleeps();
  slept = nspire_os_busy_slept(NULL);
  ready = nspire_os_ready_cost(&ready_at);
  if (step_count < EXIT_STEPS)
  {
    steps[step_count].name = name;
    steps[step_count].ms = ticks_ms(now - step_mark);
    steps[step_count].sleeps = sleeps - step_sleeps;
    steps[step_count].slept_ms = ticks_ms(slept - step_slept);
    /* (the look ended inside this step) */
    steps[step_count].ready_ms = ready && (u32)ready_at - step_mark <= now - step_mark ? ticks_ms(ready) : 0;
    step_count++;
  }
  step_mark = now;
  step_sleeps = sleeps;
  step_slept = slept;
}

void nspire_exit_steps_text(char *text, unsigned size)
{
  unsigned i;
  int n;

  if (!size)
    return;
  n = snprintf(text, size, "steps, ms:");
  for (i = 0; i < step_count && n >= 0 && (unsigned)n < size; i++)
  {
    n += snprintf(text + n, size - n, "%s %s %lu", i ? "," : "", steps[i].name, (unsigned long)steps[i].ms);
    /* "[10 OS sleeps 95 ms]", "[finding the OS's sleep 560 ms, 13 OS sleeps 120 ms]" */
    if (n >= 0 && (unsigned)n < size && (steps[i].sleeps || steps[i].ready_ms))
    {
      n += snprintf(text + n, size - n, " [");
      if (n >= 0 && (unsigned)n < size && steps[i].ready_ms)
        n += snprintf(text + n, size - n, "finding the OS's sleep %lu ms%s", (unsigned long)steps[i].ready_ms,
                      steps[i].sleeps ? ", " : "");
      if (n >= 0 && (unsigned)n < size && steps[i].sleeps)
        n += snprintf(text + n, size - n, "%lu OS sleep%s %lu ms", (unsigned long)steps[i].sleeps,
                      steps[i].sleeps == 1 ? "" : "s", (unsigned long)steps[i].slept_ms);
      if (n >= 0 && (unsigned)n < size)
        n += snprintf(text + n, size - n, "]");
    }
  }
  if (n >= 0 && (unsigned)n < size)
    snprintf(text + n, size - n, "; total %lu ms", (unsigned long)(exiting ? ticks_ms(step_mark - exit_began) : 0));
}

/* ---- r19-usb (2026-11-21): the exit's trail on the disk (nspire_exit.h) ---- */

static unsigned trail_n;
static const char *trail_now = "";   /* the step under way (the last line written) */
static char trail_last[640];          /* the last run's last line, "" when it ended */
static int trail_off;                 /* the hand-back removed the file: no more lines */

void nspire_exit_trail(const char *what)
{
  char path[600];
  FILE *f;

  trail_now = what ? what : "";
  if (trail_off)
    return;
  nspire_save_file(path, sizeof(path), EXIT_TRAIL_FILE);
  f = fopen(path, trail_n ? "a" : "w");   /* (this exit's first line starts the file anew) */
  if (!f)
    return;
  trail_n++;
  fprintf(f, "exit step %u at %lu ms: %s\n", trail_n,
          (unsigned long)(exiting ? ticks_ms(nspire_ticks() - exit_began) : 0), trail_now);
  fclose(f);
}

static void trail_remove(void)
{
  char path[600];

  if (trail_off)
    return;
  trail_off = 1;
  if (!trail_n)
    return;
  nspire_save_file(path, sizeof(path), EXIT_TRAIL_FILE);
  remove(path);
}

void nspire_exit_trail_check(void)
{
  char path[600], buf[4096], *line, *last = NULL;
  size_t n;
  unsigned lines = 0;
  FILE *f;

  nspire_save_file(path, sizeof(path), EXIT_TRAIL_FILE);
  f = fopen(path, "r");
  if (!f)
    return;
  /* (the file's end: its last line is the step the last run never left) */
  if (fseek(f, 0, SEEK_END) == 0)
  {
    long size = ftell(f);
    if (size > (long)sizeof(buf) - 1)
      fseek(f, size - (long)sizeof(buf) + 1, SEEK_SET);
    else
      fseek(f, 0, SEEK_SET);
  }
  n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  for (line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
    if (line[0])
    {
      last = line;
      lines++;
    }
  remove(path);
  snprintf(trail_last, sizeof(trail_last), "the last run's exit never reached the hand-back; its trail's last line "
           "(%u read): \"%s\"", lines, last ? last : "(empty)");
  nspire_link_log("exit trail: %s", trail_last);
}

const char *nspire_exit_trail_last_run(void)
{
  return trail_last;
}

/* ---- R11w (2026-11-15): the USB shutdowns, bounded (nspire_exit.h) ---- */

static u32 usb_began, guard_calls;
static int usb_on, usb_failed, guard_in;

/* (r19-usb) every OS sleep while the USB shutdowns run (nspire_os_sleep_guard) */
static void usb_sleep_guard(void)
{
  static char why[200];
  u32 ms;

  if (!usb_on || usb_failed || guard_in)
    return;
  guard_calls++;
  ms = ticks_ms(nspire_ticks() - usb_began);
  if (nspire_timer_runs() ? ms < EXIT_USB_MS + EXIT_USB_GRACE_MS : guard_calls < EXIT_USB_SLEEPS_MAX)
    return;
  guard_in = 1;
  snprintf(why, sizeof(why), "%s: a wait in it still sleeping %lu ms into the USB shutdowns (%lu OS sleeps): it "
           "would never end", trail_now[0] ? trail_now : "a USB shutdown", (unsigned long)ms, (unsigned long)guard_calls);
  nspire_exit_usb_timed_out(why);
  guard_in = 0;   /* (the PC's restart returns: the rest of the exit as on a timed-out step) */
}

void nspire_exit_usb_begin(void)
{
  u32 rate;

  if (usb_on)
    return;
  usb_on = 1;
  usb_began = nspire_ticks();
  guard_calls = 0;
  nspire_os_sleep_guard = usb_sleep_guard;
  rate = nspire_os_backstop_arm(EXIT_BACKSTOP_MS);
  nspire_padlog_exit_mark(PL_EXIT_USB_BEGIN, rate);
  nspire_link_log("exit: the USB shutdowns begin (%u ms for all of them; the watchdog backstop %s, %lu counts a ms)",
                  EXIT_USB_MS, rate ? "armed for 12 s" : "not armed: it doesn't count", (unsigned long)rate);
}

unsigned nspire_exit_usb_left_ms(void)
{
  u32 ms;

  if (!usb_on)
    return EXIT_USB_MS;
  ms = ticks_ms(nspire_ticks() - usb_began);
  return ms >= EXIT_USB_MS ? 0 : EXIT_USB_MS - ms;
}

void nspire_exit_usb_end(void)
{
  if (!usb_on)
    return;
  usb_on = 0;
  nspire_os_sleep_guard = NULL;
  nspire_os_backstop_disarm();
  nspire_padlog_exit_mark(PL_EXIT_USB_DONE, ticks_ms(nspire_ticks() - usb_began));
  nspire_link_log("exit: the USB shutdowns done in %lu ms; the watchdog backstop off",
                  (unsigned long)ticks_ms(nspire_ticks() - usb_began));
}

/* The plain message, over whatever gpSP's screen shows (the LCD is still
 * gpSP's in quit(); after nspire_restore it shows nothing, and the restart
 * comes all the same). */
static void restart_message(void)
{
  ui_clip(0, UI_W);
  ui_rect(20, 84, UI_W - 40, 72, UI_RAISED);
  ui_text_center(UI_W / 2, 110, EXIT_USB_TEXT1, &ui_font_small_strong, UI_TEXT);
  ui_text_center(UI_W / 2, 134, EXIT_USB_TEXT2, &ui_font_small_strong, UI_TEXT);
  ui_present();
  nspire_osd_message("%s %s", EXIT_USB_TEXT1, EXIT_USB_TEXT2);
}

/* EXIT_MESSAGE_MS on gpSP's timer, spinning: no OS sleep (the OS's USB task
 * is in a state nothing finished; nspire_wait_until would sleep into it
 * while a driver wants the OS, and waits a second at most); without a
 * timer, by the real-time clock (3 second changes). */
static void message_wait(void)
{
  u32 t0 = nspire_ticks(), r0 = nspire_rtc_seconds(), n, want = (u32)((u64)EXIT_MESSAGE_MS * NSPIRE_TICK_HZ / 1000);

  for (n = 0; n < 400000000u; n++)
  {
    u32 t = nspire_ticks();
    if (t - t0 >= want)
      break;
    if (t == t0 && nspire_rtc_seconds() - r0 >= 3)
      break;
  }
}

void nspire_bt_glue_write_log(void) __attribute__((weak));

void nspire_exit_usb_timed_out(const char *step)
{
  u32 ms = usb_on ? ticks_ms(nspire_ticks() - usb_began) : 0;

  if (usb_failed)
    return;
  usb_failed = 1;
  nspire_os_sleep_guard = NULL;   /* (its own sleeps below are no step's) */
  {
    static char line[300];
    snprintf(line, sizeof(line), "timed out at %s: the restart", step);
    nspire_exit_trail(line);
  }
  nspire_exit_step(step);
  nspire_exit_step("USB shutdown timed out: restart");
  /* (the files are written and closed before any USB shutdown begins:
   * these are the only writes after one, and they finish before the
   * restart) */
  nspire_link_log("exit: USB shutdown timed out at %s (%lu ms into the USB shutdowns)", step, (unsigned long)ms);
  nspire_link_log_flush();
  nspire_padlog_exit_timeout(step, ms);
  /* (and Bluetooth's: its log is otherwise written after the USB shutdowns) */
  if (nspire_bt_glue_write_log)
    nspire_bt_glue_write_log();
  restart_message();
  message_wait();
  nspire_os_restart();
}

/* quit() no longer waits for the keys (R6 did, unbounded): the line stays
 * off until the hand-back, so a key held there reaches nothing. Here, a
 * short wait (the timer is still gpSP's, nothing sleeps into the OS): a key
 * let go now never reaches the OS; one held on longer is waited for by
 * Ndless itself after gpSP returns (ploaderhook.c: wait_no_key_pressed with
 * the CPU's interrupts off, before the OS's keypad handler can run), so
 * the OS sees it only as up. */
#define RELEASE_MS   1500
#define RELEASE_QUIET (NSPIRE_TICK_HZ / 20)   /* 50 ms with nothing down (a bounce) */

static int wait_release(void)
{
  u32 start = nspire_ticks(), since = start, now;
  unsigned polls = 0;

  for (;;)
  {
    /* A short wait each turn (as R6's: 2 ms), by gpSP's timer (with none
     * running, nspire_wait_until moves gpSP's count on by itself). The
     * OS's keypad line is still off. */
    nspire_wait_until(nspire_ticks() + NSPIRE_TICK_HZ / 500);
    now = nspire_ticks();
    if (keys_raw_down())
      since = now;
    else if (now - since >= RELEASE_QUIET)
      return 1;
    if (now - start >= RELEASE_MS * (NSPIRE_TICK_HZ / 1000) || ++polls >= RELEASE_MS)
      return 0;
  }
}

static void hand_back(void)
{
  /* F3: five picture lines (~2.6 KB) now: part holds them all */
  static char text[7600];
  static char part[4200];   /* (fast17: and the start's two step lines) */
  int released, n;
  u32 t_marker, t_back;

  nspire_exit_begin();   /* an exit() from elsewhere (an error, main's return) */
  nspire_exit_step("gpSP's other exit handlers (pad driver)");
  nspire_exit_trail("the hand-back: the keys let go");
  released = wait_release();
  nspire_exit_step(released ? "keys let go" : "keys still down (Ndless waits for them)");

  /* The marker's lines: the steps, the hardware, an older CX's timer. */
  nspire_exit_steps_text(text, sizeof(text));
  n = (int)strlen(text);
  nspire_os_hand_back_text(part, sizeof(part));
  if ((unsigned)n < sizeof(text))
    n += snprintf(text + n, sizeof(text) - n, "\n%s\n", part);
  /* QUIT6: what the OS will be interrupted by once it has the CPU back
   * (the timer's line below says whether gpSP's count passed zero) */
  nspire_os_irq_text(part, sizeof(part), "before the put-backs");
  if (n >= 0 && (unsigned)n < sizeof(text))
    n += snprintf(text + n, sizeof(text) - n, "%s\n", part);
  if (n >= 0 && (unsigned)n < sizeof(text))
    n += snprintf(text + n, sizeof(text) - n, "after the hand-back: %s\n",
                  released ? "gpsp_exit_after.txt.tns follows" : "no file (a key was still down)");
  nspire_timer_log_text(part, sizeof(part));
  if (n >= 0 && (unsigned)n < sizeof(text))
    n += snprintf(text + n, sizeof(text) - n, "%s", part);
  /* 2026-11-11: the picture's lines (the LCD, at the start, after the last
   * menu, at the quit: nspire_host_main.c) */
  nspire_picture_log_text(part, sizeof(part));
  if (n >= 0 && (unsigned)n < sizeof(text))
    snprintf(text + n, sizeof(text) - n, "%s", part);
  nspire_exit_trail("the hand-back: the marker and the link log's last lines");
  t_marker = nspire_ticks();
  nspire_link_exit_close(text);
  /* (r19-usb-exit) the marker's own write and the link log's last lines,
   * for the file below: the last of gpSP's time that can be timed (the
   * timer is the OS's after the next line) */
  t_back = nspire_ticks();
  /* (r19-usb) the marker says clean: the trail goes (a freeze after this
   * is the hand-back's register writes or the file below) */
  trail_remove();

  /* Last: the OS's timer, then its keypad, keypad controller and cursor.
   * Register writes; after them only EXIT-R4's file, with every key up. */
  nspire_timer_hand_back();
  nspire_os_hand_back();

  /* EXIT-R4: whether the hand-back took, in a file that outlives a reset
   * (keys dead after quitting force one, and the OS's boot sets the
   * interrupt controller up again, so the next start's words can't tell).
   * Only with every key up: the file call lets the OS run, its keypad line
   * on now, and a key down would reach it. If this file is missing next
   * time while the marker says it follows, the main task stopped here (in
   * the OS's file call), not in gpSP. */
  if (released)
  {
    nspire_os_after_hand_back_text(part, sizeof(part));
    /* R11t: the clock select after the put-back (register reads only) */
    n = (int)strlen(part);
    if ((unsigned)n + 2 < sizeof(part))
    {
      part[n++] = '\n';
      nspire_timer_clock_text(part + n, sizeof(part) - n);
    }
    /* QUIT6: and the interrupt lines the OS gets (register reads only) */
    n = (int)strlen(part);
    if ((unsigned)n + 2 < sizeof(part))
    {
      part[n++] = '\n';
      nspire_os_irq_text(part + n, sizeof(part) - n, "after the hand-back");
    }
    /* (r19-usb-exit) the time after the marker's steps */
    n = (int)strlen(part);
    if ((unsigned)n + 2 < sizeof(part))
      snprintf(part + n, sizeof(part) - n,
               "\nafter the marker's steps, ms: the marker and the link log's last lines written %lu; gpSP's exit, "
               "its first step to the hand-back, %lu ms in all (after this file: Ndless's unload and the OS's "
               "redraw, which gpSP can't time)",
               (unsigned long)ticks_ms(t_back - t_marker),
               (unsigned long)(exiting ? ticks_ms(t_back - exit_began) : 0));
    nspire_link_exit_after(part);
  }
}

void nspire_exit_start(void)
{
  if (started)
    return;
  started = 1;
  nspire_os_hw_note_start();
  nspire_os_keypad_irq(0);
  /* First registered, so it runs last of gpSP's exit handlers: after the
   * pad driver's (which lets the OS run). */
  atexit(hand_back);
}
