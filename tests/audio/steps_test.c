/*
 * Exhaustive check of libretro-gpsp/sound_steps.h (the integer frequency
 * steps both builds use) against the float formulas they replaced (the audio
 * build's old code, kept here as the reference). Host program:
 *
 *   gcc -O0 -std=gnu99 -msse2 -mfpmath=sse -ffp-contract=off steps_test.c
 *
 * (single precision float and double as on the calculator's soft float:
 * IEEE, round to nearest.) Prints, per formula, how many inputs were tried,
 * how many differ and the largest difference. Every reload, including 1
 * and 65536, is checked against the exact rational formula; unlike the old
 * unfinished proof, valid small timer reloads are not excluded.
 * -DOVERCLOCK_60FPS tests the other timer base.
 */
#include <stdio.h>
#include <stdlib.h>

typedef unsigned int u32;
typedef u32 fixed16_16;
typedef u32 fixed8_24;
#define GBA_SOUND_FREQUENCY (64 * 1024)
#ifdef OVERCLOCK_60FPS
  #define GBC_BASE_RATE ((float)(60 * 228 * (272+960)))
  #define GBC_BASE_RATE_INT (60u * 228u * (272u + 960u))
#else
  #define GBC_BASE_RATE ((float)(16 * 1024 * 1024))
  #define GBC_BASE_RATE_INT (16u * 1024u * 1024u)
#endif
#define float_to_fp16_16(value) (fixed16_16)((value) * 65536.0)
#define float_to_fp8_24(value)  (fixed8_24)((value) * 16777216.0)
static const u32 sound_frequency = GBA_SOUND_FREQUENCY;

#include "../../libretro-gpsp/sound_steps.h"

/* The float formulas, as in fbfb478 */
static u32 f_square(u32 rate) { return float_to_fp16_16(((131072.0 / (2048 - (rate))) * 8.0) / sound_frequency); }
static u32 f_sweep(u32 rate)  { return float_to_fp16_16(((131072.0f / (2048 - rate)) * 8.0f) / sound_frequency); }
static u32 f_wave(u32 rate)   { return float_to_fp16_16((2097152.0 / (2048 - (rate))) / sound_frequency); }
static u32 f_noise(u32 dr, u32 fs)
{
  return dr == 0 ? float_to_fp16_16(1048576.0 / (1 << (fs + 1)) / sound_frequency)
                 : float_to_fp16_16(524288.0 / (dr * (1 << (fs + 1))) / sound_frequency);
}
static u32 f_timer(u32 timer_reload)
{
  double v = ((GBC_BASE_RATE / sound_frequency) / (timer_reload)) * 16777216.0;
  return v >= 4294967295.0 ? 0xFFFFFFFFu : (u32)v;
}
static u32 f_phase(void) { return float_to_fp16_16(1.0 / 12.0); }
static u32 f_tick(void)  { return float_to_fp16_16(256.0f / sound_frequency); }

static int bad;
struct stat { const char *name; long n, diff, max; const char *where; char first[64]; };
static void note(struct stat *s, u32 a, u32 b, const char *fmt, u32 x, u32 y, u32 z)
{
  long d = (long)a - (long)b;
  if (d < 0) d = -d;
  s->n++;
  if (d) {
    if (!s->diff) snprintf(s->first, sizeof s->first, fmt, x, y, z);
    s->diff++;
    if (d > s->max) s->max = d;
  }
}
static void show(struct stat *s)
{
  printf("%-34s inputs %8ld  differ %6ld  max diff %ld%s%s\n", s->name, s->n, s->diff, s->max,
         s->diff ? "  first at " : "", s->diff ? s->first : "");
}

int main(void)
{
  struct stat sq = {"square rate 0..2047"}, sw = {"sweep (float32 formula) rate 0..2047"},
              wv = {"wave rate 0..2047"}, nz = {"noise ratio 0..7 x shift 0..15"};
  struct stat tm[4] = {{"timer shifted reload 1.., prescale 1"}, {"timer shifted reload 1.., prescale 64"},
                       {"timer shifted reload 1.., prescale 256"}, {"timer shifted reload 1.., prescale 1024"}};
  static const u32 pre[4] = {0, 6, 8, 10};
  u32 r, dr, fs, p;
  long tm_hi_max = 0;

  for (r = 0; r < 2048; r++) {
    note(&sq, nspire_fp16_tone_square_channel(r), f_square(r), "rate %u%.0u%.0u", r, 0, 0);
    note(&sw, nspire_fp16_tone_square_channel(r), f_sweep(r), "rate %u%.0u%.0u", r, 0, 0);
    note(&wv, nspire_fp16_tone_wave_channel(r), f_wave(r), "rate %u%.0u%.0u", r, 0, 0);
  }
  for (dr = 0; dr < 8; dr++)
    for (fs = 0; fs < 16; fs++)
      note(&nz, nspire_fp16_noise_step(dr, fs), f_noise(dr, fs), "ratio %u shift %u%.0u", dr, fs, 0);
  for (p = 0; p < 4; p++)
    for (r = 1; r <= 65536; r++) {
      u32 t = r << pre[p];
      u32 a = nspire_fp8_timer_reload_step(t), b;
      unsigned long long exact = ((unsigned long long)GBC_BASE_RATE_INT << 24) /
                                 ((unsigned long long)GBA_SOUND_FREQUENCY * t);
      if (exact > 0xFFFFFFFFull) exact = 0xFFFFFFFFull;
      if (a != exact) bad = 1;
      b = f_timer(t);
      note(&tm[p], a, b, "timer_reload %u%.0u%.0u", t, 0, 0);
      if (a != b && (long)(a > b ? a - b : b - a) > 1 && t > 255) tm_hi_max++;
    }
  printf("phase 1/12: int %u float %u   tick step: int %u float %u\n", NSPIRE_FP16_ONE_TWELFTH, f_phase(),
         (u32)((256ull << 16) / (unsigned long long)GBA_SOUND_FREQUENCY), f_tick());
  if (NSPIRE_FP16_ONE_TWELFTH != f_phase()) bad = 1;
  if ((u32)((256ull << 16) / (unsigned long long)GBA_SOUND_FREQUENCY) != f_tick()) bad = 1;
  show(&sq); show(&sw); show(&wv); show(&nz);
  for (p = 0; p < 4; p++) show(&tm[p]);
  if (sq.diff || wv.diff || nz.diff) bad = 1;
  if (sw.max > 1) bad = 1;
  printf("reload 1 saturates at UINT32_MAX; reload 0 produces no samples\n");
  if (nspire_fp8_timer_reload_step(1) != 0xFFFFFFFFu || nspire_fp8_timer_reload_step(0)) bad = 1;
  printf("timer inputs (timer_reload>255) differing by more than 1: %ld\n", tm_hi_max);
  /* The legacy float32 division rounds by up to half an ULP: 43 LSBs
   * at reload 3 (normal clock). Integer arithmetic is the exact floor.
   * Report every difference rather than claiming the impossible <=1
   * error bound for these valid short periods. */
  printf(bad ? "FAIL steps\n" : "PASS steps\n");
  return bad;
}
