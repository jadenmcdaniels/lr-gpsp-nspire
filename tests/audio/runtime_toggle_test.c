/* Runtime PCM policy must never change emulated FIFO/PSG time, and no
 * previous timeline may escape after mute, reset or a successful load. */
#include "common.h"
#include <stdio.h>
#include <string.h>

#ifndef NSPIRE_RUNTIME_AUDIO
#error "runtime_toggle_test requires NSPIRE_RUNTIME_AUDIO"
#endif

cpu_alert_type function_cc write_io_register16(u32 address, u32 value);
static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL runtime:%u: %s\n", __LINE__, #c); } } while (0)
#define FRAME 280896u
static void w(u32 a, u32 v) { write_io_register16(a, v); }

static void advance(unsigned frames)
{
  while (frames--) {
    cpu_ticks += FRAME;
    render_gbc_sound();
  }
}

static void tone(void)
{
  w(0x84, 0x80);
  w(0x80, 0xFF77);
  w(0x82, 2);
  w(0x60, 0);
  w(0x62, 0xF080);
  w(0x64, 0x8000 | 1024);
}

static unsigned drain_at(int expect_silence, unsigned line)
{
  s16 pcm[512];
  unsigned n, total = 0, audible = 0;
  while ((n = sound_read_samples(pcm, 256)) != 0) {
    unsigned i;
    total += n;
    for (i = 0; i < n * 2; i++)
      audible |= pcm[i] != 0;
  }
  CHECK(total > 0);
  if (expect_silence ? audible : !audible)
    printf("drain at line %u: %u frames, audible=%u, expected silence=%d\n",
           line, total, audible, expect_silence);
  CHECK(expect_silence ? !audible : audible);
  return total;
}
#define drain(silent) drain_at(silent, __LINE__)

static void empty(void)
{
  s16 pcm[2] = {123, -123};
  CHECK(sound_read_samples(pcm, 1) == 0);
  CHECK(pcm[0] == 123 && pcm[1] == -123);
}

static unsigned snapshot(u8 *out)
{
  u8 *p = out + 4, *header = out;
  p += sound_write_savestate(p);
  *p++ = 0;
  bson_write_u32(header, p - out);
  return p - out;
}

static void test_boundaries(void)
{
  static u8 state[4096], bad[4096];
  u32 generation, state_ticks;
  unsigned size;
  sound_set_output_enabled(1);
  reset_sound(); cpu_ticks = 0;
  tone();
  advance(3);
  drain(0);
  advance(2);                     /* leave audible PCM queued */
  sound_set_output_enabled(0);
  advance(3);                     /* emulated APU keeps advancing */
  sound_set_output_enabled(1);
  empty();                         /* muted timeline never becomes backlog */
  w(0x84, 0);
  advance(3);
  drain(1);
  tone(); advance(3); drain(0);     /* output resumes without reinitializing */
  advance(2);
  sound_set_output_enabled(1);     /* idempotent: preserve current PCM */
  drain(0);
  advance(2);
  sound_discard_samples();
  empty();

  state_ticks = cpu_ticks;
  size = snapshot(state);
  CHECK(sound_check_savestate(state));
  advance(2);
  generation = sound_output_generation;
  memcpy(bad, state, size);
  u8 *on = (u8 *)bson_find_key(bson_find_document(bad, "sound"), "on");
  CHECK(on != NULL);
  bson_write_u32(on, 2);
  CHECK(!sound_read_savestate(bad));
  CHECK(sound_output_generation == generation);
  drain(0);                        /* rejected load preserves the live timeline */
  advance(2);
  CHECK(sound_read_savestate(state));
  cpu_ticks = state_ticks;           /* full state loading also restores CPU time */
  CHECK(sound_output_generation == generation + 1);
  empty();
  w(0x84, 0); advance(3); drain(1);

  tone(); advance(2);
  generation = sound_output_generation;
  reset_sound(); cpu_ticks = 0;
  CHECK(sound_output_generation == generation + 1);
  empty();
  advance(3); drain(1);
  sound_set_output_enabled(0);
  generation = sound_output_generation;
  reset_sound(); cpu_ticks = 0;
  CHECK(sound_output_generation == generation + 1);
  tone(); advance(4);
  CHECK(gbc_sound_buffer_index != 0 && gbc_sound_last_cpu_ticks == cpu_ticks);
  sound_set_output_enabled(1);
  empty();
  advance(3); drain(0);
}

static void test_fifo_transition(void)
{
  static const u32 steps[] = {0x555555, 0x1000000, 0xFFFFFFFFu, 0x800001};
  u32 frac = 0, index = 0, base = 0;
  unsigned s, n;
  reset_sound(); cpu_ticks = 0;
  sound_set_output_enabled(0);
  for (s = 0; s < sizeof steps / sizeof steps[0]; s++) {
    for (n = 0; n < 5; n++) {
      unsigned long long sum = frac;
      sound_timer_queue32(0, 0x10203040u + n);
      sound_timer(steps[s], 0);
      do { sum += steps[s]; index = (index + 2) & BUFFER_SIZE_MASK; } while (sum <= 0xFFFFFFu);
      frac = (u32)sum & 0xFFFFFFu;
      base = (base + 1) & 31;
    }
    CHECK(direct_sound_channel[0].pops != 0);
    sound_set_output_enabled(1);
    CHECK(direct_sound_channel[0].pops == 0);
    CHECK(direct_sound_channel[0].fifo_fractional == frac);
    CHECK(direct_sound_channel[0].buffer_index == index);
    CHECK(direct_sound_channel[0].fifo_base == base);
    sound_set_output_enabled(0);
  }
  sound_set_output_enabled(1);
}

int main(void)
{
  unsigned i;
  init_sound();
  for (i = 0; i < 4; i++) dma[i].direct_sound_channel = DMA_NO_DIRECT_SOUND;
  test_boundaries();
  test_fifo_transition();
  printf("runtime audio: %u checks, %u failed\n", checks, failures);
  return failures != 0;
}
