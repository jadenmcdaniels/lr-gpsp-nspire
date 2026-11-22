/* nspire-usbpad's API (usbpad.h) on the PC, for gpsp-host: a simulated
 * pair of USB gamepad slots that the script plugs in and out, and whose
 * buttons are the script's pad keys (host_hw.c: the fifth key word,
 * "hold Pad_A", "press Pad_Up"... the bits of usbpad.h). Both player
 * slots, when plugged, read those same buttons. The driver itself is
 * nspire-usbpad's (its own tests); this checks gpSP's glue
 * (nspire_usbpad_glue.c) and keys layer.
 *
 * Script lines (host_hw.c passes them here):
 *   pad in [KIND]    a pad plugs in, into the first free slot (KIND: xinput,
 *                    the default, gip, ds4, dualsense, switchpro or hid)
 *   pad out [N]      player N's pad (1, the default, or 2) comes out
 *   pad out drive    the flash drive comes out
 *   pad hub in|quiet|preplug|out  a USB hub on the adapter: offered to the
 *                    driver at once (in), never (quiet), only after a bus power
 *                    restart (preplug: there before the driver registered); out:
 *                    it and everything on it leave. While it is in, "pad in",
 *                    "pad out" and the like plug into and pull out of the hub:
 *                    the port's registers stay as they are
 *   pad in sound / pad out sound  a USB sound adapter (its driver's claim)
 *   pad wedge on|off the OS's USB has stopped: a "pad in KIND" from then is
 *                    enabled with its connect change never taken (PORTSC
 *                    0000000f) and never offered ("pad unplug" takes it out);
 *                    off: it works again (the device on the bus is offered)
 *   pad image out|in the fake drive with files (GPSP_USB_IMAGE) comes out or
 *                    goes back in, the port's registers unchanged (a hub port)
 *   pad in drive / pad preplug drive  a USB flash drive (S9): offered at once to
 *                    its driver (nspire_usbms_active says 1, the arbiter's
 *                    OTHER), or on the bus from t0 and not offered (the
 *                    driver registered after it, like a pad) until a bus
 *                    power cycle re-offers it
 *   pad event KIND [PAD]  the driver says replug, power or unsupported
 *                    (about PAD's USB id, a KIND of "pad in"; else 2563:0575)
 *   pad stat         log the cost counters (OS sleeps, the observer's samples,
 *                    the port's register reads) and the time, for S8's budgets
 *   pad want on|off  the driver asks for OS time at each poll, or not (it
 *                    does anyway once in 30 polls while a pad is in, as
 *                    nspire-usbpad's backend does: USBPAD_OS_LIVE_EVERY)
 *   pad port N       the USB port (host_usbpad_port, host_link.c reads it):
 *                    0 nothing, 1 an OTG adapter (the host's end), 2 the
 *                    device's end (a computer), -1 a CX's controller with
 *                    its bus access off ("can't tell"), -2 (r19-usb) a CX II's
 *                    controller its OS has powered down (it reads nothing; a
 *                    CX II's idle port goes -2 and 0 in turn), 3 and 4 a CX's
 *                    (ChipIdea) host's and device's end. "pad in" puts 1 on
 *                    an empty port, and the last "pad out" takes it off.
 *   pad hcstop [KIND]  (PAD3) a device on the bus the OS never starts its host
 *                    controller for (USBCMD stopped, PORTSC 00000800: the
 *                    pull-up on the wires, never connected), until a bus power
 *                    cycle that works (pad cycle); never offered before it
 *   pad slowhc ticks|wall HC ENUM [KIND]  (PAD4) a device the OS starts
 *                    late, as the R10a CX II's: connected (PORTSC 00000803)
 *                    with the host controller stopped (USBCMD 00080b00) until
 *                    the OS has had HC of its own ticks ("ticks": counted as
 *                    on the calculator, host_platform.c host_os_ticks: 2 for
 *                    a sleep after a tick or more of gpSP's own time, 1 for a
 *                    sleep right after one) or HC seconds of wall time
 *                    ("wall"); then the port in reset (PORTSC 00000103, the
 *                    controller running) for ENUM more, then enabled
 *                    (00000005) and offered. HC 0: the controller runs from
 *                    the start (the hub's debounce and reset only). A bus
 *                    power restart sets the OS back to the start of it (a
 *                    restart during its enumeration); "pad unplug" takes it out
 *   pad unplug       (PAD3) that device (or a preplugged one) comes out
 *   pad halt N       (R10b) R10a 685064: the OS halts its host controller with
 *                    the device on the bus (USBCMD 00080000, PORTSC 0000088d)
 *                    and runs it again after N more of its ticks (0 never)
 *   pad event unsupported KIND STEP EP ERR  (R10b, API 6) KIND's bring-up
 *                    failed at STEP (a usbpad_step_name word), endpoint EP
 *                    (hex), error ERR, with its trace records; a quiet port
 *   pad trace N      (R10b) N routine OUT trace records of the pad in slot 1
 *   pad preplug [KIND]  a device on the bus since t0 that the OS never offers
 *                    to the driver (usbshim.h:16-18: plugged before the
 *                    driver registered): the port is 1, no match, no attach
 *   pad resident [KIND]  a device the OS offers at register time (usbpad_init;
 *                    at once when the driver already runs)
 *   pad cycle ok|ignored|late N  what a controller-side bus power cycle does
 *                    (power off, then on): ok = the device leaves, and 300 ms
 *                    after power returns it is offered to the registered
 *                    driver; ignored = it stays gone; late = ok only on the
 *                    N-th cycle (the first N-1 are ignored). Default ok.
 *   pad buspower on|off  drive the bus power by hand, as an A_BUS_DROP/REQ or
 *                    ChipIdea PP write does (host_usb_reg_write)
 *   pad direct       the device on the bus can be read by a simulated
 *                    descriptor read (host_usbpad_direct_read), offered or not
 *   pad readdesc     do that read now; the result goes to pad.txt
 *   pad reg ADDR [V] read, or write V to, a USB register (host_usb_reg_*)
 *   pad calc node    a NavNet node is listed from t0 and our service never answers
 *   pad calc preplug the other calculator's NavNet node is listed from t0,
 *                    no callback needed (host_link.c link_cable_watch)
 *   pad calc out|in  pull the cable (the link's sends and receives fail, the port
 *                    reads as the pad's again) or put it back
 *   pad calc end device|host  (DETECT) this side has the cable's end (the port
 *                    reads DEVICE_END) or the adapter's (the default)
 *   pad calc delay S (SEAM-2) the other calculator's node is listed S seconds
 *                    after the cable came in, seen only at a look (every 1.5 s);
 *                    at this (the host's) end the cable is a connected device on
 *                    the registers, and a power cycle that drops its bus logs
 *                    "calc: bus power dropped" (it must never)
 *   pad sync PREFIX N  (S9) two gpsp-host processes meet: write PREFIX.N, wait in
 *                    real time for PREFIX.(3-N); for the calculator rows, whose
 *                    two sides run free of each other while no link is up
 *                    (only used with the cable out: a linked side waits on the
 *                    other's frames)
 *   pad diag M A E   usbpad_diag's counters: the OS offered M matches and A
 *                    attaches, the last failed request was E
 *   pad error N [S]  N IN errors with status S (default 17, STALLED) from the
 *                    pad (the driver releases its buttons: here, the slot's)
 *   pad synth        the OS's config descriptor was not the whole set
 *   pad noretire     (R11w) after the shutdown a callback stays inside gpSP for
 *                    good: the exit's bounded wait runs out and it restarts
 *   pad slowexit     the shutdown waits for a blocked attach (the wait hook
 *                    runs once)
 *   pad dropped N    the ring has lost N reports
 *   pad hub blip [MS]  (2026-11-18, Hub 5) the bus power falls MS ms (100)
 *                    under the hub (OTGCSR's A_VBUS off, OTGISR 00000420) and
 *                    comes back with the port still connected and enabled:
 *                    the hub browned out (the reference r16 run). From then on
 *                    nothing on it answers (no reports, the sound stalls),
 *                    the OS's explore flag, once set, stays set (its explore
 *                    times out on the hub), what is plugged into it is not
 *                    seen, until the port's power is restarted: then the hub
 *                    and everything that was on it (or plugged in since) is
 *                    offered again
 *   pad hub dip [MS]  the same fall of the power, which the hub rides out:
 *                    everything on it goes on working
 *   pad load heavy on|off  (r19-usb) the adapter's power turned on and held
 *                    down by what is on it (A_BUS_REQ set, no VBUS), until off
 *   pad exit hang drive|bluetooth  (r19-usb) at the exit that driver's stop
 *                    never ends (sleeping): a callback that never returns, a
 *                    dongle gone silent
 *   pad reports silent on|off  (r19-usb) the controllers attached send nothing
 *                    (no report, no button) until off
 *   pad hub stale    (r19-usb) the hub's ports read as TI keeps them, the
 *                    hub's last answer until its explore reads them again: a
 *                    controller pulled from the hub leaves its port "p!"
 *                    (powered, not connected, a change) until the next explore
 *
 * OUTDIR/pad.txt gets a line for each of these, each message over the
 * picture (nspire_osd_message, wrapped: gpSP's, the glue's toasts too),
 * the settings the glue gave the driver, OS sleeps, and at exit
 * usbpad_shutdown's polls, the shortest time between two of them and the
 * most polls in one second (of virtual time). */

#include <stdarg.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nspire_audio.h"
#include "nspire_os.h"

double host_hw_now(void);              /* virtual time, in seconds */
const char *host_hw_out_dir(void);
uint32_t host_hw_pad_keys(void);       /* the fifth key word */
uint32_t nspire_os_sleeps(void);

int host_usbpad_port;
extern long host_usb_plugged_reads, host_usb_bits_reads;
uint32_t nspire_usbport_samples(void);
static int port_by_pad;   /* "pad in" put the adapter there */

static void pad_log(const char *format, ...)
{
  char path[4200];
  va_list ap;
  FILE *f;

  snprintf(path, sizeof(path), "%s/pad.txt", host_hw_out_dir());
  f = fopen(path, "a");
  if (!f)
    return;
  fprintf(f, "%.3f ", host_hw_now());
  va_start(ap, format);
  vfprintf(f, format, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}

/* Every message over the picture, into pad.txt too. */
void __real_nspire_osd_message(const char *format, ...);

void __wrap_nspire_osd_message(const char *format, ...)
{
  char text[256];
  va_list ap;

  va_start(ap, format);
  vsnprintf(text, sizeof(text), format, ap);
  va_end(ap);
  pad_log("osd %s", text);
  __real_nspire_osd_message("%s", text);
}

void host_usbpad_note(const char *text)
{
  pad_log("%s", text);
}

/* DETECT: a calculator on the cable at this (the host's) end is offered to
 * the registered drivers by the OS, ours among them (r5's CX II: "the OS
 * called the driver's match" 215 ms after its port came up; our match
 * turns it down): the diag's matches move. */
static long diag_matches;
static void ring_offer(uint16_t vid, uint16_t pid, int iface, uint8_t cls, uint8_t sub, uint8_t proto, int score);
void host_usbpad_os_offer(void)
{
  diag_matches++;
  ring_offer(0x0451, 0xe012, 0, 0, 0, 0, -2);   /* USBPAD_OS_NO_TI: the other calculator: refused, TI's */
  pad_log("calc: offered to the drivers (match)");
}

/* S9: a flash drive the OS offered to its driver (the mass storage driver is
 * another branch's: this stands in for its nspire_usbms_active). */
static int drive_claimed;
int host_usbms_active(void)
{
  return drive_claimed;
}

/* Hub (2026-11-16): a USB hub on the adapter ("pad hub in|quiet|preplug|out").
 * The port's registers read connected, enabled and running for as long as
 * it is in, whatever is plugged into or pulled out of it ("pad in KIND",
 * "pad out", "pad in sound", "pad in drive"...). hub_offered: the OS
 * offered it to the driver (a class 9 device-level offer; "quiet": never,
 * as an OS that keeps its hubs to itself; "preplug": on the bus before the
 * driver registered, offered after a bus power restart). hub_drops: the
 * times a power restart cut it (after its offer it must never be). And a
 * USB sound adapter's claim ("pad in sound": gpSP's audio driver is not in
 * the PC build; nspire_audio.c's stub asks here). */
static int hub_in, hub_offered, hub_quiet, hub_drops, sound_claimed;
/* "pad wedge on|off": the OS's USB has stopped, as four hardware sessions
 * ended (the pad glue's watch_stuck): whatever is plugged in ("pad in KIND")
 * is reset and enabled with the port's connect change never taken (PORTSC
 * 0000000f) and never offered; "off": the OTG adapter came out and went
 * back in, the OS's USB works again (what is on the bus is offered). */
static int wedged;
/* (2026-11-17) "pad sleepy on": TI's OS as its code reads (nspire-usbshim
 * usbshim_awake.h; three hardware logs). What is on the bus with nothing
 * active on it (no controller attached, no sound adapter or drive claimed:
 * a hub alone, a device no driver took) is suspended SLEEP_S after it was
 * last active: the controller stopped and the port suspended (USBCMD
 * 00080000, PORTSC 0000088d), the transceiver's own suspend bit set
 * (register 0x40 bit 8: phy_asleep). A hub asleep has no status pipe: what
 * is plugged into it is not seen. When the suspended device leaves (pulled
 * out, or the bus power dropped) the controller runs again and nothing
 * clears the transceiver's bit: whatever comes next is reset on a sleeping
 * transceiver, shows connected and never enabled (PORTSC 00000889) and is
 * never offered (stuck_dev), until the bit is cleared by a write of the
 * register AND the port is powered anew after that, or the OTG adapter
 * itself comes out ("pad port 0": the controller set up anew).
 * usbpad_os_awake(1) (the three changes of the OS's code) keeps all of it
 * from happening; "pad awake unknown": an OS build whose code is not the
 * known one (the changes are refused, the OS sleeps as above). "pad
 * asleep": the bus is suspended now (what the OS did before gpSP started,
 * or before its changes were in). */
#define SLEEP_S 1.4
static int sleepy, awake_known = 1, bus_asleep, phy_asleep, stuck_dev;
static unsigned awake_bits, awake_puts, sleep_count, deaf_plugs, dead_plugs, phy_wakes;
static double idle_since = -1;
static uint32_t misc_other = 0x0du;   /* the register's other bits: a wake must leave them */
/* (2026-11-17, final/usb7) "pad hubslow N": what is plugged into the hub
 * ("pad in KIND") is offered only once the OS's USB task has run N times
 * since (OS sleeps: the hub's port reset, the address, the descriptors),
 * and TI's "a hub has news" flag (usbpad_os_explore_pending) is set from
 * the plug until that task's first run; "pad explore unknown": an OS whose
 * code is not the known one (the flag can't be read). "pad sound serves":
 * a sound adapter that streams gives the OS its task time itself, one
 * sleep every NSPIRE_AUDIO_HUB_MS (1000 ms when the OS's explore flag is
 * known, NSPIRE_AUDIO_HUB_FALLBACK_MS = 250 ms otherwise or while Bluetooth
 * wants the task; nspire_audio.c task_ms with a hub in,
 * nspire_audio_serves_os says so and the glue's own hub tick is off). */
static int hub_slow, pend_kind = -1, explore_flag, explore_known = 1, explore_stuck, sound_serves;
/* (2026-11-17, final/usb8) "pad hub deaf": the hub's status pipe says
 * nothing any more (the reference hub after a controller's unplug): what is
 * plugged into the hub is neither news nor offered, until the OS is asked
 * to explore (usbpad_os_hub_explore: its task asks every port itself);
 * then the device shows on its port (connected, no device yet) and is
 * offered after hub_slow (3 at the least) runs of the OS's USB task.
 * "pad hub overcurrent on|off": a port of the hub reports over-current.
 * "pad hub stuckport on|off": a port stays connected with no device made
 * of it, whatever the OS is given. usbpad_os_hub_read tells the ports as
 * that "OS" last saw them: 1 sound, 2 drive, 3 a controller, 4 what is on
 * its way or stuck. */
static int hub_deaf, deaf_kind = -1, hub_overcur, hub_stuckport, hub_explores, port4_seen;
/* (r19-usb) "pad hub stale": the ports as TI's hub driver keeps them, the
 * hub's last answer (wPortStatus, wPortChange: usbshim_hub.h), refreshed
 * only when its explore task reads the port again: a controller pulled from
 * the hub leaves its port "p!" (powered, not connected, a change) from the
 * explore that saw the pull (its detach) until the next one (r19's CX II:
 * 131433-141764 and 253260-263650, each the first look the glue asked after
 * its own 8 s wait). port3_stale: that state on the controller's port. */
static int hub_stale, port3_stale;
static int reports_onchange;
/* (r19-usb, the CX II's night freeze) "pad exit hang drive|bluetooth": at
 * the exit, that driver's stop waits for something that never comes,
 * sleeping a tick at a time as the real ones do: the drive's
 * (usbms_nspire.c usbms_platform_stop: "for (;;) { if (!callbacks) break;
 * sleep_tick(); }", a callback that never returns), Bluetooth's (a dongle
 * gone silent: its stop waits for the controller's answers). The two
 * functions are wrapped (the Makefile's HOST_WRAP). */
static int exit_hang_drive, exit_hang_bt;
/* (r19-usb) "pad load heavy on|off" (host_usb_reg_read) */
static int load_heavy;
void __real_usbms_platform_stop(void);
void __wrap_usbms_platform_stop(void)
{
  if (exit_hang_drive)
  {
    pad_log("exit hang: the drive's stop waits for a callback that never returns");
    for (;;)
      nspire_os_sleep();
  }
  __real_usbms_platform_stop();
}
int __real_nspire_bt_stop_within(unsigned ms);
int __wrap_nspire_bt_stop_within(unsigned ms)
{
  if (exit_hang_bt)
  {
    pad_log("exit hang: Bluetooth's stop waits for a dongle that went silent");
    for (;;)
      nspire_os_sleep();
  }
  return __real_nspire_bt_stop_within(ms);
}
/* (r19-usb) "pad reports silent on|off": the controllers attached send
 * nothing (r19's CX: an 8BitDo attached with its OUT pipe's open timed out,
 * 0 reports in 23 s): no report, no button, until off */
static int reports_silent;
static uint32_t onchange_last[8];
/* (2026-11-18) "pad hub blip": hub_dead from the fall on until the port's
 * power is restarted; what was on the hub then (kinds[] indexes), offered
 * again after the restart */
static int hub_dead, hub_restarts, hub_keep[8], hub_keep_n, hub_restarting, hub_image, hub_probes;
static int hub_stall, hub_gen_model;   /* the probe stalled (an answer); the hub's offers, a generation each */
static double hub_late;      /* "pad hub late S": the hub back S s after a restart (0: REOFFER_S) */
static uint32_t sound_completions;
static double blip_until = -1;
/* (2026-11-17) "pad reports byirq": as on the calculator, a controller's
 * reports reach the driver only where the USB controller's interrupt is
 * taken: in an OS sleep, or in a moment the CPU's interrupts are unmasked
 * (nspire_irqs_unmask -> host_os_irq_window -> here). Between two of those
 * the controller's state and its report count stand still. "pad reports
 * nowindows": an OS on which the completions do not run in the interrupt:
 * only OS sleeps bring them. */
static int reports_byirq, reports_nowindows;
static unsigned irq_windows, irq_windows_seen;
static uint32_t irq_sleeps_seen;
static void deliver_served(void);
static void deliver_reports(void);
static void probe_update(double now);
void host_usbpad_irq_window(void)
{
  irq_windows++;
  if (reports_byirq)
    deliver_served();   /* the completions run in the interrupt itself */
}
static uint32_t pend_sleeps0;
static double serve_slept_at = -1;
int host_audio_serves_os(void)
{
  return sound_serves && sound_claimed;
}
int host_usbaudio_active(void)
{
  return sound_claimed;
}
int host_usbaudio_stalled(void)
{
  return sound_claimed ? hub_dead : -1;
}
uint32_t host_usbaudio_completions(void)
{
  return sound_completions;
}
/* "pad sound idle on|off" (nspire-usbaudio ua17's idle mode: the game's sound
 * on Bluetooth headphones): the card healthy and streaming nothing, so no
 * completions at all */
static int sound_idle;
int host_usbaudio_idle(void)
{
  return sound_claimed && sound_idle;
}

/* nspire-usbms's fake drive (tests/fake_drive.c), when the build has it. */
int fake_plug(const char *image) __attribute__((weak));
void fake_unplug(void) __attribute__((weak));

int nspire_os_sleep(void);
void nspire_link_log(const char *fmt, ...);
uint32_t host_usb_reg_read(uint32_t addr);
void host_usb_reg_write(uint32_t addr, uint32_t value);

/* nspire_usbcycle.c's power cycle runs as on the calculator, on the
 * registers below (host_usb_reg_*); each one that did something goes into
 * pad.txt too (the link log is 12 KB). */
int __real_nspire_usb_reenumerate(int attempt);

int __wrap_nspire_usb_reenumerate(int attempt)
{
  int r = __real_nspire_usb_reenumerate(attempt);

  if (r >= 0)
    pad_log("reenum cycle %d", attempt);
  return r;
}

#ifdef NSPIRE_NO_USBPAD

uint32_t host_usb_reg_read(uint32_t addr)
{
  (void)addr;
  return 0;
}

void host_usb_reg_write(uint32_t addr, uint32_t value)
{
  (void)addr;
  (void)value;
}

int host_usbpad_direct_read(uint8_t *buf, unsigned size)
{
  (void)buf;
  (void)size;
  return -1;
}

void host_usbpad_script(const char *line)
{
  fprintf(stderr, "gpsp-host: built without nspire-usbpad: %s\n", line);
  exit(2);
}

#else

#include "usbpad.h"
#include "src/backend_os.h"

#ifndef USBPAD_API_LEVEL
#error "this stand-in is for nspire-usbpad's API level 3 (USBPAD_DIR: nspire-usbpad, after shim-callbacks)"
#endif
#if USBPAD_API_LEVEL < 3
#error "this stand-in needs USBPAD_EV_FAILED (nspire-usbpad API level 3)"
#endif

static const struct { const char *name; uint8_t kind; uint16_t vid, pid; } kinds[] =
{
  { "xinput", USBPAD_KIND_XINPUT, 0x045e, 0x028e }, { "gip", USBPAD_KIND_GIP, 0x045e, 0x02ea },
  { "ds4", USBPAD_KIND_DS4, 0x054c, 0x09cc }, { "dualsense", USBPAD_KIND_DUALSENSE, 0x054c, 0x0ce6 },
  { "switchpro", USBPAD_KIND_SWITCHPRO, 0x057e, 0x2009 }, { "hid", USBPAD_KIND_HID, 0x0079, 0x0006 },
  { "drive", USBPAD_KIND_NONE, 0x0781, 0x5567 },   /* S9: a flash drive (class 8), not a pad */
  { "sound", USBPAD_KIND_NONE, 0x0d8c, 0x0014 },   /* hub: a USB sound adapter (class 1), not a pad */
};
static const char *const kind_names[USBPAD_KIND_COUNT] =
  { "USB device", "USB gamepad", "Xbox 360", "Xbox One", "DualShock 4", "DualSense", "Switch Pro" };

static int running, want;
static struct usbpad_os_info info;
/* PAD3: each offer into the driver's match ring (usbpad_os_info()->match),
 * as the real driver keeps it: vid:pid, interface or device level, the
 * class, the answer. */
static void ring_offer(uint16_t vid, uint16_t pid, int iface, uint8_t cls, uint8_t sub, uint8_t proto, int score)
{
  struct usbpad_os_match *m = &info.match[info.match_n % USBPAD_OS_MATCH_RING];

  memset(m, 0, sizeof(*m));
  m->vid = vid;
  m->pid = pid;
  m->iface = (uint8_t)iface;
  m->cls = cls;
  m->sub = sub;
  m->proto = proto;
  m->score = (int8_t)score;
  info.match_n++;
}

static struct usbpad_state slots[USBPAD_PLAYERS], empty;
static struct usbpad_event queue[8];
static unsigned queued;
static struct usbpad_config config = { 1, 50, 35 };
static struct usbpad_os_config os_config = { .want_live = 0, .replug_hint = 1 };
static void (*hook_sleep)(void);
static int (*hook_probe)(void);
static long polls, sec_polls, max_sec_polls, poll_sec = -1;
static double last_poll = -1, min_gap = 1e9;

/* What usbpad_diag says of the OS backend: the counters of what the OS
 * offered (a match callback, an attach) and the last failed request; "pad
 * diag MATCHES ATTACHES LAST_ERR" sets them (a device the OS offered, one
 * that failed to start), "in" counts a match and an attach. */
static long diag_attaches, diag_detaches, diag_last_err;
static void (*hook_waiting)(void);
static int slow_exit;
/* R11w: "pad noretire": a callback stays inside gpSP after the shutdown, for
 * good (usbpad_os_shutdown_pending says 1 from the shutdown on) */
static int no_retire, shut_down;
static uint32_t dropped;

/* The bus: the device physically on it (bus_kind, an index of kinds[], -1
 * none), whether it is offered to the driver, the bus power the controller
 * gives it, and a power cycle's outcome (cycle_mode, cycles). */
#define CYCLE_OK       0
#define CYCLE_IGNORED  1
#define CYCLE_LATE     2
#define REOFFER_S      0.3
static int bus_kind = -1, bus_offered, bus_power = 1, direct_ok;
static int cycle_mode = CYCLE_OK, cycle_late_n, cycles, pending_resident = -1;
static double offer_at = -1;
static uint32_t fotg_req_drop;   /* OTGCSR bits 4 (A_BUS_REQ) and 5 (A_BUS_DROP) */
static uint32_t ci_portsc_pp = 1u << 12;
/* PAD3 "pad hcstop [KIND]": the OS never starts its host controller for the
 * device on the bus (the R9F CX II: USBCMD stopped, PORTSC 00000800, the
 * device's pull-up on the wires and never "connected"), until a bus power
 * cycle the cycle mode lets work (then it is offered as after any cycle). */
static int hc_stopped;
/* PAD4 "pad slowhc": the OS's own work before it offers the device, in its
 * ticks (slow_mode 1) or in wall seconds (2): the host controller's start
 * (slow_hc), then the port's reset and the descriptors (slow_enum). Phase 0
 * stopped, 1 in reset, 2 offered. */
extern double host_os_ticks;
static int slow_mode, slow_phase;
static double slow_hc, slow_enum, slow_from;
/* R10b "pad halt N": R10a 685064, the OS halts its host controller with the
 * device still on the bus (USBCMD 00080000, PORTSC 0000088d: connected,
 * enabled, suspended); it runs again after N more OS ticks (0: never, until
 * the device comes out). */
static int halt_on;
static double halt_until;

static int slots_in(void);
static int bus_present(void)
{
  return (hub_in ? hub_offered : bus_kind >= 0 && bus_offered) && bus_power &&
         (host_usbpad_port == 1 || host_usbpad_port == 3);
}

/* The OS's idle suspend (each look at the registers, each poll). */
static void sleep_update(void)
{
  if (sleepy && !(host_usbpad_port == 1 || host_usbpad_port == 3))
  {
    /* the OTG adapter is out: the controller is set up anew at its return */
    if (bus_asleep || phy_asleep || stuck_dev)
      pad_log("sleep: the adapter is out: the controller is set up anew");
    bus_asleep = phy_asleep = stuck_dev = 0;
    idle_since = -1;
    return;
  }
  if (!sleepy || awake_bits == 7u || bus_asleep || !running)
  {
    idle_since = -1;
    return;
  }
  if (!bus_present() || slots_in() || sound_claimed || drive_claimed)
  {
    idle_since = -1;
    return;
  }
  if (idle_since < 0)
    idle_since = host_hw_now();
  if (host_hw_now() - idle_since >= SLEEP_S)
  {
    bus_asleep = phy_asleep = 1;
    sleep_count++;
    idle_since = -1;
    pad_log("sleep: the OS suspended the bus, nothing active on it (%u)", sleep_count);
  }
}

/* The suspended device left: the controller runs again, the transceiver's
 * bit stays. */
static void sleep_device_left(void)
{
  if (bus_asleep)
    pad_log("sleep: the suspended device left: the transceiver stays asleep");
  bus_asleep = 0;
  stuck_dev = 0;
  idle_since = -1;
}

static void halt_update(void)
{
  if (halt_on && halt_until > 0 && host_os_ticks >= halt_until)
  {
    halt_on = 0;
    pad_log("halt: the host controller runs again (OS ticks so far %.0f)", host_os_ticks);
  }
}

/* R12 "pad usbtick N ENUM [KIND]" (slow_mode 3): the CX II's R11t run
 * (a CX II run log): the OS's host controller runs, the device
 * shows connected at once (PORTSC 0000080b: the connect change not taken)
 * and the OS resets the port N of its USB ticks after (host_hw.c
 * host_hw_os_usb_ticks: --os-usb-wait), in reset (0000010b) ENUM seconds,
 * then enabled and offered. */
double host_hw_os_usb_ticks(void);
void host_hw_adapter_in(void);
static double usbtick_plug;

static double slow_now(void)
{
  if (slow_mode == 3)
    return slow_phase == 0 ? host_hw_os_usb_ticks() : host_hw_now();
  return slow_mode == 1 ? host_os_ticks : host_hw_now();
}

static void post(uint8_t type, int player, uint8_t kind, uint16_t vid, uint16_t pid)
{
  struct usbpad_event *e;

  if (queued == 8)
  {
    memmove(queue, queue + 1, sizeof(queue[0]) * 7);
    queued--;
  }
  e = &queue[queued++];
  memset(e, 0, sizeof(*e));
  e->type = type;
  e->player = (int8_t)player;
  e->kind = kind;
  e->vid = vid;
  e->pid = pid;
  e->name = usbpad_kind_name(kind);
}

/* R10b: API level 6's bring-up trace, as the driver keeps it (two rings, a
 * seq each; here one list in time order): the stand-in's attach writes its
 * steps, "pad event unsupported|failed KIND STEP EP ERR" its failure, and
 * "pad trace N" N routine OUT records (the lights, ACKs). */
#if USBPAD_API_LEVEL >= 6
#define TRACE_N 128
static struct usbpad_trace traces[TRACE_N];
static unsigned trace_head, trace_tail;
static uint32_t trace_seq[2], polls_for_trace;

static void trace(char task, int port, uint16_t vid, uint16_t pid, int step, int ep, int err, int arg)
{
  struct usbpad_trace *t = &traces[trace_head % TRACE_N];
  int r = task == 'U' ? 0 : 1;

  if (trace_head - trace_tail == TRACE_N)
    trace_tail++;
  memset(t, 0, sizeof(*t));
  t->seq = ++trace_seq[r];
  t->poll = polls_for_trace;
  t->vid = vid;
  t->pid = pid;
  t->step = (uint8_t)step;
  t->ep = (uint8_t)ep;
  t->port = (int8_t)port;
  t->task = task;
  t->err = (int16_t)err;
  t->arg = (uint16_t)arg;
  trace_head++;
}

int usbpad_trace_next(struct usbpad_trace *t)
{
  if (trace_tail == trace_head)
    return 0;
  *t = traces[trace_tail++ % TRACE_N];
  return 1;
}

const char *usbpad_step_name(int step)
{
  static const char *const n[USBPAD_STEP_COUNT] = {
    "-", "desc", "claim", "ctl_form", "out_open", "control", "out", "out_done", "set_report", "start", "in_open",
    "live", "quiet", "refused", "gave_up", "out_stuck", "in_err", "in_stale", "detach", "close",
  };
  return step >= 0 && step < USBPAD_STEP_COUNT ? n[step] : "?";
}

int usbpad_trace_text(const struct usbpad_trace *t, char *buf, unsigned size)
{
  return snprintf(buf, size, "%c p%d %04x:%04x %s ep%02x arg %u: %s %d", t->task, t->port, t->vid, t->pid,
                  usbpad_step_name(t->step), t->ep, t->arg, t->err > 0 ? "OS" : "err", t->err);
}

int usbpad_event_detail(const struct usbpad_event *ev, char *buf, unsigned size)
{
  if ((ev->type != USBPAD_EV_UNSUPPORTED && ev->type != USBPAD_EV_FAILED) || !ev->step)
    return snprintf(buf, size, "%s", "");
  return snprintf(buf, size, "%s: %s %d (ep %02x)", usbpad_step_name(ev->step), ev->err > 0 ? "OS" : "error", ev->err,
                  ev->ep);
}
#else
static void trace(char task, int port, uint16_t vid, uint16_t pid, int step, int ep, int err, int arg)
{
  (void)task, (void)port, (void)vid, (void)pid, (void)step, (void)ep, (void)err, (void)arg;
}
#endif

static unsigned kind_index(const char *name)
{
  unsigned i;

  for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]) && name[0] && strcmp(name, kinds[i].name); i++)
    ;
  return i == sizeof(kinds) / sizeof(kinds[0]) ? 0 : i;
}

/* The OS offers the device to the driver: its match callback, then attach
 * (the pad goes into a free slot). 1 when it went in. */
static int offer(int i)
{
  int p;

  if (kinds[i].kind == USBPAD_KIND_NONE)
    p = 0;   /* the drive takes no pad slot */
  else
    for (p = 0; p < USBPAD_PLAYERS && slots[p].connected; p++)
      ;
  if (p == USBPAD_PLAYERS)
    return 0;
  if (host_usbpad_port <= 0)
  {
    host_usbpad_port = 1;
    port_by_pad = 1;
  }
  info.devices++;   /* (its device-level offer came first) */
  if (kinds[i].kind == USBPAD_KIND_NONE && !strcmp(kinds[i].name, "sound"))
  {
    sound_claimed = 1;
    pad_log("sound: the adapter taken by its driver (active), completions so far %u", (unsigned)sound_completions);
    diag_matches++;
    ring_offer(kinds[i].vid, kinds[i].pid, 1, 1, 2, 0, 0);   /* audio streaming: not a controller */
    if (!hub_in)
      bus_kind = i, bus_offered = 1;
    return 1;
  }
  if (kinds[i].kind == USBPAD_KIND_NONE)
  {
    drive_claimed = 1;
    diag_matches++;
    ring_offer(kinds[i].vid, kinds[i].pid, 1, 8, 6, 0x50, 0);   /* mass storage: not a controller */
    bus_kind = i;
    bus_offered = 1;
    return 1;
  }
  memset(&slots[p], 0, sizeof(slots[p]));
  slots[p].connected = 1;
  slots[p].kind = kinds[i].kind;
  slots[p].vid = kinds[i].vid;
  slots[p].pid = kinds[i].pid;
  diag_matches++;
  ring_offer(kinds[i].vid, kinds[i].pid, 1, kinds[i].kind == USBPAD_KIND_XINPUT ? 0xff : 3,
             kinds[i].kind == USBPAD_KIND_XINPUT ? 0x5d : 0, kinds[i].kind == USBPAD_KIND_XINPUT ? 1 : 0, 5);
  diag_attaches++;
#if USBPAD_API_LEVEL >= 6
  trace('U', p, kinds[i].vid, kinds[i].pid, USBPAD_STEP_DESC, 0x81, 0, 64);
  trace('U', p, kinds[i].vid, kinds[i].pid, USBPAD_STEP_CLAIM, 0, 0, 0);
  trace('U', p, kinds[i].vid, kinds[i].pid, USBPAD_STEP_START, 0, 0, kinds[i].kind);
  trace('U', p, kinds[i].vid, kinds[i].pid, USBPAD_STEP_IN_OPEN, 0x81, 0, 64);
  trace('U', p, kinds[i].vid, kinds[i].pid, USBPAD_STEP_LIVE, 0, 0, kinds[i].kind);
#endif
  post(USBPAD_EV_CONNECTED, p, kinds[i].kind, kinds[i].vid, kinds[i].pid);
  bus_kind = i;
  bus_offered = 1;
  return 1;
}

/* The controller's bus power: off, the device is gone (the driver sees a
 * detach); on, the OS re-enumerates and offers it after REOFFER_S unless
 * the cycle is one that does nothing. */
static void set_bus_power(int on)
{
  int p;

  if (on == bus_power)
    return;
  bus_power = on;
  if (!on)
  {
    /* (a hub the OS suspended is no hub at work: its power restart is the
     * way back, not a fault) */
    if (hub_in && hub_offered && hub_dead)
    {
      /* (2026-11-18) the dead hub's recovery: what was on it comes back */
      int k;

      if (sound_claimed && hub_keep_n < 8)
        hub_keep[hub_keep_n++] = (int)kind_index("sound");
      if (drive_claimed && hub_keep_n < 8)
        hub_keep[hub_keep_n++] = (int)kind_index("drive");
      for (p = 0; p < USBPAD_PLAYERS; p++)
        for (k = 0; slots[p].connected && k < (int)(sizeof(kinds) / sizeof(kinds[0])); k++)
          if (kinds[k].vid == slots[p].vid && kinds[k].pid == slots[p].pid && hub_keep_n < 8)
          {
            hub_keep[hub_keep_n++] = k;
            break;
          }
      pad_log("hub: power restarted under the dead hub (%d, %s): %d device(s) on it", ++hub_restarts,
              hub_dead ? "its power fell" : "its explore stuck", hub_keep_n);
      hub_dead = 0;
      explore_flag = 0;
      explore_stuck = 0;
      pend_kind = -1;
      if (hub_image && fake_unplug)
        fake_unplug();   /* (the fake drive with files was on the hub: it goes with it) */
      hub_restarting = 1;
    }
    else if (hub_in && hub_offered && !bus_asleep)
      pad_log("hub: bus power dropped under the hub (%d)", ++hub_drops);   /* must never happen */

    if (sleepy)
      sleep_device_left();
    hub_offered = 0;
    if (sound_claimed)
      pad_log("sound: the adapter's detach (active goes 0), completions so far %u", (unsigned)sound_completions);
    sound_claimed = 0;
    drive_claimed = 0;
    for (p = 0; p < USBPAD_PLAYERS; p++)
      if (slots[p].connected)
      {
        memset(&slots[p], 0, sizeof(slots[p]));
        diag_detaches++;
        post(USBPAD_EV_DISCONNECTED, p, 0, 0, 0);
      }
    bus_offered = 0;
    offer_at = -1;
    return;
  }
  cycles++;
  if (slow_mode && bus_kind >= 0)
  {
    /* PAD4: the OS starts over with the device (its debounce, its reset) */
    slow_phase = slow_hc > 0 ? 0 : 1;
    slow_from = slow_now();
    pad_log("slow: the power restart set the OS back to the start");
    return;
  }
  if (wedged)
    return;   /* the OS's USB has stopped: a power restart brings no offer either */
  if (sleepy && phy_asleep && (bus_kind >= 0 || hub_in))
  {
    stuck_dev = 1;
    dead_plugs++;
    pad_log("sleep: powered anew on a sleeping transceiver: connected, never enabled (%u)", dead_plugs);
    return;
  }
  stuck_dev = 0;
  if ((bus_kind >= 0 || hub_in) && (cycle_mode == CYCLE_OK || (cycle_mode == CYCLE_LATE && cycles >= cycle_late_n)))
  {
    offer_at = host_hw_now() + (hub_in && hub_late > 0 ? hub_late : REOFFER_S);
    hc_stopped = 0;
  }
}

/* The OS offers the hub to the driver at device level (bDeviceClass 9). */
static void offer_hub(void)
{
  if (hub_offered || hub_quiet)
    return;
  if (sleepy && (phy_asleep || stuck_dev))
  {
    if (!stuck_dev)
      pad_log("sleep: a hub on a sleeping transceiver: connected, never enabled (%u)", ++dead_plugs);
    stuck_dev = 1;
    return;
  }
  hub_offered = 1;
  hub_gen_model++;
  info.hubs++;
  info.hub_vid = 0x214b;
  info.hub_pid = 0x7260;
  diag_matches++;
  ring_offer(0x214b, 0x7260, 0, 9, 0, 1, 0);
  pad_log("hub: offered to the drivers (device 09/00/01)");
}

/* PAD4: where the OS has got with a "pad slowhc" device (at each look at
 * the registers and each poll). */
static void slow_update(void)
{
  if (!slow_mode || bus_kind < 0 || !bus_power || slow_phase == 2)
    return;
  if (slow_phase == 0 && slow_now() - slow_from >= slow_hc)
  {
    slow_phase = 1;
    if (slow_mode == 3)
      pad_log("usbtick: the OS resets the port after %.0f of its USB ticks, %.3f s after the plug", slow_hc,
              host_hw_now() - usbtick_plug);
    else
      pad_log("slow: the host controller runs (OS ticks so far %.0f)", host_os_ticks);
    slow_from = slow_now();
  }
  if (slow_phase == 1 && slow_now() - slow_from >= slow_enum)
  {
    slow_phase = 2;
    pad_log("slow: the port is enabled, the OS offers the device (OS ticks so far %.0f)", host_os_ticks);
    offer(bus_kind);
  }
}

/* The device's descriptor, as a read of it over the bus gives it, when the
 * script said "direct" and the device is powered at a host's end. */
int host_usbpad_direct_read(uint8_t *buf, unsigned size)
{
  uint8_t d[18] = { 18, 1, 0, 2, 0, 0, 0, 64, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1 };

  if (!direct_ok || bus_kind < 0 || !bus_power || (host_usbpad_port != 1 && host_usbpad_port != 3) || size < 18)
    return -1;
  d[8] = (uint8_t)kinds[bus_kind].vid;
  d[9] = (uint8_t)(kinds[bus_kind].vid >> 8);
  d[10] = (uint8_t)kinds[bus_kind].pid;
  d[11] = (uint8_t)(kinds[bus_kind].pid >> 8);
  memcpy(buf, d, 18);
  return 18;
}

/* The USB controller's registers that matter to a bus power cycle (offsets
 * from 0xB0000000). CX II FOTG210: OTGCSR 0x80, bit 5 A_BUS_DROP, bit 4
 * A_BUS_REQ, bit 19 A_VBUS, bit 21 ID; PORTSC 0x30 bit 0 connected, bit 2
 * enabled. CX ChipIdea: PORTSC 0x184 bit 12 PP, bit 0 connected; OTGSC 0x1A4
 * bit 5 IDPU, bit 8 ID (0 the host's end), bit 9 AVV; USBMODE 0x1A8 (3 host).
 * A device shows only with power at
 * a host's end. S4's register writes land here. */
#define USB_BASE_ADDR 0xB0000000u

static int host_end(void)
{
  return host_usbpad_port == 1 || host_usbpad_port == 3;
}

int host_link_calc_on_port(void);
static int calc_drops;

uint32_t host_usb_reg_read(uint32_t addr)
{
  uint32_t off = addr - USB_BASE_ADDR, v = 0;
  int calc = host_link_calc_on_port(), seen = (bus_kind >= 0 || calc || hub_in) && bus_power && (host_end() || calc);

  if (calc && off == 0x80)
    return fotg_req_drop | (bus_power ? 1u << 19 : 0);
  /* (r19-usb) "pad load heavy": the port's power turned on and held down by
   * what is on the adapter (OTGCSR 00012e10 as a CX II showed: A_BUS_REQ, no
   * A VBUS, no session, B session end); nothing on the wires, the host
   * controller stopped */
  if (load_heavy && !calc && host_end())
  {
    if (off == 0x80)
      return 0x10u | 1u << 16;
    if (off == 0x30 || off == 0x84)
      return 0;
    if (off == 0x10)
      return 0x80000u;
  }
  slow_update();
  halt_update();
  if (sleepy && !calc)
  {
    int dev = (bus_kind >= 0 || hub_in) && bus_power && host_end();

    sleep_update();
    if (off == 0x40)
      return misc_other | (phy_asleep ? 0x100u : 0);
    if (bus_asleep && dev)
    {
      if (off == 0x30)
        return 0x88du;
      if (off == 0x10)
        return 0x80000u;
    }
    else if ((phy_asleep || stuck_dev) && host_end() && bus_power)
    {
      /* nothing on it: the suspend bit and the change bits left; a device:
       * connected, never enabled */
      if (off == 0x30)
        return !dev ? (phy_asleep ? 0x8au : 0xau) : phy_asleep ? 0x889u : 0x809u;
      if (off == 0x10)
        return 0x80031u;
    }
  }
  if (halt_on && seen && !calc)
  {
    if (off == 0x30)
      return 0x88du;
    if (off == 0x10)
      return 0x80000u;
  }
  /* PAD4: a slowhc device: connected, the controller stopped; in reset with it
   * running; then enabled (the old tests' word, 5) */
  if (slow_mode && seen && !calc && slow_phase < 2)
  {
    /* (R12 usbtick: the controller running all along, connected with its
     * change bits as the CX II showed: 0000080b, then 0000010b in reset) */
    if (off == 0x30 || off == 0x184)
      return (off == 0x184 ? ci_portsc_pp : 0) |
             (slow_mode == 3 ? (slow_phase ? 0x10bu : 0x80bu) : slow_phase ? 0x103u : 0x803u);
    if (off == 0x10 || off == 0x140)
      return slow_phase || slow_mode == 3 ? 0x80031u : 0x80b00u;
  }

  if (off == 0x80)
  {
    /* (PAD3: the device's end with its session: a computer gives power.) */
    v = fotg_req_drop | (host_usbpad_port == 2 ? 1u << 21 | 1u << 20 | 1u << 17 : 0) |
        (host_end() && bus_power && !(blip_until >= 0 && host_hw_now() < blip_until) ? 1u << 19 : 0);
  }
  else if (off == 0x84)
    v = blip_until >= 0 && host_hw_now() < blip_until ? 0x420u : 0;   /* OTGISR: the VBUS error, over-current */
  /* PAD3: a device's pull-up shows on the lines (J, bits 11:10 = 2) with
   * power; connected and enabled only once the OS's host runs. */
  else if (off == 0x30)
    v = seen ? (hc_stopped && !calc ? 0x800u : wedged && !calc ? 0xfu : 5u) : 0;   /* (the lines left out when connected: the old tests' words) */
  else if (off == 0x184)
    v = ci_portsc_pp | (seen && ci_portsc_pp ? (hc_stopped ? 0x800u : 5u) : 0);
  else if (off == 0x10 || off == 0x140)
    v = (seen && !(hc_stopped && !calc)) ? 0x80031u : 0x80b00u;   /* USBCMD: run with a device the OS started */
  else if (off == 0x1A4)
    v = (host_usbpad_port == 3 ? 0 : 1u << 8) | 1u << 5 | (host_end() && bus_power ? 1u << 9 : 0);   /* IDPU on */
  else if (off == 0x1A8)
    v = host_usbpad_port == 3 ? 3 : 2;   /* USBMODE: host at the adapter's end */
  return v;
}

void host_usb_reg_write(uint32_t addr, uint32_t value)
{
  uint32_t off = addr - USB_BASE_ADDR;

  if (off == 0x80)
  {
    fotg_req_drop = value & 0x30u;
    if ((value & 0x20u) && host_link_calc_on_port())
      pad_log("calc: bus power dropped under the other calculator (%d)", ++calc_drops);   /* SEAM-2: must never happen */
    if (value & 0x20u)
      set_bus_power(0);
    else if (value & 0x10u)
      set_bus_power(1);
  }
  else if (off == 0x184)
  {
    ci_portsc_pp = value & (1u << 12);
    set_bus_power(ci_portsc_pp != 0);
  }
  else if (off == 0x40)
  {
    if ((value & ~0x100u) != misc_other)
      pad_log("phy: the register's other bits were changed (%08x, were %08x)", value & ~0x100u, misc_other);   /* must never happen */
    if (bus_asleep && (bus_kind >= 0 || hub_in) && !(value & 0x100u))
      pad_log("phy: woken under a device the OS holds suspended");   /* must never happen */
    if (phy_asleep && !(value & 0x100u))
    {
      phy_asleep = 0;
      phy_wakes++;
      pad_log("phy: the transceiver woken (%u)%s", phy_wakes,
              stuck_dev ? "; the device on it stays as it is until its power is restarted" : "");
    }
    else if (value & 0x100u)
      phy_asleep = 1;
  }
}

void host_usbpad_script(const char *line)
{
  char what[32] = "", arg[32] = "", arg2[32] = "";
  unsigned i;
  int p;

  if (!strncmp(line, "sync ", 5))
  {
    /* "sync PREFIX N": the path may be long, so it is parsed apart. */
    char prefix[600], mine[640], theirs[640];
    int n = 0;
    struct stat st;
    FILE *f;
    int tries;

    if (sscanf(line + 5, "%599s %d", prefix, &n) != 2 || (n != 1 && n != 2))
    {
      fprintf(stderr, "gpsp-host: pad sync PREFIX 1|2\n");
      exit(2);
    }
    snprintf(mine, sizeof(mine), "%s.%d", prefix, n);
    snprintf(theirs, sizeof(theirs), "%s.%d", prefix, 3 - n);
    f = fopen(mine, "w");
    if (f)
      fclose(f);
    for (tries = 0; tries < 20000 && stat(theirs, &st); tries++)
      usleep(1000);
    pad_log("pad sync %d%s", n, tries == 20000 ? " (the other side never came)" : "");
    return;
  }
  sscanf(line, "%31s %31s %31s", what, arg, arg2);
  if (!strcmp(what, "sleepy"))
  {
    sleepy = !strcmp(arg, "on");
    idle_since = -1;
    pad_log("sleepy %s", arg);
  }
  else if (!strcmp(what, "awake"))
  {
    awake_known = strcmp(arg, "unknown") != 0;
    pad_log("awake %s", arg);
  }
  else if (!strcmp(what, "asleep"))
  {
    /* suspended now, with what is on the bus */
    sleepy = 1;
    bus_asleep = phy_asleep = 1;
    sleep_count++;
    pad_log("sleep: the bus is suspended (it was before gpSP's changes were in)");
  }
  else if (!strcmp(what, "in") && sleepy && !wedged && (bus_asleep || phy_asleep || stuck_dev))
  {
    /* into a suspended hub: not seen; onto a sleeping transceiver:
     * connected, never enabled. On the bus all the same: what restarts
     * the port's power with the transceiver awake gets it offered. */
    bus_kind = (int)kind_index(arg);
    bus_offered = 0;
    if (host_usbpad_port <= 0)
    {
      host_usbpad_port = 1;
      port_by_pad = !hub_in;
    }
    if (bus_asleep && hub_in)
      pad_log("sleep: plugged into a suspended hub: not seen (%u)", ++deaf_plugs);
    else
    {
      stuck_dev = 1;
      pad_log("sleep: plugged onto a sleeping transceiver: connected, never enabled (%u)", ++dead_plugs);
    }
  }
  else if (!strcmp(what, "hubslow"))
    hub_slow = atoi(arg);
  else if (!strcmp(what, "reports"))
  {
    if (!strcmp(arg, "onchange"))
      reports_onchange = 1;
    else if (!strcmp(arg, "byirq"))
      reports_byirq = 1;
    else if (!strcmp(arg, "nowindows"))
      reports_byirq = reports_nowindows = 1;
    else if (!strcmp(arg, "silent"))
      reports_silent = strcmp(arg2, "off") != 0;
  }
  else if (!strcmp(what, "hub") && !strcmp(arg, "deaf"))
    hub_deaf = 1;
  else if (!strcmp(what, "hub") && !strcmp(arg, "stale"))
    hub_stale = 1;
  else if (!strcmp(what, "load") && !strcmp(arg, "heavy"))
    load_heavy = strcmp(arg2, "off") != 0;
  else if (!strcmp(what, "exit") && !strcmp(arg, "hang"))
  {
    exit_hang_drive |= !strcmp(arg2, "drive");
    exit_hang_bt |= !strcmp(arg2, "bluetooth");
  }
  else if (!strcmp(what, "hub") && !strcmp(arg, "stall"))
    hub_stall = 1;   /* the hub answers the probe with a stall (an answer all the same) */
  else if (!strcmp(what, "hub") && !strcmp(arg, "late"))
    hub_late = atof(arg2);
  else if (!strcmp(what, "hub") && !strcmp(arg, "image"))
    hub_image = 1;   /* the fake drive with files (GPSP_USB_IMAGE) is on the hub */
  else if (!strcmp(what, "hub") && !strcmp(arg, "wedge"))
  {
    /* dead without a fall of the power seen: the explore stuck, nothing answers */
    hub_dead = hub_in && hub_offered;
    explore_flag = 1;
    pad_log("hub: dead, no fall of the power seen (its explore times out on it)");
  }
  else if (!strcmp(what, "hub") && (!strcmp(arg, "blip") || !strcmp(arg, "dip")))
  {
    double ms = arg2[0] ? atof(arg2) : 100;

    blip_until = host_hw_now() + ms / 1000.0;
    hub_dead = hub_in && hub_offered && strcmp(arg, "dip");
    pad_log("hub: the bus power falls %.0f ms under the hub (sound completions so far %u): %s", ms,
            (unsigned)sound_completions,
            hub_dead ? "it browns out" : !strcmp(arg, "dip") ? "it rides it out" : "no hub offered");
  }
  else if (!strcmp(what, "in") && hub_in && hub_dead && running)
  {
    /* into the dead hub: not seen until its power is restarted */
    if (hub_keep_n < 8)
      hub_keep[hub_keep_n++] = (int)kind_index(arg);
    pad_log("hub: plugged into the dead hub: not seen");
  }
  else if (!strcmp(what, "hub") && !strcmp(arg, "overcurrent"))
    hub_overcur = !strcmp(arg2, "on");
  else if (!strcmp(what, "hub") && !strcmp(arg, "stuckport"))
    hub_stuckport = !strcmp(arg2, "on");
  else if (!strcmp(what, "in") && hub_in && hub_offered && hub_deaf && running && !wedged)
  {
    /* on a port of a hub that says nothing: not news, not offered */
    deaf_kind = (int)kind_index(arg);
    port4_seen = 0;
    pad_log("hub: on a port of the hub, which says nothing of it");
  }
  else if (!strcmp(what, "explore"))
  {
    explore_known = strcmp(arg, "unknown") != 0;
    explore_stuck = !strcmp(arg, "stuck");   /* the flag set whatever the OS's task runs */
  }
  else if (!strcmp(what, "sound") && !strcmp(arg, "serves"))
    sound_serves = 1;
  else if (!strcmp(what, "sound") && !strcmp(arg, "idle"))
    sound_idle = !strcmp(arg2, "on");
  else if (!strcmp(what, "in") && hub_in && hub_offered && hub_slow > 0 && running && !wedged &&
           !(sleepy && (bus_asleep || phy_asleep || stuck_dev)))
  {
    /* on a port of the hub: the OS's hub driver has news, and finds the
     * device in N of its task's runs */
    pend_kind = (int)kind_index(arg);
    pend_sleeps0 = nspire_os_sleeps();
    explore_flag = 1;
    pad_log("hub: on a port of the hub: offered after %d runs of the OS's USB task", hub_slow);
  }
  else if (!strcmp(what, "wedge"))
  {
    wedged = !strcmp(arg, "on");
    pad_log("wedge %s", arg);
    if (!wedged && bus_kind >= 0 && !bus_offered && running)
      offer(bus_kind);
  }
  else if (!strcmp(what, "in") && wedged)
  {
    /* on the bus, enabled, never offered */
    bus_kind = (int)kind_index(arg);
    bus_offered = 0;
    if (host_usbpad_port <= 0)
    {
      host_usbpad_port = 1;
      port_by_pad = 1;
    }
  }
  else if (!strcmp(what, "unplug") && wedged)
  {
    bus_kind = -1;
    if (port_by_pad)
    {
      host_usbpad_port = 0;
      port_by_pad = 0;
    }
  }
  else if (!strcmp(what, "in"))
  {
    if (running)
      offer((int)kind_index(arg));
  }
  else if (!strcmp(what, "preplug") || !strcmp(what, "resident") || !strcmp(what, "hcstop"))
  {
    hc_stopped = !strcmp(what, "hcstop");
    slow_mode = 0;
    bus_kind = (int)kind_index(arg);
    bus_offered = 0;
    if (host_usbpad_port <= 0)
    {
      host_usbpad_port = 1;
      port_by_pad = 1;   /* taking the device out takes the adapter off the port too */
    }
    if (!strcmp(what, "resident"))
    {
      if (running)
        offer(bus_kind);
      else
        pending_resident = bus_kind;
    }
  }
  else if (!strcmp(what, "slowhc"))
  {
    char kind[32] = "";
    double hc = 0, en = 0;

    if (sscanf(line, "%*s %*s %lf %lf %31s", &hc, &en, kind) < 2 || (strcmp(arg, "ticks") && strcmp(arg, "wall")))
    {
      fprintf(stderr, "gpsp-host: pad slowhc ticks|wall HC ENUM [KIND]\n");
      exit(2);
    }
    slow_mode = !strcmp(arg, "ticks") ? 1 : 2;
    slow_hc = hc;
    slow_enum = en;
    slow_phase = hc > 0 ? 0 : 1;
    slow_from = slow_now();
    hc_stopped = 0;
    bus_kind = (int)kind_index(kind);
    bus_offered = 0;
    offer_at = -1;
    if (host_usbpad_port <= 0)
    {
      host_usbpad_port = 1;
      port_by_pad = 1;
    }
  }
  else if (!strcmp(what, "usbtick"))
  {
    char kind[32] = "";
    double n = 0, en = 0;

    if (sscanf(line, "%*s %lf %lf %31s", &n, &en, kind) < 2)
    {
      fprintf(stderr, "gpsp-host: pad usbtick N ENUM [KIND]\n");
      exit(2);
    }
    host_hw_adapter_in();   /* the OS's USB timer runs from the adapter's arrival (R11t's link log: 42, then e2) */
    slow_mode = 3;
    slow_hc = n;
    slow_enum = en;
    slow_phase = 0;
    slow_from = slow_now();
    usbtick_plug = host_hw_now();
    hc_stopped = 0;
    bus_kind = (int)kind_index(kind);
    bus_offered = 0;
    offer_at = -1;
    if (host_usbpad_port <= 0)
    {
      host_usbpad_port = 1;
      port_by_pad = 1;
    }
  }
  else if (!strcmp(what, "cycle"))
  {
    cycle_mode = !strcmp(arg, "ignored") ? CYCLE_IGNORED : !strcmp(arg, "late") ? CYCLE_LATE : CYCLE_OK;
    cycle_late_n = atoi(arg2);
    cycles = 0;
  }
  else if (!strcmp(what, "buspower"))
    set_bus_power(!strcmp(arg, "on"));
  else if (!strcmp(what, "direct"))
    direct_ok = 1;
  else if (!strcmp(what, "readdesc"))
  {
    uint8_t d[18];

    if (host_usbpad_direct_read(d, sizeof(d)) == 18)
      pad_log("readdesc: ok %04x:%04x", d[8] | d[9] << 8, d[10] | d[11] << 8);
    else
      pad_log("readdesc: none");
  }
  else if (!strcmp(what, "reg"))
  {
    uint32_t addr = (uint32_t)strtoul(arg, NULL, 16);

    if (arg2[0])
      host_usb_reg_write(addr, (uint32_t)strtoul(arg2, NULL, 16));
    pad_log("reg %08x = %08x", addr, host_usb_reg_read(addr));
  }
  else if (!strcmp(what, "calc"))
  {
    extern int host_link_calc_preplug;

    extern int host_link_calc_node;
    extern void host_link_cable(int out);

    if (!strcmp(arg, "delay"))
    {
      extern double host_link_calc_delay;

      host_link_calc_delay = atof(arg2);
    }
    else if (!strcmp(arg, "out") || !strcmp(arg, "in"))
    {
      host_link_cable(arg[0] == 'o');
      pad_log("calc %s", arg);
    }
    else if (!strcmp(arg, "node"))
      host_link_calc_node = 1;
    else if (!strcmp(arg, "end"))
    {
      extern int host_link_calc_device_end;

      host_link_calc_device_end = !strcmp(arg2, "device");
    }
    else
      host_link_calc_preplug = !strcmp(arg, "preplug");
  }
  else if (!strcmp(what, "unplug"))
  {
    /* PAD3: the device on the bus (never offered, or hcstop) comes out; the
     * adapter stays unless "pad preplug/hcstop" put it there. */
    if (sleepy && !hub_in)
      sleep_device_left();
    bus_kind = -1;
    bus_offered = 0;
    offer_at = -1;
    hc_stopped = 0;
    slow_mode = 0;
    halt_on = 0;
    if (port_by_pad)
    {
      host_usbpad_port = 0;
      port_by_pad = 0;
    }
  }
  else if (!strcmp(what, "halt"))
  {
    /* R10b "pad halt N" */
    double n = atof(arg);

    halt_on = 1;
    halt_until = n > 0 ? host_os_ticks + n : 0;
    pad_log("halt: the OS halts its host controller (OS ticks so far %.0f)", host_os_ticks);
  }
  else if (!strcmp(what, "hub"))
  {
    /* "out": the hub and the OTG adapter it is in come out of the
     * calculator; "unplug" (2026-11-17): the hub comes out of the adapter,
     * which stays in (the port's end is still the host's) */
    if (!strcmp(arg, "out") || !strcmp(arg, "unplug"))
    {
      for (p = 0; p < USBPAD_PLAYERS; p++)
        if (slots[p].connected)
        {
          memset(&slots[p], 0, sizeof(slots[p]));
          diag_detaches++;
          post(USBPAD_EV_DISCONNECTED, p, 0, 0, 0);
        }
      hub_in = hub_offered = hub_quiet = 0;
      hub_dead = hub_keep_n = hub_restarting = 0;
      pend_kind = -1;
      explore_flag = 0;
      hub_deaf = hub_overcur = hub_stuckport = port4_seen = 0;
      hub_stale = port3_stale = 0;
      deaf_kind = -1;
      if (sleepy)
        sleep_device_left();
      sound_claimed = drive_claimed = 0;
      bus_kind = -1;
      bus_offered = 0;
      offer_at = -1;
      if (!strcmp(arg, "out"))
        host_usbpad_port = 0;
      port_by_pad = 0;
      pad_log("hub %s", arg);
    }
    else
    {
      hub_in = 1;
      hub_offered = 0;
      hub_quiet = !strcmp(arg, "quiet");
      if (host_usbpad_port <= 0)
        host_usbpad_port = 1;
      port_by_pad = 0;   /* a device out of the hub leaves the hub on the port */
      pad_log("hub %s", arg[0] ? arg : "in");
      if (running && strcmp(arg, "preplug"))
        offer_hub();
    }
  }
  else if (!strcmp(what, "out") && !strcmp(arg, "sound"))
  {
    sound_claimed = 0;
    if (!hub_in)
    {
      bus_kind = -1;
      bus_offered = 0;
      if (port_by_pad)
      {
        host_usbpad_port = 0;
        port_by_pad = 0;
      }
    }
  }
  else if (!strcmp(what, "image"))
  {
    /* The fake flash drive with files on it (GPSP_USB_IMAGE, nspire-usbms's
     * tests/fake_drive.c) comes out or goes back in: as on a hub, the port's
     * registers don't move. */
    if (!strcmp(arg, "out") && fake_unplug)
      fake_unplug();
    else if (!strcmp(arg, "in") && fake_plug && getenv("GPSP_USB_IMAGE"))
      fake_plug(getenv("GPSP_USB_IMAGE"));
    pad_log("image %s", arg);
  }
  else if (!strcmp(what, "out") && !strcmp(arg, "drive"))
  {
    drive_claimed = 0;
    bus_kind = -1;
    bus_offered = 0;
    offer_at = -1;
    if (port_by_pad)
    {
      host_usbpad_port = 0;
      port_by_pad = 0;
    }
  }
  else if (!strcmp(what, "out"))
  {
    p = arg[0] ? atoi(arg) - 1 : 0;
    if (p >= 0 && p < USBPAD_PLAYERS && slots[p].connected)
    {
      memset(&slots[p], 0, sizeof(slots[p]));
      diag_detaches++;
      post(USBPAD_EV_DISCONNECTED, p, 0, 0, 0);
      if (!usbpad_count() && hub_in && hub_offered && hub_stale)
      {
        port3_stale = 1;   /* the explore that saw the pull left its answer: "p!" */
        pad_log("hub: the pull's port reads as the hub last answered (powered, not connected, a change) until the "
                "OS's next explore");
      }
      if (!usbpad_count())
        bus_kind = -1, bus_offered = 0, hc_stopped = 0, slow_mode = 0, halt_on = 0;   /* unplugged: a bus power cycle re-offers nothing */
      if (!usbpad_count() && sleepy && !hub_in)
        sleep_device_left();
      if (!usbpad_count() && port_by_pad)
      {
        host_usbpad_port = 0;
        port_by_pad = 0;
      }
    }
  }
  else if (!strcmp(what, "event"))
  {
    uint16_t vid = 0x2563, pid = 0x0575;

    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++)
      if (!strcmp(arg2, kinds[i].name))
      {
        vid = kinds[i].vid;
        pid = kinds[i].pid;
      }
    if (!strcmp(arg, "failed"))
    {
      /* As the driver's fail_port: the pad's player let go first (a
       * DISCONNECTED), then FAILED without a player. "pad event failed
       * KIND": the pad of that kind (in a slot or not). */
      for (p = 0; p < USBPAD_PLAYERS; p++)
        if (slots[p].connected && slots[p].vid == vid && slots[p].pid == pid)
        {
          uint8_t kind = slots[p].kind;

          memset(&slots[p], 0, sizeof(slots[p]));
          diag_detaches++;
          post(USBPAD_EV_DISCONNECTED, p, kind, vid, pid);
          break;
        }
      for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]) && !(kinds[i].vid == vid && kinds[i].pid == pid); i++)
        ;
      post(USBPAD_EV_FAILED, -1, i < sizeof(kinds) / sizeof(kinds[0]) ? kinds[i].kind : USBPAD_KIND_NONE, vid, pid);
      return;
    }
    post(!strcmp(arg, "replug") ? USBPAD_EV_REPLUG_NEEDED : !strcmp(arg, "power") ? USBPAD_EV_NEEDS_POWER :
         USBPAD_EV_UNSUPPORTED, -1, USBPAD_KIND_NONE, vid, pid);
#if USBPAD_API_LEVEL >= 6
    {
      /* R10b: "pad event unsupported KIND STEP EP ERR": the failure's facts
       * (STEP a usbpad_step_name word), the trace records that led to it,
       * the pad's kind */
      char step_word[32] = "";
      unsigned ep = 0;
      int err = 0, s;

      if (sscanf(line, "%*s %*s %*s %31s %x %d", step_word, &ep, &err) >= 1 && !strcmp(arg, "unsupported"))
      {
        struct usbpad_event *e = &queue[queued - 1];

        for (s = 1; s < USBPAD_STEP_COUNT && strcmp(usbpad_step_name(s), step_word); s++)
          ;
        for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]) && strcmp(arg2, kinds[i].name); i++)
          ;
        e->kind = i < sizeof(kinds) / sizeof(kinds[0]) ? kinds[i].kind : USBPAD_KIND_NONE;
        e->name = usbpad_kind_name(e->kind);
        e->step = (uint8_t)(s < USBPAD_STEP_COUNT ? s : USBPAD_STEP_START);
        e->ep = (uint8_t)ep;
        e->err = (int16_t)err;
        trace('U', 0, vid, pid, USBPAD_STEP_DESC, 0x81, 0, 64);
        trace('U', 0, vid, pid, USBPAD_STEP_CLAIM, 0, 0, 0);
        trace('U', 0, vid, pid, e->step, ep, err, 0);
        trace('U', 0, vid, pid, USBPAD_STEP_QUIET, 0, err, e->step);
        diag_matches++;
        ring_offer(vid, pid, 1, 3, 0, 0, 13);
      }
    }
#endif
  }
#if USBPAD_API_LEVEL >= 6
  else if (!strcmp(what, "trace"))
  {
    /* "pad trace N": N routine OUT records of the pad in slot 1 (its lights, ACKs) */
    int n = atoi(arg);

    while (n-- > 0)
      trace('M', 0, slots[0].vid, slots[0].pid, USBPAD_STEP_OUT, 0x02, 2, 13);
  }
#endif
  else if (!strcmp(what, "diag"))
  {
    char arg3[32] = "0";

    sscanf(line, "%*s %*s %*s %31s", arg3);
    diag_matches = atol(arg);
    diag_attaches = atol(arg2);
    diag_last_err = atol(arg3);
  }
  else if (!strcmp(what, "stat"))
  {
    /* S8: the cost counters so far, for the tests to difference: a line
     * "pad stat: t=SECONDS sleeps=N samples=N plugged_reads=N bits_reads=N". */
    pad_log("stat: t=%.3f sleeps=%u samples=%u plugged_reads=%ld bits_reads=%ld sound_completions=%u", host_hw_now(),
            (unsigned)nspire_os_sleeps(), (unsigned)nspire_usbport_samples(), host_usb_plugged_reads, host_usb_bits_reads,
            (unsigned)sound_completions);
    return;
  }
  else if (!strcmp(what, "error"))
  {
    info.in_errors += (uint32_t)(arg[0] ? atol(arg) : 1);
    info.last_in_status = (uint8_t)(arg2[0] ? atoi(arg2) : 17);
  }
  else if (!strcmp(what, "synth"))
    info.synth_cfg++;
  else if (!strcmp(what, "slowexit"))
    slow_exit = 1;
  else if (!strcmp(what, "noretire"))
    no_retire = 1;
  else if (!strcmp(what, "dropped"))
    dropped = (uint32_t)atol(arg);
  else if (!strcmp(what, "want"))
    want = !strcmp(arg, "on");
  else if (!strcmp(what, "port"))
  {
    host_usbpad_port = atoi(arg);
    port_by_pad = 0;
    if (sleepy)
      sleep_update();   /* (the adapter out: the sleep's states end) */
  }
  else
  {
    fprintf(stderr, "gpsp-host: unknown pad line: pad %s\n", line);
    exit(2);
  }
  pad_log("pad %s %s (OS sleeps so far %u)", what, arg, (unsigned)nspire_os_sleeps());
}

int usbpad_init(unsigned flags)
{
  (void)flags;
  running = 1;
  if (pending_resident >= 0)
  {
    offer(pending_resident);
    pending_resident = -1;
  }
  pad_log("init: stick_dpad=%u press_at=%u release_at=%u os_time=%u replug_hint=%u sleep_hook=%d port_hook=%d",
          config.stick_dpad, config.on_pct, config.off_pct, os_config.want_live, os_config.replug_hint,
          hook_sleep != NULL, hook_probe != NULL);
  return 0;
}

const char *usbpad_status(void)
{
  return running ? "ok" : "not started";
}

int usbpad_diag(char *buf, unsigned size)
{
  return snprintf(buf, size,
                  "backend=os registered=%u missing=0 ctl=raw out=1 synth_cfg=%u leaked=0 reg=?\n"
                  "matches=%ld attaches=%ld detaches=%ld reports=%lu in_err=%lu out_sent=0 out_err=0 last_err=%ld\n"
                  "attach: step=%s err=0 ifc=0000:0000 in=00/0 out=00 hid=0 cal=-1 raw=00000000 far=00000000\n"
                  "close: abort=0 close=0 fail=0 out_stuck=0 in_stale=0 in_status=%u shut_ticks=0\n"
                  "%s"
                  "match[0]: 045e:028e iface if0 ff/5d/01 -> 5\n"
                  "addresses:\n"
                  "usbd_do_request=00000000 usbd_open_pipe=00000000\n",
                  running ? 1u : 0u, (unsigned)info.synth_cfg, diag_matches, diag_attaches, diag_detaches,
                  (unsigned long)info.reports, (unsigned long)info.in_errors, diag_last_err,
                  diag_attaches ? "live" : "none", (unsigned)info.last_in_status,
                  info.first_len ? "first_report=8:0000000000000000\n" : "");
}

int nspire_hub_recovering(void) __attribute__((weak));
static int hubrec_seen = -1;

int usbpad_poll(void)
{
  double now = host_hw_now();
  int i;

  /* (2026-11-18) the glue's nspire_hub_recovering, each change logged */
  if (nspire_hub_recovering && nspire_hub_recovering() != hubrec_seen)
  {
    hubrec_seen = nspire_hub_recovering();
    pad_log("hubrec: %d", hubrec_seen);
  }

  if (!running)
    return 0;
  polls++;
#if USBPAD_API_LEVEL >= 6
  polls_for_trace++;
#endif
  slow_update();
  if (sleepy)
    sleep_update();
  /* (2026-11-17) the sound's own task sleep, and what waits on a port of
   * the hub for the OS's USB task */
  if (sound_serves && sound_claimed && hub_in)
  {
    double interval = (explore_known && !(nspire_os_wanted & OS_WANT_BT)) ? NSPIRE_AUDIO_HUB_MS / 1000.0 :
                      NSPIRE_AUDIO_HUB_FALLBACK_MS / 1000.0;
    if (serve_slept_at < 0)
      serve_slept_at = now;
    else if (now - serve_slept_at >= interval)
    {
      serve_slept_at = now;
      nspire_os_sleep();
    }
  }
  probe_update(now);
  if (sound_claimed && !hub_dead && !sound_idle)
    sound_completions++;   /* (the stream's completions: one a poll says it lives) */
  if (hub_dead)
    ;   /* (the explore times out on the dead hub: its flag stays) */
  else if (pend_kind == -2)
  {
    /* an asked-for explore with nothing new: the flag goes at the task's run */
    if (nspire_os_sleeps() != pend_sleeps0)
    {
      explore_flag = 0;
      pend_kind = -1;
      if (port3_stale)
        pad_log("hub: the OS's explore read the pulled port again: powered, not connected");
      port3_stale = 0;   /* (its explore read every port again) */
    }
  }
  else if (pend_kind >= 0)
  {
    uint32_t n = nspire_os_sleeps() - pend_sleeps0;

    if (n)
    {
      explore_flag = 0;   /* the explore task began */
      port3_stale = 0;    /* (and read every port again) */
    }
    if (n >= (uint32_t)(hub_slow > 3 ? hub_slow : 3))
    {
      int k = pend_kind;

      pend_kind = -1;
      port4_seen = 0;
      pad_log("hub: the OS found the device on the hub's port (%u runs of its USB task)", (unsigned)n);
      offer(k);
    }
  }
  if (offer_at >= 0 && now >= offer_at)
  {
    offer_at = -1;
    if (hub_in)
      offer_hub();
    if (hub_in && (hub_keep_n || hub_restarting))
    {
      int k;

      hub_restarting = 0;
      if (hub_image && fake_plug && getenv("GPSP_USB_IMAGE"))
        fake_plug(getenv("GPSP_USB_IMAGE"));

      for (k = 0; k < hub_keep_n; k++)
        offer(hub_keep[k]);
      pad_log("hub: back after the restart: %d device(s) on it offered again", hub_keep_n);
      hub_keep_n = 0;
    }
    else if (bus_kind >= 0 && !(sleepy && (phy_asleep || stuck_dev)))
      offer(bus_kind);
  }
  if (last_poll >= 0 && now - last_poll < min_gap)
    min_gap = now - last_poll;
  last_poll = now;
  if ((long)now != poll_sec)
  {
    poll_sec = (long)now;
    sec_polls = 0;
  }
  if (++sec_polls > max_sec_polls)
    max_sec_polls = sec_polls;
  (void)i;
  if (reports_byirq)
    deliver_served();   /* (only what an interrupt taken since brought) */
  else
    deliver_reports();
  return (want || (usbpad_count() && (os_config.want_live || polls % USBPAD_OS_LIVE_EVERY == 0)) ? USBPAD_WANT_OS : 0) |
         (queued ? USBPAD_EVENTS : 0);
}

const struct usbpad_state *usbpad_state(int player)
{
  return player >= 0 && player < USBPAD_PLAYERS && slots[player].connected ? &slots[player] : &empty;
}

static int slots_in(void)
{
  return slots[0].connected + slots[1].connected;
}

/* (2026-11-17) The real driver's usbpad_os_awake (backend_os.h): the three
 * changes of the OS's code as bits; refused on an OS that is not the known
 * one ("pad awake unknown"). */
int usbpad_os_awake(int on)
{
  if (!awake_known)
  {
    info.awake = -1;
    pad_log("awake: not the known OS: nothing changed");
    return -1;
  }
  awake_bits = on ? 7u : 0u;
  awake_puts++;
  info.awake = (int8_t)awake_bits;
  pad_log("awake: the OS's code %s (%u)", on ? "changed: its USB stays awake" : "put back", awake_puts);
  return (int)awake_bits;
}

int usbpad_os_awake_state(void)
{
  return awake_known ? (int)awake_bits : -1;
}

/* (2026-11-17) The real driver's usbpad_os_explore_pending (backend_os.h). */
int usbpad_os_explore_pending(void)
{
  return explore_known ? explore_flag || explore_stuck : -1;
}

/* (2026-11-17) The real driver's usbpad_os_hub_read and usbpad_os_hub_explore. */
int usbpad_os_hub_read(struct usbpad_os_hub *out)
{
  if (!explore_known)
    return -1;
  if (!hub_in || !hub_offered)
    return 0;
  memset(out, 0, sizeof(*out));
  out->nports = 4;
  out->root_port = 1;
  out->running = 1;
  out->active = (uint8_t)(sound_claimed + drive_claimed + slots_in());
  if (sound_claimed)
    out->port[0] = 0x63;   /* connected, enabled, powered, a device */
  else
    out->port[0] = 0x20;
  out->port[1] = drive_claimed ? 0x63 : 0x20;
  out->port[2] = slots_in() ? 0x63 : port3_stale ? 0xA0 : 0x20;
  out->port[3] = 0x20;
  if (hub_stuckport || port4_seen || pend_kind >= 0)
    out->port[3] |= 0x01;  /* connected, the OS has made no device of it yet */
  if (hub_overcur)
    out->port[3] |= 0x08;
  return 1;
}

/* (2026-11-18) The real driver's usbpad_os_hub_probe_start / _poll, as TI's
 * stack runs the request: on the hub's default pipe, behind the OS's own
 * explore requests to the hub (each to its 5 s limit when the hub is dead:
 * "queued behind the explore" while the explore flag is set), then 20 ms for
 * a live hub (a stall too: "pad hub stall"), the 1 s limit for a dead one;
 * cancelled when the hub leaves (pulled, the power cut, another hub). The
 * request's number keys its result (another hub's: -2 not now, -1 not
 * yours). While it is under way the longest time between two of the
 * driver's polls is kept: the frame loop must never wait for it. */
static double probe_at = -1, probe_gap, probe_last, probe_takes;
static int probe_result = -1, probe_ids, probe_cur, probe_hubgen;
int usbpad_os_hub_probe_start(void)
{
  if (!explore_known || !hub_in || !hub_offered)
    return -1;
  if (probe_at >= 0)
    return probe_hubgen == hub_gen_model ? probe_cur : -2;
  hub_probes++;
  probe_cur = ++probe_ids;
  probe_hubgen = hub_gen_model;
  probe_at = probe_last = host_hw_now();
  probe_gap = 0;
  probe_result = -1;
  probe_takes = (explore_flag || explore_stuck ? (hub_dead ? 5.0 : 0.05) : 0) + (hub_dead ? 1.0 : 0.02);
  return probe_cur;
}

static void probe_update(double now)
{
  int gone;

  if (probe_at < 0)
    return;
  if (now - probe_last > probe_gap)
    probe_gap = now - probe_last;
  probe_last = now;
  gone = !hub_in || !hub_offered || probe_hubgen != hub_gen_model;
  if (gone || now - probe_at >= probe_takes)
  {
    probe_result = gone || hub_dead ? 0 : 1;
    pad_log("hub: asked whether it answers (%d): %s after %.0f ms; the longest time between two polls meanwhile "
            "%.0f ms", hub_probes, gone ? "no (cancelled: the hub left)" : probe_result ? (hub_stall ? "yes (a stall)" :
            "yes") : "no", (now - probe_at) * 1000, probe_gap * 1000);
    probe_at = -1;
  }
}

int usbpad_os_hub_probe_poll(int id)
{
  probe_update(host_hw_now());
  if (id <= 0 || id != probe_cur)
    return -1;
  if (probe_at >= 0)
    return 2;
  return probe_result;
}

int usbpad_os_hub_explore(void)
{
  if (!explore_known)
    return -1;
  hub_explores++;
  explore_flag = 1;        /* the OS's own flag, taken at its task's next run */
  if (pend_kind < 0)
  {
    pend_kind = -2;        /* (an explore with nothing new: only the flag's turn) */
    pend_sleeps0 = nspire_os_sleeps();
  }
  if (deaf_kind >= 0)
  {
    /* the OS's task asks every port: the device shows, and is offered
     * after its runs */
    pend_kind = deaf_kind;
    deaf_kind = -1;
    port4_seen = 1;
    pend_sleeps0 = nspire_os_sleeps();
    pad_log("hub: the OS was asked to explore: it finds the device on the hub's port (%d)", hub_explores);
  }
  return 0;
}

int usbpad_count(void)
{
  return running ? slots[0].connected + slots[1].connected : 0;   /* (the real one: 0 once stopped) */
}

/* The controllers' reports as they stand now: each one's state and count. */
static void deliver_reports(void)
{
  int i;

  if (!running || hub_dead)
    return;   /* (a controller on a hub whose power fell answers nothing) */
  for (i = 0; i < USBPAD_PLAYERS; i++)
    if (slots[i].connected && !reports_silent)
    {
      slots[i].buttons = host_hw_pad_keys() & ((1u << USBPAD_NBUTTONS) - 1);
      /* ("pad reports onchange": a controller that reports only what
       * changes: no report while nothing is pressed or let go) */
      if (reports_onchange && slots[i].buttons == onchange_last[i])
        continue;
      onchange_last[i] = slots[i].buttons;
      slots[i].reports++;
      info.reports++;
      if (!info.first_len)
      {
        info.first_len = 8;
        memcpy(info.first_report, &slots[i].buttons, 4);
      }
    }
}

/* "pad reports byirq": delivered only where the USB interrupt was taken
 * since the last delivery: an OS sleep, or (unless "nowindows") a moment
 * with the CPU's interrupts on; one report a taking at the most, as one
 * pending transfer completes. */
static void deliver_served(void)
{
  int served = nspire_os_sleeps() != irq_sleeps_seen || (!reports_nowindows && irq_windows != irq_windows_seen);

  irq_sleeps_seen = nspire_os_sleeps();
  irq_windows_seen = irq_windows;
  if (served)
    deliver_reports();
}

const struct usbpad_os_info *usbpad_os_info(void)
{
  if (reports_byirq)
    deliver_served();   /* (an OS sleep since: its interrupts brought them) */
  info.matches = (uint32_t)diag_matches;
  info.attaches = (uint32_t)diag_attaches;
  info.detaches = (uint32_t)diag_detaches;
  info.registered = running;
  return &info;
}

uint32_t usbpad_dropped(void)
{
  return running ? dropped : 0;
}

void usbpad_set_wait_hook(void (*waiting)(void))
{
  hook_waiting = waiting;
}

int usbpad_lib_active(void)
{
  return running && usbpad_count() > 0;
}

int usbpad_os_shutdown_pending(void)
{
  return no_retire && shut_down;
}

int usbpad_detail(int player, char *buf, unsigned size)
{
  (void)player;
  if (size)
    buf[0] = 0;
  return 0;
}

void usbpad_shutdown(void)
{
  if (running && slow_exit && hook_waiting)
  {
    pad_log("wait hook: shutdown waits for a blocked attach");
    hook_waiting();
  }
  if (running)
    pad_log("shutdown: %ld polls, %.1f ms apart at least, %ld in one second at most", polls, min_gap * 1000,
            max_sec_polls);
  if (running && no_retire)
    pad_log("shutdown: a callback stays inside gpSP (noretire), shutdown_ticks %u", (unsigned)os_config.shutdown_ticks);
  shut_down = 1;
  running = 0;
  queued = 0;
  memset(slots, 0, sizeof(slots));
}

int usbpad_next_event(struct usbpad_event *ev)
{
  if (!queued)
    return 0;
  *ev = queue[0];
  memmove(queue, queue + 1, sizeof(queue[0]) * --queued);
  return 1;
}

/* The library's texts (usbpad_core.c). */
int usbpad_event_text(const struct usbpad_event *ev, char *buf, unsigned size)
{
  switch (ev->type)
  {
  case USBPAD_EV_CONNECTED:
    return snprintf(buf, size, "Controller %d connected (%s)", ev->player + 1, usbpad_kind_name(ev->kind));
  case USBPAD_EV_DISCONNECTED:
    return snprintf(buf, size, "Controller %d disconnected", ev->player + 1);
  case USBPAD_EV_REPLUG_NEEDED:
    return snprintf(buf, size, "Plug the controller in again");
  case USBPAD_EV_NEEDS_POWER:
    return snprintf(buf, size, "Controller may need more power");
  case USBPAD_EV_UNSUPPORTED:
    return snprintf(buf, size, "Controller not supported (%04x:%04x)", ev->vid, ev->pid);
  case USBPAD_EV_FAILED:
    return snprintf(buf, size, "Controller stopped responding (%04x:%04x)", ev->vid, ev->pid);
  }
  return snprintf(buf, size, "%s", "");
}

const char *usbpad_kind_name(int kind)
{
  return kind > 0 && kind < USBPAD_KIND_COUNT ? kind_names[kind] : kind_names[0];
}

void usbpad_get_config(struct usbpad_config *cfg)
{
  *cfg = config;
}

int usbpad_set_config(const struct usbpad_config *cfg)
{
  if (cfg->on_pct < 1 || cfg->on_pct > 100 || cfg->off_pct >= cfg->on_pct)
    return USBPAD_EINVAL;
  config = *cfg;
  return 0;
}

void usbpad_os_get_config(struct usbpad_os_config *cfg)
{
  *cfg = os_config;
}

void usbpad_os_set_config(const struct usbpad_os_config *cfg)
{
  os_config = *cfg;
}

void usbpad_set_hooks(void (*sleep_tick)(void), int (*port_probe)(void))
{
  hook_sleep = sleep_tick;
  hook_probe = port_probe;
}

#endif
