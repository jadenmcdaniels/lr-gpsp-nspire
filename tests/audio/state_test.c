/* Compile with and without NSPIRE_NO_AUDIO. Every serialized channel byte
 * and SOUNDCNT_X must agree as time, routing, volume and registers change.
 * PCM is deliberately not drained: the host read cursor is not emulated
 * hardware and is tested separately by tone_test. */
#include "common.h"
#include <stdio.h>

cpu_alert_type function_cc write_io_register16(u32 address, u32 value);
static void w(u32 address, u32 value) { write_io_register16(address, value); }

int main(void)
{
  static u8 state[4096];
  unsigned frame, i;
  init_sound();
  for (i = 0; i < 4; i++) dma[i].direct_sound_channel = DMA_NO_DIRECT_SOUND;
  w(0x84, 0x80);
  w(0x82, 2);
  for (frame = 0; frame < 240; frame++)
  {
    u32 hash = 2166136261u;
    u32 size;
    if (!(frame % 12))
    {
      w(0x60, ((frame % 7 + 1) << 4) | (frame & 8) | (frame % 6 + 1));
      w(0x62, ((frame % 15 + 1) << 12) | 0x0380 | (frame & 63));
      w(0x64, 0xC000 | (frame * 53 % 2048));
      w(0x68, ((15 - frame % 15) << 12) | 0x0A40 | (frame & 63));
      w(0x6C, 0xC000 | (frame * 37 % 2048));
      w(0x70, 0x80 | (frame & 0x60));
      w(0x72, ((frame % 4) << 13) | (frame % 256));
      w(0x74, 0xC000 | (frame * 29 % 2048));
      w(0x78, 0xC940 | (frame & 63));
      w(0x7C, 0xC000 | (frame & 15) | ((frame % 10) << 4));
      for (i = 0; i < 8; i++) w(0x90 + i * 2, frame * 137 + i * 571);
    }
    /* Fully routed, completely unrouted, or zero master volume. */
    w(0x80, frame % 3 == 0 ? 0xFF77 : frame % 3 == 1 ? 0x77 : 0xFF00);
    for (i = 0; i < 8; i++) sound_timer_queue32(i & 1, frame * 1234567 + i);
    sound_timer(frame & 1 ? 0x555555 : 0xFFFFFFFFu, 0);
    sound_timer(frame & 2 ? 0xFFFFFF : 0x1000000, 1);
    cpu_ticks += 280896;
    render_gbc_sound();
    size = sound_write_savestate(state);
    for (i = 0; i < size; i++) hash = (hash ^ state[i]) * 16777619u;
    printf("state %u %08x %u %04x\n", frame, hash, size, read_ioreg(REG_SOUNDCNT_X));
  }
  return 0;
}
