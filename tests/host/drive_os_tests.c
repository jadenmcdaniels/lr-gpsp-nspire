/* R11u (2026-11-15): gpSP's build of nspire-usbms's TI layer
 * (nspire_drive_os.c, which includes usbms_nspire.c and wraps its attach)
 * on a fake of the OS calls it makes. TI's OS offers nothing more on a host
 * session after one attach that returns an error following a nonzero match
 * (R11u's log); usbms_nspire.c's own attach returns 6 after its match said 4
 * for a mass-storage interface without bulk endpoints, with more than 16
 * endpoints, or when the OS refuses a pipe. Checked here: the OS always
 * gets 0 from the registered attach, a refused one is kept quietly (no drive
 * attached, nothing left open, the USB port not claimed, counted with its
 * ids for the pad log), its detach does nothing, and a good drive attaches
 * after each. (nspire-usbms's own tests/os_test.c checks the transfers; this
 * one only the attach.)
 *
 *   drive_os_tests */
#define USBSHIM_TEST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usbshim.h"
#include "usbshim_usb.h"
#include "usbms_platform.h"

unsigned nspire_drive_kept(uint32_t *ids, unsigned *ifno, int *err);

static int checks, failures;
#define CHECK(c, ...)                                                                                                  \
  do                                                                                                                   \
  {                                                                                                                    \
    checks++;                                                                                                          \
    if (!(c))                                                                                                          \
    {                                                                                                                  \
      failures++;                                                                                                      \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                      \
      printf(__VA_ARGS__);                                                                                             \
      printf("\n");                                                                                                    \
    }                                                                                                                  \
  } while (0)

/* ---- the shim: what the OS gets is what was registered ---- */

struct usbshim
{
  usbshim_fn match, attach, detach;
  usbshim_cb cb;
  int retired;
};
static struct usbshim shim;
static int registered;

usbshim_t *usbshim_register(const char *name, usbshim_fn m, usbshim_fn a, usbshim_fn d, unsigned size)
{
  (void)name;
  (void)size;
  registered++;
  shim.match = m;
  shim.attach = a;
  shim.detach = d;
  shim.retired = 0;
  return &shim;
}
static void resident(void *x, void *p, unsigned s)
{
  if (!shim.retired && shim.cb)
    shim.cb(x, p, s);
}
usbshim_cb usbshim_callback(usbshim_t *s, usbshim_cb cb)
{
  s->cb = cb;
  return resident;
}
void usbshim_retire(usbshim_t *s) { s->retired = 1; }

/* ---- the OS: one device, one interface ---- */

static uint32_t self[6], dev, ifc, pipe_in, pipe_out;
static struct { int port, configno, ifaceno, vendor, product, release; void *dev, *iface; } args;
static uint8_t dd[18] = { 18, 1, 0, 2, 0, 0, 0, 64, 0x81, 0x07, 0x81, 0x55 };   /* 0781:5581 */
static uint8_t id[9] = { 9, 4, 0, 0, 2, 8, 6, 0x50, 0 };                        /* 08/06/50 */
static uint8_t ep[17][7];
static unsigned open_n, opened, refuse_open_ep, xfer_n;
static uint32_t xfer_block[16];

static void interface_with(unsigned n, int bulk)
{
  unsigned i;

  id[4] = (uint8_t)n;
  for (i = 0; i < n && i < 17; i++)
  {
    uint8_t e[7] = { 7, 5, (uint8_t)(i & 1 ? 0x02 : 0x81), (uint8_t)(bulk ? 2 : 3), 64, 0, 0 };
    memcpy(ep[i], e, 7);
  }
}

int usbms_os_test_ram(const void *p, unsigned size)
{
  const struct { const void *p; unsigned size; } ok[] = {
    { self, sizeof self }, { &dev, sizeof dev }, { &ifc, sizeof ifc }, { &args, sizeof args }, { dd, sizeof dd },
    { id, sizeof id },
  };
  unsigned i;

  for (i = 0; i < sizeof ok / sizeof ok[0]; i++)
    if (p == ok[i].p && size <= ok[i].size)
      return 1;
  for (i = 0; i < 17; i++)
    if (p == ep[i] && size <= 7)
      return 1;
  return 0;
}
static void *f_ivars(void *p) { return p == self ? (void *)&args : NULL; }
static const uint8_t *f_iface_desc(void *p) { return p == &ifc ? id : NULL; }
static unsigned f_iface2dev(void *p, void **d)
{
  *d = &dev;
  return p == &ifc ? 0 : 5;
}
static const uint8_t *f_iface2ep(void *p, unsigned i) { return p == &ifc && i < 17 ? ep[i] : NULL; }
static const uint8_t *f_dev_desc(void *p) { return p == &dev ? dd : NULL; }
static unsigned f_open_pipe(void *p, unsigned e, unsigned flags, void **out)
{
  (void)p;
  (void)flags;
  if (refuse_open_ep && e == refuse_open_ep)
    return 11;   /* NOT SUPPORTED */
  *out = e & 0x80 ? (void *)&pipe_in : (void *)&pipe_out;
  open_n++;
  opened++;
  return 0;
}
static unsigned f_close_pipe(void *p)
{
  (void)p;
  open_n--;
  return 0;
}
static unsigned f_abort_pipe(void *p) { (void)p; return 0; }
static void *f_alloc_xfer(void *d) { (void)d; xfer_n++; return xfer_block; }
static unsigned f_free_xfer(void *x) { (void)x; xfer_n--; return 0; }
static void f_setup_xfer(void *x, void *p, void *priv, void *buffer, uint32_t len, unsigned flags, uint32_t timeout,
                         usbshim_cb cb)
{
  (void)x; (void)p; (void)priv; (void)buffer; (void)len; (void)flags; (void)timeout; (void)cb;
}
static unsigned f_transfer(void *x) { (void)x; return 5; }
static void f_xfer_status(void *x, void **priv, void **buffer, uint32_t *count, uint8_t *status)
{
  (void)x;
  *priv = NULL;
  *buffer = NULL;
  *count = 0;
  *status = 5;
}
static unsigned f_do_request(void *d, void *raw, void *data) { (void)d; (void)raw; (void)data; return 0; }
static unsigned f_clear_stall(void *p) { (void)p; return 0; }
static const char *f_errstr(unsigned s)
{
  static const char *const names[] = { "NORMAL COMPLETION", "OPERATION NOT STARTED", "OPERATION IN PROGRESS",
    "OPERATION NOT PERMITTED", "NO SUCH ENTRY", "INPUT/OUTPUT ERROR", "DEVICE NOT CONFIGURED",
    "FAILED ALLOCATING MEMORY", "RESOURCE IS BUSY", "NO SUCH DEVICE", "INVALID ARGUMENT",
    "OPERATION NOT SUPPORTED", "OPERATION TIMED OUT", "DEVICE IS SUSPENDED", "GENERAL PURPOSE ERROR",
    "LOGICAL TEST FAILURE", "INCORRECT STATE", "PIPE IS STALLED", "INVALID PARAMETER", "OPERATION ABORTED",
    "SHORT TRANSFER" };
  return s < 21 ? names[s] : "bad";
}
uintptr_t usbms_os_test_address(unsigned n)
{
  switch (n)
  {
  case 260: return (uintptr_t)f_ivars;
  case 248: return (uintptr_t)f_iface_desc;
  case 240: return (uintptr_t)f_iface2dev;
  case 235: return (uintptr_t)f_iface2ep;
  case 250: return (uintptr_t)f_dev_desc;
  case 227: return (uintptr_t)f_open_pipe;
  case 228: return (uintptr_t)f_close_pipe;
  case 236: return (uintptr_t)f_abort_pipe;
  case 230: return (uintptr_t)f_alloc_xfer;
  case 231: return (uintptr_t)f_free_xfer;
  case 232: return (uintptr_t)f_setup_xfer;
  case 229: return (uintptr_t)f_transfer;
  case 234: return (uintptr_t)f_xfer_status;
  case 245: return (uintptr_t)f_do_request;
  case 237: return (uintptr_t)f_clear_stall;
  case 254: return (uintptr_t)f_errstr;
  }
  return 0;
}
void usbms_os_test_sleep(void) {}

/* ---- the OS's offer: match, then attach only after a yes (TI's way) ---- */

static int offers, attaches, refused;

static int offer(void)
{
  int m, a;

  offers++;
  m = shim.match(self);
  if (m <= 0)
    return m;
  attaches++;
  a = shim.attach(self);
  if (a != 0)
    refused++;   /* TI's rule broken: the OS would offer nothing more */
  return m;
}

static void good_drive_attaches(const char *after)
{
  interface_with(2, 1);
  refuse_open_ep = 0;
  CHECK(offer() == 4 && usbms_platform_attached() != 0 && usbms_platform_owns_usb(), "after %s: a good drive attaches",
        after);
  CHECK(open_n == 2 && xfer_n == 1, "after %s: its two pipes and its transfer (%u, %u)", after, open_n, xfer_n);
  shim.detach(self);
  CHECK(!usbms_platform_attached() && open_n == 0 && xfer_n == 0, "after %s: its detach closes them (%u, %u)", after,
        open_n, xfer_n);
}

int main(void)
{
  uint32_t ids = 0;
  unsigned ifno = 9, kept;
  int err = 0;

  args.dev = &dev;
  args.iface = &ifc;
  args.vendor = 0x0781;
  args.product = 0x5581;
  args.ifaceno = 0;
  CHECK(usbms_platform_start() == 0 && registered == 1, "the drive driver registers");
  CHECK(nspire_drive_kept(NULL, NULL, NULL) == 0, "nothing kept yet");
  good_drive_attaches("the start");

  /* a mass-storage interface with interrupt endpoints only: no bulk pair */
  interface_with(2, 0);
  CHECK(offer() == 4, "08/06/50 without bulk endpoints: the match says yes");
  kept = nspire_drive_kept(&ids, &ifno, &err);
  CHECK(kept == 1 && ids == 0x07815581u && ifno == 0 && err == 6, "...its attach would have said 6: kept (%u, %08x, %u, %d)",
        kept, (unsigned)ids, ifno, err);
  CHECK(!usbms_platform_attached() && !usbms_platform_owns_usb() && open_n == 0, "...no drive, nothing open, the port not claimed");
  shim.detach(self);
  CHECK(!usbms_platform_attached() && open_n == 0, "...its detach does nothing");
  good_drive_attaches("no bulk endpoints");

  /* more than 16 endpoints */
  interface_with(17, 1);
  CHECK(offer() == 4 && nspire_drive_kept(NULL, NULL, &err) == 2 && err == 6 && !usbms_platform_attached(),
        "17 endpoints: kept quietly (%d)", err);
  shim.detach(self);
  good_drive_attaches("17 endpoints");

  /* the OS refuses the OUT pipe: the IN pipe it opened is closed again */
  interface_with(2, 1);
  refuse_open_ep = 0x02;
  opened = 0;
  CHECK(offer() == 4 && nspire_drive_kept(NULL, NULL, NULL) == 3 && opened == 1 && open_n == 0 && xfer_n == 0 &&
        !usbms_platform_owns_usb(), "a refused pipe: kept quietly, nothing left open (%u opened, %u open)", opened, open_n);
  shim.detach(self);
  good_drive_attaches("a refused pipe");

  CHECK(refused == 0, "the OS never got an error from an attach after a yes (%d of %d)", refused, attaches);
  usbms_platform_stop();
  CHECK(shim.retired, "stopped: retired");
  printf("drive_os_tests: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
