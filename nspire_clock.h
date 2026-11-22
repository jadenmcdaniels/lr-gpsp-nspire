/* The CPU speed on the CX II and the older CX (nspire_clock.c): the "CPU speed" setting raises the
 * clock while a game runs; menus, the file browser and saving run at the
 * calculator's own speed. */
#ifndef NSPIRE_CLOCK_H
#define NSPIRE_CLOCK_H

/* nspire_cpu_speed: 0 normal, then 408 to 504 MHz in 12 MHz steps (the
 * multiplier of the 12 MHz clock, 34 to 42), as in PocketSNES; last, auto
 * (the default): normal on a CX II. */
#define CPU_SPEEDS 11
#define CPU_SPEED_AUTO 10
/* On an older CX: normal (132 MHz), then 168 to 264 MHz in 12 MHz steps
 * (the multiplier of 6 MHz, 28 to 44); auto is 192 MHz (bus 64): at 132 the
 * CX can't hold 30 pictures a second in Emerald's towns. The names are
 * gui.c's. */

/* At startup: if a raised speed froze the calculator last time (its marker
 * file is still there), the speed goes back to normal and the next game
 * says so. 1 if it found the marker: the caller saves the settings then
 * (R3: the marker is gone, so a reset before the next save would raise
 * the speed again). */
int nspire_clock_check_crash(void);
/* The game that froze, for its own settings (""; gui.c). */
extern char nspire_clock_crashed_rom[512];
/* The game's speed (the setting), or back to the calculator's own. */
void nspire_clock_game(void);
void nspire_clock_normal(void);
/* Leaving gpSP cleanly: normal speed and no marker. */
void nspire_clock_left_game(void);
/* An older CX: the speed (raw ticks a second) of the timer whose count is
 * at value_reg, timed against its CPU clock as its clock register gives it,
 * in 0.2 s; 0 on a CX II or if it can't be timed. any_rate: any speed is
 * taken (another timer than the first, whose clock isn't known), not just
 * one near a power of two of 32768. */
u32 nspire_cx_timer_hz(u32 value_reg, int any_rate);
/* R12: a CX II's: the speed (counts a second) of the down-counter at
 * value_reg (reloading from 'top' past 0: a periodic timer's load, else
 * 0xFFFFFFFF), timed against the CPU clock its clock register gives (12 MHz
 * times the multiplier) over a 20 ms loop with the interrupts off; 0 if it
 * didn't move or can't be timed. */
u32 nspire_cx2_timer_hz(u32 value_reg, u32 top);
/* An older CX's peripheral bus (APB) clock in Hz by its clock register (half
 * the memory bus's), 0 if it can't be told. */
u32 nspire_cx_apb_hz(void);
/* The CPU clock in MHz, measured (0 if it can't be). */
u32 nspire_cpu_mhz(void);
/* The clock register and the last switch, for the speed test's file. */
void nspire_clock_report(char *text, u32 size);

#endif
