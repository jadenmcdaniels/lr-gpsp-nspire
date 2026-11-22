/*
 * The audio build's part of the speed harness (tests/arm/perf.c, built by
 * "make AUDIO=1 build/perf_audio": the core without -DNSPIRE_NO_AUDIO, so it
 * mixes). Included by perf.c under PERF_AUDIO only.
 *
 * Each frame ends by draining the mixer with sound_read_samples the way a
 * frontend would (until it returns 0). The drained samples are hashed
 * (FNV-1a over the s16 values, little endian, all frames so far; printed as
 * "audio FRAME HASH SAMPLES" with the picture hashes) and, with wav=PATH,
 * written to a WAV file (16 bit stereo, perf_audio_rate Hz); wavfrom=N and
 * wavto=M limit the file to the frames N to M (the hash is of all of them).
 *
 * perf_audio_frame is a region of the speed model ("harness", left out of
 * the model's totals: calc_speed.py), so the harness's own hashing and
 * file writing does not count as the game's or the mixer's cost.
 * sound_read_samples is called through a pointer so that link-time
 * optimization can not fold it into the harness: the model finds it as its
 * own function (part "audio").
 *
 * rs=RATE (2026-11-17, final/speed9a): what nspire_audio_frame_end does with
 * the mixer's sound on the calculator: each frame resampled to RATE Hz into
 * the USB sound's ring (the driver's struct ring, RING_FRAMES 8192), one pass
 * (nspire_audio_mix.h's audio_mix, the calculator's), or with rsold=1 the
 * three passes it replaced (sound_read_samples in 512s, the driver's rs_run,
 * ring_put; also where audio_mix says -1). rsx=PERCENT: the step times
 * PERCENT/100 (fast forward at 2x: 200; the calculator's rate factor),
 * mono=1: the mono mix. The ring is then drained and its frames hashed
 * instead of the mixer's: the same "audio" lines, so the two equal each
 * other (tests/audio/run.sh's rs settings). perf_audio_resample is a region
 * of its own (part "audio", with rs_run, ring_put and sound_read_samples).
 */

#define PERF_AUDIO_CHUNK 256   /* frames per read */

static FILE *perf_audio_wav;
static u32 perf_audio_hash = 2166136261u;
static u32 perf_audio_samples;       /* s16 values drained so far */
static u32 perf_audio_wav_samples;   /* s16 values in the WAV file */
static int perf_audio_wav_from = 1, perf_audio_wav_to = 0x7FFFFFFF;
static u32 perf_audio_rate = GBA_SOUND_FREQUENCY;
static u32 perf_audio_channels = 2;
static u32 (*volatile perf_audio_read)(s16 *out, u32 frames) = sound_read_samples;

#ifdef PERF_AUDIO_RS
#include "pcm.h"
#include "../../nspire_audio_mix.h"
static struct ring perf_rs_ring;
static struct resampler perf_rs;
static int perf_rs_on, perf_rs_old, perf_rs_mono;
static unsigned perf_rs_mixed, perf_rs_passes;

/* nspire_audio_frame_end's sound: a frame's into the ring */
void __attribute__((noinline)) perf_audio_resample(void)
{
  int16_t in[512 * 2], out[512 * 2];
  if (!perf_rs_old && audio_mix(&perf_rs, &perf_rs_ring, 8 * 512, perf_rs_mono) >= 0)
  {
    perf_rs_mixed++;
    return;
  }
  perf_rs_passes++;
  for (unsigned chunk = 0; chunk < 8; chunk++)
  {
    unsigned n = perf_audio_read(in, 512), used = 0;
    if (!n)
      break;
    while (used < n)
    {
      unsigned consumed = n - used;
      unsigned made = rs_run(&perf_rs, in + 2 * used, &consumed, out, 512);
      used += consumed;
      if (perf_rs_mono)
        for (unsigned i = 0; i < made; i++)
          out[2*i] = out[2*i+1] = ((int32_t)out[2*i] + out[2*i+1]) / 2;
      ring_put(&perf_rs_ring, out, made);
      if (!made && !consumed)
        break;
    }
  }
}

static void (*volatile perf_audio_resample_p)(void) = perf_audio_resample;
#endif

static void perf_audio_put32(unsigned char *p, u32 v)
{
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

/* A 44 byte WAV header for "bytes" of PCM (rewritten at the end). */
static void perf_audio_header(u32 bytes)
{
  unsigned char h[44];
  memcpy(h, "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\1\0\2\0", 22);
  h[22] = perf_audio_channels; h[23] = 0;
  perf_audio_put32(h + 24, perf_audio_rate);
  perf_audio_put32(h + 28, perf_audio_rate * perf_audio_channels * 2);
  h[32] = perf_audio_channels * 2; h[33] = 0; h[34] = 16; h[35] = 0;
  memcpy(h + 36, "data", 4);
  perf_audio_put32(h + 4, 36 + bytes);
  perf_audio_put32(h + 40, bytes);
  fwrite(h, 1, 44, perf_audio_wav);
}

static void perf_audio_init(int argc, char **argv)
{
  const char *path = text_setting(argc, argv, "wav");
  perf_audio_wav_from = setting(argc, argv, "wavfrom", 1);
  perf_audio_wav_to = setting(argc, argv, "wavto", 0x7FFFFFFF);
#ifdef PERF_AUDIO_RS
  if (setting(argc, argv, "rs", 0) > 0)
  {
    perf_rs_on = 1;
    perf_rs_old = setting(argc, argv, "rsold", 0);
    perf_rs_mono = setting(argc, argv, "mono", 0);
    perf_audio_rate = setting(argc, argv, "rs", 0);
    ring_init(&perf_rs_ring);
    rs_init(&perf_rs, GBA_SOUND_FREQUENCY, perf_audio_rate);
    perf_rs.step = (u32)((unsigned long long)perf_rs.step * setting(argc, argv, "rsx", 100) / 100);
    if (!perf_rs.step)
      perf_rs.step = 1;
  }
#endif
  if (path)
  {
    perf_audio_wav = fopen(path, "wb");
    if (!perf_audio_wav)
      fprintf(stderr, "can't write %s\n", path);
    else
      perf_audio_header(0);
  }
}

/* The end of a frame: drain the mixer; at a hash frame print the hashes. */
void __attribute__((noinline)) perf_audio_frame(int frame, int print)
{
  static s16 buf[PERF_AUDIO_CHUNK * 2];
  u32 n, i;

#ifdef PERF_AUDIO_RS
  if (perf_rs_on)
    perf_audio_resample_p();
#endif
  while ((n =
#ifdef PERF_AUDIO_RS
          perf_rs_on ? ring_get(&perf_rs_ring, buf, PERF_AUDIO_CHUNK) :
#endif
          perf_audio_read(buf, PERF_AUDIO_CHUNK)) != 0)
  {
    u32 count = n * perf_audio_channels;
    const unsigned char *p = (const unsigned char *)buf;
    u32 h = perf_audio_hash;
    for (i = 0; i < count * 2; i++)
      h = (h ^ p[i]) * 16777619u;
    perf_audio_hash = h;
    perf_audio_samples += count;
    if (perf_audio_wav && frame >= perf_audio_wav_from && frame <= perf_audio_wav_to)
    {
      fwrite(buf, 2, count, perf_audio_wav);
      perf_audio_wav_samples += count;
    }
  }
  if (print)
    printf("audio %d %08x %u\n", frame, (unsigned)perf_audio_hash, (unsigned)perf_audio_samples);
}

static void perf_audio_finish(void)
{
#ifdef PERF_AUDIO_RS
  if (perf_rs_on)
    printf("resampled to %u Hz: %u frames in one pass, %u in three\n", (unsigned)perf_audio_rate,
           perf_rs_mixed, perf_rs_passes);
#endif
  if (perf_audio_wav)
  {
    fseek(perf_audio_wav, 0, SEEK_SET);
    perf_audio_header(perf_audio_wav_samples * 2);
    fclose(perf_audio_wav);
  }
}
