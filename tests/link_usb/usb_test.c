/* nspire_link_usb.c on the PC: two calculators, a and b (the file built
 * twice, include/side.h), linked through a fake NavNet, whose OS tasks
 * are threads. What it fakes is what the transport assumes of NavNet and
 * ND Screen Mirror saw on a CX II: a service's callback runs in a task of
 * its own, one per connection; a channel carries packets in order both
 * ways; a read waits up to its time and ends with nothing as a success;
 * the public read entry makes 1 to 61,999 ms 62 s (here CLAMP_MS); a
 * closed channel's reads fail once it's empty; a callback's return closes
 * its end; StopService closes the service's channels and waits for its
 * callbacks; and (DETECT, from every calculator log r1-r5) a connection's
 * callback starts only when the connecting side first writes on it, not at
 * the Connect (OLD_FAKE: the fake before, a callback at the Connect). What
 * the calculators really do is in the link's log on them.
 *
 *     usb_test             every case, a few seconds
 *
 * The cases: both calculators find each other (two channels) with the
 * channel's short read, and with the public entry's long ones; only one
 * finds the other (one channel); each in both orders, linking twice, then
 * closing gpSP. Each side sends messages of 1 to 600 bytes (more than a
 * piece: 254) both one at a time and in bursts, and checks the other's
 * come whole and in order.
 *
 * And as the first link on two calculators went (2026-11-06): a service's
 * callback that starts only when that calculator's game blocks in a read
 * or asks NavNet for the nodes (the OS runs its other tasks only then; the
 * callbacks didn't start in 4 s of game frames); with that, a calculator
 * whose timer doesn't run (the CX CAS's): its ticks move only as frames
 * are paced, never while it waits. With the OS's sleep (nspire_os.h, a 10
 * ms tick that also starts the callbacks waiting) and writes that wait for
 * the other side (WRITE_US, as ND Screen Mirror saw): the service's task
 * sends, so no send holds the game up; and the cable watched for, the
 * other calculator's gpSP starting a while after this one's (both link on
 * their own, as nspire_link.c's watch_cable does).
 *
 * With the OS's sleep the calculator's CPU is modeled too (cpu_model): a
 * side's callbacks run only while its game sleeps in the OS or waits in a
 * NavNet call (each side's CPU a lock its game holds otherwise), as the
 * OS runs its lower-priority tasks only then; and a write waits until the
 * other side has read it, past a window of a frame's pieces (both sides'
 * bursts then catch the service's tasks writing at once: neither may
 * write all it has before reading). Then also: the cable pulled mid-link and put
 * back (the next link must start clean), a side closing with its
 * service's task stuck in a write the other doesn't take (its channel
 * must not be Disconnected under it), a side closing in one-channel mode
 * with messages queued (the next connection mustn't get them), a gpSP
 * closing while it holds the other's connection from watching
 * (StopService only once its callback left), linking on its own with a
 * side that never connects back (given up on, never one channel), and
 * the watch's OS sleeps in a game with no spare time (each frame with
 * something on the port, every 250 ms when it can't tell, and with a CX's
 * controller off, link_cable_gated, every 250 ms and a look in 6 s). */

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../../nspire_usbpad_glue.h"   /* R1: the pad's tick rule, modelled below */
#include "usb_fast_sim.h"

typedef struct fake_end *nn_ch_t;
typedef struct fake_node *nn_nh_t;
typedef struct fake_op *nn_oh_t;
typedef int16_t (*read_fn)(nn_ch_t ch, uint32_t ms, void *buf, uint32_t size, uintptr_t got);

#define CLAMP_MS 1500   /* the public entry's 62 s, shorter here */

/* ---- The fake NavNet ---- */

#define PACKETS 4096
struct packet { unsigned len; uint8_t data[254]; };
struct queue { struct packet p[PACKETS]; unsigned head, tail; };
struct channel
{
  struct queue to[2];   /* to[e]: what end e reads */
  int open[2];
  int server;           /* the side whose service end 1 is */
  int writing[2];       /* writes waiting on end e */
  struct call *call;    /* DETECT: the service's callback, not started until data comes */
};
struct fake_end { struct channel *c; int e; };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;

static struct
{
  int registered;
  void (*cb)(nn_ch_t, void *);
  int callbacks;
  int sees_other;              /* its NodeEnum finds the other calculator */
  int stop_with_callbacks;     /* StopService with a callback still in it */
  int late_callbacks;          /* a callback starts when its game blocks */
  struct call *pending[16];    /* callbacks waiting for that */
  int pending_count;
  int timer_dead;              /* ticks move only with frames */
  uint32_t frames;
  int os_sleep;                /* nspire_os_sleep works */
  int cpu_model;               /* callbacks run only while the game lets go */
  long last_wake;               /* us_now() at the last sleep's return */
  int os_tick_us;              /* P2: a sleep waits for the OS's next tick, this far apart (0: 10 ms real, or none with fast_sleeps) */
  long waited_us;              /* P2: what the sleeps waited, in all */
  int disconnect_while_writing;   /* a Disconnect with a write on the channel */
  int sleeps;                  /* nspire_os_sleep's so far */
  int os_ran_unchecked;         /* EXIT-R5: a Disconnect or StopService (the OS ran in it) with no nspire_os_watchdog_off after it yet */
  int unchecked_at_close;      /* closes and shutdowns that returned with that */
  int need_sleeps;             /* its OS enumerates (NodeEnum finds the other) only after this many sleeps: R1, the host's OS gets ticks or it never enables the port */
} calc[2];
/* Each calculator's CPU (cpu_model): its game holds it but in its sleeps
 * and NavNet waits; a callback holds it but in its reads and writes. */
static pthread_mutex_t cpu[2] = { PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER };
static __thread int my_cpu = -1;

static void cpu_take(int side)
{
  if (side < 0 || !calc[side].cpu_model)
    return;
  pthread_mutex_lock(&cpu[side]);
  my_cpu = side;
}

/* Lets go of this thread's CPU (if it holds one): its side, or -1. */
static int cpu_give(void)
{
  int side = my_cpu;
  if (side >= 0)
  {
    my_cpu = -1;
    pthread_mutex_unlock(&cpu[side]);
  }
  return side;
}
static int write_us = 50;      /* a write waits this long for the other side to take it */
static int connect_now;        /* the old fake: a callback starts at the Connect (not the calculators') */
/* The calling thread's calculator, if it's a game's (-1: a callback's). */
static __thread int game_side = -1;
static int short_reads = 1;

static struct channel *channels[64];
static struct fake_end *channel_ends[64];   /* each channel's two ends (freed with it) */
static int channel_count;

/* S8: a case may run its frames on a virtual clock (skew_us moves it, one
 * step a frame) and its OS sleeps take no real time (fast_sleeps). */
static long skew_us;
static int fast_sleeps;

static long us_now(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000000L + t.tv_nsec / 1000 + skew_us;
}

static long ms_now(void)
{
  return us_now() / 1000;
}

static uint32_t real_ticks(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint32_t)(t.tv_sec * 32768ULL + t.tv_nsec * 32768ULL / 1000000000ULL + (uint64_t)skew_us * 32768 / 1000000);
}

/* Without a timer, nspire.c's time: a frame's (1/60 s) for each frame. */
/* The real-time clock runs whatever the timer does (the CX CAS's did). */
uint32_t a_nspire_rtc_seconds(void) { return (uint32_t)(ms_now() / 1000); }
uint32_t b_nspire_rtc_seconds(void) { return (uint32_t)(ms_now() / 1000); }
uint32_t a_nspire_ticks(void) { return calc[0].timer_dead ? calc[0].frames * 546 : real_ticks(); }
uint32_t b_nspire_ticks(void) { return calc[1].timer_dead ? calc[1].frames * 546 : real_ticks(); }

static void wait_until(long deadline);

/* A wait in a NavNet call (lock held): the CPU's let go meanwhile, and
 * taken back (the lock let go for that: the CPU comes first). */
static void blocked_wait(long deadline)
{
  int side = cpu_give();
  wait_until(deadline);
  if (side >= 0)
  {
    pthread_mutex_unlock(&lock);
    cpu_take(side);
    pthread_mutex_lock(&lock);
  }
}

static void wait_until(long deadline)
{
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  long now = ms_now(), left = deadline - now;
  if (left <= 0)
    return;
  t.tv_sec += left / 1000;
  t.tv_nsec += (left % 1000) * 1000000;
  if (t.tv_nsec >= 1000000000)
  {
    t.tv_sec++;
    t.tv_nsec -= 1000000000;
  }
  pthread_cond_timedwait(&changed, &lock, &t);
}

static void start_pending(int side);
struct call;
static void start_callback(struct call *call);
static struct call *due_callback(struct channel *ch, struct call *call);
static void ack_tick_locked(int from_read);

static int16_t channel_read(nn_ch_t ch, uint32_t ms, void *buf, uint32_t size, uintptr_t got)
{
  struct queue *q = &ch->c->to[ch->e];
  long deadline = ms_now() + ms;
  int16_t rc = 0;

  /* A game that blocks lets its OS start the callbacks waiting. */
  if (game_side >= 0)
    start_pending(game_side);
  pthread_mutex_lock(&lock);
  *(uint32_t *)got = 0;
  for (;;)
  {
    if (!ch->c->open[ch->e])
    {
      rc = -1;
      break;
    }
    if (q->head != q->tail && !usb_fast_sim_navnet_frozen(ch->e ? ch->c->server : !ch->c->server))
    {
      struct packet *p = &q->p[q->head % PACKETS];
      unsigned n = p->len < size ? p->len : size;
      memcpy(buf, p->data, n);
      *(uint32_t *)got = n;
      q->head++;
      break;
    }
    if (!ch->c->open[!ch->e])
    {
      rc = -1;   /* the other end closed, and it's all read */
      break;
    }
    if (ms_now() >= deadline)
      break;
    if (game_side == 0)
      ack_tick_locked(1);   /* a's game blocks: its OS's tasks run */
    blocked_wait(deadline);
  }
  pthread_cond_broadcast(&changed);
  pthread_mutex_unlock(&lock);
  return rc;
}

__attribute__((aligned(16))) static int16_t public_read(nn_ch_t ch, uint32_t ms, void *buf, uint32_t size, uintptr_t got)
{
  if (ms >= 1 && ms <= 61999)
    ms = CLAMP_MS;
  return channel_read(ch, ms, buf, size, got);
}

/* A write waits for the other side to take it (ND Screen Mirror's: a
 * NavNet write waits for the other side), then for its acknowledgement
 * (write_us): the other's OS takes window packets ahead of what's read
 * there, and past that the write waits for the reader. With two channels
 * the least the calculators showed: a frame's longest message whole (9
 * pieces; the CX CAS's one-channel link, where both games wrote a
 * stand-in's frames before reading, ran), which is what the service's
 * task writes in a row (WRITE_RUN) while the other side's task writes
 * too; with one, 256 (the game writes there, its sends a burst). It fails
 * if either end closes first. */
static int window = 9;
/* FAST_ACK: while b brings the fast transport up, its NavNet writes return
 * only after ACK_TICKS of a's OS ticks with a's NavNet running (a's OS
 * acks; its ack waits in a's ring meanwhile: usb_fast_sim_os_out). */
#define ACK_TICKS 10
static int ack_model, ack_pending;
static long ack_last;
static volatile int game_gone[2];   /* its game thread ended: its OS runs on its own */
static volatile int game_writes[2];   /* writes made by a game's own thread (not a callback's) */
/* R6: the GARBLE case's first link: 1 packet in 12 on the cable is damaged
 * at random (bytes changed, the stream's length words too, cut short,
 * replaced, a length word of 65535, or dropped), as a broken peer or a bad cable would:
 * nspire_link_usb.c must hand its caller only messages that fit and never
 * crash (the sanitizer build checks every access: tests/san.sh). */
static int garbling;
static volatile int bad_length_next;   /* the next packet's length word made 0x7FFF */
static uint32_t garble_state = 0x9e3779b9u;
static uint32_t garble_rand(void)
{
  garble_state ^= garble_state << 13;
  garble_state ^= garble_state >> 17;
  garble_state ^= garble_state << 5;
  return garble_state;
}
/* Under the lock: the packet as it comes out (0: dropped). */
static unsigned garble(uint8_t *data, unsigned size)
{
  unsigned i, n;
  /* Most packets whole, so the stream goes on after a damaged one (a
   * length word changed stalls it: nothing resyncs a NavNet stream). */
  if (garble_rand() % 12)
    return size;
  switch (garble_rand() % 8)
  {
  case 0:
  case 1:
  case 2:   /* one byte changed */
    data[garble_rand() % size] ^= (uint8_t)(1 + garble_rand() % 255);
    return size;
  case 3:   /* a few bytes changed (a length word among them, sometimes) */
    for (n = 1 + garble_rand() % 4; n--;)
      data[garble_rand() % size] = (uint8_t)garble_rand();
    return size;
  case 4:   /* cut short */
    return garble_rand() % size;
  case 5:   /* something else entirely */
    n = 1 + garble_rand() % 254;
    for (i = 0; i < n; i++)
      data[i] = (uint8_t)garble_rand();
    return n;
  case 6:   /* a length word that says the most */
    data[0] = data[1] = 0xFF;
    return size;
  default:
    return 0;
  }
}

__attribute__((aligned(16))) static int16_t channel_write(nn_ch_t ch, void *buf, uint32_t size)
{
  struct queue *q = &ch->c->to[!ch->e];
  int16_t rc = 0;
  unsigned mine;

  struct call *starts = NULL;

  if (size > 254)
    return -2;
  if (game_side >= 0)
    game_writes[game_side]++;
  pthread_mutex_lock(&lock);
  if (ch->c->call && ch->e == 0 && ch->c->open[0] && ch->c->open[1])
    starts = due_callback(ch->c, ch->c->call);   /* the connection's first data: its callback */
  if (!ch->c->open[0] || !ch->c->open[1] || q->tail - q->head >= PACKETS)
    rc = -1;
  else if (bad_length_next && size >= 2 && !(bad_length_next = 0))
  {
    /* R6: the BAD_LENGTH case: a length word of 0x7FFF, more than any message */
    uint8_t *d = buf;
    d[0] = 0xFF;
    d[1] = 0x7F;
    mine = q->tail;
    q->p[q->tail % PACKETS].len = size;
    memcpy(q->p[q->tail % PACKETS].data, buf, size);
    q->tail++;
  }
  else if (garbling && size && !(size = garble(buf, size)))
    ;   /* lost on the way */
  else
  {
    int writer = ch->e ? ch->c->server : !ch->c->server;
    mine = q->tail;
    usb_fast_sim_navnet_wrote(writer);
    q->p[q->tail % PACKETS].len = size;
    memcpy(q->p[q->tail % PACKETS].data, buf, size);
    q->tail++;
    pthread_cond_broadcast(&changed);
    ch->c->writing[ch->e]++;
    if (ack_model && writer == 1)
    {
      ack_pending = ACK_TICKS;
      usb_fast_sim_os_out(0, 1);
      while (ack_pending > 0 && ch->c->open[0] && ch->c->open[1])
      {
        if (game_gone[0] && !usb_fast_sim_navnet_frozen(0))
        {
          ack_pending = 0;
          usb_fast_sim_os_out(0, 0);
          break;
        }
        blocked_wait(ms_now() + 20);
      }
    }
    while ((int)(mine - q->head) >= window)
    {
      if (!ch->c->open[0] || !ch->c->open[1])
      {
        rc = -1;
        break;
      }
      blocked_wait(ms_now() + 50);
    }
    ch->c->writing[ch->e]--;
  }
  pthread_cond_broadcast(&changed);
  pthread_mutex_unlock(&lock);
  if (starts)
    start_callback(starts);
  {
    int side = cpu_give();
    usleep(write_us);   /* the packet's acknowledgement */
    cpu_take(side);
  }
  return rc;
}

uintptr_t link_usb_test_address(int number)
{
  return number == 272 ? (uintptr_t)public_read : number == 273 ? (uintptr_t)channel_write : 0;
}

read_fn link_usb_test_unwrap(uintptr_t entry)
{
  return entry == (uintptr_t)public_read && short_reads ? channel_read : NULL;
}

static pthread_mutex_t irq = PTHREAD_MUTEX_INITIALIZER;
unsigned link_usb_test_irq_off(void) { pthread_mutex_lock(&irq); return 0; }
void link_usb_test_irq_restore(unsigned old) { (void)old; pthread_mutex_unlock(&irq); }

struct call { int side; nn_ch_t end; };

static void *callback_task(void *arg)
{
  struct call *c = arg;
  cpu_take(c->side);   /* cpu_model: once the game lets go */
  calc[c->side].cb(c->end, NULL);
  pthread_mutex_lock(&lock);
  c->end->c->open[c->end->e] = 0;   /* the callback's return closes its end */
  calc[c->side].callbacks--;
  pthread_cond_broadcast(&changed);
  pthread_mutex_unlock(&lock);
  cpu_give();
  free(c);
  return NULL;
}

static void start_callback(struct call *call)
{
  pthread_t task;
  pthread_create(&task, NULL, callback_task, call);
  pthread_detach(task);
}

static void start_pending(int side)
{
  struct call *start[16];
  int i, n;

  pthread_mutex_lock(&lock);
  n = calc[side].pending_count;
  memcpy(start, calc[side].pending, sizeof(start[0]) * n);
  calc[side].pending_count = 0;
  pthread_mutex_unlock(&lock);
  for (i = 0; i < n; i++)
    start_callback(start[i]);
}

/* Under the lock: a callback that is due now counted, and either kept for
 * its game to block (late_callbacks) or handed back to be started. */
static struct call *due_callback(struct channel *ch, struct call *call)
{
  int other = call->side;

  ch->call = NULL;
  calc[other].callbacks++;
  if (calc[other].late_callbacks && !calc[other].cpu_model && calc[other].pending_count < 16)
  {
    calc[other].pending[calc[other].pending_count++] = call;
    return NULL;
  }
  return call;
}

static int16_t connect_to(int side, nn_nh_t node, uint32_t service, nn_ch_t *out)
{
  int other = (int)(intptr_t)node - 1;
  struct channel *ch;
  struct fake_end *ends;
  struct call *call;

  (void)side;
  pthread_mutex_lock(&lock);
  if (other < 0 || other > 1 || !calc[other].registered || service != 0x5047)
  {
    pthread_mutex_unlock(&lock);
    return -3;
  }
  ch = calloc(1, sizeof(*ch));
  ends = calloc(2, sizeof(*ends));
  ch->open[0] = ch->open[1] = 1;
  ch->server = other;
  ends[0].c = ends[1].c = ch;
  ends[0].e = 0;
  ends[1].e = 1;
  channel_ends[channel_count] = ends;
  channels[channel_count++] = ch;
  call = malloc(sizeof(*call));
  call->side = other;
  call->end = &ends[1];
  /* DETECT: the calculators' OS starts the service's callback at the
   * connection's first data, not at the Connect (every log r1-r5: no
   * callback before the other side's first write); connect_now: the old
   * fake's, at once. */
  if (connect_now)
    call = due_callback(ch, call);
  else
  {
    ch->call = call;
    call = NULL;
  }
  pthread_mutex_unlock(&lock);
  if (call)
    start_callback(call);
  *out = &ends[0];
  return 0;
}

static int16_t disconnect(int side, nn_ch_t ch)
{
  pthread_mutex_lock(&lock);
  if (ch->c->writing[ch->e])
    calc[side].disconnect_while_writing++;
  ch->c->open[ch->e] = 0;
  calc[side].os_ran_unchecked = 1;
  pthread_cond_broadcast(&changed);
  pthread_mutex_unlock(&lock);
  return 0;
}

static int16_t start_service(int side, uint32_t id, void *data, void (*cb)(nn_ch_t, void *))
{
  (void)data;
  if (id != 0x5047 || calc[side].registered)
    return -1;
  calc[side].cb = cb;
  calc[side].registered = 1;
  return 1;
}

static int16_t stop_service(int side, uint32_t id)
{
  int i;
  (void)id;
  pthread_mutex_lock(&lock);
  if (calc[side].callbacks)
    calc[side].stop_with_callbacks++;
  calc[side].registered = 0;
  for (i = 0; i < channel_count; i++)
    if (channels[i]->server == side)
      channels[i]->open[1] = 0;
  pthread_cond_broadcast(&changed);
  /* StopService waits: the OS runs the callbacks still to start, too. */
  pthread_mutex_unlock(&lock);
  start_pending(side);
  pthread_mutex_lock(&lock);
  while (calc[side].callbacks)
    blocked_wait(ms_now() + 100);
  calc[side].os_ran_unchecked = 1;
  pthread_mutex_unlock(&lock);
  return 1;
}

static int enum_next[2];
static int enum_inits[2];   /* looks so far */

#define SIDE_CALLS(s, n)                                                                           \
  nn_oh_t s##_TI_NN_CreateOperationHandle(void) { return (nn_oh_t)(intptr_t)1; }                  \
  int16_t s##_TI_NN_DestroyOperationHandle(nn_oh_t oh) { (void)oh; return 0; }                    \
  int16_t s##_TI_NN_NodeEnumInit(nn_oh_t oh)                                                      \
  {                                                                                                \
    (void)oh;                                                                                      \
    start_pending(n);   /* it waits for NavNet's task: the OS runs its others */                   \
    {                                                                                              \
      int side_ = cpu_give();                                                                      \
      usleep(200);                                                                                 \
      cpu_take(side_);                                                                             \
    }                                                                                              \
    enum_next[n] = 0;                                                                              \
    enum_inits[n]++;                                                                               \
    return 0;                                                                                      \
  }                                                                                                \
  int16_t s##_TI_NN_NodeEnumNext(nn_oh_t oh, nn_nh_t *nh)                                          \
  {                                                                                                \
    (void)oh;                                                                                      \
    if (enum_next[n]++ || !calc[n].sees_other || calc[n].sleeps < calc[n].need_sleeps)                                                  \
      return -1;                                                                                   \
    *nh = (nn_nh_t)(intptr_t)(!(n) + 1);                                                           \
    return 0;                                                                                      \
  }                                                                                                \
  int16_t s##_TI_NN_NodeEnumDone(nn_oh_t oh) { (void)oh; return 0; }                              \
  int16_t s##_TI_NN_Connect(nn_nh_t nh, uint32_t id, nn_ch_t *ch) { return connect_to(n, nh, id, ch); } \
  int16_t s##_TI_NN_Disconnect(nn_ch_t ch) { return disconnect(n, ch); }                          \
  int16_t s##_TI_NN_StartService(uint32_t id, void *d, void (*cb)(nn_ch_t, void *))              \
  { return start_service(n, id, d, cb); }                                                          \
  int16_t s##_TI_NN_StopService(uint32_t id) { return stop_service(n, id); }                      \
  void s##_nspire_link_log(const char *fmt, ...)                                                   \
  {                                                                                                \
    va_list ap;                                                                                    \
    if (!verbose)                                                                                  \
      return;                                                                                      \
    va_start(ap, fmt);                                                                             \
    printf("  " #s ": ");                                                                          \
    vprintf(fmt, ap);                                                                              \
    printf("\n");                                                                                  \
    va_end(ap);                                                                                    \
  }

static int verbose;
SIDE_CALLS(a, 0)
SIDE_CALLS(b, 1)

/* nspire_os.c's near_least: a tenth of the tick in timer ticks, at least
 * 16 (CX II 10.25 ms: 33; CX 2.98 ms: 16). */
static uint32_t near_least(long tick_us)
{
  uint32_t w = (uint32_t)(tick_us * 32768 / 1000000) / 10;
  return w < 16 ? 16 : w;
}

/* FAST_ACK: one of a's OS ticks (a sleep, or its game blocked in a read,
 * 10 ms at most once); with its NavNet running its OS acks. */
static void ack_tick_locked(int from_read)
{
  if (ack_pending <= 0 || (from_read && ms_now() - ack_last < 10))
    return;
  ack_last = ms_now();
  if (!usb_fast_sim_navnet_frozen(0) && --ack_pending == 0)
  {
    usb_fast_sim_os_out(0, 0);
    pthread_cond_broadcast(&changed);
  }
}

static void ack_tick(void)
{
  pthread_mutex_lock(&lock);
  ack_tick_locked(0);
  pthread_mutex_unlock(&lock);
}

/* nspire_os.c's: a sleep is an OS tick (10 ms), and the OS starts the
 * callbacks waiting meanwhile. */
#define OS_CALLS(s, n)                                                                             \
  unsigned s##_nspire_os_wanted;                                                                   \
  uint32_t s##_nspire_os_task_var;                                                                 \
  volatile const uint32_t *s##_nspire_raw_timer;                                                   \
  uint32_t s##_nspire_raw_timer_us(uint32_t counts) { return counts; }                             \
  int s##_nspire_os_task_stack(uint32_t task, uint32_t words[3]) { (void)task; (void)words; return 0; } \
  int s##_nspire_os_watchdog_off(void) { calc[n].os_ran_unchecked = 0; return 0; }                \
  int s##_nspire_os_ready(void) { return calc[n].os_sleep; }                                       \
  int s##_nspire_os_sleep(void)                                                                    \
  {                                                                                                \
    int side_;                                                                                     \
    if (!calc[n].os_sleep)                                                                         \
      return 0;                                                                                    \
    if (n == 0)                                                                                    \
      ack_tick();                                                                                  \
    start_pending(n);                                                                              \
    calc[n].sleeps++;                                                                              \
    side_ = cpu_give();                                                                            \
    if (calc[n].os_tick_us)                                                                        \
    {                                                                                              \
      long now_ = us_now(), wait_ = calc[n].os_tick_us - now_ % calc[n].os_tick_us;              \
      calc[n].waited_us += wait_;                                                                  \
      skew_us += wait_;                                                                            \
    }                                                                                              \
    else if (!fast_sleeps)                                                                         \
      usleep(10000);                                                                               \
    cpu_take(side_);                                                                               \
    calc[n].last_wake = us_now();                                                                  \
    return 1;                                                                                      \
  }                                                                                                \
  /* nspire_os.c's: the tick's phase known for 8 ticks after a wake, and the                    \
   * next at most max_wait timer ticks away (0: 33, 1 ms). */                                      \
  int s##_nspire_os_sleep_near(uint32_t max_wait)                                                  \
  {                                                                                                \
    long now_ = us_now(), tick_ = calc[n].os_tick_us ? calc[n].os_tick_us : 10000;                \
    if (!max_wait)                                                                                 \
      max_wait = near_least(tick_);                                                                \
    if (!calc[n].os_sleep || now_ - calc[n].last_wake >= 8 * tick_)                               \
      return 0;                                                                                    \
    if ((tick_ - now_ % tick_) * 32768 / 1000000 > (long)max_wait)                                \
      return 0;                                                                                    \
    return s##_nspire_os_sleep();                                                                  \
  }                                                                                                \
  uint32_t s##_nspire_os_near_wait(uint32_t since, uint32_t from, uint32_t until)                  \
  {                                                                                                \
    long tick_ = calc[n].os_tick_us ? calc[n].os_tick_us : 10000;                                 \
    uint32_t w_ = near_least(tick_);                                                               \
    if (until > from && since > from)                                                              \
      w_ += (uint32_t)(tick_ * 32768 / 2000000 * ((since > until ? until : since) - from) / (until - from)); \
    return w_;                                                                                     \
  }                                                                                                \
  int s##_nspire_os_sleep_before(uint32_t deadline) { (void)deadline; return 0; }                  \
  uint32_t s##_nspire_os_since_sleep(void) { return (uint32_t)((us_now() - calc[n].last_wake) * 32768 / 1000000); } \
  void s##_nspire_os_report(char *text, unsigned size) { snprintf(text, size, "OS sleep: fake"); }
OS_CALLS(a, 0)
OS_CALLS(b, 1)

/* ---- The two transports ---- */

#define TRANSPORT(s)                                                                               \
  int s##_link_cable_watch(int may_connect, int plugged, int fresh);                               \
  int s##_link_cable_open(int automatic);                                                          \
  int s##_link_cable_ready(void);                                                                  \
  int s##_link_cable_send(const void *msg, unsigned len);                                          \
  int s##_link_cable_receive(void *buf, unsigned size, unsigned wait_ms);                          \
  void s##_link_cable_close(void);                                                                 \
  void s##_link_cable_shutdown(void);                                                              \
  void s##_link_cable_frame_done(unsigned);                                                        \
  void s##_link_cable_flush(void);                                                                 \
  void s##_link_cable_push(void);                                                                  \
  int s##_link_cable_gated(void);                                                                  \
  const char *s##_link_cable_error(void);                                                          \
  uint32_t s##_link_cable_fast_offer(void);                                                        \
  int s##_link_cable_go_fast(uint32_t, const char *);                                              \
  void s##_link_cable_fast_drop(const char *);                                                     \
  int s##_link_cable_fast_up(void);                                                                \
  int s##_link_cable_linked_os(void);                                                              \
  int s##_link_cable_fast_holds(void);                                                             \
  void s##_link_cable_second(char *text, unsigned size);
TRANSPORT(a)
TRANSPORT(b)

struct transport
{
  int (*watch)(int, int, int);
  int (*open)(int);
  int (*ready)(void);
  int (*send)(const void *, unsigned);
  int (*receive)(void *, unsigned, unsigned);
  void (*close)(void);
  void (*shutdown)(void);
  uint32_t (*offer)(void);
  int (*go_fast)(uint32_t, const char *);
  void (*fast_drop)(const char *);
  int (*fast_up)(void);
  int (*linked_os)(void);
  void (*frame_done)(unsigned);
  void (*second)(char *, unsigned);
};
static const struct transport sides[2] = {
  { a_link_cable_watch, a_link_cable_open, a_link_cable_ready, a_link_cable_send, a_link_cable_receive, a_link_cable_close,
    a_link_cable_shutdown, a_link_cable_fast_offer, a_link_cable_go_fast, a_link_cable_fast_drop,
    a_link_cable_fast_up, a_link_cable_linked_os, a_link_cable_frame_done, a_link_cable_second },
  { b_link_cable_watch, b_link_cable_open, b_link_cable_ready, b_link_cable_send, b_link_cable_receive, b_link_cable_close,
    b_link_cable_shutdown, b_link_cable_fast_offer, b_link_cable_go_fast, b_link_cable_fast_drop,
    b_link_cable_fast_up, b_link_cable_linked_os, b_link_cable_frame_done, b_link_cable_second },
};

static int failures;
#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } while (0)

#define MESSAGES 120   /* one at a time; then a burst of BURST */
#define BURST    60

static unsigned msg_len(int side, int i) { return 1 + (unsigned)((i * 37 + side * 11) % 600); }

static void fill(uint8_t *m, int side, int i, unsigned len)
{
  unsigned k;
  for (k = 0; k < len; k++)
    m[k] = (uint8_t)(side * 101 + i * 7 + k);
}

/* What a case does besides linking: pull the cable mid-link; a's
 * service's task stuck in a write at its close; a closing with messages
 * queued; a quitting while it holds b's connection; b linking on its own
 * with an a that doesn't link back. */
enum { PLAIN, PULL, STUCK, QUEUED_AT_CLOSE, HELD_QUIT, NOT_BACK, WATCH_SLEEPS, HOST_TICKS, HOLD, GARBLE, BAD_LENGTH,
       MENU, READY_FAST, AUTO_FAST, OLD_FAKE,
       /* the fast transport (nspire_link_fast.c, usb_fast_sim.c): up; b an
        * older gpSP (no offer, never tries); b a CX II at the device end
        * (nothing to offer); the device never answers; its endpoint stalls
        * mid-link; the cable out mid-link and back; the arbiter says no at
       * a's take-over (a pad or a power cycle: nspire_arb_fast_refusal),
       * or makes a let go mid-link; (2026-11-11, F2-F5) the OS turns the
       * USB interrupt on again mid-link; the host's async schedule stops
       * slowly at the take-over (ASS lags past SCHED_POLLS) or won't stop
       * at the release; b's READY write returns only once a's OS acked it
       * (one channel, 10 of a's OS ticks with its schedule its own);
       * (LINK2) up, and b's linked frames paced as the r6 logs' CX CAS's;
       * the OS's schedule's status lags its start again at the
       * release (the r6 pull): the release reads USBSTS at most
       * SCHED_POLLS + SCHED_AGAIN_POLLS (+ a few) times, not twice SCHED_POLLS */
       FAST, FAST_OLD, FAST_CX2DEV, FAST_NOANSWER, FAST_STALL, FAST_PULL, FAST_REFUSED, FAST_DROP,
       FAST_VIC_ON, FAST_SLOW_TAKE, FAST_SLOW_RELEASE, FAST_ACK, FAST_PACE, FAST_SLOW_RESTART };
#define IS_FAST(k) ((k) >= FAST)
/* DETECT: READY_FAST / AUTO_FAST: both ready (picked, or on their own after
 * the watch) this soon after open, the callbacks starting only at the first
 * data as on the calculators (the knock). */
#define READY_FAST_MS 1000

struct game { int side; int ok; const char *what; int watch, later_ms, kind; long send_max_us, ready_ms; long rt_us; };
static int watching;   /* the case's games watch for the other first */
static volatile int got_count[2], pulled;

/* Each side's NavNet in the fake: the cable out (every channel closed, no
 * node seen) for ms, then back in. */
static void pull_cable(int ms)
{
  int i, sees[2] = { calc[0].sees_other, calc[1].sees_other };
  pthread_mutex_lock(&lock);
  calc[0].sees_other = calc[1].sees_other = 0;
  for (i = 0; i < channel_count; i++)
    channels[i]->open[0] = channels[i]->open[1] = 0;
  pthread_cond_broadcast(&changed);
  pthread_mutex_unlock(&lock);
  usleep(ms * 1000);
  calc[0].sees_other = sees[0];
  calc[1].sees_other = sees[1];
}

/* Game frames until the watch sees the other's gpSP (2); 1 if it did. */
static int watch_until_there(struct game *g, const struct transport *t, int may_connect, long give_up_ms)
{
  long start = ms_now();
  int r;

  while ((r = t->watch(may_connect, 1, 1)) != 2)
  {
    if (r < 0 || ms_now() - start > give_up_ms)
    {
      g->what = r < 0 ? "watch failed" : "the watch never saw the other";
      return 0;
    }
    usleep(1000);
    calc[g->side].frames++;
  }
  return 1;
}

/* Open, then game frames until ready: 1, 0 if it failed (g->what), -1 if
 * ready said < 0. */
static int open_and_ready(struct game *g, const struct transport *t, int automatic)
{
  long start = ms_now();
  int r;

  if (t->open(automatic) < 0)
  {
    g->what = "open failed";
    return 0;
  }
  g->ready_ms = -1;
  while ((r = t->ready()) == 0)
  {
    if (ms_now() - start > 10000)
    {
      g->what = "never ready";
      return 0;
    }
    usleep(1000);   /* a game frame */
    calc[g->side].frames++;
  }
  if (r < 0)
  {
    g->what = "ready failed";
    return -1;
  }
  g->ready_ms = ms_now() - start;
  return 1;
}

/* MESSAGES each way, one at a time (each waits for the other's) and then
 * BURST all at once, checked: 1, 0 if something went wrong (g->what), -1
 * if the cable broke (send or receive < 0). */
static int exchange(struct game *g, const struct transport *t)
{
  uint8_t out[600], in[700], want[600];
  long start = ms_now();
  int i, r, got = 0;

  got_count[g->side] = 0;
  for (i = 0; i < MESSAGES + BURST; i++)
  {
    unsigned len = msg_len(g->side, i);
    fill(out, g->side, i, len);
    if (g->kind == FAST_DROP && g->side == 0 && i == 40)
    {
      /* The arbiter's no mid-link (nspire_arb_tick: a pad, a drive, the
       * holder changed): a lets go; b hears its BYE; NavNet carries on. */
      if (!t->fast_up())
      {
        g->what = "the fast transport wasn't up before the let-go";
        return 0;
      }
      t->fast_drop("the port's owner is PAD (test)");
      if (t->fast_up())
      {
        g->what = "still up after the let-go";
        return 0;
      }
    }
    {
      struct timespec t0, t1;
      long us;
      clock_gettime(CLOCK_MONOTONIC, &t0);
      r = t->send(out, len);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
      if (us > g->send_max_us)
        g->send_max_us = us;
    }
    if (r < 0)
    {
      g->what = "send failed";
      return -1;
    }
    /* The first MESSAGES one at a time; the rest sent all before taken. */
    while (got < MESSAGES + BURST && (i < MESSAGES ? got <= i : i == MESSAGES + BURST - 1))
    {
      unsigned wlen = msg_len(!g->side, got);
      r = t->receive(in, sizeof(in), 20);
      if (r < 0)
      {
        g->what = "receive failed";
        return -1;
      }
      if (!r)
      {
        if (ms_now() - start > 30000)
        {
          g->what = "messages stopped coming";
          return 0;
        }
        continue;
      }
      fill(want, !g->side, got, wlen);
      if ((unsigned)r != wlen || memcmp(in, want, wlen))
      {
        g->what = "a message came wrong or out of order";
        return 0;
      }
      got_count[g->side] = ++got;
      if (got == MESSAGES)
        g->rt_us = (ms_now() - start) * 1000 / MESSAGES;
    }
  }
  if (g->kind == FAST_DROP && t->fast_up())
  {
    g->what = "the fast transport still up at the end (b never heard a's let-go)";
    return 0;
  }
  return 1;
}

/* LINK2: a raw timer (counts down, 1 a us) for the FAST case, so
 * the per-second line's new "read longest" field measures the service's
 * task's NavNet reads while the fast transport carries the link (nothing
 * comes on NavNet then). Checked: b's line counts turns and a longest read
 * above 0 and under 300 ms. On a calculator the field says whether each of
 * the r6 CAS's OS sleeps (2 turns each, 50-82 ms) is one of these reads. */
static volatile uint32_t fake_timer;
static volatile int fake_timer_on;
static void *fake_timer_run(void *unused)
{
  (void)unused;
  while (fake_timer_on)
  {
    fake_timer = ~(uint32_t)us_now();
    usleep(100);
  }
  return NULL;
}

static int svc_read_case(struct game *g, const struct transport *t)
{
  char text[400];
  const char *p;
  unsigned turns = 0, longest = 0;
  int side;

  if (!t->fast_up())
    return 1;
  if (g->side == 0)
  {
    /* a, the host, stays linked while b looks (b's close would end it) */
    side = cpu_give();
    usleep(700000);
    cpu_take(side);
    return 1;
  }
  /* A read that began before b gave the OS any time (the take-over: the
   * game held the CPU) ends only now and would count its whole length
   * (seen: 625-643 ms under load): the OS runs 100 ms first, so the reads
   * measured all start with the OS running. */
  side = cpu_give();
  usleep(100000);
  cpu_take(side);
  t->second(text, sizeof(text));   /* this second's counts start now */
  side = cpu_give();
  usleep(300000);
  cpu_take(side);
  t->second(text, sizeof(text));
  if (verbose)
    printf("  side b, 300 ms with the fast transport up: %s\n", text);
  p = strstr(text, " turns (read longest ");
  if (!p || sscanf(text, "svc %*u w (longest %*u ms), %u turns (read longest %u ms)", &turns, &longest) != 2 ||
      turns == 0 || longest == 0 || longest > 300)
  {
    static char what[2][200];
    snprintf(what[g->side], sizeof(what[0]), "the service's reads while fast: %u turns, the longest %u ms (%.120s)", turns,
             longest, text);
    g->what = what[g->side];
    return 0;
  }
  return 1;
}

/* The fast transport brought up as nspire_link.c does after the hellos
 * (both at once, each with the other's offer): up or not as the case
 * wants. 1 if as wanted. */
static int go_fast(struct game *g, const struct transport *t)
{
  int want = g->kind == FAST || g->kind == FAST_STALL || g->kind == FAST_PULL || g->kind == FAST_DROP ||
             g->kind == FAST_VIC_ON || g->kind == FAST_SLOW_RELEASE || g->kind == FAST_ACK || g->kind == FAST_PACE ||
             g->kind == FAST_SLOW_RESTART, r;
  uint32_t other;
  long t0 = ms_now();

  if (g->kind == FAST_OLD && g->side == 1)
    return 1;   /* an older gpSP: no offer in its hello, never tries */
  other = g->kind == FAST_OLD ? 0 : sides[!g->side].offer();
  if (g->kind == FAST_ACK && g->side == 1)
    ack_model = 1;
  r = t->go_fast(other, g->kind == FAST_REFUSED && g->side == 0 ? "a bus power cycle was just made (test)" : NULL);
  if (g->kind == FAST_ACK && g->side == 1)
    ack_model = 0;
  if (r != want)
  {
    static char what[2][80];
    snprintf(what[g->side], sizeof(what[0]), "the fast transport %s (wanted %s), after %ld ms", r ? "came up" : "didn't come up",
             want ? "up" : "NavNet", ms_now() - t0);
    g->what = what[g->side];
    return 0;
  }
  if (g->kind == FAST_NOANSWER && ms_now() - t0 > 9000)
  {
    g->what = "the fast transport took over 9 s to give up";
    return 0;
  }
  /* The refused side's FAIL reaches the other in a NavNet round trip: the
   * other gives up at once, not after its 5 s (host) or 3.6 s (device). */
  if (g->kind == FAST_REFUSED && ms_now() - t0 > 1500)
  {
    static char what[2][80];
    snprintf(what[g->side], sizeof(what[0]), "gave up %ld ms after the other was refused (1.5 s at most)", ms_now() - t0);
    g->what = what[g->side];
    return 0;
  }
  return 1;
}

/* LINK2: the r6 logs' CX CAS (192 MHz, the device end, Emerald in Dewford,
 * automatic frameskip 1): alone ~46 game frames a second; linked over the
 * fast transport 35.2, its frames waiting for the CX II 8 times in 141 s;
 * its seconds without an OS sleep made 45-46 frames, with them 30-44. The
 * sleeps were the frame's spare time (nspire_wait_until, OS_WANT_LINK set
 * while linked) after the frameskip's catch-up (more than 6 frames late:
 * carry on from now), and each came back after 50-363 ms (the once-a-second
 * lines' longest: 65 57 61 51 82 66 363 72 ms), not at the OS's next tick
 * (2.5 ms). The pacing as nspire_frameskip.c's begin_frame and nspire.c's
 * wait do it, on a frame model of 10 ms skipped / 32.4 ms drawn, one in
 * two drawn: game frames a second over 60 s, with the OS given the spare
 * time (wants) or not. */
static double pace_fps(int wants, double *sleeps_s)
{
  static const long sleep_us[] = { 65000, 57000, 61000, 51000, 82000, 66000, 363000, 72000 };
  const long period = 16743, tick = 2500, room = 2011, span = 60000000;
  long now = 0, next = 0, last_wake = -1000000, frames = 0, sleeps = 0;
  int draw = 1;

  while (now < span)
  {
    long late;
    next += period;
    late = now - next;
    if (late < 0)
    {
      while (now < next)
      {
        /* nspire_os_sleep_before: the next tick, known 8 ticks after a wake */
        long at = now - last_wake < 8 * tick ? last_wake + ((now - last_wake) / tick + 1) * tick : now + tick;
        if (wants && next - at >= room)
        {
          now += sleep_us[sleeps % (sizeof(sleep_us) / sizeof(sleep_us[0]))];
          last_wake = now;
          sleeps++;
        }
        else
          now = next;
      }
    }
    else if (late > 6 * period)
      next = now;
    now += draw ? 32400 : 10000;
    draw = !draw;
    frames++;
  }
  if (sleeps_s)
    *sleeps_s = sleeps * 1e6 / now;
  return frames * 1e6 / now;
}

/* FAST_PACE, side b with the fast transport up: the transport's own frame
 * end never sleeps (svc_out empty), the linked frame wants no OS time
 * (link_cable_linked_os), and the pacing model's figures before LINK2 (the
 * OS always wanted while linked) and now. */
static int pace_case(struct game *g, const struct transport *t)
{
  static char what[160];
  double alone, before, now, before_s, now_s;
  int i, s0 = calc[g->side].sleeps;

  for (i = 0; i < 600 && t->fast_up(); i++)
    t->frame_done(1);
  if (t->fast_up() && calc[g->side].sleeps != s0)
  {
    snprintf(what, sizeof(what), "600 linked frames' ends on the fast transport slept %d times in the OS",
             calc[g->side].sleeps - s0);
    g->what = what;
    return 0;
  }
  alone = pace_fps(0, NULL);
  before = pace_fps(1, &before_s);
  now = pace_fps(t->linked_os(), &now_s);
  printf("  LINK2 pacing (r6's CX CAS: 10 / 32.4 ms frames, sleeps of 51-363 ms): alone %.1f game frames a second; "
         "linked fast, the OS given the spare time (R9F) %.1f (%.1f sleeps a second); now %.1f (%.1f)\n",
         alone, before, before_s, now, now_s);
  if (now < 0.97 * alone || before > 0.85 * alone)
  {
    snprintf(what, sizeof(what), "pacing: alone %.1f, R9F %.1f, now %.1f", alone, before, now);
    g->what = what;
    return 0;
  }
  return 1;
}

/* R6: the GARBLE case's exchange: what comes is damaged (garble), so it
 * isn't checked, only that each message fits what was asked for; it ends
 * when nothing more comes for 1.5 s, or the link breaks (fine too). */
static int garbled_exchange(struct game *g, const struct transport *t)
{
  uint8_t out[600], in[700];
  int i, r = 0;
  long idle;

  for (i = 0; i < MESSAGES + BURST; i++)
  {
    unsigned len = msg_len(g->side, i);
    fill(out, g->side, i, len);
    if (t->send(out, len) < 0)
      return 1;
    r = t->receive(in, 300, 1);   /* asks for less than some are */
    if (r > 300)
    {
      g->what = "a message longer than the room asked for";
      return 0;
    }
    if (r < 0)
      return 1;
    got_count[g->side] += r > 0;
  }
  for (idle = ms_now(); ms_now() - idle < 1500;)
  {
    r = t->receive(in, sizeof(in), 20);
    if (r > (int)sizeof(in))
    {
      g->what = "a message longer than the room asked for";
      return 0;
    }
    if (r < 0)
      return 1;
    if (r > 0)
    {
      idle = ms_now();
      got_count[g->side]++;
    }
  }
  if (verbose)
    printf("side %c took %d damaged messages\n", 'a' + g->side, got_count[g->side]);
  return 1;
}

/* One link: watched for (the case's), ready, the exchange; then unlinked.
 * And the case's kind. */
/* EXIT-R5: each close (and shutdown) returns with the OS's time after its
 * last Disconnect or StopService followed by nspire_os_watchdog_off: the
 * keypad line and the busy cursor off again. */
#define CLOSE_CHECKED(t, g)                                                                        \
  do                                                                                               \
  {                                                                                                \
    (t)->close();                                                                                  \
    if (calc[(g)->side].os_ran_unchecked)                                                          \
      calc[(g)->side].unchecked_at_close++;                                                        \
  } while (0)

static void *game(void *arg)
{
  struct game *g = arg;
  const struct transport *t = &sides[g->side];
  int r, i;

  game_side = g->side;
  cpu_take(g->side);
  g->ok = 0;
  g->send_max_us = 0;
  if (g->later_ms)
  {
    int side = cpu_give();
    usleep(g->later_ms * 1000);   /* its gpSP starts later */
    cpu_take(side);
  }
  if (g->kind == HELD_QUIT)
  {
    /* a holds b's connection from watching (never connecting itself) and
     * closes gpSP; b links on its own and is given up on. */
    if (!watch_until_there(g, t, g->side, 15000))
      goto out;
    if (g->side == 0)
    {
      usleep(100000);   /* frames with the connection held */
      t->shutdown();
      g->ok = 1;
      goto out;
    }
    g->ok = open_and_ready(g, t, 1) < 0;
    if (!g->ok && !g->what[0])
      g->what = "ready with only this side's channel, on its own";
    CLOSE_CHECKED(t, g);
    goto out;
  }
  if (g->kind == WATCH_SLEEPS)
  {
    /* a watches with no other gpSP there, in a game with no spare time
     * (its frames 17 ms, the CPU held): with something on the port the OS
     * gets a tick each frame; a port that can't tell, every 250 ms. */
    if (g->side == 0)
    {
      int plugged, frames, before;
      g->ok = 1;
      for (plugged = 1; plugged >= -1; plugged -= 2)
      {
        before = calc[0].sleeps;
        for (frames = 0; frames < 30; frames++)
        {
          t->watch(1, plugged, 1);
          usleep(17000);
          calc[0].frames++;
        }
        before = calc[0].sleeps - before;
        if (plugged > 0 ? before < 29 : before < 1 || before > 3)
        {
          static char what[80];
          snprintf(what, sizeof(what), "plugged %d: %d OS sleeps in 30 frames", plugged, before);
          g->what = what;
          g->ok = 0;
        }
      }
      /* A CX's controller off: a tick every 250 ms, one look (then one in
       * 6 s), no connection. */
      {
        int looks = enum_inits[0];
        before = calc[0].sleeps;
        for (frames = 0; frames < 30; frames++)
        {
          if (a_link_cable_gated() < 0)
            break;
          usleep(17000);
          calc[0].frames++;
        }
        before = calc[0].sleeps - before;
        looks = enum_inits[0] - looks;
        if (frames < 30 || before < 1 || before > 3 || looks != 1)
        {
          static char what[100];
          snprintf(what, sizeof(what), "controller off: %d frames, %d OS sleeps, %d looks in 30 frames", frames,
                   before, looks);
          g->what = what;
          g->ok = 0;
        }
      }
    }
    else
      g->ok = 1;
    /* S8: the budgets over 3000 frames (50 s of 1/60 s, on the virtual clock):
     * something on the port: the OS gets a tick each frame and a look about
     * every WATCH_LOOK_MS (1.5 s); a CX's controller off: one tick per WATCH_SLEEP_MS
     * (250 ms) and one look per GATED_LOOK_MS (6 s). */
    if (g->side == 0 && g->ok)
    {
      int plugged, frames;

      fast_sleeps = 1;
      for (plugged = 1; plugged >= -1; plugged -= 2)
      {
        long s0 = calc[0].sleeps, t0 = ms_now();
        int l0 = enum_inits[0], looks, ticks;

        for (frames = 0; frames < 3000; frames++)
        {
          if (plugged > 0)
            t->watch(1, 1, 1);
          else if (a_link_cable_gated() < 0)
            break;
          skew_us += 17000;   /* 3000 frames = 51 s */
          calc[0].frames++;
        }
        ticks = (int)(calc[0].sleeps - s0);
        looks = enum_inits[0] - l0;
        if (plugged > 0 ? ticks < 2900 || ticks > 3000 || looks < 51000 / 1500 - 2 || looks > 51000 / 1500 + 1 :
                          ticks < 51000 / 270 || ticks > 51000 / 250 || looks < 8 || looks > 9)
        {
          static char what[100];
          snprintf(what, sizeof(what), "3000 frames, plugged %d: %d OS sleeps, %d looks in %ld ms", plugged, ticks, looks,
                   ms_now() - t0);
          g->what = what;
          g->ok = 0;
        }
        if (verbose)
          printf("  S8 plugged %d: 3000 frames: %d OS sleeps, %d looks in %ld ms\n", plugged, ticks, looks, ms_now() - t0);
      }
      fast_sleeps = 0;
    }
    /* P2: fast forward with something on the port, not linked, nothing
     * seen: frames of 4.6 ms (no spare time), the OS's tick 10.25 ms (a CX
     * II's), 20 s on the virtual clock. Fresh (the port changed under 10 s
     * ago: today's rule, a tick each 1/60 s): about 50 sleeps and 100 ms
     * waited a second; after that, the tick only when it's near, forced
     * every 100 ms: at most 25 sleeps and 30 ms a second, and never more
     * than 100 ms (plus a frame and a tick) without one. */
    if (g->side == 0 && g->ok)
    {
      /* 4.6 ms: the model's town frame; 5.7 ms: the calibrated real one,
       * whose length beats against the tick (a fixed 1 ms window missed it
       * every time: 88 ms a second). */
      static const long frame_us[] = { 4600, 5700 };
      int fresh, frames, f;

      calc[0].os_tick_us = 10250;
      for (f = 0; f < 2; f++)
        for (fresh = 1; fresh >= 0; fresh--)
        {
          long s0 = calc[0].sleeps, w0 = calc[0].waited_us, t0 = us_now(), gap = 0, wake = calc[0].last_wake;
          double secs, per_s, waited_ms;

          for (frames = 0; frames < 20 * 1000000 / frame_us[f]; frames++)
          {
            t->watch(1, 1, fresh);
            if (calc[0].last_wake != wake)
            {
              if (calc[0].last_wake - wake > gap && frames)
                gap = calc[0].last_wake - wake;
              wake = calc[0].last_wake;
            }
            skew_us += frame_us[f];
            calc[0].frames++;
          }
          secs = (us_now() - t0) / 1e6;
          per_s = (calc[0].sleeps - s0) / secs;
          waited_ms = (calc[0].waited_us - w0) / 1000.0 / secs;
          printf("  P2 fast forward, %.1f ms frames, %s: %.1f OS sleeps and %.1f ms waited a second, longest gap %ld ms\n",
                 frame_us[f] / 1000.0, fresh ? "port fresh (today's rule)" : "port settled", per_s, waited_ms, gap / 1000);
          if (fresh ? per_s < 40 || waited_ms < 80 : per_s > 25 || waited_ms > 30 || gap > 100000 + frame_us[f] + 10250)
          {
            static char what[120];
            snprintf(what, sizeof(what), "P2 fast forward, %ld us frames, fresh %d: %.1f sleeps, %.1f ms waited a second, "
                     "gap %ld us", frame_us[f], fresh, per_s, waited_ms, gap);
            g->what = what;
            g->ok = 0;
          }
        }
      calc[0].os_tick_us = 0;
    }
    /* R1: the pad's tick (nspire_usbpad_keys with a pad in, wanting the OS,
     * nspire_usbpad_tick_rule) in frames with no spare time, on both OS
     * ticks: a pad's reports come only while the OS runs, so the longest
     * gap between sleeps is its input's worst wait. Asserted: never more
     * than USBPAD_TICK_FORCE (33 ms) + a frame + a tick, and no more time
     * waited than the rule before P2 (forced every 33 ms). Printed for
     * comparison: P2 as first committed (near from 33 ms, forced at 66). */
    if (g->side == 0 && g->ok)
    {
      static const struct { long tick_us, frame_us; } runs[] = {
        { 10250, 4600 }, { 10250, 5700 }, { 10250, 16700 },   /* CX II: fast forward, full speed */
        { 2980, 4600 }, { 2980, 8300 }, { 2980, 16000 }, { 2980, 31000 },   /* CX: fast forward, 192, 132 MHz */
      };
      static const char *names[] = { "before P2", "P2 first", "now" };
      unsigned r;
      int rule;

      for (r = 0; r < sizeof(runs) / sizeof(runs[0]); r++)
      {
        double per_s[3], waited[3];
        long gap[3];

        calc[0].os_tick_us = (int)runs[r].tick_us;
        for (rule = 0; rule < 3; rule++)
        {
          long s0 = calc[0].sleeps, w0 = calc[0].waited_us, t0, wake;
          int frames;

          calc[0].last_wake = us_now();
          t0 = wake = calc[0].last_wake;
          gap[rule] = 0;
          for (frames = 0; frames < 20 * 1000000 / runs[r].frame_us; frames++)
          {
            uint32_t since = a_nspire_os_since_sleep();
            const uint32_t gap_ = 32768u / 30;

            if (rule == 0)
            {
              if (since >= gap_)
                a_nspire_os_sleep();
            }
            else if (rule == 1)
            {
              if (since >= 2 * gap_)
                a_nspire_os_sleep();
              else if (since >= gap_)
                a_nspire_os_sleep_near(a_nspire_os_near_wait(since, gap_, 2 * gap_));
            }
            else
            {
              int what = nspire_usbpad_tick_rule(since, 0);
              if (what == USBPAD_TICK_SLEEP)
                a_nspire_os_sleep();
              else if (what == USBPAD_TICK_NEAR)
                a_nspire_os_sleep_near(a_nspire_os_near_wait(since, USBPAD_TICK_FROM, USBPAD_TICK_FORCE));
            }
            if (calc[0].last_wake != wake)
            {
              if (calc[0].last_wake - wake > gap[rule])
                gap[rule] = calc[0].last_wake - wake;
              wake = calc[0].last_wake;
            }
            skew_us += runs[r].frame_us;
          }
          per_s[rule] = (calc[0].sleeps - s0) / ((us_now() - t0) / 1e6);
          waited[rule] = (calc[0].waited_us - w0) / 1000.0 / ((us_now() - t0) / 1e6);
        }
        printf("  pad's tick, OS tick %.2f ms, %.1f ms frames:", runs[r].tick_us / 1000.0, runs[r].frame_us / 1000.0);
        for (rule = 0; rule < 3; rule++)
          printf(" %s %.1f sleeps %.1f ms/s gap %ld ms;", names[rule], per_s[rule], waited[rule], gap[rule] / 1000);
        printf("\n");
        if (gap[2] > 33334 + runs[r].frame_us + runs[r].tick_us || waited[2] > waited[0] + 0.5)
        {
          static char what[160];
          snprintf(what, sizeof(what), "pad's tick, OS tick %ld us, %ld us frames: gap %ld us, %.1f ms/s waited (before P2 %.1f)",
                   runs[r].tick_us, runs[r].frame_us, gap[2], waited[2], waited[0]);
          g->what = what;
          g->ok = 0;
        }
      }
      calc[0].os_tick_us = 0;
    }
    goto out;
  }
  if (g->kind == NOT_BACK)
  {
    /* b links on its own; a takes its connection but never links back. */
    if (g->side == 0)
    {
      long start = ms_now();
      while (ms_now() - start < 6000)
      {
        t->watch(0, 1, 1);
        usleep(1000);
        calc[0].frames++;
      }
      g->ok = 1;
      goto out;
    }
    if (!watch_until_there(g, t, 1, 15000))
      goto out;
    r = open_and_ready(g, t, 1);
    g->ok = r < 0;
    if (r > 0)
      g->what = "ready on its own though the other never connected back";
    CLOSE_CHECKED(t, g);
    goto out;
  }
  if (g->watch && !watch_until_there(g, t, 1, 15000))
    goto out;
  if (open_and_ready(g, t, g->kind == AUTO_FAST) <= 0)
    goto out;
  if (g->kind == BAD_LENGTH)
  {
    /* R6: side a's first message comes to b with a length word over any
     * message's: b's receive fails at once (not after the 15 s give-up),
     * saying why. */
    uint8_t m[40], in[700];
    const char *(*error_of)(void) = g->side ? b_link_cable_error : a_link_cable_error;
    long t0;
    if (g->side == 0)
    {
      memset(m, 0x33, sizeof(m));
      bad_length_next = 1;
      t->send(m, sizeof(m));
      t->receive(in, sizeof(in), 1500);
      g->ok = 1;
    }
    else
    {
      int r = 0;
      for (t0 = ms_now(); ms_now() - t0 < 5000 && (r = t->receive(in, sizeof(in), 20)) >= 0;)
        ;
      g->ok = r < 0 && ms_now() - t0 < 3000 && error_of() && strstr(error_of(), "damaged");
      if (!g->ok)
        g->what = r < 0 ? "failed, but slowly or without saying why" : "the bad length word stalled the link";
    }
    CLOSE_CHECKED(t, g);
    goto out;
  }
  if (g->kind == MENU)
  {
    /* LNK-2: side a says it's in its menu (nspire_link_menu: the message,
     * then link_cable_push) and stays in the menu, which never sleeps into
     * the OS (the CPU held, so the service's task can't run on its own):
     * b must have the message within 1 s, or after 15 s it gives up on a. */
    uint8_t m[8] = { 3, 1, 0, 0, 0, 0, 0, 0 }, in[16];
    long t0 = ms_now();
    if (g->side == 0)
    {
      g->ok = t->send(m, sizeof(m)) >= 0;
      a_link_cable_push();
      while (ms_now() - t0 < 1500)
        ;
      if (!g->ok)
        g->what = "send failed";
    }
    else
    {
      int r = 0;
      while (ms_now() - t0 < 1200 && (r = t->receive(in, sizeof(in), 20)) == 0)
        ;
      g->ok = r == 8 && !memcmp(in, m, 8);
      if (!g->ok)
        g->what = "the other's menu message stayed in its queue while it was in its menu";
      while (ms_now() - t0 < 1600)
        t->receive(in, sizeof(in), 20);
    }
    CLOSE_CHECKED(t, g);
    goto out;
  }
  if (IS_FAST(g->kind) && !go_fast(g, t))
    goto out;
  if (g->kind == GARBLE)
  {
    g->ok = garbled_exchange(g, t);
    CLOSE_CHECKED(t, g);
    goto out;
  }
  r = exchange(g, t);
  if (r < 0 && (g->kind == PULL || g->kind == FAST_PULL) && pulled)
  {
    /* The cable came out: closed, and linked again once it's back. */
    CLOSE_CHECKED(t, g);
    g->what = "";
    if (!watch_until_there(g, t, 1, 15000) || open_and_ready(g, t, 0) <= 0)
      goto out;
    if (g->kind == FAST_PULL && !go_fast(g, t))
      goto out;
    r = exchange(g, t);
    if (r < 0)
      g->what = "the link after the cable came back broke";
  }
  if (r <= 0)
    goto out;
  /* LINK2: a linked frame gives the OS its spare time exactly while NavNet
   * carries the link (after a stall's or the arbiter's fall back too). */
  if (IS_FAST(g->kind) && t->linked_os() != !t->fast_up())
  {
    g->what = t->fast_up() ? "the fast transport up, yet a linked frame would give the OS its spare time" :
                             "over NavNet (after a fall back), yet a linked frame would give the OS no time";
    goto out;
  }
  if (g->kind == FAST_PACE && g->side == 1 && !pace_case(g, t))
    goto out;
  if (g->kind == FAST && fake_timer_on && !svc_read_case(g, t))
    goto out;
  if (g->kind == HOLD)
  {
    /* R2: a frame's small message waits to go with the next (the real
     * NavNet's write takes ~40 ms whatever its size): 40 messages of 8
     * bytes, a frame (17 ms) apart with the frame's end each, are far fewer
     * writes on this side's own channel, and all of them come, in order. */
    uint8_t m[8], in[16];
    int got = 0;
    if (g->side == 0)
    {
      int before = game_writes[0];
      for (i = 0; i < 40; i++)
      {
        memset(m, i, sizeof(m));
        if (t->send(m, sizeof(m)) < 0)
        {
          g->what = "send failed";
          goto out;
        }
        a_link_cable_frame_done(4);
        {
          int side = cpu_give();
          usleep(17000);
          cpu_take(side);
        }
      }
      a_link_cable_flush();
      before = game_writes[0] - before;
      if (before > 14)
      {
        static char what[80];
        snprintf(what, sizeof(what), "40 small messages took %d writes on its own channel", before);
        g->what = what;
        goto out;
      }
      g->ok = 1;
      goto out;
    }
    while (got < 40)
    {
      r = t->receive(in, sizeof(in), 20);
      if (r < 0)
      {
        g->what = "receive failed";
        goto out;
      }
      if (r)
      {
        if (r != 8 || in[0] != got)
        {
          g->what = "a held message came wrong";
          goto out;
        }
        got++;
      }
    }
    g->ok = 1;
    goto out;
  }
  if (g->kind == STUCK)
  {
    if (g->side == 0)
    {
      /* More than the other takes: the service's task is in a write as
       * this side closes. */
      uint8_t junk[200];
      memset(junk, 0x5A, sizeof(junk));
      for (i = 0; i < 40; i++)
        t->send(junk, sizeof(junk));
      usleep(30000);
      CLOSE_CHECKED(t, g);
      g->ok = 1;
      goto out;
    }
    /* b: a game frame of 1.5 s, its CPU held: its callbacks read nothing. */
    usleep(1500000);
    CLOSE_CHECKED(t, g);
    g->ok = 1;
    goto out;
  }
  if (g->kind == QUEUED_AT_CLOSE && g->side == 0)
  {
    /* Closes with messages queued for the service's task to send. */
    uint8_t junk[300];
    memset(junk, 0xA5, sizeof(junk));
    for (i = 0; i < 6; i++)
      t->send(junk, sizeof(junk));
    CLOSE_CHECKED(t, g);
    g->ok = 1;
    goto out;
  }
  /* Let the other finish taking this side's before unlinking. */
  {
    int side = cpu_give();
    usleep(100000);
    cpu_take(side);
  }
  CLOSE_CHECKED(t, g);
  g->ok = 1;
out:
  game_gone[g->side] = 1;
  cpu_give();
  return NULL;
}

static void run_case(const char *name, int a_sees, int b_sees, int shorts, int late, int a_dead, int os,
                     int wus, int watch, int b_later, int send_max_ms, int kind)
{
  pthread_t timer_th;
  int round, s;
  long start = ms_now();

  memset(calc, 0, sizeof(calc));
  calc[0].sees_other = a_sees;
  calc[1].sees_other = b_sees;
  calc[0].late_callbacks = calc[1].late_callbacks = late;
  calc[0].cpu_model = calc[1].cpu_model = late && os;
  calc[0].timer_dead = a_dead;
  if (kind == HOST_TICKS)
    calc[0].need_sleeps = calc[1].need_sleeps = 6;
  calc[0].os_sleep = calc[1].os_sleep = os;
  connect_now = kind == OLD_FAKE;
  write_us = wus;
  window = a_sees && b_sees ? 9 : 256;
  watching = watch;
  short_reads = shorts;
  pulled = 0;
  usb_fast_sim_setup(IS_FAST(kind) ? SIM_CX2_HOST : 0, kind == FAST_CX2DEV ? SIM_CX2_DEVICE : IS_FAST(kind) ? SIM_CX_DEVICE : 0);
  if (kind == FAST)
  {
    fake_timer = ~(uint32_t)us_now();
    fake_timer_on = 1;
    pthread_create(&timer_th, NULL, fake_timer_run, NULL);
    a_nspire_raw_timer = b_nspire_raw_timer = &fake_timer;
  }
  for (round = 0; round < (kind == HELD_QUIT || kind == NOT_BACK || kind == WATCH_SLEEPS ? 1 : 2); round++)
  {
    pthread_t th[2];
    garbling = kind == GARBLE && !round;
    int k2 = round ? (IS_FAST(kind) && kind != FAST_OLD && kind != FAST_CX2DEV ? FAST : IS_FAST(kind) ? kind : PLAIN) : kind;
    struct game g[2] = { { 0, 0, "", watch, 0, k2, 0, 0 },
                         { 1, 0, "", watch, round ? 0 : b_later, k2, 0, 0 } };
    usb_fast_sim_fault(round ? SIM_FAULT_NONE : kind == FAST_NOANSWER ? SIM_FAULT_NOANSWER : kind == FAST_STALL ? SIM_FAULT_STALL :
                       kind == FAST_VIC_ON ? SIM_FAULT_VIC_ON : kind == FAST_SLOW_TAKE ? SIM_FAULT_SLOW_TAKE :
                       kind == FAST_SLOW_RELEASE ? SIM_FAULT_SLOW_RELEASE :
                       kind == FAST_SLOW_RESTART ? SIM_FAULT_SLOW_RESTART : SIM_FAULT_NONE,
                       kind == FAST_STALL || kind == FAST_VIC_ON ? 40 : 0);
    game_gone[0] = game_gone[1] = 0;
    for (s = 0; s < 2; s++)
      pthread_create(&th[s], NULL, game, &g[s]);
    if ((kind == PULL || kind == FAST_PULL) && !round)
    {
      /* Mid-link: the cable out for 0.3 s. */
      long wait_from = ms_now();
      while ((got_count[0] < 30 || got_count[1] < 30) && ms_now() - wait_from < 20000)
        usleep(1000);
      pulled = 1;
      if (kind == FAST_PULL)
        usb_fast_sim_cable(0);
      pull_cable(300);
      if (kind == FAST_PULL)
        usb_fast_sim_cable(1);
    }
    for (s = 0; s < 2; s++)
      pthread_join(th[s], NULL);
    /* The release's stop that wouldn't: a's queue heads parked, still its
     * (the next link's start gives them back first). */
    if (kind == FAST_SLOW_RELEASE && !round && !a_link_cable_fast_holds())
      FAIL("%s: a's schedule wouldn't stop at the release, yet a holds nothing", name);
    if (kind == FAST_SLOW_RESTART && !round)
    {
      /* nspire_link_fast.c's SCHED_POLLS 200000, SCHED_AGAIN_POLLS 1000;
       * the release's log line reads USBSTS twice more. */
      long n = usb_fast_sim_restart_reads(0);
      if (n < 200000 || n > 200000 + 1000 + 8)
        FAIL("%s: the release read USBSTS %ld times after starting the OS's schedule again (200000-201008)", name, n);
      else if (verbose)
        printf("  the release read USBSTS %ld times after its start\n", n);
      if (a_link_cable_fast_holds())
        FAIL("%s: a still holds the controller after a release whose start only lagged", name);
    }
    if (IS_FAST(kind) && verbose)
      printf("  link %d: a message's round trip %ld us (a), %ld us (b)\n", round + 1, g[0].rt_us, g[1].rt_us);
    if (IS_FAST(kind))
      printf("  %s, link %d: round trip %ld us\n", name, round + 1, g[0].rt_us);
    for (s = 0; s < 2; s++)
      if (!g[s].ok)
        FAIL("%s, link %d, side %c: %s", name, round + 1, 'a' + s, g[s].what);
      else if (k2 == FAST && g[s].rt_us > 20000)
        FAIL("%s, link %d, side %c: a round trip took %ld us on the fast transport (20 ms at most)", name, round + 1,
             'a' + s, g[s].rt_us);
      else if (send_max_ms && g[s].send_max_us > send_max_ms * 1000L)
        FAIL("%s, link %d, side %c: a send held the game %ld us (the service's task should send)", name,
             round + 1, 'a' + s, g[s].send_max_us);
      else if ((kind == READY_FAST || kind == AUTO_FAST) && !round && g[s].ready_ms > READY_FAST_MS)
        FAIL("%s, link 1, side %c: ready %ld ms after open (more than %d: the other's callback waited for data?)", name,
             'a' + s, g[s].ready_ms, READY_FAST_MS);
      else if (verbose && !round)
        printf("  side %c ready %ld ms after open\n", 'a' + s, g[s].ready_ms);
  }
  for (s = 0; s < 2; s++)
  {
    game_side = s;
    cpu_take(s);
    sides[s].shutdown();
    if (calc[s].os_ran_unchecked)
      calc[s].unchecked_at_close++;
    cpu_give();
  }
  if (calc[0].unchecked_at_close || calc[1].unchecked_at_close)
    FAIL("%s: a close or shutdown returned with no check after the OS's time in Disconnect/StopService (%d, %d)",
         name, calc[0].unchecked_at_close, calc[1].unchecked_at_close);
  game_side = -1;
  if (fake_timer_on)
  {
    a_nspire_raw_timer = b_nspire_raw_timer = NULL;
    fake_timer_on = 0;
    pthread_join(timer_th, NULL);
  }
  if (IS_FAST(kind))
  {
    const char *why = usb_fast_sim_restored();
    if (why)
      FAIL("%s: the USB controllers weren't put back: %s", name, why);
  }
  if (calc[0].registered || calc[1].registered || calc[0].callbacks || calc[1].callbacks)
    FAIL("%s: after closing, services %d %d, callbacks %d %d", name, calc[0].registered,
         calc[1].registered, calc[0].callbacks, calc[1].callbacks);
  if (calc[0].stop_with_callbacks || calc[1].stop_with_callbacks)
    FAIL("%s: StopService with a callback still running (%d, %d)", name, calc[0].stop_with_callbacks,
         calc[1].stop_with_callbacks);
  if (calc[0].disconnect_while_writing || calc[1].disconnect_while_writing)
    FAIL("%s: a channel Disconnected with a write on it (%d, %d)", name, calc[0].disconnect_while_writing,
         calc[1].disconnect_while_writing);
  printf("%-46s %s (%.1f s)\n", name, failures ? "..." : "ok", (ms_now() - start) / 1000.0);
}

static const struct
{
  const char *name;
  int a, b, shorts, late, a_dead, os, write_us, watch, b_later, send_max_ms, kind;
} cases[] = {
  { "two channels, short reads", 1, 1, 1, 0, 0, 0, 50, 0, 0, 0, PLAIN },
  { "two channels, long reads (62 s, here 1.5)", 1, 1, 0, 0, 0, 0, 50, 0, 0, 0, PLAIN },
  { "one channel: only a finds b, short reads", 1, 0, 1, 0, 0, 0, 50, 0, 0, 0, PLAIN },
  { "one channel: only b finds a, short reads", 0, 1, 1, 0, 0, 0, 50, 0, 0, 0, PLAIN },
  { "callbacks start when the game blocks", 1, 1, 1, 1, 0, 0, 50, 0, 0, 0, PLAIN },
  { "that, and a's timer doesn't run", 1, 1, 1, 1, 1, 0, 50, 0, 0, 0, PLAIN },
  { "that, one channel: only a finds b", 1, 0, 1, 1, 1, 0, 50, 0, 0, 0, PLAIN },
  { "that, one channel: only b finds a", 0, 1, 1, 1, 1, 0, 50, 0, 0, 0, PLAIN },
  { "OS sleep, 8 ms writes: the service's task sends", 1, 1, 1, 1, 0, 1, 8000, 0, 0, 3, PLAIN },
  { "that, and a's timer doesn't run", 1, 1, 1, 1, 1, 1, 8000, 0, 0, 3, PLAIN },
  { "watching: b's gpSP starts 1.5 s after a's", 1, 1, 1, 1, 0, 1, 2000, 1, 1500, 3, PLAIN },
  { "watching, only b finds a, a's timer dead", 0, 1, 1, 1, 1, 1, 2000, 1, 1500, 0, PLAIN },
  { "the cable out mid-link and back: links again", 1, 1, 1, 1, 0, 1, 2000, 1, 0, 0, PULL },
  { "closing with a write the other doesn't take", 1, 1, 1, 1, 0, 1, 2000, 0, 0, 0, STUCK },
  { "watching, one channel: a closes with sends queued", 0, 1, 1, 1, 0, 1, 2000, 1, 0, 0, QUEUED_AT_CLOSE },
  { "gpSP closes holding the other's connection", 1, 1, 1, 1, 0, 1, 2000, 1, 0, 0, HELD_QUIT },
  { "on its own: the other never connects back", 1, 1, 1, 1, 0, 1, 2000, 1, 0, 0, NOT_BACK },
  { "watching: the OS's tick each frame if plugged in", 0, 0, 1, 0, 0, 1, 50, 1, 0, 0, WATCH_SLEEPS },
  { "one channel, a's small messages go together", 1, 0, 1, 0, 0, 0, 50, 0, 0, 0, HOLD },
  { "picked Link: the OS enumerates only with ticks", 1, 1, 1, 0, 0, 1, 2000, 0, 0, 0, HOST_TICKS },
  { "fast transport: up, round trips under 20 ms", 1, 1, 1, 1, 0, 1, 2000, 0, 0, 0, FAST },
  { "fast: that, and the host's timer doesn't run", 1, 1, 1, 1, 1, 1, 2000, 0, 0, 0, FAST },
  { "fast: the other gpSP is an older build", 1, 1, 1, 1, 0, 1, 2000, 0, 0, 0, FAST_OLD },
  { "fast: a CX II at the device end can't", 1, 1, 1, 1, 0, 1, 2000, 0, 0, 0, FAST_CX2DEV },
  { "fast: the device never answers: NavNet", 1, 1, 1, 1, 0, 1, 2000, 0, 0, 0, FAST_NOANSWER },
  { "fast: a stall mid-link: back to NavNet, no loss", 1, 1, 1, 1, 0, 1, 2000, 0, 0, 0, FAST_STALL },
  { "fast: the cable out mid-link and back", 1, 1, 1, 1, 0, 1, 2000, 1, 0, 0, FAST_PULL },
  { "fast: the arbiter says no at a's take-over", 1, 1, 1, 1, 0, 1, 2000, 0, 0, 0, FAST_REFUSED },
  { "fast: the arbiter makes a let go mid-link", 1, 1, 1, 1, 0, 1, 2000, 0, 0, 0, FAST_DROP },
  { "fast: the OS turns the USB interrupt on mid-link", 1, 1, 1, 1, 0, 1, 2000, 0, 0, 0, FAST_VIC_ON },
  { "fast: the schedule stops slowly at the take-over", 1, 1, 1, 1, 0, 1, 2000, 0, 0, 0, FAST_SLOW_TAKE },
  { "fast: the schedule won't stop at the release", 1, 1, 1, 1, 0, 1, 2000, 0, 0, 0, FAST_SLOW_RELEASE },
  { "fast: one channel, READY's write waits for a's ack", 0, 1, 1, 1, 0, 1, 2000, 0, 0, 0, FAST_ACK },
  { "fast: b's linked frames give the OS no time", 1, 1, 1, 1, 0, 1, 2000, 0, 0, 0, FAST_PACE },
  { "fast: the OS's schedule lags its start at the release", 1, 1, 1, 1, 0, 1, 2000, 0, 0, 0, FAST_SLOW_RESTART },
  { "R6: two channels, packets damaged at random", 1, 1, 1, 0, 0, 0, 50, 0, 0, 0, GARBLE },
  { "R6: one channel, packets damaged at random", 1, 0, 1, 0, 0, 0, 50, 0, 0, 0, GARBLE },
  { "R6: a length word over any message's ends the link at once", 1, 1, 1, 0, 0, 0, 50, 0, 0, 0, BAD_LENGTH },
  { "R6: that, one channel", 1, 0, 1, 0, 0, 0, 50, 0, 0, 0, BAD_LENGTH },
  { "LNK-2: OS sleep, a's menu message goes though it never sleeps", 1, 1, 1, 1, 0, 1, 8000, 0, 0, 0, MENU },
  { "DETECT: picked Link, callbacks at the first data: ready < 1 s", 1, 1, 1, 1, 0, 1, 2000, 0, 0, 0, READY_FAST },
  { "DETECT: on their own after the watch: both ways < 1 s", 1, 1, 1, 1, 0, 1, 2000, 1, 0, 0, AUTO_FAST },
  { "DETECT: that, b's gpSP starts 1.5 s after a's", 1, 1, 1, 1, 0, 1, 2000, 1, 1500, 0, AUTO_FAST },
  { "the old fake (a callback at the Connect) still links", 1, 1, 1, 1, 0, 1, 2000, 0, 0, 0, OLD_FAKE },
};

int main(int argc, char **argv)
{
  verbose = argc > 1 && !strcmp(argv[1], "-v");
  /* The resolved read is per process: these run in separate processes,
   * each given 2 minutes (a hang ends it). */
  if (argc > 2)
  {
    int c = atoi(argv[2]);
    alarm(120);
    run_case(cases[c].name, cases[c].a, cases[c].b, cases[c].shorts, cases[c].late, cases[c].a_dead,
             cases[c].os, cases[c].write_us, cases[c].watch, cases[c].b_later, cases[c].send_max_ms, cases[c].kind);
    /* Both sides shut down: nothing holds a channel now. Callbacks that
     * never started (pending) go too. */
    {
      int i, k;
      pthread_mutex_lock(&lock);
      for (i = 0; i < channel_count; i++)
      {
        free(channel_ends[i]);
        free(channels[i]);
      }
      channel_count = 0;
      for (k = 0; k < 2; k++)
        for (i = 0; i < calc[k].pending_count; i++)
          free(calc[k].pending[i]);
      pthread_mutex_unlock(&lock);
    }
    return failures ? 1 : 0;
  }
  {
    int c, bad = 0;
    char cmd[600];
    for (c = 0; c < (int)(sizeof(cases) / sizeof(cases[0])); c++)
    {
      snprintf(cmd, sizeof(cmd), "%s %s %d", argv[0], verbose ? "-v" : "-q", c);
      bad |= system(cmd) != 0;
    }
    printf(bad ? "link_usb: FAILED\n" : "link_usb: all passed\n");
    return bad;
  }
}
