/* S4: the controller side's bus power cycle (nspire_os.h,
 * nspire_usb_reenumerate), in a file of its own so that the PC tests run
 * this very code on simulated registers (tests/host: host_usbpad.c's
 * controller model for gpsp-host, frontend_tests.c's for the unit tests).
 *
 * An adapter plugged in before gpSP started is never offered to the USB
 * driver (usbshim.h): the OS enumerated it with nobody registered. Dropping
 * the bus power (the OTG A-device's VBUS) and raising it again makes the OS
 * enumerate again, now with the driver registered. Each write is one store
 * (gpSP's IRQs are masked: nothing runs between its read and its write);
 * the 300 ms between them are OS ticks (nspire_os_sleep: the OS's USB task
 * needs them, and the calculator never spins with IRQs off).
 *
 * CX II FOTG210: OTGCSR bit 5 A_BUS_DROP, bit 4 A_BUS_REQ, bit 19 A_VBUS
 * (read back); every other bit is kept: read anew before each write (the
 * OS's own bits 9-11 and 13 are set at the host's end, 'OTGCSR 000e2e10' in
 * the r2/r4 logs, and its OTG interrupt may rewrite the word in the drop).
 * CX ChipIdea: PORTSC bit 12 PP, read anew before each write; CSC, PEC and
 * OCC (1, 3, 5: write 1 to clear) are written 0 so that nothing is cleared,
 * PE (2: a 0 disables, a 1 can't enable) is written 0 on purpose. Only in
 * host mode (USBMODE 3) with the ID pin's pull-up on (without it ID reads 0,
 * which looks like the host's end).
 *
 * What is safe if a hardware fact is false (UNVERIFIED, S10):
 *  - a CX whose timers don't run: nspire_ticks stands still in an OS sleep
 *    (fallback_ticks moves only in nspire_wait_until): no cycle at all, and
 *    every wait is bounded by a count of OS sleeps as well (SEAM-1);
 *  - the CX's OS gates the controller off when the port looks empty (PP 0
 *    does that): the gate is checked before every read and write; gated in
 *    the drop, nothing is written to it, and PP is put back at the first
 *    look after the gate opens if the ID pin still says the host's end
 *    (nspire_usb_power_restore, the arbiter's every tick) (SEAM-7);
 *  - whether the drop cuts VBUS at all (the CX's VBUS may be on GPIO 2:
 *    an unconfirmed note): the log line has the registers before, in
 *    the drop and after, and says whether VBUS fell and the device left
 *    (SEAM-9). VBUS not back within 500 ms: 0, and the arbiter makes no
 *    second cycle. */
#include <stdio.h>
#include "common.h"
#include "nspire.h"
#include "nspire_os.h"
#include "nspire_link.h"

#ifdef NSPIRE_HOST
uint32_t host_usb_reg_read(uint32_t addr);
void host_usb_reg_write(uint32_t addr, uint32_t value);
#define RD(a) host_usb_reg_read(a)
#define WR(a, v) host_usb_reg_write(a, v)
#else
#define RD(a) (*(volatile u32 *)(a))
#define WR(a, v) (*(volatile u32 *)(a) = (v))
#endif

#define USB_BASE          0xB0000000u
#define FOTG_USBCMD       (USB_BASE + 0x10)    /* bit 0 run */
#define FOTG_PORTSC       (USB_BASE + 0x30)    /* bit 0 connected */
/* (2026-11-17) The register TI's port suspend sets bit 8 of after PORTSC's
 * suspend bit, and only its port resume clears (OS 6.4.0.74: 10a0e800,
 * 10a0eafc): the transceiver's own suspend. */
#define FOTG_MISC         (USB_BASE + 0x40)
#define MISC_PHY_SUSPEND  (1u << 8)
#define PORTSC_SUSPEND    (1u << 7)
#define PORTSC_RESET      (1u << 8)
#define FOTG_OTGCSR       (USB_BASE + 0x80)
#define OTGCSR_A_BUS_REQ  (1u << 4)
#define OTGCSR_A_BUS_DROP (1u << 5)
#define OTGCSR_A_VBUS     (1u << 19)
#define OTGCSR_DEV_B      (1u << 21)
#define CI_PORTSC         (USB_BASE + 0x184)
#define CI_OTGSC          (USB_BASE + 0x1A4)
#define CI_USBMODE        (USB_BASE + 0x1A8)
#define PORTSC_CCS        (1u << 0)
#define PORTSC_PE         (1u << 2)
#define PORTSC_W1C        0x2Au                /* CSC, PEC, OCC */
#define PORTSC_PP         (1u << 12)
/* PAD3: the lines (bits 11:10, J or K): a device's pull-up is there even
 * when the port never latched "connected" (R9F's CX II after a link:
 * PORTSC 00000800 with the OS's host controller stopped). */
#define PORTSC_LINES      0xC00u
#define OTGSC_IDPU        (1u << 5)
#define OTGSC_ID          (1u << 8)
#define OTGSC_AVV         (1u << 9)

#define REENUM_DROP_MS    300
#define REENUM_VBUS_MS    500
#define OS_TICK_MS        10   /* the OS's 100 Hz tick (nspire_os.h) */

/* The link's fast transport (nspire_link_fast.c) holds a word of the
 * controller's or its interrupt line: no cycle then, whatever asked (weak:
 * a build or a test without it never holds one). */
int link_fast_holds(void) __attribute__((weak, noipa));
int link_fast_holds(void) { return 0; }

static unsigned sleeps;        /* OS sleeps this cycle */
static int restore_pending;    /* a CX gated in the drop: PP to put back (SEAM-7) */

/* The controller's reads are stale (a CX's gated off, or not the one it was). */
static int gone(int kind)
{
  return nspire_usb_gated() || nspire_usb_kind() != kind;
}

/* ms of OS time, until (reg & mask) == want if mask: ends at the time or
 * after ms / OS_TICK_MS + 4 OS sleeps, whichever comes first (ticks that
 * stand still can't hold it). 1 the bits came, 0 not, -1 the controller's
 * gone (nothing read from it). */
static int wait_for(int kind, unsigned ms, u32 reg, u32 mask, u32 want)
{
  u32 t0 = nspire_ticks(), ticks = (u32)((u64)ms * NSPIRE_TICK_HZ / 1000);
  unsigned n = 0, most = ms / OS_TICK_MS + 4;

  for (;;)
  {
    if (mask)
    {
      if (gone(kind))
        return -1;
      if ((RD(reg) & mask) == want)
        return 1;
    }
    if (nspire_ticks() - t0 >= ticks || n >= most || !nspire_os_sleep())
      break;
    n++;
    sleeps++;
  }
  if (!mask)
    return gone(kind) ? -1 : 0;
  return gone(kind) ? -1 : (RD(reg) & mask) == want;
}

static unsigned ms_since(u32 t0)
{
  return (unsigned)((u64)(nspire_ticks() - t0) * 1000 / NSPIRE_TICK_HZ);
}

/* (2026-11-17) The CX II's transceiver left asleep.
 *
 * TI's stack suspends what nobody uses (nspire-usbshim usbshim_awake.h): the
 * port's suspend bit, then the transceiver's (FOTG_MISC bit 8). Only its
 * resume of that port clears the transceiver's bit, and no resume comes
 * when the suspended device is pulled out, when the bus power is dropped
 * under it, or after the controller's own resume ended the port's suspend
 * first (10a0e3f8: the root hub's resume then finds the port not suspended
 * and returns before the bit, 10a0e8f4). The next device is reset on a
 * sleeping transceiver: "connected, never enabled", or enabled and nothing
 * it is asked gets through (R10a, R11u, the 2026-11-16 and 2026-11-17 logs:
 * every "USB stopped until the adapter came out").
 *
 * nspire_usb_phy_asleep(): 1 the bit is set at the host's end, 0 not, -1
 * not this controller (a CX's ChipIdea has no such bit) or not readable.
 * nspire_usb_phy_wake(): clears it (the register's other bits as read)
 * unless the OS holds a device suspended on the port right now (connected,
 * enabled, PORTSC's suspend bit: its resume clears the bit itself), or
 * resets the port this moment. 1 woken, 0 nothing to do (or not now). One
 * read and one store with gpSP's IRQs masked: nothing of the OS's runs
 * between them. */
int nspire_usb_phy_asleep(void)
{
  if (nspire_usb_kind() != 2 || !nspire_os_ready() || nspire_usb_gated())
    return -1;
  if (RD(FOTG_OTGCSR) & OTGCSR_DEV_B)
    return 0;
  return (RD(FOTG_MISC) & MISC_PHY_SUSPEND) != 0;
}

int nspire_usb_phy_wake(void)
{
  u32 port, misc;

  if (nspire_usb_phy_asleep() <= 0 || link_fast_holds())
    return 0;
  port = RD(FOTG_PORTSC);
  if (port & PORTSC_RESET)
    return 0;
  if ((port & (PORTSC_CCS | PORTSC_PE | PORTSC_SUSPEND)) == (PORTSC_CCS | PORTSC_PE | PORTSC_SUSPEND))
    return 0;
  misc = RD(FOTG_MISC);
  WR(FOTG_MISC, misc & ~MISC_PHY_SUSPEND);
  nspire_link_log("usb: the transceiver was asleep with %s (PORTSC %08lx USBCMD %08lx, register 40 %08lx): woken, "
                  "now %08lx", port & PORTSC_CCS ? "a device waiting on it" : "nothing on the port", (unsigned long)port,
                  (unsigned long)RD(FOTG_USBCMD), (unsigned long)misc, (unsigned long)RD(FOTG_MISC));
  return 1;
}

int nspire_usb_reenumerate(int attempt)
{
  int kind = nspire_usb_kind(), valid, drop, woke = 0;
  u32 t0 = nspire_ticks(), v, port0, csr0, port1 = 0, csr1 = 0, port2 = 0, csr2 = 0;
  const char *name = kind == 2 ? "fotg" : "ci";

  if (kind == 0 || !nspire_os_ready() || nspire_usb_gated())
    return -2;
  if (link_fast_holds())
  {
    nspire_link_log("reenum: cycle %d refused: the fast link holds the USB controller", attempt);
    return -2;
  }
  if (!nspire_timer_runs())
  {
    nspire_link_log("reenum: cycle %d refused: the timer doesn't run (the waits would never end)", attempt);
    return -2;
  }
  sleeps = 0;
  if (kind == 2)
  {
    csr0 = RD(FOTG_OTGCSR);
    port0 = RD(FOTG_PORTSC);
    /* the device's end, or nothing connected and nothing on the lines (PAD3: a
     * pull-up on them is a device too; before, it was "nothing to cycle") */
    if ((csr0 & OTGCSR_DEV_B) || !(port0 & (PORTSC_CCS | PORTSC_LINES)))
      return -1;
    v = RD(FOTG_OTGCSR);
    WR(FOTG_OTGCSR, (v & ~(OTGCSR_A_BUS_DROP | OTGCSR_A_BUS_REQ)) | OTGCSR_A_BUS_DROP);
    wait_for(kind, REENUM_DROP_MS, 0, 0, 0);
    csr1 = RD(FOTG_OTGCSR);
    port1 = RD(FOTG_PORTSC);
    /* (2026-11-17) in the drop, with the device off the port: a transceiver
     * the OS's suspend left asleep is woken before the power goes back
     * (what the OS resets next must find it awake) */
    woke = nspire_usb_phy_wake();
    v = RD(FOTG_OTGCSR);   /* (power goes back even if a sleep failed) */
    WR(FOTG_OTGCSR, (v & ~(OTGCSR_A_BUS_DROP | OTGCSR_A_BUS_REQ)) | OTGCSR_A_BUS_REQ);
    valid = wait_for(kind, REENUM_VBUS_MS, FOTG_OTGCSR, OTGCSR_A_VBUS, OTGCSR_A_VBUS);
    csr2 = RD(FOTG_OTGCSR);
    port2 = RD(FOTG_PORTSC);
    drop = !(csr1 & OTGCSR_A_VBUS);
  }
  else
  {
    csr0 = RD(CI_OTGSC);
    port0 = RD(CI_PORTSC);
    /* not the host mode, or no ID pull-up: can't tell; the device's end, or nothing connected */
    if (!(csr0 & OTGSC_IDPU) || (RD(CI_USBMODE) & 3) != 3)
      return -2;
    if ((csr0 & OTGSC_ID) || !(port0 & (PORTSC_CCS | PORTSC_LINES)))
      return -1;
    v = RD(CI_PORTSC);
    WR(CI_PORTSC, v & ~(PORTSC_W1C | PORTSC_PE | PORTSC_PP));
    if (wait_for(kind, REENUM_DROP_MS, 0, 0, 0) < 0)
    {
      /* Gated off with the power down: a write now would be lost (or
       * worse); PP goes back when the gate opens (nspire_usb_power_restore). */
      restore_pending = 1;
      nspire_link_log("reenum: cycle %d (ci): the controller was gated off in the drop: PP is still 0, put back when the "
                      "gate opens; before OTGSC %08lx PORTSC %08lx; %u ms, %u OS sleeps", attempt, (unsigned long)csr0,
                      (unsigned long)port0, ms_since(t0), sleeps);
      return 0;
    }
    csr1 = RD(CI_OTGSC);
    port1 = RD(CI_PORTSC);
    v = RD(CI_PORTSC);
    WR(CI_PORTSC, (v & ~(PORTSC_W1C | PORTSC_PE)) | PORTSC_PP);
    valid = wait_for(kind, REENUM_VBUS_MS, CI_OTGSC, OTGSC_AVV, OTGSC_AVV);
    if (valid >= 0)
    {
      csr2 = RD(CI_OTGSC);
      port2 = RD(CI_PORTSC);
    }
    drop = !(csr1 & OTGSC_AVV);
  }
  nspire_link_log("reenum: cycle %d (%s): before %s %08lx PORTSC %08lx; in the drop %s %08lx PORTSC %08lx (VBUS %s, "
                  "device %s%s); after %s %08lx PORTSC %08lx%s; vbus_valid=%d, took %u ms, %u OS sleeps", attempt, name,
                  kind == 2 ? "OTGCSR" : "OTGSC", (unsigned long)csr0, (unsigned long)port0,
                  kind == 2 ? "OTGCSR" : "OTGSC", (unsigned long)csr1, (unsigned long)port1, drop ? "off" : "still on",
                  port1 & 1 ? "still connected" : "gone", woke ? "; the sleeping transceiver woken" : "",
                  kind == 2 ? "OTGCSR" : "OTGSC", (unsigned long)csr2,
                  (unsigned long)port2, valid < 0 ? " (gated off: stale)" : "", valid > 0, ms_since(t0), sleeps);
  if (valid <= 0)
    nspire_link_log("reenum: VBUS not valid %u ms after the power went back: no further cycle", REENUM_VBUS_MS);
  return valid > 0 ? 1 : 0;
}

int nspire_usb_power_restore_pending(void) { return restore_pending; }

int nspire_usb_power_restore(void)
{
  u32 v;

  if (!restore_pending || nspire_usb_kind() != 1 || nspire_usb_gated())
    return 0;
  restore_pending = 0;
  v = RD(CI_OTGSC);
  if ((v & OTGSC_ID) || !(v & OTGSC_IDPU) || (RD(CI_USBMODE) & 3) != 3 || (RD(CI_PORTSC) & PORTSC_PP))
  {
    nspire_link_log("reenum: the gate opened: PP left as it is (OTGSC %08lx PORTSC %08lx)", (unsigned long)v,
                    (unsigned long)RD(CI_PORTSC));
    return 0;
  }
  v = RD(CI_PORTSC);
  WR(CI_PORTSC, (v & ~(PORTSC_W1C | PORTSC_PE)) | PORTSC_PP);
  nspire_link_log("reenum: the gate opened: PP put back (PORTSC was %08lx, now %08lx)", (unsigned long)v,
                  (unsigned long)RD(CI_PORTSC));
  return 1;
}
