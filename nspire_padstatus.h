/* PAD3 (2026-11-11): what the USB port, the OS and the controller driver
 * are doing, in plain words, for the Controller status page (nspire_padpage.c),
 * the "not working" toast and the pad log (nspire_padlog.c).
 *
 * A snapshot (struct pad_status) is read from the controller's registers
 * (nspire_usb_raw), the port observer, the arbiter, the link and the pad
 * glue (nspire_usbpad_status); nspire_padstatus_verdict says, from the
 * snapshot alone, which one thing stands in the way; nspire_padstatus_lines
 * gives the page's lines. Both are pure (the PC tests feed them every
 * state). */
#ifndef NSPIRE_PADSTATUS_H
#define NSPIRE_PADSTATUS_H

#include <stdint.h>
#include "nspire_os.h"

/* PAD4: the come-up's steps (the glue's, nspire_usbpad_glue.c): the OS's
 * host controller running, the device connected, the port in reset, the
 * port enabled, the OS's offer, the driver's attach, the pad's first report. */
enum { CU_HC, CU_CONN, CU_RESET, CU_ENABLED, CU_OFFER, CU_ATTACH, CU_REPORT, PADSTATUS_STEPS };

/* What the pad driver says (the glue fills it; without a driver: started 0). */
struct pad_driver_view
{
  /* PAD4: the last come-up (a device onto the adapter's wires): each step's
   * ms after the plug + 1 (0 not seen), whether one ever began, is under way,
   * and its age */
  uint8_t cu_on, cu_live;
  uint32_t cu_step[PADSTATUS_STEPS];
  uint32_t cu_age_ms;
  int8_t started;          /* 1 registered with the OS, 0 not built or not started, -1 its init failed */
  int8_t pads;             /* pads attached now */
  int8_t slot;             /* the first attached pad's slot, -1 none */
  int8_t score;            /* the latest offer's answer: > 0 taken, 0 no class wanted it, < 0 USBPAD_OS_NO_* */
  uint8_t offered;         /* the OS offered a device since the device came (the latest offer is its) */
  uint8_t iface, cls, sub, proto;   /* the latest offer: interface level (1) or device level (0), its class */
  uint8_t event;           /* the last driver event since the port changed (USBPAD_EV_*), 0 none */
  uint8_t gave_up;         /* the glue's 12 s wait for an offer ran out */
  uint8_t not_pad;         /* R11u: the driver keeps an interface of this device quietly as not a
                              controller (a sound card's volume keys, a keyboard): not a failure */
  uint8_t hub;             /* hub (2026-11-16): a USB hub is on the adapter (nspire_usbhub_present) */
  uint8_t hub_busy;        /* ...and the OS is at work on a device behind it right now */
  uint8_t os_stuck;        /* the OS's USB has stopped: it enables the port and offers nothing any more
                              (the glue's watch_stuck) */
  uint8_t silent;          /* (r19-usb) a pad is attached and has sent no report in the PAD_SILENT_MS after
                              its attach (the glue's note_input): attached is not working */
  uint16_t vid, pid;       /* the latest offer */
  uint16_t ev_vid, ev_pid; /* that event's device */
  uint32_t matches, attaches;   /* the OS's calls into the driver, in all */
  char name[24];           /* the attached pad's kind ("Xbox 360"), or "" */
  char status[40];         /* the driver's own status word (usbpad_status), or "" */
  char detail[64];         /* current protocol step, or the failed attach's step */
};

enum ps_role
{
  PS_ROLE_NONE,     /* nothing plugged in */
  PS_ROLE_HOST,     /* the adapter's end: this calculator is the USB host */
  PS_ROLE_DEVICE,   /* the cable's plain end: the other side is the host */
  PS_ROLE_UNKNOWN   /* the controller doesn't read (off, gated) */
};

struct pad_status
{
  int kind;                 /* nspire_usb_raw: 2 CX II FOTG210, 1 CX ChipIdea, 0 not readable */
  int gated;                /* a CX's bus access to the controller is off */
  int plugged;              /* nspire_usb_plugged: 1, 0, -1 */
  uint32_t raw[NSPIRE_USB_RAW];
  /* decoded from the above (nspire_padstatus_decode) */
  int role;                 /* enum ps_role */
  int power;                /* the host's end: VBUS valid; the device's end: B session */
  int connected, enabled;   /* PORTSC bits 0 and 2 */
  int lines;                /* PORTSC bits 11:10: 0 SE0 (nothing on the wires), 1 K, 2 J */
  int suspended;            /* PAD4: PORTSC bit 7 (both controllers: suspend); in reset (bit 8) */
  int resetting;
  int running;              /* USBCMD bit 0: the OS's host (or device) controller runs */
  /* the rest of gpSP */
  struct pad_driver_view drv;
  int owner, stage, cycles; /* the arbiter (enum arb_owner) */
  int link;                 /* nspire_link_state() */
  int link_cable;           /* nspire_link_cable_present(): linked, looking, or the other gpSP's node seen */
  uint32_t age_ms;          /* since the port observer last saw a change */
  uint32_t stage_ms;        /* since the arbiter's stage began */
};

enum pad_verdict
{
  PV_NOTHING,         /* nothing plugged in */
  PV_UNREADABLE,      /* the controller is off */
  PV_DEVICE_END,      /* this calculator has the cable's plain end */
  PV_LINKED,          /* the link to the other calculator is up */
  PV_CALCULATOR,      /* the device is a calculator (TI's id, or its NavNet node) */
  PV_WORKING,         /* a controller is attached */
  PV_NO_DRIVER,       /* the pad driver isn't running */
  PV_NO_POWER,        /* the adapter's end, but no bus power */
  PV_EMPTY_ADAPTER,   /* bus power, nothing on the data wires */
  PV_HOST_STOPPED,    /* a device on the wires, the OS's host controller stopped */
  PV_NOT_ENABLED,     /* the host runs, the port not enabled */
  PV_NOT_OFFERED,     /* the port enabled, the OS offered the driver nothing */
  PV_STARTING,        /* the driver took the offer, the pad is starting */
  PV_NOT_A_PAD,       /* offered, no driver class wanted it, or the driver keeps it quietly as not a
                         controller (R11u: not a failing verdict, no toast) */
  PV_REFUSED,         /* offered, refused for another reason (score < 0) */
  PV_NEEDS_POWER,     /* the driver's "may need more power" */
  PV_UNSUPPORTED,     /* the driver's "not supported" */
  PV_FAILED,          /* it worked and stopped answering */
  PV_OTHER,           /* a USB drive's driver, or the pad driver's own USB mode, has the port */
  PV_NOT_ATTACHED,    /* R7: the driver took the offer, the OS never attached it (the glue's 12 s wait ran out) */
  PV_SUSPENDED,       /* PAD4: the port enabled and suspended (PORTSC bit 7), nothing offered: R10a after the Switch Pro */
  PV_HUB,             /* hub (2026-11-16): a USB hub is in and no controller on it (sound or a drive may be):
                         not a failing verdict, no toast: the port's bits say nothing of what a hub carries */
  PV_OS_STUCK,        /* 2026-11-16: the OS's USB has stopped (two plugs in a row enabled and never offered,
                         after its USB had worked): the adapter out and in again; quit restarts the calculator */
  PV_SILENT,          /* (r19-usb) a controller is attached and has sent nothing since (r19's CX: an 8BitDo
                         "working" for 23 s with 0 reports): not a failing verdict (an untouched pad that reports
                         only what changes is silent too), no toast; its first report makes it PV_WORKING */
  PV_POWER_HELD,      /* (r19-usb) the adapter's end, its power turned on (a CX II's OTGCSR A_BUS_REQ set and
                         A_BUS_DROP clear, a CX's PORTSC port power) and not there (no A VBUS): what is plugged
                         in holds it down, or the port's supply failed; PV_NO_POWER is the power not asked for */
  PV_COUNT
};

/* The live snapshot. A few register reads and compares: the glue takes one
 * every PADSTATUS_POLL_MS at most. */
void nspire_padstatus_read(struct pad_status *s);
/* role ... running from kind, gated, plugged and raw (pure). */
void nspire_padstatus_decode(struct pad_status *s);
enum pad_verdict nspire_padstatus_verdict(const struct pad_status *s);
/* "host_stopped": the verdict's name for logs and tests. */
const char *nspire_padstatus_name(enum pad_verdict v);
/* The verdict in plain words (what is wrong, what to do), one sentence or two. */
const char *nspire_padstatus_cause(enum pad_verdict v);
/* 1 for the verdicts where something is plugged in at the adapter's end and
 * nothing works (the toast's, the pad log's timer). */
int nspire_padstatus_failing(enum pad_verdict v);
/* R11w part 2: the Controller page's top, one or two plain sentences: what
 * is plugged in, whether it works, what to do (no register or driver words:
 * those are its Details). */
const char *nspire_padstatus_plain(enum pad_verdict v);
#define PADSTATUS_CHECKING "Checking the USB port..."

/* The page's lines: a label and a value each, PADSTATUS_LINES of them. */
#define PADSTATUS_LINES 9
#define PADSTATUS_LABEL 16
#define PADSTATUS_VALUE 64
void nspire_padstatus_lines(const struct pad_status *s, char label[PADSTATUS_LINES][PADSTATUS_LABEL],
                            char value[PADSTATUS_LINES][PADSTATUS_VALUE]);
/* PAD4: the come-up's times in one line ("HC 0.0 port 0.3 offer 0.3 pad 0.3
 * input 0.4 s"), the page's "Since plug". */
void nspire_padstatus_steps_text(const struct pad_driver_view *d, char *text, unsigned size);
/* The raw words in one line ("OTGCSR 000e0010 OTGISR ..."). */
void nspire_padstatus_raw_text(const struct pad_status *s, char *text, unsigned size);

/* The glue's part of the snapshot (nspire_usbpad_glue.c; weak default: no driver). */
void nspire_usbpad_status(struct pad_driver_view *d);

/* The game's frame (the glue, nspire_usbpad_frame): the toast "not working"
 * once a failing verdict has been the same for PADSTATUS_STABLE_MS (an
 * adapter with nothing on its wires: failing PADSTATUS_TOAST_EMPTY_MS), not
 * in a plug's first PADSTATUS_PLUG_QUIET_MS, not while the link is picked or
 * a calculator is seen, not while the arbiter still works on it; once per
 * plug (R11w part 2: its second time at 20 s is gone, and gpSP's own power
 * restart is no new plug). Fed by nspire_padstatus_note (the glue's poll,
 * with each new snapshot's verdict). */
#define PADSTATUS_POLL_MS        250
#define PADSTATUS_STABLE_MS      2000
#define PADSTATUS_PLUG_QUIET_MS  1000
#define PADSTATUS_TOAST_EMPTY_MS 8000
#define PADSTATUS_TOAST_STAGE0_MS 3500   /* the arbiter's stage 0 younger than this may still cycle */
#define PADSTATUS_TOAST_TEXT     "USB problem: Settings > Controller"
void nspire_padstatus_note(const struct pad_status *s, enum pad_verdict v);
/* The toast's text when one is due now (and marks it said), else NULL. */
const char *nspire_padstatus_toast_due(void);
/* R11w part 2: one message that asks the user to do something per plug, for
 * the whole USB side: may one be said now (none yet this plug, the plug's
 * first second over), and one was said (the arbiter's last resort). */
int nspire_padstatus_may_ask(void);
void nspire_padstatus_asked(void);
/* The latest verdict noted (PV_NOTHING before any). */
enum pad_verdict nspire_padstatus_last(void);
void nspire_padstatus_reset(void);   /* the tests */

#endif
