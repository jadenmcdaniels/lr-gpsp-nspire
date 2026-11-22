/* The fast transport (nspire_link_fast.h): the two calculators' USB
 * controllers driven by gpSP itself, polled, on an endpoint of our own,
 * while NavNet stays open beside it, idle, as the way back.
 *
 * Why (the R2-R5 logs): a NavNet write costs
 * 40-70 ms whatever its size and a round trip 84-500 ms. NavNet is stop and
 * wait (libnspire: each data packet answered by an ack packet of the same
 * seq before the next), and both the data and the ack are handled by an OS
 * task that runs only when its calculator's gpSP sleeps an OS tick (100 Hz)
 * and the host's OS polls its bulk IN: two OSes' schedulers each way. No
 * other NavNet call avoids that: the ack is the other OS's.
 *
 * What runs instead, the CX II as USB host (adapter end) and the CX (CAS)
 * as device (cable end), as the first trade linked:
 * - the device (a ChipIdea, the CX) takes an endpoint number its OS left
 *   off (ENDPTCTRLn with neither direction enabled; the OS's NavNet uses
 *   its own), writes that endpoint's two queue heads in the OS's list
 *   (saved first), enables it, and from then primes its own transfer
 *   descriptors and polls them. Nothing of the OS's endpoints is touched.
 * - the host (an EHCI core: the CX II's FOTG210, or a CX's ChipIdea in
 *   host mode) finds the device's address in its OS's async schedule (read
 *   only), stops the async schedule, points it at two queue heads of ours
 *   (OUT and IN to that endpoint), starts it again, and polls its own
 *   transfer descriptors. The OS's queue heads wait, untouched, unrun.
 * - both mask the USB interrupt at the interrupt controller (VIC bit 8)
 *   while the fast transport runs, so the OS never sees our completions;
 *   its own pending status stays for it, and it runs when unmasked.
 * Every register and word changed is saved and put back when the link
 * ends or falls back (link_fast_stop / fall_back): the OS then finds its
 * NavNet as it left it, and the USB port works for it again.
 *
 * Bring-up (both sides at once, after the hellos, NavNet still running):
 * the device takes its endpoint and says READY (endpoint, packet size) on
 * NavNet; once that's written it masks the interrupt and primes its OUT.
 * The host, given READY, lets its OS ack it, swaps the schedule in and
 * sends SYN; the device answers ACK. USB's own handshake decides for both:
 * the device's ACK completes only once the host's controller has it, and
 * the host looks at its IN once more with its schedule stopped before
 * giving up. On any failure each side puts everything back and says FAIL
 * on NavNet: the link goes on over NavNet as before.
 *
 * Later (the link up): messages are counted. A side that falls back
 * (an error, the cable, the other side's BYE or its NavNet traffic) puts
 * everything back, says RESUME (how many it got) on NavNet, and sends
 * nothing more until the other's RESUME: then what the other missed goes
 * again on NavNet, and NavNet carries the link from there. Nothing lost,
 * nothing twice.
 *
 * Packets: an 8-byte head (magic 0x67, kind, payload length, the count of
 * the first message), then [length:2][message] each; a packet a multiple
 * of the endpoint's packet size gets one byte more, so every transfer ends
 * with a short packet (no zero-length packets). Everything off the wire is
 * read byte by byte (the ARM traps unaligned words). */

#ifdef LINK_USB_TEST
#include "side.h"   /* the PC rig's two sides (tests/link_usb) */
#define LINK_FAST_SIM
#endif

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "nspire_link.h"
#include "nspire_link_fast.h"
#include "nspire_os.h"

uint32_t nspire_ticks(void);         /* 32768 a second */
uint32_t nspire_rtc_seconds(void);   /* runs where the CX CAS's timer doesn't */

/* ---- The hardware: registers, DMA addresses, the data cache ---- */

#ifdef LINK_FAST_SIM
/* The PC rig's two controllers (tests/link_usb/usb_fast_sim.c). */
uint32_t fast_sim_rd(uint32_t a);
void fast_sim_wr(uint32_t a, uint32_t v);
uint32_t fast_sim_phys(const void *p);
void *fast_sim_virt(uint32_t a);
void fast_sim_region(void *p, unsigned len);
void fast_sim_lock(void);
void fast_sim_unlock(void);
int fast_sim_cx2(void);
#define RD(a) fast_sim_rd(a)
#define WR(a, v) fast_sim_wr((a), (v))
#define PHYS(p) fast_sim_phys(p)
#define VIRT(a) fast_sim_virt(a)
#define IS_CX2() fast_sim_cx2()
#define DMA_BEGIN() fast_sim_lock()
#define DMA_END() fast_sim_unlock()
static void dma_clean(const void *p, unsigned n) { (void)p; (void)n; }
static void dma_flush(const void *p, unsigned n) { (void)p; (void)n; }
#else
#include <os.h>
static inline uint32_t RD(uint32_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static inline void WR(uint32_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }
/* SDRAM is mapped one to one (the OS's and Ndless's USB drivers hand the
 * controller plain pointers); checked against SDRAM's range below. */
#define PHYS(p) ((uint32_t)(uintptr_t)(p))
#define VIRT(a) ((void *)(uintptr_t)(a))
#define IS_CX2() (is_cx2)
#define DMA_BEGIN() do { } while (0)
#define DMA_END() do { } while (0)
/* ARM926: clean the data cache's lines over [p, p+n) (what the CPU wrote
 * reaches memory), or clean and invalidate them (the next read sees what
 * the controller wrote); then drain the write buffer. Only ever on our
 * own DMA words and the two queue heads we own, whole 32-byte lines. */
static void dma_clean(const void *p, unsigned n)
{
  uintptr_t a = (uintptr_t)p & ~(uintptr_t)31, e = (uintptr_t)p + n;
  for (; a < e; a += 32)
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(a) : "memory");
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(0) : "memory");
}
static void dma_flush(const void *p, unsigned n)
{
  uintptr_t a = (uintptr_t)p & ~(uintptr_t)31, e = (uintptr_t)p + n;
  for (; a < e; a += 32)
    __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" : : "r"(a) : "memory");
  __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(0) : "memory");
}
#endif

#define SDRAM_LO 0x10000000u
#define SDRAM_HI 0x14000000u
static int in_sdram(uint32_t a, unsigned n) { return a >= SDRAM_LO && a + n <= SDRAM_HI && a + n >= a; }

#define USB      0xB0000000u
#define VIC_EN   0xDC000010u   /* PL190: read the enabled IRQs; write 1s to enable */
#define VIC_OFF  0xDC000014u   /* write 1s to disable */
#define USB_IRQ  (1u << 8)

/* A CX II's FOTG210 (EHCI-like host registers at 0x10) */
#define FOTG_CAP     (USB + 0x00)
#define FOTG_OP      (USB + 0x10)
#define FOTG_PORTSC  (USB + 0x30)
#define FOTG_OTGCSR  (USB + 0x80)
#define OTGCSR_ROLE_D (1u << 20)
/* A CX's ChipIdea */
#define CI_ID        (USB + 0x000)
#define CI_CAP       (USB + 0x100)
#define CI_DCCPARAMS (USB + 0x124)
#define CI_OP        (USB + 0x140)
#define CI_USBSTS    (USB + 0x144)
#define CI_DEVADDR   (USB + 0x154)
#define CI_EPLIST    (USB + 0x158)
#define CI_PORTSC    (USB + 0x184)
#define CI_OTGSC     (USB + 0x1A4)
#define CI_USBMODE   (USB + 0x1A8)
#define CI_PRIME     (USB + 0x1B0)
#define CI_FLUSH     (USB + 0x1B4)
#define CI_EPSTAT    (USB + 0x1B8)
#define CI_COMPLETE  (USB + 0x1BC)
#define CI_EPCTRL(n) (USB + 0x1C0 + 4 * (n))
#define CX_USB_GATE  0x900B0018u
/* EHCI operational registers, from the op base */
#define OP_USBCMD  0x00
#define OP_USBSTS  0x04
#define OP_ASYNC   0x18
#define CMD_RUN    (1u << 0)
#define CMD_ASE    (1u << 5)
#define STS_ASS    (1u << 15)
#define STS_URI    (1u << 6)
#define STS_SLI    (1u << 8)
#define OTGSC_BSV  (1u << 11)

/* Transfer descriptor tokens (EHCI qTD; the ChipIdea dTD the same bits) */
#define TOK_ACTIVE  (1u << 7)
#define TOK_HALTED  (1u << 6)
#define TOK_BUFERR  (1u << 5)
#define TOK_BABBLE  (1u << 4)
#define TOK_XACT    (1u << 3)
#define TOK_ERRORS  (TOK_HALTED | TOK_BUFERR | TOK_BABBLE | TOK_XACT)
#define TOK_LEN(t)  (((t) >> 16) & 0x7FFF)
#define PID_OUT     (0u << 8)
#define PID_IN      (1u << 8)
#define CERR3       (3u << 10)
#define LINK_T      1u          /* a link pointer's terminate bit */
#define LINK_QH     2u          /* type QH */

#define EPCTRL_RXE  (1u << 7)
#define EPCTRL_RXR  (1u << 6)
#define EPCTRL_TXE  (1u << 23)
#define EPCTRL_TXR  (1u << 22)
#define EPCTRL_BULK ((2u << 2) | (2u << 18))

#define PKT       4096            /* a transfer at most */
#define PKT_HEAD  8
#define PKT_ROOM  (PKT - PKT_HEAD - 1)
#define K_DATA    1
#define K_SYN     2
#define K_ACK     3
#define K_BYE     4
#define MAGIC     0x67

#define C_READY   1
#define C_FAIL    2
#define C_RESUME  3

/* Bring-up's waits, ms (NavNet's round trips are 0.1-3 s) */
#define READY_WAIT_MS   5000
#define READY_OUT_MS    600
#define SYN_WAIT_MS     3000
#define ACK_WAIT_MS     1000
#define SCHED_POLLS     200000
#define SCHED_AGAIN_POLLS 1000   /* the release's second look after a start that lagged */
#define ACK_TICKS_MAX   50       /* OS ticks the host waits for its OS's NavNet ack of READY */

/* The DMA words: queue heads and descriptors 32-byte aligned, each in
 * lines of its own (the cache works in 32-byte lines). */
struct qh  { uint32_t hlink, ep_char, ep_cap, cur, next, alt, token, buf[5], pad[4]; };
struct qtd { uint32_t next, alt, token, buf[5]; };
struct dtd { uint32_t next, token, buf[5], pad; };
struct dma
{
  struct qh qh[2];     /* host: 0 OUT (the reclamation head), 1 IN */
  struct qtd qtd[2];   /* host: 0 OUT, 1 IN */
  struct dtd dtd[2];   /* device: 0 OUT (rx), 1 IN (tx) */
  uint8_t tx[PKT];
  uint8_t rx[PKT];
};
typedef char dma_lines[sizeof(struct qh) == 64 && sizeof(struct qtd) == 32 && sizeof(struct dtd) == 32 ? 1 : -1];
/* 64-byte aligned (2026-11-11, F6): an EHCI queue head is 48 bytes and
 * must not cross a 4 KB page (Linux's EHCI and FOTG210 pools forbid it).
 * At 32 a queue head at a page's last 32 bytes would; at 64 each 64-byte
 * qh[] slot lies inside one page. The PC rig places d at the last slot of
 * a page this alignment allows and checks every queue head it walks. */
#define DMA_ALIGN 64
typedef char dma_qh_pages[DMA_ALIGN % sizeof(struct qh) == 0 && 4096 % DMA_ALIGN == 0 ? 1 : -1];
#ifdef LINK_FAST_SIM
static uint8_t dma_raw[sizeof(struct dma) + 8192];
#else
static uint8_t dma_raw[sizeof(struct dma) + DMA_ALIGN];
#endif
static struct dma *d;

/* ---- State ---- */

enum { F_OFF, F_UP, F_RESUMING, F_NAVNET, F_BROKEN };
static int state, role;   /* role: 1 host, 2 device */
static const struct link_fast_nav *nav;

/* Bring-up's news from NavNet (link_fast_control) */
static int got_ready, got_fail, in_start, tried, fell_news;
static unsigned ready_ep, ready_mp;
static char fail_text[48];

/* The saved and the owned: the host's */
static uint32_t op, h_portsc, saved_cmd, saved_async, vic_was;
/* os_head: the OS's ring's head (its H bit), what ASYNCLISTADDR gets back.
 * EHCI 1.0 4.8: the controller leaves the last queue head's horizontal
 * pointer in ASYNCLISTADDR, so while it runs the register walks the ring:
 * the value at take-over can be any of the OS's queue heads, and while
 * ours it is qh[0] or qh[1] (F1). cmd_touched: this side wrote USBCMD.ASE
 * (set before the first write, F2): the OS's ASE goes back whatever
 * happened. parked: the schedule wouldn't stop at a release; our queue
 * heads lead into the OS's ring, inactive, and are still ours (held). */
static uint32_t os_head;
static int sched_ours, vic_masked, cmd_touched, parked;
/* ASYNCLISTADDR sampled while up (the log's, once), and the OS turning the
 * USB interrupt on again mid-link (F5). */
static uint32_t async_seen[8];
static unsigned async_samples, async_kinds, irq_again;
/* Why the controller wasn't given back as found (empty: it was); the off
 * switch reads it (link_fast_damage, nspire_link_speed.c). */
static char damage[96];
static unsigned h_addr, h_eps, h_ep;
static uint32_t h_char, h_cap;   /* the OS's queue head's words 1 and 2 (for the log; its hub/port bits are copied) */
/* the device's */
static unsigned ep, mp, os_rx_bits;
static uint32_t saved_epctrl, saved_dqh[32], sts_at_start;
static uint32_t *dqh_out, *dqh_in;
static int ep_taken, watch_os_rx;
/* The log's: the first packet each way after a take-over, the fall back's
 * time (no RESUME from the other side: said once). */
static int first_out, first_in, resume_late_said;
static uint32_t fell_at;

/* In flight */
static int out_busy, in_armed;
static unsigned out_end_off, out_end_count;

/* Sent messages (for a RESUME): [len:2][msg], from h_done (not known to
 * have reached the other side) to h_end; h_wire: the first not sent. */
#define HIST 65536
static uint8_t hist[HIST];
static unsigned h_done, h_wire, h_end, n_done, n_wire, n_end;
/* Messages come, for the link (and NavNet's that came during bring-up) */
#define RXQ 32768
static uint8_t rxq[RXQ];
static unsigned rq_head, rq_tail;
static unsigned rx_count;

/* Figures for the log */
static unsigned pk_out, pk_in, by_out, by_in, polls, fallbacks;
static uint32_t up_at, out_at, out_longest, rt_in_at;
static char why_down[96];
static int ever;

static unsigned hist_used(void) { return (h_end - h_done) % HIST; }
static unsigned rxq_used(void) { return (rq_tail - rq_head) % RXQ; }

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }
static unsigned get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }

/* What a transfer took, from the token the controller wrote back: a
 * remainder past what was asked is a token that isn't sane (0: nothing). */
static unsigned got_of(uint32_t t)
{
  unsigned left = TOK_LEN(t);
  return left > PKT ? 0 : PKT - left;
}

static void hist_copy(unsigned at, uint8_t *dst, unsigned n)
{
  while (n--)
  {
    *dst++ = hist[at];
    at = (at + 1) % HIST;
  }
}
static unsigned hist_len(unsigned at) { return hist[at] | (hist[(at + 1) % HIST] << 8); }

static void rxq_put(const uint8_t *msg, unsigned len)
{
  unsigned i;
  if (RXQ - 1 - rxq_used() < len + 2)
  {
    nspire_link_log("fast transport: %u bytes came with no room: dropped", len);
    return;
  }
  rxq[rq_tail] = (uint8_t)len;
  rxq[(rq_tail + 1) % RXQ] = (uint8_t)(len >> 8);
  rq_tail = (rq_tail + 2) % RXQ;
  for (i = 0; i < len; i++)
  {
    rxq[rq_tail] = msg[i];
    rq_tail = (rq_tail + 1) % RXQ;
  }
}

static int rxq_take(void *buf, unsigned size)
{
  unsigned n, i;
  uint8_t *b = buf;
  if (rxq_used() < 2)
    return 0;
  n = rxq[rq_head] | (rxq[(rq_head + 1) % RXQ] << 8);
  rq_head = (rq_head + 2) % RXQ;
  for (i = 0; i < n; i++)
  {
    if (i < size)
      b[i] = rxq[rq_head];
    rq_head = (rq_head + 1) % RXQ;
  }
  return n <= size ? (int)n : 0;
}

/* ms since c0/r0: the timer's, or, where it doesn't move (the CX CAS's),
 * the real-time clock's whole seconds less one. */
struct clock { uint32_t t0, r0; };
static void clock_start(struct clock *c) { c->t0 = nspire_ticks(); c->r0 = nspire_rtc_seconds(); }
static unsigned clock_ms(const struct clock *c)
{
  unsigned ms = (unsigned)((uint64_t)(nspire_ticks() - c->t0) * 1000 / 32768);
  unsigned rs = nspire_rtc_seconds() - c->r0;
  if (rs > 1 && ms < (rs - 1) * 1000)
    ms = (rs - 1) * 1000;
  return ms;
}
static unsigned ticks_us(uint32_t t) { return (unsigned)((uint64_t)t * 1000000 / 32768); }

/* A buffer's five page pointers (EHCI qTD and ChipIdea dTD alike) */
static void set_pages(uint32_t *b, const void *p)
{
  uint32_t a = PHYS(p);
  int i;
  b[0] = a;
  for (i = 1; i < 5; i++)
    b[i] = (a & ~0xFFFu) + 0x1000u * (uint32_t)i;
}

/* ---- What this calculator can be ---- */

static int ci_present(void)
{
  if (RD(CX_USB_GATE) & 0x20)
    return 0;
  return (RD(CI_ID) & 0xFFFF) == 0xFA05 && (RD(CI_CAP) & 0xFFFF00FFu) == 0x01000040u;
}

/* The host's op base, if this one's an EHCI host now with its port up. */
static uint32_t host_op(void)
{
  uint32_t o, port;
  if (IS_CX2())
  {
    if ((RD(FOTG_CAP) & 0xFFFF00FFu) != 0x01000010u || (RD(FOTG_OTGCSR) & OTGCSR_ROLE_D))
      return 0;
    o = FOTG_OP;
    port = FOTG_PORTSC;
  }
  else
  {
    if (!ci_present() || (RD(CI_USBMODE) & 3) != 3)
      return 0;
    o = CI_OP;
    port = CI_PORTSC;
  }
  if (!(RD(o + OP_USBCMD) & CMD_RUN) || (RD(port) & 5) != 5)
    return 0;
  h_portsc = port;
  return o;
}

/* A ChipIdea device endpoint its OS left off: the lowest one above the
 * OS's highest enabled endpoint, or 0 (F7, 2026-11-11: its two queue heads
 * are then the nearest past the OS's own in its list; the controller
 * doesn't need the list allocated past what the OS uses, so the highest,
 * DEN-1, is the likeliest to lie past it). */
static unsigned free_endpoint(void)
{
  unsigned n, den, top = 0;
  if (IS_CX2() || !ci_present() || (RD(CI_USBMODE) & 3) != 2 || !(RD(CI_DEVADDR) >> 25))
    return 0;
  den = RD(CI_DCCPARAMS) & 0x1F;
  if (den > 16)
    den = 16;
  for (n = 1; n < den; n++)
    if (RD(CI_EPCTRL(n)) & (EPCTRL_RXE | EPCTRL_TXE))
      top = n;
  return top + 1 < den ? top + 1 : 0;
}

uint32_t link_fast_offer(void)
{
  uint32_t can = 0;
  if (host_op())
    can = LINK_FAST_CAN_HOST;
  else if (free_endpoint())
    can = LINK_FAST_CAN_DEVICE;
  return can ? can | (LINK_FAST_VERSION << 8) : 0;
}

/* ---- The interrupt ---- */

static void irq_mask(void)
{
  if (vic_masked)
    return;
  vic_was = RD(VIC_EN) & USB_IRQ;
  WR(VIC_OFF, USB_IRQ);
  vic_masked = 1;
}

/* What irq_unmask did, for the release's log line. */
static const char *irq_said(int was_masked)
{
  return !was_masked ? "never masked" : vic_was ? "on again" : "left off, as it was";
}

static void irq_unmask(void)
{
  if (!vic_masked)
    return;
  if (vic_was)
    WR(VIC_EN, USB_IRQ);
  vic_masked = 0;
}

/* The first reason the controller wasn't given back as found (kept). */
static void set_damage(const char *why)
{
  if (!damage[0])
    snprintf(damage, sizeof(damage), "%s", why);
}

/* ---- The host's side ---- */

/* One of our two queue heads (EHCI 4.8: ASYNCLISTADDR is either while
 * ours). */
static int ours(uint32_t a)
{
  a &= ~31u;
  return d && (a == PHYS(&d->qh[0]) || a == PHYS(&d->qh[1]));
}

/* The OS's ring's head: the queue head with the H bit, walked from a (any
 * of the ring's), read only; a itself if none is found. */
static uint32_t ring_head(uint32_t a0)
{
  uint32_t a = a0 & ~31u, first = a;
  unsigned hops;
  for (hops = 0; hops < 16 && a && in_sdram(a, 48); hops++)
  {
    const volatile uint32_t *q = VIRT(a);
    if (q[1] & (1u << 15))
      return a;
    if ((q[0] & LINK_T) || (q[0] & 6) != LINK_QH)
      break;
    a = q[0] & ~31u;
    if (a == first)
      break;
  }
  return a0 & ~31u;
}

/* An OUT transfer of the OS's to the device still waiting in its ring
 * (read only: the overlay's token, or the next qTD's): NavNet's ack of
 * READY not gone yet (F4). If the OS's queue heads are cached, a stale
 * word costs a wait (bounded) or what the 3 ticks did before, no more. */
static int os_out_busy(void)
{
  uint32_t a = os_head, first = a;
  unsigned hops;
  for (hops = 0; hops < 16 && a && in_sdram(a, 48); hops++)
  {
    const volatile uint32_t *q = VIRT(a);
    if ((q[1] & 0x7F) == h_addr)
    {
      uint32_t t = q[6], nx = q[4];
      if ((t & TOK_ACTIVE) && ((t >> 8) & 3) == 0)
        return 1;
      if (!(nx & LINK_T) && in_sdram(nx & ~31u, 32))
      {
        t = ((const volatile uint32_t *)VIRT(nx & ~31u))[2];
        if ((t & TOK_ACTIVE) && ((t >> 8) & 3) == 0)
          return 1;
      }
    }
    if ((q[0] & LINK_T) || (q[0] & 6) != LINK_QH)
      break;
    a = q[0] & ~31u;
    if (a == first)
      break;
  }
  return 0;
}

static int sched_wait_n(uint32_t want, unsigned polls)
{
  unsigned i;
  for (i = 0; i < polls; i++)
    if ((RD(op + OP_USBSTS) & STS_ASS) == want)
      return 1;
  return 0;
}

static int sched_wait(uint32_t want)
{
  return sched_wait_n(want, SCHED_POLLS);
}

/* The device's address and speed from the OS's async ring (read only). */
static const char *host_find_device(void)
{
  static char why[64];
  uint32_t a = RD(op + OP_ASYNC) & ~31u, first = a, link;
  unsigned hops, found = 0;

  for (hops = 0; hops < 16 && a; hops++)
  {
    const uint32_t *q;
    unsigned addr;
    if (!in_sdram(a, 48))
    {
      snprintf(why, sizeof(why), "the OS's queue head %08x is outside SDRAM", (unsigned)a);
      return why;
    }
    q = VIRT(a);
    addr = q[1] & 0x7F;
    if (addr)
    {
      if (found && addr != h_addr)
        return "more than one device in the OS's schedule";
      h_addr = addr;
      h_eps = (q[1] >> 12) & 3;
      h_char = q[1];
      h_cap = q[2];
      found = 1;
    }
    link = q[0];
    if ((link & LINK_T) || (link & 6) != LINK_QH)
      break;
    a = link & ~31u;
    if (a == first)
      break;
  }
  if (!found)
    return "no device in the OS's async schedule";
  if (h_eps == 1)
    return "a low-speed device";
  return NULL;
}

static void host_build(unsigned ep_no, unsigned maxp)
{
  int i;
  uint32_t ch = h_addr | (ep_no << 8) | (h_eps << 12) | ((uint32_t)maxp << 16);

  DMA_BEGIN();
  memset(d->qh, 0, sizeof(d->qh));
  memset(d->qtd, 0, sizeof(d->qtd));
  for (i = 0; i < 2; i++)
  {
    d->qh[i].hlink = PHYS(&d->qh[!i]) | LINK_QH;
    d->qh[i].ep_char = ch | (i == 0 ? (1u << 15) : 0);   /* H: the ring's head */
    /* Mult 1; a full-speed device's hub address and port as the OS's
     * own queue head has them (0 on a root port; a transaction translator's
     * otherwise): the controller routes our transfers as it does the OS's. */
    d->qh[i].ep_cap = (1u << 30) | (h_eps != 2 ? h_cap & 0x3FFF0000u : 0);
    d->qh[i].next = LINK_T;
    d->qh[i].alt = LINK_T;
    d->qtd[i].next = d->qtd[i].alt = LINK_T;
  }
  dma_clean(d->qh, sizeof(d->qh));
  dma_clean(d->qtd, sizeof(d->qtd));
  DMA_END();
}

/* Stops the async schedule; it's ours (or the OS's) once ASS says so. */
static int sched_stop(void)
{
  WR(op + OP_USBCMD, RD(op + OP_USBCMD) & ~CMD_ASE);
  return sched_wait(0);
}

static int sched_start(void)
{
  WR(op + OP_USBCMD, RD(op + OP_USBCMD) | CMD_ASE);
  return sched_wait(STS_ASS);
}

static const char *host_take(void)
{
  uint32_t vic = RD(VIC_EN);
  saved_cmd = RD(op + OP_USBCMD);
  saved_async = RD(op + OP_ASYNC);
  os_head = ring_head(saved_async);
  /* Masking proves the OS sees none of ours only if its line was on. */
  if (!(vic & USB_IRQ))
  {
    nspire_link_log("fast transport: host: the USB interrupt line is off (VIC %08x): not taken", (unsigned)vic);
    return "the OS had the USB interrupt off";
  }
  irq_mask();
  nspire_link_log("fast transport: host: taking over: saved USBCMD %08x, ASYNCLISTADDR %08x (the OS's ring's head %08x), "
                  "USB interrupt line %s (VIC %08x before, masked)", (unsigned)saved_cmd, (unsigned)saved_async,
                  (unsigned)os_head, vic_was ? "on" : "off", (unsigned)vic);
  cmd_touched = 1;   /* before the first ASE write: given back whatever comes */
  if (!sched_stop())
    return "the async schedule didn't stop";
  WR(op + OP_ASYNC, PHYS(&d->qh[0]));
  sched_ours = 1;
  if (!sched_start())
    return "our async schedule didn't start";
  return NULL;
}

/* The schedule wouldn't stop at a release: ASYNCLISTADDR can't be written
 * (EHCI forbids it while the schedule runs). Our queue heads are made
 * harmless instead: both lead to the OS's ring head, no H bit, nothing
 * active; the controller walks on into the OS's ring (and, EHCI 4.8, the
 * register with it). They stay ours (link_fast_holds) until a later give
 * back stops the schedule. */
static void host_park(void)
{
  int i;
  DMA_BEGIN();
  for (i = 0; i < 2; i++)
  {
    d->qh[i].hlink = os_head | LINK_QH;
    d->qh[i].ep_char &= ~(1u << 15);
    d->qh[i].next = d->qh[i].alt = LINK_T;
    d->qh[i].token &= ~TOK_ACTIVE;
    d->qtd[i].token &= ~TOK_ACTIVE;
  }
  dma_clean(d->qh, sizeof(d->qh));
  dma_clean(d->qtd, sizeof(d->qtd));
  DMA_END();
  parked = 1;
}

/* Everything back as the OS left it (also after a failed take), and the
 * words in the log: what they were while ours, what they are now. 1 if
 * nothing of the controller's is ours any more. */
static int host_give_back(void)
{
  int held = sched_ours || cmd_touched || vic_masked, masked = vic_masked, stopped = 1;
  uint32_t cmd_ours = 0, async_ours = 0;
  const char *bad = NULL;   /* this give back's: not as found */

  if (!held)
    return 1;
  cmd_ours = RD(op + OP_USBCMD);
  async_ours = RD(op + OP_ASYNC);
  if (sched_ours)
  {
    stopped = sched_stop();
    if (!stopped)
    {
      /* As Linux's EHCI does after its ASS wait times out: ASE is set as
       * it was anyway, so the OS's ring runs (through ours, parked). */
      host_park();
      bad = "the async schedule would not stop";
      nspire_link_log("fast transport: host: schedule would not stop (USBSTS %08x, USBCMD %08x, ASYNCLISTADDR %08x): "
                      "our queue heads lead into the OS's ring (%08x), inactive; still held, tried again later",
                      (unsigned)RD(op + OP_USBSTS), (unsigned)RD(op + OP_USBCMD), (unsigned)RD(op + OP_ASYNC),
                      (unsigned)os_head);
    }
    else
    {
      /* Stopped: the register holds the last queue head walked (EHCI
       * 4.8), ours either one; the OS's ring's head goes back. */
      if (ours(RD(op + OP_ASYNC)))
        WR(op + OP_ASYNC, os_head);
      sched_ours = 0;
      parked = 0;
    }
  }
  if (cmd_touched)
  {
    if (saved_cmd & CMD_ASE)
    {
      if (!(RD(op + OP_USBCMD) & CMD_ASE) && !sched_start())
      {
        uint32_t sts = RD(op + OP_USBSTS), cmd = RD(op + OP_USBCMD), port = RD(h_portsc), again;
        /* The second look short (F5): after SCHED_POLLS reads more
         * of them only lengthen the stand-still after a pull. */
        sched_wait_n(STS_ASS, SCHED_AGAIN_POLLS);
        again = RD(op + OP_USBSTS);
        /* LINK2 (r6, the cable pulled: USBSTS 0000400e, PORTSC 0000000a):
         * ASE is set as the OS had it, ASYNCLISTADDR its ring's head; only
         * the status bit (ASS) hadn't followed within SCHED_POLLS reads,
         * the port gone. Nothing is ours: the OS's own disconnect handling
         * stopped the controller as it does with nothing in (00080b00 in
         * the heartbeat 20 s later). Said with the port and a second look. */
        nspire_link_log("fast transport: host: the OS's async schedule didn't start again within %u reads (USBSTS "
                        "%08x, USBCMD %08x, PORTSC %08x; a second look %u reads on: USBSTS %08x): ASE as the OS had it, "
                        "its status bit lags", (unsigned)SCHED_POLLS, (unsigned)sts, (unsigned)cmd, (unsigned)port,
                        (unsigned)SCHED_AGAIN_POLLS, (unsigned)again);
      }
    }
    else if (RD(op + OP_USBCMD) & CMD_ASE)
      sched_stop();
    if (stopped)
      cmd_touched = 0;
  }
  irq_unmask();
  out_busy = in_armed = 0;
  /* As found? The off switch keeps the fast transport off if not. */
  if (!bad && ours(RD(op + OP_ASYNC)))
    bad = "ASYNCLISTADDR still one of ours";
  if (!bad && (RD(op + OP_USBCMD) & CMD_ASE) != (saved_cmd & CMD_ASE))
    bad = "USBCMD's async enable not as saved";
  if (!bad && masked && (RD(VIC_EN) & USB_IRQ) != vic_was)
    bad = "the USB interrupt line not as saved";
  if (bad)
    set_damage(bad);
  nspire_link_log("fast transport: host: %s: USBCMD %08x -> %08x (saved %08x), ASYNCLISTADDR %08x -> %08x "
                  "(saved %08x, the ring's head %08x), USBSTS %08x (status bits left for the OS), USB interrupt line %s "
                  "(VIC %08x)%s%s", stopped ? "released" : "NOT released", (unsigned)cmd_ours,
                  (unsigned)RD(op + OP_USBCMD), (unsigned)saved_cmd, (unsigned)async_ours, (unsigned)RD(op + OP_ASYNC),
                  (unsigned)saved_async, (unsigned)os_head, (unsigned)RD(op + OP_USBSTS), irq_said(masked),
                  (unsigned)RD(VIC_EN), bad ? "; NOT as found: " : "", bad ? bad : "");
  return stopped;
}

static void host_send_qtd(unsigned len)
{
  DMA_BEGIN();
  dma_flush(&d->qh[0], sizeof(d->qh[0]));
  d->qtd[0].next = d->qtd[0].alt = LINK_T;
  set_pages(d->qtd[0].buf, d->tx);
  d->qtd[0].token = ((uint32_t)len << 16) | CERR3 | PID_OUT | TOK_ACTIVE;
  dma_clean(d->tx, len);
  dma_clean(&d->qtd[0], sizeof(d->qtd[0]));
  d->qh[0].next = PHYS(&d->qtd[0]);
  dma_clean(&d->qh[0], sizeof(d->qh[0]));
  DMA_END();
  out_busy = 1;
}

static void host_arm_in(void)
{
  DMA_BEGIN();
  dma_flush(d->rx, PKT);
  dma_flush(&d->qh[1], sizeof(d->qh[1]));
  d->qtd[1].next = d->qtd[1].alt = LINK_T;
  set_pages(d->qtd[1].buf, d->rx);
  d->qtd[1].token = ((uint32_t)PKT << 16) | CERR3 | PID_IN | TOK_ACTIVE;
  dma_clean(&d->qtd[1], sizeof(d->qtd[1]));
  d->qh[1].next = PHYS(&d->qtd[1]);
  dma_clean(&d->qh[1], sizeof(d->qh[1]));
  DMA_END();
  in_armed = 1;
}

/* A descriptor's token as the controller left it */
static uint32_t token_of(uint32_t *t, void *line)
{
  uint32_t v;
  DMA_BEGIN();
  dma_flush(line, 32);
  v = *(volatile uint32_t *)t;
  DMA_END();
  return v;
}

/* ---- The device's side ---- */

static const char *device_take(void)
{
  uint32_t list = RD(CI_EPLIST) & ~0x7FFu;
  unsigned n, den = RD(CI_DCCPARAMS) & 0x1F;

  ep = free_endpoint();
  if (!ep)
    return "no endpoint its OS left off";
  if (!in_sdram(list, 64 * 2 * (ep + 1)))
    return "the endpoint list is outside SDRAM";
  /* Masking proves the OS sees none of ours only if its line was on. */
  if (!(RD(VIC_EN) & USB_IRQ))
  {
    nspire_link_log("fast transport: device: the USB interrupt line is off (VIC %08x): not taken", (unsigned)RD(VIC_EN));
    return "the OS had the USB interrupt off";
  }
  mp = ((RD(CI_PORTSC) >> 26) & 3) == 2 ? 512 : 64;
  os_rx_bits = 0;
  for (n = 1; n < den && n < 16; n++)
    if (n != ep && (RD(CI_EPCTRL(n)) & EPCTRL_RXE))
      os_rx_bits |= 1u << n;
  dqh_out = (uint32_t *)VIRT(list + 64 * 2 * ep);
  dqh_in = dqh_out + 16;
  DMA_BEGIN();
  dma_flush(dqh_out, 128);
  memcpy(saved_dqh, dqh_out, 128);
  memset(dqh_out, 0, 128);
  dqh_out[0] = ((uint32_t)mp << 16) | (1u << 29);   /* no zero-length packets */
  dqh_out[2] = LINK_T;
  dqh_in[0] = ((uint32_t)mp << 16) | (1u << 29);
  dqh_in[2] = LINK_T;
  dma_clean(dqh_out, 128);
  DMA_END();
  saved_epctrl = RD(CI_EPCTRL(ep));
  WR(CI_EPCTRL(ep), EPCTRL_BULK | EPCTRL_RXE | EPCTRL_RXR | EPCTRL_TXE | EPCTRL_TXR);
  ep_taken = 1;
  nspire_link_log("fast transport: device: taking over endpoint %u (DEN %u, the lowest above the OS's): saved ENDPTCTRL "
                  "%08x and its queue heads' 32 words; ENDPTCTRL now %08x", ep, den, (unsigned)saved_epctrl,
                  (unsigned)RD(CI_EPCTRL(ep)));
  /* In full: zeros (or stale dQHs) say the list is the OS's and unused
   * there; anything else may be live data of the OS's (F7). */
  for (n = 0; n < 2; n++)
  {
    const uint32_t *w = saved_dqh + 16 * n;
    nspire_link_log("fast transport: device: saved %s dQH %08x: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x "
                    "%08x %08x %08x %08x %08x", n ? "IN" : "OUT", (unsigned)(list + 64 * (2 * ep + n)),
                    (unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3], (unsigned)w[4], (unsigned)w[5],
                    (unsigned)w[6], (unsigned)w[7], (unsigned)w[8], (unsigned)w[9], (unsigned)w[10], (unsigned)w[11],
                    (unsigned)w[12], (unsigned)w[13], (unsigned)w[14], (unsigned)w[15]);
  }
  sts_at_start = RD(CI_USBSTS) & (STS_URI | STS_SLI);
  return NULL;
}

static int prime_wait(uint32_t bit)
{
  unsigned i;
  WR(CI_PRIME, bit);
  for (i = 0; i < SCHED_POLLS; i++)
    if (!(RD(CI_PRIME) & bit))
      return 1;
  return 0;
}

static void device_arm_rx(void)
{
  DMA_BEGIN();
  dma_flush(d->rx, PKT);
  d->dtd[0].next = LINK_T;
  d->dtd[0].token = ((uint32_t)PKT << 16) | TOK_ACTIVE;
  set_pages(d->dtd[0].buf, d->rx);
  dma_clean(&d->dtd[0], sizeof(d->dtd[0]));
  dma_flush(dqh_out, 64);
  dqh_out[2] = PHYS(&d->dtd[0]);
  dqh_out[3] = 0;
  dma_clean(dqh_out, 64);
  DMA_END();
  prime_wait(1u << ep);
  in_armed = 1;
}

static void device_send_dtd(unsigned len)
{
  DMA_BEGIN();
  d->dtd[1].next = LINK_T;
  d->dtd[1].token = ((uint32_t)len << 16) | TOK_ACTIVE;
  set_pages(d->dtd[1].buf, d->tx);
  dma_clean(d->tx, len);
  dma_clean(&d->dtd[1], sizeof(d->dtd[1]));
  dma_flush(dqh_in, 64);
  dqh_in[2] = PHYS(&d->dtd[1]);
  dqh_in[3] = 0;
  dma_clean(dqh_in, 64);
  DMA_END();
  prime_wait(0x10000u << ep);
  out_busy = 1;
}

static void device_give_back(void)
{
  unsigned i;
  int held = ep_taken || vic_masked, was_taken = ep_taken, masked = vic_masked;
  uint32_t ctrl_ours = 0;
  const char *bad = NULL;

  if (ep_taken)
  {
    uint32_t bits = (1u << ep) | (0x10000u << ep);
    ctrl_ours = RD(CI_EPCTRL(ep));
    WR(CI_FLUSH, bits);
    for (i = 0; i < SCHED_POLLS && (RD(CI_FLUSH) & bits); i++)
      ;
    WR(CI_EPCTRL(ep), saved_epctrl);
    DMA_BEGIN();
    memcpy(dqh_out, saved_dqh, 128);
    dma_clean(dqh_out, 128);
    DMA_END();
    WR(CI_COMPLETE, bits);   /* ours only: write-one-to-clear */
    ep_taken = 0;
  }
  irq_unmask();
  out_busy = in_armed = 0;
  /* As found? The off switch keeps the fast transport off if not. */
  if (was_taken && RD(CI_EPCTRL(ep)) != saved_epctrl)
    bad = "the endpoint's ENDPTCTRL not as saved";
  else if (masked && (RD(VIC_EN) & USB_IRQ) != vic_was)
    bad = "the USB interrupt line not as saved";
  if (bad)
  {
    set_damage(bad);
    nspire_link_log("fast transport: device: NOT given back as found: %s", bad);
  }
  if (held && was_taken)
    nspire_link_log("fast transport: device: released endpoint %u: ENDPTCTRL %08x -> %08x (saved %08x), its queue "
                    "heads' 32 words put back (OUT %08x %08x, IN %08x %08x), ENDPTCOMPLETE %08x, ENDPTSTAT %08x, USBSTS "
                    "%08x (reset/suspend bits at take-over %08x), USB interrupt line %s (VIC %08x)", ep, (unsigned)ctrl_ours,
                    (unsigned)RD(CI_EPCTRL(ep)), (unsigned)saved_epctrl, (unsigned)saved_dqh[0], (unsigned)saved_dqh[2],
                    (unsigned)saved_dqh[16], (unsigned)saved_dqh[18], (unsigned)RD(CI_COMPLETE), (unsigned)RD(CI_EPSTAT),
                    (unsigned)RD(CI_USBSTS), (unsigned)sts_at_start, irq_said(masked), (unsigned)RD(VIC_EN));
  else if (held)
    nspire_link_log("fast transport: device: released: USB interrupt line %s (VIC %08x)", irq_said(masked),
                    (unsigned)RD(VIC_EN));
}

/* ---- Packets ---- */

/* Builds the next packet from the history (or a bare one of kind k) in
 * d->tx: its length, padded so it ends with a short packet. */
static unsigned build(int kind)
{
  unsigned len = PKT_HEAD, at = h_wire, count = n_wire, pkt_mp = role == 1 ? ready_mp : mp;

  if (kind == K_DATA)
    while (count != n_end)
    {
      unsigned m = hist_len(at) + 2;
      if (len + m > PKT_HEAD + PKT_ROOM)
        break;
      hist_copy(at, d->tx + len, m);
      len += m;
      at = (at + m) % HIST;
      count++;
    }
  d->tx[0] = MAGIC;
  d->tx[1] = (uint8_t)kind;
  put16(d->tx + 2, len - PKT_HEAD);
  put32(d->tx + 4, n_wire);
  out_end_off = at;
  out_end_count = count;
  if (pkt_mp && len % pkt_mp == 0)
    d->tx[len++] = 0;
  return len;
}

static void fire(int kind)
{
  unsigned len = build(kind);
  if (role == 1)
    host_send_qtd(len);
  else
    device_send_dtd(len);
  if (kind == K_DATA)
  {
    h_wire = out_end_off;
    n_wire = out_end_count;
  }
  out_at = nspire_ticks();
  pk_out++;
  by_out += len;
}

/* What a packet that came carries: its kind (< 0 bad). */
static int parse(unsigned got)
{
  const uint8_t *p = d->rx;
  unsigned len, at;
  uint32_t seq;

  if (got < PKT_HEAD || got > PKT || p[0] != MAGIC)
    return -1;
  len = get16(p + 2);
  if (PKT_HEAD + len > got)
    return -1;
  pk_in++;
  by_in += got;
  if (p[1] != K_DATA)
    return p[1];
  seq = get32(p + 4);
  for (at = PKT_HEAD; at + 2 <= PKT_HEAD + len;)
  {
    unsigned m = get16(p + at);
    if (at + 2 + m > PKT_HEAD + len || m > LINK_MSG_MAX)
      return -1;
    if (seq == rx_count)
    {
      rxq_put(p + at + 2, m);
      rx_count++;
    }
    else if (seq > rx_count)
      return -2;   /* a gap: can't be on USB, falls back */
    seq++;
    at += 2 + m;
  }
  rt_in_at = nspire_ticks();
  return K_DATA;
}

/* ---- Falling back ---- */

static void send_ctrl(int op_code, const void *body, unsigned n)
{
  uint8_t m[64];
  if (n > sizeof(m) - 4)
    n = sizeof(m) - 4;
  m[0] = LINK_FAST_CTRL;
  m[1] = MAGIC;
  m[2] = 'F';
  m[3] = (uint8_t)op_code;
  memcpy(m + 4, body, n);
  if (nav && nav->send(m, n + 4) < 0)
    state = F_BROKEN;
}

/* Whatever of the controller's is ours, back (whatever the role now: a
 * parked schedule outlives its link). 1 if nothing is held after. */
static int give_back(void)
{
  if (sched_ours || cmd_touched || (role == 1 && vic_masked))
    host_give_back();
  if (ep_taken || (role == 2 && vic_masked))
    device_give_back();
  irq_unmask();
  return !link_fast_holds();
}

/* The fast transport's done: everything back, RESUME said, NavNet next.
 * bye: tell the other side first (it isn't the one that went). */
static void fall_back(const char *why, int bye)
{
  uint8_t c[4];
  if (state != F_UP)
    return;
  fallbacks++;
  snprintf(why_down, sizeof(why_down), "%s", why);
  if (bye && !out_busy)
  {
    struct clock c0;
    uint32_t *tok = role == 1 ? &d->qtd[0].token : &d->dtd[1].token;
    void *line = role == 1 ? (void *)&d->qtd[0] : (void *)&d->dtd[1];
    fire(K_BYE);
    clock_start(&c0);
    while (clock_ms(&c0) < 50 && (token_of(tok, line) & TOK_ACTIVE))
      ;
  }
  /* What's in the IN buffer already came: taken before it's let go. */
  if (role == 1 && sched_ours)
    sched_stop();
  if (in_armed)
  {
    uint32_t t = role == 1 ? token_of(&d->qtd[1].token, &d->qtd[1]) : token_of(&d->dtd[0].token, &d->dtd[0]);
    if (!(t & (TOK_ACTIVE | TOK_ERRORS)))
      parse(got_of(t));
  }
  give_back();
  nspire_link_log("fast transport: fell back to NavNet: %s (up %u ms, packets %u out, %u in; %u messages sent, "
                  "%u reached the other side for sure, %u came)", why,
                  ticks_us(nspire_ticks() - up_at) / 1000, pk_out, pk_in, n_end, n_done, rx_count);
  /* What wasn't known to arrive goes again if the other asks for it. */
  h_wire = h_done;
  n_wire = n_done;
  state = F_RESUMING;
  fell_at = nspire_ticks();
  resume_late_said = 0;
  put32(c, rx_count);
  send_ctrl(C_RESUME, c, 4);
  if (state == F_BROKEN)
    nspire_link_log("fast transport: NavNet's broken too: the link's over");
}

/* The other's RESUME: what it got; the rest of ours goes on NavNet. */
static void resume(uint32_t got)
{
  uint8_t m[LINK_MSG_MAX + 2];
  unsigned at = h_done, count = n_done, sent = 0;

  if (got < n_done || got > n_end)
  {
    nspire_link_log("fast transport: the other side got %u messages, this side has %u to %u: can't resume",
                    (unsigned)got, n_done, n_end);
    state = F_BROKEN;
    return;
  }
  while (count != n_end)
  {
    unsigned len = hist_len(at);
    if (count >= got)
    {
      hist_copy((at + 2) % HIST, m, len);
      if (nav->send(m, len) < 0)
      {
        state = F_BROKEN;
        return;
      }
      sent++;
    }
    at = (at + 2 + len) % HIST;
    count++;
  }
  h_done = h_wire = h_end;
  n_done = n_wire = n_end;
  state = F_NAVNET;
  fell_news = 1;
  nspire_link_log("fast transport: resumed on NavNet: the other side had %u, %u sent again", (unsigned)got, sent);
}

/* ---- Polling ---- */

static void check_out(void)
{
  uint32_t t;
  void *line = role == 1 ? (void *)&d->qtd[0] : (void *)&d->dtd[1];
  if (!out_busy)
    return;
  t = token_of(role == 1 ? &d->qtd[0].token : &d->dtd[1].token, line);
  if (t & TOK_ACTIVE)
    return;
  out_busy = 0;
  if (role == 2)
    WR(CI_COMPLETE, 0x10000u << ep);
  if (t & TOK_ERRORS)
  {
    char why[64];
    snprintf(why, sizeof(why), "a send ended with an error (token %08x)", (unsigned)t);
    fall_back(why, 0);
    return;
  }
  {
    uint32_t dt = nspire_ticks() - out_at;
    if (dt > out_longest)
      out_longest = dt;
    if (!first_out)
    {
      first_out = 1;
      nspire_link_log("fast transport: the first data packet went: %u us, token %08x", ticks_us(dt), (unsigned)t);
    }
  }
  h_done = out_end_off;
  n_done = out_end_count;
}

static void check_in(void)
{
  uint32_t t;
  int k;
  if (!in_armed)
    return;
  t = role == 1 ? token_of(&d->qtd[1].token, &d->qtd[1]) : token_of(&d->dtd[0].token, &d->dtd[0]);
  if (t & TOK_ACTIVE)
    return;
  in_armed = 0;
  if (role == 2)
    WR(CI_COMPLETE, 1u << ep);
  if (t & TOK_ERRORS)
  {
    char why[64];
    snprintf(why, sizeof(why), "a receive ended with an error (token %08x)", (unsigned)t);
    fall_back(why, 0);
    return;
  }
  k = parse(got_of(t));
  if (k == K_DATA && !first_in)
  {
    first_in = 1;
    nspire_link_log("fast transport: the first data packet came: %u bytes, token %08x", got_of(t), (unsigned)t);
  }
  if (k == K_BYE)
  {
    fall_back("the other side went back to NavNet (its BYE)", 0);
    return;
  }
  if (k < 0)
  {
    fall_back(k == -2 ? "a message missing" : "a packet that isn't ours", 1);
    return;
  }
  if (role == 1)
    host_arm_in();
  else
    device_arm_rx();
}

/* The cable, the port, the OS: still as the fast transport needs them. */
static const char *health(void)
{
  static char why[96];
  /* The OS (or a driver of its) turned the USB line on again: its handler
   * would see our completions. Masked again at once and counted; the
   * device falls back, as its OS's handler may already have run on an
   * endpoint it never set up (F5). */
  if (vic_masked && (RD(VIC_EN) & USB_IRQ))
  {
    WR(VIC_OFF, USB_IRQ);
    if (!irq_again++)
      nspire_link_log("fast transport: %s: the OS turned the USB interrupt on again (VIC %08x): masked again",
                      role == 1 ? "host" : "device", (unsigned)(RD(VIC_EN) | USB_IRQ));
    if (role == 2)
      return "the OS turned the USB interrupt on again";
  }
  if (role == 1)
  {
    uint32_t port = RD(h_portsc), a;
    if ((port & 5) != 5)
    {
      snprintf(why, sizeof(why), "the port's %s (PORTSC %08x)", port & 1 ? "disabled" : "disconnected",
               (unsigned)port);
      return why;
    }
    /* EHCI 4.8: while ours the register is qh[0] or qh[1], whichever the
     * controller walked last (F1). */
    a = RD(op + OP_ASYNC);
    if (!ours(a) || !(RD(op + OP_USBCMD) & CMD_ASE))
    {
      snprintf(why, sizeof(why), "the OS took its schedule back (ASYNCLISTADDR %08x, USBCMD %08x)", (unsigned)a,
               (unsigned)RD(op + OP_USBCMD));
      return why;
    }
    if (async_samples < 8)
    {
      unsigned i;
      for (i = 0; i < async_kinds && async_seen[i] != (a & ~31u); i++)
        ;
      if (i == async_kinds)
        async_seen[async_kinds++] = a & ~31u;
      if (++async_samples == 8)
      {
        char seen[80];
        unsigned at = 0;
        for (i = 0; i < async_kinds && at + 10 < sizeof(seen); i++)
          at += (unsigned)snprintf(seen + at, sizeof(seen) - at, " %08x", (unsigned)async_seen[i]);
        nspire_link_log("fast transport: host: ASYNCLISTADDR while ours, 8 polls, %u value%s:%s (qh[0] %08x, qh[1] "
                        "%08x)", async_kinds, async_kinds > 1 ? "s" : "", seen, (unsigned)PHYS(&d->qh[0]),
                        (unsigned)PHYS(&d->qh[1]));
      }
    }
    return NULL;
  }
  if (!(RD(CI_OTGSC) & OTGSC_BSV))
    return "VBUS went (the cable's out)";
  if ((RD(CI_USBSTS) & (STS_URI | STS_SLI)) & ~sts_at_start)
    return "a bus reset or suspend";
  if (watch_os_rx && (RD(CI_COMPLETE) & os_rx_bits))
    return "NavNet traffic from the host (it went back)";
  return NULL;
}

static void poll_up(void)
{
  const char *bad;
  if (state != F_UP)
    return;
  polls++;
  if ((bad = health()) != NULL)
  {
    fall_back(bad, 0);
    return;
  }
  check_in();
  if (state != F_UP)
    return;
  check_out();
  if (state == F_UP && !out_busy && n_wire != n_end)
    fire(K_DATA);
}

/* ---- The interface ---- */

int link_fast_fell_back(void)
{
  int r = fell_news;
  fell_news = 0;
  return r;
}

int link_fast_up(void)
{
  return state == F_UP;
}

int link_fast_holds(void)
{
  return sched_ours || cmd_touched || ep_taken || vic_masked;
}

const char *link_fast_damage(void)
{
  return damage[0] ? damage : NULL;
}

void link_fast_drop(const char *why)
{
  char w[96];
  if (state != F_UP)
    return;
  snprintf(w, sizeof(w), "let go: %s", why);
  fall_back(w, 1);
}

int link_fast_owns(void)
{
  return state == F_UP || state == F_RESUMING || state == F_BROKEN || rxq_used();
}

int link_fast_send(const void *msg, unsigned len)
{
  unsigned i;
  const uint8_t *m = msg;

  if (state == F_BROKEN)
    return -1;
  if (state == F_NAVNET || state == F_OFF)
    return nav ? nav->send(msg, len) : -1;
  if (len > LINK_MSG_MAX)
    return -1;
  if (HIST - 1 - hist_used() < len + 2)
  {
    poll_up();
    if (HIST - 1 - hist_used() < len + 2)
      return -1;
  }
  hist[h_end] = (uint8_t)len;
  hist[(h_end + 1) % HIST] = (uint8_t)(len >> 8);
  h_end = (h_end + 2) % HIST;
  for (i = 0; i < len; i++)
  {
    hist[h_end] = m[i];
    h_end = (h_end + 1) % HIST;
  }
  n_end++;
  poll_up();
  return 0;
}

int link_fast_receive(void *buf, unsigned size, unsigned wait_ms)
{
  struct clock c;
  unsigned looks;
  int n;

  if ((n = rxq_take(buf, size)) > 0)
    return n;
  if (state == F_BROKEN)
    return -1;
  if (state == F_RESUMING)
  {
    if (!resume_late_said && ticks_us(nspire_ticks() - fell_at) >= 5000000u)
    {
      resume_late_said = 1;
      nspire_link_log("fast transport: no RESUME from the other side 5 s after the fall back (the link's own give-up "
                      "ends it if none comes)");
    }
    return nav->receive(buf, size, wait_ms);
  }
  if (state != F_UP)
    return LINK_FAST_NAVNET;
  clock_start(&c);
  for (looks = 0;; looks++)
  {
    poll_up();
    if ((n = rxq_take(buf, size)) > 0)
      return n;
    if (state != F_UP)
      return state == F_BROKEN ? -1 : nav->receive(buf, size, 0);
    if (!wait_ms || clock_ms(&c) >= wait_ms)
      return 0;
    /* A timer that doesn't move (the CX CAS's): a look counts as an
     * eighth of a ms, as nspire_link_usb.c's; the link's give-up counts
     * real seconds. */
    if (nspire_ticks() == c.t0 && looks >= wait_ms * 8)
      return 0;
  }
}

void link_fast_poll(void)
{
  poll_up();
}

int link_fast_is_control(const uint8_t *msg, unsigned len)
{
  return tried && len >= 4 && msg[0] == LINK_FAST_CTRL && msg[1] == MAGIC && msg[2] == 'F';
}

void link_fast_control(const uint8_t *msg, unsigned len)
{
  if (len < 4)
    return;
  msg += 2;   /* from here as op, body: msg[1] the op */
  len -= 2;
  switch (msg[1])
  {
  case C_READY:
    if (len >= 6 && in_start)
    {
      ready_ep = msg[2];
      ready_mp = get16(msg + 3);
      got_ready = 1;
    }
    break;
  case C_FAIL:
    if (in_start)
    {
      unsigned n = len - 2 < sizeof(fail_text) - 1 ? len - 2 : sizeof(fail_text) - 1;
      memcpy(fail_text, msg + 2, n);
      fail_text[n] = 0;
      got_fail = 1;
    }
    break;
  case C_RESUME:
    if (len < 6)
      break;
    if (state == F_UP)
      fall_back("the other side went back to NavNet (its RESUME)", 0);
    if (state == F_RESUMING)
      resume(get32(msg + 2));
    break;
  }
}

static void say_fail(const char *why)
{
  send_ctrl(C_FAIL, why, (unsigned)strlen(why));
}

static void not_used(const char *step, const char *why)
{
  nspire_link_log("fast transport: not used, NavNet carries the link: %s: %s", step, why);
  snprintf(why_down, sizeof(why_down), "%s: %s", step, why);
}

/* NavNet read while bringing up: control messages land in the flags,
 * anything else is kept for the link (rxq). */
static void nav_pump(unsigned ms)
{
  uint8_t m[LINK_MSG_MAX];
  int n = nav->receive(m, sizeof(m), ms);
  if (n > 0)
    rxq_put(m, (unsigned)n);
  else if (n < 0)
    state = F_BROKEN;
}

static void reset_counts(void)
{
  h_done = h_wire = h_end = n_done = n_wire = n_end = 0;
  rx_count = 0;
  pk_out = pk_in = by_out = by_in = polls = 0;
  out_longest = 0;
  out_busy = in_armed = 0;
  first_out = first_in = 0;
  why_down[0] = 0;
  async_samples = async_kinds = irq_again = 0;
}

/* The device: endpoint, READY, mask, SYN in, ACK out. */
static int start_device(void)
{
  struct clock c;
  const char *why;
  uint8_t r[4];
  uint32_t t;

  if ((why = device_take()) != NULL)
  {
    say_fail(why);
    not_used("device", why);
    return 0;
  }
  r[0] = (uint8_t)ep;
  put16(r + 1, mp);
  r[3] = LINK_FAST_VERSION;
  /* Timed itself (F4): with one channel the write returns only once the
   * host's OS acked it, which it can't while the host's schedule is ours. */
  clock_start(&c);
  send_ctrl(C_READY, r, 4);
  nspire_link_log("fast transport: device: READY's NavNet write took %u ms", clock_ms(&c));
  nspire_link_log("fast transport: device: endpoint %u (%u-byte packets, %s speed), READY sent; list %08x, "
                  "its control was %08x, the OS's OUT endpoints %04x, USBSTS %08x, OTGSC %08x, VIC %08x", ep, mp,
                  mp == 512 ? "high" : "full", (unsigned)RD(CI_EPLIST), (unsigned)saved_epctrl, os_rx_bits,
                  (unsigned)RD(CI_USBSTS), (unsigned)RD(CI_OTGSC), (unsigned)RD(VIC_EN));
  /* Its write out (the host's OS acks it), and the host's FAIL heard. */
  clock_start(&c);
  while (state != F_BROKEN && !got_fail && !nav->writes_out() && clock_ms(&c) < READY_OUT_MS)
    nav_pump(10);
  watch_os_rx = nav->writes_out();
  if (state == F_BROKEN || got_fail)
  {
    device_give_back();
    not_used("device", got_fail ? fail_text : "NavNet broke");
    return 0;
  }
  nspire_link_log("fast transport: device: READY %s after %u ms; masked, waiting for SYN",
                  watch_os_rx ? "written" : "not known written", clock_ms(&c));
  irq_mask();
  /* The OS's own completions from before the mask aren't the host's news. */
  os_rx_bits &= ~RD(CI_COMPLETE);
  device_arm_rx();
  clock_start(&c);
  for (;;)
  {
    if (clock_ms(&c) >= SYN_WAIT_MS)
    {
      device_give_back();
      not_used("device", "no SYN from the host in 3 s");
      return 0;
    }
    if (watch_os_rx && (RD(CI_COMPLETE) & os_rx_bits))
    {
      device_give_back();
      not_used("device", "the host wrote on NavNet instead of SYN (its FAIL)");
      return 0;
    }
    t = token_of(&d->dtd[0].token, &d->dtd[0]);
    if (!(t & TOK_ACTIVE))
      break;
  }
  WR(CI_COMPLETE, 1u << ep);
  in_armed = 0;
  if ((t & TOK_ERRORS) || parse(got_of(t)) != K_SYN)
  {
    device_give_back();
    not_used("device", "what came instead of SYN was wrong");
    return 0;
  }
  device_arm_rx();
  role = 2;
  fire(K_ACK);
  clock_start(&c);
  while (clock_ms(&c) < ACK_WAIT_MS)
    if (!(token_of(&d->dtd[1].token, &d->dtd[1]) & TOK_ACTIVE))
      break;
  t = token_of(&d->dtd[1].token, &d->dtd[1]);
  if (t & (TOK_ACTIVE | TOK_ERRORS))
  {
    device_give_back();
    role = 0;
    not_used("device", "the host didn't take ACK");
    return 0;
  }
  WR(CI_COMPLETE, 0x10000u << ep);
  out_busy = 0;
  return 1;
}

/* The host: READY in, schedule ours, SYN out, ACK in. */
static int start_host(void)
{
  struct clock c;
  const char *why;
  uint32_t t;
  int k;

  clock_start(&c);
  while (state != F_BROKEN && !got_ready && !got_fail && clock_ms(&c) < READY_WAIT_MS)
    nav_pump(20);
  if (!got_ready)
  {
    not_used("host", state == F_BROKEN ? "NavNet broke" : got_fail ? fail_text : "no READY from the device in 5 s");
    return 0;
  }
  nspire_link_log("fast transport: host: READY came after %u ms: endpoint %u, %u-byte packets", clock_ms(&c),
                  ready_ep, ready_mp);
  if (!ready_ep || ready_ep > 15 || (ready_mp != 64 && ready_mp != 512))
  {
    say_fail("a READY this host can't use");
    not_used("host", "a READY this host can't use");
    return 0;
  }
  op = host_op();
  if (!op)
    why = "this calculator isn't a running USB host now";
  else
    why = host_find_device();
  if (why)
  {
    say_fail(why);
    not_used("host", why);
    return 0;
  }
  if ((h_eps == 2) != (ready_mp == 512))
  {
    say_fail("the speeds disagree");
    not_used("host", "the speeds disagree");
    return 0;
  }
  nspire_link_log("fast transport: host: device %u at %s speed in the OS's schedule (async %08x, its queue head's "
                  "words %08x %08x, USBCMD %08x, PORTSC %08x, VIC %08x)", h_addr, h_eps == 2 ? "high" : "full",
                  (unsigned)RD(op + OP_ASYNC), (unsigned)h_char, (unsigned)h_cap, (unsigned)RD(op + OP_USBCMD),
                  (unsigned)RD(h_portsc), (unsigned)RD(VIC_EN));
  h_ep = ready_ep;
  host_build(h_ep, ready_mp);
  /* Its OS's NavNet acks READY: 3 ticks at least, then until no OUT of
   * the OS's to the device waits in its ring (F4: the device's READY write
   * may return only once that ack is out; with our schedule in, it can't
   * go), ACK_TICKS_MAX at most. */
  os_head = ring_head(RD(op + OP_ASYNC));
  {
    unsigned i;
    int busy;
    for (i = 0; i < 3; i++)
      nspire_os_sleep();
    while ((busy = os_out_busy()) && i < ACK_TICKS_MAX)
    {
      nspire_os_sleep();
      i++;
    }
    nspire_link_log("fast transport: host: %u OS ticks for its NavNet ack: its OUT to the device %s", i,
                    busy ? "still waiting: taking over anyway" : "idle");
  }
  role = 1;
  if ((why = host_take()) != NULL)
  {
    host_give_back();
    role = 0;
    say_fail(why);
    not_used("host", why);
    return 0;
  }
  nspire_link_log("fast transport: host: our schedule runs (USBSTS %08x); SYN going", (unsigned)RD(op + OP_USBSTS));
  host_arm_in();
  fire(K_SYN);
  clock_start(&c);
  for (;;)
  {
    t = token_of(&d->qtd[1].token, &d->qtd[1]);
    if (!(t & TOK_ACTIVE))
      break;
    if (clock_ms(&c) >= SYN_WAIT_MS + ACK_WAIT_MS)
    {
      /* Once more with the schedule stopped: the device's ACK may have
       * come in between, and then the device is up. */
      sched_stop();
      t = token_of(&d->qtd[1].token, &d->qtd[1]);
      if (!(t & TOK_ACTIVE))
      {
        sched_start();
        break;
      }
      why = token_of(&d->qtd[0].token, &d->qtd[0]) & TOK_ACTIVE ? "the device never took SYN"
                                                                  : "no ACK from the device";
      host_give_back();
      role = 0;
      say_fail(why);
      not_used("host", why);
      return 0;
    }
  }
  in_armed = 0;
  k = (t & TOK_ERRORS) ? -1 : parse(got_of(t));
  if (k != K_ACK)
  {
    char w[64];
    snprintf(w, sizeof(w), "no ACK: token %08x, kind %d", (unsigned)t, k);
    host_give_back();
    role = 0;
    say_fail(w);
    not_used("host", w);
    return 0;
  }
  out_busy = 0;
  host_arm_in();
  return 1;
}

#ifndef LINK_FAST_SIM
/* gpSP's exit by any way at all (quit() calls link_fast_stop first): if a
 * word of the controller's is still ours, it goes back here, before the pad
 * driver's shutdown and the keys' give-back (atexit runs the last
 * registered first). */
static void at_exit(void)
{
  if (link_fast_holds())
  {
    nspire_link_log("fast transport: still held at exit: given back now");
    if (!give_back())
      nspire_link_log("fast transport: STILL HELD as gpSP exits (%s): the controller may walk gpSP's freed queue "
                      "heads; reset the calculator", damage[0] ? damage : "?");
  }
}
#endif

int link_fast_start(uint32_t other, const struct link_fast_nav *n, const char *refused)
{
  uint32_t mine = link_fast_offer();
  uint32_t t0 = nspire_ticks();
  int ok;

  nav = n;
  state = F_OFF;
  role = 0;
  reset_counts();
  if (!d)
  {
#ifdef LINK_FAST_SIM
    /* The rig: the last DMA_ALIGN slot of a page, the worst place the
     * alignment allows (its addresses keep the page offset). */
    d = (struct dma *)((((uintptr_t)dma_raw + 4095) & ~(uintptr_t)4095) + 4096 - DMA_ALIGN);
#else
    d = (struct dma *)(((uintptr_t)dma_raw + DMA_ALIGN - 1) & ~(uintptr_t)(DMA_ALIGN - 1));
#endif
  }
#ifdef LINK_FAST_SIM
  fast_sim_region(d, sizeof(*d));
#endif
  /* What the last link couldn't give back (a schedule that wouldn't stop)
   * goes back now, or this one isn't tried. */
  if (link_fast_holds() && !give_back() && !refused)
    refused = "the USB controller from the last link isn't given back yet";
  if (!link_fast_holds())
    damage[0] = 0;
  if (!(mine & 3) || !(other & 3))
  {
    not_used("offer", !(other & 3) ? (other ? "the other side can't" : "the other gpSP doesn't offer it (an older build, or its Link speed is Compatible)")
                                   : "this calculator can't (a CX II at the device end, or the port isn't up)");
    return 0;
  }
  if ((other >> 8 & 0xFF) != LINK_FAST_VERSION || (mine & other & 3) || ((mine | other) & 3) != 3)
  {
    not_used("offer", "both at the same end, or another version");
    return 0;
  }
#ifndef LINK_FAST_SIM
  if (!refused && !in_sdram(PHYS(d), sizeof(*d)))
    refused = "gpSP's memory isn't SDRAM one to one";
#endif
  tried = 1;   /* from here the other side's control messages are ours */
  if (refused)
  {
    /* The arbiter's no (nspire_arbiter.c): the other side, trying now, is
     * told at once, so it gives up in one NavNet round trip, not 5 s. */
    say_fail(refused);
    not_used("this side", refused);
    return 0;
  }
#ifndef LINK_FAST_SIM
  {
    static int registered;
    if (!registered)
      registered = atexit(at_exit) == 0;
  }
#endif
  ever = 1;
  in_start = 1;
  got_ready = got_fail = 0;
  ok = mine & LINK_FAST_CAN_HOST ? start_host() : start_device();
  in_start = 0;
  if (state == F_BROKEN)
    return 0;
  if (!ok)
  {
    role = 0;
    return 0;
  }
  state = F_UP;
  up_at = nspire_ticks();
  if (role == 1)
    nspire_link_log("fast transport: up in %u ms: host, device %u at %s speed, endpoint %u, %u-byte packets", ticks_us(up_at - t0) / 1000,
                    h_addr, h_eps == 2 ? "high" : "full", h_ep, ready_mp);
  else
    nspire_link_log("fast transport: up in %u ms: device, endpoint %u, %u-byte packets", ticks_us(up_at - t0) / 1000, ep, mp);
  return 1;
}

void link_fast_stop(const char *why)
{
  if (state == F_UP)
  {
    /* The link's BYE went already, on the fast transport: what's left
     * goes first (briefly), then everything back. */
    struct clock c;
    clock_start(&c);
    while (state == F_UP && (out_busy || n_wire != n_end) && clock_ms(&c) < 100)
      poll_up();
    if (role == 1 && sched_ours)
      sched_stop();
    give_back();
    nspire_link_log("fast transport: stopped (%s): up %u ms, packets %u out, %u in", why,
                    ticks_us(nspire_ticks() - up_at) / 1000, pk_out, pk_in);
  }
  else
    give_back();
  state = F_OFF;
  role = 0;
  tried = 0;
  rq_head = rq_tail = 0;
}

void link_fast_second(char *text, unsigned size)
{
  static const char *names[] = { "off", "up", "resuming", "fell back", "broken" };
  static unsigned last_out, last_in;
  if (!size)
    return;
  text[0] = 0;
  if (!ever)
    return;
  snprintf(text, size, "; fast %s, %u out %u in", names[state], pk_out - last_out, pk_in - last_in);
  last_out = pk_out;
  last_in = pk_in;
}

void link_fast_stats(char *text, unsigned size)
{
  static const char *names[] = { "off", "up", "falling back (RESUME awaited)", "fell back", "broken" };
  if (!size)
    return;
  text[0] = 0;
  if (!ever)
    return;
  snprintf(text, size, "fast transport %s (%s): packets %u out (%u B), %u in (%u B), the longest send %u us, polls %u, "
           "fallbacks %u, messages %u sent, %u came, the USB line turned on by the OS %u times%s%s%s",
           names[state], role == 1 ? "host" : role == 2 ? "device" : "-", pk_out, by_out, pk_in, by_in,
           ticks_us(out_longest), polls, fallbacks, n_end, rx_count, irq_again,
           parked ? "; HELD: the schedule wouldn't stop" : "", why_down[0] ? "; last: " : "", why_down);
}
