/* The two calculators' USB controllers for nspire_link_fast.c on the PC
 * (usb_test.c's FAST cases): side a's and b's registers and the bus
 * between them, as far as the fast transport and what it must leave alone
 * go. What's modelled:
 * - a CX II's FOTG210 as USB host: run, the async schedule (enable, its
 *   status following at once), its address, the port; its OS's queue head
 *   for the other calculator's NavNet endpoint 1 with an IN transfer
 *   always waiting (NAKed: the device's OS has nothing to send).
 * - a CX's ChipIdea as USB device: its OS's endpoints 0 and 1 enabled
 *   (NavNet), 2 and 3 off, a 4-endpoint queue head list whose unused heads
 *   hold a pattern (so a restore can be checked word for word), prime,
 *   flush, status, complete (write one to clear), the endpoint controls
 *   with their stall bits, OTGSC's VBUS, the bus reset/suspend status.
 * - or a CX II as device (its FOTG210's own device registers aren't
 *   modelled: it offers nothing), or a CX as host (ChipIdea's host
 *   registers at 0x140).
 * - the VIC's enable (bit 8 the USB's): masked, that side's OS takes no
 *   NavNet (usb_test.c's channel_read asks fast_sim_navnet_frozen); the
 *   host's schedule not the OS's: nobody's NavNet moves. A NavNet write
 *   from the host while the device is masked sets the device's endpoint 1
 *   OUT complete bit, as its controller would (its OS isn't there to
 *   clear it).
 * - a transaction: an active qTD of the host's against the device's
 *   primed dTD of the same endpoint and direction, copied whole (a short
 *   packet ends it; the fast transport always ends with one); not primed:
 *   NAK, tried again; endpoint off or the device not there: transaction
 *   error, halted; stalled: halted.
 * Faults (usb_fast_sim_fault): no answer (every transaction to an endpoint
 * but the OS's NAKed), stall after N packets (the device's endpoint),
 * the cable out after N packets (both ports, VBUS, and the bus), the OS
 * turning the USB interrupt line on at both sides after N packets, the
 * async schedule's status (ASS) lagging its enable for 250,000 reads at
 * the take-over's stop (more than SCHED_POLLS) or 1,000,000 at the
 * release's stop (more than two) or at its start of the OS's schedule
 * again (SIM_FAULT_SLOW_RESTART: the r6 pull's "didn't start again"; the
 * USBSTS reads from that start counted, usb_fast_sim_restart_reads).
 * EHCI 1.0 4.8 (2026-11-11, F1): ASYNCLISTADDR holds the last queue head
 * walked's horizontal pointer, and a walk may end mid-ring (every third
 * hop here): while ours it is qh[0] or qh[1], while the OS's either of its
 * two queue heads (endpoint 1 IN, the ring's head; endpoint 1 OUT, where
 * NavNet's acks to the device wait: usb_fast_sim_os_out). The schedule runs
 * while ASS says so, not ASE.
 * What it flags (usb_fast_sim_restored): ASYNCLISTADDR written while ASS
 * was on; a queue head across a 4 KB page (DMA addresses keep the page
 * offset); our transfers done while either side's USB interrupt line was
 * on (more than 4: a side re-masks at its next poll); and the device's
 * endpoint 3 queue heads written (the OS's list is taken to end at
 * endpoint 2's: F7).
 * Everything is under one lock; the bus moves whenever either side reads
 * a register or ends a DMA update (its polls). */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_fast_sim.h"

#define ACTIVE 0x80u
#define HALTED 0x40u
#define XACT   0x08u
#define BABBLE 0x10u
#define TLEN(t) (((t) >> 16) & 0x7FFF)

struct region { uint8_t *p; unsigned len; uint32_t phys; };

static struct
{
  int model;          /* SIM_CX2_HOST, SIM_CX_DEVICE, SIM_CX2_DEVICE, SIM_CX_HOST */
  uint32_t vic;
  /* host */
  uint32_t usbcmd, async, portsc, os_async, os_out_qh, ass;
  long ass_left;
  long restart_reads;   /* USBSTS reads since the release's start (the 4th ASE change), -1 before */
  int ase_changes, bad_async_writes, qh_cross, irq_seen, past_list;
  /* device */
  uint32_t eplist, devaddr, otgsc, usbsts, epstat, complete, epctrl[4];
  struct region reg[4];
  int nreg;
} s[2];

static pthread_mutex_t mu = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static int cable = 1, fault, fault_after, packets;
static unsigned walk_hops;
static uint32_t next_phys = 0x10000000u;

/* The OS's memory each side: the host's queue head + qTD, the device's
 * queue head list (2 KB aligned). */
static uint8_t os_mem[2][8192] __attribute__((aligned(4096)));
static uint32_t dqh_pattern[2][64];   /* endpoints 2-3, as the OS left them */

static void add_region(int side, void *p, unsigned len)
{
  int i;
  for (i = 0; i < s[side].nreg; i++)
    if (s[side].reg[i].p == p)
      return;
  s[side].reg[s[side].nreg].p = p;
  s[side].reg[s[side].nreg].len = len;
  /* The page offset kept, as SDRAM one to one has it. */
  s[side].reg[s[side].nreg].phys = next_phys + (uint32_t)((uintptr_t)p & 0xFFF);
  next_phys += (len + 0x1000 + 0xFFFFF) & ~0xFFFFFu;
  s[side].nreg++;
}

static uint32_t phys_of(const void *p)
{
  int k, i;
  for (k = 0; k < 2; k++)
    for (i = 0; i < s[k].nreg; i++)
      if ((const uint8_t *)p >= s[k].reg[i].p && (const uint8_t *)p < s[k].reg[i].p + s[k].reg[i].len)
        return s[k].reg[i].phys + (uint32_t)((const uint8_t *)p - s[k].reg[i].p);
  fprintf(stderr, "usb_fast_sim: %p isn't DMA memory\n", p);
  abort();
}

static void *virt_of(uint32_t a)
{
  int k, i;
  for (k = 0; k < 2; k++)
    for (i = 0; i < s[k].nreg; i++)
      if (a >= s[k].reg[i].phys && a < s[k].reg[i].phys + s[k].reg[i].len)
        return s[k].reg[i].p + (a - s[k].reg[i].phys);
  fprintf(stderr, "usb_fast_sim: address %08x isn't DMA memory\n", (unsigned)a);
  abort();
}

static int is_host(int k) { return s[k].model == SIM_CX2_HOST || s[k].model == SIM_CX_HOST; }
static int os_qh(int k, uint32_t a) { a &= ~31u; return a == s[k].os_async || a == s[k].os_out_qh; }
static int is_ci_device(int k) { return s[k].model == SIM_CX_DEVICE; }

void usb_fast_sim_setup(int model_a, int model_b)
{
  int k, i;
  pthread_mutex_lock(&mu);
  memset(s, 0, sizeof(s));
  s[0].restart_reads = s[1].restart_reads = -1;
  next_phys = 0x10000000u;
  cable = 1;
  fault = SIM_FAULT_NONE;
  packets = 0;
  s[0].model = model_a;
  s[1].model = model_b;
  for (k = 0; k < 2; k++)
  {
    s[k].vic = 1u << 8 | 1u << 3;
    memset(os_mem[k], 0, sizeof(os_mem[k]));
    add_region(k, os_mem[k], sizeof(os_mem[k]));
    if (is_host(k))
    {
      /* the OS's ring: its head (device 1, endpoint 1 IN, high speed) with
       * an IN qTD always waiting, and endpoint 1 OUT's, idle */
      uint32_t *qh = (uint32_t *)os_mem[k], *qtd = (uint32_t *)(os_mem[k] + 256);
      uint32_t *qo = (uint32_t *)(os_mem[k] + 128);
      s[k].os_async = phys_of(qh);
      s[k].os_out_qh = phys_of(qo);
      qh[0] = s[k].os_out_qh | 2;
      qh[1] = 1 | (1u << 8) | (2u << 12) | (1u << 15) | (512u << 16);
      qh[2] = 1u << 30;
      qtd[0] = qtd[1] = 1;
      qtd[2] = (64u << 16) | (3u << 10) | (1u << 8) | ACTIVE;
      qtd[3] = phys_of(os_mem[k] + 4096);
      qh[4] = phys_of(qtd);
      qh[5] = 1;
      qo[0] = s[k].os_async | 2;
      qo[1] = 1 | (1u << 8) | (2u << 12) | (512u << 16);
      qo[2] = 1u << 30;
      qo[4] = qo[5] = 1;
      s[k].async = s[k].os_async;
      s[k].usbcmd = 1 | (1u << 5);
      s[k].ass = 1;
      s[k].portsc = 5;
    }
    else if (is_ci_device(k))
    {
      uint32_t *list = (uint32_t *)(os_mem[k] + 2048);
      s[k].eplist = phys_of(list);
      s[k].devaddr = 1u << 25;
      s[k].otgsc = 0x120 | (1u << 11);
      s[k].epctrl[0] = 0x00800080;
      s[k].epctrl[1] = (2u << 2) | (1u << 7) | (2u << 18) | (1u << 23);
      for (i = 0; i < 64; i++)
        list[64 + i] = dqh_pattern[k][i] = 0x0D000000u | (uint32_t)(k << 16) | (uint32_t)i;
    }
  }
  pthread_mutex_unlock(&mu);
}

void usb_fast_sim_fault(int f, int after)
{
  int k;
  pthread_mutex_lock(&mu);
  fault = f;
  fault_after = after;
  packets = 0;
  for (k = 0; k < 2; k++)
  {
    s[k].ase_changes = 0;
    s[k].restart_reads = -1;
    if (s[k].ass_left)
    {
      s[k].ass_left = 0;
      s[k].ass = s[k].usbcmd & (1u << 5);
    }
  }
  pthread_mutex_unlock(&mu);
}

/* NavNet's ack to the device waiting in the host's OS's ring (endpoint 1
 * OUT's qTD active, NAKed: the device's OS has its OUT endpoint unprimed
 * here) or gone. */
void usb_fast_sim_os_out(int host, int waiting)
{
  uint32_t *qo, *qtd;
  pthread_mutex_lock(&mu);
  if (is_host(host))
  {
    qo = (uint32_t *)(os_mem[host] + 128);
    qtd = (uint32_t *)(os_mem[host] + 384);
    if (waiting)
    {
      qtd[0] = qtd[1] = 1;
      qtd[2] = (8u << 16) | (3u << 10) | (0u << 8) | ACTIVE;
      qtd[3] = phys_of(os_mem[host] + 4096 + 512);
      qo[4] = phys_of(qtd);
    }
    else
    {
      qtd[2] &= ~ACTIVE;
      qo[6] &= ~ACTIVE;
      qo[4] = 1;
    }
  }
  pthread_mutex_unlock(&mu);
}

static void cable_out_locked(void)
{
  int k;
  cable = 0;
  for (k = 0; k < 2; k++)
  {
    s[k].portsc = 0;
    s[k].otgsc &= ~(1u << 11);
  }
}

void usb_fast_sim_cable(int in)
{
  int k;
  pthread_mutex_lock(&mu);
  if (!in)
    cable_out_locked();
  else
  {
    cable = 1;
    for (k = 0; k < 2; k++)
    {
      if (is_host(k))
        s[k].portsc = 5;
      if (is_ci_device(k))
        s[k].otgsc |= 1u << 11;
    }
  }
  pthread_mutex_unlock(&mu);
}

/* ---- The bus ---- */

static void finish(uint32_t *qh, uint32_t *qtd, uint32_t token)
{
  qh[6] = token;
  qtd[2] = token;
}

static void transact(int h, uint32_t *qh)
{
  int dv = !h;
  uint32_t *qtd = virt_of(qh[3]);
  uint32_t tok = qh[6], len = TLEN(tok), pid = (tok >> 8) & 3;
  unsigned dev = qh[1] & 0x7F, ep = (qh[1] >> 8) & 15;
  uint32_t ctrl, bit, *dqh, *dtd, dlen, n;

  if (!cable || !is_ci_device(dv) || (s[dv].devaddr >> 25) != dev || ep > 3)
  {
    finish(qh, qtd, (tok & ~ACTIVE) | HALTED | XACT);
    return;
  }
  if (fault == SIM_FAULT_NOANSWER && ep != 1)
    return;   /* NAK */
  ctrl = s[dv].epctrl[ep];
  if (pid == 0 ? !(ctrl & (1u << 7)) : !(ctrl & (1u << 23)))
  {
    finish(qh, qtd, (tok & ~ACTIVE) | HALTED | XACT);
    return;
  }
  if (pid == 0 ? ctrl & 1u : ctrl & (1u << 16))
  {
    finish(qh, qtd, (tok & ~ACTIVE) | HALTED);   /* STALL */
    return;
  }
  bit = pid == 0 ? 1u << ep : 0x10000u << ep;
  if (!(s[dv].epstat & bit))
    return;   /* NAK */
  dqh = virt_of(s[dv].eplist + 64 * (2 * ep + (pid != 0)));
  dtd = virt_of(dqh[1]);
  dlen = TLEN(dtd[1]);
  if (pid == 0)
  {
    n = len < dlen ? len : dlen;
    memcpy(virt_of(dtd[2]), virt_of(qtd[3]), n);
    finish(qh, qtd, (tok & ~(ACTIVE | 0x7FFF0000u)) | ((len - n) << 16));
    dtd[1] = (dtd[1] & ~(ACTIVE | 0x7FFF0000u)) | ((dlen - n) << 16);
  }
  else
  {
    n = dlen;
    if (n > len)
    {
      finish(qh, qtd, (tok & ~ACTIVE) | HALTED | BABBLE);
      return;
    }
    memcpy(virt_of(qtd[3]), virt_of(dtd[2]), n);
    finish(qh, qtd, (tok & ~(ACTIVE | 0x7FFF0000u)) | ((len - n) << 16));
    dtd[1] = (dtd[1] & ~(ACTIVE | 0x7FFF0000u));
  }
  dqh[3] = dtd[1];
  s[dv].epstat &= ~bit;
  s[dv].complete |= bit;
  s[dv].usbsts |= 1;
  if (ep != 1 && ((s[h].vic | s[dv].vic) & (1u << 8)))
    s[(s[h].vic & (1u << 8)) ? h : dv].irq_seen++;
  if (ep != 1 && ++packets == fault_after)
  {
    if (fault == SIM_FAULT_STALL)
      s[dv].epctrl[ep] |= 1u | (1u << 16);
    else if (fault == SIM_FAULT_PULL)
      cable_out_locked();
    else if (fault == SIM_FAULT_VIC_ON)
      s[h].vic |= 1u << 8, s[dv].vic |= 1u << 8;
  }
}

static void bus(void)
{
  int h, hops;
  uint32_t a, first;

  for (h = 0; h < 2; h++)
  {
    if (is_ci_device(h))
    {
      /* F7: endpoint 3's queue heads lie past the OS's list here */
      uint32_t *list = (uint32_t *)(os_mem[h] + 2048);
      int i;
      for (i = 32; i < 64; i++)
        if (list[64 + i] != dqh_pattern[h][i])
        {
          s[h].past_list++;
          break;
        }
    }
    if (!is_host(h) || !s[h].ass || !s[h].async)
      continue;
    first = a = s[h].async & ~31u;
    for (hops = 0; hops < 8; hops++)
    {
      uint32_t *qh = virt_of(a);
      if ((a & 0xFFF) > 0x1000 - 48)
        s[h].qh_cross++;
      if (!(qh[6] & ACTIVE) && !(qh[6] & HALTED) && !(qh[4] & 1))
      {
        uint32_t *qtd = virt_of(qh[4]);
        qh[3] = qh[4];
        qh[4] = qtd[0];
        qh[5] = qtd[1];
        qh[6] = qtd[2];
        memcpy(qh + 7, qtd + 3, 20);
      }
      if (qh[6] & ACTIVE)
        transact(h, qh);
      if ((qh[0] & 1) || (qh[0] & 6) != 2)
        break;
      a = qh[0] & ~31u;
      s[h].async = a;   /* EHCI 4.8: the last queue head's horizontal pointer */
      if (a == first || ++walk_hops % 3 == 0)
        break;
    }
  }
}

/* ---- One side's registers ---- */

/* USBSTS's ASS: it follows ASE, after the lag a fault set. */
static uint32_t ass_read(int k)
{
  uint32_t ase = s[k].usbcmd & (1u << 5);
  if (s[k].restart_reads >= 0)
    s[k].restart_reads++;
  if (!s[k].ass != !ase)
  {
    if (s[k].ass_left > 0)
      s[k].ass_left--;
    else
      s[k].ass = ase;
  }
  return s[k].ass ? 1u << 15 : 0;
}

static void usbcmd_write(int k, uint32_t v)
{
  uint32_t old = s[k].usbcmd, ase = v & (1u << 5);
  s[k].usbcmd = v;
  if (!((old ^ v) & (1u << 5)))
    return;
  s[k].ase_changes++;
  if (fault == SIM_FAULT_SLOW_TAKE && s[k].ase_changes == 1)
    s[k].ass_left = 250000;
  else if (fault == SIM_FAULT_SLOW_RELEASE && s[k].ase_changes == 3)
    s[k].ass_left = 1000000;
  else if (fault == SIM_FAULT_SLOW_RESTART && s[k].ase_changes == 4 && ase)
    s[k].ass_left = 1000000;
  else
    s[k].ass_left = 0;
  if (!s[k].ass == !ase)
    s[k].ass_left = 0;
  if (!s[k].ass_left)
    s[k].ass = ase;
  if (s[k].ase_changes == 4 && ase)
    s[k].restart_reads = 0;
}

long usb_fast_sim_restart_reads(int side)
{
  long n;
  pthread_mutex_lock(&mu);
  n = s[side].restart_reads;
  pthread_mutex_unlock(&mu);
  return n;
}

static uint32_t rd(int k, uint32_t a)
{
  uint32_t v = 0, off = a - 0xB0000000u;
  pthread_mutex_lock(&mu);
  bus();
  if (a == 0xDC000010u)
    v = s[k].vic;
  else if (a == 0x900B0018u)
    v = 0;
  else if (s[k].model == SIM_CX2_HOST || s[k].model == SIM_CX2_DEVICE)
  {
    switch (off)
    {
    case 0x00: v = 0x01000010; break;
    case 0x10: v = s[k].usbcmd; break;
    case 0x14: v = ass_read(k); break;
    case 0x28: v = s[k].async; break;
    case 0x30: v = s[k].portsc; break;
    case 0x80: v = s[k].model == SIM_CX2_HOST ? 0x00080000 : 0x00310000; break;
    }
  }
  else
  {
    int host = s[k].model == SIM_CX_HOST;
    switch (off)
    {
    case 0x000: v = 0xFA05; break;
    case 0x100: v = 0x01000040; break;
    case 0x124: v = 0x184; break;
    case 0x140: v = host ? s[k].usbcmd : 1; break;
    case 0x144: v = host ? ass_read(k) : s[k].usbsts; break;
    case 0x154: v = s[k].devaddr; break;
    case 0x158: v = host ? s[k].async : s[k].eplist; break;
    case 0x184: v = host ? s[k].portsc : (2u << 26) | 1; break;
    case 0x1A4: v = s[k].otgsc; break;
    case 0x1A8: v = host ? 3 : 2; break;
    case 0x1B0: case 0x1B4: v = 0; break;
    case 0x1B8: v = s[k].epstat; break;
    case 0x1BC: v = s[k].complete; break;
    default:
      if (off >= 0x1C0 && off < 0x1D0)
        v = s[k].epctrl[(off - 0x1C0) / 4];
    }
  }
  pthread_mutex_unlock(&mu);
  return v;
}

static void wr(int k, uint32_t a, uint32_t v)
{
  uint32_t off = a - 0xB0000000u;
  int i;
  pthread_mutex_lock(&mu);
  if (a == 0xDC000010u)
  {
    s[k].vic |= v;
    if ((v & (1u << 8)) && is_ci_device(k))
      s[k].complete &= ~2u;   /* the OS's interrupt takes its endpoint 1's news */
  }
  else if (a == 0xDC000014u)
    s[k].vic &= ~v;
  else if (is_host(k))
  {
    uint32_t base = s[k].model == SIM_CX2_HOST ? 0x10 : 0x140;
    if (off == base)
      usbcmd_write(k, v);
    else if (off == base + 0x18)
    {
      if (s[k].ass)
        s[k].bad_async_writes++;   /* EHCI: never while the schedule runs */
      s[k].async = v;
    }
  }
  else if (is_ci_device(k))
  {
    switch (off)
    {
    case 0x1B0:
      for (i = 0; i < 32; i++)
        if (v & (1u << i))
        {
          unsigned ep = i & 15, in = i >= 16;
          uint32_t *dqh = virt_of(s[k].eplist + 64 * (2 * ep + in));
          if (!(dqh[2] & 1))
          {
            dqh[1] = dqh[2];
            s[k].epstat |= 1u << i;
          }
        }
      break;
    case 0x1B4:
      s[k].epstat &= ~v;
      break;
    case 0x1BC:
      s[k].complete &= ~v;
      break;
    default:
      if (off >= 0x1C0 && off < 0x1D0)
        s[k].epctrl[(off - 0x1C0) / 4] = v & ~((1u << 6) | (1u << 22));
    }
  }
  bus();
  pthread_mutex_unlock(&mu);
}

int usb_fast_sim_navnet_frozen(int reader)
{
  int k, frozen = 0;
  pthread_mutex_lock(&mu);
  if (!(s[reader].vic & (1u << 8)))
    frozen = 1;
  for (k = 0; k < 2; k++)
    if (is_host(k) && (!os_qh(k, s[k].async) || !s[k].ass))
      frozen = 1;
  pthread_mutex_unlock(&mu);
  return frozen;
}

void usb_fast_sim_navnet_wrote(int writer)
{
  int other = !writer;
  pthread_mutex_lock(&mu);
  if (is_ci_device(other) && !(s[other].vic & (1u << 8)) && cable &&
      is_host(writer) && os_qh(writer, s[writer].async))
    s[other].complete |= 2u;
  pthread_mutex_unlock(&mu);
}

/* After a case: everything as the OS left it? 0, or what isn't. */
const char *usb_fast_sim_restored(void)
{
  static char why[120];
  int k, i;
  why[0] = 0;
  pthread_mutex_lock(&mu);
  for (k = 0; k < 2 && !why[0]; k++)
  {
    if (!(s[k].vic & (1u << 8)))
      snprintf(why, sizeof(why), "side %c: the USB interrupt left masked", 'a' + k);
    else if (is_host(k) && (!os_qh(k, s[k].async) || (s[k].usbcmd & 0x21) != 0x21))
      snprintf(why, sizeof(why), "side %c: the async schedule %08x (OS's %08x / %08x), USBCMD %08x", 'a' + k,
               (unsigned)s[k].async, (unsigned)s[k].os_async, (unsigned)s[k].os_out_qh, (unsigned)s[k].usbcmd);
    else if (s[k].bad_async_writes)
      snprintf(why, sizeof(why), "side %c: ASYNCLISTADDR written %d times while the schedule ran (ASS on)", 'a' + k,
               s[k].bad_async_writes);
    else if (s[k].qh_cross)
      snprintf(why, sizeof(why), "side %c: a queue head across a 4 KB page (%d walks)", 'a' + k, s[k].qh_cross);
    else if (s[k].irq_seen > 4)
      snprintf(why, sizeof(why), "side %c: %d of our transfers done with its USB interrupt line on", 'a' + k,
               s[k].irq_seen);
    else if (s[k].past_list)
      snprintf(why, sizeof(why), "side %c: endpoint 3's queue heads (past the OS's list) written", 'a' + k);
    else if (is_ci_device(k))
    {
      uint32_t *list = (uint32_t *)(os_mem[k] + 2048);
      if (s[k].epctrl[2] || s[k].epctrl[3] || s[k].epctrl[1] != ((2u << 2) | (1u << 7) | (2u << 18) | (1u << 23)))
        snprintf(why, sizeof(why), "side %c: endpoint controls %08x %08x %08x", 'a' + k, (unsigned)s[k].epctrl[1],
                 (unsigned)s[k].epctrl[2], (unsigned)s[k].epctrl[3]);
      else if (s[k].complete & 0xFFFCFFFCu)
        snprintf(why, sizeof(why), "side %c: completes %08x left", 'a' + k, (unsigned)s[k].complete);
      for (i = 0; i < 64 && !why[0]; i++)
        if (list[64 + i] != dqh_pattern[k][i])
          snprintf(why, sizeof(why), "side %c: queue head word %d %08x, the OS's %08x", 'a' + k, 64 + i,
                   (unsigned)list[64 + i], (unsigned)dqh_pattern[k][i]);
    }
  }
  pthread_mutex_unlock(&mu);
  return why[0] ? why : NULL;
}

#define SIDE_FNS(sd, k)                                                                \
  uint32_t sd##_fast_sim_rd(uint32_t a) { return rd(k, a); }                           \
  void sd##_fast_sim_wr(uint32_t a, uint32_t v) { wr(k, a, v); }                       \
  uint32_t sd##_fast_sim_phys(const void *p) { uint32_t r; pthread_mutex_lock(&mu); r = phys_of(p); pthread_mutex_unlock(&mu); return r; } \
  void *sd##_fast_sim_virt(uint32_t a) { void *r; pthread_mutex_lock(&mu); r = virt_of(a); pthread_mutex_unlock(&mu); return r; } \
  void sd##_fast_sim_region(void *p, unsigned len) { pthread_mutex_lock(&mu); add_region(k, p, len); pthread_mutex_unlock(&mu); } \
  void sd##_fast_sim_lock(void) { pthread_mutex_lock(&mu); }                           \
  void sd##_fast_sim_unlock(void) { bus(); pthread_mutex_unlock(&mu); }                \
  int sd##_fast_sim_cx2(void) { return s[k].model == SIM_CX2_HOST || s[k].model == SIM_CX2_DEVICE; }
SIDE_FNS(a, 0)
SIDE_FNS(b, 1)
