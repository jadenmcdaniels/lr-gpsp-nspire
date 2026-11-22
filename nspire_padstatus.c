/* PAD3: the Controller status (nspire_padstatus.h). */
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "nspire.h"
#include "nspire_padstatus.h"
#include "nspire_usbport.h"
#include "nspire_arbiter.h"
#include "nspire_link.h"
#include "nspire_padlog.h"

/* The driver's events (usbpad.h's USBPAD_EV_*: this file builds without it). */
#define EV_CONNECTED   1
#define EV_NEEDS_POWER 4
#define EV_UNSUPPORTED 5
#define EV_FAILED      6
/* Its refusal reasons (nspire-usbpad src/backend_os.h USBPAD_OS_NO_*). */
#define NO_TI (-2)

/* Without the pad glue (a build without pads, the unit tests): no driver. */
void nspire_usbpad_status(struct pad_driver_view *d) __attribute__((weak, noipa));
void nspire_usbpad_status(struct pad_driver_view *d)
{
  memset(d, 0, sizeof(*d));
  d->slot = -1;
}

int nspire_usbms_active(void) __attribute__((weak, noipa));
int nspire_usbms_active(void) { return 0; }
int nspire_usbaudio_active(void) __attribute__((weak, noipa));
int nspire_usbaudio_active(void) { return 0; }
int nspire_usbaudio_owns_usb(void) __attribute__((weak, noipa));
int nspire_usbaudio_owns_usb(void) { return 0; }
int nspire_bt_usb_active(void) __attribute__((weak, noipa));
int nspire_bt_usb_active(void) { return 0; }

void nspire_padstatus_decode(struct pad_status *s)
{
  const uint32_t *w = s->raw;

  s->role = PS_ROLE_UNKNOWN;
  s->power = s->connected = s->enabled = s->lines = s->running = 0;
  if (s->plugged == 0)
    s->role = PS_ROLE_NONE;
  if (s->kind == 2)
  {
    /* OTGCSR bit 21: the ID pin (1 the device's end), 19 A VBUS valid, 17 B session. */
    if (s->plugged != 0)
      s->role = w[0] & (1u << 21) ? PS_ROLE_DEVICE : PS_ROLE_HOST;
    s->power = s->role == PS_ROLE_HOST ? (w[0] >> 19 & 1) : (w[0] >> 17 & 1);
  }
  else if (s->kind == 1)
  {
    /* OTGSC bit 8: the ID pin (1 the device's end), 9 A VBUS valid, 11 B session valid. */
    if (s->plugged != 0)
      s->role = w[0] & (1u << 8) ? PS_ROLE_DEVICE : PS_ROLE_HOST;
    s->power = s->role == PS_ROLE_HOST ? (w[0] >> 9 & 1) : (w[0] >> 11 & 1);
  }
  else
    return;
  s->running = w[2] & 1;
  s->connected = w[4] & 1;
  s->enabled = w[4] >> 2 & 1;
  s->lines = w[4] >> 10 & 3;
  s->suspended = w[4] >> 7 & 1;
  s->resetting = w[4] >> 8 & 1;
}

void nspire_padstatus_read(struct pad_status *s)
{
  memset(s, 0, sizeof(*s));
  s->kind = nspire_usb_raw(s->raw);
  s->gated = nspire_usb_gated();
  s->plugged = nspire_usb_plugged();
  nspire_padstatus_decode(s);
  nspire_usbpad_status(&s->drv);
  s->owner = nspire_arb_owner();
  s->stage = nspire_arb_stage();
  s->cycles = nspire_arb_cycles();
  s->stage_ms = nspire_arb_stage_ms();
  s->link = nspire_link_state();
  s->link_cable = nspire_link_cable_present();
  s->age_ms = nspire_usbport_age_ms();
}

enum pad_verdict nspire_padstatus_verdict(const struct pad_status *s)
{
  const struct pad_driver_view *d = &s->drv;

  if (s->role == PS_ROLE_NONE)
    return PV_NOTHING;
  if (s->role == PS_ROLE_UNKNOWN)
    return PV_UNREADABLE;
  if (s->link == LINK_LINKED)
    return PV_LINKED;
  if (s->role == PS_ROLE_DEVICE)
    return PV_DEVICE_END;
  /* The adapter's end. */
  if (d->pads > 0)
    return d->silent ? PV_SILENT : PV_WORKING;
  /* The OS's USB has stopped: whatever is plugged in now gets no further
   * (and what the lines below would say of it is not its fault). */
  if (d->os_stuck)
    return PV_OS_STUCK;
  /* Hub: no controller on it and none failing (a failed one's own verdict
   * stays until it is pulled out of the hub), none starting either: the
   * verdicts below read one device off the port's bits, which a hub holds
   * still for as long as it is in ("empty adapter" never, "not offered"
   * and "another driver has the port" wrongly). */
  if (d->hub && !d->event && !(d->offered && d->score > 0))
    return PV_HUB;
  if (s->owner == ARB_OTHER && !d->event)
    return PV_OTHER;
  if ((d->offered && (d->vid == 0x0451 || d->score == NO_TI)) || (s->link_cable && s->link == LINK_OFF))
    return PV_CALCULATOR;
  if (d->started <= 0)
    return PV_NO_DRIVER;
  if (d->event == EV_FAILED)
    return PV_FAILED;
  if (d->event == EV_NEEDS_POWER)
    return PV_NEEDS_POWER;
  if (d->event == EV_UNSUPPORTED)
    return PV_UNSUPPORTED;
  /* R11u: the driver keeps it quietly as not a controller (taken at match,
   * then its report descriptor said no gamepad): not "starting", nor "no
   * attach in 12 s" */
  if (d->not_pad)
    return PV_NOT_A_PAD;
  if (!s->power)
    return (s->kind == 2 && (s->raw[0] & 0x30u) == 0x10u) || (s->kind == 1 && (s->raw[4] >> 12 & 1)) ? PV_POWER_HELD :
           PV_NO_POWER;
  /* R7: taken, and no pad came in the glue's 12 s wait (no driver event
   * either: those are above): not "starting" for ever. */
  if (d->offered && d->score > 0 && d->gave_up)
    return PV_NOT_ATTACHED;
  if (d->offered)
    return d->score > 0 ? PV_STARTING : d->score == 0 ? PV_NOT_A_PAD : PV_REFUSED;
  if (!s->connected && !s->lines)
    return PV_EMPTY_ADAPTER;
  if (!s->running)
    return PV_HOST_STOPPED;
  if (!s->enabled)
    return PV_NOT_ENABLED;
  /* PAD4 (R10a): from the moment the OS stopped its host controller after
   * the Switch Pro (685064, PORTSC 0000088d) every enabled reading had the
   * suspend bit, none before; and the OS offered no device again. */
  if (s->suspended)
    return PV_SUSPENDED;
  return PV_NOT_OFFERED;
}

static const char *const names[PV_COUNT] = {
  "nothing", "unreadable", "device_end", "linked", "calculator", "working", "no_driver", "no_power",
  "empty_adapter", "host_stopped", "not_enabled", "not_offered", "starting", "not_a_pad", "refused",
  "needs_power", "unsupported", "failed", "other", "not_attached", "suspended", "hub", "os_stuck", "silent",
  "power_held"
};

/* Each at most three lines of the page (~50 characters a line). */
static const char *const causes[PV_COUNT] = {
  /* nothing */
  "Nothing is plugged in. Put the OTG adapter in this calculator and the controller in the adapter.",
  /* unreadable */
  "The calculator's USB controller is off: nothing is plugged in, or the OS switched it off.",
  /* device_end */
  "This calculator has the plain end of the cable. A controller needs the adapter end: the OTG "
  "adapter goes in THIS calculator.",
  /* linked */
  "Linked to the other calculator.",
  /* calculator */
  "The device is another calculator: use Trade / Link.",
  /* working */
  "The controller works.",
  /* no_driver */
  "gpSP's controller driver isn't running on this calculator (see Driver).",
  /* no_power */
  "The adapter is in, but the port gives no power: unplug the adapter and plug it in again.",
  /* empty_adapter */
  "The adapter has power, nothing on data wires: plug the controller in, or try another cable (not charge-only).",
  /* host_stopped */
  "A device is on the adapter but the OS hasn't started USB (its host controller is stopped). "
  "With Link off, press enter.",
  /* not_enabled */
  "The OS sees the device but hasn't finished starting it (the port isn't enabled). Wait, or with Link off "
  "press enter.",
  /* not_offered */
  "The OS started the device but didn't offer it to gpSP's driver (an OS driver may have it).",
  /* starting */
  "The driver took the controller: starting it.",
  /* not_a_pad (R11u: not a failure; the driver leaves it alone) */
  "Not a controller: the device isn't one this driver knows (see OS reported). It is left alone; plug a controller "
  "in to play.",
  /* refused */
  "The OS offered the device and the driver refused it (see Driver).",
  /* needs_power */
  "The controller may need more power than the calculator gives: try a powered hub or another "
  "controller.",
  /* unsupported */
  "The controller was found but didn't start (Driver: where it stopped). Unplug it; the next one gets a fresh "
  "try.",
  /* failed */
  "The controller worked and stopped answering: unplug it and plug it in again.",
  /* other */
  "Another driver has the port (a USB drive, or the controller driver's own USB mode).",
  /* not_attached */
  "The driver took the controller, but the OS never started it (no attach in 12 s): unplug it and plug it in "
  "again.",
  /* suspended */
  "The OS left the port suspended and can't talk to this device. Unplug and replug the OTG adapter.",
  /* hub */
  "A USB hub is in, with no controller on it. Plug a controller into the hub; sound and a drive can stay in.",
  /* os_stuck */
  "The calculator's USB has stopped. Take the OTG adapter out and put it back in. If that fails, quit gpSP.",
  /* silent */
  "Attached, but the controller has sent nothing yet: press a button, or unplug it and plug it in again.",
  /* power_held */
  "The port's power stays down: what is plugged in draws too much (a charging controller). Use a powered hub, "
  "or Bluetooth."
};

/* R11w part 2 (2026-11-15): the page's top, for a beginner: what is plugged
 * in, whether it works, what to do; no register or driver words (those are
 * the Details below it, and the pad log's "status:" lines keep `causes`). */
static const char *const plain[PV_COUNT] = {
  /* nothing */       "Nothing plugged in. Put the OTG adapter here, then the controller.",
  /* unreadable */    "Nothing plugged in. Put the OTG adapter here, then the controller.",
  /* device_end */    "Wrong cable end (plain end). A controller needs the OTG adapter here.",
  /* linked */        "Linked to the other calculator.",
  /* calculator */    "Another calculator is on the cable: use Trade / Link.",
  /* working */       "The controller works.",
  /* no_driver */     "Controllers can't be used on this calculator's OS.",
  /* no_power */      "The adapter gets no power. Take it out and put it back in.",
  /* empty_adapter */ "Adapter in, no controller. Plug one in (data cable, not charge-only).",
  /* host_stopped */  "Device in, USB hasn't started yet. Wait, or press enter to restart USB.",
  /* not_enabled */   "The device is starting. Wait, or press enter to restart USB.",
  /* not_offered */   "The calculator kept this device from gpSP. Unplug and replug it.",
  /* starting */      "Starting the controller...",
  /* not_a_pad */     "This device isn't a controller. Plug a controller in to play.",
  /* refused */       "gpSP can't use this device. Plug a controller in to play.",
  /* needs_power */   "The controller may need more power. Try a powered USB hub.",
  /* unsupported */   "The controller didn't start. Unplug it and plug it in again.",
  /* failed */        "The controller stopped. Unplug it and plug it in again.",
  /* other */         "A USB drive has the port. Unplug it to use a controller.",
  /* not_attached */  "The controller didn't start. Unplug it and plug it in again.",
  /* suspended */     "USB is stuck. Take the OTG adapter out and put it back in.",
  /* hub */           "USB hub in. Plug a controller into the hub to use one.",
  /* os_stuck */      "USB has stopped. Take the OTG adapter out and put it back in.",
  /* silent */        "Connected, but nothing has come from it. Press a button, or unplug and replug it.",
  /* power_held */    "Not enough power for what is plugged in. Use a hub with its own power, or Bluetooth."
};

/* Hub: what else is on it, said with it (a hub carries a controller, the
 * sound and a drive at once: none of them "has the port"). */
static const char *hub_plain(void)
{
  int ms = nspire_usbms_active();
  int au = nspire_usbaudio_active() || nspire_usbaudio_owns_usb();
  if (ms && au)
    return "USB hub in, with sound and a drive. Plug a controller into the hub too.";
  if (au)
    return "USB hub in, with USB sound. Plug a controller into the hub too.";
  if (ms)
    return "USB hub in, with a USB drive. Plug a controller into the hub too.";
  if (nspire_bt_usb_active())
    return "USB hub in, with the Bluetooth dongle. Plug a controller into the hub too.";
  return plain[PV_HUB];
}

static const char *other_plain(void)
{
  int ms = nspire_usbms_active();
  int au = nspire_usbaudio_active() || nspire_usbaudio_owns_usb();
  if (ms && au)
    return "USB sound and a drive have the port. Unplug one to use a controller.";
  if (au)
    return "USB sound has the port. Unplug it or plug both into a USB hub.";
  if (nspire_bt_usb_active())
    return "The Bluetooth dongle has the port: its controllers work (Settings > Bluetooth).";
  return plain[PV_OTHER];
}

static const char *other_cause(void)
{
  int ms = nspire_usbms_active();
  int au = nspire_usbaudio_active() || nspire_usbaudio_owns_usb();
  if (ms && au)
    return "Other drivers have the port (USB sound and a USB drive).";
  if (au)
    return "Another driver has the port (USB sound).";
  if (nspire_bt_usb_active())
    return "Another driver has the port (Bluetooth).";
  return causes[PV_OTHER];
}

const char *nspire_padstatus_plain(enum pad_verdict v)
{
  if (v == PV_HUB)
    return hub_plain();
  if (v == PV_OTHER)
    return other_plain();
  return (unsigned)v < PV_COUNT ? plain[v] : "";
}

const char *nspire_padstatus_name(enum pad_verdict v)
{
  return (unsigned)v < PV_COUNT ? names[v] : "?";
}

const char *nspire_padstatus_cause(enum pad_verdict v)
{
  if (v == PV_OTHER)
    return other_cause();
  return (unsigned)v < PV_COUNT ? causes[v] : "";
}

int nspire_padstatus_failing(enum pad_verdict v)
{
  switch (v)
  {
  case PV_OS_STUCK:
  case PV_NO_DRIVER:
  case PV_NO_POWER:
  case PV_POWER_HELD:
  case PV_EMPTY_ADAPTER:
  case PV_HOST_STOPPED:
  case PV_NOT_ENABLED:
  case PV_NOT_OFFERED:
  /* (PV_NOT_A_PAD is not one since R11u, 2026-11-15: a device that is
   * simply not a controller is no failure, no "USB not working" toast) */
  case PV_REFUSED:
  case PV_NEEDS_POWER:
  case PV_UNSUPPORTED:
  case PV_FAILED:
  case PV_NOT_ATTACHED:
  case PV_SUSPENDED:
    return 1;
  default:
    return 0;
  }
}

static const char *refusal(int score)
{
  switch (score)
  {
  case -1: return "the driver was stopping";
  case -2: return "TI calculator (link)";
  case -3: return "descriptors unreadable";
  case -4: return "no attach arguments";
  case -5: return "a bad device handle";
  case -6: return "no vendor/product id";
  }
  return "refused";
}

static const char *owner_text(int owner)
{
  switch (owner)
  {
  case ARB_WAIT: return "nobody yet";
  case ARB_PAD: return "the controller";
  case ARB_LINK: return "the link";
  case ARB_OTHER: return "another driver";
  }
  return "nobody";
}

void nspire_padstatus_lines(const struct pad_status *s, char label[PADSTATUS_LINES][PADSTATUS_LABEL],
                            char value[PADSTATUS_LINES][PADSTATUS_VALUE])
{
  const struct pad_driver_view *d = &s->drv;
  int host = s->role == PS_ROLE_HOST, n = 0;

#define LINE(l, ...) (snprintf(label[n], PADSTATUS_LABEL, "%s", l), snprintf(value[n], PADSTATUS_VALUE, __VA_ARGS__), n++)
  LINE("Port", "%s", s->role == PS_ROLE_NONE ? "nothing plugged in" :
                     s->role == PS_ROLE_HOST ? "host: the adapter's end" :
                     s->role == PS_ROLE_DEVICE ? "device end: use adapter end" :
                     s->gated ? "can't tell: gated off" : "can't tell: doesn't read");
  if (s->role == PS_ROLE_HOST || s->role == PS_ROLE_DEVICE)
    LINE("Bus power", "%s", s->power ? "on" : "off");
  else
    LINE("Bus power", "-");
  if (host)
    LINE("Device", "%s", s->connected ? "connected" : s->lines ? "on wires, not connected" :
                         "nothing on the data wires");
  else
    LINE("Device", "-");
  if (host)
    LINE("Port enabled", "%s", s->enabled ? (s->suspended ? "yes, but suspended" : "yes") :
                               s->resetting ? "no: the OS is resetting it" : "no");
  else
    LINE("Port enabled", "-");
  if (s->kind)
    LINE("OS USB", "%s %s", s->role == PS_ROLE_DEVICE ? "device controller" : "host controller",
         s->running ? "running" : "stopped");
  else
    LINE("OS USB", "-");
  if (d->offered)
    LINE("OS reported", "%04x:%04x, %s %02x/%02x/%02x", d->vid, d->pid, d->iface ? "interface" : "device",
         d->cls, d->sub, d->proto);
  else
    LINE("OS reported", "%s", d->matches ? "nothing since this device came" : "none reported by the OS");
  if (d->started < 0)
    LINE("Driver", "failed (%s)", d->status[0] ? d->status : "?");
  else if (d->started == 0)
    LINE("Driver", "not running");
  else if (d->detail[0])
    LINE("Driver", "%s", d->detail);
  else if (d->pads > 0)
    LINE("Driver", "took it: %s (slot %d)", d->name, d->slot + 1);
  else if (d->event == EV_NEEDS_POWER)
    LINE("Driver", "needs more power (%04x:%04x)", d->ev_vid, d->ev_pid);
  else if (d->event == EV_UNSUPPORTED)
    LINE("Driver", "not supported (%04x:%04x)", d->ev_vid, d->ev_pid);
  else if (d->event == EV_FAILED)
    LINE("Driver", "stopped answering (%04x:%04x)", d->ev_vid, d->ev_pid);
  else if (d->offered && d->score > 0)
    LINE("Driver", d->gave_up ? "took it (%d): no attach 12 s" : "took it (score %d): starting", d->score);
  else if (d->offered && d->score == 0)
    LINE("Driver", "said no: not a controller");
  else if (d->offered)
    LINE("Driver", "said no: %s", refusal(d->score));
  else
    LINE("Driver", "%s", d->gave_up ? "no offer from the OS in 12 s" : "waiting for OS offer");
  if (s->link == LINK_LINKED)
    LINE("Port owner", "the link (linked)");
  else if (s->link == LINK_LOOKING)
    LINE("Port owner", "the link (looking for GBA)");
  else if (s->owner == ARB_WAIT && s->cycles)
    LINE("Port owner", "nobody yet (%d restart%s)", s->cycles, s->cycles > 1 ? "s" : "");
  else
    LINE("Port owner", "%s", owner_text(s->owner));
  {
    char steps[PADSTATUS_VALUE];

    nspire_padstatus_steps_text(d, steps, sizeof(steps));
    LINE("Since plug", "%s", steps);
  }
#undef LINE
}

/* PAD4: "HC 0.0 offer 0.3 pad 0.3 input 0.4 s" (the seconds from the plug
 * to the host controller running, the OS's offer, the driver's attach, the
 * pad's first report; "-" not seen; from 10 s on whole seconds), or while it
 * is under way "HC 9.9 offer - pad - (12 s so far)"; "-" before any device
 * came. (The pad log has every step, the port's reset and enable too.) */
void nspire_padstatus_steps_text(const struct pad_driver_view *d, char *text, unsigned size)
{
  static const int shown[] = { CU_HC, CU_OFFER, CU_ATTACH, CU_REPORT };
  static const char *const names[] = { "HC", "off", "pad", "in" };
  unsigned i, n = 0, steps = d->cu_live ? 3 : 4;

  if (!d->cu_on)
  {
    snprintf(text, size, "-");
    return;
  }
  text[0] = 0;
  for (i = 0; i < steps && n < size; i++)
  {
    uint32_t v = d->cu_step[shown[i]];

    if (!v)
      n += (unsigned)snprintf(text + n, size - n, "%s%s -", i ? " " : "", names[i]);
    else if (v - 1 < 9950)
      n += (unsigned)snprintf(text + n, size - n, "%s%s %u.%u", i ? " " : "", names[i], (unsigned)((v - 1 + 50) / 1000),
                              (unsigned)((v - 1 + 50) / 100 % 10));
    else
      n += (unsigned)snprintf(text + n, size - n, "%s%s %u", i ? " " : "", names[i], (unsigned)((v - 1 + 500) / 1000));
  }
  if (n < size)
  {
    if (d->cu_live)
      snprintf(text + n, size - n, " (%us)", (unsigned)(d->cu_age_ms / 1000));
    else
      snprintf(text + n, size - n, " s");
  }
}

void nspire_padstatus_raw_text(const struct pad_status *s, char *text, unsigned size)
{
  const uint32_t *w = s->raw;

  if (s->kind == 2)
    snprintf(text, size, "OTGCSR %08lx OTGISR %08lx USBCMD %08lx USBSTS %08lx PORTSC %08lx GISR %08lx",
             (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3],
             (unsigned long)w[4], (unsigned long)w[5]);
  else if (s->kind == 1)
    snprintf(text, size, "OTGSC %08lx USBMODE %08lx USBCMD %08lx USBSTS %08lx PORTSC %08lx USBINTR %08lx",
             (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3],
             (unsigned long)w[4], (unsigned long)w[5]);
  else
    snprintf(text, size, "the controller doesn't read%s", s->gated ? " (gated off)" : "");
}

/* ---- The toast ---- */

static enum pad_verdict last = PV_NOTHING;
static int failing, told;          /* told: this plug's one action message was said (R11w part 2: once per plug) */
static u32 failing_since;
static u32 verdict_at, plug_at;    /* R11w part 2: when the verdict last changed, the last plug */
static int empty_only;             /* the failing stretch is an empty adapter so far */
static u32 port_counter;
static int arb_speaks;
static int device_seen;            /* a device was at the adapter's end since it came to the host's end */
static u32 wires_counter;          /* nspire_padlog_wires_counter at the last note */

void nspire_padstatus_note(const struct pad_status *s, enum pad_verdict v)
{
  u32 counter = nspire_usbport_changed_counter();
  int fail;

  /* An adapter left with nothing in it after a device was there (pulled
   * out, SEAM-5) is not a failure to toast about; only an adapter that
   * never had one on its wires (a charge-only cable). */
  if (s->role != PS_ROLE_HOST)
    device_seen = 0;
  else if (v != PV_EMPTY_ADAPTER && v != PV_NO_POWER && v != PV_POWER_HELD)
    device_seen = 1;
  fail = nspire_padstatus_failing(v) && s->link == LINK_OFF && !s->link_cable &&
         !(v == PV_EMPTY_ADAPTER && device_seen);

  /* A new port state, or the port through nothing: a new try, toasts again. */
  if (v == PV_NOTHING || v == PV_UNREADABLE || v == PV_DEVICE_END || v == PV_WORKING || v == PV_SILENT ||
      v == PV_LINKED || v == PV_CALCULATOR)
    told = 0;
  if (counter != port_counter && !failing && !nspire_arb_cycling())
  {
    told = 0;
    plug_at = nspire_ticks();
  }
  port_counter = counter;
  /* R1: a new device onto the wires of an adapter that stays in (the port's
   * bits may not move: the host controller stopped) is a new try too.
   * R11w part 2: not gpSP's own power restart's drop and return (R11w's
   * "USB not working" every 7 s for one stuck device: each restart made
   * it a new try). */
  if (nspire_padlog_wires_counter() != wires_counter)
  {
    wires_counter = nspire_padlog_wires_counter();
    if (!nspire_arb_cycling())
    {
      told = 0;
      failing = 0;   /* its failing stretch starts now */
      plug_at = nspire_ticks();
    }
  }
  if (v != last)
    verdict_at = nspire_ticks();
  if (fail && !failing)
  {
    failing_since = nspire_ticks();
    empty_only = 1;
  }
  if (fail && v != PV_EMPTY_ADAPTER)
    empty_only = 0;
  failing = fail;
  last = v;
  /* An unclaimed device's stages say their own (Starting the USB device...,
   * Unplug and replug, Swap the cable): this one not while they cycle (1-3)
   * or may still (stage 0 younger than PADSTATUS_TOAST_STAGE0_MS). Once they
   * gave up (4) it follows their last word (R1: it was held off for good
   * there, so the page was never pointed to; the glue shows it only once
   * the line is free). */
  arb_speaks = s->owner == ARB_WAIT && ((s->stage >= ARB_STAGE_CYCLE && s->stage < ARB_STAGE_GAVE_UP) ||
                                        (s->stage == ARB_STAGE_WAIT_OFFER && s->stage_ms < PADSTATUS_TOAST_STAGE0_MS));
}

static u32 ms_since_t(u32 at)
{
  return (u32)((u64)(nspire_ticks() - at) * 1000 / NSPIRE_TICK_HZ);
}

/* R11w part 2: the USB side's one message that asks the user to do
 * something, per plug: the toast below and the arbiter's last resort. */
int nspire_padstatus_may_ask(void)
{
  return !told && ms_since_t(plug_at) >= PADSTATUS_PLUG_QUIET_MS;
}

void nspire_padstatus_asked(void)
{
  told = 1;
}

const char *nspire_padstatus_toast_due(void)
{
  u32 ms;

  /* R11w part 2: once per plug (no second toast at 20 s), only once the
   * failing verdict has been the same for PADSTATUS_STABLE_MS (the empty
   * adapter's: PADSTATUS_TOAST_EMPTY_MS of failing), never in a plug's
   * first PADSTATUS_PLUG_QUIET_MS, never while the arbiter still works on it */
  if (!failing || arb_speaks || !nspire_padstatus_may_ask())
    return NULL;
  ms = ms_since_t(failing_since);
  if (empty_only ? ms < PADSTATUS_TOAST_EMPTY_MS : ms_since_t(verdict_at) < PADSTATUS_STABLE_MS)
    return NULL;
  told = 1;
  return PADSTATUS_TOAST_TEXT;
}

enum pad_verdict nspire_padstatus_last(void)
{
  return last;
}

void nspire_padstatus_reset(void)
{
  last = PV_NOTHING;
  failing = told = empty_only = arb_speaks = device_seen = 0;
  failing_since = port_counter = 0;
  verdict_at = plug_at = nspire_ticks() - NSPIRE_TICK_HZ * 10;   /* (no plug's quiet second pending) */
  wires_counter = nspire_padlog_wires_counter();
}
