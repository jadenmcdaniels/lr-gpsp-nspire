/* What the USB audio path costs a frame besides the mixer (2026-11-15):
 * the speed model's harness for tests/audio/rsbench.sh. Per GBA frame it
 * does what nspire_audio_frame_end does with the mixer's samples (1097
 * stereo frames at 65536 Hz, read in chunks of 512: the resampler to
 * 44100 Hz at a rate factor, the mono mix if asked, the ring) and what the
 * USB task does with them (16.74 ms of 1 ms packets: the packet schedule,
 * the ring read, the device's format at a volume). The regions (rs_run,
 * ring_put, ring_get, pcm_write) are the model's parts; the rest is the
 * loop's own. Built with the driver's pcm.c as gpSP builds it.
 *   rsbench FRAMES MONO RATE_PERCENT */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcm.h"

void __attribute__((noinline)) perf_frame_mark(void)
{
  __asm__ volatile ("" ::: "memory");
}

static struct ring ring;
static struct resampler rs;
static struct pkt_sched sched;
static int16_t src[1200 * 2];
static uint8_t pkt[200 * 4];

/* nspire_audio_frame_end's loop, the mixer's read left out (tests/audio/speed.sh has it) */
static void __attribute__((noinline)) produce(int mono)
{
  int16_t out[512 * 2];
  unsigned left = 1097, at = 0;
  while (left) {
    unsigned n = left > 512 ? 512 : left, used = 0;
    while (used < n) {
      unsigned consumed = n - used;
      unsigned made = rs_run(&rs, src + 2 * (at + used), &consumed, out, 512);
      used += consumed;
      if (mono)
        for (unsigned i = 0; i < made; i++)
          out[2*i] = out[2*i+1] = ((int32_t)out[2*i] + out[2*i+1]) / 2;
      ring_put(&ring, out, made);
      if (!made && !consumed) break;
    }
    at += n;
    left -= n;
  }
}

/* ua_os.c's refill, a frame's worth of packets */
static void __attribute__((noinline)) consume(const struct dev_fmt *f, unsigned ms)
{
  int16_t lr[200 * 2];
  for (unsigned p = 0; p < ms; p++) {
    unsigned n = pkt_next(&sched), got = ring_get(&ring, lr, n), b;
    b = pcm_write(f, lr, got, 179, pkt);
    pcm_silence(f, n - got, pkt + b);
  }
}

int main(int argc, char **argv)
{
  int frames = argc > 1 ? atoi(argv[1]) : 600, mono = argc > 2 ? atoi(argv[2]) : 0;
  int percent = argc > 3 ? atoi(argv[3]) : 100;
  struct dev_fmt f = { 2, 2, 0 };
  uint32_t acc = 0;
  for (unsigned i = 0; i < sizeof(src) / sizeof(src[0]); i++)
    src[i] = (int16_t)((i * 2654435761u) >> 20);
  ring_init(&ring);
  rs_init(&rs, 65536, 44100);
  rs.step = (uint32_t)((uint64_t)rs.step * (uint32_t)percent / 100);
  pkt_init(&sched, 44100);
  for (int fr = 0; fr < frames; fr++) {
    perf_frame_mark();
    produce(mono);
    acc += 16743;                       /* us a GBA frame: 280896 / 16.777216 */
    consume(&f, acc / 1000);
    acc %= 1000;
    if (ring_used(&ring) > RING_FRAMES / 2)   /* (a rate factor not 1: keep the ring from filling) */
      ring_request_clear(&ring);
  }
  perf_frame_mark();
  printf("rsbench %d frames, ring %u\n", frames, ring_used(&ring));
  return 0;
}
