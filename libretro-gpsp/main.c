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
#include <ctype.h>

timer_type timer[4];

u32 frame_counter = 0;
u32 cpu_ticks = 0;
u32 execute_cycles = 0;
s32 video_count = 0;

u32 last_frame = 0;
u32 flush_ram_count = 0;
u32 gbc_update_count = 0;
u32 oam_update_count = 0;

char main_path[512];

static u32 random_state = 0;

// Generate 16 random bits.
u16 rand_gen() {
  random_state = ((random_state * 1103515245) + 12345) & 0x7fffffff;
  return random_state;
}

// Add some random state to the initial seed.
void rand_seed(u32 data) {
  random_state ^= rand_gen() ^ data;
}


static unsigned update_timers(irq_type *irq_raised, unsigned completed_cycles)
{
   unsigned i, ret = 0;
   /* Timers 1-3 are usually all off: timer 0 only then. A status changes
      here only in an overflow (a sound DMA could write a timer's control),
      and the limit is read again after each, so the timers that run, their
      order and cascade are exactly as with i < 4. */
   unsigned limit = (timer[1].status | timer[2].status | timer[3].status) ? 4 : 1;
   for (i = 0; i < limit; i++)
   {
      if(timer[i].status == TIMER_INACTIVE)
         continue;

      if(timer[i].status != TIMER_CASCADE)
      {
         timer[i].count -= completed_cycles;
         /* io_registers accessors range: REG_TM0D, REG_TM1D, REG_TM2D, REG_TM3D */
         write_ioreg(REG_TMXD(i), -(timer[i].count >> timer[i].prescale));
      }

      if(timer[i].count > 0)
         continue;

      /* irq_raised value range: IRQ_TIMER0, IRQ_TIMER1, IRQ_TIMER2, IRQ_TIMER3 */
      if(timer[i].irq)
         *irq_raised |= (IRQ_TIMER0 << i);

      if((i != 3) && (timer[i + 1].status == TIMER_CASCADE))
      {
         timer[i + 1].count--;
         write_ioreg(REG_TMXD(i + 1), -timer[i+1].count);
      }

      if(i < 2)
      {
         if(timer[i].direct_sound_channels & 0x01)
            ret += sound_timer(timer[i].frequency_step, 0);

         if(timer[i].direct_sound_channels & 0x02)
            ret += sound_timer(timer[i].frequency_step, 1);
      }

      timer[i].count += (timer[i].reload << timer[i].prescale);
      limit = (timer[1].status | timer[2].status | timer[3].status) ? 4 : 1;
   }
   return ret;
}

#if defined(NSPIRE_BUILD) && defined(HAVE_DYNAREC)
static void idle_pass_reset(void);
#endif

void init_main(void)
{
  u32 i;
  for(i = 0; i < 4; i++)
  {
    timer[i].status = TIMER_INACTIVE;
    timer[i].prescale = 0;
    timer[i].irq = 0;
    timer[i].reload = timer[i].count = 0x10000;
    timer[i].direct_sound_channels = TIMER_DS_CHANNEL_NONE;
    timer[i].frequency_step = 0;
  }

  timer[0].direct_sound_channels = TIMER_DS_CHANNEL_BOTH;
  timer[1].direct_sound_channels = TIMER_DS_CHANNEL_NONE;

  frame_counter = 0;
  cpu_ticks = 0;
  execute_cycles = 960;
  video_count = 960;

#ifdef HAVE_DYNAREC
  init_dynarec_caches();
  init_emitter(gamepak_must_swap());
#endif
#if defined(NSPIRE_BUILD) && defined(HAVE_DYNAREC)
  idle_pass_reset();
#endif
}

// Transition from vrefresh to vblank, once a frame: out of line (and not
// in the hot section) so update_gba stays small.
static __attribute__((noinline)) u32 enter_vblank(u32 dispstat,
  irq_type *irq_raised, int *dma_cycles)
{
  u32 i;
  dispstat |= 0x01;

#ifdef NSPIRE_BUILD
  // The lines still waiting to be drawn (video.h)
  nspire_flush_lines();
#endif

  // Reinit affine transformation counters for the next frame
  video_reload_counters();

  // Trigger VBlank interrupt if enabled
  if (dispstat & 0x8)
    *irq_raised |= IRQ_VBLANK;

  // Trigger the VBlank DMAs if enabled
  for (i = 0; i < 4; i++)
  {
    if(dma[i].start_type == DMA_START_VBLANK)
      dma_transfer(i, dma_cycles);
  }
  return dispstat;
}

#if defined(NSPIRE_BUILD) && defined(HAVE_DYNAREC)
/* speed9b (2026-11-18): Advance Wars' idle loop, its next events without
 * its pass while the pass would change nothing.
 *
 *   0803880a ldr r0,[r4]  cmp r0,#0  beq 8038814  bl 807b44c (bx r0)
 *   08038814 bl 8038824   08038818 b 803880a   (gba_over.h's idle address)
 *
 * r4 = 0x03004478 holds a callback; Advance Wars' (0x080383f8) returns at
 * once while the byte at 0x03004434 is set: push {r4,lr}, ldrb, cmp, bne,
 * pop {r4}, pop {r0}, bx r0. 0x08038824 reads the keys (soft reset when A,
 * B, Select and Start are down): push {lr}, ldrh KEYINPUT, ..., cmp r0,#15,
 * bne, pop {r0}, bx r0. core-16's static test can't take it (calls, stack
 * stores), so the branch calls arm_update_gba_idle_pass_thumb (arm_stub.S)
 * when its code is this (nspire_idle_pass_code, at translation), and
 * update_gba runs the next event as the loop's next call would when the
 * state is the pass's fixed point (arm_stub.S nspire_idle_pass_fixed): with
 * whatever registers it starts but r4, that pass leaves r0 = lr =
 * 0x08038819, r1 = 15, r2 = 0x3FF, N set and Z C V clear (cmp of a value
 * below 15 with 15), the words 0x03004478 and 0x08038819 at sp - 8 and
 * sp - 4, and nothing else. Its loads: the callback and its flag (IWRAM),
 * KEYINPUT, the ROM's literals and its own pushes; an event without
 * interrupt or DMA writes none of them (only DISPSTAT, VCOUNT, IF, the
 * timers' counts, serial registers), so after it the pass would again leave
 * the state as it is. And it would run whole inside the next window: its
 * cost C (nspire_update_gba_idle_pass) is at most the window, so no block
 * exit of it could have reached an event first; then it ends at the branch,
 * whose idle call runs the event with no remaining cycles, as the repeat
 * does.
 * Its state is in reg[] (cpu.h REG_IDLE_*), its table in .text: no new
 * variable in LTO's data, whose order would move the game's data. */
static const struct
{
  u32 at;
  u32 count;
  u16 code[18];
} idle_pass_code[] __attribute__((section(".text.idle_pass_code"), aligned(4))) = {
  { 0x0803880A, 8, { 0x6820, 0x2800, 0xD001, 0xF042, 0xFE1C, 0xF000, 0xF806,
                     0xE7F7 } },
  { 0x080383F8, 5, { 0xB510, 0x4C0C, 0x7820, 0x2800, 0xD111 } },
  { 0x08038426, 5, { 0xBC10, 0xBC01, 0x4700, 0x4434, 0x0300 } },
  { 0x08038824, 18, { 0xB500, 0x4806, 0x8801, 0x4A06, 0x1C10, 0x4388, 0x210F,
                      0x4008, 0x280F, 0xD101, 0xF7FF, 0xFF4E, 0xBC01, 0x4700,
                      0x0130, 0x0400, 0x03FF, 0x0000 } },
  { 0x0807B44C, 1, { 0x4700 } },
};

/* The idle branch to new_pc (arm_emit.h, translating) is that loop's. */
bool nspire_idle_pass_code(u32 new_pc)
{
  u32 i, j;
  if (new_pc != 0x0803880A)
    return false;
  for (i = 0; i < sizeof(idle_pass_code) / sizeof(idle_pass_code[0]); i++)
    for (j = 0; j < idle_pass_code[i].count; j++)
    {
      u32 at = idle_pass_code[i].at + j * 2;
      const u8 *page = memory_map_read[at >> 15];
      if (!page || readaddress16(page, at & 0x7FFF) != idle_pass_code[i].code[j])
        return false;
    }
  return true;
}

#define idle_pass_cost       reg[REG_IDLE_PASS_COST]    /* C */
#define idle_pass_window     reg[REG_IDLE_PASS_WINDOW]  /* what the last call returned */
#define idle_pass_ticks      reg[REG_IDLE_PASS_TICKS]   /* cpu_ticks then */
#define idle_pass_from_fixed reg[REG_IDLE_PASS_FIXED]   /* the pass after it starts at the fixed point */
#define idle_pass_seen       reg[REG_IDLE_PASS_SEEN]    /* C measured: 0, 1, 2 twice the same (used), 3 off */

/* The state is the fixed point of the pass: arm_stub.S (its stub calls it
   before update_gba; in the hot stretch). */
int nspire_idle_pass_fixed(void);

static void idle_pass_reset(void)
{
  idle_pass_cost = 0;
  idle_pass_seen = 0;
  idle_pass_from_fixed = 0;
  reg[REG_IDLE_LIMIT] = 0;
}

#ifdef NSPIRE_IDLE_CHECK
/* tests/arm IDLE_CHECK=1: update_gba runs no event without the pass;
   where it would have (idle_pass_would), the pass runs and the next call
   must find the same registers and stack words, no update_gba call between
   and the pass's cost C. */
u32 nspire_idle_pass_check_ok, nspire_idle_pass_check_bad, nspire_idle_pass_check_cost_bad;
u32 idle_pass_would;
static u32 idle_pass_check_pending, idle_pass_check_regs[17];
static u32 idle_pass_check_stack[2], idle_pass_check_ticks;
#endif
#endif

#ifdef NSPIRE_IDLE_CHECK
/* tests/arm IDLE_CHECK=1 (core-16): arm_update_gba_pure_thumb passes 0, so
 * update_gba never runs a pure loop's next event itself; where it would
 * have (the call was armed and untainted and its event took no interrupt,
 * didn't end the frame and ran no DMA: the result is execute_cycles, not
 * 0), the loop's body runs as before and its stub's next call must come
 * right after, armed and untainted, with the registers and CPSR the repeat
 * took for granted. */
u32 nspire_idle_check_ok, nspire_idle_check_bad;
static u32 idle_check_pending, idle_check_regs[17];
u32 function_cc update_gba_real(int remaining_cycles);

u32 function_cc update_gba(int remaining_cycles)
{
  u32 ret;
  if (idle_check_pending)
  {
    idle_check_pending = 0;
    if (reg[REG_IDLE_OK] && !memcmp(idle_check_regs, reg, sizeof(idle_check_regs)))
      nspire_idle_check_ok++;
    else
      nspire_idle_check_bad++;
  }
  ret = update_gba_real(remaining_cycles);
  if (reg[REG_IDLE_OK] && ret && ret == execute_cycles)
  {
    memcpy(idle_check_regs, reg, sizeof(idle_check_regs));
    idle_check_pending = 1;
  }
  return ret;
}
#define update_gba update_gba_real
#endif

NSPIRE_HOT_FIRST u32 function_cc update_gba(int remaining_cycles)
{
  u32 changed_pc = 0;
  u32 frame_complete = 0;
  irq_type irq_raised = IRQ_NONE;
  int dma_cycles;
#ifdef NSPIRE_BUILD
  /* core-16: called by a proven-pure idle loop's stub, armed and untainted
     (arm_stub.S arm_update_gba_pure_thumb; remaining cycles 0 as for any
     idle call). Its events run here one after another while each changes
     nothing the loop's body reads (see the loop's end). */
  int idle_pure = remaining_cycles == NSPIRE_IDLE_PURE_CALL;
  if (idle_pure)
    remaining_cycles = 0;
#endif
  trace_update_gba(remaining_cycles);

  remaining_cycles = MAX(remaining_cycles, -64);

#ifdef NSPIRE_BUILD
next_idle_event:
#endif
  do
  {
    unsigned i;
    // Number of cycles we ask to run - cycles that we did not execute
    // (remaining_cycles can be negative and should be close to zero)
    unsigned completed_cycles = execute_cycles - remaining_cycles;
    cpu_ticks += completed_cycles;

    remaining_cycles = 0;

    // Timers can trigger DMA (usually sound) and consume cycles
    dma_cycles = update_timers(&irq_raised, completed_cycles);
    // Check for serial port IRQs as well.
    if (update_serial(completed_cycles))
      irq_raised |= IRQ_SERIAL;

    // Video count tracks the video cycles remaining until the next event
    video_count -= completed_cycles;

    // Ran out of cycles, move to the next video area
    if(video_count <= 0)
    {
      u32 vcount = read_ioreg(REG_VCOUNT);
      u32 dispstat = read_ioreg(REG_DISPSTAT);

      // Check if we are in hrefresh (0) or hblank (1)
      if ((dispstat & 0x02) == 0)
      {
        // Transition from hrefresh to hblank
        dispstat |= 0x02;
        video_count += (272);    // hblank duration, 272 cycles

        // Check if we are drawing (0) or we are in vblank (1)
        if ((dispstat & 0x01) == 0)
        {
          u32 i;

          // Render the scan line
          if(reg[OAM_UPDATED])
            oam_update_count++;

#ifdef NSPIRE_BUILD
          nspire_video_line_ready(vcount);
#else
          update_scanline();
#endif

          // Trigger the HBlank DMAs if enabled
          for (i = 0; i < 4; i++)
          {
            if(dma[i].start_type == DMA_START_HBLANK)
              dma_transfer(i, &dma_cycles);
          }
        }

        // Trigger the hblank interrupt, if enabled in DISPSTAT
        if (dispstat & 0x10)
          irq_raised |= IRQ_HBLANK;
      }
      else
      {
        // Transition from hblank to the next scan line (vdraw or vblank)
        video_count += 960;
        dispstat &= ~0x02;
        vcount++;

        if(vcount == 160)
          dispstat = enter_vblank(dispstat, &irq_raised, &dma_cycles);
        else if (vcount == 228)
        {
          // Transition from vblank to next screen
          vcount = 0;
          dispstat &= ~0x01;

          /* If there's no cheat hook, run on vblank! */
#if !defined(NSPIRE_LIBRETRO)
          /* (r19: the Nspire frontend runs the codes once a frame itself, after the frame's emulation
           * (nspire_host_main.c). Once codes could be loaded this call was no longer provably empty, and the
           * values update_gba keeps in registers had to be loaded again after it: 188 bytes more in the hot
           * stretch, which moved sound_timer off its pinned place in the cache's way, nspire_lineio.ld.) */
          if (cheat_master_hook == ~0U)
             process_cheats();
#endif

/*        printf("frame update (%x), %d instructions total, %d RAM flushes\n",
           reg[REG_PC], instruction_count - last_frame, flush_ram_count);
          last_frame = instruction_count;
*/
/*          printf("%d gbc audio updates\n", gbc_update_count);
          printf("%d oam updates\n", oam_update_count); */
          gbc_update_count = 0;
          oam_update_count = 0;
          flush_ram_count = 0;

          // Force audio generation. Need to flush samples for this frame.
          render_gbc_sound();
#if defined(NSPIRE_NO_AUDIO) || defined(NSPIRE_RUNTIME_AUDIO)
          sound_flush_all_pops();   // core-04: keeps the counts short
#endif

          // We completed a frame, tell the dynarec to exit to the main thread
          frame_complete = 0x80000000;
          frame_counter++;
        }

        // Vcount trigger (flag) and IRQ if enabled
        if(vcount == (dispstat >> 8))
        {
          dispstat |= 0x04;
          if(dispstat & 0x20)
            irq_raised |= IRQ_VCOUNT;
        }
        else
          dispstat &= ~0x04;

        write_ioreg(REG_VCOUNT, vcount);
      }
      write_ioreg(REG_DISPSTAT, dispstat);
    }

    // Flag any V/H blank interrupts, DMA IRQs, Vcount, etc.
    if (irq_raised)
      flag_interrupt(irq_raised);

    // Raise any pending interrupts. This changes the CPU mode.
    if (check_and_raise_interrupts())
      changed_pc = 0x40000000;

    // Figure out when we need to stop CPU execution. The next event is
    // a video event or a timer event, whatever happens first.
    execute_cycles = MAX(video_count, 0);
    {
      u32 cc = serial_next_event();
      execute_cycles = MIN(execute_cycles, cc);
    }

    // If we are paused due to a DMA, cap the number of cyles to that amount.
    if (reg[CPU_HALT_STATE] == CPU_DMA) {
      u32 dma_cyc = reg[REG_SLEEP_CYCLES];
      // The first iteration is marked by bit 31 set, just do nothing now.
      if (dma_cyc & 0x80000000)
        dma_cyc &= 0x7FFFFFFF;  // Start counting DMA cycles from now on.
      else
        dma_cyc -= MIN(dma_cyc, completed_cycles);  // Account DMA cycles.

      reg[REG_SLEEP_CYCLES] = dma_cyc;
      if (!dma_cyc)
        reg[CPU_HALT_STATE] = CPU_ACTIVE;   // DMA finished, resume execution.
      else
        execute_cycles = MIN(execute_cycles, dma_cyc);  // Continue sleeping.
    }

    // Timers 1-3 only when one runs (an HBlank DMA may have started one
    // since update_timers, so this reads the status again).
    unsigned timer_limit =
      (timer[1].status | timer[2].status | timer[3].status) ? 4 : 1;
    for (i = 0; i < timer_limit; i++)
    {
       if (timer[i].status == TIMER_PRESCALE &&
           timer[i].count < execute_cycles)
          execute_cycles = timer[i].count;
    }
  } while(reg[CPU_HALT_STATE] != CPU_ACTIVE && !frame_complete);

#ifdef NSPIRE_BUILD
  // core-16: for a pure idle loop (idle_pure), the next event as the
  // loop's next call would run it, when this one took no interrupt (no PC
  // change), the frame goes on and no DMA ran (any DMA of a unit or more
  // costs cycles: def_seq_cycles is at least 1 everywhere). The rest of an
  // event writes only I/O registers (DISPSTAT, VCOUNT, IF, the timers'
  // counts, the serial port's), which the body doesn't read untainted: the
  // body would end in the same registers and flags and call again
  // (remaining cycles 0, as they are now; irq_raised as at a call's start).
  // speed9b: also Advance Wars' idle pass (arm_update_gba_idle_pass_thumb:
  // its state at the pass's fixed point when it called) while the pass would
  // fit in the next window (reg[REG_IDLE_LIMIT]: its cost less one; 0 for
  // core-16's loops, any window).
  if (__builtin_expect(idle_pure, 0) && !frame_complete && !changed_pc &&
      !dma_cycles && execute_cycles > reg[REG_IDLE_LIMIT])
  {
#ifdef NSPIRE_IDLE_CHECK
    idle_pass_would = 1;   /* the check build: from the idle pass's calls only */
#else
    irq_raised = IRQ_NONE;
    goto next_idle_event;
#endif
  }
#endif

  // We voluntarily limit this. It is not accurate but it would be much harder.
  dma_cycles = MIN(64, dma_cycles);
  dma_cycles = MIN(execute_cycles, dma_cycles);

  return (execute_cycles - dma_cycles) | changed_pc | frame_complete;
}
#ifdef NSPIRE_IDLE_CHECK
#undef update_gba
#endif

#if defined(NSPIRE_BUILD) && defined(HAVE_DYNAREC)
/* speed9b: what arm_update_gba_idle_pass_thumb calls until the pass's cost
   C is known (in the check build always), with the cycles left in the window
   (see idle_pass_code above): update_gba as an idle call. C: measured on a
   pass between two of these calls that started at the fixed point (the first
   call returned with no interrupt and the state still at it, a DMA included;
   the window it returned less the cycles left at the second; cpu_ticks
   unchanged: no update_gba call between, so no event inside the pass). Used
   once two measurements agree (the stub then calls update_gba itself, with
   NSPIRE_IDLE_PURE_CALL and reg[REG_IDLE_LIMIT] = C - 1 when the state is
   the fixed point: an event without interrupt or DMA keeps it so); one that differs
   turns it off until the next reset. The pass's cost is its instructions'
   (the translator's fixed counts, the same in any block split), so it isn't
   measured again; the check build measures every pass it can. */
u32 nspire_update_gba_idle_pass(int left)
{
  u32 ret, fixed = nspire_idle_pass_fixed();
  if (fixed && idle_pass_from_fixed && cpu_ticks == idle_pass_ticks)
  {
    u32 c = idle_pass_window - (u32)left;
    if (!idle_pass_seen)
    {
      idle_pass_cost = c;
      idle_pass_seen = c >= 2 && c < 0x10000 ? 1 : 3;
    }
    else if (idle_pass_seen == 1)
      idle_pass_seen = c == idle_pass_cost ? 2 : 3;
#ifdef NSPIRE_IDLE_CHECK
    else if (idle_pass_seen == 2 && c != idle_pass_cost)
      nspire_idle_pass_check_cost_bad++;
#endif
  }
#ifdef NSPIRE_IDLE_CHECK
  if (idle_pass_check_pending)
  {
    u32 sp = reg[REG_SP];
    idle_pass_check_pending = 0;
    if (fixed && cpu_ticks == idle_pass_check_ticks &&
        !memcmp(idle_pass_check_regs, reg, sizeof(idle_pass_check_regs)) &&
        readaddress32(iwram, 0x8000 + ((sp - 8) & 0x7FFF)) == idle_pass_check_stack[0] &&
        readaddress32(iwram, 0x8000 + ((sp - 4) & 0x7FFF)) == idle_pass_check_stack[1] &&
        idle_pass_window - (u32)left == idle_pass_cost)
      nspire_idle_pass_check_ok++;
    else
      nspire_idle_pass_check_bad++;
  }
  idle_pass_would = 0;
  if (idle_pass_seen == 2 && fixed)
  {
    /* as the stub would: update_gba's repeat (here only noted) */
    reg[REG_IDLE_LIMIT] = idle_pass_cost - 1;
    ret = update_gba(NSPIRE_IDLE_PURE_CALL);
    reg[REG_IDLE_LIMIT] = 0;
  }
  else
#endif
  ret = update_gba(0);
#ifdef NSPIRE_IDLE_CHECK
  if (idle_pass_would && !(ret & 0xC0000000))
  {
    u32 sp = reg[REG_SP];
    memcpy(idle_pass_check_regs, reg, sizeof(idle_pass_check_regs));
    idle_pass_check_stack[0] = readaddress32(iwram, 0x8000 + ((sp - 8) & 0x7FFF));
    idle_pass_check_stack[1] = readaddress32(iwram, 0x8000 + ((sp - 4) & 0x7FFF));
    idle_pass_check_ticks = cpu_ticks;
    idle_pass_check_pending = 1;
  }
#endif
  /* Back into the loop (no interrupt taken, the frame goes on): its next
     pass starts at the fixed point if the state still is (nothing but an
     interrupt changes the registers; a DMA may have changed memory). */
  idle_pass_window = ret & 0x0FFFFFFF;
  idle_pass_ticks = cpu_ticks;
  idle_pass_from_fixed = fixed && !(ret & 0xC0000000) && nspire_idle_pass_fixed();
  return ret;
}
#endif

// A DMA that may run while the CPU is halted: whether what it writes, over
// up to a frame of its triggers, can wake the CPU or change the I/O
// registers other than the display's and sound's (0x08-0xAF): IE, the
// timers, the DMAs and the serial port's.
static bool dma_may_matter(const dma_transfer_type *d, u32 ie, unsigned i)
{
  s64 size = d->length_type == DMA_32BIT ? 4 : 2;
  s64 span = (s64)d->length * size;
  s64 times = d->repeat_type == DMA_REPEAT && d->start_type == DMA_START_HBLANK ? 228 : 1;
  s64 lo = d->dest_address, hi;

  if (d->irq && (ie & (IRQ_DMA0 << i)))
    return true;
  switch (d->dest_direction)
  {
  case DMA_INCREMENT: hi = lo + times * span; break;
  case DMA_DECREMENT: hi = lo + size; lo -= times * span; break;
  case DMA_FIXED:     hi = lo + size; break;
  default:            hi = lo + span; break;   // reloads each time
  }
  return hi > 0x4000000 && lo < 0x5000000 && !(lo >= 0x4000008 && hi <= 0x40000B0);
}

// For the link cable (nspire_link.c), between update_gba's events (after
// their interrupts): how many cycles from now the halted CPU surely stays
// halted and nothing changes its I/O registers (dma_may_matter): until an
// interrupt it has enabled could come (video, timers, DMAs, the serial
// transfer under way, serial_busy cycles from now, keys at the frame's
// end), a DMA that matters could run, or the frame ends (the frontend's
// time). 0 when it's running. Each is counted the earliest it can be (a
// cascading timer's the timer below's overflow).
u32 halted_cycles(u32 serial_busy)
{
  u32 ie = read_ioreg(REG_IE), dispstat = read_ioreg(REG_DISPSTAT);
  u32 line = read_ioreg(REG_VCOUNT), lyc = dispstat >> 8;
  u32 overflow[4], next_line, hblank, until, i;

  if (reg[CPU_HALT_STATE] != CPU_HALT || (ie & read_ioreg(REG_IF)) || video_count < 0)
    return 0;
  // The next line's start (from the hblank of this one, or this hblank's
  // end) and the next hblank's (this line's, or the next's).
  next_line = (dispstat & 0x02) ? video_count : video_count + 272;
  hblank = (dispstat & 0x02) ? video_count + 960 : video_count;
  #define LINE_START(l) (next_line + ((l) - line - 1) * 1232)
  until = LINE_START(228);   // the frame's end (line 0's start)

  if (serial_busy)
    until = MIN(until, serial_busy);
  if ((ie & IRQ_VBLANK) && (dispstat & 0x08) && line < 160)
    until = MIN(until, LINE_START(160));
  if ((ie & IRQ_HBLANK) && (dispstat & 0x10))
    until = MIN(until, hblank);
  if ((ie & IRQ_VCOUNT) && (dispstat & 0x20) && lyc > line && lyc < 228)
    until = MIN(until, LINE_START(lyc));

  for (i = 0; i < 4; i++)
  {
    if (timer[i].status == TIMER_PRESCALE)
    {
      if (timer[i].count <= 0)
        return 0;
      overflow[i] = timer[i].count;
    }
    else if (timer[i].status == TIMER_CASCADE && i > 0)
      overflow[i] = overflow[i - 1];
    else
      overflow[i] = ~0U;
    if (timer[i].irq && (ie & (IRQ_TIMER0 << i)))
      until = MIN(until, overflow[i]);
  }

  for (i = 0; i < DMA_CHAN_CNT; i++)
  {
    const dma_transfer_type *d = &dma[i];
    if (d->start_type == DMA_INACTIVE || !dma_may_matter(d, ie, i))
      continue;
    if (d->start_type == DMA_START_HBLANK)
    {
      // In the lines drawn only
      if (((dispstat & 0x02) ? line + 1 : line) < 160)
        until = MIN(until, hblank);
    }
    else if (d->start_type == DMA_START_VBLANK)
    {
      if (line < 160)
        until = MIN(until, LINE_START(160));
    }
    else if (d->start_type == DMA_START_SPECIAL)
      until = MIN(until, MIN(overflow[0], overflow[1]));   // the sound's timers
    else
      return 0;
  }
  #undef LINE_START
  return until;
}

void reset_gba(void)
{
  init_memory();
  init_main();
  init_cpu();
  reset_sound();
#ifdef NSPIRE_BUILD
  nspire_video_lines_reset();
#endif
#if defined(NSPIRE_LIBRETRO)
  nspire_fps_overlay_reset();
#endif
}

#ifdef TRACE_REGISTERS
void print_regs(void)
{
  printf("R0=%08x R1=%08x R2=%08x R3=%08x "
         "R4=%08x R5=%08x R6=%08x R7=%08x "
         "R8=%08x R9=%08x R10=%08x R11=%08x "
         "R12=%08x R13=%08x R14=%08x\n",
         reg[0], reg[1], reg[2], reg[3],
         reg[4], reg[5], reg[6], reg[7],
         reg[8], reg[9], reg[10], reg[11],
         reg[12], reg[13], reg[14]);
}
#endif

bool main_check_savestate(const u8 *src)
{
  int i;
  u32 reload, prescale, status, irq, channels;
  const u8 *p1 = bson_find_document(src, "emu");
  const u8 *p2 = bson_find_document(src, "timers");
  if (!p1 || !p2)
    return false;

  if (!bson_contains_key(p1, "cpu-ticks", BSON_TYPE_INT32) ||
      !bson_contains_key(p1, "exec-cycles", BSON_TYPE_INT32) ||
      !bson_contains_key(p1, "video-count", BSON_TYPE_INT32) ||
      !bson_contains_key(p1, "sleep-cycles", BSON_TYPE_INT32))
    return false;

  for (i = 0; i < 4; i++)
  {
    char tname[2] = {'0' + i, 0};
    const u8 *p = bson_find_document(p2, tname);
    if (!p)
      return false;

    if (!bson_contains_key(p, "count", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "reload", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "prescale", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "freq-step", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "dsc", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "irq", BSON_TYPE_INT32) ||
        !bson_contains_key(p, "status", BSON_TYPE_INT32))
      return false;
    if (!bson_read_int32(p, "reload", &reload) || !reload || reload > 0x10000 ||
        !bson_read_int32(p, "prescale", &prescale) ||
        (prescale != 0 && prescale != 6 && prescale != 8 && prescale != 10) ||
        !bson_read_int32(p, "status", &status) || status > TIMER_CASCADE ||
        !bson_read_int32(p, "irq", &irq) || irq > TIMER_TRIGGER_IRQ ||
        !bson_read_int32(p, "dsc", &channels) || channels > TIMER_DS_CHANNEL_BOTH)
      return false;
  }

  return true;
}

bool main_read_savestate(const u8 *src)
{
  int i;
  const u8 *p1 = bson_find_key(src, "emu");
  const u8 *p2 = bson_find_key(src, "timers");
  if (!p1 || !p2)
    return false;

  if (!(bson_read_int32(p1, "cpu-ticks", &cpu_ticks) &&
         bson_read_int32(p1, "exec-cycles", &execute_cycles) &&
         bson_read_int32(p1, "video-count", (u32*)&video_count) &&
         bson_read_int32(p1, "sleep-cycles", &reg[REG_SLEEP_CYCLES])))
    return false;

  if (!bson_read_int32(p1, "frame-count", &frame_counter))
    frame_counter = 60 * 10;  // Use "fake" 10 seconds.

  for (i = 0; i < 4; i++)
  {
    char tname[2] = {'0' + i, 0};
    const u8 *p = bson_find_key(p2, tname);

    if (!(
      bson_read_int32(p, "count", (u32*)&timer[i].count) &&
      bson_read_int32(p, "reload", &timer[i].reload) &&
      bson_read_int32(p, "prescale", &timer[i].prescale) &&
      bson_read_int32(p, "freq-step", &timer[i].frequency_step) &&
      bson_read_int32(p, "dsc", &timer[i].direct_sound_channels) &&
      bson_read_int32(p, "irq", &timer[i].irq) &&
      bson_read_int32(p, "status", &timer[i].status)))
      return false;
  }

  return true;
}

unsigned main_write_savestate(u8* dst)
{
  int i;
  u8 *wbptr, *wbptr2, *startp = dst;
  bson_start_document(dst, "emu", wbptr);
  bson_write_int32(dst, "frame-count", frame_counter);
  bson_write_int32(dst, "cpu-ticks", cpu_ticks);
  bson_write_int32(dst, "exec-cycles", execute_cycles);
  bson_write_int32(dst, "video-count", video_count);
  bson_write_int32(dst, "sleep-cycles", reg[REG_SLEEP_CYCLES]);
  bson_finish_document(dst, wbptr);

  bson_start_document(dst, "timers", wbptr);
  for (i = 0; i < 4; i++)
  {
    char tname[2] = {'0' + i, 0};
    bson_start_document(dst, tname, wbptr2);
    bson_write_int32(dst, "count", timer[i].count);
    bson_write_int32(dst, "reload", timer[i].reload);
    bson_write_int32(dst, "prescale", timer[i].prescale);
    bson_write_int32(dst, "freq-step", timer[i].frequency_step);
    bson_write_int32(dst, "dsc", timer[i].direct_sound_channels);
    bson_write_int32(dst, "irq", timer[i].irq);
    bson_write_int32(dst, "status", timer[i].status);
    bson_finish_document(dst, wbptr2);
  }
  bson_finish_document(dst, wbptr);

  return (unsigned int)(dst - startp);
}


