/* The real APU writes nonzero game PCM through resampling, the producer
 * ring, actual ua_os callbacks and the captured TI transfer ABI fake. */
#include "common.h"
#include "nspire_audio.h"
#include "nspire_bt_glue.h"
#include "nspire_os.h"
#include "ua_os.h"
/* (nspire_audio.c USB_PIPE_MS: the queue ahead of the bus and half a transfer; 52 with 7 of 8, 49 with 4 of 14) */
#define SINK_PIPE_MS ((UA_NX - 1) * UA_NF + UA_NF / 2)
#include "fake/fake_os.h"
#include "corpus.h"
#include "t.h"

extern u32 unit_ticks;
extern int unit_usb_plugged;
static u32 slept_at, usb_at;
static unsigned sleeps, irq_windows, draining;
void nspire_audio_draining(void) { draining++; T_TRUE(nspire_usbaudio_owns_usb()); }
cpu_alert_type function_cc write_io_register16(u32 address, u32 value);

void host_os_irq_window(void) { irq_windows++; }
void nspire_os_after_os_time(void) { }
uint32_t nspire_os_since_sleep(void) { return unit_ticks - slept_at; }
int nspire_os_sleep_near(uint32_t max_wait) { (void)max_wait; return 0; }
/* 2026-11-15: what nspire_audio.c's OS time and log read (the OS's tick's
 * phase unknown here: the frame loop's OS time is tests/audio/pace_test.c's) */
int nspire_os_next_tick_in(uint32_t *ticks, uint32_t *period) { *ticks = *period = 327; return 0; }
/* (speed9a) the frame loop's watch for the sound's task tick: none here (the sound's own sleeps) */
int nspire_frameskip_os_task(u32 force) { (void)force; return 0; }
uint32_t nspire_os_near_wait(uint32_t since, uint32_t from, uint32_t until) { (void)since; (void)from; (void)until; return 33; }
/* (speed18) no controller frame clock here: this fixture's bus runs only in
 * the OS's sleeps, so FRINDEX would stand still while gpSP works and the
 * queue's clock (nspire_audio.c queue_now) would read a full queue for ever;
 * the fixed limit is what these tests drive. The clock's rule is pace_test's,
 * whose bus runs in gpSP's time too. */
int nspire_usb_kind(void) { return 0; }
int nspire_os_keypad_keep_off(void) { return 0; }
#include "nspire_frameskip.h"
struct nspire_perf nspire_perf_totals;
int __wrap_nspire_os_sleep(void)
{
  sleeps++;
  if (unit_ticks - usb_at < 327) unit_ticks = usb_at + 327;
  while (unit_ticks - usb_at >= 327) {
    fos_sleep();
    usb_at += 327;
  }
  slept_at = unit_ticks;
  return 1;
}

static void tone(void)
{
  write_io_register16(0x84, 0x80);
  write_io_register16(0x82, 2);
  write_io_register16(0x80, 0x1177);
  write_io_register16(0x62, 0xF080);
  write_io_register16(0x64, 0x8400);
}

static void frames(unsigned n, int normal)
{
  for (unsigned i = 0; i < n; i++) {
    unit_ticks += 548;
    nspire_audio_frame_begin(normal);
    cpu_ticks += 280896;
    render_gbc_sound();
    sound_flush_all_pops();
    nspire_audio_frame_end();
  }
}

static unsigned captured(void)
{
  const uint8_t *p;
  return fos_cap_bytes(&p);
}

static int any_nonzero(unsigned from)
{
  const uint8_t *p;
  unsigned n = fos_cap_bytes(&p);
  for (unsigned i = from; i < n; i++) if (p[i]) return 1;
  return 0;
}

static void reset(void)
{
  nspire_audio_stop();
  fos_init();
  unit_ticks = slept_at = usb_at = sleeps = irq_windows = 0;
  unit_usb_plugged = 0;
  nspire_audio_enabled = 1; nspire_audio_volume = 10;
  nspire_audio_mono = nspire_audio_buffer = 0;
  init_sound();
  for (unsigned i = 0; i < 4; i++) dma[i].direct_sound_channel = DMA_NO_DIRECT_SOUND;
  nspire_audio_start();
  tone();
}

static void connect(unsigned flags)
{
  struct fos_spec s = {0x0d8c, 0x000c, cfg_cm108, sizeof cfg_cm108, flags};
  T_EQ(fos_plug(&s), 0);
  __wrap_nspire_os_sleep();
  nspire_audio_poll();
}

static void clean(void)
{
  nspire_audio_stop();
  T_EQ(nspire_usbaudio_active(), 0);
  T_EQ(nspire_usbaudio_owns_usb(), 0);
  T_EQ(nspire_os_wanted & OS_WANT_AUDIO, 0);
  fos_late_callback();
  T_EQ(fos_stats()->violations, 0);
  T_EQ(fos_stats()->pipes_open, 0);
  T_EQ(fos_stats()->xfers_open, 0);
}

static void test_no_device(void)
{
  struct nspire_audio_stats s;
  reset();
  frames(90, 1);
  nspire_audio_stats(&s);
  T_EQ(s.rendering, 0);
  T_EQ(sleeps, 0);
  T_EQ(nspire_usbaudio_owns_usb(), 0);
  T_EQ(nspire_os_wanted & OS_WANT_AUDIO, 0);
  T_TRUE(read_ioreg(REG_SOUNDCNT_X) & 1); /* APU continued while silent */
  clean();
}

static void test_play_pause_resume(void)
{
  struct nspire_audio_stats s;
  reset(); connect(0);
  T_TRUE(nspire_usbaudio_active()); T_TRUE(nspire_usbaudio_owns_usb());
  frames(60, 1);
  nspire_audio_stats(&s);
  T_EQ(s.rate, 44100); T_TRUE(s.rendering); T_EQ(s.priming, 0);
  T_TRUE(s.completions > 20); T_TRUE(any_nonzero(0)); T_TRUE(irq_windows);
  nspire_audio_pause();
  /* Old submitted packets finish in <=48 ms, then all packets are silent. */
  frames(8, 0);
  unsigned start = captured();
  frames(20, 0);
  T_TRUE(captured() > start); T_EQ(any_nonzero(start), 0);
  nspire_audio_stats(&s); T_EQ(s.rendering, 0);
  start = captured(); frames(12, 1); T_TRUE(any_nonzero(start));
  /* A reset discards the outgoing ring and resampler history. */
  reset_sound(); frames(8, 1);
  start = captured(); frames(12, 1); T_EQ(any_nonzero(start), 0);
  clean();
}

/* r19-snd: a pause (the menu, a load) ends the wire's sound with a fade, not
 * a cut (the driver paused at once played the queued transfers, then
 * silence wherever the wave was), and the sound after it starts with one.
 * The tone's peak in the last ms before the silence against its peak 10-20
 * ms earlier, and in the first half ms after it against 10-20 ms later. */
static int peak(const int16_t *pcm, unsigned from, unsigned to)
{
  int m = 0;
  for (unsigned i = from; i < to; i++) {
    int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
    if (v > m) m = v;
  }
  return m;
}

static void test_pause_fades(void)
{
  const uint8_t *p;
  const int16_t *pcm;
  unsigned start, n, last = 0, first = 0, i, ms;
  reset(); connect(0);
  frames(60, 1);
  start = captured() / 2;
  nspire_audio_pause();
  frames(20, 0);                           /* (a menu's frames: paused) */
  n = fos_cap_bytes(&p) / 2;
  pcm = (const int16_t *)(const void *)p;
  for (i = start; i < n; i++)
    if (pcm[i]) last = i;
  ms = 44100 / 1000 * 2;                   /* (stereo samples in a ms at 44.1 kHz) */
  T_TRUE(last > start + 20 * ms);
  T_TRUE(n > last + 10 * ms);              /* silence after it */
  printf("    stop: peak %d in the last ms, %d 10-20 ms before\n", peak(pcm, last - ms, last + 1),
         peak(pcm, last - 20 * ms, last - 10 * ms));
  T_TRUE(peak(pcm, last - ms, last + 1) * 2 < peak(pcm, last - 20 * ms, last - 10 * ms));
  start = n;
  frames(20, 1);                           /* (the game again) */
  n = fos_cap_bytes(&p) / 2;
  pcm = (const int16_t *)(const void *)p;
  for (i = start; i < n && !first; i++)
    if (pcm[i]) first = i;
  T_TRUE(first && n > first + 20 * ms);
  printf("    start: peak %d in the first half ms, %d 10-20 ms after\n", peak(pcm, first, first + ms / 2),
         peak(pcm, first + 10 * ms, first + 20 * ms));
  T_TRUE(peak(pcm, first, first + ms / 2) * 2 < peak(pcm, first + 10 * ms, first + 20 * ms));
  clean();
}

/* Settings > Sound > Output: Bluetooth means the headphones
 * only (none here: the adapter gets silence, nothing rendered), USB adapter
 * and Automatic the adapter. */
static void test_output_setting(void)
{
  struct nspire_audio_stats s;
  unsigned start;
  reset(); connect(0);
  nspire_audio_output = NSPIRE_OUTPUT_BT;
  frames(60, 1);
  nspire_audio_stats(&s);
  T_EQ(s.rendering, 0);
#ifdef UA_HAS_IDLE
  T_TRUE(s.card_idle);                         /* (bt10: the card idle: nothing comes out of it) */
#else
  T_TRUE(s.completions > 20);                 /* (the adapter still streams: silence) */
#endif
  start = captured(); frames(20, 1); T_EQ(any_nonzero(start), 0);
  nspire_audio_output = NSPIRE_OUTPUT_USB;
  frames(8, 1);
  start = captured(); frames(12, 1); T_TRUE(any_nonzero(start));
  nspire_audio_stats(&s); T_TRUE(s.rendering);
  nspire_audio_output = NSPIRE_OUTPUT_BT;
  frames(8, 1);
  start = captured(); frames(20, 1); T_EQ(any_nonzero(start), 0);
  nspire_audio_output = NSPIRE_OUTPUT_AUTO;
  frames(8, 1);
  start = captured(); frames(12, 1); T_TRUE(any_nonzero(start));
  clean();
}

/* bt10: both outputs at once. The glue's audio calls are wrapped (sink.mk
 * links with --wrap): headphones that stream when hp_on, taking the sound
 * as the glue decides (Output Automatic or Bluetooth, sound on, volume),
 * their writes counted: frames, flags, the words' nonzero ones (the tone,
 * or the zeros of a mixer someone cleared before them), and their delay. */
static int hp_on, hp_taking;
static unsigned long hp_frames, hp_nonzero, hp_calls, hp_clear_calls;
static unsigned hp_last_flags;
extern unsigned nspire_bt_live;
int __real_nspire_bt_glue_audio_frame(int mode, int usb_streaming);
int __wrap_nspire_bt_glue_audio_frame(int mode, int usb_streaming);
int __wrap_nspire_bt_glue_audio_frame(int mode, int usb_streaming)
{
  if (!hp_on)
    return __real_nspire_bt_glue_audio_frame(mode, usb_streaming);
  hp_taking = mode && nspire_audio_output != NSPIRE_OUTPUT_USB && nspire_audio_enabled && nspire_audio_volume;
  return hp_taking;
}
void __wrap_nspire_bt_glue_audio_write_mix(uint32_t *w1, unsigned n1, uint32_t *w2, unsigned n2, unsigned flags);
void __wrap_nspire_bt_glue_audio_write_mix(uint32_t *w1, unsigned n1, uint32_t *w2, unsigned n2, unsigned flags)
{
  unsigned i;
  hp_calls++;
  hp_last_flags = flags;
  hp_clear_calls += flags & 1;
  hp_frames += n1 + n2;
  for (i = 0; i < n1; i++) { hp_nonzero += w1[i] != 0; if (flags & 1) w1[i] = 0; }
  for (i = 0; i < n2; i++) { hp_nonzero += w2[i] != 0; if (flags & 1) w2[i] = 0; }
}
unsigned __wrap_nspire_bt_glue_audio_delay_ms(void);
static unsigned hp_delay = 273;
unsigned __wrap_nspire_bt_glue_audio_delay_ms(void) { return hp_on ? hp_delay : 0; }

static void test_both_outputs(void)
{
  struct nspire_audio_stats s;
  unsigned start, alone, both;
  unsigned long f0, nz0, calls0;
  reset(); connect(0);
  nspire_audio_output = NSPIRE_OUTPUT_AUTO;
  frames(90, 1);
  nspire_audio_stats(&s);
  T_TRUE(s.rendering); T_EQ(s.priming, 0);
  alone = s.buffered;                          /* the adapter's ring at its own target */
  /* headphones in: Automatic plays both; the adapter reads the frames after
   * them, so they don't clear the mixer, and its ring is held back */
  hp_on = 1; nspire_bt_live = 1;
  frames(60, 1);
  f0 = hp_frames; nz0 = hp_nonzero; calls0 = hp_calls;
  start = captured(); frames(60, 1);
  T_TRUE(any_nonzero(start));                  /* the adapter plays */
  T_TRUE(hp_frames - f0 > 60 * 1000);          /* the headphones get every frame's sound */
  T_TRUE(hp_nonzero - nz0 > 60 * 900);         /* ...the tone, not a cleared mixer */
  T_EQ(hp_calls - calls0, 60);                 /* one write a frame */
  T_EQ(hp_last_flags & 1, 0);                  /* (no clear: the adapter reads them after) */
  nspire_audio_stats(&s);
  T_TRUE(s.rendering); T_EQ(s.priming, 0);
  both = s.buffered;
  T_TRUE(both > alone * 2);                    /* held back to the headphones' delay */
  T_TRUE(s.sync_ms >= 273 - SINK_PIPE_MS - 1 && s.sync_ms <= 273 - SINK_PIPE_MS + 1);
  /* Output Bluetooth: the headphones alone, the mixer cleared by them */
  nspire_audio_output = NSPIRE_OUTPUT_BT;
  frames(8, 1);
  f0 = hp_frames; nz0 = hp_nonzero;
  start = captured(); frames(20, 1);
  T_EQ(any_nonzero(start), 0);                 /* the adapter: silence */
  T_TRUE(hp_frames - f0 > 20 * 1000); T_TRUE(hp_nonzero - nz0 > 20 * 900);
  T_EQ(hp_last_flags & 1, 1);
  nspire_audio_stats(&s); T_EQ(s.rendering, 0);

  /* Output USB adapter: the adapter alone, its own delay again */
  nspire_audio_output = NSPIRE_OUTPUT_USB;
  frames(8, 1);
  f0 = hp_frames;
  start = captured(); frames(60, 1);
  T_TRUE(any_nonzero(start)); T_EQ(hp_frames, f0);
  nspire_audio_stats(&s);
  T_TRUE(s.buffered < both);
  /* a menu pauses both */
  nspire_audio_output = NSPIRE_OUTPUT_AUTO;
  frames(30, 1);
  nspire_audio_pause();
  f0 = hp_frames;
  frames(8, 0);
  start = captured(); frames(20, 0);
  T_EQ(any_nonzero(start), 0); T_EQ(hp_frames, f0);
  /* headphones out: the adapter alone at its own delay */
  hp_on = 0;
  frames(90, 1);
  nspire_audio_stats(&s);
  T_TRUE(s.rendering); T_TRUE(s.buffered < both);
  start = captured(); frames(12, 1); T_TRUE(any_nonzero(start));
  nspire_bt_live = 0;
  clean();
}

/* bt10: the card's re-syncs. The headphones' delay wobbling within
 * the slack, or off it for less than 2 s: none; off it for good: one,
 * faded, then the card plays again. A gap in the game's sound (an OS call:
 * the card's queue served, nothing made) leaves the card ahead of the
 * headphones: re-synced (a moment of silence), not left ahead for ~50 s a
 * 100 ms. */
/* the card's ring at the frames' starts, averaged over n frames */
static unsigned ring_avg(unsigned n)
{
  struct nspire_audio_stats s;
  unsigned long sum = 0;
  for (unsigned i = 0; i < n; i++) {
    frames(1, 1);
    nspire_audio_stats(&s);
    sum += s.buffered;
  }
  return (unsigned)(sum / n);
}

static void test_sync_resync(void)
{
  struct nspire_audio_stats s;
  unsigned start, alone;
  unsigned syncs_in;
  reset(); connect(0);
  nspire_audio_output = NSPIRE_OUTPUT_AUTO;
  frames(90, 1);
  nspire_audio_stats(&s);
  alone = s.buffered;
  syncs_in = s.syncs;
  hp_on = 1; nspire_bt_live = 1; hp_delay = 273;
  frames(1, 1);
  nspire_audio_stats(&s);
  T_EQ(s.syncs, syncs_in);                     /* the headphones come: the card follows them (a 4-frame fade first) */
  frames(4, 1);
  nspire_audio_stats(&s);
  T_EQ(s.syncs, syncs_in + 1);
  frames(60, 1);
  nspire_audio_stats(&s);
  T_TRUE(s.buffered > alone * 2); T_EQ(s.priming, 0);
  /* bt10: the headphones' delay wobbling within the slack, or off it
   * for less than 2 s: no re-sync; off it for good: one, faded, then the card
   * plays again */
  {
    unsigned k, syncs0 = s.syncs;
    for (k = 0; k < 200; k++) {
      hp_delay = k & 1 ? 290 : 260;
      frames(1, 1);
    }
    hp_delay = 210;
    frames(100, 1);
    nspire_audio_stats(&s);
    T_EQ(s.syncs, syncs0);
    hp_delay = 273;
    frames(150, 1);
    nspire_audio_stats(&s);
    T_EQ(s.syncs, syncs0);
    hp_delay = 210;
    unsigned cap0 = captured();
    frames(200, 1);
    nspire_audio_stats(&s);
    T_EQ(s.syncs, syncs0 + 1);
    {
      /* the cut faded: the sound before the re-sync's silence steps down to
       * a third and less, not cut from the full tone */
      const uint8_t *p;
      unsigned n = fos_cap_bytes(&p) / 4, i, run = 0, at = 0, from = cap0 / 4, k;
      int peak_early = 0, peak_last = 0;
      for (i = from; i < n; i++) {
        int16_t l = (int16_t)(p[4 * i] | p[4 * i + 1] << 8);
        if (l) { run = 0; continue; }
        if (++run == s.rate / 50) { at = i + 1 - run; break; }   /* 20 ms of silence: the cut */
      }
      T_TRUE(at > from + s.rate / 5);
      for (k = at - s.rate * 150 / 1000; at && k < at; k++) {
        int16_t l = (int16_t)(p[4 * k] | p[4 * k + 1] << 8);
        int a = l < 0 ? -l : l;
        if (k < at - s.rate * 70 / 1000) { if (a > peak_early) peak_early = a; }
        else if (k >= at - s.rate * 12 / 1000) { if (a > peak_last) peak_last = a; }
      }
      printf("a re-sync's cut: the tone's peak %d before the fade, %d in the 12 ms before the silence\n",
             peak_early, peak_last);
      T_TRUE(peak_early > 1000);
      T_TRUE(peak_last * 100 <= peak_early * 40);
    }
    T_TRUE(s.sync_ms >= 210 - SINK_PIPE_MS - 1 && s.sync_ms <= 210 - SINK_PIPE_MS + 1);
    start = captured(); frames(30, 1); T_TRUE(any_nonzero(start));
    hp_delay = 273;
    frames(130, 1);
    nspire_audio_stats(&s);
    T_EQ(s.syncs, syncs0 + 2);
  }
  /* bt10: a gap in the game's sound (an OS call of a save: the card's
   * queue served, nothing made) leaves the card ahead of the headphones: re-
   * synced (a moment of silence), not left ahead for ~50 s a 100 ms */
  {
    unsigned k, gaps0;
    unsigned before, after;
    frames(90, 1);                             /* (settled since the re-sync above) */
    before = ring_avg(20);
    nspire_audio_stats(&s);
    gaps0 = s.sync_gaps;
    T_EQ(gaps0, 0);                            /* (none in the wobble, the re-syncs and their priming) */
    for (k = 0; k < 12; k++)
      __wrap_nspire_os_sleep();                /* 120 ms of the card's sound played, none made */
    frames(30, 1);
    nspire_audio_stats(&s);
    T_EQ(s.sync_gaps, gaps0 + 1);
    after = ring_avg(20);
    nspire_audio_stats(&s);
    T_TRUE(s.buffered > alone * 2); T_EQ(s.priming, 0);
    printf("a gap of 120 ms with the headphones: the card's ring %u ms before, %u ms 0.5-0.8 s after\n",
           before * 1000 / s.rate, after * 1000 / s.rate);
    T_TRUE(after + s.rate * 30 / 1000 > before && after < before + s.rate * 30 / 1000);   /* (where it was: in step again) */
    start = captured(); frames(20, 1); T_TRUE(any_nonzero(start));
  }
  hp_on = 0; nspire_bt_live = 0; hp_delay = 273;
  clean();
}

/* bt10: Output Bluetooth: the card idle (the driver's ua_set_idle): nothing
 * queued but its keep-alive now and then, no IRQ window for it, no stall
 * taken; back to Automatic it plays again. Against it the card streaming
 * silence (a menu: as bt9's Output Bluetooth did): the measured saving. */
static void test_card_idle(void)
{
  reset(); connect(0);
  nspire_audio_output = NSPIRE_OUTPUT_AUTO;
  hp_on = 1; nspire_bt_live = 1;
  frames(60, 1);
  nspire_audio_pause();
  frames(30, 0);
  {
    struct ua_counters c0, c1;
    unsigned w0, sl0 = sleeps;
    ua_counters(&c0); w0 = irq_windows;
    frames(360, 0);                            /* six seconds of a menu */
    ua_counters(&c1);
    printf("card streaming silence, 6 s: %lu completions, %u IRQ windows, %u OS sleeps\n",
           (unsigned long)(c1.completions - c0.completions), irq_windows - w0, sleeps - sl0);
    T_TRUE(c1.completions - c0.completions > 4800 / UA_NF);   /* (600 of 8 ms, 4 in 5 of six seconds) */
  }
#ifdef UA_HAS_IDLE
  struct nspire_audio_stats s;
  unsigned start;
  nspire_audio_output = NSPIRE_OUTPUT_BT;
  frames(60, 1);
  nspire_audio_stats(&s);
  T_TRUE(s.card_idle); T_EQ(s.rendering, 0);
  {
    struct ua_counters c0, c1;
    unsigned w0, sl0 = sleeps;
    ua_counters(&c0); w0 = irq_windows;
    frames(360, 1);                            /* six seconds */
    ua_counters(&c1);
    printf("idle card, 6 s: %lu completions, %lu keep-alives, %u IRQ windows, %u OS sleeps\n",
           (unsigned long)(c1.completions - c0.completions), (unsigned long)(c1.keepalives - c0.keepalives),
           irq_windows - w0, sleeps - sl0);
    T_TRUE(c1.completions - c0.completions <= 12);
    T_TRUE(c1.keepalives - c0.keepalives >= 1);
    T_EQ(irq_windows - w0, 0);
    T_TRUE(sleeps - sl0 <= 6);
    nspire_audio_stats(&s); T_EQ(s.stalled, 0);
  }
  nspire_audio_output = NSPIRE_OUTPUT_AUTO;
  frames(30, 1);                               /* (primed to the headphones' delay: 13 frames) */
  nspire_audio_stats(&s);
  T_EQ(s.card_idle, 0); T_TRUE(s.rendering); T_EQ(s.priming, 0); T_EQ(s.stalled, 0);
  start = captured(); frames(20, 1); T_TRUE(any_nonzero(start));
#endif
  hp_on = 0; nspire_bt_live = 0;
  clean();
}

static void test_mute_mono_and_full_buffer(void)
{
  struct nspire_audio_stats s;
  reset(); connect(0);
  nspire_audio_mono = 1;
  frames(12, 1);
  const uint8_t *p;
  unsigned n = fos_cap_bytes(&p);
  T_TRUE(any_nonzero(0));
  for (unsigned i = 0; i + 3 < n; i += 4) T_MEM(p+i, p+i+2, 2);
  nspire_audio_volume = 0; frames(8, 1);
  unsigned start = captured(); frames(8, 1); T_EQ(any_nonzero(start), 0);
  nspire_audio_volume = 10; frames(12, 1);
  /* Producer outruns USB time: bounded dropping, never a synchronous wait. */
  unsigned before = sleeps;
  for (unsigned i = 0; i < 100; i++) {
    nspire_audio_frame_begin(1);
    cpu_ticks += 280896; render_gbc_sound(); nspire_audio_frame_end();
  }
  nspire_audio_stats(&s);
  T_TRUE(s.dropped); T_EQ(sleeps, before); T_TRUE(s.buffered <= RING_FRAMES);
  /* Fast forward mode (2) keeps playing sped-up nonzero PCM rather than pausing. */
  frames(4, 0);
  start = captured();
  for (unsigned i = 0; i < 24; i++) {
    unit_ticks += 274;
    nspire_audio_frame_begin(2);
    cpu_ticks += 280896; render_gbc_sound(); sound_flush_all_pops();
    nspire_audio_frame_end();
  }
  nspire_audio_stats(&s);
  T_TRUE(s.rendering);
  T_TRUE(s.speed > 65536u * 3u / 2u);
  T_TRUE(any_nonzero(start));
  frames(12, 0); start = captured(); frames(12, 0); T_EQ(any_nonzero(start), 0);
  clean();
}

static void test_audio_only_discovery(void)
{
  reset();
  unit_usb_plugged = 1;
  struct fos_spec spec = {0x0d8c, 0x000c, cfg_cm108, sizeof cfg_cm108, 0};
  T_EQ(fos_plug(&spec), 0);
  T_EQ(nspire_usbaudio_owns_usb(), 0);
  nspire_audio_poll();
  T_TRUE(nspire_os_wanted & OS_WANT_AUDIO);
  T_EQ(nspire_usbaudio_owns_usb(), 0); /* discovery never claims the port */
  for (unsigned i = 0; i < 20; i++) { /* menu: no pad/drive/frame service */
    unit_ticks += 100;
    nspire_audio_poll();
  }
  T_TRUE(sleeps); T_EQ(ua_state(), UA_STREAMING);
  T_TRUE(nspire_usbaudio_active());
  clean();
}

static void test_native_44k(void)
{
  struct nspire_audio_stats s;
  reset();
  struct fos_spec spec = {0x1234, 0x5678, cfg_44k, sizeof cfg_44k, 0};
  T_EQ(fos_plug(&spec), 0);
  __wrap_nspire_os_sleep(); nspire_audio_poll();
  frames(60, 1); nspire_audio_stats(&s);
  T_EQ(s.rate, 44100); T_TRUE(s.completions > 20); T_TRUE(any_nonzero(0));
  clean();

  /* A 48 kHz-only adapter falls back to 48000 Hz when no single-split rate exists. */
  uint8_t cfg_48k[sizeof cfg_44k];
  memcpy(cfg_48k, cfg_44k, sizeof cfg_44k);
  cfg_48k[81] = 0x80; cfg_48k[82] = 0xBB; cfg_48k[83] = 0x00;
  cfg_48k[88] = 0xC8; cfg_48k[89] = 0x00;
  reset();
  fos_set_hw(FOS_HW_EHCI);
  struct fos_spec spec48 = {0x1234, 0x5679, cfg_48k, sizeof cfg_48k, 0};
  T_EQ(fos_plug(&spec48), 0);
  __wrap_nspire_os_sleep(); nspire_audio_poll();
  frames(60, 1); nspire_audio_stats(&s);
  T_EQ(s.rate, 48000); T_TRUE(s.completions > 20); T_TRUE(any_nonzero(0));
  clean();
}

static void test_detach_and_retire(void)
{
  reset(); connect(0); frames(10, 1);
  fos_unplug(); __wrap_nspire_os_sleep(); nspire_audio_poll();
  T_EQ(nspire_usbaudio_active(), 0); T_EQ(nspire_usbaudio_owns_usb(), 0);
  frames(10, 1);
  connect(0); frames(12, 1); T_TRUE(nspire_usbaudio_active());
  clean();
  for (unsigned mode = 0; mode < 3; mode++) {
    reset(); connect(FOS_NEVER_DONE | (mode == 0 ? FOS_ABORT_DELAYED : mode == 1 ? FOS_ABORT_ERROR : FOS_CLOSE_ERROR));
    frames(10, 1);
    unsigned shown = draining;
    clean(); T_TRUE(draining > shown);
  }
  reset();
  /* A missing ABI function must never claim the USB port. */
  clean(); fos_init(); fos_remove_syscall(236); nspire_audio_start();
  T_EQ(nspire_usbaudio_owns_usb(), 0);
  clean();
}

/* 2026-11-15 round 2 (on hardware, with the adapter kept in, a sound card
 * pulled and put back was not picked up): the driver's own detach and
 * re-attach, twice: back to waiting, not active, the port's claim given
 * up; the card again streams, its sound on the wire. */
static void test_card_out_and_in_twice(void)
{
  struct nspire_audio_stats s;
  reset(); connect(0); frames(30, 1);
  nspire_audio_stats(&s);
  T_TRUE(s.rendering); T_EQ(ua_state(), UA_STREAMING);
  for (unsigned round = 0; round < 2; round++) {
    fos_unplug(); __wrap_nspire_os_sleep(); nspire_audio_poll();
    T_TRUE(ua_state() == UA_GONE || ua_state() == UA_WAITING);
    T_EQ(nspire_usbaudio_active(), 0); T_EQ(nspire_usbaudio_owns_usb(), 0);
    frames(20, 1);
    nspire_audio_stats(&s); T_EQ(s.rendering, 0);
    connect(0); frames(30, 1);
    T_EQ(ua_state(), UA_STREAMING); T_TRUE(nspire_usbaudio_active());
    nspire_audio_stats(&s); T_TRUE(s.rendering); T_EQ(s.priming, 0); T_EQ(s.stalled, 0);
    unsigned start = captured();
    frames(20, 1);
    T_TRUE(any_nonzero(start));
    T_EQ(fos_stats()->violations, 0);
  }
  clean();
}

/* The DualShock 4 and the DualSense are also USB
 * sound cards (interfaces 0-2), and the OS offers those before the gamepad
 * (interface 3). gpSP's audio (nspire_audio_os.c) leaves a device the pad
 * driver knows alone: nothing attached, audio not active, the declines
 * counted for the pad log. The pad driver's own table is checked in its
 * tests (backend_os_test test_review_known_kind); here a stand-in with the
 * same Sony entries (gpSP links the real one). */
int usbpad_known_kind(uint16_t vid, uint16_t pid)
{
  static const uint16_t sony[] = { 0x05c4, 0x09cc, 0x0ba0, 0x0ce6, 0x0df2 };
  for (unsigned i = 0; vid == 0x054c && i < sizeof(sony) / sizeof(sony[0]); i++)
    if (pid == sony[i]) return pid == 0x0ce6 || pid == 0x0df2 ? 5 : 4;
  return vid == 0x057e && pid == 0x2009 ? 6 : 0;
}
unsigned nspire_audio_declined(uint32_t *ids, unsigned *ifno);

#ifdef SINK_HAVE_SONY
#include "corpus/cfg_sony_real.h"

static void plug_pad(uint16_t vid, uint16_t pid, const uint8_t *cfg, unsigned len)
{
  struct fos_spec s = {vid, pid, cfg, len, 0};
  T_EQ(fos_plug(&s), 0);
  for (unsigned i = 0; i < 5; i++) {
    __wrap_nspire_os_sleep();
    nspire_audio_poll();
  }
}

static void test_sony_pads_left_alone(void)
{
  uint32_t ids = 0;
  unsigned ifno = 99, before;

  reset();
  before = nspire_audio_declined(NULL, NULL);
  plug_pad(CORPUS_DS4V2_REAL_VID, CORPUS_DS4V2_REAL_PID, corpus_ds4v2_cfg, sizeof corpus_ds4v2_cfg);
  T_TRUE(fos_stats()->probes_iface >= 2);         /* its audio interfaces were offered */
  T_EQ(fos_stats()->attach_ok, 0);
  T_EQ(fos_stats()->attach_fail, 0);
  T_EQ(fos_stats()->ctl_requests, 0);             /* nothing sent to the pad */
  T_EQ(nspire_usbaudio_active(), 0);
  T_EQ(nspire_usbaudio_owns_usb(), 0);
  T_EQ(ua_state(), UA_WAITING);
  T_TRUE(nspire_audio_declined(&ids, &ifno) > before);
  T_EQ(ids, 0x054c09ccu);
  T_TRUE(ifno == 1 || ifno == 2);
  fos_unplug(); __wrap_nspire_os_sleep(); nspire_audio_poll();
  clean();

  reset();
  before = nspire_audio_declined(NULL, NULL);
  plug_pad(CORPUS_DUALSENSE_REAL_VID, CORPUS_DUALSENSE_REAL_PID, corpus_dualsense_cfg,
           sizeof corpus_dualsense_cfg);
  T_EQ(fos_stats()->attach_ok, 0);
  T_EQ(nspire_usbaudio_active(), 0);
  T_TRUE(nspire_audio_declined(&ids, NULL) > before);
  T_EQ(ids, 0x054c0ce6u);
  frames(10, 1);                                  /* the game's sound goes nowhere */
  T_EQ(captured(), 0);
  fos_unplug(); __wrap_nspire_os_sleep(); nspire_audio_poll();
  clean();

  /* A sound card still plays after a pad was left alone. */
  reset();
  plug_pad(CORPUS_DS4V2_REAL_VID, CORPUS_DS4V2_REAL_PID, corpus_ds4v2_cfg, sizeof corpus_ds4v2_cfg);
  fos_unplug(); __wrap_nspire_os_sleep(); nspire_audio_poll();
  connect(0); frames(60, 1);
  T_TRUE(nspire_usbaudio_active()); T_TRUE(any_nonzero(0));
  clean();

  /* The same descriptors under ids the pad driver does not know: the
   * audio driver takes them as it always did (what a Sony pad met before). */
  reset();
  before = nspire_audio_declined(NULL, NULL);
  plug_pad(0x1234, 0x09cc, corpus_ds4v2_cfg, sizeof corpus_ds4v2_cfg);
  T_TRUE(fos_stats()->attach_ok + fos_stats()->attach_fail >= 1);
  T_TRUE(nspire_usbaudio_active());
  T_EQ(nspire_audio_declined(NULL, NULL), before);
  T_EQ(ua_state(), UA_STREAMING);                 /* DS4 v2: 2 ch 32 kHz, the audio driver plays it */
  fos_unplug(); __wrap_nspire_os_sleep(); nspire_audio_poll();
  clean();
}
#endif

int main(void)
{
  setvbuf(stdout, NULL, _IONBF, 0);
  corpus_fix();
#ifdef SINK_HAVE_SONY
  T_RUN(test_sony_pads_left_alone);
#endif
  T_RUN(test_no_device);
  T_RUN(test_play_pause_resume);
  T_RUN(test_pause_fades);
  T_RUN(test_output_setting);
  T_RUN(test_both_outputs);
  T_RUN(test_sync_resync);
  T_RUN(test_card_idle);
  T_RUN(test_mute_mono_and_full_buffer);
  T_RUN(test_audio_only_discovery);
  T_RUN(test_native_44k);
  T_RUN(test_detach_and_retire);
  T_RUN(test_card_out_and_in_twice);
  fos_fini();
  return T_DONE();
}
