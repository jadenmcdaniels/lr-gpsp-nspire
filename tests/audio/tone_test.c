/*
 * gpSP's mixer, driven by hand: the tone channels and the FIFOs get their
 * sound registers written through the core's own register write functions
 * (write_io_register16/32, as the game's stores do), time is advanced by
 * cpu_ticks, and the mixer is drained with sound_read_samples as the
 * frontend does. Built for the calculator's CPU with the mixer on
 * (tests/audio/tone.mk) and run under qemu-arm (tests/audio/run.sh).
 *
 *   tone_test
 *
 * It prints one line per case: its name, a hash of everything the mixer
 * produced (FNV-1a over the s16 values), the number of s16 values, the peak
 * and RMS of the left and right side. run.sh compares the lines with
 * tests/audio/ref/tone.txt (the reference output of the mixer), and the exit
 * status says whether the checks of what each case must do hold (a channel
 * that expired is off in SOUNDCNT_X, an unrouted channel is silent, ...).
 *
 * Covers: both square channels with every duty, envelope up and down, length
 * expiry, sweep up and down (and the overflow that silences the channel),
 * the wave channel (32 and 64 sample banks, both banks, each volume), noise
 * (7 and 15 bit, several ratios), routing left/right/both/neither of each
 * channel, every master volume, sound master on/off, and both FIFO channels
 * on both timers (two rates) with each routing and volume.
 */

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

cpu_alert_type function_cc write_io_register16(u32 address, u32 value);
cpu_alert_type function_cc write_io_register32(u32 address, u32 value);

/* The registers' byte addresses. */
#define IO 0x04000000u
#define A_SOUND1CNT_L 0x60
#define A_SOUND1CNT_H 0x62
#define A_SOUND1CNT_X 0x64
#define A_SOUND2CNT_L 0x68
#define A_SOUND2CNT_H 0x6C
#define A_SOUND3CNT_L 0x70
#define A_SOUND3CNT_H 0x72
#define A_SOUND3CNT_X 0x74
#define A_SOUND4CNT_L 0x78
#define A_SOUND4CNT_H 0x7C
#define A_SOUNDCNT_L 0x80
#define A_SOUNDCNT_H 0x82
#define A_SOUNDCNT_X 0x84
#define A_WAVERAM 0x90
#define A_FIFO_A 0xA0
#define A_FIFO_B 0xA4
#define A_TM0D 0x100
#define A_TM0CNT 0x102

#define FRAME 280896u   /* CPU cycles in a GBA frame */

extern timer_type timer[4];
static int failures, checks;
static u32 total_hash = 2166136261u, last_hash;

static void w16(u32 addr, u32 value) { write_io_register16(IO + addr, value); }
static void w32(u32 addr, u32 value) { write_io_register32(IO + addr, value); }
static u32 r16(u32 addr) { return read_ioreg(addr >> 1); }

/* What the mixer produced in a case. */
typedef struct
{
  u32 hash, n;
  u32 count[2];
  double sumsq[2];
  int peak[2];
  u32 nonzero[2];
} stats;

static void stats_reset(stats *st)
{
  memset(st, 0, sizeof(*st));
  st->hash = 2166136261u;
}

/* Renders up to now and drains everything the mixer gives. */
static void drain(stats *st)
{
  static s16 buf[512];
  u32 n;
  render_gbc_sound();
  while ((n = sound_read_samples(buf, 256)) != 0)
  {
    u32 i;
    for (i = 0; i < n * 2; i++)
    {
      int v = buf[i], side = i & 1;
      unsigned char b0 = (unsigned char)v, b1 = (unsigned char)(v >> 8);
      st->hash = (st->hash ^ b0) * 16777619u;
      st->hash = (st->hash ^ b1) * 16777619u;
      st->count[side]++;
      st->sumsq[side] += (double)v * v;
      if (abs(v) > st->peak[side])
        st->peak[side] = abs(v);
      if (v)
        st->nonzero[side]++;
    }
    st->n += n * 2;
  }
}

static double rms(const stats *st, int side)
{
  return st->count[side] ? __builtin_sqrt(st->sumsq[side] / st->count[side]) : 0;
}

/* Time passes: n cycles, then whatever the mixer has is drained. A frontend
 * drains every frame, and the buffer holds only about 7. */
static void advance(stats *st, u32 cycles)
{
  while (cycles)
  {
    u32 step = cycles > FRAME ? FRAME : cycles;
    cpu_ticks += step;
    cycles -= step;
    drain(st);
  }
}

static void check(int ok, const char *name, const char *what)
{
  checks++;
  if (!ok)
  {
    failures++;
    printf("FAIL  %s: %s\n", name, what);
  }
}

/* A fresh mixer and all sound registers cleared. */
static void begin(stats *st)
{
  u32 i;
  reset_sound();
  cpu_ticks = 0;
  for (i = 0x60 >> 1; i < 0xB0 >> 1; i++)
    io_registers[i] = 0;
  for (i = 0; i < 4; i++)
  {
    timer[i].status = TIMER_INACTIVE;
    timer[i].prescale = 0;
    timer[i].irq = 0;
    timer[i].reload = timer[i].count = 0x10000;
    timer[i].direct_sound_channels = TIMER_DS_CHANNEL_NONE;
    timer[i].frequency_step = 0;
  }
  stats_reset(st);
  w32(A_SOUNDCNT_L, 0);
  w16(A_SOUNDCNT_X, 0x80);    /* sound master on */
  w16(A_SOUNDCNT_L, 0xFF77);  /* all channels both sides, volume 7 */
  w16(A_SOUNDCNT_H, 0x0002);  /* GBC volume 100% */
}

static void finish(const char *name, stats *st)
{
  drain(st);
  printf("%-28s %08x %6u  L %4d %7.1f  R %4d %7.1f\n", name, (unsigned)st->hash, (unsigned)st->n,
         st->peak[0], rms(st, 0), st->peak[1], rms(st, 1));
  last_hash = st->hash;
  total_hash = (total_hash ^ st->hash) * 16777619u;
  total_hash = (total_hash ^ st->n) * 16777619u;
}

/* ---- the tone channels ---- */

/* The square channels: duty, envelope (initial volume, direction 1 = up,
 * step time) and frequency rate. */
static u32 square_ctl(u32 duty, u32 length, u32 vol, u32 up, u32 step)
{
  return (vol << 12) | (up << 11) | (step << 8) | (duty << 6) | length;
}

static void case_square(int ch, u32 duty)
{
  char name[40];
  stats st;
  begin(&st);
  snprintf(name, sizeof(name), "square%d_duty%u", ch + 1, (unsigned)duty);
  if (ch == 0)
  {
    w16(A_SOUND1CNT_L, 0);
    w16(A_SOUND1CNT_H, square_ctl(duty, 0, 12, 0, 0));
    w16(A_SOUND1CNT_X, 0x8000 | 1750);
  }
  else
  {
    w16(A_SOUND2CNT_L, square_ctl(duty, 0, 12, 0, 0));
    w16(A_SOUND2CNT_H, 0x8000 | 1750);
  }
  advance(&st, FRAME / 2);
  advance(&st, FRAME * 3);
  check(r16(A_SOUNDCNT_X) & (1u << ch), name, "channel is on");
  check(rms(&st, 0) > 50 && rms(&st, 1) > 50, name, "it is audible");
  finish(name, &st);
}

/* Envelope: up from 0 and down from 15, 1 frame or so a step. */
static void case_envelope(int ch, int up)
{
  char name[40];
  stats a, b;
  begin(&a);
  stats_reset(&b);
  snprintf(name, sizeof(name), "square%d_env_%s", ch + 1, up ? "up" : "down");
  if (ch == 0)
  {
    w16(A_SOUND1CNT_H, square_ctl(2, 0, up ? 0 : 15, up, 1));
    w16(A_SOUND1CNT_X, 0x8000 | 1700);
  }
  else
  {
    w16(A_SOUND2CNT_L, square_ctl(2, 0, up ? 0 : 15, up, 1));
    w16(A_SOUND2CNT_H, 0x8000 | 1700);
  }
  advance(&a, FRAME * 2);          /* the first frames... */
  stats_reset(&b);
  advance(&a, FRAME * 24);
  advance(&b, FRAME * 2);          /* ...and after 26 frames */
  a.hash ^= b.hash;
  a.n += b.n;
  if (up)
    check(rms(&b, 0) > 100 && gbc_sound_channel[ch].envelope_volume == 15, name,
          "envelope climbed to 15 and plays");
  else
    check(gbc_sound_channel[ch].envelope_volume == 0 && rms(&b, 0) < 1.0, name, "envelope fell to 0 and is silent");
  finish(name, &a);
}

static void case_length(int ch, u32 length, int enabled, u32 frames_expiry)
{
  char name[48];
  stats st;
  u32 bit = 1u << ch;
  begin(&st);
  snprintf(name, sizeof(name), "square%d_len%u_%s", ch + 1, (unsigned)length, enabled ? "on" : "off");
  if (ch == 0)
  {
    w16(A_SOUND1CNT_H, square_ctl(2, length, 10, 0, 0));
    w16(A_SOUND1CNT_X, 0x8000 | (enabled ? 0x4000 : 0) | 1800);
  }
  else
  {
    w16(A_SOUND2CNT_L, square_ctl(2, length, 10, 0, 0));
    w16(A_SOUND2CNT_H, 0x8000 | (enabled ? 0x4000 : 0) | 1800);
  }
  advance(&st, FRAME);
  if (enabled && 64 - length <= 4)
    check(!(r16(A_SOUNDCNT_X) & bit) && !gbc_sound_channel[ch].active_flag,
          name, "short length expires within the first frame");
  else
    check(r16(A_SOUNDCNT_X) & bit, name, "on after one frame");
  advance(&st, FRAME * (frames_expiry + 4));
  if (enabled)
    check(!(r16(A_SOUNDCNT_X) & bit) && !gbc_sound_channel[ch].active_flag, name,
          "length ran out: the channel is off in SOUNDCNT_X");
  else
    check((r16(A_SOUNDCNT_X) & bit) && gbc_sound_channel[ch].active_flag, name,
          "no length counter: still on");
  finish(name, &st);
}

/* Sweep: shift, direction (1 = down), time. Up from a high rate overflows
 * 2047 and silences channel 1; down lowers the rate. */
static void case_sweep(const char *tag, u32 time, u32 down, u32 shift, u32 rate, int mode)
{
  char name[48];
  stats st;
  begin(&st);
  snprintf(name, sizeof(name), "sweep_%s", tag);
  w16(A_SOUND1CNT_L, (time << 4) | (down << 3) | shift);
  w16(A_SOUND1CNT_H, square_ctl(2, 0, 12, 0, 0));
  w16(A_SOUND1CNT_X, 0x8000 | rate);
  advance(&st, FRAME * 30);
  if (mode == 0)
    check(!gbc_sound_channel[0].active_flag && !(r16(A_SOUNDCNT_X) & 1), name, "overflow silenced channel 1");
  else if (mode == 1)
    check(gbc_sound_channel[0].active_flag && gbc_sound_channel[0].rate < rate, name,
          "rate went down and the channel plays on");
  else
    check(gbc_sound_channel[0].active_flag && gbc_sound_channel[0].rate > rate, name,
          "rate went up and the channel plays on");
  finish(name, &st);
}

/* Wave: 16 bytes of RAM per bank (a ramp and a different shape), 32 or 64
 * samples, volume code 0..3 (mute, 100%, 50%, 25%) or 4 (force 75%). */
static void write_wave(u32 pattern)
{
  u32 i;
  for (i = 0; i < 8; i++)
  {
    u32 a = (pattern ? (i * 2 + 1) * 13 : i * 2 * 17) & 0xFF;
    u32 b = (pattern ? 0xF0 - i * 15 : 0xFF - i * 31) & 0xFF;
    w16(A_WAVERAM + i * 2, a | (b << 8));
  }
}

static void case_wave(const char *tag, u32 type64, u32 bank, u32 volume)
{
  char name[48];
  stats st;
  u32 vol_bits = volume == 4 ? 0x8000 : volume << 13;
  begin(&st);
  snprintf(name, sizeof(name), "wave_%s", tag);
  /* Each write targets the bank opposite the selected playback bank. */
  w16(A_SOUND3CNT_L, 0x80 | (type64 << 5) | (1u << 6));
  write_wave(0);
  w16(A_SOUND3CNT_L, 0x80 | (type64 << 5) | (0u << 6));
  write_wave(1);
  w16(A_SOUND3CNT_L, 0x80 | (type64 << 5) | (bank << 6));
  w16(A_SOUND3CNT_H, vol_bits);
  w16(A_SOUND3CNT_X, 0x8000 | 1900);
  advance(&st, FRAME * 3);
  check(!!(r16(A_SOUNDCNT_X) & 4), name, "channel 3 is on");
  if (volume == 0)
    check(st.peak[0] == 0 && st.peak[1] == 0, name, "volume 0 is silent");
  else
    check(rms(&st, 0) > 20, name, "it is audible");
  finish(name, &st);
}

static void case_noise(u32 narrow, u32 ratio, u32 shift)
{
  char name[48];
  stats st;
  begin(&st);
  snprintf(name, sizeof(name), "noise_%s_r%u_s%u", narrow ? "7bit" : "15bit", (unsigned)ratio, (unsigned)shift);
  w16(A_SOUND4CNT_L, square_ctl(0, 0, 12, 0, 0));
  w16(A_SOUND4CNT_H, 0x8000 | (shift << 4) | (narrow << 3) | ratio);
  advance(&st, FRAME * 3);
  check(!!(r16(A_SOUNDCNT_X) & 8), name, "channel 4 is on");
  check(rms(&st, 0) > 50, name, "it is audible");
  finish(name, &st);
}

/* One channel (0..3) alone, routed: 0 none, 1 right, 2 left, 3 both. */
static void start_channel(int ch)
{
  switch (ch)
  {
    case 0: w16(A_SOUND1CNT_L, 0); w16(A_SOUND1CNT_H, square_ctl(2, 0, 13, 0, 0)); w16(A_SOUND1CNT_X, 0x8000 | 1750); break;
    case 1: w16(A_SOUND2CNT_L, square_ctl(1, 0, 13, 0, 0)); w16(A_SOUND2CNT_H, 0x8000 | 1650); break;
    case 2:
      w16(A_SOUND3CNT_L, 0xC0);
      write_wave(0);
      w16(A_SOUND3CNT_L, 0x80);
      w16(A_SOUND3CNT_H, 1 << 13);
      w16(A_SOUND3CNT_X, 0x8000 | 1800);
      break;
    default: w16(A_SOUND4CNT_L, square_ctl(0, 0, 13, 0, 0)); w16(A_SOUND4CNT_H, 0x8000 | 0x23); break;
  }
}

static void case_routing(int ch, u32 route)
{
  static const char *rn[4] = { "none", "right", "left", "both" };
  char name[48];
  stats st;
  u32 enable = 0;
  begin(&st);
  snprintf(name, sizeof(name), "route_ch%d_%s", ch + 1, rn[route]);
  if (route & 1)
    enable |= 1u << (ch + 8);
  if (route & 2)
    enable |= 1u << (ch + 12);
  w16(A_SOUNDCNT_L, enable | 0x77);
  start_channel(ch);
  advance(&st, FRAME * 3);
  check(!!(r16(A_SOUNDCNT_X) & (1u << ch)), name, "the channel is on");
  if (route & 2)
    check(rms(&st, 0) > 20, name, "left is audible");
  else
    check(st.peak[0] == 0, name, "left is silent");
  if (route & 1)
    check(rms(&st, 1) > 20, name, "right is audible");
  else
    check(st.peak[1] == 0, name, "right is silent");
  finish(name, &st);
}

/* Volumes: SOUNDCNT_L's left and right 0..7, and SOUNDCNT_H's 25/50/100%. */
static void case_master(u32 left, u32 right, u32 gbc)
{
  char name[48];
  stats st;
  begin(&st);
  snprintf(name, sizeof(name), "master_l%u_r%u_g%u", (unsigned)left, (unsigned)right, (unsigned)gbc);
  w16(A_SOUNDCNT_L, 0xFF00 | (left << 4) | right);
  w16(A_SOUNDCNT_H, gbc);
  start_channel(0);
  advance(&st, FRAME * 3);
  if (left == 0 || gbc == 3)
    check(rms(&st, 0) < 40, name, "left is (nearly) silent");
  if (right == 0 || gbc == 3)
    check(rms(&st, 1) < 40, name, "right is (nearly) silent");
  if (left && right && gbc != 3)
    check(rms(&st, 0) > 5 && rms(&st, 1) > 5, name, "audible");
  finish(name, &st);
}

static void case_master_onoff(void)
{
  stats st, mid;
  begin(&st);
  start_channel(0);
  advance(&st, FRAME * 2);
  check(!!(r16(A_SOUNDCNT_X) & 1), "master_onoff", "channel 1 on");
  w16(A_SOUNDCNT_X, 0);        /* sound master off: all stops */
  stats_reset(&mid);
  advance(&mid, FRAME);        /* the frame it went off in (written at its start) */
  stats_reset(&mid);
  advance(&mid, FRAME * 2);
  check(mid.peak[0] == 0 && mid.peak[1] == 0, "master_onoff", "silent while off");
  check(!(r16(A_SOUNDCNT_X) & 0xF), "master_onoff", "no channel is on");
  w16(A_SOUNDCNT_X, 0x80);
  start_channel(0);
  stats_reset(&mid);
  advance(&mid, FRAME * 2);
  check(rms(&mid, 0) > 20, "master_onoff", "audible again after on");
  advance(&st, FRAME);
  finish("master_onoff", &st);
}

/* All four channels at once with the routing of a real game. */
static void case_mix(void)
{
  stats st;
  begin(&st);
  w16(A_SOUNDCNT_L, 0xBB66 | 0x1100 | 0x0000);
  start_channel(0);
  start_channel(1);
  start_channel(2);
  start_channel(3);
  advance(&st, FRAME * 4);
  w16(A_SOUND1CNT_X, 0x8000 | 1500);
  advance(&st, FRAME * 4);
  finish("mix_all", &st);
}

/* Channel 2 left must not also route channel 3 right. */
static void case_routing_crosstalk(void)
{
  stats st;
  begin(&st);
  w16(A_SOUNDCNT_L, (1u << 13) | 0x77);   /* only channel 2 on the left */
  start_channel(2);                        /* a playing channel 3 */
  advance(&st, FRAME * 3);
  check(st.peak[0] == 0 && st.peak[1] == 0, "route crosstalk",
        "an unrelated channel route stays silent");
  finish("route_crosstalk", &st);
}

static void case_wave_bank_and_noise_period(void)
{
  extern s8 wave_samples[64];
  stats st;
  begin(&st);
  w16(A_SOUNDCNT_X, 0);  /* Wave memory is writable with master sound off. */
  w16(A_SOUND3CNT_L, 0xC0);
  w16(A_WAVERAM, 0x7654);
  w16(A_SOUND3CNT_L, 0x80);
  check(r16(A_WAVERAM) == 0, "wave bank", "new CPU-visible bank is independent");
  w16(A_WAVERAM, 0xFEDC);
  w16(A_SOUND3CNT_L, 0xC0);
  check(r16(A_WAVERAM) == 0x7654, "wave bank", "zero-time switch retains previous bank");
  check(wave_samples[0] == -3 && wave_samples[1] == -4 &&
        wave_samples[32] == 5 && wave_samples[33] == 4,
        "wave bank", "writes update opposite playback bank while output is off");
  w16(A_SOUND3CNT_L, 0xE0);
  w16(A_SOUND3CNT_X, 0x8000 | 1900);
  check(gbc_sound_channel[2].sample_index == (32u << 16), "wave bank",
        "64-sample playback starts with selected bank");

  begin(&st);
  w16(A_SOUND4CNT_L, 0xF000);
  w16(A_SOUND4CNT_H, 0x8008); /* 7-bit LFSR, 8 shifts per output sample */
  cpu_ticks += 16 * (GBC_BASE_RATE_INT / GBA_SOUND_FREQUENCY);
  render_gbc_sound();
  check(gbc_sound_channel[3].sample_index == (1u << 16), "noise period",
        "128 shifts wrap a 127-state cycle once");
  w16(A_SOUND4CNT_H, 0);  /* Switch width without restarting the phase. */
  cpu_ticks += FRAME;
  render_gbc_sound();
  w16(A_SOUND4CNT_H, 8);
  cpu_ticks += FRAME;
  render_gbc_sound();
  check(gbc_sound_channel[3].sample_index < (127u << 16), "noise period",
        "wide-to-narrow mode change normalizes its table index");
}

static void case_fifo_extreme_steps(void)
{
  static const u32 steps[] = {0, 1, 0xFFFFFF, 0x1000000, 0x80000000,
                             0xFF000001, 0xFFFFFFFF};
  stats st;
  u32 i;
  for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
  {
    direct_sound_struct *ds;
    u32 j;
    begin(&st);
    ds = &direct_sound_channel[0];
    ds->fifo_fractional = 0xFFFFFF;
    ds->buffer_index = BUFFER_SIZE - 2;
    for (j = 0; j < 3; j++)
    {
      u32 frac = ds->fifo_fractional;
      u32 index = ds->buffer_index;
      unsigned long long n = steps[i] ?
        (0x1000000ull - frac + steps[i] - 1) / steps[i] : 0;
      /* step 1 gets an entire ring's worth on the second call. One
       * boundary call is enough to exercise its exact interpolation. */
      if (steps[i] == 1 && j) break;
      sound_timer(steps[i], 0);
      check(ds->fifo_fractional == ((frac + n * steps[i]) & 0xFFFFFF),
            "fifo extreme step", "fraction is exact after wrap");
      check(ds->buffer_index == ((index + 2 * n) & BUFFER_SIZE_MASK),
            "fifo extreme step", "index counts finite output frames");
      check(ds->fifo_base == j + 1, "fifo extreme step", "one FIFO byte popped");
    }
  }
}

/* Validate every channel before applying any fields, including the channels
 * that the old nested-loop index skipped. The surrounding document is small
 * but has the same representation as a normal full save state. */
static void sound_snapshot(u8 *out)
{
  u8 *p = out + 4, *header = out;
  p += sound_write_savestate(p);
  *p++ = 0;
  bson_write_u32(header, p - out);
}

static void case_sound_savestate(void)
{
  static const struct { const char *doc, *key; u32 value; } bad[] = {
    {"", "on", 2}, {"", "gbc-buf-idx", BUFFER_SIZE},
    {"", "buf-base", 1}, {"", "gbc-partial-ticks", 0x10000},
    {"", "gbc-ms-vol-left", 8}, {"", "gbc-ms-vol-right", 8},
    {"", "gbc-ms-vol", 4},
    {"ds0", "fifo-base", 32}, {"ds1", "fifo-top", 32},
    {"ds0", "volume", 32}, {"ds1", "status", 4},
    {"ds1", "buf-idx", 0xFFFFFFFFu}, {"ds1", "fifo-frac", 0x1000000},
    {"gs0", "sample-tbl", 4}, {"gs1", "env-vol", 16},
    {"gs2", "wav-vol", 16385}, {"gs3", "noise-type", 2},
    {"gs0", "sweep-shift", 32}, {"gs3", "freq-step", 2097153}
  };
  u8 original[4096], modified[4096], after[4096];
  stats st;
  unsigned i;
  begin(&st);
  sound_snapshot(original);
  check(sound_check_savestate(original), "sound state", "fresh state accepted");
  for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
  {
    const u8 *doc;
    u8 *p;
    memcpy(modified, original, sizeof(modified));
    doc = bson_find_document(modified, "sound");
    if (*bad[i].doc) doc = bson_find_document(doc, bad[i].doc);
    p = (u8 *)bson_find_key(doc, bad[i].key);
    bson_write_u32(p, bad[i].value);
    check(!sound_check_savestate(modified) && !sound_read_savestate(modified),
          "sound state", "bad table index/shift rejected");
    sound_snapshot(after);
    check(!memcmp(original, after, bson_read_u32(original)),
          "sound state", "rejection leaves all channels untouched");
  }
  for (i = 0; i < 6; i++)
  {
    char key[4] = {i < 2 ? 'd' : 'g', 's', '0' + (i < 2 ? i : i - 2), 0};
    const u8 *doc;
    u8 *p;
    memcpy(modified, original, sizeof(modified));
    doc = bson_find_document(modified, "sound");
    p = (u8 *)bson_find_document(doc, key);
    p[-4] = 'x';  /* rename the expected nested document */
    check(!sound_check_savestate(modified), "sound state", "each channel required");
  }
  memcpy(modified, original, sizeof(modified));
  ((u8 *)bson_find_key(bson_find_document(modified, "sound"), "wav-samples"))[5] = 127;
  check(!sound_check_savestate(modified), "sound state", "wave values bounded before volume multiply");
  start_channel(0);
  cpu_ticks += FRAME * 2;
  render_gbc_sound();
  check(!sound_read_samples(NULL, 1), "sound read", "null output does not consume PCM");
  sound_snapshot(original);
  check(sound_read_savestate(original), "sound state", "playing state accepted");
  {
    s16 pcm[2];
    check(!sound_read_samples(pcm, 1), "sound state", "old timeline PCM discarded");
    reset_sound();
    gbc_sound_buffer_index = 514;
    check(sound_read_samples(pcm, 0x80000000u) == 1, "sound read",
          "huge frame request is bounded before multiplying");
  }
}

/* ---- the FIFOs ---- */

static u32 fifo_seed = 1;
static u32 fifo_word(int ch, u32 shape)
{
  u32 i, w = 0;
  for (i = 0; i < 4; i++)
  {
    int v;
    fifo_seed++;
    if (shape == 0)                      /* a triangle, different per channel */
      v = (int)((fifo_seed * (ch ? 7 : 5)) & 63) - 32;
    else                                 /* a square-ish wave, amplitude 100 */
      v = ((fifo_seed >> (ch ? 3 : 2)) & 1) ? 100 : -100;
    w |= (u32)(v & 0xFF) << (i * 8);
  }
  return w;
}

/* A timer run at "period" cycles per overflow for count overflows: each
 * overflow pops a sample from the FIFOs on that timer (update_timers in
 * main.c); the "DMA" refills a FIFO of 16 or fewer samples by 4 words. */
static void run_timer(stats *st, int t, u32 period, u32 count, u32 shape)
{
  u32 since_drain = 0;
  while (count--)
  {
    int ch;
    cpu_ticks += period;
    since_drain += period;
    for (ch = 0; ch < 2; ch++)
      if (timer[t].direct_sound_channels & (1u << ch))
      {
        sound_timer(timer[t].frequency_step, ch);
        if (((direct_sound_channel[ch].fifo_top - direct_sound_channel[ch].fifo_base) % 32) <= 16)
        {
          u32 k;
          for (k = 0; k < 4; k++)
            w32(ch ? A_FIFO_B : A_FIFO_A, fifo_word(ch, shape));
        }
      }
    if (since_drain >= FRAME / 2)
    {
      drain(st);
      since_drain = 0;
    }
  }
  drain(st);
}

/* Starts timer t with an overflow period of "period" cycles. */
static void start_timer(int t, u32 period)
{
  w16(A_TM0D + t * 4, 0x10000 - period);
  w16(A_TM0CNT + t * 4, 0x80);
}

/* FIFO channel ch (0 = A, 1 = B) on timer t at period, routed (bit 0 right,
 * bit 1 left), volume 100% (full = 1) or 50%. */
static void case_fifo(int ch, int t, u32 period, u32 route, u32 full)
{
  static const char *rn[4] = { "none", "right", "left", "both" };
  char name[56];
  stats st;
  u32 h;
  begin(&st);
  snprintf(name, sizeof(name), "fifo%c_t%d_p%u_%s_%s", 'A' + ch, t, (unsigned)period, rn[route],
           full ? "full" : "half");
  /* SOUNDCNT_H: GBC volume 100% (2), A volume bit 2, B volume bit 3, A
   * right/left bits 8/9, B bits 12/13, timer select bits 10 and 14 */
  h = 0x0002;
  w16(A_SOUNDCNT_L, 0);          /* the tone channels off: only the FIFO */
  if (ch == 0)
    h |= (full ? 0x0004 : 0) | (route << 8) | (t << 10);
  else
    h |= (full ? 0x0008 : 0) | (route << 12) | (t << 14);
  w16(A_SOUNDCNT_H, h);
  fifo_seed = 1 + ch * 100 + t * 7;
  { u32 k; for (k = 0; k < 4; k++) w32(ch ? A_FIFO_B : A_FIFO_A, fifo_word(ch, 0)); }
  start_timer(t, period);
  run_timer(&st, t, period, 5 * FRAME / period, 0);
  if (route & 2)
    check(rms(&st, 0) > 3, name, "left is audible");
  else
    check(st.peak[0] == 0, name, "left is silent");
  if (route & 1)
    check(rms(&st, 1) > 3, name, "right is audible");
  else
    check(st.peak[1] == 0, name, "right is silent");
  check(direct_sound_channel[ch].fifo_top < 32 && direct_sound_channel[ch].fifo_base < 32, name,
        "FIFO positions in range");
  finish(name, &st);
}

/* Both FIFOs at once on the two timers (two rates), with the tone channels. */
static void case_fifo_both(u32 pa, u32 pb, int with_tone)
{
  char name[56];
  stats st;
  u32 n, k;
  begin(&st);
  snprintf(name, sizeof(name), "fifo_both_%u_%u%s", (unsigned)pa, (unsigned)pb, with_tone ? "_tone" : "");
  if (with_tone)
  {
    w16(A_SOUNDCNT_L, 0x1177 | 0x0200);
    start_channel(0);
    start_channel(3);
  }
  else
    w16(A_SOUNDCNT_L, 0);
  w16(A_SOUNDCNT_H, 0x0002 | 0x0004 | 0x0008 | (3u << 8) | (1u << 10) | (3u << 12) | (0u << 14));
  fifo_seed = 3;
  for (k = 0; k < 4; k++) { w32(A_FIFO_A, fifo_word(0, 1)); w32(A_FIFO_B, fifo_word(1, 0)); }
  start_timer(0, pb);   /* B on timer 0 */
  start_timer(1, pa);   /* A on timer 1 */
  for (n = 0; n < 5; n++)
  {
    /* both timers interleaved by their own periods */
    u32 next0 = pb, next1 = pa;
    u32 end = FRAME;
    while (next0 <= end || next1 <= end)
    {
      int which = next0 <= next1 ? 0 : 1;
      u32 at = which ? next1 : next0;
      int ch = which ? 0 : 1;
      cpu_ticks = n * FRAME + at;
      sound_timer(timer[which].frequency_step, ch);
      if (((direct_sound_channel[ch].fifo_top - direct_sound_channel[ch].fifo_base) % 32) <= 16)
        for (k = 0; k < 4; k++)
          w32(ch ? A_FIFO_B : A_FIFO_A, fifo_word(ch, ch ? 0 : 1));
      if (which)
        next1 += pa;
      else
        next0 += pb;
    }
    cpu_ticks = (n + 1) * FRAME;
    drain(&st);
  }
  check(rms(&st, 0) > 3 && rms(&st, 1) > 3, name, "both sides audible");
  finish(name, &st);
}

int main(void)
{
  int ch;
  u32 i, bank0 = 0;

  init_sound();

  for (ch = 0; ch < 2; ch++)
    for (i = 0; i < 4; i++)
      case_square(ch, i);
  for (ch = 0; ch < 2; ch++)
  {
    case_envelope(ch, 1);
    case_envelope(ch, 0);
  }
  for (ch = 0; ch < 2; ch++)
  {
    case_length(ch, 63, 1, 3);    /* 1 tick: gone within a frame or two */
    case_length(ch, 32, 1, 9);    /* 32 ticks: 0.125 s, 7.5 frames */
    case_length(ch, 0, 1, 16);    /* 64 ticks: 0.25 s, 15 frames */
    case_length(ch, 40, 0, 0);    /* length counter off */
  }
  case_sweep("up_overflow", 1, 0, 1, 1200, 0);
  case_sweep("up_slow", 3, 0, 6, 300, 2);
  case_sweep("down_a", 1, 1, 2, 1500, 1);
  case_sweep("down_b", 2, 1, 1, 1900, 1);
  case_sweep("down_c", 7, 1, 7, 1000, 1);
  for (i = 0; i < 5; i++)
  {
    static const char *vn[5] = { "mute", "full", "half", "quarter", "force75" };
    char tag[32];
    snprintf(tag, sizeof(tag), "32_bank0_%s", vn[i]);
    case_wave(tag, 0, 0, i);
    bank0 = last_hash;
    snprintf(tag, sizeof(tag), "32_bank1_%s", vn[i]);
    case_wave(tag, 0, 1, i);
    if (i)
      check(last_hash != bank0, tag, "the two banks sound different");
    snprintf(tag, sizeof(tag), "64_%s", vn[i]);
    case_wave(tag, 1, 0, i);
  }
  for (i = 0; i < 2; i++)
  {
    case_noise(i, 0, 3);
    case_noise(i, 1, 2);
    case_noise(i, 3, 5);
    case_noise(i, 7, 1);
    case_noise(i, 2, 8);
  }
  for (ch = 0; ch < 4; ch++)
    for (i = 0; i < 4; i++)
      case_routing(ch, i);
  case_routing_crosstalk();
  case_wave_bank_and_noise_period();
  case_fifo_extreme_steps();
  case_sound_savestate();
  for (i = 0; i < 8; i++)
    case_master(i, 7 - i, 2);
  for (i = 0; i < 4; i++)
    case_master(7, 7, i);
  case_master_onoff();
  case_mix();

  /* the FIFOs: each channel on each timer (two rates), every routing and
   * both volumes */
  for (ch = 0; ch < 2; ch++)
  {
    int t;
    for (t = 0; t < 2; t++)
    {
      u32 period = t ? 512 : 1280;     /* 32768 Hz and 13107 Hz */
      for (i = 0; i < 4; i++)
      {
        case_fifo(ch, t, period, i, 1);
        case_fifo(ch, t, period, i, 0);
      }
    }
  }
  case_fifo(0, 0, 2048, 3, 1);          /* 8192 Hz */
  case_fifo(1, 1, 1536, 2, 1);          /* a rate that does not divide a frame */
  case_fifo_both(1280, 512, 0);
  case_fifo_both(1024, 1365, 1);

  printf("tone hash %08x\n", (unsigned)total_hash);
  printf("%s: %d of %d checks passed\n", failures ? "FAIL" : "PASS", checks - failures, checks);
  return failures != 0;
}
