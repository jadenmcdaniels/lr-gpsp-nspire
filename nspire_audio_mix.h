/* The mixer's samples into the USB sound's ring in one pass (2026-11-17,
 * final/speed9a).
 *
 * nspire_audio_frame_end took a frame's sound in three passes over three
 * buffers: sound_read_samples (sound.c: each sample read, cleared, clamped
 * to 12 bits and scaled into a copy), the driver's rs_run (pcm.c: linear
 * interpolation, its state in memory: in and out may alias) and ring_put
 * (copied again into the ring). audio_mix does the same in one loop: a
 * frame (L and R, one word) is read from the mixer's buffer and cleared,
 * the resampler's prev and cur are registers, and each output frame is one
 * word stored into the ring. The ring's frames and the resampler's state
 * after it are those of the three passes, bit for bit (tests/audio/mix_test.c:
 * against sound_read_samples's loop, rs_run and ring_put on random input,
 * every step, wraps, priming and mono; tests/audio/run.sh (5): the five
 * scenes' sound through both at 44.1 and 48 kHz, normal and fast forward).
 *
 * audio_mix(r, g, max, mono): up to max frames read (sound_read_samples's
 * count: what it would return over calls of 512 until it returns 0), all
 * resampled into g. -1 (nothing read) when g might not hold all that comes
 * out (the three passes then drop what doesn't fit: the caller runs them),
 * or the mixer's buffer or its read position isn't word aligned. Else the
 * frames read.
 * Header-only: nspire_audio.c and the speed harness (tests/audio/perf_audio.h)
 * compile the same code. Needs pcm.h (the driver's struct ring and
 * resampler, RING_FRAMES) and sound.h (gpSP's s16, u32). */
#ifndef NSPIRE_AUDIO_MIX_H
#define NSPIRE_AUDIO_MIX_H

#include <stdint.h>

#define AUDIO_MIX_FRAMES (BUFFER_SIZE / 2)   /* the mixer's buffer in stereo frames */
#define AUDIO_MIX_CHUNK 512                  /* frame_end's sound_read_samples calls and rs_run's out_max */
#ifndef AUDIO_MIX_SEEN
#define AUDIO_MIX_SEEN(what)                 /* (tests/audio/mix_test.c counts the paths taken) */
#endif
typedef uint32_t __attribute__((may_alias)) audio_mix_word;   /* a frame: L, R (int16) */

/* sound_read_samples's sample: clamped to 12 bits, times 16 */
static inline int32_t audio_mix_in(int32_t v)
{
  if ((v >> 11) != (v >> 31))
    v = (v >> 31) ^ 2047;
  return v * 16;
}

/* (speed9a, the second step) The loops for the ARM926: input from in to
 * in_end and output from out to out_end, each contiguous (no index masks);
 * the previous frame and the step to the current one (cur = prev + d) in
 * registers. Returns 0 when the input is used up with the phase past the
 * current frame, 1 when the output is full with a frame due. Most samples
 * are within 12 bits: both halves of a frame tested at once, then two
 * shifts. */
struct audio_mix_run {
  audio_mix_word *in, *in_end, *out, *out_end;
  uint32_t phase, step;
  int32_t pl, pr, dl, dr;
};

static int __attribute__((noinline)) audio_mix_run(struct audio_mix_run *m)
{
  audio_mix_word *in = m->in, *in_end = m->in_end, *out = m->out, *out_end = m->out_end;
  uint32_t phase = m->phase, step = m->step;
  int32_t pl = m->pl, pr = m->pr, dl = m->dl, dr = m->dr;
  int full;
  for (;;) {
    while (phase >= 0x10000u) {
      uint32_t w;
      int32_t cl, cr;
      if (in == in_end) {
        full = 0;
        goto done;
      }
      w = *in;
      *in++ = 0;
      pl += dl;
      pr += dr;
      if (__builtin_expect(((w + 0x800u) & 0xF000u) | ((w + 0x08000000u) & 0xF0000000u), 0)) {
        cl = audio_mix_in((int16_t)w);
        cr = audio_mix_in((int32_t)w >> 16);
      } else {
        cl = (int32_t)(w << 16) >> 12;        /* both within 12 bits: times 16 */
        cr = ((int32_t)w >> 12) & ~15;
      }
      dl = cl - pl;
      dr = cr - pr;
      phase -= 0x10000u;
    }
    do {
      int32_t f;
      if (out == out_end) {
        full = 1;
        goto done;
      }
      f = (int32_t)(phase >> 1);
      *out++ = (uint16_t)(pl + ((dl * f) >> 15)) | (uint32_t)(pr + ((dr * f) >> 15)) << 16;
      phase += step;
    } while (phase < 0x10000u);
  }
done:
  m->in = in;
  m->out = out;
  m->phase = phase;
  m->pl = pl;
  m->pr = pr;
  m->dl = dl;
  m->dr = dr;
  return full;
}

static int audio_mix(struct resampler *r, struct ring *g, unsigned max, int mono)
{
  s16 *buffer;
  u32 base;
  uint32_t n, fi, wr, phase, step, used = 0, w, chunk_end, left;
  int32_t pl, pr, cl, cr;
  audio_mix_word *in, *out;
  unsigned free_frames;

  n = sound_samples_ready(&buffer, &base);
  if (n > max)
    n = max;
  if (!n)
    return 0;
  if (((uintptr_t)buffer & 3) || (base & 1))
    return -1;                                /* (an odd base: only from a damaged state) */
  step = r->step;
  free_frames = ring_free(g);
  /* the most frames n inputs make: each output after the first moves the
   * phase by step, at most (n + 1) whole frames in all */
  if (!free_frames || (uint64_t)(free_frames - 1) * step < ((uint64_t)(n + 1) << 16))
    return -1;
  in = (audio_mix_word *)(void *)buffer;
  out = (audio_mix_word *)(void *)g->buf;
  fi = base >> 1;
  wr = g->wr;
  phase = r->phase;
  if (!r->primed) {
    w = in[fi];
    in[fi] = 0;
    fi = (fi + 1) & (AUDIO_MIX_FRAMES - 1);
    pl = cl = audio_mix_in((int16_t)w);
    pr = cr = audio_mix_in((int32_t)w >> 16);
    used = 1;
    r->primed = 1;
    phase = 0x10000u;
  } else {
    pl = r->prev[0];
    pr = r->prev[1];
    cl = r->cur[0];
    cr = r->cur[1];
  }
  /* The three passes' calls: sound_read_samples gives 512 frames at a time
   * (a chunk); rs_run takes a chunk's frames and returns when it needs one
   * past them or has made 512 frames (out_max), then is called again: on
   * the same chunk while it has frames, else on the next. So where a call's
   * 512th frame comes as the last chunk's last frame is taken, the next
   * frame it would make waits for the next frame_end: the same here (the
   * ring's fill, which the rate reads, is then the same each frame). */
  chunk_end = n < AUDIO_MIX_CHUNK ? n : AUDIO_MIX_CHUNK;
  left = AUDIO_MIX_CHUNK;                     /* rs_run's out_max left in this call */
  if (!mono) {
    /* by audio_mix_run, between the chunks' ends, rs_run's returns at 512
     * made and the two buffers' wraps */
    struct audio_mix_run m;
    m.phase = phase;
    m.step = step;
    m.pl = pl;
    m.pr = pr;
    m.dl = cl - pl;
    m.dr = cr - pr;
    for (;;) {
      uint32_t in_n = chunk_end - used, out_n = left, k;
      audio_mix_word *in0, *out0;
      if (in_n > AUDIO_MIX_FRAMES - fi)
        in_n = AUDIO_MIX_FRAMES - fi;
      if (out_n > RING_FRAMES - (wr & (RING_FRAMES - 1)))
        out_n = RING_FRAMES - (wr & (RING_FRAMES - 1));
      m.in = in0 = in + fi;
      m.in_end = in0 + in_n;
      m.out = out0 = out + (wr & (RING_FRAMES - 1));
      m.out_end = out0 + out_n;
      if (!audio_mix_run(&m)) {               /* the input to here used up */
        k = (uint32_t)(m.in - in0);
        used += k;
        fi = (fi + k) & (AUDIO_MIX_FRAMES - 1);
        k = (uint32_t)(m.out - out0);
        wr += k;
        left -= k;
        if (used == chunk_end) {
          if (chunk_end == n)
            break;                            /* no frame more: rs_run returns */
          AUDIO_MIX_SEEN(0);
          chunk_end = n - chunk_end < AUDIO_MIX_CHUNK ? n : chunk_end + AUDIO_MIX_CHUNK;
          left = AUDIO_MIX_CHUNK;             /* rs_run on the next chunk */
        }
        continue;                             /* (or the mixer's buffer wraps) */
      }
      k = (uint32_t)(m.in - in0);
      used += k;
      fi = (fi + k) & (AUDIO_MIX_FRAMES - 1);
      k = (uint32_t)(m.out - out0);
      wr += k;
      left -= k;
      if (!left) {                            /* rs_run returned with 512 made */
        if (used == chunk_end) {
          if (chunk_end == n) {
            AUDIO_MIX_SEEN(1);
            break;                            /* no chunk more: this frame waits */
          }
          AUDIO_MIX_SEEN(2);
          chunk_end = n - chunk_end < AUDIO_MIX_CHUNK ? n : chunk_end + AUDIO_MIX_CHUNK;
        } else
          AUDIO_MIX_SEEN(3);
        left = AUDIO_MIX_CHUNK;
      }                                       /* (or the ring wraps) */
    }
    phase = m.phase;
    pl = m.pl;
    pr = m.pr;
    cl = m.pl + m.dl;
    cr = m.pr + m.dr;
    goto done;
  }
  /* the mono mix (a setting): frame by frame */
  for (;;) {
    int32_t f, ol, orr;
    while (phase >= 0x10000u) {
      if (used == chunk_end) {
        if (chunk_end == n)
          goto done;                          /* no frame more: rs_run returns */
        AUDIO_MIX_SEEN(0);
        chunk_end = n - chunk_end < AUDIO_MIX_CHUNK ? n : chunk_end + AUDIO_MIX_CHUNK;
        left = AUDIO_MIX_CHUNK;               /* rs_run on the next chunk */
      }
      pl = cl;
      pr = cr;
      w = in[fi];
      in[fi] = 0;
      fi = (fi + 1) & (AUDIO_MIX_FRAMES - 1);
      cl = audio_mix_in((int16_t)w);
      cr = audio_mix_in((int32_t)w >> 16);
      used++;
      phase -= 0x10000u;
    }
    if (!left) {                              /* rs_run returned with 512 made */
      if (used == chunk_end) {
        if (chunk_end == n) {
          AUDIO_MIX_SEEN(1);
          goto done;                          /* no chunk more: this frame waits */
        }
        AUDIO_MIX_SEEN(2);
        chunk_end = n - chunk_end < AUDIO_MIX_CHUNK ? n : chunk_end + AUDIO_MIX_CHUNK;
      } else
        AUDIO_MIX_SEEN(3);
      left = AUDIO_MIX_CHUNK;
    }
    f = (int32_t)(phase >> 1);
    ol = pl + (((cl - pl) * f) >> 15);
    orr = pr + (((cr - pr) * f) >> 15);
    ol = orr = (ol + orr) / 2;
    out[wr & (RING_FRAMES - 1)] = (uint16_t)ol | (uint32_t)orr << 16;
    wr++;
    left--;
    phase += step;
  }
done:
  r->phase = phase;
  r->prev[0] = (int16_t)pl;
  r->prev[1] = (int16_t)pr;
  r->cur[0] = (int16_t)cl;
  r->cur[1] = (int16_t)cr;
  __asm__ __volatile__("" ::: "memory");   /* the frames before the index (ring_put's order) */
  g->wr = wr;
  sound_samples_taken(used);
  return (int)used;
}

#endif
