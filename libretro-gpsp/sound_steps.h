/*
 * The mixer's frequency steps, integer only (no soft float on the ARM926).
 * One set of formulas for the NSPIRE_NO_AUDIO build and the mixing build, so
 * save states hold the same step values in both. Included by sound.h.
 * tests/audio/steps_test.c compares them with the float formulas they replace.
 */
#ifndef SOUND_STEPS_H
#define SOUND_STEPS_H

#define NSPIRE_FP16_ONE_TWELFTH ((fixed16_16)(65536u / 12u))

/* square channel: (131072 / (2048 - rate)) * 8 / 65536 Hz, 16.16 */
static inline fixed16_16 nspire_fp16_tone_square_channel(u32 rate)
{
  u32 den = 2048u - (rate & 2047u);
  return (fixed16_16)(1048576u / den);
}

static inline fixed16_16 nspire_fp16_tone_wave_channel(u32 rate)
{
  u32 den = 2048u - (rate & 2047u);
  return (fixed16_16)(2097152u / den);
}

static inline fixed16_16 nspire_fp16_noise_step(u32 dividing_ratio, u32 frequency_shift)
{
  u32 sh = (frequency_shift & 15u) + 1u;
  u32 divisor = 1u << sh;

  if (dividing_ratio == 0u)
    return (fixed16_16)(1048576u / divisor);
  return (fixed16_16)(524288u / (dividing_ratio * divisor));
}

static inline fixed8_24 nspire_fp8_timer_reload_step(u32 timer_reload)
{
  unsigned long long num;

  if (timer_reload == 0u)
    return (fixed8_24)0;
  /* Same as float_to_fp8_24((GBC_BASE_RATE / sound_frequency) / timer_reload). */
  num = (unsigned long long)GBC_BASE_RATE_INT * 16777216ull /
        (unsigned long long)GBA_SOUND_FREQUENCY;
  num /= timer_reload;
  /* Reload 1 exceeds 8.24's range (also under OVERCLOCK_60FPS).
   * Wrapping it to zero made the mixer loop forever. Saturate, as ARM's
   * former float-to-unsigned conversion did, without undefined C casts. */
  return (fixed8_24)(num > 0xFFFFFFFFull ? 0xFFFFFFFFu : num);
}

#define gbc_fp16_tone_square(rate)     nspire_fp16_tone_square_channel(rate)
#define gbc_fp16_tone_wave(rate)       nspire_fp16_tone_wave_channel(rate)
#define gbc_fp16_noise(dr, fs)         nspire_fp16_noise_step((dr), (fs))
#define gbc_fp16_sample_phase12()      NSPIRE_FP16_ONE_TWELFTH
#define sound_update_frequency_step(timer_number)                             \
  timer[timer_number].frequency_step = nspire_fp8_timer_reload_step(timer_reload)

#endif
