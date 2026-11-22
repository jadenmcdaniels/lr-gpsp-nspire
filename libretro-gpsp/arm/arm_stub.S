
#include "../gpsp_config.h"

#define defsymbl(symbol) \
.align 2;                \
.type symbol, %function ;\
.global symbol ;         \
.global _##symbol ;      \
symbol:                  \
_##symbol:

#ifdef NSPIRE_BUILD
@ With update_gba and the other code every interrupt runs (common.h)
.section .text.hot.gpsp,"ax",%progbits
@ Pinned (nspire_lineio.ld): the C hot code before this differs
@ by a few bytes between the calculator build and the speed model's (LTO
@ sees other programs); starting here at a multiple of 256 bytes from
@ update_gba (which starts a 4 KB way) puts every stub on the same cache
@ line and set in both.
.balign 256
#else
.text
.align 2
#endif

#define REG_R0            (0 * 4)
#define REG_R1            (1 * 4)
#define REG_R2            (2 * 4)
#define REG_R3            (3 * 4)
#define REG_R4            (4 * 4)
#define REG_R5            (5 * 4)
#define REG_R6            (6 * 4)
#define REG_R7            (7 * 4)
#define REG_R8            (8 * 4)
#define REG_R9            (9 * 4)
#define REG_R10           (10 * 4)
#define REG_R11           (11 * 4)
#define REG_R12           (12 * 4)
#define REG_R13           (13 * 4)
#define REG_R14           (14 * 4)
#define REG_SP            (13 * 4)
#define REG_LR            (14 * 4)
#define REG_PC            (15 * 4)
#define REG_CPSR          (16 * 4)
#define CPU_MODE          (17 * 4)
#define CPU_HALT_STATE    (18 * 4)

#define REG_BUS_VALUE     (19 * 4)
#define REG_N_FLAG        (20 * 4)
#define REG_Z_FLAG        (21 * 4)
#define REG_C_FLAG        (22 * 4)
#define REG_V_FLAG        (23 * 4)
#define REG_SLEEP_CYCLES  (24 * 4)
#define OAM_UPDATED       (25 * 4)
@ speed10 (cpu.h REG_VIDEO_GEN, nspire_reuse.c): one up at every store that
@ changes a value in palette RAM or VRAM (the handlers' _watch forms, at
@ the hot stretch's end); negative while no line is stamped: then a VRAM
@ store counts as a change without reading the value it replaces (the read
@ would take a data cache line)
#define REG_VIDEO_GEN     (31 * 4)
@ The handlers' first instructions: on the Nspire a branch to the _watch
@ form; elsewhere (no _watch forms) the instruction it took the place of.
#ifdef NSPIRE_BUILD
#define watch_vram(type)         b ext_store_vram_u##type##_watch
#define watch_vram_u32_safe()    b ext_store_vram_u32_safe_watch
#define watch_palette_u8()       b ext_store_palette_u8_watch
#define watch_palette(type)      b ext_store_palette_u##type##_watch
#else
#define watch_vram(type)         save_flags()
#define watch_vram_u32_safe()    mov r0, r0, lsl #(32 - 17)
#define watch_palette_u8()       bic r1, r1, #0xff00
#define watch_palette(type)      mov r0, r0, lsl #(32 - 10)
#endif
@ core-16 (cpu.h REG_IDLE_*): arm_update_gba_pure_thumb
#define REG_IDLE_ARMED    (27 * 4)
#define REG_IDLE_TAINT    (28 * 4)
#define REG_IDLE_OK       (29 * 4)
#define NSPIRE_IDLE_PURE_CALL 0x80000000

@ An SMC store's RAM flush: Nspire since speed10b keeps the RAM blocks' code
@ (cpu_threaded.c nspire_ram_flush_smc), elsewhere the whole flush.
#ifdef NSPIRE_BUILD
#define ram_flush_smc nspire_ram_flush_smc
#else
#define ram_flush_smc flush_translation_cache_ram
#endif

#define CPU_ALERT_HALT    (1 << 0)
#define CPU_ALERT_SMC     (1 << 1)
#define CPU_ALERT_IRQ     (1 << 2)

#define reg_a0            r0
#define reg_a1            r1
#define reg_a2            r2

#define reg_base          r11
#define reg_flags         r9

#define reg_cycles        r12

#define reg_x0            r3
#define reg_x1            r4
#define reg_x2            r5
#define reg_x3            r6
#define reg_x4            r7
#define reg_x5            r8


#define MODE_SUPERVISOR   0x13
#define SUPERVISOR_OFFSET 0x03

@ Memory offsets from reg_base to the different buffers
#define IWRAM_OFF      -0xA8000
#define VRAM_OFF       -0x98000
#define EWRAM_OFF      -0x80000
#define SPSR_RAM_OFF      0x100
#define STORE_TBL_OFF     0x118
#define REGMODE_RAM_OFF   0x400
#define OAM_RAM_OFF       0x500
#define PAL_RAM_OFF       0x900
#define RDMAP_OFF         0xD00
#define IOREG_OFF        0x8D00
#define PAL_CONV_OFF     0x9100


#if __ARM_ARCH >= 6
  #define extract_u16(rd, rs) \
    uxth rd, rs
#else
  #define extract_u16(rd, rs) \
    bic rd, rs, #0xff000000;  \
    bic rd, rd, #0x00ff0000
#endif

@ Will load the register set from memory into the appropriate cached registers.
@ See arm_emit.h for listing explanation.

#define load_registers_arm()                                                 ;\
  ldr reg_x0, [reg_base, #REG_R0]                                            ;\
  ldr reg_x1, [reg_base, #REG_R1]                                            ;\
  ldr reg_x2, [reg_base, #REG_R6]                                            ;\
  ldr reg_x3, [reg_base, #REG_R9]                                            ;\
  ldr reg_x4, [reg_base, #REG_R12]                                           ;\
  ldr reg_x5, [reg_base, #REG_R14]                                           ;\

#define load_registers_thumb()                                               ;\
  ldm reg_base, {reg_x0, reg_x1, reg_x2, reg_x3, reg_x4, reg_x5}


@ Will store the register set from cached registers back to memory.

#define store_registers_arm()                                                ;\
  str reg_x0, [reg_base, #REG_R0]                                            ;\
  str reg_x1, [reg_base, #REG_R1]                                            ;\
  str reg_x2, [reg_base, #REG_R6]                                            ;\
  str reg_x3, [reg_base, #REG_R9]                                            ;\
  str reg_x4, [reg_base, #REG_R12]                                           ;\
  str reg_x5, [reg_base, #REG_R14]                                           ;\

#define store_registers_thumb()                                              ;\
  stm reg_base, {reg_x0, reg_x1, reg_x2, reg_x3, reg_x4, reg_x5}

#define store_registers_cond()                                               ;\
  stmne reg_base, {reg_x0, reg_x1, reg_x2, reg_x3, reg_x4, reg_x5}           ;\
  bne 80f                                                                    ;\
  str reg_x0, [reg_base, #REG_R0]                                            ;\
  str reg_x1, [reg_base, #REG_R1]                                            ;\
  str reg_x2, [reg_base, #REG_R6]                                            ;\
  str reg_x3, [reg_base, #REG_R9]                                            ;\
  str reg_x4, [reg_base, #REG_R12]                                           ;\
  str reg_x5, [reg_base, #REG_R14]                                           ;\
  80:

@ Returns an updated persistent cpsr with the cached flags register.
@ Uses reg as a temporary register and returns the CPSR here.

#define collapse_flags_no_update(reg)                                        ;\
  ldr reg, [reg_base, #REG_CPSR]          /* reg = cpsr                    */;\
  bic reg, reg, #0xF0000000               /* clear ALU flags in cpsr       */;\
  and reg_flags, reg_flags, #0xF0000000   /* clear non-ALU flags           */;\
  orr reg, reg, reg_flags                 /* update cpsr with ALU flags    */;\

@ Updates cpsr using the above macro.

#define collapse_flags(reg)                                                  ;\
  collapse_flags_no_update(reg)                                              ;\
  str reg, [reg_base, #REG_CPSR]                                             ;\

@ Loads the saved flags register from the persistent cpsr.

#define extract_flags()                                                      ;\
  ldr reg_flags, [reg_base, #REG_CPSR]                                       ;\
  msr cpsr_f, reg_flags                                                      ;\


#define save_flags()                                                         ;\
  mrs reg_flags, cpsr                                                        ;\

#define restore_flags()                                                      ;\
  msr cpsr_f, reg_flags                                                      ;\

@ Align the stack to 64 bits (ABIs that don't require it, still recommend so)
#define call_c_saved_regs r2, r3, r12, lr

@ Calls a C function - reloads the stack pointer and saves all caller save
@ registers which are important to the dynarec.

#define call_c_function(function)                                            ;\
  stmdb sp!, { call_c_saved_regs }                                           ;\
  bl function                                                                ;\
  ldmia sp!, { call_c_saved_regs }                                           ;\

@ The memory handlers' return and the GBA PC of their access (arm_emit.h
@ generate_mem_pc). Nspire since core-17 (2026-11-15): no PC word after the
@ translated code's "bl mem_dispatch_*", so "bx lr", and the PC from
@ nspire_mem_pc (cpu_threaded.c's tables, by the return address) into r2;
@ r0, r1, r3, r12 and lr kept, the flags not (saved in reg_flags first).
@ NSPIRE_PCWORD_CHECK (tests/arm PCWORD_CHECK=1): the words are there as
@ before and nspire_mem_pc checks its answer against them.
#if defined(NSPIRE_BUILD) && !defined(NSPIRE_PCWORD_CHECK)
#define mem_return()        bx lr
#define palette_safe_lr()                 /* returns as the u32 one */
#else
#define mem_return()        add pc, lr, #4
#define palette_safe_lr()   sub lr, lr, #4  /* no word after a _safe call */
#endif
#ifdef NSPIRE_BUILD
#define mem_pc_r2()                                                          ;\
  stmdb sp!, { r0, r1, r2, r3, r12, lr }                                     ;\
  mov r0, lr                                                                 ;\
  bl nspire_mem_pc                                                           ;\
  str r0, [sp, #8]                                                           ;\
  ldmia sp!, { r0, r1, r2, r3, r12, lr }                                     ;\

#else
#define mem_pc_r2()         ldr r2, [lr]
#endif

@ A store to I/O registers or the cartridge (RTC, backup): the PC written
@ out before write_io_register_* / write_memory*, or Nspire since core-17
@ only when write_io_register_* returns an alert (io_store_alert:
@ write_epilogue resumes there and raises interrupts with it). No C code
@ under them reads reg[REG_PC] (DMA reads no open bus nor BIOS through
@ read_memory; their u32_safe calls from block stores never wrote it;
@ core-17), so the lookup is saved but for those few: its
@ tables outlive a flush inside the call until the next translation. The
@ check build stores 0x0F000000 there instead, so that a reader would
@ change the replays' pictures.
#if defined(NSPIRE_BUILD) && defined(NSPIRE_PCWORD_CHECK)
#define unread_pc()         mov r2, #0x0F000000; str r2, [reg_base, #REG_PC]
#elif defined(NSPIRE_BUILD)
#define unread_pc()
#else
#define unread_pc()         ldr r2, [lr]; str r2, [reg_base, #REG_PC]
#define io_store_alert      write_epilogue
#endif

#ifdef NSPIRE_BUILD
@ Looks up the translated block for r0 (the PC as block_lookup_address_MODE
@ takes it) and leaves it in r0: ROM and BIOS blocks from nspire_block_cache
@ (cpu_threaded.c) when they're there, RAM blocks from their tags
@ (nspire_ram_lookup_MODE, below), the rest from block_lookup_address_*,
@ which fills the cache. Clobbers r1, r2, lr and the flags (call with them
@ saved). Keys from 0x0E000000 up are never in the cache (block_cache_put)
@ and never equal an empty entry's ~1 (ARM keys are multiples of 4, Thumb
@ ones odd): their probe misses, then C. (2026-11-17, speed9b) The RAM
@ test is a conditional call to code at the hot stretch's end that comes
@ back to the probe (the BIOS), to its beq with Z set and r0 the block (a
@ RAM block found) or to 5 (C): every expansion as long as before, the
@ stubs after them where they were.
#define cache_key_arm()    bic r1, r0, #3
#define cache_key_thumb()  bic r1, r0, #1; orr r1, r1, #1

@ Entry (key ^ (key >> 12)) & 2047, 8 bytes (cpu_threaded.c block_cache_put).
#define cached_lookup(mode)                                                  ;\
  cache_key_##mode()                      /* r1 = key                      */;\
  cmp r1, #0x04000000                     /* the BIOS, RAM                 */;\
  bllo nspire_ram_lookup_##mode           /* (back at the eor, beq or 5)   */;\
  eor r2, r1, r1, lsr #12                                                    ;\
  mov r2, r2, lsl #21                     /* the entry << 21               */;\
  ldr lr, =nspire_block_cache                                                ;\
  add r2, lr, r2, lsr #18                                                    ;\
  ldr lr, [r2]                                                               ;\
  cmp lr, r1                                                                 ;\
  ldreq r0, [r2, #4]                                                         ;\
  beq 6f                                                                     ;\
5:                                                                           ;\
  call_c_function(block_lookup_address_##mode)                               ;\
6:                                                                           ;\

#else
#define cached_lookup(mode) call_c_function(block_lookup_address_##mode)
#endif


@ Update the GBA hardware (video, sound, input, etc)

@ Input:
@ r0: current PC

#define return_straight()   bx lr
#define return_add()        add pc, lr, #4

#define load_pc_straight()  ldr r0, [lr, #-12]
#define load_pc_add()       ldr r0, [lr]

#define cycles_straight()  mvn r0, reg_cycles
#define cycles_add()       mov r0, #0

@ For arm_check_update_* (Nspire): PC word after the call, cycles counted.
#define return_check()      add pc, lr, #4
#define load_pc_check()     ldr r0, [lr]
#define cycles_check()      mvn r0, reg_cycles

#define arm_update_gba_builder(name, mode, return_op)                        ;\
                                                                             ;\
defsymbl(arm_update_gba_##name)                                              ;\
  load_pc_##return_op()                                                      ;\
  str r0, [reg_base, #REG_PC]             /* write out the PC              */;\
                                                                             ;\
  save_flags()                                                               ;\
  collapse_flags(r0)                      /* update the flags              */;\
                                                                             ;\
  store_registers_##mode()                /* save out registers            */;\
  cycles_##return_op()                    /* remaining cycles in arg0      */;\
  call_c_function(update_gba)             /* update GBA state              */;\
                                                                             ;\
  cmp r0, #0                              /* check MSB for frame completion*/;\
  blt return_to_main                                                         ;\
                                                                             ;\
  bic reg_cycles, r0, #0xF0000000         /* clear MSB, not part of count  */;\
  mvn reg_cycles, reg_cycles              /* we count negative to zero     */;\
                                                                             ;\
  tst r0, #0x40000000                     /* set if PC changed             */;\
  bne 1f                                  /* go jump/translate             */;\
                                                                             ;\
  load_registers_##mode()                 /* reload registers              */;\
  restore_flags()                                                            ;\
  return_##return_op()                    /* continue, no PC change        */;\
                                                                             ;\
1:                                                                           ;\
  ldr r1, [reg_base, #REG_CPSR]           /* r1 = flags                    */;\
  ldr r0, [reg_base, #REG_PC]             /* load new PC                   */;\
  tst r1, #0x20                           /* see if Thumb bit is set       */;\
  bne 2f                                  /* if so load Thumb PC           */;\
                                                                             ;\
  load_registers_arm()                    /* load ARM regs                 */;\
  b arm_lookup_saved_arm                  /* jump to new ARM block         */;\
2:                                                                           ;\
  load_registers_thumb()                  /* load Thumb regs               */;\
  b arm_lookup_saved_thumb                /* jump to new Thumb block       */;\
.size arm_update_gba_##name, .-arm_update_gba_##name

arm_update_gba_builder(arm, arm, straight)
arm_update_gba_builder(thumb, thumb, straight)

arm_update_gba_builder(idle_arm, arm, add)
arm_update_gba_builder(idle_thumb, thumb, add)

arm_update_gba_builder(check_arm, arm, check)
arm_update_gba_builder(check_thumb, thumb, check)
.ltorg

@ A block's exit (Nspire, arm_emit.h generate_branch_update):
@   bl arm_check_update_MODE ; .word PC ; b next block
@ With cycles left (the counter is negative) back past the PC word, else
@ update the GBA as arm_update_gba_MODE does. Flags untouched; r0 is free.

#define check_update_builder(mode)                                           ;defsymbl(arm_check_update_##mode)                                            ;  mov r0, reg_cycles, lsr #31             /* 1: cycles left                */;  add pc, pc, r0, lsl #2                                                     ;  nop                                                                        ;  b arm_update_gba_check_##mode                                              ;  add pc, lr, #4                          /* past the PC word              */;
check_update_builder(arm)
check_update_builder(thumb)

#ifdef NSPIRE_BUILD
@ Block transfers on IWRAM (arm_emit.h nspire_block_fast_check, core-12,
@ 2026-11-15): the check that used to be inline in every transfer,
@   bl nspire_block_check{,_tag} + 8 * (K - 1) ; b <per-register code>
@ for K words (1-16). Each entry loads K << 19 and goes to the body, which
@ makes today's tests: the top byte of r9 (the aligned GBA address) is 3,
@ and the offset in IWRAM's 32 KB mirror plus 4K doesn't reach 0x8000 (so
@ a run ending right at its end is slow, as before); _tag also: the tag
@ (translated code) of the last word is 0. Passed: r0 = the host address of
@ the first word, back past the b. Else back to the b. The GBA's flags (the
@ host's) are kept, r9 too; r1 and r2 are clobbered.
defsymbl(nspire_block_check)
.irp k, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
  mov r0, #(\k << 19)
  b nspire_block_check_body
.endr
defsymbl(nspire_block_check_tag)
.irp k, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
  mov r0, #(\k << 19)
  b nspire_block_check_tag_body
.endr

defsymbl(nspire_block_check_body)
  mrs r2, cpsr
  mov r1, r9, lsr #24
  teq r1, #3                              @ IWRAM
  bne nspire_block_check_slow
  mov r1, r9, lsl #17                     @ the offset << 17
  cmn r1, r0                              @ carry: offset + 4K >= 0x8000
  bcs nspire_block_check_slow
  add r0, reg_base, r1, lsr #17
  add r0, r0, #(IWRAM_OFF + 0x8000)       @ the first word
  msr cpsr_f, r2
  add pc, lr, #4

defsymbl(nspire_block_check_tag_body)
  mrs r2, cpsr
  mov r1, r9, lsr #24
  teq r1, #3                              @ IWRAM
  bne nspire_block_check_slow
  mov r1, r9, lsl #17                     @ the offset << 17
  cmn r1, r0                              @ carry: offset + 4K >= 0x8000
  bcs nspire_block_check_slow
  add r1, reg_base, r1, lsr #17
  add r1, r1, #IWRAM_OFF                  @ the first word's tag
  add r0, r1, r0, lsr #17
  ldr r0, [r0, #-4]                       @ the last word's tag
  teq r0, #0
  bne nspire_block_check_slow
  add r0, r1, #0x8000                     @ the first word
  msr cpsr_f, r2
  add pc, lr, #4

@ Its own symbol: the model's listing counts the slow transfers (2 each).
defsymbl(nspire_block_check_slow)
  msr cpsr_f, r2
  bx lr
#endif


@ Cheat hooks for master function
@ This is called whenever PC == cheats-master-function
@ Just calls the C function to process cheats

#define cheat_hook_builder(mode)                                             ;\
defsymbl(mode##_cheat_hook)                                                  ;\
  save_flags()                                                               ;\
  store_registers_##mode()                                                   ;\
  call_c_function(process_cheats)                                            ;\
  load_registers_##mode()                                                    ;\
  restore_flags()                                                            ;\
  bx lr                                                                      ;\

cheat_hook_builder(arm)
cheat_hook_builder(thumb)


@ These are b stubs for performing indirect branches. They are not
@ linked to and don't return, instead they link elsewhere.

@ Input:
@ r0: PC to branch to

defsymbl(arm_indirect_branch_arm)
  save_flags()
@ The rest of every lookup with the flags already in reg_flags (the update,
@ CPSR/SPSR and dual branch stubs come here: one copy of cached_lookup).
arm_lookup_saved_arm:
  cached_lookup(arm)
  restore_flags()
  bx r0

defsymbl(arm_indirect_branch_thumb)
  save_flags()
arm_lookup_saved_thumb:
  cached_lookup(thumb)
  restore_flags()
  bx r0

defsymbl(arm_indirect_branch_dual_arm)
  save_flags()
  tst r0, #0x01                           @ check lower bit
  bne 1f                                  @ if set going to Thumb mode
  add r0, #2                              @ two LSB are cleared after
  b arm_lookup_saved_arm                  @ keep executing arm code
1:
  store_registers_arm()                   @ save out ARM registers
  ldr r1, [reg_base, #REG_CPSR]           @ load cpsr
  load_registers_thumb()                  @ load in Thumb registers
  orr r1, r1, #0x20                       @ set Thumb mode
  str r1, [reg_base, #REG_CPSR]           @ store flags
  b arm_lookup_saved_thumb
.size arm_indirect_branch_dual_arm, .-arm_indirect_branch_dual_arm

defsymbl(arm_indirect_branch_dual_thumb)
  save_flags()
  tst r0, #0x01                           @ check lower bit
  beq 1f                                  @ if set going to ARM mode
  b arm_lookup_saved_thumb                @ keep executing thumb code
1:
  store_registers_thumb()                 @ save out Thumb registers
  ldr r1, [reg_base, #REG_CPSR]           @ load cpsr
  load_registers_arm()                    @ load in ARM registers
  bic r1, r1, #0x20                       @ clear Thumb mode
  str r1, [reg_base, #REG_CPSR]           @ store flags
  add r0, #2                              @ two LSB are cleared after
  b arm_lookup_saved_arm
.size arm_indirect_branch_dual_thumb, .-arm_indirect_branch_dual_thumb
.ltorg

@ Update the cpsr.

@ Input:
@ r0: new cpsr value
@ [lr]:   bitmask (user mode)
@ [lr+4]: bitmask (privileged mode)
@ [lr+8]: current PC

defsymbl(execute_store_cpsr)
#ifdef NSPIRE_BUILD
  b nspire_store_cpsr_fast                @ (speed10b: at the hot stretch's end)
execute_store_cpsr_way:                   @ its slow way, r0 the value
#else
  save_flags()
#endif
  ldr r1, [reg_base, #CPU_MODE]           @ r1 = cpu_mode
  lsr r1, r1, #4                          @ Load privilege bit
  ldr r1, [lr, r1, lsl #2]                @ Load mask

  and reg_flags, r0, r1                   @ reg_flags = new_cpsr & store_mask
  ldr r0, [reg_base, #REG_CPSR]           @ r0 = cpsr
  bic r0, r0, r1                          @ r0 = cpsr & ~store_mask
  orr reg_flags, reg_flags, r0            @ reg_flags = new_cpsr | cpsr

  mov r0, reg_flags                       @ also put new cpsr in r0

execute_store_cpsr_call:                  @ (r0 the new CPSR, r1 the mask)
  store_registers_arm()                   @ save ARM registers
  ldr r2, [lr, #8]                        @ r2 = pc
  call_c_function(execute_store_cpsr_body)
  load_registers_arm()                    @ restore ARM registers

  cmp r0, #0                              @ check new PC
  bne 1f                                  @ if it's zero, resume

  restore_flags()
  add pc, lr, #12                         @ return (skip data)

1:
  b arm_lookup_saved_arm                  @ return to PC ARM address
.size execute_store_cpsr, .-execute_store_cpsr

@ Restore the cpsr from the mode spsr and mode shift.

@ Input:
@ r0: current pc

defsymbl(execute_spsr_restore)
  save_flags()
  ldr r2, [reg_base, #CPU_MODE]           @ r2 = cpu_mode
  ands r2, r2, #0xF                       @ Ignore privilege bits
  beq 2f                                  @ if user/system mode no side effects

  add r1, reg_base, #SPSR_RAM_OFF         @ r1 = spsr
  ldr r1, [r1, r2, lsl #2]                @ r1 = spsr[cpu_mode] (new cpsr)
  str r1, [reg_base, #REG_CPSR]           @ update cpsr
  mov reg_flags, r1                       @ also, update shadow flags

  @ This function call will pass r0 (address) and return it.
  store_registers_arm()                   @ save ARM registers
#ifdef NSPIRE_BUILD
  b nspire_spsr_restore_fast              @ (speed10b: at the hot stretch's end;
  nop                                     @  the stubs after keep their places)
  nop
#else
  call_c_function(execute_spsr_restore_body)
#endif

execute_spsr_restore_done:
  ldr r1, [reg_base, #REG_CPSR]           @ r1 = cpsr
  tst r1, #0x20                           @ see if Thumb mode is set
  bne 1f                                  @ if so handle it

  load_registers_arm()                    @ restore ARM registers
2:
  b arm_lookup_saved_arm

1:
  load_registers_thumb()                  @ load Thumb registers
  b arm_lookup_saved_thumb


@ Setup the mode transition work for calling an SWI.

@ Input:
@ r0: current pc

#define execute_swi_builder(mode)                                            ;\
                                                                             ;\
defsymbl(execute_swi_##mode)                                                 ;\
  save_flags()                                                               ;\
  add r1, reg_base, #REGMODE_RAM_OFF      /* r1 = reg_mode                 */;\
  /* REG_MODE(MODE_SUPERVISOR)[6] = pc                                     */;\
  ldr r0, [lr]                            /* load PC                       */;\
  str r0, [r1, #((SUPERVISOR_OFFSET * (7 * 4)) + (6 * 4))]                   ;\
  collapse_flags_no_update(r0)            /* r0 = cpsr                     */;\
  add r1, reg_base, #SPSR_RAM_OFF         /* r1 = spsr                     */;\
  str r0, [r1, #(SUPERVISOR_OFFSET * 4)]  /* spsr[MODE_SUPERVISOR] = cpsr  */;\
  bic r0, r0, #0x3F                       /* clear mode flag in r0         */;\
  orr r0, r0, #(0x13 | 0x80)              /* supervisor mode + disable IRQ */;\
  str r0, [reg_base, #REG_CPSR]           /* update cpsr                   */;\
                                                                             ;\
  mov r0, #MODE_SUPERVISOR                                                   ;\
                                                                             ;\
  store_registers_##mode()                /* store regs for mode           */;\
  call_c_function(set_cpu_mode)           /* set the CPU mode to svsr      */;\
  load_registers_arm()                    /* load ARM regs                 */;\
  ldr r0, =0xe3a02004                     /* Update open BUS value         */;\
  str r0, [reg_base, #REG_BUS_VALUE]                                         ;\
                                                                             ;\
  restore_flags()                                                            ;\
  add pc, lr, #4                          /* return                        */;\

execute_swi_builder(arm)
execute_swi_builder(thumb)


@ Start program execution. Normally the mode should be Thumb and the
@ PC should be 0x8000000, however if a save state is preloaded this
@ will be different.

@ Input:
@ r0: initial value for cycle counter

@ Uses sp as reg_base; must hold consistently true.

defsymbl(execute_arm_translate_internal)

  @ save the registers to be able to return later
  stmdb sp!, { r4, r5, r6, r7, r8, r9, r10, r11, r12, lr }

  mov reg_base, r1                        @ init base_reg

  mvn reg_cycles, r0                      @ load cycle counter

#ifdef NSPIRE_BUILD
  mov r1, #0                              @ core-16: no pure idle loop armed
  str r1, [reg_base, #REG_IDLE_ARMED]
#endif

  @ Check whether the CPU is sleeping already, we should just wait for IRQs
  ldr r1, [reg_base, #CPU_HALT_STATE]
  cmp r1, #0
  bne cpu_sleep_loop
  b lookup_pc


@ Epilogue to return to the main thread (whatever called execute_arm_translate)
@ (arm_return_to_main: block_lookup_address_* returns it to stop there)

defsymbl(arm_return_to_main)
return_to_main:
  @ restore the saved regs and return
  ldmia sp!, { r4, r5, r6, r7, r8, r9, r10, r11, r12, lr }
  bx lr

@ Prepares stores for external write function (align + zero extend value)
#define store_align_8()   and r1, r1, #0xff
#define store_align_16()  bic r0, r0, #0x01; extract_u16(r1, r1)
#define store_align_32()  bic r0, r0, #0x03

#define mask_addr_8(nbits)                                                   ;\
  mov r0, r0, lsl #(32 - nbits)            /* isolate bottom n bits in top */;\
  mov r0, r0, lsr #(32 - nbits)            /* high bits are now clear      */;\

#define mask_addr_16(nbits)                                                  ;\
  mov r0, r0, lsl #(32 - nbits)            /* isolate bottom n bits in top */;\
  mov r0, r0, lsr #(32 - nbits + 1)        /* high bits are now clear      */;\
  mov r0, r0, lsl #1                       /* LSB is also zero             */;\

#define mask_addr_32(nbits)                                                  ;\
  mov r0, r0, lsl #(32 - nbits)            /* isolate bottom n bits in top */;\
  mov r0, r0, lsr #(32 - nbits + 2)        /* high bits are now clear      */;\
  mov r0, r0, lsl #2                       /* 2 LSB are also zero          */;\

@ Vram, OAM and palette memories can only be accessed at a 16 bit boundary
#define mask_addr_bus16_32(nbits) mask_addr_32(nbits)
#define mask_addr_bus16_16(nbits) mask_addr_16(nbits)
#define mask_addr_bus16_8(nbits)  mask_addr_16(nbits)

#define dup8(reg)  bic r1, r1, #0xff00; orr r1, r1, lsl #8;
#define dup16(reg)
#define dup32(reg)

@ Write out to memory.

@ Input:
@ r0: address
@ r1: value
@
@ The call's PC (the next instruction's: SMC resumes there) is the word at
@ LR (return past it) or, Nspire since core-17, nspire_mem_pc's (mem_pc_r2).

@ speed10b: a 16-bit store to IE, IF or IME takes nspire_io_store16_fast
@ (at the hot stretch's end) instead of write_io_register16: one branch in
@ save_flags()'s place, so the stubs after keep their places.
#define io_store_entry_8()  save_flags()
#ifdef NSPIRE_BUILD
#define io_store_entry_16() b nspire_io_store16_fast
#else
#define io_store_entry_16() save_flags()
#endif
#define io_store_entry_32() save_flags()

#define execute_store_builder(store_type, str_op, str_op16, load_op)         ;\
                                                                             ;\
ext_store_u##store_type:                                                     ;\
  save_flags()                                                               ;\
  unread_pc()                             /* PC (or not: see unread_pc)    */;\
  store_align_##store_type()                                                 ;\
  call_c_function(write_memory##store_type)                                  ;\
  restore_flags()                                                            ;\
  mem_return()                            /* return                        */;\
                                                                             ;\
ext_io_store_u##store_type:                                                  ;\
  io_store_entry_##store_type()           /* save_flags(), or u16's fast way */;\
ext_io_store_c_u##store_type:                                                ;\
  unread_pc()                             /* PC (or not: see unread_pc)    */;\
                                                                             ;\
  call_c_function(write_io_register##store_type)                             ;\
                                                                             ;\
  cmp r0, #0                                                                 ;\
  bne io_store_alert                      /* handle additional write stuff */;\
  restore_flags()                                                            ;\
  mem_return()                            /* return                        */;\
                                                                             ;\
ext_store_iwram_u##store_type:                                               ;\
  save_flags()                                                               ;\
  mask_addr_##store_type(15)              /* Mask to mirror memory (+align)*/;\
  add r2, reg_base, #(IWRAM_OFF+0x8000)   /* r2 = iwram base               */;\
  str_op r1, [r0, r2]                     /* store data                    */;\
  sub r2, r2, #0x8000                     /* r2 = iwram smc base           */;\
  load_op r1, [r0, r2]                    /* r1 = SMC sentinel             */;\
  cmp r1, #0                              /* Check value, should be zero   */;\
  bne 3f                                  /* if so perform smc write       */;\
  restore_flags()                                                            ;\
  mem_return()                            /* return                        */;\
                                                                             ;\
ext_store_ewram_u##store_type:                                               ;\
  save_flags()                                                               ;\
  mask_addr_##store_type(18)              /* Mask to mirror memory (+align)*/;\
  add r2, reg_base, #EWRAM_OFF            /* r2 = ewram base               */;\
  str_op r1, [r0, r2]                     /* store data                    */;\
  add r2, r2, #0x40000                    /* r2 = ewram smc base           */;\
  load_op r1, [r0, r2]                    /* r1 = SMC sentinel             */;\
  cmp r1, #0                              /* Check value, should be zero   */;\
  bne 3f                                  /* if so perform smc write       */;\
  restore_flags()                                                            ;\
  mem_return()                            /* return                        */;\
                                                                             ;\
ext_store_vram_u##store_type:                                                ;\
  watch_vram(store_type)                  /* speed10 (in save_flags' place)*/;\
  mask_addr_bus16_##store_type(17)        /* Mask to mirror memory (+align)*/;\
  dup##store_type(r1)                     /* Duplicate byte if necessary   */;\
  cmp r0, #0x18000                        /* Check if exceeds 96KB         */;\
  subcs r0, r0, #0x8000                   /* Mirror to the last bank       */;\
  add r2, reg_base, #VRAM_OFF             /* r2 = vram base                */;\
  restore_flags()                                                            ;\
  str_op16 r1, [r0, r2]                   /* store data                    */;\
  mem_return()                            /* return                        */;\
                                                                             ;\
ext_store_oam_ram_u##store_type:                                             ;\
  mask_addr_bus16_##store_type(10)        /* Mask to mirror memory (+align)*/;\
  dup##store_type(r1)                     /* Duplicate byte if necessary   */;\
  add r2, reg_base, #OAM_RAM_OFF          /* r2 = oam ram base             */;\
  str_op16 r1, [r0, r2]                   /* store data                    */;\
  str r2, [reg_base, #OAM_UPDATED]        /* write non zero to signal      */;\
  mem_return()                            /* return                        */;\
                                                                             ;\
3: /* Flush RAM cache and "resume" execution via re-compile */               ;\
  mem_pc_r2()                             /* load PC (before the flush)    */;\
  str r2, [reg_base, #REG_PC]             /* write out PC                  */;\
  call_c_function(ram_flush_smc)          /* (speed10b: Nspire's keeps)    */;\
  b resume_pc                             /* continue execution            */;\

@ for ignored areas, just return (Nspire since core-17 also right for the
@ u32_safe calls, which have no word after them: past it skipped an
@ instruction). The u32_safe tables' entry is ext_store_ignore_u32_safe:
@ in the PCWORD_CHECK build (and other ARM builds) the other stores return
@ past their word, a u32_safe call has none (before 2026-11-15 the check
@ build skipped a tail's "b join" there; tests/arm/rom/ram_code.S step 13).
ext_store_ignore:
  mem_return()                            @ return
#if defined(NSPIRE_BUILD) && !defined(NSPIRE_PCWORD_CHECK)
.set ext_store_ignore_u32_safe, ext_store_ignore
#else
ext_store_ignore_u32_safe:
  bx lr
#endif
.set ext_store_ignore_u8,  ext_store_ignore
.set ext_store_ignore_u16, ext_store_ignore
.set ext_store_ignore_u32, ext_store_ignore

#define store_lookup_table(store_type)                                       ;\
  .word ext_store_ignore_u##store_type    /*   -1: ignore, for > 0x0F      */;\
  .word ext_store_ignore_u##store_type    /* 0x00: BIOS, ignore            */;\
  .word ext_store_ignore_u##store_type    /* 0x01: ignore                  */;\
  .word ext_store_ewram_u##store_type     /* 0x02: ewram                   */;\
  .word ext_store_iwram_u##store_type     /* 0x03: iwram                   */;\
  .word ext_io_store_u##store_type        /* 0x04: I/O regs                */;\
  .word ext_store_palette_u##store_type   /* 0x05: palette RAM             */;\
  .word ext_store_vram_u##store_type      /* 0x06: vram                    */;\
  .word ext_store_oam_ram_u##store_type   /* 0x07: oam ram                 */;\
  .word ext_store_u##store_type           /* 0x08: gamepak: ignore         */;\
  .word ext_store_ignore_u##store_type    /* 0x09: gamepak: ignore         */;\
  .word ext_store_ignore_u##store_type    /* 0x0A: gamepak: ignore         */;\
  .word ext_store_ignore_u##store_type    /* 0x0B: gamepak: ignore         */;\
  .word ext_store_ignore_u##store_type    /* 0x0C: gamepak: ignore         */;\
  .word ext_store_u##store_type           /* 0x0D: EEPROM                  */;\
  .word ext_store_u##store_type           /* 0x0E: backup                  */;\
  .word ext_store_ignore_u##store_type    /* 0x0F: ignore                  */;\

execute_store_builder(8,  strb, strh, ldrb)
execute_store_builder(16, strh, strh, ldrh)
execute_store_builder(32, str,  str,  ldr )

@ Palette writes are special since they are converted on the fly for speed

@ speed10: on the Nspire each of these handlers' first instruction is a
@ branch to its _watch form at the hot stretch's end (which counts the
@ changes), in the place of the instruction it replaces: the code around
@ keeps its places (nspire_lineio.ld). The rest of each
@ is not run there (elsewhere the handlers as they were: watch_*).
ext_store_palette_u8:
  watch_palette_u8()                      // speed10 (in the bic's place)
  orr r1, r1, lsl #8
ext_store_palette_u16:
  watch_palette(16)                       // speed10 (in the mask's first place)
  mov r0, r0, lsr #(32 - 10 + 1)
  mov r0, r0, lsl #1
  add r2, reg_base, #PAL_RAM_OFF          // r2 = palette base
  strh r1, [r0, r2]                       // store data

  and r2, r1, #0x3E0                      // Convert color point
  lsl r2, r2, #1
  orr r2, r1, lsl #11
  and r1, r1, #0x7C00
  orr r2, r1, lsr #10

  add r1, reg_base, #PAL_CONV_OFF         // r1 = converted palette ram
  strh r2, [r0, r1]                       // Converted value write (r2)
  mem_return()                            // return

ext_store_palette_u32_safe:
  palette_safe_lr()
ext_store_palette_u32:
  watch_palette(32)                       // speed10 (in the mask's first place)
  mov r0, r0, lsr #(32 - 10 + 2)
  mov r0, r0, lsl #2
  add r2, reg_base, #PAL_RAM_OFF          // r2 = palette base
  str r1, [r0, r2]                        // store data
  add r2, reg_base, #PAL_CONV_OFF         // r2 = converted palette ram

  lsr r9, r1, #10
  and r9, r9, #0x1F
  orr r9, r1, lsl #11
  bic r1, r1, #0x1F
  bic r1, r1, #0xFC00
  orr r9, r1, lsl #1
  strh r9, [r0, r2]                       // Write first halfword
  add r0, r0, #2

  lsr r1, r1, #16
  and r9, r1, #0x3E0
  lsl r9, r9, #1
  orr r9, r1, lsl #11
  and r1, r1, #0x7C00
  orr r9, r1, lsr #10
  strh r9, [r0, r2]                       // Write second halfword

  mem_return()                            // return


#ifdef NSPIRE_BUILD
@ Stores to palette, VRAM and OAM while picture lines wait to be drawn
@ (video.h): draw them first, then store as usual. update_gba's HBlank puts
@ these in entries 5-7 of mem_dispatch's store tables (nspire_mem_tables,
@ nspire_lines.c; since core-10) and drawing puts the usual handlers back,
@ so stores pay nothing while no lines wait.
@ r0: address, r1: value, lr: the return (kept for the handler).

#define video_flush_store(handler)                                           ;\
handler##_flush:                                                             ;\
  save_flags()                                                               ;\
  stmdb sp!, { r0, r1, r2, r3, r12, lr }                                     ;\
  bl nspire_flush_lines                                                      ;\
  ldmia sp!, { r0, r1, r2, r3, r12, lr }                                     ;\
  restore_flags()                                                            ;\
  b handler                                                                  ;\

#define video_flush_stores(store_type)                                       ;\
video_flush_store(ext_store_palette_u##store_type)                           ;\
video_flush_store(ext_store_vram_u##store_type)                              ;\
video_flush_store(ext_store_oam_ram_u##store_type)                           ;\

video_flush_stores(8)
video_flush_stores(16)
video_flush_stores(32)
video_flush_stores(32_safe)
#endif

@ This is a store that is executed in a strm case (so no SMC checks in-between)

ext_store_u32_safe:
ext_io_store_u32_safe:
  save_flags()
  call_c_function(write_memory32)         @ Perform 32bit store
  restore_flags()
  bx lr                                   @ Return

ext_store_iwram_u32_safe:
  mask_addr_8(15)                         @ Mask to mirror memory (no need to align!)
  add r2, reg_base, #(IWRAM_OFF+0x8000)   @ r2 = iwram base
  str r1, [r0, r2]                        @ store data
  bx lr                                   @ Return

ext_store_ewram_u32_safe:
  mask_addr_8(18)                         @ Mask to mirror memory (no need to align!)
  add r2, reg_base, #EWRAM_OFF            @ r2 = ewram base
  str r1, [r0, r2]                        @ store data
  bx lr                                   @ Return

ext_store_vram_u32_safe:
  watch_vram_u32_safe()                   @ speed10 (in the mask's first place)
  mov r0, r0, lsr #(32 - 17)
  save_flags()
  add r2, reg_base, #VRAM_OFF             @ r2 = vram base
  cmp r0, #0x18000                        @ Check if exceeds 96KB
  subcs r0, r0, #0x8000                   @ Mirror to the last bank
  str r1, [r0, r2]                        @ store data
  restore_flags()
  bx lr                                   @ Return

ext_store_oam_ram_u32_safe:
  mask_addr_8(10)                         @ Mask to mirror memory (no need to align!)
  add r2, reg_base, #OAM_RAM_OFF          @ r2 = oam ram base
  str r1, [r0, r2]                        @ store data
  str r2, [reg_base, #OAM_UPDATED]        @ store anything non zero here
  bx lr                                   @ Return


#ifdef NSPIRE_BUILD
io_store_alert:                           @ r0: the alerts (kept)
  mem_pc_r2()
  str r2, [reg_base, #REG_PC]             @ write out PC
#endif
write_epilogue:
  ldr r2, [reg_base, #REG_CPSR]           @ Save all register and CPSR
  tst r2, #0x20                           @ Check thumb bit
  store_registers_cond()                  @ Store ARM/Thumb regs
  collapse_flags(r1)                      @ interrupt needs current flags

  mov r2, r0                              @ r2 is stored across C calls
  tst r2, #CPU_ALERT_SMC                  @ check for SMC code
  beq 1f
  call_c_function(ram_flush_smc)          @ Flush RAM if bit is set

1:
  tst r2, #CPU_ALERT_IRQ                  @ check for IRQs
  beq 2f
  call_c_function(check_and_raise_interrupts)    @ Update CPU state to raise IRQ

2:
  tst r2, #CPU_ALERT_HALT                 @ check for CPU halt bit
  beq lookup_pc                           @ Resume execution if not halted

  @ Fallthrough to cpu_sleep_loop on purpose (CPU is now halted)

  mvn r0, reg_cycles                      @ setup for update_gba

cpu_sleep_loop:
  call_c_function(update_gba)             @ update GBA until CPU isn't halted

  cmp r0, #0
  blt return_to_main                      @ New frame if bit 31 is set. Exit

  bic r0, r0, #0xF0000000                 @ clear MSB, not part of count

  @ The cpu is active again, go ahead and resume execution at current PC
  mvn reg_cycles, r0                      @ load new cycle count
  ldr r0, [reg_base, #REG_PC]             @ load new PC
  ldr r1, [reg_base, #REG_CPSR]           @ r1 = flags
  tst r1, #0x20                           @ see if Thumb bit is set
  bne 2f

  cached_lookup(arm)
  load_registers_arm()
  extract_flags()
  bx r0                                   @ jump to new ARM block

2:
  cached_lookup(thumb)
  load_registers_thumb()
  extract_flags()
  bx r0                                   @ jump to new Thumb block
.ltorg


resume_pc:
  @ Resume regular execution (except we might need to recompile due to flush)
  @ assume flags are spilled to reg_flags
  ldr r0, [reg_base, #REG_PC]             @ r0 = new pc
  ldr r1, [reg_base, #REG_CPSR]           @ r1 = partial flags valid
  tst r1, #0x20                           @ see if Thumb bit is set
  beq 1f                                  @ if not lookup ARM

  call_c_function(block_lookup_address_thumb)
  restore_flags()
  bx r0                                   @ jump to new Thumb block
1:
  call_c_function(block_lookup_address_arm)
  restore_flags()
  bx r0                                   @ jump to new ARM block

lookup_pc:
  @ Restart CPU execution, assumes CPU mode might have changed
  ldr r0, [reg_base, #REG_PC]             @ r0 = new pc
  ldr r1, [reg_base, #REG_CPSR]           @ r1 = flags
  tst r1, #0x20                           @ see if Thumb bit is set
  beq 2f                                  @ if not lookup ARM

  call_c_function(block_lookup_address_thumb)
  load_registers_thumb()
  extract_flags()
  bx r0                                   @ jump to new Thumb block
2:
  call_c_function(block_lookup_address_arm)
  load_registers_arm()
  extract_flags()
  bx r0                                   @ jump to new ARM block


#define exec_ld_op_s8(mirrorbits)                                            ;\
  mov r0, r0, lsl #(32 - mirrorbits)                                         ;\
  mov r0, r0, lsr #(32 - mirrorbits)                                         ;\
  ldrsb r0, [r2, r0]

#define exec_ld_op_u8(mirrorbits)                                            ;\
  mov r0, r0, lsl #(32 - mirrorbits)                                         ;\
  mov r0, r0, lsr #(32 - mirrorbits)                                         ;\
  ldrb r0, [r2, r0]

#define exec_ld_op_s16(mirrorbits)                                           ;\
  mov r0, r0, lsl #(32 - mirrorbits)                                         ;\
  mov r0, r0, lsr #(32 - mirrorbits)                                         ;\
  ldrsh r0, [r2, r0]

#define exec_ld_op_u16(mirrorbits)                                           ;\
  mov r0, r0, lsl #(32 - mirrorbits)                                         ;\
  mov r0, r0, lsr #(32 - mirrorbits)                                         ;\
  ldrh r0, [r2, r0]

#define exec_ld_op_u32(mirrorbits)                                           ;\
  mov r0, r0, lsl #(32 - mirrorbits)                                         ;\
  ldr r0, [r2, r0, lsr #(32 - mirrorbits)]                                   ;\


#define execute_load_builder(load_type, albits, load_function)               ;\
                                                                             ;\
ld_bios_##load_type:                      /* BIOS area, need to verify PC  */;\
  save_flags()                                                               ;\
  mem_pc_r2()                                                                ;\
  mov r1, r2                              /* r1 = PC                       */;\
  mov r2, r1, lsr #15                     /* r2 = High addr bits from PC   */;\
  cmp r2, #0                                                                 ;\
  bne 10f                                 /* Jump to slow handler          */;\
  ldr r2, [reg_base, #RDMAP_OFF]          /* r2 = read_mem[0]              */;\
  exec_ld_op_##load_type(15)              /* Clear upper bits (15 LSB)     */;\
  restore_flags()                                                            ;\
  mem_return()                                                               ;\
                                                                             ;\
ld_ewram_##load_type:                     /* EWRAM area                    */;\
  add r2, reg_base, #EWRAM_OFF                                               ;\
  exec_ld_op_##load_type(18)              /* Clear upper bits (18 LSB)     */;\
  mem_return()                                                               ;\
                                                                             ;\
ld_iwram_##load_type:                     /* IWRAM area                    */;\
  add r2, reg_base, #(IWRAM_OFF+0x8000)                                      ;\
  exec_ld_op_##load_type(15)              /* Clear upper bits (15 LSB)     */;\
  mem_return()                                                               ;\
                                                                             ;\
ld_ioram_##load_type:                     /* I/O RAM area                  */;\
  add r2, reg_base, #IOREG_OFF                                               ;\
  exec_ld_op_##load_type(10)              /* Clear upper bits (10 LSB)     */;\
  mem_return()                                                               ;\
                                                                             ;\
ld_palram_##load_type:                    /* Palette RAM area              */;\
  add r2, reg_base, #PAL_RAM_OFF                                             ;\
  exec_ld_op_##load_type(10)              /* Clear upper bits (10 LSB)     */;\
  mem_return()                                                               ;\
                                                                             ;\
ld_oamram_##load_type:                    /* OAM RAM area                  */;\
  add r2, reg_base, #OAM_RAM_OFF                                             ;\
  exec_ld_op_##load_type(10)              /* Clear upper bits (10 LSB)     */;\
  mem_return()                                                               ;\
                                                                             ;\
/* ROM area: uses generic memory handlers to handle swapping */              ;\
ld_rdmap_slow_##load_type:                                                   ;\
  save_flags()                                                               ;\
  add r2, reg_base, #RDMAP_OFF            /* r2 = memory_map_read          */;\
  mov r1, r0, lsr #15                     /* r1 = page index of address    */;\
  ldr r2, [r2, r1, lsl #2]                /* r2 = base addr                */;\
  cmp r2, #0                                                                 ;\
  beq 9f                                  /* Page miss, go slow            */;\
                                                                             ;\
  exec_ld_op_##load_type(15)              /* Pages are 32KB big            */;\
  restore_flags()                                                            ;\
  mem_return()                                                               ;\
                                                                             ;\
/* ROM/VRAM area: uses generic memory handlers, assumes is mapped */         ;\
ld_rdmap_##load_type:                                                        ;\
  add r2, reg_base, #RDMAP_OFF            /* r2 = memory_map_read          */;\
  mov r1, r0, lsr #15                     /* r1 = page index of address    */;\
  ldr r2, [r2, r1, lsl #2]                /* r2 = base addr                */;\
                                                                             ;\
  exec_ld_op_##load_type(15)              /* Pages are 32KB big            */;\
  mem_return()                                                               ;\
                                                                             ;\
/* Slow load path, for open/unmapped loads                                 */;\
ld_slow_##load_type:                                                         ;\
  save_flags()                                                               ;\
9:                                                                           ;\
  mem_pc_r2()                                                                ;\
  mov r1, r2                              /* r1 = PC                       */;\
10:                                                                          ;\
  str r1, [reg_base, #REG_PC]             /* update PC                     */;\
  call_c_function(load_function)                                             ;\
  restore_flags()                                                            ;\
  mem_return()                            /* return                        */;\


#define load_table_gen(load_type, rdmapfn)                                   ;\
  .long ld_slow_##load_type               /* -1 (for regions above F)      */;\
  .long ld_bios_##load_type               /* 0 BIOS                        */;\
  .long ld_slow_##load_type               /* 1 Bad region                  */;\
  .long ld_ewram_##load_type              /* 2 EWRAM                       */;\
  .long ld_iwram_##load_type              /* 3 IWRAM                       */;\
  .long ld_ioram_##load_type              /* 4 I/O                         */;\
  .long ld_palram_##load_type             /* 5 Palette RAM, via map        */;\
  .long ld_rdmap_##load_type              /* 6 VRAM area                   */;\
  .long ld_oamram_##load_type             /* 7 OAM RAM                     */;\
  .long ld_##rdmapfn##_##load_type        /* 8 ROM, via map                */;\
  .long ld_##rdmapfn##_##load_type        /* 9 ROM, via map                */;\
  .long ld_##rdmapfn##_##load_type        /* A ROM, via map                */;\
  .long ld_##rdmapfn##_##load_type        /* B ROM, via map                */;\
  .long ld_##rdmapfn##_##load_type        /* C ROM, via map                */;\
  .long ld_slow_##load_type               /* D ROM or EEPROM/FLASH         */;\
  .long ld_slow_##load_type               /* E EEPROM/FLASH                */;\
  .long ld_slow_##load_type               /* F Bad region                  */;\

.pool

execute_load_builder(u8,  0, read_memory8  )
execute_load_builder(s8,  0, read_memory8s )
execute_load_builder(u16, 1, read_memory16 )
execute_load_builder(s16, 1, read_memory16s)
execute_load_builder(u32, 2, read_memory32 )

@ Memory access dispatch, shared by all translated code (Nspire): the code
@ calls one of these with "bl" instead of having the region lookup inline,
@ which made a third of the translated code and cost instruction cache
@ misses (16 KB on the ARM926). Same math as mem_calc_region in arm_emit.h
@ (flags untouched; the non-Nspire version uses r9 as scratch: the handlers
@ clobber it anyway), then on
@ to the region's handler with lr still pointing at the PC word (Nspire
@ since core-17: at the next instruction; mem_pc_r2 finds the PC).
@ r0: address (r1: value for stores)

#ifdef NSPIRE_BUILD
@ Each kind has its own 256-word table indexed by the whole top byte (as
@ mem_calc_region builds it: unaligned u16/u32 addresses set bit 7, or bits
@ 7:6, of it), so no clamp: entries 0x10-0xFF hold what the 17-entry tables'
@ "-1" entry holds (init_emitter fills them). The tables sit below at
@ reg_base + MEMTBL_*, each a multiple of 256 bytes so the word index base
@ is an ARM immediate. Their hot first 64 bytes (regions 0x00-0x0F) fall at
@ these offsets in the data cache's 2 KB way: the busiest loads at 0x200 and
@ 0x300, where only the old 17-entry tables were (no other hot data of the
@ reg page there), load_s16 with reg's r0-r15 at 0x000, the stores at
@ 0x100 (spsr), 0x400 (reg_mode), 0x600 (timers), 0x700 (IE/IF). Tried
@ (model, Advance Wars cx2, 4 placements): all eight slots one table each,
@ load_u16 and store_u32_safe together at 0x100: game 6.71 ms; this: 6.44.
#define MEMTBL_ST_U8          0x9500
#define MEMTBL_LD_U32         0x9A00
#define MEMTBL_ST_U32         0x9E00
#define MEMTBL_LD_U16         0xA300
#define MEMTBL_LD_S16         0xA800
#define MEMTBL_ST_U16         0xAC00
#define MEMTBL_LD_S8          0xB200
#define MEMTBL_ST_U32_SAFE    0xB700
#define MEMTBL_LD_U8          0xBB00

#define mem_dispatch(name, tbloff, abits)                                    ;\
defsymbl(name)                                                               ;\
  .if abits                                                                  ;\
  orr r2, r0, r0, lsl #(32 - abits)                                          ;\
  mov r2, r2, lsr #24                                                        ;\
  .else                                                                      ;\
  mov r2, r0, lsr #24                                                        ;\
  .endif                                                                     ;\
  add r2, r2, #((tbloff) >> 2)                                               ;\
  ldr pc, [reg_base, r2, lsl #2]                                             ;\

mem_dispatch(mem_dispatch_store_u8,      MEMTBL_ST_U8,       0)
mem_dispatch(mem_dispatch_store_u16,     MEMTBL_ST_U16,      0)
mem_dispatch(mem_dispatch_store_u32,     MEMTBL_ST_U32,      0)
mem_dispatch(mem_dispatch_store_u32_safe, MEMTBL_ST_U32_SAFE, 0)
mem_dispatch(mem_dispatch_load_u8,       MEMTBL_LD_U8,       0)
mem_dispatch(mem_dispatch_load_s8,       MEMTBL_LD_S8,       0)
mem_dispatch(mem_dispatch_load_u16,      MEMTBL_LD_U16,      1)
mem_dispatch(mem_dispatch_load_s16,      MEMTBL_LD_S16,      1)
mem_dispatch(mem_dispatch_load_u32,      MEMTBL_LD_U32,      2)
#else
#define mem_dispatch(name, tblnum, abits)                                  ;defsymbl(name)                                                               ;  .if abits                                                                  ;  orr r2, r0, r0, lsl #(32 - abits)                                          ;  mov r2, r2, lsr #24                                                        ;  .else                                                                      ;  mov r2, r0, lsr #24                                                        ;  .endif                                                                     ;  rsb r9, r2, #15                                                            ;  orr r2, r2, r9, asr #31                                                    ;  add r2, r2, #((STORE_TBL_OFF + 68 * tblnum + 4) >> 2)                      ;  ldr pc, [reg_base, r2, lsl #2]                                             ;
mem_dispatch(mem_dispatch_store_u8,      0, 0)
mem_dispatch(mem_dispatch_store_u16,     1, 0)
mem_dispatch(mem_dispatch_store_u32,     2, 0)
mem_dispatch(mem_dispatch_store_u32_safe, 3, 0)
mem_dispatch(mem_dispatch_load_u8,       4, 0)
mem_dispatch(mem_dispatch_load_s8,       5, 0)
mem_dispatch(mem_dispatch_load_u16,      6, 1)
mem_dispatch(mem_dispatch_load_s16,      7, 1)
mem_dispatch(mem_dispatch_load_u32,      8, 2)
#endif

#ifdef NSPIRE_BUILD
@ core-16 (2026-11-15): the update of a proven-pure idle loop (arm_emit.h
@ nspire_idle_body_pure), emitted as
@   bl arm_update_gba_pure_thumb ; .word loop PC ; b loop
@ and on the loop's exit "mov r0, #0 ; str r0, [reg_base, #REG_IDLE_ARMED]".
@ As arm_update_gba_idle_thumb (update_gba(0): an idle call uses all of
@ execute_cycles whatever the body cost), and: ARMED means this stub went
@ back into the body last (no PC change since), TAINT that one of the
@ body's loads since its last call wasn't plain memory (the body's load
@ calls go to mem_dispatch_load_*_pure below). Armed and untainted, the body
@ just ran once on its own from its start on what memory holds now and gave
@ these registers and flags; until memory changes it gives them again, so
@ update_gba runs the next events itself while each changes nothing the
@ body reads (NSPIRE_IDLE_PURE_CALL; REG_IDLE_OK: armed and untainted, for
@ tests/arm IDLE_CHECK=1's update_gba).
defsymbl(arm_update_gba_pure_thumb)
  ldr r0, [lr]                            @ the loop's PC
  str r0, [reg_base, #REG_PC]
  save_flags()
  collapse_flags(r0)
  store_registers_thumb()
  ldr r0, [reg_base, #REG_IDLE_ARMED]
  ldr r1, [reg_base, #REG_IDLE_TAINT]
  cmp r1, #0
  movne r0, #0
  str r0, [reg_base, #REG_IDLE_OK]        @ armed and untainted
  @ update_gba(0), or armed and untainted: it runs the next events too
  @ (main.c; the check build: always 0)
#ifdef NSPIRE_IDLE_CHECK
  mov r0, #0
#else
  cmp r0, #0
  movne r0, #NSPIRE_IDLE_PURE_CALL
#endif
  mov r1, #0
  str r1, [reg_base, #REG_IDLE_TAINT]
  call_c_function(update_gba)
  mov r1, #0
  str r1, [reg_base, #REG_IDLE_OK]
  str r1, [reg_base, #REG_IDLE_ARMED]
  cmp r0, #0                              @ frame done
  blt return_to_main
  bic reg_cycles, r0, #0xF0000000
  mvn reg_cycles, reg_cycles
  tst r0, #0x40000000                     @ PC changed
  bne 1f
  str reg_base, [reg_base, #REG_IDLE_ARMED]  @ back into the body (never 0)
  load_registers_thumb()
  restore_flags()
  add pc, lr, #4
1:
  ldr r1, [reg_base, #REG_CPSR]
  ldr r0, [reg_base, #REG_PC]
  tst r1, #0x20
  bne 2f
  load_registers_arm()
  b arm_lookup_saved_arm
2:
  load_registers_thumb()
  b arm_lookup_saved_thumb
.size arm_update_gba_pure_thumb, .-arm_update_gba_pure_thumb

@ A pure idle loop's loads (arm_emit.h nspire_idle_patch_loads points its
@ body's "bl mem_dispatch_load_*" here): plain memory is EWRAM, IWRAM,
@ palette, VRAM and OAM at an aligned address (mem_dispatch's index 2, 3,
@ 5, 6 or 7); anything else (I/O, BIOS, ROM, save memory, open bus,
@ unaligned) marks REG_IDLE_TAINT (reg_base: never 0). Then on to the
@ usual dispatch, lr as it was; flags kept, r1 and r2 clobbered.
#define pure_load(name, target, abits)                                       ;\
defsymbl(name)                                                               ;\
  mrs r2, cpsr                                                               ;\
  .if abits                                                                  ;\
  orr r1, r0, r0, lsl #(32 - abits)                                          ;\
  mov r1, r1, lsr #24                                                        ;\
  .else                                                                      ;\
  mov r1, r0, lsr #24                                                        ;\
  .endif                                                                     ;\
  sub r1, r1, #2                                                             ;\
  cmp r1, #1                              /* EWRAM, IWRAM                  */;\
  bls 1f                                                                     ;\
  sub r1, r1, #3                                                             ;\
  cmp r1, #2                              /* palette, VRAM, OAM            */;\
  strhi reg_base, [reg_base, #REG_IDLE_TAINT]                                ;\
1:                                                                           ;\
  msr cpsr_f, r2                                                             ;\
  b target                                                                   ;\

pure_load(mem_dispatch_load_u8_pure,  mem_dispatch_load_u8,  0)
pure_load(mem_dispatch_load_s8_pure,  mem_dispatch_load_s8,  0)
pure_load(mem_dispatch_load_u16_pure, mem_dispatch_load_u16, 1)
pure_load(mem_dispatch_load_s16_pure, mem_dispatch_load_s16, 1)
pure_load(mem_dispatch_load_u32_pure, mem_dispatch_load_u32, 2)
#endif

#ifdef NSPIRE_BUILD
@ cached_lookup's keys below 0x04000000 (2026-11-17, speed9b): r1 = the key,
@ lr = its probe (the eor; its beq at +28, 5 (C) at +32). The BIOS's go
@ back to the probe. A RAM block (IWRAM or EWRAM) that is there is what
@ block_lookup_translate_MODE (cpu_threaded.c) returns without
@ translating, here instead of in C (interrupt handlers and their returns
@ in IWRAM: 498 lookups a frame in Advance Wars, 396 in Fire Emblem): the
@ PC's halfword tag (iwram[pc & 0x7FFE], ewram[0x40000 + (pc & 0x3FFFE)]),
@ when above LAST_TAG_NUM (0x0101), indexes the ramtag_type table that ends
@ the RAM cache: entry (s16)tag >> 1 (tags are odd: its byte offset is
@ ((s16)tag << 2) - 4), word offset_arm or offset_thumb; when that is not
@ 0 the block is ram_translation_cache + it: back at the beq with Z set and
@ r0 the block. Anything else (no tag, code not a block's start, not
@ translated in this mode) goes to 5, C as before (it allocates and
@ translates; r0 kept). Every translation syncs the caches in C before its
@ block runs, so a block found here needs no translate_icache_sync. r1 and
@ r2 clobbered, the flags too.
#define ram_lookup_builder(mode, table)                                      ;\
defsymbl(nspire_ram_lookup_##mode)                                           ;\
  cmp r1, #0x02000000                                                        ;\
  bxlo lr                                 /* the BIOS: the cache           */;\
  tst r1, #0x01000000                     /* 3: IWRAM, 2: EWRAM            */;\
  movne r2, r1, lsl #17                                                      ;\
  movne r2, r2, lsr #17                   /* pc & 0x7FFF                   */;\
  subne r2, r2, #(-IWRAM_OFF)             /* its tag: iwram[0..0x7FFF]     */;\
  moveq r2, r1, lsl #14                                                      ;\
  moveq r2, r2, lsr #14                   /* pc & 0x3FFFF                  */;\
  subeq r2, r2, #(-(EWRAM_OFF + 0x40000)) /* its tag: ewram[0x40000..]     */;\
  bic r2, r2, #1                          /* the Thumb bit of the key      */;\
  ldrh r2, [reg_base, r2]                                                    ;\
  sub r1, r2, #0x100                                                         ;\
  subs r1, r1, #2                         /* tag > 0x0101                  */;\
  addmi pc, lr, #32                                                          ;\
  mov r2, r2, lsl #16                                                        ;\
  ldr r1, =table                                                             ;\
  ldr r2, [r1, r2, asr #14]               /* offset_MODE                   */;\
  cmp r2, #0                                                                 ;\
  addeq pc, lr, #32                                                          ;\
  ldr r1, =ram_translation_cache                                             ;\
  add r0, r1, r2                                                             ;\
  cmp r0, r0                              /* Z: the probe's beq            */;\
  add pc, lr, #28                                                            ;\
.size nspire_ram_lookup_##mode, .-nspire_ram_lookup_##mode

ram_lookup_builder(arm, ram_translation_cache + RAM_TRANSLATION_CACHE_SIZE - 4)
ram_lookup_builder(thumb, ram_translation_cache + RAM_TRANSLATION_CACHE_SIZE)
.ltorg

@ Advance Wars' idle loop's branch (2026-11-18, speed9b; arm_emit.h
@ nspire_idle_call, main.c "speed9b"):
@   ldr lr, [reg_base, #(4 * (REG_USERDEF + 15))] ; blx lr ; .word loop PC
@   b loop
@ As arm_update_gba_idle_thumb (update_gba as an idle call), and: once the
@ pass's cost C is known (REG_IDLE_PASS_SEEN 2) and the state is the pass's
@ fixed point (nspire_idle_pass_fixed, below), update_gba gets core-16's
@ NSPIRE_IDLE_PURE_CALL with REG_IDLE_LIMIT = C - 1 for the call (it then
@ runs the next event itself when the window is over that); until then (and
@ always in the check build, tests/arm IDLE_CHECK=1) main.c's
@ nspire_update_gba_idle_pass measures C from the cycles really left. Here
@ at the hot stretch's end so that no stub before it moves.
#define REG_IDLE_LIMIT      (30 * 4)
#define REG_IDLE_PASS_COST  (48 * 4)
#define REG_IDLE_PASS_SEEN  (52 * 4)
defsymbl(arm_update_gba_idle_pass_thumb)
  ldr r0, [lr]                            @ the loop's PC
  str r0, [reg_base, #REG_PC]
  save_flags()
  collapse_flags(r0)
  store_registers_thumb()
#ifndef NSPIRE_IDLE_CHECK
  ldr r1, [reg_base, #REG_IDLE_PASS_SEEN]
  cmp r1, #2
  bne 3f
  call_c_function(nspire_idle_pass_fixed)
  cmp r0, #0                              @ not at the fixed point:
  beq 5f                                  @ update_gba(0), an idle call
  ldr r1, [reg_base, #REG_IDLE_PASS_COST]
  sub r1, r1, #1
  str r1, [reg_base, #REG_IDLE_LIMIT]
  mov r0, #NSPIRE_IDLE_PURE_CALL
5:
  call_c_function(update_gba)
  mov r1, #0
  str r1, [reg_base, #REG_IDLE_LIMIT]
  b 4f
#endif
3:
  mvn r0, reg_cycles                      @ the cycles left
  call_c_function(nspire_update_gba_idle_pass)
4:
  cmp r0, #0                              @ frame done
  blt return_to_main
  bic reg_cycles, r0, #0xF0000000
  mvn reg_cycles, reg_cycles
  tst r0, #0x40000000                     @ PC changed
  bne 1f
  load_registers_thumb()
  restore_flags()
  add pc, lr, #4                          @ past the PC word: b loop
1:
  ldr r1, [reg_base, #REG_CPSR]
  ldr r0, [reg_base, #REG_PC]
  tst r1, #0x20
  bne 2f
  load_registers_arm()
  b arm_lookup_saved_arm
2:
  load_registers_thumb()
  b arm_lookup_saved_thumb
.size arm_update_gba_idle_pass_thumb, .-arm_update_gba_idle_pass_thumb

@ int nspire_idle_pass_fixed(void) (C, main.c): Advance Wars' state is its
@ idle pass's fixed point (main.c "speed9b"): r0 = lr = 0x08038819, r1 = 15,
@ r2 = 0x3FF, r4 = 0x03004478, N set and Z C V clear; the callback
@ 0x080383F9 at 0x03004478 and its flag at 0x03004434 set; not A, B,
@ Select and Start all down (KEYINPUT); sp aligned in IWRAM, sp - 8 and
@ sp - 4 in one 32 KB mirror holding 0x03004478 and 0x08038819, and no code
@ there (their tags 0). 1 or 0; r0-r3, r12 and the flags clobbered.
defsymbl(nspire_idle_pass_fixed)
  ldr r3, =reg
  ldr r1, =0x08038819
  ldr r0, [r3, #REG_R0]
  cmp r0, r1
  ldreq r0, [r3, #REG_LR]
  cmpeq r0, r1
  bne 9f
  ldr r0, [r3, #REG_R1]
  cmp r0, #15
  bne 9f
  ldr r0, [r3, #REG_R2]
  ldr r1, =0x3FF
  cmp r0, r1
  bne 9f
  ldr r0, [r3, #REG_R4]
  ldr r1, =0x03004478
  cmp r0, r1
  bne 9f
  ldr r0, [r3, #REG_CPSR]
  mov r0, r0, lsr #28
  cmp r0, #8                              @ N, not Z C V
  bne 9f
  sub r2, r3, #(-(IWRAM_OFF + 0x8000))    @ IWRAM's data
  add r2, r2, #0x4400
  ldr r0, [r2, #0x78]                     @ the callback
  ldr r12, =0x080383F9
  cmp r0, r12
  bne 9f
  ldrb r0, [r2, #0x34]                    @ its flag
  cmp r0, #0
  beq 9f
  add r2, r3, #(IOREG_OFF + 0x100)
  ldrh r0, [r2, #0x30]                    @ KEYINPUT (0: down)
  tst r0, #15
  beq 9f
  ldr r0, [r3, #REG_SP]
  tst r0, #3
  bne 9f
  mov r2, r0, lsr #24
  cmp r2, #3
  bne 9f
  sub r0, r0, #8
  mov r2, r0, lsr #24
  cmp r2, #3
  bne 9f
  mov r0, r0, lsl #17
  mov r0, r0, lsr #17                     @ (sp - 8) & 0x7FFF
  add r2, r0, #4
  tst r2, #0x8000                         @ sp - 4 past the mirror's end
  bne 9f
  sub r2, r3, #(-(IWRAM_OFF + 0x8000))
  add r2, r2, r0                          @ its data
  ldr r12, [r2]
  cmp r12, r1                             @ 0x03004478
  bne 9f
  ldr r12, [r2, #4]
  ldr r1, =0x08038819
  cmp r12, r1
  bne 9f
  sub r2, r2, #0x8000                     @ their tags
  ldr r12, [r2]
  ldr r1, [r2, #4]
  orrs r12, r12, r1
  bne 9f
  mov r0, #1
  bx lr
9:
  mov r0, #0
  bx lr
.size nspire_idle_pass_fixed, .-nspire_idle_pass_fixed
.ltorg

@ speed10b (2026-11-18): MSR CPSR and the SPSR's return (subs pc, lr / ldm
@ with ^) without C for the modes a game switches between: an interrupt
@ handler's dispatcher (Advance Wars' every line: IRQ -> System, back, and the
@ BIOS's return), Fire Emblem's. They do what execute_store_cpsr_body and
@ execute_spsr_restore_body do (set_cpu_mode, then the interrupt test):
@ the new mode by cpu_modes[cpsr & 0xF] for User (0), IRQ (2), Supervisor
@ (3) and System (15), from any mode but FIQ, the banked r13/r14 swapped
@ as set_cpu_mode swaps them (REG_MODE: reg_mode[mode & 0xF], 7 words);
@ an interrupt to raise (IE & IF, IME, I clear) goes to the C body, whose
@ set_cpu_mode then does nothing (the mode is already the new one) and
@ which raises it; FIQ, Abort, Undefined and invalid modes take the C way
@ as before. ARM mode: GBA r14 is reg_x5, r13 lives in reg[] (memory).
#ifdef NSPIRE_BUILD
@ r0: the value; [lr] the masks, [lr + 8] the PC (execute_store_cpsr's)
nspire_store_cpsr_fast:
  ldr r1, [reg_base, #CPU_MODE]
  lsr r1, r1, #4
  ldr r1, [lr, r1, lsl #2]                @ the mask
  and reg_flags, r0, r1
  ldr r2, [reg_base, #REG_CPSR]
  bic r2, r2, r1
  orr reg_flags, reg_flags, r2            @ the new CPSR (r0 kept for the C way)
  tst r1, #0xFF
  beq 6f                                  @ no control field: the CPSR alone
  and r2, reg_flags, #0xF                 @ cpu_modes[cpsr & 0xF]:
  cmp r2, #0xF
  moveq r1, #0x10                         @   System
  beq 1f
  cmp r2, #2
  moveq r1, #0x11                         @   IRQ
  beq 1f
  cmp r2, #3
  moveq r1, #0x13                         @   Supervisor
  beq 1f
  cmp r2, #0
  bne execute_store_cpsr_way              @   (FIQ, Abort, Undefined, invalid)
  mov r1, #0x00                           @   User
1:
  ldr r2, [reg_base, #CPU_MODE]
  cmp r2, r1
  beq 5f                                  @ the same mode: nothing to swap
  cmp r2, #0x12
  beq execute_store_cpsr_way              @ from FIQ (r8-r14 banked)
  str r1, [reg_base, #CPU_MODE]
  and r2, r2, #0xF
  rsb r2, r2, r2, lsl #3
  add r2, reg_base, r2, lsl #2            @ + 28 x the old mode's bank
  ldr r0, [reg_base, #REG_SP]
  str r0, [r2, #(REGMODE_RAM_OFF + 5 * 4)]
  str reg_x5, [r2, #(REGMODE_RAM_OFF + 6 * 4)]
  and r1, r1, #0xF
  rsb r1, r1, r1, lsl #3
  add r1, reg_base, r1, lsl #2            @ + 28 x the new mode's bank
  ldr r0, [r1, #(REGMODE_RAM_OFF + 5 * 4)]
  str r0, [reg_base, #REG_SP]
  ldr reg_x5, [r1, #(REGMODE_RAM_OFF + 6 * 4)]
5:
  str reg_flags, [reg_base, #REG_CPSR]
  tst reg_flags, #0x80
  bne 7f                                  @ I set: no interrupt
  add r2, reg_base, #(IOREG_OFF + 0x200)
  ldrh r0, [r2]                           @ IE
  ldrh r1, [r2, #2]                       @ IF
  tst r0, r1
  beq 7f
  ldrh r0, [r2, #8]                       @ IME
  cmp r0, #0
  beq 7f
  mov r0, reg_flags                       @ one to raise: the C body raises it
  mov r1, #0xFF
  b execute_store_cpsr_call
6:
  str reg_flags, [reg_base, #REG_CPSR]
7:
  restore_flags()
  add pc, lr, #12                         @ past the masks and the PC

@ ext_io_store_u16's IE, IF and IME (an interrupt handler's acknowledge and
@ its IE juggling: Advance Wars' every line, Fire Emblem's): what
@ write_io_register16 does for them, then as its return (an interrupt to
@ raise: io_store_alert with CPU_ALERT_IRQ; nothing: back). r0: the address
@ (its low 24 bits pick the register, as write_io_register16's), r1: the
@ value (its low 16 bits); the rest as the C way.
nspire_io_store16_fast:
  save_flags()
  bic r2, r0, #0xFF000000
  mov r2, r2, lsr #1                      @ write_io_register16's ioreg
  sub r2, r2, #0x100                      @ IE 0, IF 1, IME 4
  cmp r2, #1
  beq 2f
  bhi 1f
  add r2, reg_base, #(IOREG_OFF + 0x200)  @ IE
  strh r1, [r2]
  b 3f
1:
  cmp r2, #4
  bne ext_io_store_c_u16                  @ any other: write_io_register16
  add r2, reg_base, #(IOREG_OFF + 0x200)  @ IME
  strh r1, [r2, #8]
3:                                        @ check_interrupt (r2: IE's address)
  ldrh r0, [r2]
  ldrh r1, [r2, #2]
  tst r0, r1
  beq 4f
  ldrh r0, [r2, #8]
  cmp r0, #0
  beq 4f
  ldr r0, [reg_base, #REG_CPSR]
  tst r0, #0x80
  bne 4f
  mov r0, #CPU_ALERT_IRQ
  b io_store_alert
2:
  add r2, reg_base, #(IOREG_OFF + 0x200)  @ IF: the bits written are cleared
  ldrh r0, [r2, #2]
  mov r1, r1, lsl #16
  bic r0, r0, r1, lsr #16
  strh r0, [r2, #2]
4:
  restore_flags()
  mem_return()

@ r0: the PC; the registers stored, reg[REG_CPSR] = reg_flags = the SPSR
@ (execute_spsr_restore's); lr is free here (its end is a lookup)
nspire_spsr_restore_fast:
  and r2, reg_flags, #0xF
  cmp r2, #0xF
  moveq r1, #0x10
  beq 1f
  cmp r2, #2
  moveq r1, #0x11
  beq 1f
  cmp r2, #3
  moveq r1, #0x13
  beq 1f
  cmp r2, #0
  bne 8f
  mov r1, #0x00
1:
  ldr r2, [reg_base, #CPU_MODE]
  cmp r2, r1
  beq 5f
  cmp r2, #0x12
  beq 8f
  str r1, [reg_base, #CPU_MODE]
  and r2, r2, #0xF
  rsb r2, r2, r2, lsl #3
  add r2, reg_base, r2, lsl #2
  ldr lr, [reg_base, #REG_SP]
  str lr, [r2, #(REGMODE_RAM_OFF + 5 * 4)]
  ldr lr, [reg_base, #REG_LR]
  str lr, [r2, #(REGMODE_RAM_OFF + 6 * 4)]
  and r1, r1, #0xF
  rsb r1, r1, r1, lsl #3
  add r1, reg_base, r1, lsl #2
  ldr lr, [r1, #(REGMODE_RAM_OFF + 5 * 4)]
  str lr, [reg_base, #REG_SP]
  ldr lr, [r1, #(REGMODE_RAM_OFF + 6 * 4)]
  str lr, [reg_base, #REG_LR]
5:
  tst reg_flags, #0x80                    @ check_for_interrupts
  bne execute_spsr_restore_done
  add r2, reg_base, #(IOREG_OFF + 0x200)
  ldrh r1, [r2]
  ldrh lr, [r2, #2]
  tst r1, lr
  beq execute_spsr_restore_done
  ldrh r1, [r2, #8]
  cmp r1, #0
  beq execute_spsr_restore_done
8:
  call_c_function(execute_spsr_restore_body)
  b execute_spsr_restore_done
#endif

@ (speed10's merge: these _watch forms in a section of their own, which
@ nspire_lineio.ld puts inside the hot stretch where the MSR and SPSR return
@ C bodies were, cold since 340950b's fast paths: the stretch stays within
@ 16 KB, four pages; tests/calc_checks.py)
.pushsection .text.hot.gpsp.watch,"ax",%progbits
.align 2
@ speed10: the palette and VRAM stores' _watch forms (see the handlers):
@ the same stores, the value each replaces read first and compared (VRAM's
@ only while REG_VIDEO_GEN isn't negative, nspire_reuse.c's frames that
@ stamp lines), a change counted in REG_VIDEO_GEN. Registers and flags as
@ the handlers': r0-r2 and r9 used, the flags kept.
#define vram_watch(store_type, ldr_op, str_op, dup, changed, mask)           ;\
ext_store_vram_u##store_type##_watch:                                        ;\
  save_flags()                                                               ;\
  mask                                                                       ;\
  dup                                                                        ;\
  cmp r0, #0x18000                        /* Check if exceeds 96KB         */;\
  subcs r0, r0, #0x8000                   /* Mirror to the last bank       */;\
  add r2, reg_base, #VRAM_OFF             /* r2 = vram base                */;\
  add r0, r0, r2                          /* r0 = the address in vram      */;\
  ldr r2, [reg_base, #REG_VIDEO_GEN]      /* values compared?              */;\
  cmp r2, #0                                                                 ;\
  blt 4f                                  /* no: a write is a change       */;\
  ldr_op r2, [r0]                         /* the value before              */;\
  str_op r1, [r0]                         /* store data                    */;\
  changed                                 /* ne: a value changed           */;\
  ldrne r2, [reg_base, #REG_VIDEO_GEN]                                       ;\
  addne r2, r2, #1                                                           ;\
  strne r2, [reg_base, #REG_VIDEO_GEN]                                       ;\
  restore_flags()                                                            ;\
  mem_return()                                                               ;\
4:                                                                           ;\
  str_op r1, [r0]                         /* store data                    */;\
  add r2, r2, #1                                                             ;\
  str r2, [reg_base, #REG_VIDEO_GEN]                                         ;\
  restore_flags()                                                            ;\
  mem_return()                                                               ;\

#define vram_changed_16 eor r2, r2, r1; movs r2, r2, lsl #16
#define vram_changed_32 cmp r2, r1
vram_watch(8, ldrh, strh, dup8(r1), vram_changed_16, mask_addr_16(17))
vram_watch(16, ldrh, strh, , vram_changed_16, mask_addr_16(17))
vram_watch(32, ldr, str, , vram_changed_32, mask_addr_32(17))

ext_store_vram_u32_safe_watch:            @ (from block stores: no PC word, no align)
  mask_addr_8(17)
  save_flags()
  add r2, reg_base, #VRAM_OFF
  cmp r0, #0x18000
  subcs r0, r0, #0x8000
  add r0, r0, r2
  ldr r2, [reg_base, #REG_VIDEO_GEN]
  cmp r2, #0
  blt 4f
  ldr r2, [r0]
  str r1, [r0]
  cmp r2, r1
  ldrne r2, [reg_base, #REG_VIDEO_GEN]
  addne r2, r2, #1
  strne r2, [reg_base, #REG_VIDEO_GEN]
  restore_flags()
  bx lr
4:
  str r1, [r0]
  add r2, r2, #1
  str r2, [reg_base, #REG_VIDEO_GEN]
  restore_flags()
  bx lr

ext_store_palette_u8_watch:
  bic r1, r1, #0xff00                     // Duplicate the byte
  orr r1, r1, lsl #8
ext_store_palette_u16_watch:
  mask_addr_16(10)                        // Accesses are always 16 bit
  add r2, reg_base, #PAL_RAM_OFF          // r2 = palette base
  ldrh r9, [r0, r2]                       // the value before
  strh r1, [r0, r2]                       // store data
  mrs r2, cpsr                            // changed? (the flags kept)
  eor r9, r9, r1
  movs r9, r9, lsl #16
  ldrne r9, [reg_base, #REG_VIDEO_GEN]
  addne r9, r9, #1
  strne r9, [reg_base, #REG_VIDEO_GEN]
  msr cpsr_f, r2

  and r2, r1, #0x3E0                      // Convert color point
  lsl r2, r2, #1
  orr r2, r1, lsl #11
  and r1, r1, #0x7C00
  orr r2, r1, lsr #10

  add r1, reg_base, #PAL_CONV_OFF         // r1 = converted palette ram
  strh r2, [r0, r1]                       // Converted value write (r2)
  mem_return()                            // return

ext_store_palette_u32_watch:
  mask_addr_32(10)                        // Accesses are always 16 bit
  add r2, reg_base, #PAL_RAM_OFF          // r2 = palette base
  ldr r9, [r0, r2]                        // the value before
  str r1, [r0, r2]                        // store data
  mrs r2, cpsr                            // changed? (the flags kept)
  cmp r9, r1
  ldrne r9, [reg_base, #REG_VIDEO_GEN]
  addne r9, r9, #1
  strne r9, [reg_base, #REG_VIDEO_GEN]
  msr cpsr_f, r2
  add r2, reg_base, #PAL_CONV_OFF         // r2 = converted palette ram

  lsr r9, r1, #10
  and r9, r9, #0x1F
  orr r9, r1, lsl #11
  bic r1, r1, #0x1F
  bic r1, r1, #0xFC00
  orr r9, r1, lsl #1
  strh r9, [r0, r2]                       // Write first halfword
  add r0, r0, #2

  lsr r1, r1, #16
  and r9, r1, #0x3E0
  lsl r9, r9, #1
  orr r9, r1, lsl #11
  and r1, r1, #0x7C00
  orr r9, r1, lsr #10
  strh r9, [r0, r2]                       // Write second halfword

  mem_return()                            // return
.popsection

@ The end of the hot stretch (update_gba is its start): nspire_sram.c runs
@ these pages from the CX II's on-chip SRAM. A label only: no bytes.
.global nspire_hot_end
nspire_hot_end:
#endif

.data
.align 4

defsymbl(st_handler_functions)
  store_lookup_table(8)
  store_lookup_table(16)
  store_lookup_table(32)
  store_lookup_table(32_safe)
#ifdef NSPIRE_BUILD
@ Regions 0x05-0x07 of each store table while lines wait (see above)
#define flush_table(store_type)                                              ;\
  .word ext_store_palette_u##store_type##_flush                              ;\
  .word ext_store_vram_u##store_type##_flush                                 ;\
  .word ext_store_oam_ram_u##store_type##_flush                              ;\

defsymbl(st_flush_functions)
  flush_table(8)
  flush_table(16)
  flush_table(32)
  flush_table(32_safe)

@ mem_dispatch's 256-entry tables (.bss below) in the 17-entry tables'
@ order: stores u8, u16, u32, u32_safe, loads u8, s8, u16, s16, u32
defsymbl(nspire_mem_tables)
  .word mem_tbl_st_u8, mem_tbl_st_u16, mem_tbl_st_u32, mem_tbl_st_u32_safe
  .word mem_tbl_ld_u8, mem_tbl_ld_s8, mem_tbl_ld_u16, mem_tbl_ld_s16
  .word mem_tbl_ld_u32
#endif
defsymbl(ld_handler_functions)
  load_table_gen(u8,  rdmap)
  load_table_gen(s8,  rdmap)
  load_table_gen(u16, rdmap)
  load_table_gen(s16, rdmap)
  load_table_gen(u32, rdmap)
defsymbl(ld_swap_handler_functions)
  load_table_gen(u8,  rdmap_slow)
  load_table_gen(s8,  rdmap_slow)
  load_table_gen(u16, rdmap_slow)
  load_table_gen(s16, rdmap_slow)
  load_table_gen(u32, rdmap_slow)

.bss
#ifdef NSPIRE_BUILD
@ Pinned (nspire_lineio.ld): the GBA's memory and the dynarec's
@ registers start a data cache way (2 KB) and an instruction cache way
@ (4 KB) apart from nothing else, in the calculator build and the speed
@ model alike, so their cache sets don't move with unrelated changes.
.balign 4096
#else
.align 4
#endif

defsymbl(iwram)
  .space 0x10000
defsymbl(vram)
  .space 0x18000
defsymbl(ewram)
  .space 0x80000
defsymbl(reg)
  .space 0x100
defsymbl(spsr)
  .space 24
@ Place lookup tables here for easy access via base_reg too (Nspire since
@ core-10: unused, mem_dispatch reads nspire_mem_tables; kept so that the
@ pinned .bss after them stays where it is)
#ifdef NSPIRE_BUILD
@ speed10b: cpu_threaded.c's table of kept RAM blocks (128 words) and its
@ scratch bitmap (32 words) in this unused space (st/ld_lookup_tables and
@ their padding: 744 bytes)
defsymbl(nspire_ram_keep_table)
#endif
defsymbl(st_lookup_tables)
  .space 4*17*4   @ store
defsymbl(ld_lookup_tables)
  .space 5*17*4   @ loads
  .space 132      @ Padding for alignment
defsymbl(reg_mode)
  .space 196
  .space 60       @ More padding!
defsymbl(oam_ram)
  .space 0x400
defsymbl(palette_ram)
  .space 0x400
defsymbl(memory_map_read)
  .space 0x8000
defsymbl(io_registers)
  .space 0x400
defsymbl(palette_ram_converted)
  .space 0x400
#ifdef NSPIRE_BUILD
@ mem_dispatch's tables (above), at reg + MEMTBL_*, in that order from
@ where palette_ram_converted ends (reg + 0x9500), each 0x400 bytes; the
@ whole stretch is 0x3000 bytes, so the .bss after it keeps its place in a
@ 4 KB way (nspire_lineio.ld). tests/arm/dispatch_test.c checks
@ the stubs read these labels.
#define mem_table(name, off, prev_end)                                       ;\
  .if (off) < (prev_end)                                                     ;\
  .error "mem_dispatch tables: MEMTBL_ offsets overlap"                      ;\
  .endif                                                                     ;\
  .if (off) > (prev_end)                                                     ;\
  .space (off) - (prev_end)                                                  ;\
  .endif                                                                     ;\
defsymbl(name)                                                               ;\
  .space 0x400                                                               ;\

mem_table(mem_tbl_st_u8,       MEMTBL_ST_U8,       0x9500)
mem_table(mem_tbl_ld_u32,      MEMTBL_LD_U32,      MEMTBL_ST_U8 + 0x400)
mem_table(mem_tbl_st_u32,      MEMTBL_ST_U32,      MEMTBL_LD_U32 + 0x400)
mem_table(mem_tbl_ld_u16,      MEMTBL_LD_U16,      MEMTBL_ST_U32 + 0x400)
mem_table(mem_tbl_ld_s16,      MEMTBL_LD_S16,      MEMTBL_LD_U16 + 0x400)
mem_table(mem_tbl_st_u16,      MEMTBL_ST_U16,      MEMTBL_LD_S16 + 0x400)
mem_table(mem_tbl_ld_s8,       MEMTBL_LD_S8,       MEMTBL_ST_U16 + 0x400)
mem_table(mem_tbl_st_u32_safe, MEMTBL_ST_U32_SAFE, MEMTBL_LD_S8 + 0x400)
mem_table(mem_tbl_ld_u8,       MEMTBL_LD_U8,       MEMTBL_ST_U32_SAFE + 0x400)
  .if MEMTBL_LD_U8 + 0x400 > 0xC500
  .error "mem_dispatch tables: more than 0x3000 bytes"
  .endif
  .space 0xC500 - (MEMTBL_LD_U8 + 0x400)
#endif

@ Vita and 3DS (and of course mmap) map their own cache sections through some
@ platform-speficic mechanisms.
#if !defined(MMAP_JIT_CACHE) && !defined(VITA) && !defined(_3DS)

@ Make this section executable!
.text
.section .jit,"awx",%nobits
#ifdef NSPIRE_BUILD
@ Pinned as the .bss above: the blocks' cache sets against the hot code's.
.balign 4096
#else
.align 4
#endif
defsymbl(rom_translation_cache)
  .space ROM_TRANSLATION_CACHE_SIZE
.size rom_translation_cache, .-rom_translation_cache
defsymbl(ram_translation_cache)
  .space RAM_TRANSLATION_CACHE_SIZE
.size ram_translation_cache, .-ram_translation_cache

#endif

#if defined(__linux__) && defined(__ELF__)
  .section .note.GNU-stack,"",%progbits
#endif

