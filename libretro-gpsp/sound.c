/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */


#include "common.h"

direct_sound_struct direct_sound_channel[2];
gbc_sound_struct gbc_sound_channel[4];

const u32 sound_frequency = GBA_SOUND_FREQUENCY;

fixed16_16 sound_tick_delta_to_buffer_ticks(u32 tick_delta)
{
  return (fixed16_16)(((unsigned long long)tick_delta * (unsigned long long)GBA_SOUND_FREQUENCY *
                       65536ULL) /
                      (unsigned long long)GBC_BASE_RATE_INT);
}

u32 sound_on;
#ifdef NSPIRE_RUNTIME_AUDIO
static int sound_render_enabled = 1;
u32 sound_output_generation;
#endif
#ifdef NSPIRE_LIBRETRO
u32 global_enable_audio = 1;
u32 audio_buffer_size_number = 7;
#endif
#ifndef NSPIRE_NO_AUDIO
static s16 sound_buffer[BUFFER_SIZE];
#endif
/* Kept without audio too: it and the channels' buffer indexes are in states. */
static u32 sound_buffer_base;

static fixed16_16 gbc_sound_tick_step;

/* Queue 4 samples to the top of the DS FIFO, wrap around circularly */

NSPIRE_HOT void sound_timer_queue32(u32 channel, u32 value)
{
  direct_sound_struct *ds = &direct_sound_channel[channel];

  ds->fifo[ds->fifo_top++] = value & 0xFF;
  ds->fifo_top &= 31;

  ds->fifo[ds->fifo_top++] = (value >> 8) & 0xFF;
  ds->fifo_top &= 31;

  ds->fifo[ds->fifo_top++] = (value >> 16) & 0xFF;
  ds->fifo_top &= 31;

  ds->fifo[ds->fifo_top++] = (value >> 24);
  ds->fifo_top &= 31;
}


NSPIRE_HOT unsigned sound_timer(fixed8_24 frequency_step, u32 channel)
{
  int ret = 0;
  direct_sound_struct *ds = &direct_sound_channel[channel];

#if defined(NSPIRE_NO_AUDIO) || defined(NSPIRE_RUNTIME_AUDIO)
#ifdef NSPIRE_RUNTIME_AUDIO
  if (!sound_render_enabled)
#endif
  {
  /* No speaker on a calculator (the mixing a speaker would need is left
   * out): the FIFO's sample
   * is popped and its position moves as if mixed (games time off the FIFO,
   * its DMA refills and their interrupts, and the index and fraction are
   * in states), but nothing is mixed: no 128 KB sound_buffer at all.
   * core-04: the position is only counted here (pops at pops_step);
   * sound_flush_pops works it out in closed form when it is read. */
  ds->fifo_base = (ds->fifo_base + 1) & 31;
  if (ds->pops_step != frequency_step)
  {
    sound_flush_pops(ds);
    ds->pops_step = frequency_step;
  }
  ds->pops++;
  }
#endif
#ifndef NSPIRE_NO_AUDIO
#ifdef NSPIRE_RUNTIME_AUDIO
  else
#endif
  {
  fixed8_24 fifo_fractional = ds->fifo_fractional;
  u32 buffer_index = ds->buffer_index;
  u32 sample_status = DIRECT_SOUND_INACTIVE;
  s16 current_sample, next_sample;

  current_sample = ds->fifo[ds->fifo_base] * 16;
  ds->fifo_base = (ds->fifo_base + 1) % 32;
  next_sample = ds->fifo[ds->fifo_base] * 16;

  if(sound_on == 1)
  {
    current_sample >>= ds->volume_halve;
    next_sample >>= ds->volume_halve;

    sample_status = ds->status;
  }

  // Unqueue 1 sample from the base of the DS FIFO and place it on the audio
  // buffer for as many samples as necessary. If the DS FIFO is 16 bytes or
  // smaller and if DMA is enabled for the sound channel initiate a DMA transfer
  // to the DS FIFO.

  /* A step of 1.0 or more emits at most one output frame per FIFO pop.
   * In particular UINT32_MAX + the saved fraction may wrap back below
   * 1.0; do not let that restart the loop. A zero step from an old state
   * still pops/refills the FIFO, but cannot produce samples. */
  if (frequency_step >= 0x1000000 && fifo_fractional <= 0xFFFFFF)
  {
    s16 dest_sample = current_sample +
      fp16_16_to_u32((next_sample - current_sample) * (fifo_fractional >> 8));
    if (sample_status & DIRECT_SOUND_LEFT)
      sound_buffer[buffer_index] += dest_sample;
    if (sample_status & DIRECT_SOUND_RIGHT)
      sound_buffer[buffer_index + 1] += dest_sample;
    buffer_index = (buffer_index + 2) % BUFFER_SIZE;
    fifo_fractional += frequency_step;
  }
  else if (frequency_step)
  {
     /* nspire (2026-11-17, final/speed9a): the same samples, fewer
      * instructions a frame: the sample's interpolation in 32 bits (its low
      * 16 bits are what the buffer adds: the original's s16 dest_sample and
      * logical shift give the same ones), the buffer walked by a byte offset
      * (ldrh/strh take no scaled index), the two halves of a frame by two
      * bases. Bit for bit the original loops (tests/audio/run.sh). */
     u8 *b0 = (u8 *)sound_buffer, *b1 = (u8 *)sound_buffer + 2;
     u32 off = buffer_index << 1;
     s32 diff = next_sample - current_sample;
#define ds_render_frames(store)                                               \
     while(fifo_fractional <= 0xFFFFFF)                                       \
     {                                                                        \
        s32 d = current_sample + ((diff * (s32)(fifo_fractional >> 8)) >> 16); \
        store;                                                                \
        fifo_fractional += frequency_step;                                    \
        off = (off + 4) & (BUFFER_SIZE * 2 - 1);                              \
     }
     switch(sample_status)
     {
        case DIRECT_SOUND_INACTIVE:
           /* render samples NULL */
           ds_render_frames((void)d);
           break;

        case DIRECT_SOUND_RIGHT:
           ds_render_frames(*(s16 *)(b1 + off) += d);
           break;

        case DIRECT_SOUND_LEFT:
           ds_render_frames(*(s16 *)(b0 + off) += d);
           break;

        case DIRECT_SOUND_LEFTRIGHT:
           /* (two sums kept 32-bit: as one d added twice GCC truncated it
            * to 16 bits first, two instructions a frame) */
           ds_render_frames(u32 t0 = *(u16 *)(b0 + off) + (u32)d;
                            u32 t1 = *(u16 *)(b1 + off) + (u32)d;
                            *(u16 *)(b0 + off) = t0; *(u16 *)(b1 + off) = t1);
           break;
     }
#undef ds_render_frames
     buffer_index = off >> 1;
  }

  ds->buffer_index = buffer_index;
  ds->fifo_fractional = fp8_24_fractional_part(fifo_fractional);
  }
#endif

  if(((ds->fifo_top - ds->fifo_base) % 32) <= 16)
  {
    /* core-06: dma_transfer with a short path for 4 words into a FIFO */
    if(dma[1].direct_sound_channel == channel)
      dma_sound_fifo(1, &ret);

    if(dma[2].direct_sound_channel == channel)
      dma_sound_fifo(2, &ret);
  }
  return ret;
}

void sound_reset_fifo(u32 channel)
{
  memset(direct_sound_channel[channel].fifo, 0, 32);
}

#if defined(NSPIRE_NO_AUDIO) || defined(NSPIRE_RUNTIME_AUDIO)
/* k pops of step S, as k runs of the old per-pop loop
 *   while(frac <= 0xFFFFFF) { frac += S; index += 2; }  frac &= 0xFFFFFF;
 * S >= 1.0: one step a pop. S < 1.0: each pop crosses exactly one 1.0, so
 * the k pops take the least n with frac + n*S >= k (in 8.24), and leave
 * frac + n*S - k. S == 0 would hang the old loop; it does nothing here.
 * Out of the hot stretch: it runs at a step change, a state, a timer start
 * and once a frame (tests/host/core_tests.c checks it against the loop). */
void sound_flush_pops(direct_sound_struct *ds)
{
  u32 k = ds->pops;
  u32 step = ds->pops_step;
  u32 frac = ds->fifo_fractional;
  u32 n;

  ds->pops = 0;
  if (!k || !step)
    return;
  if (frac > 0xFFFFFF)
  {
    /* Only from a state: the first pop's loop doesn't run. */
    frac &= 0xFFFFFF;
    k--;
  }
  if (!k)
    n = 0;
  else if (step > 0xFFFFFF)
  {
    frac = (frac + k * step) & 0xFFFFFF;
    n = k;
  }
  else
  {
    unsigned long long whole = (unsigned long long)k << 24;
    unsigned long long n64 = (whole - frac + step - 1) / step;
    frac = (u32)(frac + n64 * step - whole);
    n = (u32)n64;
  }
  ds->fifo_fractional = frac;
  ds->buffer_index = (ds->buffer_index + 2 * n) % BUFFER_SIZE;
}

void sound_flush_all_pops(void)
{
  sound_flush_pops(&direct_sound_channel[0]);
  sound_flush_pops(&direct_sound_channel[1]);
}
#endif

// Initial pattern data = 4bits (signed)
// Channel volume = 12bits
// Envelope volume = 14bits
// Master volume = 2bits

// Recalculate left and right volume as volume changes.
// To calculate the current sample, use (sample * volume) >> 16

// Square waves range from -8 (low) to 7 (high)

const s8 square_pattern_duty[4][8] =
{
  { -8, -8, -8, -8,  7, -8, -8, -8 },
  { -8, -8, -8, -8,  7,  7, -8, -8 },
  { -8, -8,  7,  7,  7,  7, -8, -8 },
  {  7,  7,  7,  7, -8, -8,  7,  7 },
};

s8 wave_samples[64];

#ifndef NSPIRE_NO_AUDIO
u32 noise_table15[1024];
u32 noise_table7[4];
#endif

const u32 gbc_sound_master_volume_table[4] = { 1, 2, 4, 0 };

const u32 gbc_sound_channel_volume_table[8] =
{
  fixed_div(0, 7, 12),
  fixed_div(1, 7, 12),
  fixed_div(2, 7, 12),
  fixed_div(3, 7, 12),
  fixed_div(4, 7, 12),
  fixed_div(5, 7, 12),
  fixed_div(6, 7, 12),
  fixed_div(7, 7, 12)
};

const u32 gbc_sound_envelope_volume_table[16] =
{
  fixed_div(0, 15, 14),
  fixed_div(1, 15, 14),
  fixed_div(2, 15, 14),
  fixed_div(3, 15, 14),
  fixed_div(4, 15, 14),
  fixed_div(5, 15, 14),
  fixed_div(6, 15, 14),
  fixed_div(7, 15, 14),
  fixed_div(8, 15, 14),
  fixed_div(9, 15, 14),
  fixed_div(10, 15, 14),
  fixed_div(11, 15, 14),
  fixed_div(12, 15, 14),
  fixed_div(13, 15, 14),
  fixed_div(14, 15, 14),
  fixed_div(15, 15, 14)
};

u32 gbc_sound_buffer_index = 0;
u32 gbc_sound_last_cpu_ticks = 0;
u32 gbc_sound_partial_ticks = 0;

u32 gbc_sound_master_volume_left;
u32 gbc_sound_master_volume_right;
u32 gbc_sound_master_volume;

/* The emulated channel clock must advance even when the host has no audio
 * sink, a channel is unrouted, or both output volumes are zero. Advance whole
 * runs up to the next 256 Hz counter edge, without visiting each sample. */
static void gbc_sound_advance_silent(gbc_sound_struct *gs, u32 ticks,
                                     unsigned channel)
{
  u32 phase = gs->sample_index;
  u32 counter = gs->tick_counter;
  u32 period = channel == 3 ? (gs->noise_type ? 127u : 32767u) << 16 : 0;
  if (period) phase %= period;
  while (ticks && gs->active_flag)
  {
    u32 run = counter <= 0xFFFF ? ((0xFFFF - counter) >> 8) + 1 : 1;
    if (run > ticks) run = ticks;
    phase += gs->frequency_step * run;
    if (period) phase %= period;
    counter += run * gbc_sound_tick_step;
    ticks -= run;
    if (counter > 0xFFFF)
    {
      if (gs->length_status && --gs->length_ticks == 0)
      {
        gs->active_flag = 0;
        break;
      }
      if (channel != 2 && gs->envelope_status)
      {
        if (--gs->envelope_ticks == 0)
        {
          if (gs->envelope_direction)
          {
            if (gs->envelope_volume < 15) gs->envelope_volume++;
          }
          else if (gs->envelope_volume) gs->envelope_volume--;
          gs->envelope_ticks = gs->envelope_initial_ticks;
        }
      }
      if (channel == 0 && gs->sweep_status)
      {
        u32 sweep_ticks = gs->sweep_ticks - 1;
        if (sweep_ticks == 0)
        {
          u32 rate = gs->rate;
          if (gs->sweep_direction) rate -= rate >> gs->sweep_shift;
          else rate += rate >> gs->sweep_shift;
          if (rate > 2047)
          {
            gs->active_flag = 0;
            break;
          }
          gs->rate = rate;
          gs->frequency_step = nspire_fp16_tone_square_channel(rate);
          sweep_ticks = gs->sweep_initial_ticks;
        }
        gs->sweep_ticks = sweep_ticks;
      }
      counter &= 0xFFFF;
    }
  }
  gs->sample_index = phase;
  gs->tick_counter = counter;
}

static void sound_sync_wave_samples(void)
{
  if (gbc_sound_wave_update)
  {
    u32 i;
    /* GBA exposes the bank opposite the one selected for playback. */
    s8 *bank = wave_samples + (gbc_sound_channel[2].wave_bank ^ 1u) * 32;
    const u8 *ram = (const u8 *)io_registers + 0x90;
    for (i = 0; i < 16; i++)
    {
      bank[i * 2] = (ram[i] >> 4) - 8;
      bank[i * 2 + 1] = (ram[i] & 15) - 8;
    }
    gbc_sound_wave_update = 0;
  }
}

void sound_select_wave_bank(u32 bank)
{
  gbc_sound_struct *gs = &gbc_sound_channel[2];
  sound_sync_wave_samples();
  if (gs->wave_bank != bank)
  {
    const s8 *samples = wave_samples + (bank ^ 1u) * 32;
    u8 *ram = (u8 *)io_registers + 0x90;
    u32 i;
    for (i = 0; i < 16; i++)
      ram[i] = ((samples[i * 2] + 8) << 4) | (samples[i * 2 + 1] + 8);
    gs->wave_bank = bank;
  }
}

static u32 sound_current_status(void)
{
  return sound_on != 1 ? 0 :
    (!!gbc_sound_channel[0].active_flag) |
    (!!gbc_sound_channel[1].active_flag << 1) |
    ((!!gbc_sound_channel[2].active_flag && gbc_sound_channel[2].master_enable) << 2) |
    (!!gbc_sound_channel[3].active_flag << 3);
}

#define update_volume_channel_envelope(channel)                               \
  volume_##channel = gbc_sound_envelope_volume_table[envelope_volume] *       \
   gbc_sound_channel_volume_table[gbc_sound_master_volume_##channel] *        \
   gbc_sound_master_volume_table[gbc_sound_master_volume]                     \

#define update_volume_channel_noenvelope(channel)                             \
  volume_##channel = gs->wave_volume *                                        \
   gbc_sound_channel_volume_table[gbc_sound_master_volume_##channel] *        \
   gbc_sound_master_volume_table[gbc_sound_master_volume]                     \

#define update_volume(type)                                                   \
  update_volume_channel_##type(left);                                         \
  update_volume_channel_##type(right)                                         \

#define update_tone_sweep()                                                   \
  if(gs->sweep_status)                                                        \
  {                                                                           \
    u32 sweep_ticks = gs->sweep_ticks - 1;                                    \
                                                                              \
    if(sweep_ticks == 0)                                                      \
    {                                                                         \
      u32 rate = gs->rate;                                                    \
                                                                              \
      if(gs->sweep_direction)                                                 \
        rate = rate - (rate >> gs->sweep_shift);                              \
      else                                                                    \
        rate = rate + (rate >> gs->sweep_shift);                              \
                                                                              \
      if(rate > 2047) {                                                       \
        rate = 2047;                                                          \
        gs->active_flag = 0;                                                  \
        break;                                                                \
      }                                                                       \
                                                                              \
      frequency_step = nspire_fp16_tone_square_channel(rate);                 \
                                                                              \
      gs->frequency_step = frequency_step;                                    \
      gs->rate = rate;                                                        \
                                                                              \
      sweep_ticks = gs->sweep_initial_ticks;                                  \
    }                                                                         \
    gs->sweep_ticks = sweep_ticks;                                            \
  }                                                                           \

#define update_tone_nosweep()                                                 \

#define update_tone_envelope()                                                \
  if(gs->envelope_status)                                                     \
  {                                                                           \
    u32 envelope_ticks = gs->envelope_ticks - 1;                              \
    envelope_volume = gs->envelope_volume;                                    \
                                                                              \
    if(envelope_ticks == 0)                                                   \
    {                                                                         \
      if(gs->envelope_direction)                                              \
      {                                                                       \
        if(envelope_volume != 15)                                             \
          envelope_volume = gs->envelope_volume + 1;                          \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        if(envelope_volume != 0)                                              \
          envelope_volume = gs->envelope_volume - 1;                          \
      }                                                                       \
                                                                              \
      update_volume(envelope);                                                \
                                                                              \
      gs->envelope_volume = envelope_volume;                                  \
      gs->envelope_ticks = gs->envelope_initial_ticks;                        \
    }                                                                         \
    else                                                                      \
      gs->envelope_ticks = envelope_ticks;                                    \
  }                                                                           \

#define update_tone_noenvelope()                                              \

#define update_tone_counters(envelope_op, sweep_op)                           \
  tick_counter += gbc_sound_tick_step;                                        \
  if(tick_counter > 0xFFFF)                                                   \
  {                                                                           \
    if(gs->length_status)                                                     \
    {                                                                         \
      u32 length_ticks = gs->length_ticks - 1;                                \
      gs->length_ticks = length_ticks;                                        \
                                                                              \
      if(length_ticks == 0)                                                   \
      {                                                                       \
        gs->active_flag = 0;                                                  \
        break;                                                                \
      }                                                                       \
    }                                                                         \
                                                                              \
    update_tone_##envelope_op();                                              \
    update_tone_##sweep_op();                                                 \
                                                                              \
    tick_counter &= 0xFFFF;                                                   \
  }                                                                           \

#define gbc_sound_render_sample_right()                                       \
  sound_buffer[buffer_index + 1] += (current_sample * volume_right) >> 22     \

#define gbc_sound_render_sample_left()                                        \
  sound_buffer[buffer_index] += (current_sample * volume_left) >> 22          \

#define gbc_sound_render_sample_both()                                        \
  gbc_sound_render_sample_right();                                            \
  gbc_sound_render_sample_left()                                              \

/* nspire (2026-11-17, final/speed9a): the same samples, fewer instructions
 * a sample (bit for bit: tests/audio/run.sh, the tone and state tests):
 *  - in runs up to the 256 Hz counter's next edge (the samples between need
 *    no counter test; the edge's sample ends its run, then the counters as
 *    before: gbc_sound_tick_step is 256, as gbc_sound_advance_silent has it);
 *  - the buffer walked by a byte offset, the frame's halves by two bases;
 *  - a square or noise channel's samples are -8 or 7 (the duty patterns;
 *    the noise bit), so (sample * volume) >> 22 for both halves is looked up
 *    packed (L in the low 16 bits: the buffer adds those; R in the high,
 *    arithmetic shift), made again when the envelope moves the volume. */
#define gbc_pack_sample(v)                                                    \
  ((((s32)(v) * volume_left) >> 22 & 0xFFFF) |                                \
   ((u32)(((s32)(v) * volume_right) >> 22) << 16))                           \

#define gbc_sound_add_left(w)                                                 \
  *(s16 *)(b0 + off) += (s32)(w)                                              \

#define gbc_sound_add_right(w)                                                \
  *(s16 *)(b1 + off) += (s32)(w) >> 16                                        \

#define gbc_sound_add_both(w)                                                 \
  gbc_sound_add_right(w);                                                     \
  gbc_sound_add_left(w)                                                       \

#define gbc_sound_mul_left()                                                  \
  *(s16 *)(b0 + off) += (current_sample * volume_left) >> 22                  \

#define gbc_sound_mul_right()                                                 \
  *(s16 *)(b1 + off) += (current_sample * volume_right) >> 22                 \

#define gbc_sound_mul_both()                                                  \
  gbc_sound_mul_right();                                                      \
  gbc_sound_mul_left()                                                        \

#define gbc_sound_runs(setup, per_sample, envelope_op, sweep_op)              \
  for(i = 0; i < buffer_ticks; )                                              \
  {                                                                           \
    u32 n = tick_counter <= 0xFFFF ? ((0xFFFF - tick_counter) >> 8) + 1 : 1;  \
    u32 off = buffer_index << 1;                                              \
    if(n > buffer_ticks - i)                                                  \
      n = buffer_ticks - i;                                                   \
    i += n;                                                                   \
    tick_counter += (n - 1) * gbc_sound_tick_step;                             \
    setup;                                                                    \
    do                                                                        \
    {                                                                         \
      per_sample;                                                             \
      off = (off + 4) & (BUFFER_SIZE * 2 - 1);                                \
    } while(--n);                                                             \
    buffer_index = off >> 1;                                                  \
    update_tone_counters(envelope_op, sweep_op);                              \
  }                                                                           \

#define gbc_sound_lut_square(sample_length)                                   \
  if(volume_left != lut_left || volume_right != lut_right)                    \
  {                                                                           \
    u32 k;                                                                    \
    for(k = 0; k < 8; k++)                                                    \
      lut[k] = gbc_pack_sample(sample_data[k]);                               \
    lut_left = volume_left;                                                   \
    lut_right = volume_right;                                                 \
  }                                                                           \

#define gbc_sound_render_samples(type, sample_length, envelope_op, sweep_op)  \
  if(sample_length == 8)                                                      \
  {                                                                           \
    gbc_sound_runs(gbc_sound_lut_square(sample_length),                       \
      u32 w = lut[fp16_16_to_u32(sample_index) % 8];                          \
      gbc_sound_add_##type(w);                                                \
      sample_index += frequency_step,                                         \
      envelope_op, sweep_op);                                                 \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    gbc_sound_runs((void)0,                                                   \
      current_sample =                                                        \
       sample_data[fp16_16_to_u32(sample_index) % sample_length];             \
      gbc_sound_mul_##type();                                                 \
      sample_index += frequency_step,                                         \
      envelope_op, sweep_op);                                                 \
  }                                                                           \

#define gbc_noise_wrap_full 32767

#define gbc_noise_wrap_half 127

#define gbc_sound_normalize_samples(sample_length)
#define gbc_sound_normalize_noise(noise_type) \
  sample_index %= u32_to_fp16_16(gbc_noise_wrap_##noise_type)

#define get_noise_sample_full()                                               \
  current_sample =                                                            \
   ((s32)(noise_table15[fp16_16_to_u32(sample_index) >> 5] <<                 \
   (fp16_16_to_u32(sample_index) & 0x1F)) >> 31) ^ 0x07                       \

#define get_noise_sample_half()                                               \
  current_sample =                                                            \
   ((s32)(noise_table7[fp16_16_to_u32(sample_index) >> 5] <<                  \
   (fp16_16_to_u32(sample_index) & 0x1F)) >> 31) ^ 0x07                       \

#define noise_bit_full()                                                      \
   ((s32)(noise_table15[fp16_16_to_u32(sample_index) >> 5] <<                 \
   (fp16_16_to_u32(sample_index) & 0x1F)) < 0)                                \

#define noise_bit_half()                                                      \
   ((s32)(noise_table7[fp16_16_to_u32(sample_index) >> 5] <<                  \
   (fp16_16_to_u32(sample_index) & 0x1F)) < 0)                                \

/* (the noise's sample: -8 where its bit is set, else 7: get_noise_sample_*) */
#define gbc_sound_lut_noise()                                                 \
  if(volume_left != lut_left || volume_right != lut_right)                    \
  {                                                                           \
    lut[0] = gbc_pack_sample(7);                                              \
    lut[1] = gbc_pack_sample(-8);                                             \
    lut_left = volume_left;                                                   \
    lut_right = volume_right;                                                 \
  }                                                                           \

#define gbc_sound_render_noise(type, noise_type, envelope_op, sweep_op)       \
  gbc_sound_runs(gbc_sound_lut_noise(),                                       \
    u32 w = lut[noise_bit_##noise_type()];                                    \
    gbc_sound_add_##type(w);                                                  \
    sample_index += frequency_step;                                           \
    if(sample_index >= u32_to_fp16_16(gbc_noise_wrap_##noise_type))           \
      sample_index -= u32_to_fp16_16(gbc_noise_wrap_##noise_type),            \
    envelope_op, sweep_op);                                                   \

#define gbc_sound_render_channel(type, sample_length, envelope_op, sweep_op)  \
  buffer_index = gbc_sound_buffer_index;                                      \
  sample_index = gs->sample_index;                                            \
  gbc_sound_normalize_##type(sample_length);                                 \
  frequency_step = gs->frequency_step;                                        \
  tick_counter = gs->tick_counter;                                            \
                                                                              \
  update_volume(envelope_op);                                                 \
  lut_left = lut_right = -1;                 /* (made at the first run) */     \
                                                                              \
  if (gs->status == GBC_SOUND_INACTIVE || (!volume_left && !volume_right))    \
  {                                                                          \
    gbc_sound_advance_silent(gs, buffer_ticks, gs - gbc_sound_channel);        \
    sample_index = gs->sample_index;                                          \
    tick_counter = gs->tick_counter;                                          \
  }                                                                          \
  else                                                                       \
  switch(gs->status)                                                          \
  {                                                                           \
    case GBC_SOUND_INACTIVE:                                                  \
      break;                                                                  \
                                                                              \
    case GBC_SOUND_LEFT:                                                      \
      gbc_sound_render_##type(left, sample_length, envelope_op, sweep_op);    \
      break;                                                                  \
                                                                              \
    case GBC_SOUND_RIGHT:                                                     \
      gbc_sound_render_##type(right, sample_length, envelope_op, sweep_op);   \
      break;                                                                  \
                                                                              \
    case GBC_SOUND_LEFTRIGHT:                                                 \
      gbc_sound_render_##type(both, sample_length, envelope_op, sweep_op);    \
      break;                                                                  \
  }                                                                           \
                                                                              \
  gs->sample_index = sample_index;                                            \
  gs->tick_counter = tick_counter;                                            \

void render_gbc_sound()
{
#if defined(NSPIRE_NO_AUDIO) || defined(NSPIRE_RUNTIME_AUDIO)
#ifdef NSPIRE_RUNTIME_AUDIO
  if (!sound_render_enabled)
#endif
  {
  u16 sound_status = read_ioreg(REG_SOUNDCNT_X) & 0xFFF0;
  u32 tick_delta = cpu_ticks - gbc_sound_last_cpu_ticks;
  fixed16_16 buffer_ticks = sound_tick_delta_to_buffer_ticks(tick_delta);
  gbc_sound_struct *gs;

  sound_sync_wave_samples();
  if (!tick_delta)
    return;

  gbc_update_count++;
  gbc_sound_partial_ticks += fp16_16_fractional_part(buffer_ticks);
  buffer_ticks = fp16_16_to_u32(buffer_ticks);

  if (gbc_sound_partial_ticks > 0xFFFF)
  {
    buffer_ticks += 1;
    gbc_sound_partial_ticks &= 0xFFFF;
  }

  if (sound_on == 1)
  {
    gs = gbc_sound_channel + 0;
    if (gs->active_flag)
    {
      sound_status |= 0x01;
      gbc_sound_advance_silent(gs, buffer_ticks, 0);
    }
    gs = gbc_sound_channel + 1;
    if (gs->active_flag)
    {
      sound_status |= 0x02;
      gbc_sound_advance_silent(gs, buffer_ticks, 1);
    }
    gs = gbc_sound_channel + 2;
    if (gs->active_flag && gs->master_enable)
    {
      sound_status |= 0x04;
      gbc_sound_advance_silent(gs, buffer_ticks, 2);
    }
    gs = gbc_sound_channel + 3;
    if (gs->active_flag)
    {
      sound_status |= 0x08;
      gbc_sound_advance_silent(gs, buffer_ticks, 3);
    }
  }

  write_ioreg(REG_SOUNDCNT_X, (sound_status & 0xFFF0) | sound_current_status());
  gbc_sound_last_cpu_ticks = cpu_ticks;
  gbc_sound_buffer_index =
   (gbc_sound_buffer_index + (buffer_ticks * 2)) % BUFFER_SIZE;
  return;
  }
#endif
#ifndef NSPIRE_NO_AUDIO
  {
  u32 i;
  gbc_sound_struct *gs = gbc_sound_channel;
  fixed16_16 sample_index, frequency_step;
  fixed16_16 tick_counter;
  u32 buffer_index;
  s32 volume_left, volume_right;
  u32 envelope_volume;
  s32 current_sample;
  u16 sound_status = read_ioreg(REG_SOUNDCNT_X) & 0xFFF0;
  const s8 *sample_data;
  u8 *b0 = (u8 *)sound_buffer, *b1 = (u8 *)sound_buffer + 2;
  u32 lut[8];
  s32 lut_left, lut_right;
  u32 tick_delta = cpu_ticks - gbc_sound_last_cpu_ticks;
  fixed16_16 buffer_ticks = sound_tick_delta_to_buffer_ticks(tick_delta);
  sound_sync_wave_samples();
  if (!tick_delta)
    return;

  gbc_update_count++;
  gbc_sound_partial_ticks += fp16_16_fractional_part(buffer_ticks);
  buffer_ticks = fp16_16_to_u32(buffer_ticks);

  if(gbc_sound_partial_ticks > 0xFFFF)
  {
    buffer_ticks += 1;
    gbc_sound_partial_ticks &= 0xFFFF;
  }

  if(sound_on == 1)
  {
    gs = gbc_sound_channel + 0;
    if(gs->active_flag)
    {
      sound_status |= 0x01;
      sample_data = &square_pattern_duty[gs->sample_table_idx][0];
      envelope_volume = gs->envelope_volume;
      gbc_sound_render_channel(samples, 8, envelope, sweep);
    }

    gs = gbc_sound_channel + 1;
    if(gs->active_flag)
    {
      sound_status |= 0x02;
      sample_data = &square_pattern_duty[gs->sample_table_idx][0];
      envelope_volume = gs->envelope_volume;
      gbc_sound_render_channel(samples, 8, envelope, nosweep);
    }

    gs = gbc_sound_channel + 2;

    if((gs->active_flag) && (gs->master_enable))
    {
      sound_status |= 0x04;
      sample_data = wave_samples;
      if(gs->wave_type == 0)
      {
        if(gs->wave_bank == 1)
          sample_data += 32;

        gbc_sound_render_channel(samples, 32, noenvelope, nosweep);
      }
      else
      {
        gbc_sound_render_channel(samples, 64, noenvelope, nosweep);
      }
    }

    gs = gbc_sound_channel + 3;
    if(gs->active_flag)
    {
      sound_status |= 0x08;
      envelope_volume = gs->envelope_volume;

      if(gs->noise_type == 1)
      {
        gbc_sound_render_channel(noise, half, envelope, nosweep);
      }
      else
      {
        gbc_sound_render_channel(noise, full, envelope, nosweep);
      }
    }
  }

  write_ioreg(REG_SOUNDCNT_X, (sound_status & 0xFFF0) | sound_current_status());

  gbc_sound_last_cpu_ticks = cpu_ticks;
  gbc_sound_buffer_index =
   (gbc_sound_buffer_index + (buffer_ticks * 2)) % BUFFER_SIZE;
  }
#endif /* !NSPIRE_NO_AUDIO */
}

#ifndef NSPIRE_NO_AUDIO
// Special thanks to blarrg for the LSFR frequency used in Meridian, as posted
// on the forum at http://meridian.overclocked.org:
// http://meridian.overclocked.org/cgi-bin/wwwthreads/showpost.pl?Board=merid
// angeneraldiscussion&Number=2069&page=0&view=expanded&mode=threaded&sb=4
// Hope you don't mind me borrowing it ^_-

static void init_noise_table(u32 *table, u32 period, u32 bit_length)
{
  u32 shift_register = 0xFF;
  u32 mask = ~(1 << bit_length);
  s32 table_pos, bit_pos;
  u32 current_entry;
  u32 table_period = (period + 31) / 32;

  // Bits are stored in reverse order so they can be more easily moved to
  // bit 31, for sign extended shift down.

  for(table_pos = 0; table_pos < table_period; table_pos++)
  {
    current_entry = 0;
    for(bit_pos = 31; bit_pos >= 0; bit_pos--)
    {
      current_entry |= (shift_register & 0x01) << bit_pos;

      shift_register =
       ((1 & (shift_register ^ (shift_register >> 1))) << bit_length) |
       ((shift_register >> 1) & mask);
    }

    table[table_pos] = current_entry;
  }
}

#endif

void reset_sound(void)
{
#ifdef NSPIRE_RUNTIME_AUDIO
  sound_output_generation++;
#endif
  direct_sound_struct *ds = direct_sound_channel;
  gbc_sound_struct *gs = gbc_sound_channel;
  u32 i;

  sound_on = 0;
  sound_buffer_base = 0;
#ifndef NSPIRE_NO_AUDIO
  memset(sound_buffer, 0, sizeof(sound_buffer));
#endif

  for(i = 0; i < 2; i++, ds++)
  {
    ds->buffer_index = 0;
    ds->status = DIRECT_SOUND_INACTIVE;
    ds->fifo_top = 0;
    ds->fifo_base = 0;
    ds->fifo_fractional = 0;
#if defined(NSPIRE_NO_AUDIO) || defined(NSPIRE_RUNTIME_AUDIO)
    ds->pops = 0;
    ds->pops_step = 0;
#endif
    ds->volume_halve = 1;
    memset(ds->fifo, 0, 32);
  }

  gbc_sound_buffer_index = 0;
  gbc_sound_last_cpu_ticks = 0;
  gbc_sound_partial_ticks = 0;

  gbc_sound_master_volume_left = 0;
  gbc_sound_master_volume_right = 0;
  gbc_sound_master_volume = 0;
  memset(wave_samples, -8, sizeof(wave_samples));
  gbc_sound_wave_update = 0;

  memset(&gbc_sound_channel[0], 0, sizeof(gbc_sound_channel));
  for(i = 0; i < 4; i++, gs++)
  {
    gs->status = GBC_SOUND_INACTIVE;
    gs->sample_table_idx = 2;
    gs->active_flag = 0;
  }
}

void init_sound()
{
  /* 256 Hz / 65536 Hz in 16.16 (= 256); integer, as the other steps. */
  gbc_sound_tick_step =
   (fixed16_16)((256ull << 16) / (unsigned long long)GBA_SOUND_FREQUENCY);

#ifndef NSPIRE_NO_AUDIO
  init_noise_table(noise_table15, 32767, 14);
  init_noise_table(noise_table7, 127, 6);
#endif

  reset_sound();
}


/* Values that index tables or control shifts must be checked before any
 * subsystem applies a state. The BSON layer has already bounded the tree. */
static bool sound_state_value(const u8 *doc, const char *key, u32 max,
                              u32 alignment)
{
  u32 value;
  return bson_read_int32(doc, key, &value) && value <= max &&
         !(value & (alignment - 1));
}

bool sound_check_savestate(const u8 *src)
{
  static const struct { const char *name; u32 max; } gsvars[] = {
    {"status", 3}, {"rate", 2047}, {"freq-step", 2097152},
    {"sample-idx", 0xFFFFFFFFu}, {"tick-cnt", 0xFFFFFFFFu},
    {"volume", 0xFFFFFFFFu}, {"active", 1}, {"enable", 1},
    {"env-vol0", 15}, {"env-vol", 15}, {"env-dir", 1}, {"env-status", 1},
    {"env-ticks0", 28}, {"env-ticks", 0xFFFFFFFFu},
    {"sweep-status", 1}, {"sweep-dir", 1}, {"sweep-ticks0", 14},
    {"sweep-ticks", 0xFFFFFFFFu}, {"sweep-shift", 7},
    {"wav-type", 1}, {"wav-bank", 1}, {"wav-vol", 16384},
    {"len-status", 1}, {"len-ticks", 0xFFFFFFFFu},
    {"noise-type", 1}, {"sample-tbl", 3}
  };
  const u8 *doc = bson_find_document(src, "sound");
  const u8 *wave;
  unsigned i, j;
  u32 value;

  if (!doc ||
      !sound_state_value(doc, "on", 1, 1) ||
      !sound_state_value(doc, "buf-base", BUFFER_SIZE_MASK, 2) ||
      !sound_state_value(doc, "gbc-buf-idx", BUFFER_SIZE_MASK, 2) ||
      !bson_read_int32(doc, "gbc-last-cpu-ticks", &value) ||
      !sound_state_value(doc, "gbc-partial-ticks", 0xFFFF, 1) ||
      !sound_state_value(doc, "gbc-ms-vol-left", 7, 1) ||
      !sound_state_value(doc, "gbc-ms-vol-right", 7, 1) ||
      !sound_state_value(doc, "gbc-ms-vol", 3, 1) ||
      !bson_check_bytes(doc, "wav-samples", sizeof(wave_samples)))
    return false;
  wave = bson_find_key(doc, "wav-samples") + 5;
  for (i = 0; i < sizeof(wave_samples); i++)
    if ((s8)wave[i] < -8 || (s8)wave[i] > 7)
      return false;

  for (i = 0; i < 2; i++)
  {
    char name[4] = {'d', 's', '0' + i, 0};
    const u8 *channel = bson_find_document(doc, name);
    if (!channel ||
        !sound_state_value(channel, "status", 3, 1) ||
        !sound_state_value(channel, "volume", 1, 1) ||
        !sound_state_value(channel, "fifo-base", 31, 1) ||
        !sound_state_value(channel, "fifo-top", 31, 1) ||
        !sound_state_value(channel, "fifo-frac", 0xFFFFFF, 1) ||
        !sound_state_value(channel, "buf-idx", BUFFER_SIZE_MASK, 2) ||
        !bson_check_bytes(channel, "fifo-bytes", sizeof(direct_sound_channel[i].fifo)))
      return false;
  }

  for (i = 0; i < 4; i++)
  {
    char name[4] = {'g', 's', '0' + i, 0};
    const u8 *channel = bson_find_document(doc, name);
    if (!channel)
      return false;
    for (j = 0; j < sizeof(gsvars) / sizeof(gsvars[0]); j++)
      if (!sound_state_value(channel, gsvars[j].name, gsvars[j].max, 1))
        return false;
  }
  return true;
}

bool sound_read_savestate(const u8 *src)
{
  int i;
  const u8 *snddoc = bson_find_document(src, "sound");

  if (!sound_check_savestate(src))
    return false;

  if (!(
    bson_read_int32(snddoc, "on", &sound_on) &&
    bson_read_int32(snddoc, "buf-base", &sound_buffer_base) &&
    bson_read_int32(snddoc, "gbc-buf-idx", &gbc_sound_buffer_index) &&
    bson_read_int32(snddoc, "gbc-last-cpu-ticks", &gbc_sound_last_cpu_ticks) &&
    bson_read_int32(snddoc, "gbc-partial-ticks", &gbc_sound_partial_ticks) &&
    bson_read_int32(snddoc, "gbc-ms-vol-left", &gbc_sound_master_volume_left) &&
    bson_read_int32(snddoc, "gbc-ms-vol-right", &gbc_sound_master_volume_right) &&
    bson_read_int32(snddoc, "gbc-ms-vol", &gbc_sound_master_volume) &&
    bson_read_bytes(snddoc, "wav-samples", wave_samples, sizeof(wave_samples))))
    return false;

  for (i = 0; i < 2; i++)
  {
    direct_sound_struct *ds = &direct_sound_channel[i];
    char tn[4] = {'d', 's', '0' + i, 0};
    const u8 *sndchan = bson_find_document(snddoc, tn);
    if (!(
      bson_read_int32(sndchan, "status", &ds->status) &&
      bson_read_int32(sndchan, "volume", &ds->volume_halve) &&
      bson_read_int32(sndchan, "fifo-base", &ds->fifo_base) &&
      bson_read_int32(sndchan, "fifo-top", &ds->fifo_top) &&
      bson_read_int32(sndchan, "fifo-frac", &ds->fifo_fractional) &&
      bson_read_bytes(sndchan, "fifo-bytes", ds->fifo, sizeof(ds->fifo)) &&
      bson_read_int32(sndchan, "buf-idx", &ds->buffer_index)))
      return false;
#if defined(NSPIRE_NO_AUDIO) || defined(NSPIRE_RUNTIME_AUDIO)
    ds->pops = 0;   /* the state's index and fraction are whole */
#endif
  }

  for (i = 0; i < 4; i++)
  {
    gbc_sound_struct *gs = &gbc_sound_channel[i];
    char tn[4] = {'g', 's', '0' + i, 0};
    const u8 *sndchan = bson_find_document(snddoc, tn);
    if (!(
      bson_read_int32(sndchan, "status", &gs->status) &&
      bson_read_int32(sndchan, "rate", &gs->rate) &&
      bson_read_int32(sndchan, "freq-step", &gs->frequency_step) &&
      bson_read_int32(sndchan, "sample-idx", &gs->sample_index) &&
      bson_read_int32(sndchan, "tick-cnt", &gs->tick_counter) &&
      bson_read_int32(sndchan, "volume", &gs->total_volume) &&
      bson_read_int32(sndchan, "active", &gs->active_flag) &&
      bson_read_int32(sndchan, "enable", &gs->master_enable) &&

      bson_read_int32(sndchan, "env-vol0", &gs->envelope_initial_volume) &&
      bson_read_int32(sndchan, "env-vol", &gs->envelope_volume) &&
      bson_read_int32(sndchan, "env-dir", &gs->envelope_direction) &&
      bson_read_int32(sndchan, "env-status", &gs->envelope_status) &&
      bson_read_int32(sndchan, "env-ticks0", &gs->envelope_initial_ticks) &&
      bson_read_int32(sndchan, "env-ticks", &gs->envelope_ticks) &&
      bson_read_int32(sndchan, "sweep-status", &gs->sweep_status) &&
      bson_read_int32(sndchan, "sweep-dir", &gs->sweep_direction) &&
      bson_read_int32(sndchan, "sweep-ticks0", &gs->sweep_initial_ticks) &&
      bson_read_int32(sndchan, "sweep-ticks", &gs->sweep_ticks) &&
      bson_read_int32(sndchan, "sweep-shift", &gs->sweep_shift) &&
      bson_read_int32(sndchan, "wav-type", &gs->wave_type) &&
      bson_read_int32(sndchan, "wav-bank", &gs->wave_bank) &&
      bson_read_int32(sndchan, "wav-vol", &gs->wave_volume) &&

      bson_read_int32(sndchan, "len-status", &gs->length_status) &&
      bson_read_int32(sndchan, "len-ticks", &gs->length_ticks) &&
      bson_read_int32(sndchan, "noise-type", &gs->noise_type) &&
      bson_read_int32(sndchan, "sample-tbl", &gs->sample_table_idx)))
      return false;
  }

  gbc_sound_wave_update = 0;
#ifndef NSPIRE_NO_AUDIO
  /* Queued PCM belongs to the previous timeline and is not in a state. */
  memset(sound_buffer, 0, sizeof(sound_buffer));
  sound_buffer_base = gbc_sound_buffer_index;
#ifdef NSPIRE_RUNTIME_AUDIO
  sound_output_generation++;
#endif
#endif
  return true;
}

unsigned sound_write_savestate(u8 *dst)
{
  int i;
  u8 *wbptr, *startp = dst;
#if defined(NSPIRE_NO_AUDIO) || defined(NSPIRE_RUNTIME_AUDIO)
  sound_flush_all_pops();   /* the bytes as if every pop had been applied */
#endif
  sound_sync_wave_samples();
  bson_start_document(dst, "sound", wbptr);
  bson_write_int32(dst, "on", sound_on);
  bson_write_int32(dst, "buf-base", sound_buffer_base);
  bson_write_int32(dst, "gbc-buf-idx", gbc_sound_buffer_index);
  bson_write_int32(dst, "gbc-last-cpu-ticks", gbc_sound_last_cpu_ticks);
  bson_write_int32(dst, "gbc-partial-ticks", gbc_sound_partial_ticks);
  bson_write_int32(dst, "gbc-ms-vol-left", gbc_sound_master_volume_left);
  bson_write_int32(dst, "gbc-ms-vol-right", gbc_sound_master_volume_right);
  bson_write_int32(dst, "gbc-ms-vol", gbc_sound_master_volume);
  bson_write_bytes(dst, "wav-samples", wave_samples, sizeof(wave_samples));

  for (i = 0; i < 2; i++)
  {
    u8 *wbptr2;
    char tn[4] = {'d', 's', '0' + i, 0};
    bson_start_document(dst, tn, wbptr2);
    bson_write_int32(dst, "status", direct_sound_channel[i].status);
    bson_write_int32(dst, "volume", direct_sound_channel[i].volume_halve);
    bson_write_int32(dst, "fifo-base", direct_sound_channel[i].fifo_base);
    bson_write_int32(dst, "fifo-top", direct_sound_channel[i].fifo_top);
    bson_write_int32(dst, "fifo-frac", direct_sound_channel[i].fifo_fractional);
    bson_write_bytes(dst, "fifo-bytes", direct_sound_channel[i].fifo,
                          sizeof(direct_sound_channel[i].fifo));
    bson_write_int32(dst, "buf-idx", direct_sound_channel[i].buffer_index);
    bson_finish_document(dst, wbptr2);
  }

  for (i = 0; i < 4; i++)
  {
    gbc_sound_struct *gs = &gbc_sound_channel[i];
    u8 *wbptr2;
    char tn[4] = {'g', 's', '0' + i, 0};
    bson_start_document(dst, tn, wbptr2);
    bson_write_int32(dst, "status", gs->status);
    bson_write_int32(dst, "rate", gs->rate);
    bson_write_int32(dst, "freq-step", gs->frequency_step);
    bson_write_int32(dst, "sample-idx", gs->sample_index);
    bson_write_int32(dst, "tick-cnt", gs->tick_counter);
    bson_write_int32(dst, "volume", gs->total_volume);
    bson_write_int32(dst, "active", gs->active_flag);
    bson_write_int32(dst, "enable", gs->master_enable);

    bson_write_int32(dst, "env-vol0", gs->envelope_initial_volume);
    bson_write_int32(dst, "env-vol", gs->envelope_volume);
    bson_write_int32(dst, "env-dir", gs->envelope_direction);
    bson_write_int32(dst, "env-status", gs->envelope_status);
    bson_write_int32(dst, "env-ticks0", gs->envelope_initial_ticks);
    bson_write_int32(dst, "env-ticks", gs->envelope_ticks);
    bson_write_int32(dst, "sweep-status", gs->sweep_status);
    bson_write_int32(dst, "sweep-dir", gs->sweep_direction);
    bson_write_int32(dst, "sweep-ticks0", gs->sweep_initial_ticks);
    bson_write_int32(dst, "sweep-ticks", gs->sweep_ticks);
    bson_write_int32(dst, "sweep-shift", gs->sweep_shift);
    bson_write_int32(dst, "wav-type", gs->wave_type);
    bson_write_int32(dst, "wav-bank", gs->wave_bank);
    bson_write_int32(dst, "wav-vol", gs->wave_volume);

    bson_write_int32(dst, "len-status", gs->length_status);
    bson_write_int32(dst, "len-ticks", gs->length_ticks);
    bson_write_int32(dst, "noise-type", gs->noise_type);
    bson_write_int32(dst, "sample-tbl", gs->sample_table_idx);

    // No longer used fields, keep for backwards compatibility.
    bson_write_int32(dst, "env-step", 0);
    bson_finish_document(dst, wbptr2);
  }

  bson_finish_document(dst, wbptr);
  return (unsigned int)(dst - startp);
}

#ifdef NSPIRE_RUNTIME_AUDIO
void sound_discard_samples(void)
{
  memset(sound_buffer, 0, sizeof(sound_buffer));
  sound_buffer_base = gbc_sound_buffer_index;
}

void sound_set_output_enabled(int enabled)
{
  enabled = !!enabled;
  if (sound_render_enabled == enabled)
    return;
  render_gbc_sound();
  sound_flush_all_pops();
  sound_render_enabled = enabled;
  sound_discard_samples();
}
#endif

#ifndef NSPIRE_NO_AUDIO
u32 sound_read_samples(s16 *out, u32 frames)
{
   u32 i;
   u32 samples_to_read;
   /* Get total number of samples in the buffer */
   u32 samples_available = (gbc_sound_buffer_index - sound_buffer_base) & BUFFER_SIZE_MASK;
   if (!out || !frames)
      return 0;
   /* The last 512 samples are 'in use', and cannot
    * be read out yet */
   samples_available     = (samples_available > 512) ? (samples_available - 512) : 0;
   /* Available sample count must be an even number */
   samples_available     = (samples_available >> 1) << 1;

   /* Bound frames before doubling: a large request must not wrap to zero. */
   samples_to_read = frames > samples_available / 2 ? samples_available : frames * 2;

   for(i = 0; i < samples_to_read; i++)
   {
      u32 source_index   = (sound_buffer_base + i) & BUFFER_SIZE_MASK;
      s32 current_sample = sound_buffer[source_index];

      sound_buffer[source_index] = 0;

      if(current_sample > 2047)
         current_sample = 2047;
      if(current_sample < -2048)
         current_sample = -2048;

      out[i] = current_sample * 16;
   }

   sound_buffer_base += samples_to_read;
   sound_buffer_base &= BUFFER_SIZE_MASK;

   /* Function returns number of frames read */
   return (samples_to_read >> 1);
}

/* nspire (speed9a): the frontend's resampler reads the samples where they
 * are (nspire_audio_mix.h: read, cleared, clamped and resampled in one pass)
 * instead of through sound_read_samples's copy. The frames it would read
 * next, in the same order: from *base (a sample index, even) on, wrapping at
 * BUFFER_SIZE; sound_samples_taken moves the base past those read. */
u32 sound_samples_ready(s16 **buffer, u32 *base)
{
   u32 samples_available = (gbc_sound_buffer_index - sound_buffer_base) & BUFFER_SIZE_MASK;
   *buffer = sound_buffer;
   *base = sound_buffer_base;
   return samples_available > 512 ? (samples_available - 512) >> 1 : 0;
}

void sound_samples_taken(u32 frames)
{
   sound_buffer_base = (sound_buffer_base + frames * 2) & BUFFER_SIZE_MASK;
}
#endif
