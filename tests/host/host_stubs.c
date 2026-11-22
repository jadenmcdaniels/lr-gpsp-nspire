/* What the core needs from the calculator frontend, for the PC tests: the
 * parts of nspire_input.c and nspire_video_present.c that don't touch the
 * hardware, and C versions of the ARM assembly helpers. */

#include "common.h"
#include "nspire_hw.h"
#include "nspire_link.h"
#include "nspire_os.h"

#include <stdarg.h>

static u32 old_key;

/* The calculator's registers: only the backlight is used here
 * (nspire_brightness.c); the tests look at what was written. */
uint32_t unit_backlight = 120;
long unit_backlight_writes;

uint32_t host_hw_read(uint32_t reg)
{
  return reg == HW_BACKLIGHT || reg == HW_CX_BACKLIGHT ? unit_backlight : 0;
}

void host_hw_write(uint32_t reg, uint32_t value)
{
  if (reg == HW_BACKLIGHT || reg == HW_CX_BACKLIGHT)
  {
    unit_backlight = value;
    unit_backlight_writes++;
  }
}

/* The calculator's documents folder (Ndless's get_documents_dir), where
 * the saves folder goes: the tests point it at their own folder. */
char unit_documents_dir[600] = "./";

const char *get_documents_dir(void)
{
  return unit_documents_dir;
}

/* The last message that would have been on the screen. */
char unit_osd_message[64];
int unit_osd_count;   /* and how many there were */

static u32 unit_osd_until;
u32 nspire_ticks(void);
/* DETECT: every message since the test last emptied it, "|" after each. */
char unit_osd_log[2048];

void nspire_osd_message(const char *format, ...)
{
  va_list ap;
  unit_osd_count++;
  va_start(ap, format);
  vsnprintf(unit_osd_message, sizeof(unit_osd_message), format, ap);
  va_end(ap);
  unit_osd_until = nspire_ticks() + NSPIRE_TICK_HZ * 2;
  if (strlen(unit_osd_log) + strlen(unit_osd_message) + 2 < sizeof(unit_osd_log))
  {
    strcat(unit_osd_log, unit_osd_message);
    strcat(unit_osd_log, "|");
  }
}

/* As nspire_video_present.c's: the last message for two seconds. */
const char *nspire_osd_visible(void)
{
  return unit_osd_message[0] && (s32)(nspire_ticks() - unit_osd_until) < 0 ? unit_osd_message : NULL;
}

void nspire_osd_extend(void)
{
  if (nspire_osd_visible())
    unit_osd_until = nspire_ticks() + NSPIRE_TICK_HZ * 2;
}

/* nspire.c's (nspire_osd.c, now in the unit tests for its rule, reads it) */
int nspire_lcd_portrait;

/* R11w part 2: a message whose state is over taken down (counted) */
int unit_osd_withdrawn;
void nspire_osd_withdraw(const char *text)
{
  if (!text || !strcmp(unit_osd_message, text))
  {
    unit_osd_until = nspire_ticks();
    unit_osd_withdrawn++;
  }
}

bool input_check_savestate(const u8 *src)
{
  const u8 *p = bson_find_key(src, "input");
  return (p && bson_contains_key(p, "prevkey", BSON_TYPE_INT32));
}

bool input_read_savestate(const u8 *src)
{
  const u8 *p = bson_find_key(src, "input");
  if (p)
    return bson_read_int32(p, "prevkey", &old_key);
  return false;
}

unsigned input_write_savestate(u8 *dst)
{
  u8 *wbptr1, *startp = dst;
  bson_start_document(dst, "input", wbptr1);
  bson_write_int32(dst, "prevkey", old_key);
  bson_finish_document(dst, wbptr1);
  return (unsigned int)(dst - startp);
}

void video_fill_u16_strip_arm(u16 *dst, u32 nh, u32 value)
{
  while (nh--)
    *dst++ = (u16)value;
}

void nspire_fps_overlay_reset(void) {}
/* (nspire_frameskip.c's where that is linked: the pacing model, gpsp-host) */
__attribute__((weak)) void nspire_os_watch_line(void) {}
static const uint32_t os_watch_never;   /* (nspire_lines.c's inline check: never near; host_platform.c's for gpsp-host) */
__attribute__((weak)) struct nspire_os_watch nspire_os_watch_at = { &os_watch_never, 0 };
/* (the pad log's line on the OS tick's phase; host_platform.c's for gpsp-host) */
__attribute__((weak)) void nspire_os_tick_rule_report(char *text, unsigned size)
{
  snprintf(text, size, "OS tick: (unit tests)");
}
/* The unit tests' time (nspire_link.c's waits and give-ups): it moves
 * only when a test moves it. */
u32 unit_ticks;
u32 nspire_ticks(void) { return unit_ticks; }
/* The unit tests' waits count slices (nspire_link.c), as with a timer;
 * unit_timer_dead 1: a CX whose timers don't run (nspire_ticks stands still
 * in an OS sleep, as fallback_ticks does). */
int unit_timer_dead;   /* 2: the ticks stand still though the timer says it runs */
int nspire_timer_runs(void) { return unit_timer_dead != 1; }
/* R11t: the clock select's log line (nspire/nspire.c; gpsp-host runs the real one) */
int nspire_timer_clock_text(char *text, u32 size) { snprintf(text, size, "timer clock select: unit test"); return 1; }
/* R12: the time base's lines after it (one here) */
int nspire_timer_header_text(int line, char *text, u32 size)
{
  if (line == 0)
    return nspire_timer_clock_text(text, size);
  if (line == 1)
  {
    snprintf(text, size, "time base: unit test");
    return 1;
  }
  return 0;
}
/* R12: the come-up's timer words (nothing runs here), the legacy lend */
void nspire_timer_usb_probe(uint32_t w[6]) { int i; for (i = 0; i < 6; i++) w[i] = 0; }
void nspire_timer_lend(void) {}
void nspire_timer_retake(void) {}
/* The OS's sleeps (nspire_usbcycle.c's waits): a 10 ms tick each, the time
 * moving unless the timer is dead; unit_os_sleep_hook runs the OS's part. */
long unit_os_sleeps;
void (*unit_os_sleep_hook)(void);
int nspire_os_ready(void) { return 1; }
int nspire_os_sleep(void)
{
  unit_os_sleeps++;
  if (!unit_timer_dead)
    unit_ticks += 32768 / 100;
  if (unit_os_sleep_hook)
    unit_os_sleep_hook();
  return 1;
}
u32 nspire_rtc_seconds(void) { return 0; }
void nspire_video_mix_reset(void) {}

/* nspire_host_main.c */
u32 update_backup_flag = 1;

/* Frontend globals nspire_config.c works on (nspire_video_ui.c,
 * nspire_host_main.c, nspire_input.c, nspire_clock.c on the calculator). */
u32 screen_scale = 3;
u32 screen_filter;
u32 current_frameskip_type;
u32 frameskip_value = 4;
u32 random_skip;
u32 global_enable_analog;
u32 analog_sensitivity_level = 4;
char nspire_clock_crashed_rom[512];

/* The menus' frame (nspire_ui.c draws into it; the unit tests read it). */
u16 nspire_landscape[320 * 240];
void flip_screen_ui(void) {}

/* The unit tests' link cable (gpsp-host has host_link.c): unplugged unless
 * unit_cable_on; what comes is unit_cable_in's message, once, then the
 * ones unit_cable_push queued, in order; the last sent is in
 * unit_cable_out, and the first UNIT_LOG since unit_cable_logged was set
 * to 0 in unit_cable_log. The nonces are unit_cable_nonce, then one more
 * each time. */
#define UNIT_LOG 16
int unit_cable_on;
uint8_t unit_cable_in[64];
int unit_cable_in_len;
long unit_cable_sent;
uint8_t unit_cable_out[2048];
unsigned unit_cable_out_len;
uint8_t unit_cable_log[UNIT_LOG][64];
unsigned unit_cable_log_len[UNIT_LOG];
int unit_cable_logged;
uint32_t unit_cable_nonce = 1;
static uint8_t queued[24][64];
static unsigned queued_len[24];
static int queued_n, queued_at;
/* The other gpSP's part in nspire_link.c's ping_phase (protocol 2): each
 * ping sent is answered (its pong, the body as sent), and the other side
 * pings back once with the same number; these come before the queue. */
static uint8_t pong[2048];
static uint8_t delay_back[16];
static unsigned delay_back_len;
static unsigned pong_len;
static int ping_backs;   /* pings the other side still sends back */
long unit_cable_pings;

void unit_cable_push(const void *msg, unsigned len)
{
  if (queued_at == queued_n)
    queued_at = queued_n = 0;
  if (queued_n < 24 && len <= sizeof(queued[0]))
  {
    memcpy(queued[queued_n], msg, len);
    queued_len[queued_n++] = len;
  }
}

void unit_cable_clear(void)
{
  delay_back_len = 0;
  queued_at = queued_n = 0;
  unit_cable_in_len = 0;
  pong_len = 0;
  ping_backs = 0;
}

int unit_cable_opened_automatic = -1;   /* the last open's: on its own or picked */
int link_cable_open(int automatic)
{
  unit_cable_opened_automatic = automatic;
  return unit_cable_on ? 0 : -1;
}
/* LINK2: unit_cable_wait: the other side not there yet (Link picked, looking). */
int unit_cable_wait;
int link_cable_ready(void) { return !unit_cable_on ? -1 : unit_cable_wait ? 0 : 1; }

int link_cable_send(const void *msg, unsigned len)
{
  if (!unit_cable_on)
    return -1;
  if (len >= 8 && ((const uint8_t *)msg)[0] == 9 && len <= sizeof(pong))   /* MSG_PING */
  {
    memcpy(pong, msg, len);
    pong[0] = 10;   /* MSG_PONG */
    pong_len = len;
    ping_backs++;
    unit_cable_pings++;
    return 0;
  }
  if (len >= 8 && ((const uint8_t *)msg)[0] == 10)   /* the pong for its ping */
    return 0;
  if (len >= 10 && ((const uint8_t *)msg)[0] == 11 && len <= sizeof(delay_back))   /* MSG_DELAY */
  {
    /* The other side (this build too) asks the same. */
    memcpy(delay_back, msg, len);
    delay_back_len = len;
    return 0;
  }
  unit_cable_sent++;
  unit_cable_out_len = len < sizeof(unit_cable_out) ? len : sizeof(unit_cable_out);
  memcpy(unit_cable_out, msg, unit_cable_out_len);
  if (unit_cable_logged < UNIT_LOG)
  {
    unsigned n = len < sizeof(unit_cable_log[0]) ? len : sizeof(unit_cable_log[0]);
    memcpy(unit_cable_log[unit_cable_logged], msg, n);
    unit_cable_log_len[unit_cable_logged++] = n;
  }
  return 0;
}

/* R6: the fuzz test's messages (frontend_tests.c test_link_fuzz): when
 * set, it gives what comes once nothing else is queued (its length, which
 * may be more than size: then nothing comes, as NavNet's read). */
int (*unit_cable_source)(void *buf, unsigned size);

long unit_cable_receives;   /* reads that waited (wait_ms > 0) */

int link_cable_receive(void *buf, unsigned size, unsigned wait_ms)
{
  int len = unit_cable_in_len;

  if (wait_ms)
    unit_cable_receives++;

  if (!unit_cable_on)
    return -1;
  if (unit_cable_source && !pong_len && !delay_back_len && ping_backs <= 0 && len <= 0 && queued_at >= queued_n)
    return unit_cable_source(buf, size);
  if (pong_len)
  {
    len = pong_len <= size ? (int)pong_len : 0;
    if (len)
      memcpy(buf, pong, len);
    pong_len = 0;
    return len;
  }
  if (delay_back_len)
  {
    len = delay_back_len <= size ? (int)delay_back_len : 0;
    if (len)
      memcpy(buf, delay_back, len);
    delay_back_len = 0;
    return len;
  }
  if (ping_backs > 0)
  {
    uint8_t *p = buf;
    if (size < 8)
      return 0;
    memset(p, 0, 8);
    p[0] = 9;   /* MSG_PING, bare */
    ping_backs--;
    return 8;
  }
  if (len <= 0 && queued_at < queued_n)
  {
    len = (int)queued_len[queued_at];
    if ((unsigned)len <= size)
      memcpy(buf, queued[queued_at], len);
    queued_at++;
    return (unsigned)len <= size ? len : 0;
  }
  if (len <= 0 || (unsigned)len > size)
    return 0;
  memcpy(buf, unit_cable_in, len);
  unit_cable_in_len = 0;
  return len;
}

void link_cable_close(void) {}
void link_cable_shutdown(void) {}
int link_cable_shutdown_within(unsigned ms) { (void)ms; return 0; }
/* The cable watch: what it sees is unit_watch_seen (0 unless a test sets
 * it), and how often it was asked, whether it could connect then and
 * what the port said, how often watching was let go. The USB port says
 * unit_usb_plugged (nspire_usb_plugged: 0, nothing on it, unless a test
 * says). */
int unit_watch_seen, unit_usb_plugged, unit_watch_plugged;
long unit_watch_calls, unit_watch_may_connect, unit_unwatches;
/* SEAM-2: its looks, every 1.5 s from the first watch (nspire_link_usb.c's
 * WATCH_LOOK_MS), and unit_look_nodes: what a look made now finds. */
unsigned unit_looks, unit_look_nodes, unit_looks_now, unit_last_nodes;
static u32 unit_look_at;
unsigned link_cable_looks(void) { return unit_looks; }
unsigned link_cable_look_now(void) { unit_looks++; unit_looks_now++; return unit_last_nodes = unit_look_nodes; }
/* DETECT: a port change: a look at once, then every LINK_HURRY_LOOK_MS for
 * LINK_HURRY_MS (nspire_link_usb.c's, a look's cost taken as nothing). */
unsigned unit_hurries;
static u32 unit_hurry_at;
static int unit_hurrying, unit_look_due;
void link_cable_hurry(void)
{
  unit_hurries++;
  unit_hurry_at = unit_ticks;
  unit_hurrying = 1;
  unit_look_due = 1;
}
unsigned link_cable_progress(void)
{
  return unit_watch_seen == 2 ? LINK_UP_NODE | LINK_UP_OUT : unit_watch_seen || unit_last_nodes ? LINK_UP_NODE : 0;
}
long unit_watch_fresh;
int link_cable_watch(int may_connect, int plugged, int fresh)
{
  u32 period = 32768u * 3 / 2;

  unit_watch_fresh = fresh;
  unit_watch_calls++;
  unit_watch_may_connect = may_connect;
  unit_watch_plugged = plugged;
  if (unit_hurrying && unit_ticks - unit_hurry_at < 32768u * LINK_HURRY_MS / 1000)
    period = 32768u * LINK_HURRY_LOOK_MS / 1000;
  if (!unit_looks || unit_look_due || unit_ticks - unit_look_at >= period)
  {
    unit_looks++;
    unit_look_at = unit_ticks;
    unit_look_due = 0;
  }
  return unit_watch_seen ? unit_watch_seen : unit_last_nodes ? 1 : 0;   /* as the real one: what the last look found */
}
void link_cable_unwatch(void) { unit_unwatches++; }
void link_cable_frame_done(unsigned frames) { (void)frames; }
void link_cable_flush(void) {}
uint32_t link_cable_fast_offer(void) { return 0; }
/* What the link's hello asked of the fast transport: calls, and the last refusal (NULL: none). */
int unit_go_fasts;
const char *unit_go_fast_refused;
int link_cable_go_fast(uint32_t other_offer, const char *refused)
{
  (void)other_offer;
  unit_go_fasts++;
  unit_go_fast_refused = refused;
  return 0;
}
int link_cable_fast_up(void) { return 0; }
int link_cable_linked_os(void) { return 1; }
void link_cable_fast_drop(const char *why) { (void)why; }
/* What the fast transport says it left not as found (frontend_tests sets it). */
const char *unit_fast_damage;
const char *link_cable_fast_damage(void) { return unit_fast_damage; }
int link_cable_fell_back(void) { return 0; }
void link_cable_push(void) {}
void link_cable_stats(char *text, unsigned size) { snprintf(text, size, "unit test"); }
unsigned nspire_os_wanted;
/* The USB port's registers, counted (unit_usb_reads: the observer's samples,
 * unit_os_sleeps: OS time taken), and unit_usb_bits_set (not 0) as the bits. */
long unit_usb_reads;
uint32_t unit_usb_bits_set;
int nspire_usb_plugged(void) { unit_usb_reads++; return unit_usb_plugged; }
int unit_usb_gated;
uint32_t nspire_usb_bits(void) { return unit_usb_gated ? 0 : unit_usb_bits_set ? unit_usb_bits_set : (uint32_t)unit_usb_plugged; }
/* PAD3: the raw words (nspire_usb_raw): unit_usb_raw_kind and unit_usb_raw. */
int unit_usb_raw_kind;
uint32_t unit_usb_raw[6];
int nspire_usb_raw(uint32_t w[6]) { memcpy(w, unit_usb_raw, sizeof(unit_usb_raw)); return unit_usb_raw_kind; }
/* LINK2: unit_usb_stopped: a host's controller its OS never started (r5, r6). */
int unit_usb_stopped;
int nspire_usb_running(void) { return unit_usb_plugged > 0 && !unit_usb_stopped; }
unsigned link_cable_last_nodes(void) { return 0; }
/* A CX's gated controller: unit_usb_gated (0 unless a test says), its
 * ticks counted in unit_gated_calls. */
long unit_gated_calls;
int nspire_usb_gated(void) { return unit_usb_gated; }
/* S4: the power cycles asked for (unit_reenum_result: what the next ones return). */
int unit_reenums, unit_reenum_result;
int unit_reenum_to_zero;   /* the cycle ends at tick 0 (SEAM-13) */
/* (2026-11-17) nspire_usbcycle.c's transceiver wake: the unit tests'
 * controller has none asleep. */
/* (weak: frontend_tests links the real nspire_usbcycle.c too) */
__attribute__((weak)) int nspire_usb_phy_asleep(void) { return 0; }
__attribute__((weak)) int nspire_usb_phy_wake(void) { return 0; }
int nspire_usb_reenumerate(int attempt)
{
  (void)attempt;
  unit_reenums++;
  if (unit_reenum_to_zero)
    unit_ticks = 0;
  return unit_reenum_result;
}
int nspire_usb_power_restore(void) { return 0; }
int unit_restore_pending;
int nspire_usb_power_restore_pending(void) { return unit_restore_pending; }
int link_cable_gated(void) { unit_gated_calls++; return 0; }
uint32_t nspire_os_sleeps(void) { return 0; }
/* (r19-usb-exit) nspire_os.c's: the drive's quiet stop (drive_tests) holds the sleeps */
void nspire_os_sleep_hold(int on) { (void)on; }
uint32_t nspire_os_sleeps_held(void) { return 0; }
uint32_t nspire_os_ready_cost(uint32_t *at) { if (at) *at = 0; return 0; }
uint32_t nspire_os_sleep_longest(void) { return 0; }
uint32_t nspire_os_busy_slept(uint32_t *count) { if (count) *count = 0; return 0; }
void nspire_os_diag_watch(char *text, unsigned size) { snprintf(text, size, "wd: unit test"); }
int nspire_os_watchdog_off(void) { return 0; }
uint32_t nspire_os_watchdog_offs(void) { return 0; }
/* R11w: the exit's backstop and restart (the unit tests count them) */
int unit_backstop, unit_restarts;
uint32_t nspire_os_backstop_arm(uint32_t ms) { unit_backstop = 1; return ms ? 1000 : 0; }
void nspire_os_backstop_disarm(void) { unit_backstop = 0; }
int nspire_os_backstop_armed(void) { return unit_backstop; }
void nspire_os_restart(void) { unit_restarts++; }
void link_cable_second(char *text, unsigned size) { snprintf(text, size, "svc: unit test"); }
void nspire_os_report(char *text, unsigned size) { snprintf(text, size, "OS sleep: unit test"); }
void nspire_os_note_start(void) {}
void nspire_os_diag_system(char *text, unsigned size) { snprintf(text, size, "calculator: unit test"); }
void nspire_os_diag_usb(char *text, unsigned size) { snprintf(text, size, "USB: unit test, plugged %d", unit_usb_plugged); }
void nspire_os_diag_screen(char *text, unsigned size) { snprintf(text, size, "LCD: unit test"); }
uint32_t link_cable_nonce(void) { return unit_cable_nonce++; }
const char *link_cable_error(void) { return NULL; }

/* os.h's is_cx2: a CX II. gpsp-host has host_hw.c's instead (--model). */
int host_is_cx2 = 1;
__attribute__((weak)) void host_os_irq_window(void) {}
