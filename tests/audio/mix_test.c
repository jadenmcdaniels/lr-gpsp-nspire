/* nspire_audio_mix.h's audio_mix against what it replaces (2026-11-17,
 * final/speed9a): nspire_audio_frame_end's three passes, sound_read_samples's
 * loop (sound.c, copied: the mixer's buffer read, cleared, clamped, times 16,
 * in calls of 512 frames, 8 at most), the driver's rs_run and ring_put (its
 * pcm.c, linked) and the mono mix. Both run from the same state on random
 * frames (every 16-bit value, near the clamp's ends, silence), the mixer's
 * buffer wrapping, the resampler unprimed and primed at every phase, steps
 * from 1 to 16x fast forward's and the calculators' 44.1 and 48 kHz ones, the
 * ring's index wrapping, mono or not; then frames in a row with the state
 * carried. Where audio_mix runs (it returns the frames read) everything must
 * be equal after: the ring's words and index, the resampler (phase, prev,
 * cur, primed), the mixer's buffer and base. Where it says -1 (the ring
 * might not hold its frames), nothing may have changed, and the ring then
 * can't have held them all. Built by tests/audio/mix.mk, run by
 * tests/host/run.sh with the sink tests. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcm.h"

typedef signed short int s16;
typedef unsigned int u32;
#define BUFFER_SIZE (1 << 16)
#define BUFFER_SIZE_MASK (BUFFER_SIZE - 1)

/* The mixer: a buffer, its read base, the write index (gbc_sound_buffer_index) */
struct mixer {
  s16 *buf;
  u32 base, top;
};
static s16 buf_a[BUFFER_SIZE] __attribute__((aligned(4))), buf_b[BUFFER_SIZE] __attribute__((aligned(4)));
static s16 buf_odd[BUFFER_SIZE + 2] __attribute__((aligned(4)));
static struct mixer *cur_mixer;

u32 sound_samples_ready(s16 **buffer, u32 *base);
void sound_samples_taken(u32 frames);
/* the paths audio_mix took: the next chunk (input), a frame waiting for the
 * next call (512 made as the last frame went), the next chunk (512 made as a
 * chunk's last went), the same chunk again (512 made) */
static unsigned seen[4];
#define AUDIO_MIX_SEEN(what) (seen[what]++)
#include "../../nspire_audio_mix.h"

/* sound.c's, on cur_mixer */
u32 sound_samples_ready(s16 **buffer, u32 *base)
{
  u32 a = (cur_mixer->top - cur_mixer->base) & BUFFER_SIZE_MASK;
  *buffer = cur_mixer->buf;
  *base = cur_mixer->base;
  return a > 512 ? (a - 512) >> 1 : 0;
}

void sound_samples_taken(u32 frames)
{
  cur_mixer->base = (cur_mixer->base + frames * 2) & BUFFER_SIZE_MASK;
}

/* sound.c's sound_read_samples, on a mixer */
static u32 read_samples(struct mixer *m, s16 *out, u32 frames)
{
  u32 i, samples_to_read;
  u32 samples_available = (m->top - m->base) & BUFFER_SIZE_MASK;
  if (!out || !frames)
    return 0;
  samples_available = (samples_available > 512) ? (samples_available - 512) : 0;
  samples_available = (samples_available >> 1) << 1;
  samples_to_read = frames > samples_available / 2 ? samples_available : frames * 2;
  for (i = 0; i < samples_to_read; i++) {
    u32 source_index = (m->base + i) & BUFFER_SIZE_MASK;
    int32_t current_sample = m->buf[source_index];
    m->buf[source_index] = 0;
    if (current_sample > 2047)
      current_sample = 2047;
    if (current_sample < -2048)
      current_sample = -2048;
    out[i] = current_sample * 16;
  }
  m->base += samples_to_read;
  m->base &= BUFFER_SIZE_MASK;
  return samples_to_read >> 1;
}

/* nspire_audio_frame_end's three passes (no fade, nothing left out) */
static unsigned three_passes(struct mixer *m, struct resampler *rs, struct ring *g, int mono, unsigned *dropped)
{
  int16_t in[512 * 2], out[512 * 2];
  unsigned total = 0;
  for (unsigned chunk = 0; chunk < 8; chunk++) {
    unsigned n = read_samples(m, in, 512), used = 0;
    if (!n) break;
    total += n;
    while (used < n) {
      unsigned consumed = n - used;
      unsigned made = rs_run(rs, in + 2 * used, &consumed, out, 512);
      used += consumed;
      if (mono)
        for (unsigned i = 0; i < made; i++)
          out[2*i] = out[2*i+1] = ((int32_t)out[2*i] + out[2*i+1]) / 2;
      *dropped += made - ring_put(g, out, made);
      if (!made && !consumed) break;
    }
  }
  return total;
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void)
{
  rng ^= rng << 13;
  rng ^= rng >> 7;
  rng ^= rng << 17;
  return (uint32_t)(rng >> 16);
}

static struct mixer ma = { buf_a, 0, 0 }, mb = { buf_b, 0, 0 };
static struct ring ga, gb;
static unsigned checks, fails, ran, refused;

static void check(int ok, const char *what, int k)
{
  checks++;
  if (!ok && fails++ < 20)
    printf("FAIL  case %d: %s\n", k, what);
}

static s16 sample(int kind)
{
  switch (kind) {
  case 0: return (s16)rnd();                                   /* anything */
  case 1: return (s16)((int)(rnd() % 64) - 32 + ((rnd() & 1) ? 2047 : -2048));   /* the clamp's ends */
  case 2: return 0;
  default: return (s16)((int)(rnd() % 4096) - 2048);            /* in range */
  }
}

static uint32_t pick_step(void)
{
  static const uint32_t steps[] = { 1, 2, 3, 0x8000, 0xffff, 0x10000, 0x10001, 97391, 89478, 97391 * 2, 89478 * 2,
                                    97391 * 16, 89478 * 16, 97391 / 4, 89478 / 4, 0x200000 };
  switch (rnd() % 4) {
  case 0: return steps[rnd() % (sizeof steps / sizeof steps[0])];
  case 1: return 1 + rnd() % 0x30000;
  case 2: return 80000 + rnd() % 20000;
  default: return 1 + (rnd() % 0x1000000);
  }
}

static int sweep_n = -1;            /* a directed case: n frames, this step and phase */
static uint32_t sweep_step, sweep_phase;

static void one_case(int k)
{
  struct resampler ra, rb;
  unsigned dropped, frames_in_row = sweep_n >= 0 ? 3 : 1 + (rnd() % 4 ? 0 : rnd() % 6);
  int mono = rnd() & 1, kind = rnd() % 4;
  uint32_t step = sweep_n >= 0 ? sweep_step : pick_step();

  /* the mixer: a random fill, the base anywhere (even), up to ~2 frames' worth available */
  ma.base = (rnd() & BUFFER_SIZE_MASK) & ~1u;
  if (rnd() % 8 == 0)
    ma.base = (BUFFER_SIZE - 2 * (rnd() % 600)) & BUFFER_SIZE_MASK;   /* near the wrap */
  ma.top = (ma.base + 2 * (rnd() % 5000)) & BUFFER_SIZE_MASK;
  if (sweep_n >= 0)
    ma.top = (ma.base + 512 + 2 * sweep_n) & BUFFER_SIZE_MASK;
  for (u32 i = 0; i < 10000; i++)                                      /* (what can be read) */
    ma.buf[(ma.base + i) & BUFFER_SIZE_MASK] = sample(kind);

  /* the resampler */
  rs_init(&ra, 65536, (rnd() & 1) ? 44100 : 48000);
  ra.step = step;
  if (rnd() % 4) {
    ra.primed = 1;
    ra.phase = rnd() % (0x10000u + step);
    ra.prev[0] = (int16_t)(audio_mix_in((s16)rnd()));
    ra.prev[1] = (int16_t)(audio_mix_in((s16)rnd()));
    ra.cur[0] = (int16_t)(audio_mix_in((s16)rnd()));
    ra.cur[1] = (int16_t)(audio_mix_in((s16)rnd()));
    if (rnd() % 8 == 0)
      ra.phase = 0x10000u + rnd() % 0x30000u;   /* (a phase past the next input: after a call that ran out) */
  }
  if (sweep_n >= 0) {
    ra.primed = 1;
    ra.phase = sweep_phase;
  }

  /* the ring: its index anywhere, some of it unread */
  for (unsigned i = 0; i < RING_FRAMES * 2; i++)
    ga.buf[i] = (int16_t)rnd();
  ga.wr = rnd();
  ga.rd = ga.wr - (rnd() % 4 ? rnd() % 4000 : rnd() % (RING_FRAMES + 1));
  ga.clear_wr = ga.clear_seq = ga.cleared_seq = 0;

  memcpy(mb.buf, ma.buf, sizeof buf_a);
  mb.base = ma.base;
  mb.top = ma.top;
  gb = ga;
  rb = ra;
  for (unsigned f = 0; f < frames_in_row; f++) {
    int got;
    dropped = 0;
    cur_mixer = &mb;
    got = audio_mix(&rb, &gb, 4096, mono);
    if (got < 0) {
      unsigned n;
      refused++;
      check(!memcmp(&gb, &ga, sizeof ga) && !memcmp(&rb, &ra, sizeof ra) && mb.base == ma.base &&
            !memcmp(mb.buf, ma.buf, sizeof buf_a), "refused, yet something changed", k);
      n = three_passes(&ma, &ra, &ga, mono, &dropped);
      check(n > 0, "refused with nothing to read", k);
      /* (the three passes ran on a: b takes a's state, then on equal) */
      memcpy(mb.buf, ma.buf, sizeof buf_a);
      mb.base = ma.base;
      gb = ga;
      rb = ra;
    } else {
      unsigned n = three_passes(&ma, &ra, &ga, mono, &dropped);
      ran++;
      check((unsigned)got == n, "frames read", k);
      check(dropped == 0, "the three passes dropped frames where audio_mix ran", k);
      if (getenv("MIX_DEBUG") && (ga.wr != gb.wr || ra.phase != rb.phase))
        printf("case %d frame %u: step %u mono %d kind %d got %d n %u; wr %u/%u phase %u/%u primed %d/%d\n", k, f,
               (unsigned)step, mono, kind, got, n, (unsigned)ga.wr, (unsigned)gb.wr, (unsigned)ra.phase,
               (unsigned)rb.phase, ra.primed, rb.primed);
      check(ga.wr == gb.wr && ga.rd == gb.rd, "ring index", k);
      check(!memcmp(ga.buf, gb.buf, sizeof ga.buf), "ring frames", k);
      check(ra.phase == rb.phase && ra.step == rb.step && ra.primed == rb.primed &&
            ra.prev[0] == rb.prev[0] && ra.prev[1] == rb.prev[1] && ra.cur[0] == rb.cur[0] &&
            ra.cur[1] == rb.cur[1], "resampler state", k);
      check(ma.base == mb.base && ma.top == mb.top, "mixer base", k);
      check(!memcmp(ma.buf, mb.buf, sizeof buf_a), "mixer buffer", k);
    }
    /* the next frame: more mixed, some of the ring played */
    {
      u32 more = 2 * (rnd() % 1500);
      for (u32 i = 0; i < more; i++) {
        s16 v = sample(kind);
        ma.buf[(ma.top + i) & BUFFER_SIZE_MASK] = v;
        mb.buf[(mb.top + i) & BUFFER_SIZE_MASK] = v;
      }
      ma.top = mb.top = (ma.top + more) & BUFFER_SIZE_MASK;
      if (rnd() & 1) {
        unsigned used = ring_used(&ga), take = used ? rnd() % (used + 1) : 0;
        ga.rd += take;
        gb.rd += take;
      }
    }
  }
}

int main(int argc, char **argv)
{
  int n = argc > 1 ? atoi(argv[1]) : 1500;
  for (int k = 0; k < n; k++)
    one_case(k);
  /* where a call's 512th frame comes with the input's last (the frame that
   * waits): n frames read, steps around n / 512 of a frame, phases near 0 */
  for (int fr = 1; fr <= 48; fr++)
    for (int d = -24; d <= 24; d += 3)
      for (uint32_t ph = 0; ph < 0x10000u; ph += 0x5555u) {
        int32_t st = (int32_t)(((uint32_t)fr << 16) / 512) + d;
        sweep_n = fr;
        sweep_step = st > 0 ? (uint32_t)st : 1;
        sweep_phase = ph;
        one_case(n);
      }
  sweep_n = -1;
  /* a mixer buffer at an odd halfword is refused (its words would be wrong) */
  {
    struct resampler r;
    struct mixer odd = { buf_odd + 1, 0, 4000 };
    rs_init(&r, 65536, 48000);
    ring_init(&ga);
    cur_mixer = &odd;
    check(audio_mix(&r, &ga, 4096, 0) == -1 && ga.wr == 0 && odd.base == 0 && !r.primed,
          "unaligned buffer refused", -1);
  }
  /* an odd read position (only from a damaged state) is refused too */
  {
    struct resampler r;
    struct mixer oddbase = { buf_a, 7, 4007 };
    rs_init(&r, 65536, 48000);
    ring_init(&ga);
    cur_mixer = &oddbase;
    check(audio_mix(&r, &ga, 4096, 0) == -1 && ga.wr == 0 && oddbase.base == 7 && !r.primed,
          "odd read position refused", -1);
  }
  for (int i = 0; i < 4; i++)
    check(seen[i] > 0, "a path of audio_mix's never taken", -2 - i);
  printf("mix_test: %u cases ran audio_mix, %u refused (ring too full); paths %u next chunk, %u waits, "
         "%u next chunk at 512, %u same chunk at 512; %u checks, %u failed\n", ran, refused, seen[0], seen[1],
         seen[2], seen[3], checks, fails);
  return fails != 0;
}
