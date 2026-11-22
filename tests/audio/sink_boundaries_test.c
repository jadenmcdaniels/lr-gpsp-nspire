/* Reuse the real mixer/USB-task fixture, observing the first task yield
 * after a discontinuity. Waiting several frames for silence would miss
 * an extra transfer filled from old PCM before the discard takes effect. */
#define __wrap_nspire_os_sleep fixture_sleep
#define main sink_fixture_main
#include "sink_test.c"
#undef main
#undef __wrap_nspire_os_sleep

static int observe_next_sleep, expect_rendering, device_paused;
/* r19-snd: a pause (the menu) plays out 2 ms of the ring faded and then silence (nspire_audio.c
 * soft_stop): the device is paused only once those went out; nothing of the old PCM but them */
static int expect_soft;
static unsigned observations;

void __real_ua_set_pause(int paused);
void __wrap_ua_set_pause(int paused)
{
  device_paused = !!paused;
  __real_ua_set_pause(paused);
}

int __wrap_nspire_os_sleep(void)
{
  if (observe_next_sleep) {
    struct nspire_audio_stats s;
    nspire_audio_stats(&s);
    T_EQ(s.rendering, expect_rendering);
    T_EQ(device_paused, !expect_rendering && !expect_soft);
    if (expect_soft)
      T_TRUE(s.buffered <= 96 + 44100 / 4);   /* (at most FADE_FRAMES faded and 250 ms of zeros) */
    observe_next_sleep = 0;
    observations++;
  }
  return fixture_sleep();
}

static void save_sound(u8 *out)
{
  u8 *p = out + 4, *header = out;
  p += sound_write_savestate(p);
  *p++ = 0;
  bson_write_u32(header, p - out);
}

static void test_discard_before_yield(void)
{
  enum { NORMAL, RESET, LOAD, MUTE, DISABLE, VOLUME, FAST_FORWARD, MENU };
  for (unsigned scenario = NORMAL; scenario <= MENU; scenario++) {
    struct nspire_audio_stats s;
    u8 state[4096];
    u32 saved_ticks;
    reset(); connect(0);
    save_sound(state);
    saved_ticks = cpu_ticks;
    frames(30, 1);
    nspire_audio_stats(&s);
    T_TRUE(s.rendering);
    T_EQ(s.priming, 0);
    T_TRUE(s.buffered > 0);
    T_TRUE(any_nonzero(0));
    switch (scenario) {
    case RESET: reset_sound(); break;
    case LOAD:
      T_TRUE(sound_read_savestate(state));
      cpu_ticks = saved_ticks;
      break;
    case MUTE: nspire_audio_volume = 0; break;
    case DISABLE: nspire_audio_enabled = 0; break;
    case VOLUME: nspire_audio_volume = 5; break;
    case MENU: nspire_audio_pause(); break;
    }
    /* Force the very next poll to yield while old frames are still
     * buffered. NORMAL is a control: polling must preserve live audio. */
    unit_ticks += NSPIRE_TICK_HZ / 20;
    expect_rendering = scenario == NORMAL;
    expect_soft = scenario == MENU;
    observe_next_sleep = 1;
    unsigned before = observations;
    if (scenario == FAST_FORWARD)
      nspire_audio_frame_begin(0);
    else
      nspire_audio_poll();
    T_EQ(observe_next_sleep, 0);
    T_EQ(observations, before + 1);
    clean();
  }
}

int main(void)
{
  setvbuf(stdout, NULL, _IONBF, 0);
  corpus_fix();
  T_RUN(test_discard_before_yield);
  fos_fini();
  return T_DONE();
}
