/* The calculators' link cable (nspire_link.h): NavNet, the OS's own link
 * between calculators and to a computer, over the USB port. Every model
 * gpSP runs on has it (CX, CX CAS, CX II, CX II CAS, CX II-T; OS 3.1 to
 * 6.4, except a CX's 3.9.1: Ndless's syscall tables), so any two of them
 * link, in either order and with either at the cable's host end: nothing
 * here depends on the model or on which end is the USB host (each side
 * both serves and connects, so it doesn't matter which one finds the
 * other), and the hello's nonce, not the cable, picks player 1. (Not yet
 * seen on two calculators: the link's log, gpsp_link.txt.tns, says what
 * happened.)
 *
 * Two channels, one each way. gpSP runs a NavNet service and connects to
 * the other calculator's: what this side sends goes on the channel it
 * opened, and what comes, on the channel the other side opened to this
 * one's service. The OS runs a service's callback in a task of its own,
 * which reads into a ring the game empties and, where the OS can be slept
 * in (nspire_os.h), also writes what the game queued (svc_writes): a NavNet
 * write waits for the other side to take it (ND Screen Mirror saw 40 ms,
 * and 60 s once with the other end gone), so the game never writes, reads
 * or waits on NavNet, and each channel has one task on it. Without the
 * OS's sleep the game writes its channel itself, as before. If after
 * ONE_CHANNEL_MS only one channel came up (the other side couldn't reach
 * this one's service), it carries both ways.
 *
 * The knock (DETECT, 2026-11-11): the OS starts a service's callback only
 * when the first data comes on the connection, not at the Connect. Every
 * calculator log (r1-r5) shows it: "the other side connected to this one's
 * service" was never logged before the other side's first write, both
 * sides connected and each waited ONE_CHANNEL_MS for the other's channel
 * (every "Link on its own" gave up: "only this side's channel after 4000
 * ms"; a picked Link went one-channel after 4 s), and the CX II's two
 * channels came 170 ms after the CX CAS's first hello (r5). So right after
 * its Connect a side writes an empty message (a length word of 0, which
 * every build's reader drops) on its channel: the other side's callback
 * starts, and both have both channels within a write's time.
 *
 * Looks (DETECT): a port change (link_cable_hurry, from nspire_link.c's
 * per-frame port read) makes the next look at once, then one every
 * LINK_HURRY_LOOK_MS (or LOOK_COST_TIMES a look's own cost, if more: a
 * CX's empty look took 44-54 ms) for LINK_HURRY_MS, then the usual rate;
 * and until a node is listed in that window the OS gets up to
 * COMEUP_OS_MS more of each frame (its timers run only in gpSP's sleeps).
 *
 * The cable is watched for while no link is up (link_cable_watch): once
 * the USB port's controller says something's on it (nspire_os.h), the
 * service is up, a look for the other calculator's every WATCH_LOOK_MS,
 * and the OS gets the CPU in spare time and at least once a frame (at
 * least every WATCH_SLEEP_MS where the port can't tell), so a cable
 * plugged in while gpSP runs is set up by the OS and found, and
 * nspire_link.c links on its own. With nothing on the port nothing's
 * watched and the OS gets no time; on a CX whose OS has turned off the
 * controller's bus access (it reads "can't tell" then) a tick every
 * WATCH_SLEEP_MS and a look every GATED_LOOK_MS (link_cable_gated), so it
 * can turn it on again when a cable comes. A connection that
 * comes meanwhile is held: its callback reads what comes.
 *
 * Each connection's callback starts the link's shared state anew (svc_gen
 * and, under the same IRQs-off, the ring of what came, and whether it
 * broke): nothing of an earlier connection (a message cut off when the
 * cable came out, its break) reaches the next one. And a close or a newer
 * connection makes an older callback leave without touching the rings: it
 * checks its gen under IRQs off before each put or get. A channel is never
 * Disconnected while the service's task writes on it (a write the other
 * side doesn't take can wait a minute: ND Screen Mirror saw 60 s, and
 * NavNet's deletion of a channel waits without a deadline for its users):
 * its Disconnect waits for the write's end (stale_ch), at the next open,
 * watch or close.
 *
 * What ND Screen Mirror (GigaZelensky/Nspire-ScreenMirror-Ndless, the one
 * program known to keep a NavNet service up, on a CX II's OS 6.2) found on
 * the calculators, and what's done about it here:
 * - Ndless's syscall gateway keeps the caller's return in one shared place
 *   (one level of nesting, Ndless resources/ints.c), so two tasks in
 *   syscalls that block, a read in the service's and a write in the game's,
 *   can each return into the other's code. So TI_NN_Read and TI_NN_Write
 *   are called at their OS addresses (asked of Ndless once, before the
 *   service starts), never through the gateway, in either task.
 * - TI_NN_Read's public entry makes any wait of 1 to 61,999 ms 62 s, then
 *   jumps to the channel's read, which takes short waits; a wait that ends
 *   with nothing is a success with no bytes. Where the entry is that
 *   wrapper (its 8 instructions and 2 numbers, checked here) the reads go
 *   to the channel's read with short waits; elsewhere (an OS whose entry is
 *   something else) to the entry, with long ones.
 * - TI_NN_StopService deletes the service's channels and waits, without a
 *   deadline, for their users to let go: so it's called only when no
 *   callback is running, and the service stays up from the first Link
 *   until gpSP closes (link_cable_shutdown), not stopped at each unlink.
 *   A callback's task runs only while gpSP's lets go of the CPU, so the
 *   waits for it to leave sleep in the OS, and two more OS ticks follow
 *   (ND Screen Mirror's: the OS's dispatch wrapper lets go of a callback's
 *   references after it returns).
 *
 * Waiting for a message, the game spins, then sleeps an OS tick (or,
 * without the OS's sleep, blocks a moment in a read on its own channel,
 * where nothing comes in two-channel mode), which lets the OS run its other
 * tasks. The log counts the messages that came while spinning and after a
 * block.
 *
 * What the first link on two calculators showed (a CX II and a CX CAS with
 * an OTG cable, 2026-11-06): NavNet finds the other at once and both
 * connect, but neither service's callback started in 4 s, while each game
 * ran its frames: the OS starts the callback only when the game's task
 * lets go of the CPU. (Once it's in its read, what comes reaches it at
 * once: the hellos came while the game spun.) So while looking, the game
 * blocks a moment each frame on its channel. And the CX CAS's timer didn't
 * run: its waits for a message ended at once and never blocked, so it gave
 * up on the other 0.3 s after linking. Without a timer, a wait blocks after
 * a few looks, the OS timing it.
 *
 * Messages are a byte stream of 2-byte lengths and messages, sent in
 * pieces of 254 bytes: a CX's NavNet packets (a CX II's can be bigger,
 * but the other calculator may be a CX). */

#include <os.h>
#include <libndls.h>
#include <stdio.h>
#include <string.h>

#include "nspire_link.h"
#include "nspire_link_fast.h"
#include "nspire_os.h"

#define LINK_SERVICE    0x5047   /* outside the OS's own 0x40xx services */
#define PIECE           254
#define LOOK_EVERY_MS   1500
/* DETECT: in the hurry after a port change, a look at most this many times
 * its own cost apart (a look's share of the game's time 1/8 at most). */
#define LOOK_COST_TIMES 8
/* DETECT: in the hurry, until a node is listed: the OS's ticks in up to
 * this much more of each frame (nspire_os_sleep_before: only a tick that
 * comes in time), at most COMEUP_SLEEPS_MAX of them. */
#define COMEUP_OS_MS      6
#define COMEUP_SLEEPS_MAX 3
/* Only one channel this long after the first: it carries both ways (or,
 * linking on its own, the other side isn't linking back: given up). */
#define ONE_CHANNEL_MS  4000
/* What came and isn't taken yet, and what's queued for the service's task
 * to send: the lockstep has up to the delay's frames and one more of a
 * side's in flight (nspire_link.c: LINK_DELAY_MAX, a frame's message
 * LINK_MSG_MAX at most), and the small messages between: twice that. A
 * power of two (the ring's index). */
#define RING            65536
typedef char ring_holds_the_frames[RING >= (LINK_DELAY_MAX + 2) * (LINK_MSG_MAX + 2) ? 1 : -1];
/* The service's reads wait this long (with the channel's read; the public
 * entry's 62 s otherwise): it looks at whether the link's over between
 * them. Where it also sends (svc_writes, one-channel mode) a short one,
 * after it sent what's queued. (A read ends on the OS's tick or when
 * something comes, so what the game queues while the task's in one waits
 * for that: a tick at most, less while the other side sends.) */
#define SERVICE_READ_MS      50
#define SERVICE_READ_ONE_MS  1
/* The service's task writes at most this many pieces in a row, a frame's
 * longest message and its length (LINK_MSG_MAX + 2), and starts the next
 * run only once a read found nothing more of the other side's. A write
 * waits for the other side's OS to take it, and the other side's task
 * writes too: if both wrote all they had before reading, both could wait
 * on each other for good. NavNet takes at least this much ahead (the CX
 * CAS's one-channel link on 2026-11-06: both games wrote a stand-in's
 * frames whole before reading, and it ran); with runs that fit what it
 * takes, and a run only after a read that came back empty, one side
 * always reads what the other's waiting on. */
#define WRITE_RUN            ((LINK_MSG_MAX + 2 + PIECE - 1) / PIECE)
#define SERVICE_READ_LONG_MS 1000   /* the public entry (made 62 s where it's the wrapper) */
/* Waiting for a message, the game side spins this long (nspire_ticks,
 * 32768 a second: 1 ms) before it blocks for BLOCK_MS in a read. */
#define SPIN_TICKS      33
#define BLOCK_MS        1
/* Without a timer (nspire_ticks doesn't move): this many looks, then a
 * block, which ends the wait. With one, a tick (31 us) is a few dozen. */
#define DEAD_LOOKS      2000
/* Closing: how long to wait for the service's task to leave its read. */
#define LEAVE_MS        2000
/* Watching for the cable (no link): a look this often, and where the USB
 * port can't say whether something's on it, the OS's tasks the CPU at
 * least this often (at its spare time otherwise). */
#define WATCH_LOOK_MS   1500
/* A look's nodes at most (look_for_other). */
#define LOOK_NODES      4
#define WATCH_SLEEP_MS  250
/* A CX's USB controller with its bus access off (link_cable_gated): a
 * look this often. */
#define GATED_LOOK_MS   6000
/* Closing: how long what's queued (the BYE) may take to go. */
#define WAIT_SEND_MS    300
/* gpSP closing with the service's task in a write the other side doesn't
 * take: how long to wait for it to end (ND Screen Mirror's longest, with
 * the other end gone: 60 s) before StopService, which can't wait for it. */
#define WRITE_END_MS    65000
/* A frame, in timer ticks (nspire_ticks). */
#define FRAME_TICKS     (32768 / 60)
/* P2: something on the port, nothing connected or seen, the port's last
 * change long ago: the OS's tick only when it's near (nspire_os_sleep_near,
 * a frame after the last), forced only after this long. */
#define WATCH_FORCE_MS  100

typedef int16_t (*nn_read_fn)(nn_ch_t ch, uint32_t timeout_ms, void *buf, uint32_t size, uintptr_t got);
typedef int16_t (*nn_write_fn)(nn_ch_t ch, void *buf, uint32_t size);

#ifndef LINK_USB_TEST
/* A syscall's OS address (Ndless's "address of" form of the swi): called
 * at it, a syscall skips the gateway. (The PC's test, tests/link_usb, has
 * its own: a fake NavNet.) */
#define SYSCALL_ADDRESS(number)                                                          \
  ({                                                                                     \
    register uintptr_t address_ __asm__("r0");                                            \
    __asm__ volatile("swi %[nr]"                                                         \
                     : "=r"(address_)                                                    \
                     : [nr] "i"(__SYSCALLS_ISVAR | (number))                             \
                     : "r1", "r2", "r3", "r12", "lr", "memory", "cc");                    \
    address_;                                                                            \
  })
#endif

/* nspire.c's timer, read with no call (the service's task times its
 * writes by it: nspire_ticks isn't for two tasks at once), and a count's
 * time. NULL without a timer. */
extern volatile const uint32_t *nspire_raw_timer;
uint32_t nspire_raw_timer_us(uint32_t counts);

static nn_read_fn nn_read;           /* the public entry */
static nn_read_fn nn_read_short;     /* the channel's read, if the entry is the 62 s wrapper */
static nn_write_fn nn_write;

/* A byte ring between the service's task and the game's: one writes, the
 * other reads; head and tail each change on one side only. */
struct ring
{
  uint8_t buf[RING];
  volatile unsigned head, tail;   /* tail: written up to; head: read up to */
};

static unsigned ring_used(const struct ring *r) { return (r->tail - r->head) % RING; }
static unsigned ring_free(const struct ring *r) { return RING - 1 - ring_used(r); }

static void ring_put(struct ring *r, const void *src, unsigned n)
{
  const uint8_t *p = src;
  unsigned t = r->tail, i;
  for (i = 0; i < n; i++)
    r->buf[(t + i) % RING] = p[i];
  __asm__ volatile ("" ::: "memory");
  r->tail = (t + n) % RING;
}

static void ring_get(struct ring *r, void *dst, unsigned n)
{
  uint8_t *p = dst;
  unsigned h = r->head, i;
  for (i = 0; i < n; i++)
    p[i] = r->buf[(h + i) % RING];
  __asm__ volatile ("" ::: "memory");
  r->head = (h + n) % RING;
}

/* IRQs off around a count two OS tasks may change (the callbacks run in
 * the OS's tasks, in SVC mode; FIQs stay on, as Ndless's lcd_compat wants). */
#ifdef LINK_USB_TEST
unsigned link_usb_test_irq_off(void);
void link_usb_test_irq_restore(unsigned old);
#define irq_off link_usb_test_irq_off
#define irq_restore link_usb_test_irq_restore
#else
static unsigned irq_off(void)
{
  unsigned old, off;
  __asm__ volatile("mrs %0, cpsr\n\torr %1, %0, #0x80\n\tmsr cpsr_c, %1" : "=r"(old), "=r"(off) : : "memory");
  return old;
}

static void irq_restore(unsigned old)
{
  __asm__ volatile("msr cpsr_c, %0" : : "r"(old) : "memory");
}
#endif

/* Whether the IRQs are masked now (the CPSR's I bit). The PC test's
 * threads stand in for tasks that run at any time: never. */
static int irqs_masked(void)
{
#ifdef LINK_USB_TEST
  return 0;
#else
  unsigned cpsr;
  __asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
  return (cpsr & 0x80) != 0;
#endif
}

/* The service's side. svc_gen: the connection a callback belongs to (each
 * connection and each close start a new one); a callback of an older one
 * leaves after its read or write without touching the rings. */
static struct ring svc_in, svc_out;
static volatile int svc_tasks;        /* callbacks running */
static volatile unsigned svc_gen;
static volatile int accepting;        /* linking: a connection is taken */
static volatile int one_channel;      /* the service's channel carries both ways */
static volatile int svc_connected;    /* this link's: the other side connected */
static volatile int svc_broken;
static volatile int svc_overran;
/* What broke it (the call's result), for the game side's log: the service's
 * task can't write the log itself. */
static volatile int svc_broke_on_write;
static volatile int16_t svc_broke_rc;
static volatile int quitting;
/* A callback holds the other side's connection now (linking or not). */
static volatile int svc_alive;
/* The service's task writes what the game queued (svc_out) on out_ch. */
static volatile int svc_writes;
/* The service's task's work, for the log: turns of its loop, writes and
 * their bytes, the most queued at once, the longest write (raw timer
 * counts) and the last one's result. Reset with IRQs off (link_cable_open). */
static volatile unsigned svc_loops, svc_write_count, svc_write_bytes, svc_queue_max;
static volatile uint32_t svc_write_longest;
static volatile int16_t svc_write_rc;
/* The channel the service's task is writing on now, or NULL. */
static nn_ch_t volatile svc_in_write;
/* The service's task (its Nucleus task, as it read it in the callback), and
 * what the game's task saw of its stack: the lowest stack pointer saved
 * while it waited (in a read or write: the deepest it went that's seen). */
static volatile uint32_t svc_task;
static uint32_t svc_stack[3], svc_sp_lowest;
/* R3, for the crash ring's line each second (link_cable_second): what the
 * service's task is in now (SVC_*) and since when (raw timer counts), its
 * writes and turns this second and the longest write; a sum of its stack's
 * lowest KB, taken when first seen in a link (a change: it went that deep). */
enum { SVC_NONE, SVC_RUN, SVC_WRITE, SVC_READ };
static volatile int svc_at;
static volatile uint32_t svc_at_since;
static volatile unsigned svc_sec_writes, svc_sec_turns;
static volatile uint32_t svc_sec_longest;
/* LINK2 F2: the longest read of this second's (raw timer counts):
 * whether a CAS's OS sleep while the fast transport is up costs one of the
 * service's reads that nothing can end (the USB line masked). */
static volatile uint32_t svc_sec_read_longest;
static uint32_t stack_sum;
static int stack_sum_set;
#define STACK_SUM_WORDS 256

/* The channel this side opened. The service's task reads it (svc_writes),
 * the game's task sets it. */
static nn_ch_t volatile out_ch;
/* A channel let go of while the service's task was writing on it: its
 * Disconnect waits for the write's end (drop_stale). */
static nn_ch_t stale_ch;
/* Linking on its own (link_cable_open): both ways or nothing. */
static int automatic;
static int out_broken;
/* R6: a length word over LINK_MSG_MAX came (a damaged stream: NavNet has
 * no way to find the next message's start): the link ends at once with a
 * plain message instead of waiting 15 s for bytes that never come. */
static int stream_bad;
static int started;                   /* the service is registered */
static int resolved;
static unsigned first_channel_ms;     /* when the first channel came */
static unsigned last_look_ms;
static int ready;
/* DETECT: the hurry after a port change (link_cable_hurry), a look due at
 * once, the last look's cost, the come-up's extra OS ticks, knocks made. */
static unsigned hurry_from, look_cost_ms;
static int hurrying, look_due;
static unsigned comeup_sleeps, knocks;
/* The hurry's looks, for the log: how many since the port change, when the
 * last one that listed nothing was, whether the first node was said. */
static unsigned hurry_looks, empty_look_ms;
static int node_said;
static unsigned last_nodes;   /* nodes the last look found */
static unsigned looks;        /* looks made (SEAM-2: the arbiter counts the watch's) */
static int logged_svc, logged_out, logged_broken, logged_dead;
/* Messages that came while the game spun, and after it blocked. */
static unsigned came_spinning, came_blocked, blocks, sleeps;

/* What came on the channel this side opened (one-channel mode), not yet
 * whole messages. */
/* R2: small messages held to go in one write (link_cable_send). */
static uint8_t pend[PIECE];
static unsigned pend_len;
static uint32_t pend_since;               /* nspire_ticks when the first byte was held */
/* R3: frame ends (link_cable_frame_done) since the first byte was held. */
static unsigned pend_frames;
static unsigned direct_writes, direct_held;   /* writes on this side's channel; messages that went with another */
static uint32_t direct_ticks, direct_longest; /* their time (nspire_ticks), total and longest */

static uint8_t out_stream[RING];
static unsigned out_stream_len;

/* The last look's result, logged when it changes (a look every
 * LOOK_EVERY_MS while nothing answers). */
static char last_look[120];
static char error[80];

uint32_t nspire_ticks(void);   /* nspire.c: 32768 a second */
static int block_a_moment(void);

static unsigned now_ms(void)
{
  return (unsigned)((uint64_t)nspire_ticks() * 1000 / 32768);
}

/* The public read entry is the 62 s wrapper if it's exactly: push {r4, r5};
 * ldr r4, =61998; ldr r12, =62000; sub r5, r1, #1; cmp r5, r4;
 * movls r1, r12; pop {r4, r5}; b <the channel's read>; .word 61998, 62000
 * (OS 6.2's, ND Screen Mirror's notes). Then the branch's target. */
#ifdef LINK_USB_TEST
nn_read_fn link_usb_test_unwrap(uintptr_t entry);
#define unwrap_read link_usb_test_unwrap
#else
static nn_read_fn unwrap_read(uintptr_t entry)
{
  static const uint32_t want[7] = { 0xe92d0030, 0xe59f4014, 0xe59fc014, 0xe2415001, 0xe1550004,
                                    0x91a0100c, 0xe8bd0030 };
  const volatile uint32_t *w = (const volatile uint32_t *)entry;
  unsigned i;

  if (!entry || (entry & 3) || entry < 0x10000000 || entry >= 0x14000000)
    return NULL;
  for (i = 0; i < 7; i++)
    if (w[i] != want[i])
      return NULL;
  if ((w[7] & 0xff000000) != 0xea000000 || w[8] != 61998 || w[9] != 62000)
    return NULL;
  return (nn_read_fn)(entry + 36 + (uint32_t)((int32_t)(w[7] << 8) >> 6));
}
#endif

/* A read for the service's task (or the game's block): ms with the
 * channel's read, or the entry's long wait. */
static int16_t read_some(nn_ch_t ch, unsigned ms, void *buf, unsigned size, uint32_t *got)
{
  if (nn_read_short)
    return nn_read_short(ch, ms, buf, size, (uintptr_t)got);
  return nn_read(ch, SERVICE_READ_LONG_MS, buf, size, (uintptr_t)got);
}

/* Where the service's task sends what the game queued: this side's
 * channel (svc_writes) or, in one-channel mode, the one it reads (ch). */
static nn_ch_t svc_target(nn_ch_t ch)
{
  return svc_writes ? out_ch : one_channel ? ch : NULL;
}

/* Up to a piece of what the game queued, for the service's task, if its
 * connection is still the link's (gen): with IRQs off, so a close or a
 * newer connection (which empty svc_out) can't come in between; to is
 * then the channel being written on (svc_in_write). 0 if nothing. */
static unsigned svc_take_out(unsigned gen, nn_ch_t to, uint8_t *piece)
{
  unsigned irqs = irq_off(), n = 0;

  if (gen == svc_gen)
  {
    n = ring_used(&svc_out);
    if (n > svc_queue_max)
      svc_queue_max = n;
    if (n > PIECE)
      n = PIECE;
    if (n)
    {
      ring_get(&svc_out, piece, n);
      svc_in_write = to;
    }
  }
  irq_restore(irqs);
  return n;
}

/* The link's done with this connection (its read or write failed, or what
 * came didn't fit), if it's still the link's: with IRQs off, as the next
 * connection's start clears these. */
static void svc_break(unsigned gen, int16_t rc, int on_write, int overran)
{
  unsigned irqs = irq_off();

  if (gen == svc_gen)
  {
    svc_broke_rc = rc;
    svc_broke_on_write = on_write;
    svc_overran = overran;
    svc_broken = 1;
  }
  irq_restore(irqs);
}

/* The service's callback, in the OS's task for it: one per connection,
 * taken whether a link is up or not (until gpSP closes): the newest is the
 * other side's (svc_gen; an older one leaves after its read or write).
 * Sends what the game queued in svc_out (on out_ch with svc_writes, or in
 * one-channel mode on this one) and then reads what comes into svc_in.
 * Until the channel breaks, a newer one comes, the link closes or gpSP
 * does. No syscall through the gateway here, and no call that keeps state
 * of its own (nspire_ticks): only memory, the rings and the OS's read and
 * write at their addresses. */
static void service(nn_ch_t ch, void *unused)
{
  uint8_t piece[PIECE];
  unsigned gen, irqs, n;
  int dry = 1;   /* the last read found nothing: a write run may start */
  uint32_t task_var = nspire_os_task_var;
  (void)unused;

  irqs = irq_off();
  svc_tasks++;
  if (quitting)
  {
    svc_tasks--;
    irq_restore(irqs);
    return;
  }
  /* A new connection: the other side's stream starts here, and nothing of
   * an earlier one's (bytes of a message it cut off, its break) counts. */
  gen = ++svc_gen;
  svc_in.head = svc_in.tail = 0;
  svc_broken = svc_overran = svc_broke_on_write = 0;
  svc_broke_rc = 0;
  svc_alive = 1;
  svc_connected = 1;
  irq_restore(irqs);
  if (task_var)
    svc_task = *(volatile uint32_t *)(uintptr_t)task_var;   /* the OS's current task: this one */
  svc_at = SVC_RUN;
  while (gen == svc_gen && !quitting)
  {
    nn_ch_t to;
    uint32_t got = 0;
    int16_t rc;
    unsigned run = 0;

    /* What the game queued goes first (a read only ends on the OS's tick
     * or when something comes), up to WRITE_RUN pieces, once the last read
     * found nothing more (dry). */
    while ((to = svc_target(ch)) && dry && run < WRITE_RUN && (n = svc_take_out(gen, to, piece)))
    {
      volatile const uint32_t *timer = nspire_raw_timer;
      uint32_t t0 = timer ? *timer : 0;
      int16_t wrc;
      svc_at_since = t0;
      svc_at = SVC_WRITE;
      wrc = nn_write(to, piece, n);
      svc_at = SVC_RUN;
      if (timer && t0 - *timer > svc_write_longest)
        svc_write_longest = t0 - *timer;   /* it counts down */
      if (timer && t0 - *timer > svc_sec_longest)
        svc_sec_longest = t0 - *timer;
      svc_sec_writes++;
      svc_write_rc = wrc;
      svc_in_write = NULL;
      if (wrc < 0)
      {
        svc_break(gen, wrc, 1, 0);
        break;
      }
      svc_write_count++;
      svc_write_bytes += n;
      run++;
    }
    if (gen != svc_gen || svc_broken)
      break;
    {
      volatile const uint32_t *timer = nspire_raw_timer;
      uint32_t r0 = timer ? *timer : 0;
      svc_at_since = r0;
      svc_at = SVC_READ;
      rc = read_some(ch, to ? SERVICE_READ_ONE_MS : SERVICE_READ_MS, piece, PIECE, &got);
      svc_at = SVC_RUN;
      if (timer && r0 - *timer > svc_sec_read_longest)   /* it counts down */
        svc_sec_read_longest = r0 - *timer;
    }
    if (rc < 0)
    {
      svc_break(gen, rc, 0, 0);
      break;
    }
    dry = !got;
    if (got)
    {
      int fits;
      irqs = irq_off();
      fits = ring_free(&svc_in) >= got;
      if (gen == svc_gen && fits)
        ring_put(&svc_in, piece, got);
      irq_restore(irqs);
      if (!fits)
      {
        svc_break(gen, 0, 0, 1);
        break;
      }
    }
    svc_loops++;
    svc_sec_turns++;
  }
  irqs = irq_off();
  svc_tasks--;
  if (gen == svc_gen)
    svc_alive = 0;
  if (!svc_tasks)
    svc_at = SVC_NONE;
  irq_restore(irqs);
}

static int has_navnet(void)
{
  return nl_hassyscall(TI_NN_StartService) && nl_hassyscall(TI_NN_StopService) &&
         nl_hassyscall(TI_NN_Read) && nl_hassyscall(TI_NN_Write) &&
         nl_hassyscall(TI_NN_Connect) && nl_hassyscall(TI_NN_Disconnect) &&
         nl_hassyscall(TI_NN_NodeEnumInit) && nl_hassyscall(TI_NN_NodeEnumNext) &&
         nl_hassyscall(TI_NN_NodeEnumDone) && nl_hassyscall(TI_NN_CreateOperationHandle) &&
         nl_hassyscall(TI_NN_DestroyOperationHandle);
}

/* The OS addresses of the read and the write, once, before the service
 * starts (no other task is in a syscall then). */
static int resolve(void)
{
  uintptr_t read_at, write_at;

  if (resolved)
    return 0;
  read_at = SYSCALL_ADDRESS(e_TI_NN_Read);
  write_at = SYSCALL_ADDRESS(e_TI_NN_Write);
  nn_read = (nn_read_fn)read_at;
  nn_write = (nn_write_fn)write_at;
  nn_read_short = unwrap_read(read_at);
  if (nn_read_short)
    nspire_link_log("NavNet: read at %08x is the 62 s wrapper: reads go to %08x; write at %08x",
                    (unsigned)read_at, (unsigned)(uintptr_t)nn_read_short, (unsigned)write_at);
  else
  {
    nspire_link_log("NavNet: read at %08x isn't the known wrapper (long reads, no blocking waits); "
                    "write at %08x", (unsigned)read_at, (unsigned)write_at);
#ifndef LINK_USB_TEST
    /* Its first words, to know this OS's next time. */
    if (read_at >= 0x10000000 && read_at < 0x14000000 && !(read_at & 3))
    {
      const volatile uint32_t *w = (const volatile uint32_t *)read_at;
      nspire_link_log("  its code: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned)w[0],
                      (unsigned)w[1], (unsigned)w[2], (unsigned)w[3], (unsigned)w[4], (unsigned)w[5],
                      (unsigned)w[6], (unsigned)w[7], (unsigned)w[8], (unsigned)w[9]);
    }
#endif
  }
  if (!read_at || !write_at || (read_at & 3) || (write_at & 3))
  {
    strcpy(error, "This OS's link calls look wrong.");
    return -1;
  }
  resolved = 1;
  return 0;
}

/* The service up (from the first need until gpSP closes): 0, or < 0 with
 * error set if this calculator can't link. */
static int start_service(void)
{
  int16_t rc;

  if (started)
    return 0;
  if (!has_navnet())
  {
    strcpy(error, "This OS can't link: update it.");
    nspire_link_log("NavNet: this OS's Ndless has no NavNet calls");
    return -1;
  }
  if (resolve() < 0)
    return -1;
  quitting = 0;
  rc = (int16_t)TI_NN_StartService(LINK_SERVICE, NULL, service);
  nspire_link_log("NavNet: StartService(0x%04x) %d", LINK_SERVICE, rc);
  if (rc < 0)
  {
    strcpy(error, "Couldn't start the link on USB.");
    return -1;
  }
  started = 1;
  return 0;
}

static void drop_stale(void);
static void drop_channel(nn_ch_t ch);
static void wait_for_write(nn_ch_t ch, unsigned ms);

/* DETECT: in the hurry after a port change (LINK_HURRY_MS). */
static int hurry_now(void)
{
  if (hurrying && now_ms() - hurry_from >= LINK_HURRY_MS)
    hurrying = 0;
  return hurrying;
}

/* The time between two looks: normal, or in the hurry LINK_HURRY_LOOK_MS
 * (LOOK_COST_TIMES the last look's cost if that's more). */
static unsigned look_period(unsigned normal)
{
  unsigned p;

  if (!hurry_now())
    return normal;
  p = look_cost_ms * LOOK_COST_TIMES;
  if (p < LINK_HURRY_LOOK_MS)
    p = LINK_HURRY_LOOK_MS;
  return p < normal ? p : normal;
}

void link_cable_hurry(void)
{
  hurry_from = now_ms();
  hurrying = 1;
  look_due = 1;
  hurry_looks = empty_look_ms = 0;
  node_said = 0;
  comeup_sleeps = 0;
}

/* DETECT: the OS's tasks a little more of this frame while a cable comes up
 * (in the hurry, no node listed yet, no channel): its USB and NavNet work
 * runs only in gpSP's sleeps, its timers too (the IRQs are masked between
 * them), one tick a frame at most otherwise. Only ticks that come within
 * COMEUP_OS_MS (nspire_os_sleep_before); none where the OS can't sleep. */
static void comeup_os_time(void)
{
  uint32_t deadline;
  int n = 0;

  if (!hurry_now() || last_nodes || out_ch || svc_connected)
    return;
  deadline = nspire_ticks() + (uint32_t)COMEUP_OS_MS * 32768 / 1000;
  while (n < COMEUP_SLEEPS_MAX && nspire_os_sleep_before(deadline))
    n++;
  comeup_sleeps += (unsigned)n;
}

int link_cable_open(int on_its_own)
{
  unsigned irqs;

  error[0] = 0;
  if (start_service() < 0)
    return -1;
  drop_stale();
  {
    char line[160];
    nspire_os_report(line, sizeof(line));
    nspire_link_log("%s", line);
  }
  /* What watching holds stays: the other side's connection (its callback's
   * ring may have its hello already: the connection's own, as each one
   * starts it anew) and this side's channel to it. */
  irqs = irq_off();
  if (!svc_alive)
  {
    svc_in.head = svc_in.tail = 0;
    svc_connected = svc_broken = svc_overran = svc_broke_on_write = 0;
    svc_broke_rc = 0;
  }
  one_channel = svc_writes = 0;
  svc_out.head = svc_out.tail = 0;
  svc_loops = svc_write_count = svc_write_bytes = svc_queue_max = 0;
  svc_write_longest = 0;
  svc_write_rc = 0;
  irq_restore(irqs);
  svc_sp_lowest = 0;
  stack_sum_set = 0;
  if (out_ch && out_broken)
  {
    drop_channel(out_ch);
    out_ch = NULL;
  }
  out_broken = 0;
  out_stream_len = 0;
  stream_bad = 0;
  pend_len = 0;
  direct_writes = direct_held = direct_ticks = direct_longest = 0;
  first_channel_ms = 0;
  last_look_ms = now_ms() - LOOK_EVERY_MS + (link_cable_nonce() % 500);
  /* DETECT: the other side's connection is here, or the port just changed:
   * look at once (a look costs 2-54 ms: no reason to wait up to 0.5 s). */
  if (svc_alive || hurry_now())
    look_due = 1;
  ready = 0;
  logged_svc = logged_out = logged_broken = logged_dead = 0;
  came_spinning = came_blocked = blocks = sleeps = 0;
  last_look[0] = 0;
  automatic = on_its_own;
  accepting = 1;
  nspire_os_want(OS_WANT_LINK, 1);
  if (svc_alive)
    nspire_link_log("NavNet: the other side's connection from before is held");
  if (out_ch)
    nspire_link_log("NavNet: this side's channel from before is held");
  return 0;
}

/* Another calculator's link service, if one answers. The log gets what
 * happened (the calls' results: each node found and its Connect) when it
 * differs from the last look's, and how long it took. */

/* DETECT: the knock, right after this side's Connect (see the top): an
 * empty message (length word 0: every build's take_service/take_out drops
 * it) starts the other side's callback for this connection. Written by the
 * game's task, as one-channel mode writes its hellos (the service's task
 * isn't writing yet: not ready). A failed write lets the channel go; the
 * next look connects again. */
static void knock(void)
{
  static const uint8_t empty[2] = { 0, 0 };
  uint32_t t0 = nspire_ticks();
  int16_t rc = nn_write(out_ch, (void *)empty, sizeof(empty));
  nn_ch_t ch = out_ch;

  nspire_os_watchdog_off();   /* R3: the OS ran in the write */
  knocks++;
  nspire_link_log("NavNet: knock (an empty message) on this side's channel: Write %d in %u ms", rc,
                  (unsigned)((uint64_t)(nspire_ticks() - t0) * 1000 / 32768));
  if (rc < 0)
  {
    out_ch = NULL;
    drop_channel(ch);
  }
}

static void look_for_other(int may_connect)
{
  nn_oh_t oh;
  nn_nh_t node;
  char what[sizeof(last_look)];
  unsigned n = 0, start = now_ms(), nodes = 0;
  int16_t rc;
  int connected = 0;

  oh = TI_NN_CreateOperationHandle();
  if (!oh)
    n += snprintf(what + n, sizeof(what) - n, "no operation handle");
  else
  {
    rc = (int16_t)TI_NN_NodeEnumInit(oh);
    n += snprintf(what + n, sizeof(what) - n, "NodeEnumInit %d", rc);
    if (rc >= 0)
    {
      /* Not connecting, the first node is all a look needs to know; and
       * at most LOOK_NODES (a cable has one calculator on it). The CX II's
       * log (2026-11-08): a look that only listed nodes went on for 107 s,
       * the game held up, NodeEnumNext giving "node" after "node". */
      while (!out_ch && nodes < (may_connect ? LOOK_NODES : 1) &&
             (rc = (int16_t)TI_NN_NodeEnumNext(oh, &node)) >= 0)
      {
        nn_ch_t ch = NULL;
        int16_t crc;
        nodes++;
        if (!may_connect)
        {
          if (n < sizeof(what) - 32)
            n += snprintf(what + n, sizeof(what) - n, ", node %u", nodes);
          continue;
        }
        crc = (int16_t)TI_NN_Connect(node, LINK_SERVICE, &ch);
        if (n < sizeof(what) - 32)
          n += snprintf(what + n, sizeof(what) - n, ", node %u: Connect %d%s", nodes, crc,
                        crc >= 0 && !ch ? " (no channel)" : "");
        if (crc >= 0 && ch)
        {
          out_ch = ch;
          connected = 1;
        }
      }
      if (!nodes && n < sizeof(what) - 32)
        n += snprintf(what + n, sizeof(what) - n, ", no node (NodeEnumNext %d)", rc);
      TI_NN_NodeEnumDone(oh);
    }
    TI_NN_DestroyOperationHandle(oh);
  }
  nspire_os_watchdog_off();   /* R3: the OS ran in those calls (nspire_os.h) */
  last_nodes = nodes;
  looks++;
  look_cost_ms = now_ms() - start;
  if (out_ch || strcmp(what, last_look))
  {
    nspire_link_log("NavNet look (%u ms): %s", look_cost_ms, what);
    strcpy(last_look, what);
  }
  /* DETECT: when the OS listed the node, to within the looks' period: the
   * OS's share of a plug-in's time (r5: about 6 s after the port came up). */
  if (hurry_now())
  {
    hurry_looks++;
    if (!nodes)
      empty_look_ms = now_ms();
    else if (!node_said)
    {
      node_said = 1;
      nspire_link_log("NavNet: a node listed %u ms after the port change (look %u of the hurry, the last empty one %d ms "
                      "before; %u more OS ticks in the come-up)", now_ms() - hurry_from, hurry_looks,
                      empty_look_ms ? (int)(now_ms() - empty_look_ms) : -1, comeup_sleeps);
    }
  }
  if (connected)
    knock();
}

unsigned link_cable_last_nodes(void)
{
  return last_nodes;
}

unsigned link_cable_looks(void)
{
  return looks;
}

/* SEAM-2: the arbiter's look right before a bus power cycle. */
unsigned link_cable_look_now(void)
{
  if (!started)
    return 0;
  look_for_other(0);
  return last_nodes;
}

/* The link can't go on: in two-channel mode either channel gone (each
 * carries one way), in one-channel mode the one. */
static int cable_broken(void)
{
  if (one_channel)
    return (svc_broken || !svc_connected) && (out_broken || !out_ch);
  return svc_broken || !svc_connected || out_broken || !out_ch;
}

int link_cable_ready(void)
{
  unsigned now = now_ms();
  int slept = 0;

  if (!started || !accepting)
    return -1;
  if (svc_connected && !logged_svc)
  {
    logged_svc = 1;
    nspire_link_log("NavNet: the other side connected to this one's service");
    look_due = !out_ch;   /* DETECT: it's there: this side's channel to it now, not at the next look */
  }
  if (out_ch && !logged_out)
  {
    logged_out = 1;
    nspire_link_log("NavNet: connected to the other side's service");
  }
  if (ready)
  {
    if (pend_len && nspire_ticks() - pend_since >= FRAME_TICKS)
      link_cable_flush();
    return cable_broken() ? -1 : 1;
  }
  /* Both ways: keep looking until this side has its own channel too (at
   * once after the port changed or the other's connection came, often in
   * the hurry: DETECT). */
  if (!out_ch && (look_due || now - last_look_ms >= look_period(LOOK_EVERY_MS)))
  {
    look_due = 0;
    last_look_ms = now;
    look_for_other(1);
  }
  /* The other side's connection to this one's service gets its callback
   * only when the game lets the OS run (the calculators' first link): a
   * block each frame until it's there. */
  /* R1 (the real logs, 2026-11-09): picked Link, nothing connected: no
   * sleep at all here, only the game's pacing ones, which a game with no
   * spare time never gets (257 -> 268 sleeps in 50 s), so the host's OS
   * never got the ticks to enable the port and enumerate the other
   * calculator (NodeEnumInit -274 every look). The OS gets a tick each
   * frame until both channels are up, as the watch does. */
  if (!(out_ch && svc_connected) && nspire_os_since_sleep() >= FRAME_TICKS)
  {
    slept = nspire_os_sleep();
    comeup_os_time();
  }
  if (out_ch && !svc_connected && !slept && !nspire_os_sleep())
    block_a_moment();
  if (!svc_connected && !out_ch)
    return 0;
  if (!first_channel_ms)
    first_channel_ms = now ? now : 1;
  if (!(svc_connected && out_ch))
  {
    if (now - first_channel_ms < ONE_CHANNEL_MS)
      return 0;
    /* On its own, only both ways: the other side's gpSP took this side's
     * connection but doesn't link (its menu, its file list, an older
     * build), and one channel would have the game write and wait. */
    if (automatic)
    {
      nspire_link_log("NavNet: only %s after %u ms: not linking on its own",
                      out_ch ? "this side's channel" : "the other side's channel", ONE_CHANNEL_MS);
      strcpy(error, "The other calculator isn't linking.");
      return -1;
    }
    one_channel = 1;
    nspire_link_log("NavNet: only %s after %u ms: it carries both ways",
                    out_ch ? "this side's channel" : "the other side's channel", ONE_CHANNEL_MS);
  }
  /* (Both channels: ready now. The 300 ms LINK_SETTLE_MS that was here
   * gave a connection both made at once room to show up; with both up
   * there is nothing left to wait for: DETECT.) */
  ready = 1;
  /* Two channels with the OS's sleep: the service's task sends too. */
  svc_writes = !one_channel && nspire_os_ready();
  nspire_link_log("NavNet: ready (%s), %u ms after the first channel", one_channel ? "one channel" :
                  svc_writes ? "two channels: the service's task sends on this side's and reads the other's" :
                  "two channels: sends on its own, reads the service's", now - first_channel_ms);
  return 1;
}

unsigned link_cable_progress(void)
{
  return (last_nodes ? LINK_UP_NODE : 0) | (out_ch ? LINK_UP_OUT : 0) | (svc_connected ? LINK_UP_SVC : 0) |
         (ready ? LINK_UP_READY : 0);
}

static int write_out(const uint8_t *p, unsigned n)
{
  while (n)
  {
    unsigned k = n > PIECE ? PIECE : n;
    int16_t rc = nn_write(out_ch, (void *)p, k);
    nspire_os_watchdog_off();   /* R3: the OS ran in the write (nspire_os.h) */
    if (rc < 0)
    {
      nspire_link_log("NavNet: Write on its own channel %d", rc);
      out_broken = 1;
      return -1;
    }
    p += k;
    n -= k;
  }
  return 0;
}

/* R2 (the real logs, 2026-11-10): a NavNet write takes ~40 ms to come back
 * on the calculators (a round trip of 8-byte messages: min 84 ms), and a
 * game that writes on its own channel (this side's, in one-channel mode
 * here too) is held up that long by each message, one a frame: 8 frames a
 * second. A write costs about the same for 254 bytes as for 8, so a frame's
 * small message waits in pend and goes with the next ones in one write:
 * when the next doesn't fit a piece, when the game waits for the other side
 * (link_cable_receive), within the frames the other side has to spare
 * (link_cable_frame_done), or when asked (link_cable_flush: hellos, menu,
 * BYE). The receiver reads a byte stream: nothing changes on the wire. */

static int write_timed(const uint8_t *p, unsigned n)
{
  uint32_t t0 = nspire_ticks(), dt;
  int rc = write_out(p, n);

  dt = nspire_ticks() - t0;
  direct_writes++;
  direct_ticks += dt;
  if (dt > direct_longest)
    direct_longest = dt;
  return rc;
}

static int flush_pending(void)
{
  unsigned n = pend_len;

  if (!n)
    return 0;
  pend_len = 0;
  return write_timed(pend, n);
}

void link_cable_flush(void)
{
  link_fast_poll();
  if (pend_len && out_ch && !out_broken)
    flush_pending();
}

/* Not inlined into the core's update_gba (the cable's code is): its
 * stretch of hot code has to fit the 16 KB instruction cache. */
static int navnet_send(const void *msg, unsigned len)
{
  uint8_t head[2] = { (uint8_t)len, (uint8_t)(len >> 8) };

  if (!ready || len > LINK_MSG_MAX)   /* R6: was 0xFFFF; big[] below holds LINK_MSG_MAX */
    return -1;
  /* Queued for the service's task (svc_writes, or one-channel mode on the
   * service's channel); else on this side's channel. */
  if (svc_writes)
  {
    if (svc_broken || ring_free(&svc_out) < len + 2)
      return -1;
    ring_put(&svc_out, head, 2);
    ring_put(&svc_out, msg, len);
    return 0;
  }
  if (out_ch && !out_broken)
  {
    if (len + 2 > PIECE)
    {
      /* Longer than a piece: what's held first, then this on its own. */
      uint8_t big[LINK_MSG_MAX + 2];
      if (flush_pending() < 0)
        return -1;
      memcpy(big, head, 2);
      memcpy(big + 2, msg, len);
      return write_timed(big, len + 2) < 0 ? -1 : 0;
    }
    if (pend_len + len + 2 > PIECE && flush_pending() < 0)
      return -1;
    if (!pend_len)
    {
      pend_since = nspire_ticks();
      pend_frames = 0;
    }
    else
      direct_held++;
    memcpy(pend + pend_len, head, 2);
    memcpy(pend + pend_len + 2, msg, len);
    pend_len += len + 2;
    return 0;
  }
  if (!one_channel || svc_broken || ring_free(&svc_out) < len + 2)
    return -1;
  ring_put(&svc_out, head, 2);
  ring_put(&svc_out, msg, len);
  return 0;
}

/* A whole message from the stream in svc_in, or 0. With IRQs off: a new
 * connection's callback empties the ring (a message's 2 KB copy is a few
 * microseconds). */
static int take_service(void *buf, unsigned size)
{
  uint8_t head[2];
  unsigned n, irqs;
  int got = 0;

  if (ring_used(&svc_in) < 2)
    return 0;
  irqs = irq_off();
  if (ring_used(&svc_in) >= 2)
  {
    head[0] = svc_in.buf[svc_in.head];
    head[1] = svc_in.buf[(svc_in.head + 1) % RING];
    n = head[0] | (head[1] << 8);
    if (n > LINK_MSG_MAX)
      stream_bad = 1;
    else if (ring_used(&svc_in) >= n + 2)
    {
      ring_get(&svc_in, head, 2);
      if (n > size)
      {
        /* Too big for the caller: dropped. */
        while (n--)
          ring_get(&svc_in, head, 1);
      }
      else
      {
        ring_get(&svc_in, buf, n);
        got = (int)n;
      }
    }
  }
  irq_restore(irqs);
  return got;
}

/* A whole message from what came on this side's channel, or 0. */
static int take_out(void *buf, unsigned size)
{
  unsigned n;

  if (out_stream_len < 2)
    return 0;
  n = out_stream[0] | (out_stream[1] << 8);
  if (n > LINK_MSG_MAX)
  {
    stream_bad = 1;
    return 0;
  }
  if (out_stream_len < n + 2)
    return 0;
  if (n <= size)
    memcpy(buf, out_stream + 2, n);
  memmove(out_stream, out_stream + 2 + n, out_stream_len - 2 - n);
  out_stream_len -= 2 + n;
  return n <= size ? (int)n : 0;
}

/* Whether block_a_moment can block: this side's channel and the channel's
 * read (not only the public entry's 62 s). */
static int can_block(void)
{
  return out_ch && !out_broken && nn_read_short;
}

/* A moment in a read on this side's channel (BLOCK_MS with the channel's
 * read): the OS runs its other tasks meanwhile. In one-channel mode with
 * only this side's channel, what comes on it is the other side's. 1 if it
 * blocked; 0 if it couldn't (can_block), and the game spins. */
static int block_a_moment(void)
{
  uint32_t got = 0;
  int16_t rc;

  if (!can_block())
    return 0;
  if (out_stream_len + PIECE > sizeof(out_stream))
    return 0;   /* take_out empties it first */
  blocks++;
  rc = nn_read_short(out_ch, BLOCK_MS, out_stream + out_stream_len, PIECE, (uintptr_t)&got);
  nspire_os_watchdog_off();   /* R3: the OS ran in the read (nspire_os.h) */
  if (rc < 0)
  {
    nspire_link_log("NavNet: Read on its own channel %d", rc);
    out_broken = 1;
    return 0;
  }
  out_stream_len += got;
  return 1;
}

/* Whether pause_a_moment can pause. */
static int can_pause(void)
{
  return nspire_os_ready() || (can_block() && !svc_writes);
}

/* What the service's task's stack looks like while it waits (it ran in the
 * sleep just past and stopped in its read or write, its stack pointer
 * saved in its task): the lowest seen. Only read; only where the task's
 * words look as on a CX II's OS 6.2 (ND Screen Mirror's), the stack
 * pointer between the two bounds. */
static void look_at_service_stack(void)
{
  uint32_t w[3], low, high;

  if (!svc_task || !nspire_os_task_stack(svc_task, w))
    return;
  memcpy(svc_stack, w, sizeof(svc_stack));
  low = w[0] < w[1] ? w[0] : w[1];
  high = w[0] < w[1] ? w[1] : w[0];
  if (high - low <= 0x100000 && w[2] >= low && w[2] <= high && (!svc_sp_lowest || w[2] < svc_sp_lowest))
    svc_sp_lowest = w[2];
}

/* The OS's tasks get the CPU a moment: an OS tick's sleep, or without it a
 * block in a read on this side's channel (block_a_moment). 1 if it did. */
static int pause_a_moment(void)
{
  /* One channel, this side's: what comes, comes on it, read here. */
  if (one_channel && can_block() && block_a_moment())
    return 1;
  if (nspire_os_sleep())
  {
    sleeps++;
    look_at_service_stack();
    return 1;
  }
  return !svc_writes && block_a_moment();
}

static int navnet_receive(void *buf, unsigned size, unsigned wait_ms)
{
  unsigned start = now_ms(), looks = 0;
  uint32_t first = nspire_ticks(), spin_from = first;
  int n, blocked = 0, dead = 0;

  if (!ready)
  {
    strcpy(error, "The link cable came out.");   /* what nspire_link.c shows (R6) */
    return -1;
  }
  for (;;)
  {
    if ((n = take_service(buf, size)) > 0 || (n = take_out(buf, size)) > 0)
    {
      if (link_fast_is_control(buf, (unsigned)n))
      {
        /* The fast transport's (nspire_link_fast.h): never the link's. */
        link_fast_control(buf, (unsigned)n);
        continue;
      }
      if (blocked)
        came_blocked++;
      else
        came_spinning++;
      return n;
    }
    if (stream_bad)
    {
      nspire_link_log("NavNet: a message's length word was over %d: the stream is damaged", LINK_MSG_MAX);
      strcpy(error, "The link's data came damaged: unlinked.");
      return -1;
    }
    if (svc_broken && !logged_broken)
    {
      logged_broken = 1;
      nspire_link_log("NavNet: the service's channel broke (%s %d)", svc_overran ? "overran" :
                      svc_broke_on_write ? "Write" : "Read", svc_broke_rc);
    }
    if (cable_broken())
    {
      strcpy(error, "The link cable came out.");
      return -1;
    }
    if (pend_len && wait_ms)
      link_cable_flush();   /* waiting for the other side: what's held goes first */
    if (!wait_ms || now_ms() - start >= wait_ms || (dead && blocked))
      return 0;
    /* A timer that doesn't run (nspire.c's fallback: time moves only as
     * frames are paced): after DEAD_LOOKS a block, as long as the OS
     * makes it, and the wait's over after it (the link's give-up counts
     * real seconds, nspire_link.c); or without one, each look counts as an
     * eighth of a ms. */
    if (nspire_ticks() == first)
    {
      ++looks;
      if (looks >= DEAD_LOOKS && can_pause())
      {
        dead = 1;
        if (!logged_dead)
        {
          logged_dead = 1;
          nspire_link_log("NavNet: the timer doesn't move: a wait pauses, the OS timing it");
        }
        blocked = pause_a_moment() || blocked;
        if (!blocked)
          return 0;
      }
      else if (!can_pause() && looks >= wait_ms * 8)
        return 0;
      continue;
    }
    /* Spin a while (where the interrupts are on, a task the USB wakes may
     * take the CPU at once), then block a moment, then spin again. With
     * them masked, as gpSP runs (nspire_os.h), nothing comes while it
     * spins: the OS's tasks run only in a pause, so it pauses at once. */
    if (irqs_masked() || nspire_ticks() - spin_from >= SPIN_TICKS)
    {
      pause_a_moment();
      blocked = 1;
      spin_from = nspire_ticks();
    }
  }
}

/* ---- The fast transport over this one (nspire_link_fast.c) ---- */

/* A NavNet message for the fast transport: written now, not held. */
static int navnet_send_now(const void *msg, unsigned len)
{
  int rc = navnet_send(msg, len);
  if (rc >= 0)
    link_cable_flush();
  return rc;
}

/* This side's NavNet writes all out (none queued or in a write). */
static int navnet_writes_out(void)
{
  return !pend_len && !ring_used(&svc_out) && !svc_in_write;
}

static const struct link_fast_nav fast_nav = { navnet_send_now, navnet_receive, navnet_writes_out };

uint32_t link_cable_fast_offer(void)
{
  return link_fast_offer();
}

int link_cable_fell_back(void)
{
  return link_fast_fell_back();
}

int link_cable_go_fast(uint32_t other_offer, const char *refused)
{
  if (!ready)
    return 0;
  link_cable_flush();
  return link_fast_start(other_offer, &fast_nav, refused);
}

int link_cable_fast_up(void)
{
  return link_fast_up();
}

/* LINK2: NavNet's messages move only while the OS runs; the fast
 * transport's never touch it (and its RESUME after a fall back goes on
 * NavNet: link_fast_up() is 0 from the fall back on). */
int link_cable_linked_os(void)
{
  return !link_fast_up();
}

const char *link_cable_fast_damage(void)
{
  return link_fast_damage();
}

int link_cable_fast_holds(void)
{
  return link_fast_holds();
}

void link_cable_fast_drop(const char *why)
{
  link_fast_drop(why);
}

/* Not inlined into the core's update_gba (the cable's code is): its
 * stretch of hot code has to fit the 16 KB instruction cache. */
__attribute__((noinline)) int link_cable_send(const void *msg, unsigned len)
{
  if (link_fast_owns())
    return ready ? link_fast_send(msg, len) : -1;
  return navnet_send(msg, len);
}

__attribute__((noinline)) int link_cable_receive(void *buf, unsigned size, unsigned wait_ms)
{
  if (!ready)
    return -1;
  if (link_fast_owns())
  {
    int n = link_fast_receive(buf, size, wait_ms);
    if (n != LINK_FAST_NAVNET)
      return n;
  }
  return navnet_receive(buf, size, wait_ms);
}

uint32_t nspire_rtc_seconds(void);   /* nspire.c: the real-time clock, which runs without the timer */

/* A wait of up to some ms, by the timer; while the timer doesn't move
 * (nspire.c's fallback time moves only as frames are paced), a pause counts
 * as BLOCK_MS (the OS's, at least that), and a look with no pause by the
 * real-time clock: the wait's over once more whole seconds than ms have
 * gone by (one more for the second it began in). Counting those looks as
 * eighths of a ms ended the waits for the service's task at once, before
 * it left its read (tests/link_usb, a dead timer under load: StopService
 * with a callback in it, a hang on the calculator). A real-time clock that
 * doesn't move either (only the PC's --rtc-frozen): at most WAIT_LOOKS_MAX
 * looks a ms, far more than a second's looks take. */
#define WAIT_LOOKS_MAX 65536
struct wait
{
  unsigned start, eighths, looks;
  uint32_t first, rtc;
};

static void wait_begin(struct wait *w)
{
  w->start = now_ms();
  w->first = nspire_ticks();
  w->rtc = nspire_rtc_seconds();
  w->eighths = w->looks = 0;
}

static int wait_over(struct wait *w, unsigned ms, int paused)
{
  if (now_ms() - w->start >= ms)
    return 1;
  if (nspire_ticks() != w->first)
    return 0;
  if (paused)
  {
    w->eighths += 8 * BLOCK_MS;
    return w->eighths >= ms * 8;
  }
  {
    uint32_t seconds = nspire_rtc_seconds() - w->rtc;
    if (seconds)
      return seconds > (ms + 999) / 1000;
  }
  return ++w->looks >= ms * WAIT_LOOKS_MAX;
}

/* The service's task leaving: up to ms, pausing (an OS sleep or a block:
 * its task runs only then) or, without either, spinning. */
static void wait_for_service(unsigned ms)
{
  struct wait w;
  int paused = 0;

  wait_begin(&w);
  while (svc_tasks && !wait_over(&w, ms, paused))
    paused = pause_a_moment();
}

/* A channel this side's done with: Disconnected, unless the service's
 * task is writing on it (the other side hasn't taken the write: a pulled
 * cable, a stalled calculator), whose Disconnect then waits for the
 * write's end (drop_stale). Once its gen is old the task starts no new
 * write, so a channel it isn't on now is safe to let go. */
static void drop_channel(nn_ch_t ch)
{
  int16_t rc;

  if (!ch)
    return;
  if (svc_in_write == ch)
  {
    drop_stale();   /* an older one first, if its write's over */
    if (!stale_ch)
    {
      stale_ch = ch;
      nspire_link_log("NavNet: the service's task is still writing on this side's channel: "
                      "it's let go once the write ends");
      return;
    }
    /* Two writes stuck, which only a newer callback's could be: wait for the
     * older to end, as long as its write can last. */
    nspire_link_log("NavNet: two channels with a write on them: waiting for one");
    wait_for_write(stale_ch, WRITE_END_MS);
    drop_stale();
    stale_ch = ch;
    return;
  }
  rc = (int16_t)TI_NN_Disconnect(ch);
  nspire_os_watchdog_off();   /* EXIT-R5: the OS ran in it: its keypad line and busy cursor off again */
  if (rc < 0)
    nspire_link_log("NavNet: Disconnect %d", rc);
}

/* A channel kept for its write's end (drop_channel), Disconnected if the
 * write's over. */
static void drop_stale(void)
{
  int16_t rc;

  if (!stale_ch || svc_in_write == stale_ch)
    return;
  rc = (int16_t)TI_NN_Disconnect(stale_ch);
  nspire_os_watchdog_off();   /* EXIT-R5 */
  nspire_link_log("NavNet: the channel kept for its write let go (Disconnect %d)", rc);
  stale_ch = NULL;
}

/* Up to ms for the service's task's write on ch to end. */
static void wait_for_write(nn_ch_t ch, unsigned ms)
{
  struct wait w;
  int paused = 0;

  wait_begin(&w);
  while (ch && svc_in_write == ch && !wait_over(&w, ms, paused))
    paused = pause_a_moment();
}

/* Done with the other side, linked or watching: the callback holding its
 * connection leaves (a new gen; it checks it before each put, get and
 * write), the shared state's emptied (what came, what's queued, how it
 * broke), and this side's channel goes (drop_channel). */
static void let_go(void)
{
  unsigned irqs = irq_off();

  accepting = 0;
  svc_writes = 0;
  one_channel = 0;
  svc_gen++;
  svc_alive = 0;   /* its connection's done: a new one is the other's next link */
  svc_connected = svc_broken = svc_overran = svc_broke_on_write = 0;
  svc_broke_rc = 0;
  svc_in.head = svc_in.tail = 0;
  svc_out.head = svc_out.tail = 0;
  irq_restore(irqs);
  /* With short reads it leaves within SERVICE_READ_MS (blocking on this
   * side's channel, still open, gives its task the CPU); with long ones
   * once the other side closes its channel to this one (at its next frame,
   * on this side's BYE); from a write once the other side takes it. */
  if (nn_read_short)
    wait_for_service(SERVICE_READ_MS * 4);
  drop_channel(out_ch);
  out_ch = NULL;
  out_broken = 0;
  out_stream_len = 0;
  stream_bad = 0;
  ready = 0;
}

/* What's queued for the service's task (svc_writes, or one-channel mode on
 * its channel) goes, the OS let run until it has: up to WAIT_SEND_MS. */
static void send_queued(void)
{
  if (svc_alive && (svc_writes || one_channel))
  {
    struct wait w;
    int paused = 0;
    wait_begin(&w);
    while (ring_used(&svc_out) && !svc_broken && svc_alive && !wait_over(&w, WAIT_SEND_MS, paused))
      paused = pause_a_moment();
  }
}

/* LNK-2: a message no wait follows (MSG_MENU: gpSP's menu never sleeps
 * into the OS, so the service's task would send it only after the menu). */
void link_cable_push(void)
{
  if (!ready)
    return;
  link_cable_flush();
  send_queued();
}

void link_cable_close(void)
{
  link_fast_stop("the link closed");
  if (!started || !accepting)
    return;
  link_cable_flush();   /* a held BYE goes */
  {
    char line[300];
    link_cable_stats(line, sizeof(line));
    nspire_link_log("NavNet: closing; %s", line);
  }
  /* What's queued (the BYE) goes first. */
  send_queued();
  let_go();
  if (svc_tasks)
    nspire_link_log("NavNet: the service's task is still in its read or write (it leaves when it ends)");
}

void link_cable_unwatch(void)
{
  if (!started || accepting)
    return;
  drop_stale();
  if (!svc_alive && !out_ch)
    return;
  nspire_link_log("NavNet: watching stopped: lets go of %s%s%s", svc_alive ? "the other side's connection" : "",
                  svc_alive && out_ch ? " and " : "", out_ch ? "this side's channel" : "");
  let_go();
}

int link_cable_watch(int may_connect, int plugged, int fresh)
{
  static unsigned look_at;
  static int looked;
  unsigned now;

  if (start_service() < 0)
    return -1;
  drop_stale();
  nspire_os_want(OS_WANT_LINK, 1);
  /* The OS's tasks get the CPU at least once a frame while something's on
   * the port (a game with no spare time gets none in nspire_wait_until):
   * what sets up a cable plugged in now, and takes the other side's
   * connection. When the port can't tell, at least every WATCH_SLEEP_MS.
   * (Nothing on it: nspire_link.c doesn't watch, and the OS gets nothing.)
   * P2: a frame's tick is forced only while it may be setting something up
   * (the port changed less than 10 s ago: 'fresh'; DETECT's hurry; the other
   * side's connection, this side's channel, a node seen); else the tick when
   * it's near (1 ms away on a CX II, up to half a tick as WATCH_FORCE_MS
   * nears: nspire_os_near_wait), forced after WATCH_FORCE_MS: a sleep cost
   * 1-4 ms of a fast forward's frames, 50 a second.
   * DETECT: after a tick taken with something on the port, the come-up's
   * extra OS time (comeup_os_time: only in the hurry, before a node). */
  if (plugged > 0)
  {
    uint32_t since = nspire_os_since_sleep();
    uint32_t force = fresh || hurry_now() || svc_alive || out_ch || last_nodes ?
                     FRAME_TICKS : (uint32_t)WATCH_FORCE_MS * 32768 / 1000;
    int slept = 0;

    if (since >= force)
      slept = nspire_os_sleep();
    else if (since >= FRAME_TICKS)
      slept = nspire_os_sleep_near(nspire_os_near_wait(since, FRAME_TICKS, force));
    if (slept)
      comeup_os_time();
  }
  else if (nspire_os_since_sleep() > (uint32_t)WATCH_SLEEP_MS * 32768 / 1000)
    nspire_os_sleep();
  if (svc_alive && !svc_broken)
    return 2;
  now = now_ms();
  if (!looked || look_due || now - look_at >= look_period(WATCH_LOOK_MS))
  {
    looked = 1;
    look_due = 0;
    look_at = now;
    look_for_other(may_connect);
  }
  if (out_ch)
    return 2;
  return last_nodes ? 1 : 0;
}

/* The CX CAS's log (2026-11-08, before gpSP's sleeps worked) has its
 * controller come on again with a cable plugged back in and no OS sleep
 * at all, so in a look or what a look let run: a look now and then (not
 * connecting: it stops at the first node), and the tick that lets the OS
 * do it on its own. Once it's on, the port reads plugged and
 * nspire_link.c watches as usual. */
int link_cable_gated(void)
{
  static unsigned look_at;
  static int looked;
  unsigned now;

  if (start_service() < 0)
    return -1;
  drop_stale();
  if (nspire_os_since_sleep() > (uint32_t)WATCH_SLEEP_MS * 32768 / 1000)
    nspire_os_sleep();
  now = now_ms();
  if (!looked || now - look_at >= GATED_LOOK_MS)
  {
    looked = 1;
    look_at = now;
    look_for_other(0);
  }
  return 0;
}

void link_cable_frame_done(unsigned frames)
{
  /* R3 (the R2 logs): what's held goes after the frames the other side has
   * room for, counted in frames. R2 counted them in 60ths of a second
   * (frames * FRAME_TICKS): at the CX CAS's 18.7 frames a second that held
   * 117 ms, 2.2 of its frames, so it wrote every 3.5 frames (227 writes in
   * 788 frames), each 69 ms in its game's time (15.6 s of 42 s: 37%), and
   * never waited for the CX II ("after a pause 18"): its writes were what
   * held the link to 19 frames a second. Counted in frames, a write goes
   * every `frames` frames (7 at a delay of 15: half the other side's room,
   * as before at 60 a second), a write's cost spread over twice as many. */
  link_fast_poll();
  if (pend_len)
    pend_frames++;
  if (pend_len && frames && pend_frames >= frames)
    link_cable_flush();
  /* Without a sleep since, what's queued (this frame's end) waits for the
   * next one: a game with no spare time would send only when it waits for
   * the other side, its frames in bursts. A tick's sleep (5 ms on
   * average) once in the frames the other side has room for. */
  if (!svc_writes || !ring_used(&svc_out) || !frames)
    return;
  if (nspire_os_since_sleep() >= frames * FRAME_TICKS && nspire_os_sleep())
  {
    sleeps++;
    look_at_service_stack();
  }
}

void link_cable_stats(char *text, unsigned size)
{
  unsigned n;
  uint32_t task = svc_task;

  n = (unsigned)snprintf(text, size, "%s; came spinning %u, after a pause %u (%u blocks, %u OS sleeps); the service's "
                         "task: %u turns, %u writes (%u bytes, the longest %u us, the last %d), %u queued at most, "
                         "%u to read", svc_writes ? "the service's task sends" : one_channel ? "one channel" :
                         "this side sends", came_spinning, came_blocked, blocks, sleeps, svc_loops, svc_write_count,
                         svc_write_bytes, (unsigned)nspire_raw_timer_us(svc_write_longest), svc_write_rc,
                         svc_queue_max, ring_used(&svc_in));
  if (n < size)
    n += (unsigned)snprintf(text + n, size - n, "; own-channel writes %u (%u held messages went with another, %u us in all, the longest %u us), held now %u B, "
                            "out_stream %u B, queued %u B",
                            direct_writes, direct_held, (unsigned)((uint64_t)direct_ticks * 1000000 / 32768),
                            (unsigned)((uint64_t)direct_longest * 1000000 / 32768), pend_len, out_stream_len,
                            ring_used(&svc_out));
  if (n < size)
  {
    char fast[260];
    link_fast_stats(fast, sizeof(fast));
    if (fast[0])
      n += (unsigned)snprintf(text + n, size - n, "; %s", fast);
  }
  if (task && n < size)
  {
    uint32_t low = svc_stack[0] < svc_stack[1] ? svc_stack[0] : svc_stack[1];
    n += (unsigned)snprintf(text + n, size - n, "; its task %08x, stack words %08x %08x %08x", (unsigned)task,
                            (unsigned)svc_stack[0], (unsigned)svc_stack[1], (unsigned)svc_stack[2]);
    if (svc_sp_lowest && n < size)
      snprintf(text + n, size - n, ", lowest saved stack pointer %08x (%u bytes over its bound)",
               (unsigned)svc_sp_lowest, (unsigned)(svc_sp_lowest - low));
  }
}

/* R3: the service's task's stack's lowest KB, summed (only read; only
 * where its words were seen sane: look_at_service_stack). Its task is
 * stopped while the game's runs (gpSP's IRQs are masked). 0 if unknown. */
static uint32_t stack_bottom_sum(void)
{
  const volatile uint32_t *w;
  uint32_t low, sum = 0;
  unsigned i;

  if (!svc_sp_lowest)
    return 0;
  low = svc_stack[0] < svc_stack[1] ? svc_stack[0] : svc_stack[1];
  w = (const volatile uint32_t *)(uintptr_t)((low + 3) & ~3u);
  for (i = 0; i < STACK_SUM_WORDS; i++)
    sum = (sum << 1 | sum >> 31) ^ w[i];
  return sum | 1;
}

void link_cable_second(char *text, unsigned size)
{
  static const char *const at[] = { "none", "running", "in a write", "in a read" };
  volatile const uint32_t *timer = nspire_raw_timer;
  int now_at = svc_at;
  uint32_t since = svc_at_since, longest = svc_sec_longest, read_longest = svc_sec_read_longest, sum = stack_bottom_sum(), low;
  unsigned writes = svc_sec_writes, turns = svc_sec_turns;
  const char *stack = "?";

  svc_sec_writes = svc_sec_turns = 0;
  svc_sec_longest = svc_sec_read_longest = 0;
  if (sum && !stack_sum_set)
  {
    stack_sum = sum;
    stack_sum_set = 1;
  }
  if (sum)
    stack = sum == stack_sum ? "same" : "CHANGED";
  low = svc_stack[0] < svc_stack[1] ? svc_stack[0] : svc_stack[1];
  snprintf(text, size, "svc %u w (longest %u ms), %u turns (read longest %u ms), %s", writes,
           (unsigned)nspire_raw_timer_us(longest) / 1000, turns, (unsigned)nspire_raw_timer_us(read_longest) / 1000,
           at[now_at & 3]);
  if (now_at >= SVC_WRITE && timer)
  {
    unsigned n = (unsigned)strlen(text);
    if (n < size)
      snprintf(text + n, size - n, " for %u ms", (unsigned)nspire_raw_timer_us(since - *timer) / 1000);
  }
  {
    unsigned n = (unsigned)strlen(text);
    if (n < size)
      snprintf(text + n, size - n, "; stack bottom %s, margin %u B; held %u B %u fr", stack,
               svc_sp_lowest ? (unsigned)(svc_sp_lowest - low) : 0, pend_len, pend_frames);
  }
  {
    unsigned n = (unsigned)strlen(text);
    if (n < size)
      link_fast_second(text + n, size - n);
  }
}

/* R11w (2026-11-15): the exit's USB budget (0: none, the waits as they
 * were): every wait of the shutdown ends by exit_end (now_ms) */
static unsigned exit_cap, exit_end;

static unsigned capped(unsigned ms)
{
  unsigned left;

  if (!exit_cap)
    return ms;
  left = (int)(exit_end - now_ms()) > 0 ? exit_end - now_ms() : 1;
  return left < ms ? left : ms;
}

int link_cable_shutdown_within(unsigned ms)
{
  int short_of = 0;

  exit_cap = 1;
  exit_end = now_ms() + (ms ? ms : 1);
  if (started)
  {
    link_cable_shutdown();
    short_of = started;   /* still started: it stopped before StopService */
  }
  exit_cap = 0;
  return short_of ? -1 : 0;
}

void link_cable_shutdown(void)
{
  unsigned start = now_ms();
  int16_t rc;

  link_cable_close();
  if (!started)
    return;
  quitting = 1;
  /* A connection held while watching goes too, and this side's channel. */
  let_go();
  /* StopService waits, without a deadline, for a callback still in a read
   * or write: give it the time to leave first; a write the other side
   * doesn't take, as long as it can last (R11w: at gpSP's exit, within its
   * USB budget: past it StopService is not called, the exit restarts the
   * calculator instead of handing it back with a task inside gpSP). */
  wait_for_service(capped(LEAVE_MS));
  if (svc_tasks && svc_in_write)
  {
    nspire_link_log("NavNet: closing gpSP: the service's task is in a write, waited for (up to %u s)",
                    capped(WRITE_END_MS) / 1000);
    wait_for_write(svc_in_write, capped(WRITE_END_MS));
    wait_for_service(capped(LEAVE_MS));
  }
  if (exit_cap && svc_tasks)
  {
    nspire_link_log("NavNet: closing gpSP: a callback is still in a read or write after %u ms: StopService not "
                    "called (it would wait for it without a deadline)", now_ms() - start);
    return;   /* started stays 1: link_cable_shutdown_within says -1 */
  }
  drop_stale();
  /* The OS's dispatch wrapper lets go of a callback's references only once
   * it's back from it: two more ticks (as ND Screen Mirror does). */
  if (!svc_tasks)
  {
    nspire_os_sleep();
    nspire_os_sleep();
  }
  rc = (int16_t)TI_NN_StopService(LINK_SERVICE);
  /* EXIT-R5/R6: the OS ran in it (9-23 ms in every log, and with no
   * callback left nothing sleeps after it): its keypad line off again
   * before the pad driver's shutdown takes interrupts, its cursor off. */
  nspire_os_watchdog_off();
  nspire_link_log("NavNet: StopService %d after %u ms%s", rc, now_ms() - start,
                  svc_tasks ? " (a callback was still in a read or write)" : "");
  wait_for_service(capped(LEAVE_MS));
  drop_stale();
  if (stale_ch)
    nspire_link_log("NavNet: this side's channel still has a write on it after %u s: left as it is",
                    WRITE_END_MS / 1000);
  stale_ch = NULL;
  started = 0;
  svc_alive = 0;
  nspire_os_want(OS_WANT_LINK, 0);
}

/* A new number each call (a hello that tied rolls again): the timer's
 * ticks, which two calculators don't share, mixed into the last one. */
uint32_t link_cable_nonce(void)
{
  static uint32_t n;
  n = (n * 0x9E3779B9u) ^ (nspire_ticks() * 2654435761u) ^ 0x6A09E667u;
  return n | 1;
}

const char *link_cable_error(void)
{
  return error[0] ? error : NULL;
}
