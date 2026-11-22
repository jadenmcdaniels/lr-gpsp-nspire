/* gpSP's build of nspire-usbaudio's ua_os.c (2026-11-15, controller fix
 * F1). That driver takes the first 01/02 (audio streaming) interface of any
 * device but a calculator. The DualShock 4 and the DualSense are also USB
 * sound cards (interfaces 0-2 audio, 3 the gamepad: the pad driver's corpus
 * tests/corpus/cfg_sony_real.h), and TI's OS offers 0-2 first. With the
 * audio driver on (gpSP's default), the game's sound would go to the pad's
 * headset jack (the DualSense is a high-speed device: an isochronous stream
 * that driver has never run), and while it holds the pad gpSP's audio
 * counts as active the whole session: the arbiter reads OTHER (the
 * come-up's OS-time burst ends early), link-log flushes and pad-log writes
 * are held, the halt watch is off. The controller comes first.
 *
 * So here a device the pad driver knows by vendor:product
 * (usbpad_known_kind: Sony's five, the Switch Pro, ...) is declined at
 * match time, and the decline is counted for the pad log. Everything else
 * is ua_os.c exactly: this file includes it and wraps only the match and
 * attach it registers. Without the pad driver in the build
 * (usbpad_known_kind absent: a weak reference) nothing is declined.
 *
 * The wrapper runs in the OS's USB task like the functions it wraps: no
 * Ndless syscall, only ua_os.c's resolved OS calls; it holds ua_os.c's own
 * `callbacks` count while it runs so ua_shutdown() waits for it. The
 * proper home of this check is ua_os.c's match_body (nspire-usbaudio; not
 * this folder's to change). */
#include <stdint.h>

#if defined(UA_HOST_TEST)
#define USBSHIM_TEST 1
#endif
#include "usbshim.h"

/* ua_os.c's one call of usbshim_register, as a function-like macro: its
 * `if (!usbshim_register)` (host builds) and its #pragma weak still name the
 * real one; usbshim.h's declaration came first (included above). */
static usbshim_t *gpsp_ua_register(const char *name, usbshim_fn match, usbshim_fn attach, usbshim_fn detach,
                                   unsigned softc_size);
#define usbshim_register(...) gpsp_ua_register(__VA_ARGS__)
#include "ua_os.c"
#undef usbshim_register

int usbpad_known_kind(uint16_t vid, uint16_t pid) __attribute__((weak));

static usbshim_fn ua_real_match, ua_real_attach;
static volatile uint32_t declined_n, declined_ids;
static volatile uint8_t declined_if;

/* The offer is a game controller the pad driver takes by its ids. */
static int pad_device(device_t self, const struct os_attach_arg **out)
{
    const struct os_attach_arg *u;
    int (*known)(uint16_t, uint16_t) = usbpad_known_kind;

    *out = NULL;
    if (!known || !api.ivars || !(u = api.ivars(self)))
        return 0;
    *out = u;
    return known((uint16_t) u->vendor, (uint16_t) u->product) != 0;
}

static int gpsp_ua_match(device_t self)
{
    const struct os_attach_arg *u;
    int r;

    callbacks++;
    barrier();
    r = ua_real_match(self);        /* its own step log and probe count, as before */
    if (r > 0 && pad_device(self, &u)) {
        declined_ids = (uint32_t) (uint16_t) u->vendor << 16 | (uint16_t) u->product;
        declined_if = (uint8_t) u->ifaceno;
        barrier();
        declined_n = declined_n + 1;
        r = 0;
    }
    barrier();
    callbacks--;
    return r;
}

/* TI's OS does not call attach after a match of 0 (R10a: matches=4,
 * attaches=2). Should it anyway, the pad's interface is still not taken:
 * attach returns success and does nothing (2026-11-15, R11u: on this OS one
 * attach that returns an error makes it offer no further device until the
 * whole adapter is pulled; detach of it is ua_os.c's no-op for an offer
 * that isn't att_self). */
static int gpsp_ua_attach(device_t self)
{
    const struct os_attach_arg *u;
    int r;

    callbacks++;
    barrier();
    r = pad_device(self, &u) ? 0 : ua_real_attach(self);
    barrier();
    callbacks--;
    return r;
}

static usbshim_t *gpsp_ua_register(const char *name, usbshim_fn match, usbshim_fn attach, usbshim_fn detach,
                                   unsigned softc_size)
{
    ua_real_match = match;
    ua_real_attach = attach;
    return usbshim_register(name, gpsp_ua_match, gpsp_ua_attach, detach, softc_size);
}

/* MAIN: how many audio interfaces were left to a pad since gpSP started,
 * and the latest one's vid << 16 | pid and interface number. */
unsigned nspire_audio_declined(uint32_t *ids, unsigned *ifno)
{
    unsigned n = declined_n;
    barrier();
    if (ids)
        *ids = declined_ids;
    if (ifno)
        *ifno = declined_if;
    return n;
}
