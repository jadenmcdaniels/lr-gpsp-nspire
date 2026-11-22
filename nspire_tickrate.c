/* The timer's speed against the real-time clock: see nspire_tickrate.h. */
#include <stdio.h>
#include <string.h>

#include "nspire_tickrate.h"

#define MIN_SECONDS 2    /* each stretch timed */
#define MAX_SECONDS 60   /* longer: the clock was set, start over */
#define OFF_PERCENT 5    /* off by more than this: corrected */

/* A change of the real-time clock's second, between two timer readings. */
struct edge
{
  uint32_t before, after, seconds;
};

static uint32_t meant_hz, measured_hz, assumed_hz;
static uint32_t edges_seen, stretches_timed, last_stretch_hz;
static int checked, corrected;
static int have_last, have_start;
static uint32_t last_raw, last_seconds;
static struct edge start;    /* where the stretch being timed starts */
static uint32_t stretch_hz;  /* the last stretch's speed, for the next to agree with */

/* ticks = base_ticks + (raw - base_raw) * scale / 65536: the readings
 * themselves (scale 1.0) until the timer is found to be off. */
static uint32_t base_raw, base_ticks, scale = 1u << 16;

static uint32_t mapped(uint32_t raw)
{
  uint32_t since = raw - base_raw;
  uint32_t ticks = base_ticks + (uint32_t)(((uint64_t)since * scale) >> 16);
  if (since >= 0x40000000u)
  {
    base_raw = raw;
    base_ticks = ticks;
  }
  return ticks;
}

static void restart_check(void)
{
  measured_hz = 0;
  checked = 0;
  have_last = have_start = 0;
  stretch_hz = 0;
}

void tick_rate_start(uint32_t hz)
{
  meant_hz = hz;
  assumed_hz = 0;
  edges_seen = stretches_timed = last_stretch_hz = 0;
  corrected = 0;
  base_raw = base_ticks = 0;
  scale = 1u << 16;
  restart_check();
}

void tick_rate_recheck(uint32_t raw)
{
  uint32_t ticks = mapped(raw);
  base_raw = raw;
  base_ticks = ticks;
  restart_check();
}

void tick_rate_assume(uint32_t raw, uint32_t hz)
{
  uint32_t off = hz > meant_hz ? hz - meant_hz : meant_hz - hz;
  uint32_t ticks = mapped(raw);

  if (!hz)
    return;
  base_raw = raw;
  base_ticks = ticks;
  /* Within 5%: the readings as they are again (a speed that came back
   * after a clock switch). */
  if ((uint64_t)off * 100 <= (uint64_t)meant_hz * OFF_PERCENT)
  {
    assumed_hz = 0;
    corrected = 0;
    scale = 1u << 16;
    return;
  }
  assumed_hz = hz;
  corrected = 1;
  scale = (uint32_t)(((uint64_t)meant_hz << 16) / hz);
}

static void decide(uint32_t raw, uint32_t hz)
{
  uint32_t off = hz > meant_hz ? hz - meant_hz : meant_hz - hz;
  uint32_t ticks = mapped(raw);

  checked = 1;
  measured_hz = hz;
  corrected = (uint64_t)off * 100 > (uint64_t)meant_hz * OFF_PERCENT;
  /* From here on at the speed found, carrying on from the ticks now. */
  base_raw = raw;
  base_ticks = ticks;
  scale = corrected ? (uint32_t)(((uint64_t)meant_hz << 16) / hz) : 1u << 16;
}

static void edge(uint32_t raw, const struct edge *e)
{
  uint32_t seconds, shortest, blur;

  if (!have_start)
  {
    start = *e;
    have_start = 1;
    return;
  }

  seconds = e->seconds - start.seconds;
  if (seconds > MAX_SECONDS)
  {
    start = *e;
    stretch_hz = 0;
    return;
  }

  /* The second changed somewhere between 'before' and 'after' at both ends,
   * so the stretch is timed to within both of those gaps. */
  shortest = e->before - start.after;
  blur = (e->after - e->before) + (start.after - start.before);
  if (seconds >= MIN_SECONDS && shortest > 0 && blur <= shortest / 100)
  {
    uint32_t hz = (uint32_t)(((uint64_t)(e->before - start.before) + (e->after - start.after)) /
                             (2 * seconds));
    stretches_timed++;
    last_stretch_hz = hz;
    uint32_t diff = hz > stretch_hz ? hz - stretch_hz : stretch_hz - hz;
    start = *e;
    if (hz < meant_hz / 64 || hz / 64 > meant_hz)
      stretch_hz = 0;
    else if (stretch_hz && (uint64_t)diff * 100 <= stretch_hz)
      decide(raw, (uint32_t)(((uint64_t)hz + stretch_hz) / 2));
    else
      stretch_hz = hz;
    return;
  }

  /* A sharper start for the stretch. */
  if (e->after - e->before < start.after - start.before)
    start = *e;
}

uint32_t tick_rate_ticks(uint32_t raw, uint32_t seconds)
{
  if (!checked)
  {
    if (have_last && seconds != last_seconds)
    {
      struct edge e = { last_raw, raw, seconds };
      edges_seen++;
      edge(raw, &e);
    }
    have_last = 1;
    last_raw = raw;
    last_seconds = seconds;
  }
  return mapped(raw);
}

int tick_rate_checked(void)
{
  return checked;
}

uint32_t tick_rate_measured(void)
{
  return measured_hz;
}

int tick_rate_corrected(void)
{
  return corrected;
}

void tick_rate_resume(uint32_t raw, uint32_t ticks)
{
  base_raw = raw;
  base_ticks = ticks;
  restart_check();
}

void tick_rate_report(char *text, size_t size)
{
  if (!checked && assumed_hz)
    snprintf(text, size, "timer: %lu ticks/s by the CPU clock, not %lu; corrected (real-time clock: %lu changes "
             "seen, %lu stretches timed, last %lu/s)", (unsigned long)assumed_hz, (unsigned long)meant_hz,
             (unsigned long)edges_seen, (unsigned long)stretches_timed, (unsigned long)last_stretch_hz);
  else if (!checked)
    snprintf(text, size, "timer: %lu ticks/s, not checked against the real-time clock yet (%lu changes seen, "
             "%lu stretches timed, last %lu/s)", (unsigned long)meant_hz, (unsigned long)edges_seen,
             (unsigned long)stretches_timed, (unsigned long)last_stretch_hz);
  else if (corrected)
    snprintf(text, size, "timer: %lu ticks/s by the real-time clock, not %lu; corrected",
             (unsigned long)measured_hz, (unsigned long)meant_hz);
  else
    snprintf(text, size, "timer: %lu ticks/s (%lu by the real-time clock)", (unsigned long)meant_hz,
             (unsigned long)measured_hz);
}

/* ---- R12: the CX II's time base (nspire_tickrate.h) ---- */

#define TB_TICK_HZ   32768u
#define TB_FIRST_S   2        /* the first stretch from the sharp start */
#define TB_LAST_S    1024     /* the longest: done after it */
#define TB_SHARP     1000     /* both ends' readings within 1/TB_SHARP of the stretch: within 0.05% */
#define TB_NEAR_PPM  100000   /* a first stretch: within 10% of the speed in use */
#define TB_AGREE_PPM 5000     /* a later one, or a second one in a row: within 0.5% */
#define TB_LOG       6

struct tb_edge
{
  uint64_t before, after;   /* the readings around the second's change */
  uint32_t seconds;
};

static struct
{
  uint64_t base_raw, hz_q16, start_q16, last_raw, cand_q16;
  uint32_t base_ticks, last_ticks, start_ticks, scale, last_seconds, next_s;
  int shift, have_last, have_anchor, refined_here, done;
  struct tb_edge anchor;
  uint32_t refines, disagree, edges;
  struct { uint32_t at, stretch; uint64_t from, to; } log[TB_LOG];
} tb;

/* ticks = base_ticks + (raw - base_raw) * scale >> shift, the shift as
 * large as lets scale fit (2^-32 of a tick a count for a fast count). */
static uint32_t tb_map(uint64_t raw)
{
  uint64_t since = raw >= tb.base_raw ? raw - tb.base_raw : 0;

  while (since >= (1ull << 31))
  {
    tb.base_ticks += (uint32_t)(((1ull << 31) * tb.scale) >> tb.shift);
    tb.base_raw += 1ull << 31;
    since -= 1ull << 31;
  }
  return tb.base_ticks + (uint32_t)((since * tb.scale) >> tb.shift);
}

/* From 'raw' on at hz_q16 (counts a second x 65536), the ticks going on
 * from where they are now. */
static void tb_set(uint64_t raw, uint64_t hz_q16)
{
  uint64_t scale = 0;
  int s;

  if (!hz_q16)
    return;
  tb.base_ticks = tb_map(raw);
  tb.base_raw = raw;
  tb.hz_q16 = hz_q16;
  for (s = 32; s > 0; s--)
    if ((scale = ((uint64_t)TB_TICK_HZ << (s + 16)) / hz_q16) <= 0xFFFFFFFFu)
      break;
  tb.shift = s;
  tb.scale = (uint32_t)scale;
}

static uint32_t tb_ppm(uint64_t a, uint64_t b)
{
  uint64_t d = a > b ? a - b : b - a;
  return b ? (uint32_t)(d * 1000000u / b > 0xFFFFFFFFu ? 0xFFFFFFFFu : d * 1000000u / b) : 0xFFFFFFFFu;
}

static void tb_edge(uint64_t raw, const struct tb_edge *e)
{
  uint64_t blur_e = e->after - e->before, blur_a, counts, hz;
  uint32_t secs;
  int ok;

  if (!tb.have_anchor)
  {
    tb.anchor = *e;
    tb.have_anchor = 1;
    tb.refined_here = 0;
    tb.next_s = TB_FIRST_S;
    return;
  }
  secs = e->seconds - tb.anchor.seconds;
  if ((int32_t)secs <= 0 || secs > 2 * TB_LAST_S)
  {
    /* the clock was set: from here again */
    tb.anchor = *e;
    tb.refined_here = 0;
    tb.next_s = TB_FIRST_S;
    return;
  }
  blur_a = tb.anchor.after - tb.anchor.before;
  if (secs < tb.next_s)
  {
    /* a sharper start, while nothing was timed from this one */
    if (!tb.refined_here && blur_e < blur_a)
      tb.anchor = *e;
    return;
  }
  counts = e->before - tb.anchor.after;
  if (!counts || (blur_e + blur_a) * TB_SHARP > counts)
    return;   /* not sharp enough: a later second (a longer stretch) may be */
  /* the second changed between 'before' and 'after' at both ends: the
   * middle of both, over the seconds between */
  hz = (((e->before - tb.anchor.before) + (e->after - tb.anchor.after)) << 15) / secs;
  ok = tb_ppm(hz, tb.hz_q16) <= (tb.refined_here ? TB_AGREE_PPM : TB_NEAR_PPM) ||
       (tb.cand_q16 && tb_ppm(hz, tb.cand_q16) <= TB_AGREE_PPM);
  if (!ok)
  {
    /* the clock set, or the speed changed: a new start, and this speed for
     * the next stretch to agree with */
    tb.disagree++;
    tb.cand_q16 = hz;
    tb.anchor = *e;
    tb.refined_here = 0;
    tb.next_s = TB_FIRST_S;
    return;
  }
  {
    unsigned i = tb.refines < TB_LOG ? tb.refines : TB_LOG - 1;
    tb.log[i].at = tb_map(raw) - tb.start_ticks;
    tb.log[i].stretch = secs;
    tb.log[i].from = tb.hz_q16;
    tb.log[i].to = hz;
  }
  tb_set(raw, hz);
  tb.refines++;
  tb.refined_here = 1;
  tb.cand_q16 = 0;
  tb.next_s = secs * 2;
  if (secs >= TB_LAST_S)
    tb.done = 1;
}

void tick_base_start(uint64_t raw, uint32_t hz, uint32_t ticks)
{
  memset(&tb, 0, sizeof(tb));
  tb.base_raw = raw;
  tb.base_ticks = tb.last_ticks = tb.start_ticks = ticks;
  tb.start_q16 = (uint64_t)hz << 16;
  tb.next_s = TB_FIRST_S;
  tb_set(raw, tb.start_q16 ? tb.start_q16 : (uint64_t)TB_TICK_HZ << 16);
}

uint32_t tick_base_ticks(uint64_t raw, uint32_t seconds)
{
  uint32_t t;

  if (!tb.done)
  {
    if (tb.have_last && seconds != tb.last_seconds)
    {
      struct tb_edge e = { tb.last_raw, raw, seconds };
      tb.edges++;
      tb_edge(raw, &e);
    }
    tb.have_last = 1;
    tb.last_raw = raw;
    tb.last_seconds = seconds;
  }
  t = tb_map(raw);
  /* never back (a reading older than the last: none should be) */
  if ((int32_t)(t - tb.last_ticks) < 0)
    t = tb.last_ticks;
  tb.last_ticks = t;
  return t;
}

void tick_base_recheck(void)
{
  tb.have_last = tb.have_anchor = tb.refined_here = tb.done = 0;
  tb.cand_q16 = 0;
  tb.next_s = TB_FIRST_S;
}

int tick_base_checked(void)
{
  return tb.done;
}

uint64_t tick_base_hz_q16(void)
{
  return tb.hz_q16;
}

uint32_t tick_base_hz(void)
{
  return (uint32_t)((tb.hz_q16 + 0x8000) >> 16);
}

uint32_t tick_base_refinements(void)
{
  return tb.refines;
}

uint32_t tick_base_disagreements(void)
{
  return tb.disagree;
}

static long tb_signed_ppm(uint64_t to, uint64_t from)
{
  long p = (long)tb_ppm(to, from);
  return to >= from ? p : -p;
}

void tick_base_report(char *text, size_t size)
{
  unsigned i, n = tb.refines < TB_LOG ? tb.refines : TB_LOG;
  int len;

  len = snprintf(text, size, "%lu.%02lu counts/s in use (%+ld ppm from the start's %lu by the CPU clock); the "
                 "real-time clock: %lu refinement(s), %lu disagreement(s), %lu second changes seen%s",
                 (unsigned long)(tb.hz_q16 >> 16), (unsigned long)(((tb.hz_q16 & 0xFFFF) * 100) >> 16),
                 tb_signed_ppm(tb.hz_q16, tb.start_q16), (unsigned long)(tb.start_q16 >> 16), (unsigned long)tb.refines,
                 (unsigned long)tb.disagree, (unsigned long)tb.edges, tb.done ? " (done)" : "");
  for (i = 0; i < n && len >= 0 && (size_t)len < size; i++)
    len += snprintf(text + len, size - len, "%s%s at %lu.%lu s over %lu s: %lu.%02lu (%+ld ppm)", i ? "," : ":",
                    i == TB_LOG - 1 && tb.refines > TB_LOG ? " ... the last" : "",
                    (unsigned long)(tb.log[i].at / TB_TICK_HZ), (unsigned long)(tb.log[i].at % TB_TICK_HZ * 10 / TB_TICK_HZ),
                    (unsigned long)tb.log[i].stretch, (unsigned long)(tb.log[i].to >> 16),
                    (unsigned long)(((tb.log[i].to & 0xFFFF) * 100) >> 16), tb_signed_ppm(tb.log[i].to, tb.log[i].from));
}

/* ---- R12: following a down-counter (nspire_tickrate.h) ---- */

void tick_follow_start(struct tick_follow *f, uint32_t value, uint32_t hz, uint32_t seconds)
{
  memset(f, 0, sizeof(*f));
  f->prev = value;
  f->hz = hz;
  f->last_seconds = seconds;
}

void tick_follow_restart(struct tick_follow *f, uint32_t value, uint32_t seconds)
{
  f->prev = value;
  f->last_seconds = seconds;
  f->still = 0;
}

uint64_t tick_follow_step(struct tick_follow *f, uint32_t value, uint32_t top, uint32_t seconds)
{
  uint32_t dt = seconds - f->last_seconds;
  uint64_t delta, bound;

  if ((int32_t)dt < 0)
    dt = 0;   /* the clock set back: no telling */
  bound = (uint64_t)(dt + 1) * f->hz + f->hz / 4;
  if (value <= f->prev)
    delta = f->prev - value;
  else
    delta = (uint64_t)f->prev + 1 + (top >= value ? top - value : 0);   /* past 0: on from its top */
  if (delta > bound)
  {
    /* rewritten (or stopped and started): no time is made of it */
    f->jumps++;
    f->jump_from = f->prev;
    f->jump_to = value;
    delta = 0;
  }
  else if (dt >= 2)
  {
    /* a long gap: the whole periods that went by unseen (the clock's +-1 s
     * is less than half a period of any count followed) */
    uint64_t period = (uint64_t)top + 1, expect = (uint64_t)dt * f->hz;
    if (expect > delta + period / 2)
    {
      uint64_t k = (expect - delta + period / 2) / period;
      if (k && delta + k * period <= bound)
      {
        f->lost_wraps += (uint32_t)k;
        delta += k * period;
      }
    }
  }
  if (value == f->prev)
    f->still += dt;
  else
    f->still = 0;
  f->prev = value;
  f->last_seconds = seconds;
  f->total += delta;
  return f->total;
}
