/* The PC's link cable (nspire_link.h): a socket between two gpsp-hosts.
 *
 * Both start with --link PATH; player 1 (or the first to start) waits for
 * the other, so
 * the cable is there from the start and each links at the frame its script
 * says ("link"), the same every run. --player 1 or 2 picks the parent
 * (player 1); without it, the process ids do.
 *
 * Waits for a message wait until one comes (or 20 s): on the PC the other
 * side is only ever busy, never gone without the socket closing, and waiting
 * that way never runs nspire_link's waiting screen, which would read keys and
 * so move the virtual time differently each run.
 *
 * The link's speed, in virtual time (host_hw.c): every message carries the
 * time it was sent at, and comes --link-delay MS after it (one way). A wait
 * for it lasts until then; a look (wait_ms 0) doesn't see it before then,
 * and it waits here meanwhile. Each message also costs the calculator's
 * time to send and to receive (the script's cost send / receive lines),
 * as NavNet's work does on the calculator.
 *
 * What that can't do: a look finds only what the other gpsp-host has sent
 * already, and it runs at its own pace, so a message whose time has come
 * can still be on its way and come at a later look or wait. nspire_link.c
 * hands the core what came only at points that don't depend on that (the
 * frames' starts, the cable's horizon and transfers), so the games run the
 * same frame for frame at any delay; only when a message's cost is counted
 * can move. The waiting screen's time isn't counted (on the calculator it
 * comes after 0.4 s of waiting).
 *
 * The report's link lines count from linking, or the script's "measure",
 * to unlinking or "measure stop": the cable's transfers and their sum
 * (nspire_link.h's: both GBAs' cycles and words), messages sent and
 * received, the game's frames and how many were drawn, the virtual seconds
 * from the first of those frames to the last, game frames a second over
 * them and the fewest in any one second, and the time waiting on the link
 * and the messages' cost. Also the status at the first frame linked
 * (link_linked), as the one at the end ("link") is often the other side
 * having quit first, the protocol the two spoke then (link_protocol), and
 * the hellos this side sent (link_hellos_sent: a tie or a protocol-1 side
 * makes more than one), and how many times it linked (link_count).
 *
 * --link-peer legacy (host_link_setup's legacy_peer): the other gpsp-host is
 * e24876d's (the calculators' first link build), whose socket carries the
 * messages bare, without the time they were sent: so they go to it bare,
 * and what comes from it is due as it comes (no delay). The messages'
 * cost still counts on this side.
 *
 * --link-nonce-tie N (host_link_nonce_tie): the first N nonces asked for
 * are one number, the same on both gpsp-hosts given it, so N hellos tie
 * and nspire_link.c rolls again; then they're 3 - player as without it.
 *
 * --link-auto (host_link_auto): the calculators' watch for the cable
 * (link_cable_watch) and the connections under it, as NavNet's: a side
 * that may connect (or links) connects to the other's service, which the
 * other then holds (link_cable_watch says 2 there too, even while it's
 * held: "the other connects anew"); a close lets go of both, and what
 * the other sent until it connects again belonged to the connection let
 * go of and is dropped. They go over the socket as control packets
 * (CTRL_*, never e24876d's). Linking on its own (link_cable_open's
 * automatic) is ready only once the other side connected too, given up
 * after ONE_CHANNEL_S (the other's gpSP took the connection but doesn't
 * link back: its menu, its file list).
 *
 * --link-os (host_link_os): the calculators' NavNet timing. What's sent is
 * queued and goes (its time stamped then) only when this side lets the
 * OS run: an OS sleep (nspire_os_sleep: frame pacing, link_cable_frame_done)
 * or a wait for a message; and a wait that waits longer than the
 * calculator's spin (SPIN_S) ends at the OS's next tick after the message
 * is due, as the calculator's pause does. So a game with no spare time
 * sends in bursts, as on the calculator.
 *
 * --link-trace FILE (host_link_trace): every transfer on the cable as wired,
 * a line each: player 1's frame, the cycle it started at (counted from
 * linking), this GBA's SIOCNT then, and player 1's and player 2's words
 * (hex), as the two GBAs' SIOMULTI0 and SIOMULTI1 get them. Both players can
 * write one; they're the same but for SIOCNT. */

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "nspire_link.h"

#define LONGEST_WAIT_MS 20000
/* nspire_link.c's messages are up to 2 KB; each goes with its time. */
#define MSG_BYTES       4096
#define STAMP           sizeof(double)
/* A control packet: STAMP + 1 bytes (a message is at least 8). */
#define CTRL_UNPLUG     3   /* S9: the cable's pulled (both ends see it) */
#define CTRL_PLUG       4
#define CTRL_FAST_BYE   5   /* LINK2: the fast transport's BYE / FAIL: the other side is on NavNet */
#define CTRL_CONNECT    1
#define CTRL_DISCONNECT 2
#define ONE_CHANNEL_S   4.0
/* --link-os: the calculator's spin before it sleeps (nspire_link_usb.c's
 * SPIN_TICKS), the OS's tick (host_platform.c's), sends queued. */
#define SPIN_S          0.001
#define OS_TICK_S       0.01
#define QUEUED          256

double host_hw_now(void);
void host_hw_spend(double seconds);
double host_hw_message_cost(int received);
extern uint16_t io_registers[];   /* the GBA's (gba_memory.h) */

static int fd = -1;
static int player;
static char error[96];
static double delay;                /* seconds a message takes to come */
static int legacy;                  /* --link-peer legacy: bare messages */
static int ties;                    /* --link-nonce-tie: nonces still to tie */

/* --link-auto's connections: this side's to the other's service, the
 * other's to this one's (held), and whether what comes is a connection's
 * let go of (dropped until the other's next CTRL_CONNECT). */
int host_link_auto;
static int me_connected, peer_connected, dropping;
static int open_automatic;
static double opened_at;

/* --link-os: messages sent but not gone yet. */
int host_link_os;
static uint8_t *queued[QUEUED];
static unsigned queued_len[QUEUED];
static int queued_n;
/* --link-write MS (R2, the real logs): a write on this side's own channel
 * holds the game up this long (NavNet's write is acknowledged by the other
 * side: ~40 ms on the calculators); as nspire_link_usb.c does, small
 * messages are held and go in one write (a piece: 254 bytes) when the next
 * doesn't fit, at a wait, within the frames link_cable_frame_done is given,
 * or at link_cable_flush. --link-nohold: each message its own write (the
 * build before R2). */
double host_link_write_ms;
/* --link-slow MS N: every Nth write takes MS (the real round trips came in
 * two groups: 84-120 ms and 257-264 ms, 90% at 264). */
double host_link_slow_ms;
int host_link_slow_every;
static unsigned writes_made;
int host_link_nohold;
/* --link-hold-time: R2's hold, in 60ths of a second (R3 counts frames). */
int host_link_hold_time;
static unsigned held_bytes;
static double held_since;
static unsigned held_frames;   /* R3: frame ends since the first was held */

/* A message here already, whose time to come hasn't: when it was sent
 * (STAMP bytes), and it. */
static uint8_t held[STAMP + MSG_BYTES];
static int held_len = -1;
static double held_sent;

/* The report's (host_link_report): whether it's counting (measure stop
 * stops it), and whether a linked frame has ended since it started. */
static int measuring = 1, counting;
static double *frame_times;         /* the frames' ends, from the first */
static long ends, ends_size, drawn;
static long sent, received, bytes_sent, bytes_received;
static double waited, spent;
static uint32_t transfers_before, transfers, sum_before, sum;

/* --link-trace: the file, the transfer started and whether it's answered
 * (from nspire_link.c's MSG_XFER: frame, cycle, player 1's word), and
 * player 2's promise not yet used (MSG_PROMISE: from, until, word). */
static FILE *trace;
static uint32_t trace_frame, trace_word, promise_word;
static uint64_t trace_cycle, promise_from, promise_until;
static unsigned trace_siocnt;
static int trace_open, promise_open;
static char linked_as[128];
static int linked_protocol;
static int links, was_linked;   /* times linked; linked at the last frame */
static long hellos;                 /* hellos sent (MSG_HELLO 1, MSG_HELLO_EXT 8) */

/* gpsp-host's --link PATH: connects to the other gpsp-host, or waits for it
 * (up to a minute). --link-delay: how long a message takes, one way. */
int host_link_setup(const char *path, int which_player, double delay_ms, int legacy_peer)
{
  struct sockaddr_un addr;
  int listener, i;

  player = which_player;
  legacy = legacy_peer;
  delay = legacy ? 0 : delay_ms / 1000.0;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(addr.sun_path))
    return -1;
  strcpy(addr.sun_path, path);

  /* Player 2 (or the second to start) connects; player 1 waits, so two
   * started at once don't both wait. */
  for (i = 0; i < (player == 2 ? 600 : 1); i++)
  {
    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0)
      return -1;
    if (player != 1 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
      return 0;
    close(fd);
    fd = -1;
    if (player == 2)
      usleep(100000);
  }
  if (player == 2)
    return -1;

  /* The first one here: wait for the other. A socket file left by an
   * earlier run is in the way. */
  listener = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  unlink(path);
  if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(listener, 1) != 0)
  {
    fd = -1;
    return -1;
  }
  for (i = 0; i < 600; i++)
  {
    struct pollfd p = { listener, POLLIN, 0 };
    if (poll(&p, 1, 100) == 1)
      break;
  }
  fd = i < 600 ? accept(listener, NULL, NULL) : -1;
  close(listener);
  unlink(path);
  return fd >= 0 ? 0 : -1;
}

/* gpsp-host's --link-trace FILE. */
int host_link_trace(const char *path)
{
  trace = fopen(path, "w");
  return trace ? 0 : -1;
}

static uint32_t le32(const uint8_t *p)
{
  return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void trace_line(uint32_t word)
{
  fprintf(trace, "%u %llu %04x %04x %04x\n", (unsigned)trace_frame, (unsigned long long)trace_cycle,
          trace_siocnt, (unsigned)trace_word, (unsigned)(word & 0xFFFF));
  trace_open = 0;
}

/* The transfer started takes player 2's promise, as nspire_link.c's
 * use_promise: one made by its cycle is used up, and has its word if the
 * transfer starts before the promise's end (else a reply comes). */
static void trace_promise(void)
{
  if (!trace_open || !promise_open || promise_from > trace_cycle)
    return;
  promise_open = 0;
  if (trace_cycle < promise_until)
    trace_line(promise_word);
}

/* A message sent or received: a transfer's start (MSG_XFER, 5: frame,
 * cycle, word) is kept, and its answer (MSG_REPLY, 6: player 2's word) or
 * player 2's promise (MSG_PROMISE, 7) makes the line. */
static void trace_message(const uint8_t *msg, int len, int received)
{
  if (len >= 8 + 16 && msg[0] == 5)
  {
    trace_frame = le32(msg + 4);
    trace_cycle = le32(msg + 8) | (uint64_t)le32(msg + 12) << 32;
    trace_word = le32(msg + 20) & 0xFFFF;
    trace_siocnt = io_registers[0x94];   /* REG_SIOCNT */
    trace_open = 1;
    trace_promise();
  }
  else if (len >= 8 + 4 && msg[0] == 6 && trace_open)
  {
    if (!received)
      trace_siocnt = io_registers[0x94];
    trace_line(le32(msg + 8));
  }
  else if (len >= 8 + 20 && msg[0] == 7)
  {
    promise_from = le32(msg + 8) | (uint64_t)le32(msg + 12) << 32;
    promise_until = le32(msg + 16) | (uint64_t)le32(msg + 20) << 32;
    promise_word = le32(msg + 24);
    promise_open = 1;
    if (!received)
      trace_siocnt = io_registers[0x94];
    trace_promise();
  }
}

static int take(int wait_ms);

/* A control packet to the other gpsp-host (not to e24876d's). */
static void send_control(int what)
{
  uint8_t packet[STAMP + 1];
  double at = host_hw_now();

  if (fd < 0 || legacy)
    return;
  memcpy(packet, &at, STAMP);
  packet[STAMP] = (uint8_t)what;
  send(fd, packet, sizeof(packet), MSG_NOSIGNAL);
}

/* This side connects to the other's service (once a connection). */
static void connect_to_other(void)
{
  if (me_connected || legacy)
    return;
  me_connected = 1;
  send_control(CTRL_CONNECT);
}

/* The control packets the other sent so far (and a message, if one's
 * next, held: link_cable_receive's). */
static void take_controls(void)
{
  while (held_len < 0 && take(0) > 0)
    ;
}

int link_cable_open(int automatic)
{
  if (fd < 0)
  {
    snprintf(error, sizeof(error), "No link cable (gpsp-host --link)");
    return -1;
  }
  error[0] = 0;
  open_automatic = automatic;
  opened_at = host_hw_now();
  connect_to_other();
  return 0;
}

static int calc_plugged_in_play;
static int calc_listed(void);
static int pending_n;

int link_cable_ready(void)
{
  if (fd < 0)
    return -1;
  /* DETECT: picked with the cable plugged in during play: not before the
   * OS lists the other calculator (its looks are often then). */
  if (!open_automatic)
    return host_link_auto && calc_plugged_in_play && !calc_listed() ? 0 : 1;
  /* On its own: both ways, or given up on. */
  take_controls();
  if (!peer_connected && fd >= 0 && pending_n == 0 && held_len < 0)
  {
    /* DETECT: the other gpsp-host's connect is due about now (both connect
     * at the node's listing); it runs at its own pace in wall time, and
     * without a moment's wait here this side's virtual time ran up to 4 s
     * ahead of it under load and gave up (ONE_CHANNEL_S): a few ms of wall
     * time a frame for it, no virtual time. */
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, 20) > 0)
      take_controls();
  }
  if (peer_connected)
    return 1;
  if (host_hw_now() - opened_at >= ONE_CHANNEL_S)
  {
    snprintf(error, sizeof(error), "The other calculator isn't linking.");
    return -1;
  }
  return 0;
}

/* The calculator's cable watch: with --link-auto the other gpsp-host is on
 * the cable from the start (the scripts' "link" lines link otherwise). 2
 * once this side connected (it may) or the other did; else 1. */
int host_link_calc_preplug;   /* "pad calc preplug": the node is listed from t0 */
int host_link_calc_node;      /* "pad calc node": a NavNet node is listed, our service never answers */
int host_link_calc_out;       /* "pad calc out": the cable's pulled; "pad calc in" puts it back */
void host_usbpad_note(const char *text);

/* SEAM-2: the looks, as nspire_link_usb.c makes them (one at the first watch,
 * then every 1.5 s; DETECT: at once after a port change, link_cable_hurry,
 * then every LINK_HURRY_LOOK_MS for LINK_HURRY_MS); "pad calc delay S": the
 * other calculator's node is listed only S seconds after the cable came in
 * (t0, or "pad calc in"), and a watch sees it only at a look: until then the
 * cable shows nothing but the other's own connection to us.
 * DETECT: without "pad calc delay", the calculators' OS's time (the r5
 * hardware logs): a cable in from the start is listed CALC_NODE_START_S
 * after it (r2-r4: connected 570-578 ms after the port's first reading on a
 * CX CAS), one plugged in during play (either end's "pad calc in": the
 * cable is one) CALC_NODE_PLUG_S after (r5: the CX II's port came up at
 * 8504 ms, both calculators listed the other between 13.3 and 14.5 s). */
#define CALC_NODE_START_S 0.6
#define CALC_NODE_PLUG_S  5.0
double host_link_calc_delay = -1, host_link_calc_in_at;
int host_link_calc_device_end;   /* DETECT: this side has the cable's (device's) end; the other side's sim keeps the host's */
static unsigned sim_looks, sim_seen_on_look;
static double sim_look_at = -1, sim_hurry_at = -1;
static int sim_look_due;

static double calc_node_delay(void)
{
  if (host_link_calc_delay >= 0)
    return host_link_calc_delay;
  return calc_plugged_in_play ? CALC_NODE_PLUG_S : CALC_NODE_START_S;
}

static int calc_listed(void)
{
  return host_link_calc_node ||
         (fd >= 0 && host_link_auto && !host_link_calc_out && host_hw_now() - host_link_calc_in_at >= calc_node_delay());
}

void link_cable_hurry(void)
{
  sim_hurry_at = host_hw_now();
  sim_look_due = 1;
}

unsigned link_cable_progress(void)
{
  return (sim_seen_on_look ? LINK_UP_NODE : 0) | (me_connected ? LINK_UP_OUT : 0) | (peer_connected ? LINK_UP_SVC : 0);
}

unsigned link_cable_looks(void) { return sim_looks; }

unsigned link_cable_look_now(void)
{
  sim_looks++;
  sim_look_at = host_hw_now();
  sim_seen_on_look = calc_listed();
  host_usbpad_note(sim_seen_on_look ? "look now: a node" : "look now: no node");
  return sim_seen_on_look;
}

int link_cable_watch(int may_connect, int plugged, int fresh)
{
  double period = 1.5;

  (void)plugged;
  (void)fresh;
  if (sim_hurry_at >= 0 && host_hw_now() - sim_hurry_at < LINK_HURRY_MS / 1000.0)
    period = LINK_HURRY_LOOK_MS / 1000.0;
  if (sim_look_at < 0 || sim_look_due || host_hw_now() - sim_look_at >= period)
  {
    sim_looks++;
    sim_look_at = host_hw_now();
    sim_seen_on_look = calc_listed();
    sim_look_due = 0;
  }
  if (calc_node_delay() > 0 && !host_link_calc_out && !sim_seen_on_look && !host_link_calc_node)
  {
    if (fd >= 0)
      take_controls();
    /* The other side's connection (its knock) comes whatever this side's
     * looks have listed yet: its callback holds it. */
    return fd >= 0 && host_link_auto && peer_connected ? 2 : 0;
  }
  if (host_link_calc_out)
  {
    /* The cable's out: what was in flight is lost, and the other side's
     * putting it back (a control packet) is read here. */
    held_len = -1;
    take_controls();
    return 0;
  }
  if (host_link_calc_node)
    return 1;
  if (host_link_calc_preplug && fd >= 0 && host_link_auto)
  {
    static int noted;

    if (!noted++)
      host_usbpad_note("calc preplug: node listed at the first watch, no callback");
    take_controls();
    if (may_connect)
      connect_to_other();
    return 2;
  }
  if (fd < 0 || !host_link_auto)
    return 0;
  take_controls();
  if (may_connect)
    connect_to_other();
  return me_connected || peer_connected ? 2 : 1;
}

/* The script's "pad calc out|in": the cable is one, the other calculator's
 * side sees it pulled and put back too. */
void host_usbpad_os_offer(void);

void host_link_cable(int out)
{
  host_link_calc_out = out;
  if (!out)
  {
    host_link_calc_in_at = host_hw_now();
    calc_plugged_in_play = 1;
    if (!host_link_calc_device_end)
      host_usbpad_os_offer();   /* at the host's end the OS offers the calculator to the drivers (r5: match 215 ms after) */
  }
  sim_seen_on_look = 0;
  send_control(out ? CTRL_UNPLUG : CTRL_PLUG);
}

void link_cable_unwatch(void)
{
  if (fd < 0 || (!me_connected && !peer_connected))
    return;
  link_cable_close();
}

/* --link-os: what's queued goes now, its time stamped now. */
static void send_queued(void)
{
  int i;

  for (i = 0; i < queued_n; i++)
  {
    uint8_t packet[STAMP + MSG_BYTES];
    double at = host_hw_now();
    memcpy(packet, &at, STAMP);
    memcpy(packet + STAMP, queued[i], queued_len[i]);
    send(fd, packet, STAMP + queued_len[i], MSG_NOSIGNAL);
    free(queued[i]);
  }
  queued_n = 0;
}

/* host_platform.c's nspire_os_sleep: the OS ran. */
void host_link_os_ran(void)
{
  if (fd >= 0 && !(host_link_write_ms > 0))
    send_queued();
}

uint32_t nspire_os_since_sleep(void);
void host_os_runs(void);   /* host_platform.c: the OS's keys model */

/* One write of what's held: the game's time, then it goes. */
static void direct_flush(void)
{
  if (!queued_n)
    return;
  writes_made++;
  host_os_runs();   /* R6: the write waits on the OS's tasks: its keypad's handled */
  host_hw_spend((host_link_slow_every && writes_made % host_link_slow_every == 0 ? host_link_slow_ms :
                 host_link_write_ms) / 1000.0);
  send_queued();
  held_bytes = 0;
}

void link_cable_flush(void)
{
  if (host_link_write_ms > 0)
    direct_flush();
}

/* LNK-2: --link-os: what's queued goes now (the calculator lets the OS
 * run until its task has sent it). */
void link_cable_push(void)
{
  link_cable_flush();
  if (host_link_os && fd >= 0)
  {
    host_os_runs();
    send_queued();
  }
}

void link_cable_frame_done(unsigned frames)
{
  int nspire_os_sleep(void);

  if (host_link_write_ms > 0)
  {
    if (queued_n)
      held_frames++;
    if (queued_n && frames && (host_link_hold_time ? host_hw_now() - held_since >= frames / 60.0 : held_frames >= frames))
      direct_flush();
    return;
  }

  if (host_link_os && queued_n && frames && nspire_os_since_sleep() >= frames * (32768 / 60))
    nspire_os_sleep();
}

/* gpsp-host's USB port (nspire_os.h): the socket's the cable; else what
 * the script's pad lines put on it (host_usbpad.c: host_usbpad_port, 1 an
 * adapter at the host's end, 2 the device's end, -1 a CX's controller with
 * its bus access off). Its bits, for a pad on it, are a CX II's FOTG210's
 * (bit 5: the ID pin, 1 the device's end). */
extern int host_usbpad_port;
uint32_t host_usb_reg_read(uint32_t addr);

/* S8: how often the port's registers are read (nspire_usb_plugged, the
 * role bits), for "pad stat". */
long host_usb_plugged_reads, host_usb_bits_reads;

int nspire_usb_plugged(void)
{
  host_usb_plugged_reads++;
  if (fd >= 0 && host_link_auto && host_link_calc_out)
  {
    held_len = -1;       /* the cable's out: the other side's putting it back is read here (a control) */
    take_controls();
  }
  if (fd >= 0 && host_link_auto && !host_link_calc_out)
    return 1;
  return host_usbpad_port < 0 ? -1 : host_usbpad_port ? 1 : 0;
}

int nspire_usb_running(void) { return nspire_usb_plugged() > 0; }
unsigned link_cable_last_nodes(void) { return 0; }

uint32_t nspire_usb_bits(void)
{
  host_usb_bits_reads++;
  if (fd >= 0 && host_link_auto && !host_link_calc_out)
    return host_link_calc_device_end ? 0x20000030u :   /* DETECT: "pad calc end device": the cable's end (r5's CX II: 20000030) */
           0x20000501u;   /* a CX II's, this end the host's (adapter), a session; PORTSC connected, enabled (r5: 2000050e) */
  if (host_usbpad_port <= 0)
    return 0;
  /* SEAM-5: with PORTSC's connected and enabled (bits 8 and 10), as
   * nspire_os.c's: a device leaving an adapter that stays changes them. */
  if (host_usbpad_port >= 3)
    return 0x10000000u | (host_usbpad_port == 4 ? 1u : 0) | (host_usb_reg_read(0xB0000184u) & 5) << 8;   /* a CX's OTGSC ID, bit 0 */
  return 0x20000000u | (host_usbpad_port == 2 ? 0x20u : (host_usb_reg_read(0xB0000030u) & 5) << 8);
}

/* PAD3: the raw words (nspire_os.h nspire_usb_raw) from the simulated
 * registers (host_usbpad.c). The other calculator at this end's cable: a
 * CX II host end with it connected and enabled, or the cable's device end. */
int nspire_usb_kind(void);
int nspire_usb_raw(uint32_t w[6])
{
  int kind = nspire_usb_kind();

  memset(w, 0, 6 * sizeof(w[0]));
  if (kind == 2)
  {
    w[0] = host_usb_reg_read(0xB0000080u);
    w[1] = host_usb_reg_read(0xB0000084u);   /* (2026-11-18) OTGISR: a "pad hub blip"'s VBUS error */
    w[2] = host_usb_reg_read(0xB0000010u);
    w[4] = host_usb_reg_read(0xB0000030u);
    if (fd >= 0 && host_link_auto && !host_link_calc_out && host_link_calc_device_end)
    {
      w[0] = 1u << 21 | 1u << 20 | 1u << 17;   /* the device's end, acting as one, B session */
      w[4] = 0;
    }
  }
  else if (kind == 1)
  {
    w[0] = host_usb_reg_read(0xB00001A4u);
    w[1] = host_usb_reg_read(0xB00001A8u);
    w[2] = host_usb_reg_read(0xB0000140u);
    w[4] = host_usb_reg_read(0xB0000184u);
  }
  return kind;
}

/* The controller nspire_usbcycle.c drives: a CX II's FOTG210 (the cable,
 * an adapter, the device's end), a CX's ChipIdea (3, 4); none while gated. */
/* The other calculator is on the cable at this (the host's) end: a power
 * cycle drops the bus under it (host_usbpad.c's registers). */
int host_link_calc_on_port(void)
{
  return fd >= 0 && host_link_auto && !host_link_calc_out;
}

int nspire_usb_kind(void)
{
  if (fd >= 0 && host_link_auto && !host_link_calc_out)
    return 2;
  return host_usbpad_port < 0 ? 0 : host_usbpad_port >= 3 ? 1 : 2;
}

int nspire_usb_gated(void)
{
  /* (r19-usb) -1 a CX's gate; -2 a CX II's controller its OS has powered down (not a gate: unreadable) */
  return !(fd >= 0 && host_link_auto && !host_link_calc_out) && host_usbpad_port == -1;
}

/* The gated controller's ticks (nspire_link_usb.c): a tick at least every
 * 250 ms, counted in host_link_gated_calls for the tests. */
long host_link_gated_calls;

int link_cable_gated(void)
{
  int nspire_os_sleep(void);

  host_link_gated_calls++;
  if (nspire_os_since_sleep() > 250u * 32768 / 1000)
    nspire_os_sleep();
  return 0;
}

void link_cable_second(char *text, unsigned size)
{
  snprintf(text, size, "svc: PC");
}

/* --link-fast ROLE (host, device, noanswer: offers the device's end but
 * never answers): the fast transport (nspire_link_fast.c) as nspire_link.c
 * sees it: offered in the hello, brought up after it with the other's
 * offer (both ends, the same version; else NavNet, a log line why), and
 * once up the cable's timing is USB's (--link-write, --link-slow,
 * --link-os off; a message --link-fast-us one way, 300 us: a high-speed
 * bulk packet and the other side's next poll) until the link closes.
 * --link-fast-fall N: falls back to NavNet's timing after its Nth send
 * (nothing is lost: the real one resends what the other didn't get). */
int host_link_fast;
int host_link_fast_fall;
double host_link_fast_us = 300;
static int fast_up, fast_sends, fast_fell;
static int fast_bye_seen;   /* LINK2: the other's FAIL came before this side's go_fast */
static double saved_delay, saved_write, saved_slow;
static int saved_os, saved_every;

#define FAST_HOST   1u
#define FAST_DEVICE 2u
#define FAST_MUTE   0x8000u   /* the model's: this end never answers */

uint32_t link_cable_fast_offer(void)
{
  if (!host_link_fast || legacy)
    return 0;
  return (host_link_fast == 1 ? FAST_HOST : host_link_fast == 3 ? FAST_DEVICE | FAST_MUTE : FAST_DEVICE) | 0x100u;
}

static void fast_down(const char *how)
{
  if (!fast_up)
    return;
  fast_up = 0;
  delay = saved_delay;
  host_link_write_ms = saved_write;
  host_link_slow_ms = saved_slow;
  host_link_os = saved_os;
  host_link_slow_every = saved_every;
  nspire_link_log("fast transport: %s (model, after %d sends)", how, fast_sends);
  if (!strncmp(how, "fell back", 9))
    fast_fell = 1;
}

int link_cable_fell_back(void)
{
  int r = fast_fell;
  fast_fell = 0;
  return r;
}

int link_cable_fast_up(void)
{
  return fast_up;
}

/* LINK2: as nspire_link_usb.c's: a linked frame gives the OS its spare time
 * only while NavNet carries the link. --link-fast-os: the build before LINK2
 * (R9F: always), for the before/after figures (host_tests link_fast "cxos"). */
int host_link_fast_os;
int link_cable_linked_os(void)
{
  return !fast_up || host_link_fast_os;
}

/* The model gives everything back as found. */
const char *link_cable_fast_damage(void)
{
  return NULL;
}

/* The arbiter's let-go (nspire_arbiter.c): NavNet's timing from here. */
void link_cable_fast_drop(const char *why)
{
  char how[120];
  snprintf(how, sizeof(how), "fell back to NavNet: let go: %s", why);
  fast_down(how);
  /* LINK2: as the real one's BYE, the other side falls back too (before,
   * the model's other side ran on as if fast, and asked the OS nothing). */
  send_control(CTRL_FAST_BYE);
}

int link_cable_go_fast(uint32_t other, const char *refused)
{
  uint32_t mine = link_cable_fast_offer();

  if (!(mine & 3) || !(other & 3))
  {
    nspire_link_log("fast transport: not used, NavNet carries the link: offer: %s",
                    !(other & 3) ? "the other gpSP doesn't offer it (an older build, or its Link speed is Compatible)" : "this calculator can't");
    return 0;
  }
  if ((mine & other & 3) || ((mine | other) & 3) != 3 || (other >> 8 & 0x7F) != 1)
  {
    nspire_link_log("fast transport: not used, NavNet carries the link: offer: both at the same end, or another version");
    return 0;
  }
  if (refused)
  {
    /* The real one tells the other side FAIL at once: one NavNet trip. */
    host_hw_spend(0.1);
    nspire_link_log("fast transport: not used, NavNet carries the link: this side: %s", refused);
    send_control(CTRL_FAST_BYE);   /* LINK2: the real one's FAIL: the other doesn't run fast alone */
    return 0;
  }
  if (fast_bye_seen)
  {
    fast_bye_seen = 0;
    host_hw_spend(0.1);
    nspire_link_log("fast transport: not used, NavNet carries the link: %s: the other side said FAIL",
                    mine & FAST_HOST ? "host" : "device");
    return 0;
  }
  if ((mine | other) & FAST_MUTE)
  {
    host_hw_spend(4.0);   /* the host's SYN and ACK waits */
    nspire_link_log("fast transport: not used, NavNet carries the link: %s: %s", mine & FAST_HOST ? "host" : "device",
                    mine & FAST_HOST ? "the device never took SYN" : "no SYN from the host in 3 s");
    return 0;
  }
  host_hw_spend(0.2);   /* READY over NavNet, SYN, ACK */
  fast_up = 1;
  fast_sends = 0;
  saved_delay = delay;
  saved_write = host_link_write_ms;
  saved_slow = host_link_slow_ms;
  saved_os = host_link_os;
  saved_every = host_link_slow_every;
  if (!legacy)
    delay = host_link_fast_us / 1e6;
  host_link_write_ms = 0;
  host_link_slow_ms = 0;
  host_link_slow_every = 0;
  if (host_link_os)
    send_queued();
  host_link_os = 0;
  nspire_link_log("fast transport: up in 200 ms: %s (model)", mine & FAST_HOST ? "host" : "device");
  return 1;
}

void link_cable_stats(char *text, unsigned size)
{
  snprintf(text, size, "PC socket%s%s", host_link_os ? ", the calculators' timing" : "",
           fast_up ? ", fast transport up (model)" : "");
}

/* Sending costs its time first; then the message goes, with the time
 * (--link-os: queued until this side lets the OS run). */
int link_cable_send(const void *msg, unsigned len)
{
  uint8_t packet[STAMP + MSG_BYTES];
  double cost = host_hw_message_cost(0), at;

  if (fd < 0 || len > MSG_BYTES || host_link_calc_out)
    return -1;
  if (fast_up && host_link_fast_fall && ++fast_sends >= host_link_fast_fall)
    fast_down("fell back to NavNet: --link-fast-fall");
  host_hw_spend(cost);
  if (host_link_write_ms > 0 && !legacy)
  {
    if (queued_n && (host_link_nohold || held_bytes + len + 2 > 254 || queued_n == QUEUED))
      direct_flush();
    if (!queued_n)
    {
      held_since = host_hw_now();
      held_frames = 0;
    }
    queued[queued_n] = malloc(len);
    memcpy(queued[queued_n], msg, len);
    queued_len[queued_n++] = len;
    held_bytes += len + 2;
    if (host_link_nohold)
      direct_flush();
  }
  else if (host_link_os && !legacy)
  {
    if (queued_n == QUEUED)
      send_queued();
    queued[queued_n] = malloc(len);
    memcpy(queued[queued_n], msg, len);
    queued_len[queued_n++] = len;
  }
  else
  {
    at = host_hw_now();
    memcpy(packet, &at, STAMP);
    memcpy(packet + STAMP, msg, len);
    if (legacy ? send(fd, msg, len, MSG_NOSIGNAL) != (ssize_t)len :
        send(fd, packet, STAMP + len, MSG_NOSIGNAL) != (ssize_t)(STAMP + len))
      return -1;
  }
  if (trace)
    trace_message(msg, len, 0);
  if (len >= 8 && (((const uint8_t *)msg)[0] == 1 || ((const uint8_t *)msg)[0] == 8))
    hellos++;
  if (counting)
  {
    sent++;
    bytes_sent += len;
    spent += cost;
  }
  return 0;
}

/* The other side's connect and disconnect, applied when this side's virtual
 * time reaches their stamp (S9): the two processes run ahead of each other in
 * wall time, and a control packet read early (a replug: disconnect, connect)
 * was applied before this side's own link end, which then threw it away. */
static struct { double at; int what; } pending[32];

static void do_control(int what)
{
  if (what == CTRL_CONNECT)
  {
    peer_connected = 1;
    dropping = 0;
  }
  else if (what == CTRL_DISCONNECT)
    peer_connected = 0;
  else if (what == CTRL_UNPLUG)
    host_link_calc_out = 1;
  else if (what == CTRL_FAST_BYE)
  {
    fast_bye_seen = !fast_up;   /* before this side's go: its bring-up hears the FAIL */
    fast_down("fell back to NavNet: the other side went back to NavNet (its BYE)");
  }
  else if (what == CTRL_PLUG)
  {
    /* The cable is one: its node comes up on this side at the same time. */
    host_link_calc_out = 0;
    host_link_calc_in_at = host_hw_now();
    calc_plugged_in_play = 1;
    sim_seen_on_look = 0;
    if (!host_link_calc_device_end)
      host_usbpad_os_offer();
  }
}

static void apply_pending(void)
{
  int i, keep = 0;
  double now = host_hw_now();

  for (i = 0; i < pending_n; i++)
    if (pending[i].at <= now)
      do_control(pending[i].what);
    else
      pending[keep++] = pending[i];
  pending_n = keep;
}

/* A control that is not due yet holds back what follows it on the stream
 * (the other side's messages after its connect are not this side's to drop
 * or keep before the connect is applied). 1: still held back (not waiting). */
static int hold_back(int wait_ms)
{
  apply_pending();
  while (pending_n)
  {
    double now = host_hw_now();

    if (!wait_ms)
      return 1;
    if (pending[0].at > now)
      host_hw_spend(pending[0].at - now);
    apply_pending();
  }
  return 0;
}

static void apply_control(double at, int what)
{
  if (at > host_hw_now() && pending_n < 32)
  {
    pending[pending_n].at = at;
    pending[pending_n++].what = what;
  }
  else
    do_control(what);
}

/* The next message off the socket, into held (waiting up to wait_ms of
 * real time for it): 1, 0 if none came, -1 if the other side's gone. */
static int take(int wait_ms)
{
  struct pollfd p = { fd, POLLIN, 0 };
  ssize_t n;

  if (hold_back(wait_ms))
    return 0;
  for (;;)
  {
    if (fd < 0)
      return -1;
    if (poll(&p, 1, wait_ms) == 0)
      return 0;
    n = legacy ? recv(fd, held + STAMP, MSG_BYTES, 0) : recv(fd, held, sizeof(held), 0);
    if (n <= 0)
    {
      snprintf(error, sizeof(error), "The link cable came out.");
      close(fd);
      fd = -1;
      return -1;
    }
    if (legacy)
    {
      held_sent = host_hw_now();   /* due now */
      held_len = (int)n;
      return 1;
    }
    if ((size_t)n == STAMP + 1)
    {
      /* The other's connection to this side's service: a new one starts
       * its stream, or it let go. */
      {
        double at;

        memcpy(&at, held, STAMP);
        if (wait_ms && (held[STAMP] == CTRL_UNPLUG || held[STAMP] == CTRL_PLUG) && at > host_hw_now())
          host_hw_spend(at - host_hw_now());   /* waiting for it: the time goes by to the pull */
        apply_control(at, held[STAMP]);
        if (hold_back(wait_ms))
          return 0;
        if (wait_ms && host_link_calc_out)
        {
          snprintf(error, sizeof(error), "The link cable came out.");
          return -1;
        }
      }
      continue;
    }
    if ((size_t)n < STAMP || dropping)
      continue;   /* a connection's let go of: its messages with it */
    memcpy(&held_sent, held, STAMP);
    held_len = (int)(n - STAMP);
    return 1;
  }
}

int host_link_slices;   /* --link-slices */

/* R5: the stand-ins read their packets as u32 words, and the calculator
 * faults on an unaligned load (nspire.c enableAlignmentExceptions): every
 * packet handed to one must be word-aligned. Counted, in the report
 * (link_unaligned); a PC doesn't fault. */
static long unaligned_deliveries;
#define WRAP_RECEIVE(name)                                                        \
  void __real_##name(const void *buf, size_t len, uint16_t client_id);           \
  void __wrap_##name(const void *buf, size_t len, uint16_t client_id)            \
  {                                                                               \
    if ((uintptr_t)buf & 3)                                                       \
      unaligned_deliveries++;                                                     \
    __real_##name(buf, len, client_id);                                           \
  }
WRAP_RECEIVE(serialpoke_net_receive)
WRAP_RECEIVE(serialaw_net_receive)
WRAP_RECEIVE(rfu_net_receive)
WRAP_RECEIVE(serialfe_net_receive)
WRAP_RECEIVE(serialmsio_net_receive)

int link_cable_receive(void *buf, unsigned size, unsigned wait_ms)
{
  double due, now, cost;
  int len;

  if (host_link_calc_out)
    return -1;   /* the cable's out */
  /* --link-os: a wait lets the OS run, and what's queued goes. */
  if (host_link_write_ms > 0 && wait_ms && fd >= 0)
    direct_flush();
  else if (host_link_os && wait_ms && fd >= 0)
    send_queued();
  if (held_len < 0)
  {
    int got = take(wait_ms ? LONGEST_WAIT_MS : 0);
    if (got <= 0)
      return got;
  }
  due = held_sent + delay;
  now = host_hw_now();
  if (due > now)
  {
    if (!wait_ms)
      return 0;   /* not here yet */
    if (host_link_slices && due - now > wait_ms / 1000.0)
    {
      host_hw_spend(wait_ms / 1000.0);
      if (counting)
        waited += wait_ms / 1000.0;
      return 0;   /* the slice went by with nothing */
    }
    /* --link-os: past the spin, the calculator sleeps a tick at a time. */
    if (host_link_os && due - now > SPIN_S)
      due = ((uint64_t)(due / OS_TICK_S) + 1) * OS_TICK_S;
    host_hw_spend(due - now);
    if (counting)
      waited += due - now;
  }
  len = held_len < (int)size ? held_len : (int)size;
  memcpy(buf, held + STAMP, len);
  held_len = -1;
  if (trace)
    trace_message(buf, len, 1);
  cost = host_hw_message_cost(1);
  host_hw_spend(cost);
  if (counting)
  {
    received++;
    bytes_received += len;
    spent += cost;
  }
  return len;
}

/* The socket stays: unlinking leaves the cable plugged in. The
 * connections go (--link-auto's): what the other sent until it connects
 * again was the old connection's. */
void link_cable_close(void)
{
  fast_down("stopped (the link closed)");
  fast_bye_seen = 0;
  if (fd >= 0 && host_link_os)
    send_queued();   /* the BYE */
  if (me_connected)
    send_control(CTRL_DISCONNECT);
  if (!legacy)
    dropping = 1;
  me_connected = peer_connected = 0;
  held_len = -1;
}

void link_cable_shutdown(void)
{
  void host_hw_quit_began(void);
  void host_os_slice_at_stop(void);

  host_hw_quit_began();
  host_os_slice_at_stop();
  if (fd >= 0 && host_link_os)
    send_queued();
}

/* R11w: the exit's bounded form (nothing here can hang: always done) */
int link_cable_shutdown_within(unsigned ms)
{
  (void)ms;
  link_cable_shutdown();
  return 0;
}

void host_link_nonce_tie(int hellos)
{
  ties = hellos;
}

uint32_t link_cable_nonce(void)
{
  if (ties > 0)
  {
    ties--;
    return 0x5A5A5A5Au;
  }
  return player ? (uint32_t)(3 - player) : (uint32_t)getpid();
}

const char *link_cable_error(void)
{
  return error[0] ? error : NULL;
}

/* ---- The report (host_hw.c's) ---- */

/* host_hw.c: a game frame ended (drawn or not). The first one linked is
 * where the time counts from; the frames counted are the ones after it. */
void host_link_frame(int was_drawn)
{
  if (nspire_link_state() == LINK_LINKED && !was_linked)
    links++;
  was_linked = nspire_link_state() == LINK_LINKED;
  if (!linked_as[0] && nspire_link_state() == LINK_LINKED)
  {
    snprintf(linked_as, sizeof(linked_as), "%s", nspire_link_status());
    linked_protocol = nspire_link_protocol();
  }
  if (!measuring || nspire_link_state() != LINK_LINKED)
    return;
  if (!counting)
  {
    counting = 1;
    transfers_before = nspire_link_transfers;
    sum_before = nspire_link_transfer_sum;
  }
  else
    drawn += was_drawn;
  if (ends == ends_size)
  {
    ends_size = ends_size ? ends_size * 2 : 4096;
    frame_times = realloc(frame_times, ends_size * sizeof(*frame_times));
  }
  frame_times[ends++] = host_hw_now();
  transfers = nspire_link_transfers - transfers_before;
  sum = nspire_link_transfer_sum - sum_before;
}

/* The script's "measure" (on) and "measure stop". */
void host_link_measure(int on)
{
  measuring = on;
  counting = 0;
  if (on)
  {
    ends = drawn = sent = received = bytes_sent = bytes_received = 0;
    waited = spent = 0;
    transfers = sum = 0;
  }
}

/* Frames a second over the second before each frame's end (from the
 * first frame then), at the slowest; over all of them if that's less
 * than a second. */
static double slowest_second(void)
{
  double slowest = -1;
  long i, j = 0;

  for (i = 1; i < ends; i++)
  {
    if (frame_times[i] - frame_times[0] < 1.0)
      continue;
    while (frame_times[j] < frame_times[i] - 1.0)
      j++;
    if (slowest < 0 || (i - j) / (frame_times[i] - frame_times[j]) < slowest)
      slowest = (i - j) / (frame_times[i] - frame_times[j]);
  }
  if (slowest < 0 && ends > 1)
    slowest = (ends - 1) / (frame_times[ends - 1] - frame_times[0]);
  return slowest < 0 ? 0 : slowest;
}

void host_link_report(FILE *f)
{
  long frames = ends > 1 ? ends - 1 : 0;
  double seconds = frames ? frame_times[ends - 1] - frame_times[0] : 0;

  fprintf(f, "link %s\n", nspire_link_status());
  fprintf(f, "link_linked %s\n", linked_as[0] ? linked_as : "-");
  fprintf(f, "link_protocol %d\n", linked_protocol);
  fprintf(f, "link_hellos_sent %ld\n", hellos);
  fprintf(f, "link_count %d\n", links);
  fprintf(f, "link_transfers %u\n", (unsigned)transfers);
  fprintf(f, "link_transfer_sum %08x\n", (unsigned)sum);
  fprintf(f, "link_messages_sent %ld\n", sent);
  fprintf(f, "link_messages_received %ld\n", received);
  fprintf(f, "link_bytes_sent %ld\n", bytes_sent);
  fprintf(f, "link_bytes_received %ld\n", bytes_received);
  fprintf(f, "link_frames %ld\n", frames);
  fprintf(f, "link_drawn %ld\n", drawn);
  fprintf(f, "link_seconds %.3f\n", seconds);
  fprintf(f, "link_fps %.2f\n", seconds > 0 ? frames / seconds : 0.0);
  fprintf(f, "link_worst_fps %.2f\n", slowest_second());
  fprintf(f, "link_wait %.3f\n", waited);
  fprintf(f, "link_unaligned %ld\n", unaligned_deliveries);
  {
    extern unsigned nspire_link_wait_screens;
    fprintf(f, "link_wait_screens %u\n", nspire_link_wait_screens);
  }
  fprintf(f, "link_cost %.3f\n", spent);
}
