/* gpSP's build of nspire-usbms's usbms_nspire.c (2026-11-15, R11u). TI's OS
 * (CX II 6.4.0.74) offers nothing more on a host session after one attach
 * that returns an error following a nonzero match: R11u's sound card, whose
 * HID interface the pad driver refused once, left six later plug-ins
 * unoffered for 705 s, until the adapter came out. usbms_nspire.c's attach
 * returns 6 (ENXIO) after its match said 4 in these cases: the driver's
 * state changed between its match and its attach (stopping, a drive already
 * attached, a pipe still closing), an interface with more than 16 endpoints,
 * a mass-storage interface without a bulk IN and a bulk OUT endpoint, and
 * usbd_open_pipe / usbd_alloc_xfer failing (it closes what it opened first).
 *
 * So here its attach is wrapped: whatever the real one returns, the OS gets
 * 0. A refused one is kept quiet: the real attach has already closed what it
 * opened and set nothing (no generation, no attached_self), so no drive
 * appears, usbms_platform_owns_usb() says what it said, the device's detach
 * reaches the real detach, which finds it is not its device and does
 * nothing. The kept attaches are counted for the pad log (nspire_padlog.c:
 * "drive: ... kept quietly"). Everything else is usbms_nspire.c exactly:
 * this file includes it and wraps only the attach it registers, as
 * nspire_audio_os.c does with the audio driver's match and attach. It runs
 * in the OS's USB task like the function it wraps (no Ndless syscall: only
 * the resolved OS call ivars, checked as usbms_nspire.c checks it), and
 * holds usbms_nspire.c's own callback count meanwhile, so that
 * usbms_platform_stop() waits for it too. The proper home of the rule is
 * usbms_nspire.c's attach (nspire-usbms, not this folder's to change).
 *
 * UNVERIFIED on hardware: that TI's OS treats a kept attach that opened
 * nothing as a bound driver and goes on offering (the pad driver's quiet
 * keep is the same bet; R11t's sessions with only successful attaches kept
 * offering). */
#include <stdint.h>

#if defined(USBMS_OS_TEST)
#define USBSHIM_TEST 1
#endif
#include "usbshim.h"

/* usbms_nspire.c's one call of usbshim_register, as a function-like macro
 * (usbshim.h's declaration came first, included above). */
static usbshim_t *gpsp_ms_register(const char *name, usbshim_fn match, usbshim_fn attach, usbshim_fn detach,
                                   unsigned softc_size);
#define usbshim_register(...) gpsp_ms_register(__VA_ARGS__)
#include "usbms_nspire.c"
#undef usbshim_register

static usbshim_fn ms_real_attach;
static volatile uint32_t ms_kept_n, ms_kept_ids;
static volatile int32_t ms_kept_err;
static volatile uint8_t ms_kept_if;

static int gpsp_ms_attach(device_t self)
{
  int r;
  uint32_t prev_gen = generation;

  callback_enter();
  r = ms_real_attach(self);
  if (r != 0 || (!prev_gen && !generation))
  {
    /* (its match said yes: TI's OS calls attach only then) */
    const struct attach_arg *u = ram(self, 24, 4) && api.ivars ? api.ivars(self) : NULL;

    if (u && ram(u, sizeof *u, sizeof(void *)))
    {
      ms_kept_ids = (uint32_t)(uint16_t)u->vendor << 16 | (uint16_t)u->product;
      ms_kept_if = (uint8_t)u->ifaceno;
    }
    ms_kept_err = r ? r : 6;
    BARRIER();
    ms_kept_n = ms_kept_n + 1;
    r = 0;
  }
  callback_leave();
  return r;
}

static usbshim_t *gpsp_ms_register(const char *name, usbshim_fn match, usbshim_fn attach, usbshim_fn detach,
                                   unsigned softc_size)
{
  ms_real_attach = attach;
  return usbshim_register(name, match, gpsp_ms_attach, detach, softc_size);
}

/* MAIN: how many drive attaches were kept quiet instead of refused since
 * gpSP started, and the latest one's vid << 16 | pid, interface and the
 * error the real attach gave. */
unsigned nspire_drive_kept(uint32_t *ids, unsigned *ifno, int *err)
{
  unsigned n = ms_kept_n;

  BARRIER();
  if (ids)
    *ids = ms_kept_ids;
  if (ifno)
    *ifno = ms_kept_if;
  if (err)
    *err = (int)ms_kept_err;
  return n;
}

/* (2026-11-17) MAIN: the drive driver's own account of what the OS offered
 * it (usbms_platform_diag): how many mass storage interfaces so far, and
 * of the latest: vid << 16 | pid, its interface, why match left it (0: it
 * took it; usbms_platform.h USBMS_WHY_*), and the driver's matches,
 * attaches and failed attaches so far. On a test CX II the driver said
 * no to a SanDisk three times and nothing said so. */
unsigned nspire_drive_offers(uint32_t *ids, unsigned *ifno, unsigned *why, unsigned *counts)
{
#ifdef USBMS_HAS_DIAG
  usbms_platform_diag_t g;

  usbms_platform_diag(&g);
  if (ids)
    *ids = g.ids;
  if (ifno)
    *ifno = g.iface;
  if (why)
    *why = g.why;
  if (counts)
    *counts = (g.matches & 0xFFu) | (g.attaches & 0xFFu) << 8 | (g.attach_fails & 0xFFu) << 16 | (uint32_t)g.step << 24;
  return g.storage_offers;
#else
  (void)ids; (void)ifno; (void)why; (void)counts;
  return 0;
#endif
}

/* R11w (2026-11-15): MAIN, at exit: a callback of the drive driver is inside
 * gpSP's code (usbms_nspire.c's own count). Its usbms_platform_stop waits
 * for that without an end (a loop on it): nspire_drive.c waits for it here
 * first, within the exit's budget, and calls the stop only once it is 0. */
unsigned nspire_drive_os_callbacks(void)
{
  return callbacks;
}
