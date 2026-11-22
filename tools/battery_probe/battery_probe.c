/* battery_probe: a small Ndless program (not gpSP) that logs what the
 * calculator's ADC says, to find which reading follows the OS's own battery
 * percentage (docs/ui/spec.txt, section 9, "What must be measured").
 *
 * What it does, every 2 s until esc is pressed, one line in
 * battery_probe.txt.tns next to the program:
 *   CX / CX CAS (not a CX II): the ten-bit result register of each of the
 *     seven ADC channels at 0xC4000100 + n * 0x20 + 0x10 (Firebird's map).
 *     Read only. The build with -DPROBE_WRITE also writes 1 to each
 *     channel's command register (+0x00) first, and waits 2 ms, which is how
 *     Firebird says a measurement is started.
 *   CX II: the 64 words at 0x900B0000-0x900B00FF, read only. Writes happen
 *     only at the word offsets listed in -DPROBE_CX2_WRITE="0x20,0x40" (none
 *     by default: nothing is known about that ADC's registers, so nobody
 *     should poke it before the first dump has been looked at).
 * enter puts a "mark" line in the log with the time: press it each time the
 * OS's own percentage (the status screen: ctrl+doc's Settings > Status, or
 * the home screen's battery icon) has been written down, so the notes and
 * the log can be put side by side.
 *
 * To run (by hand, on a calculator; nothing here is run by the build):
 *   1. the OS's percentage written down, the program started, enter pressed
 *   2. at about 90%, 50% and 15% (three runs, or one long one), on battery
 *   3. once more at each level with the USB cable to a computer plugged in
 * Send battery_probe.txt.tns back with the notes.
 *
 * Build: tools/battery_probe/build.sh (needs the Ndless SDK on PATH).
 * Reading an address the OS didn't map aborts the program (the OS may then
 * reset): that is a result too, say which model and which line was last in
 * the file (every line is flushed to the file). */

#include <os.h>
#include <stdio.h>
#include <stdint.h>

#ifndef PROBE_CX2_WRITE
#define PROBE_CX2_WRITE
#endif

#define VREG(a) (*(volatile uint32_t *)(a))

static unsigned seconds(void)
{
  /* the real-time clock's counter (0x90090000): whole seconds, on both models */
  return VREG(0x90090000);
}

static void sample_cx(FILE *f)
{
  int n;
  unsigned v[7];
  for (n = 0; n < 7; n++)
  {
    uint32_t base = 0xC4000100u + (uint32_t)n * 0x20u;
#ifdef PROBE_WRITE
    VREG(base) = 1;
    msleep(2);
#endif
    v[n] = VREG(base + 0x10) & 0x3FF;
  }
  fprintf(f, "%u cx adc %u %u %u %u %u %u %u\n", seconds(), v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
}

static void sample_cx2(FILE *f)
{
  static const unsigned writes[] = { PROBE_CX2_WRITE 0xFFFFFFFFu };
  unsigned w, i;
  for (i = 0; writes[i] != 0xFFFFFFFFu; i++)
  {
    fprintf(f, "%u cx2 write 1 to +%02X (was %08X)\n", seconds(), writes[i], (unsigned)VREG(0x900B0000u + writes[i]));
    VREG(0x900B0000u + writes[i]) = 1;
    msleep(2);
  }
  for (w = 0; w < 64; w += 8)
    fprintf(f, "%u cx2 +%02X %08X %08X %08X %08X %08X %08X %08X %08X\n", seconds(), w * 4,
            (unsigned)VREG(0x900B0000u + w * 4), (unsigned)VREG(0x900B0004u + w * 4),
            (unsigned)VREG(0x900B0008u + w * 4), (unsigned)VREG(0x900B000Cu + w * 4),
            (unsigned)VREG(0x900B0010u + w * 4), (unsigned)VREG(0x900B0014u + w * 4),
            (unsigned)VREG(0x900B0018u + w * 4), (unsigned)VREG(0x900B001Cu + w * 4));
}

int main(void)
{
  FILE *f = fopen("battery_probe.txt.tns", "a");
  unsigned n = 0;

  if (!f)
  {
    show_msgbox("Battery probe", "Could not open battery_probe.txt.tns for writing.");
    return 1;
  }
  fprintf(f, "# battery_probe, %s%s, %s\n", is_cx2 ? "CX II" : "CX or CX CAS",
#ifdef PROBE_WRITE
          " (writes the channels' command registers)",
#else
          " (read only)",
#endif
          is_classic ? "classic (not supported)" : "colour");
  fflush(f);
  if (is_classic)
  {
    fclose(f);
    show_msgbox("Battery probe", "A classic (non-CX) calculator: nothing to measure here.");
    return 1;
  }
  show_msgbox("Battery probe", "Logging every 2 s to battery_probe.txt.tns. enter: mark the time (after writing "
                               "down the OS's percentage). esc: stop.");
  while (!isKeyPressed(KEY_NSPIRE_ESC))
  {
    if (isKeyPressed(KEY_NSPIRE_ENTER))
    {
      fprintf(f, "%u mark\n", seconds());
      while (isKeyPressed(KEY_NSPIRE_ENTER))
        msleep(20);
    }
    if (is_cx2)
      sample_cx2(f);
    else
      sample_cx(f);
    fflush(f);
    n++;
    msleep(2000);
  }
  fprintf(f, "# stopped after %u samples\n", n);
  fclose(f);
  show_msgbox("Battery probe", "Stopped. battery_probe.txt.tns has the log.");
  return 0;
}
