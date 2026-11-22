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

#ifndef ARM_EMIT_H
#define ARM_EMIT_H

#include "arm_codegen.h"

void generate_indirect_branch_arm(void);
u32 arm_prepare_load_reg(u8 **tptr, u32 scratch_reg, u32 reg_index);
u32 arm_prepare_load_reg_pc(u8 **tptr, u32 scratch_reg, u32 reg_index, u32 pc_offset);
u32 arm_prepare_store_reg(u32 scratch_reg, u32 reg_index);
u32 thumb_prepare_load_reg(u8 **tptr, u32 scratch_reg, u32 reg_index);
u32 thumb_prepare_load_reg_pc(u8 **tptr, u32 scratch_reg, u32 reg_index, u32 pc_offset);
u32 thumb_prepare_store_reg(u32 scratch_reg, u32 reg_index);
void thumb_cheat_hook(void);
void arm_cheat_hook(void);

u32 arm_update_gba_arm(u32 pc);
u32 arm_update_gba_thumb(u32 pc);
u32 arm_update_gba_idle_arm(u32 pc);
u32 arm_update_gba_idle_thumb(u32 pc);

/* Although these are defined as a function, don't call them as
 * such (jump to it instead) */
void arm_indirect_branch_arm(u32 address);
void arm_indirect_branch_thumb(u32 address);
void arm_indirect_branch_dual_arm(u32 address);
void arm_indirect_branch_dual_thumb(u32 address);

void execute_store_cpsr(u32 new_cpsr);
u32 execute_store_cpsr_body(u32 _cpsr, u32 store_mask, u32 address);
u32 execute_spsr_restore(u32 address);

void execute_swi_arm(u32 pc);
void execute_swi_thumb(u32 pc);

#define armfn_gbaup_idle_arm       0
#define armfn_gbaup_idle_thumb     1
#define armfn_gbaup_arm            2
#define armfn_gbaup_thumb          3
#define armfn_swi_arm              4
#define armfn_swi_thumb            5
#define armfn_cheat_arm            6
#define armfn_cheat_thumb          7
#define armfn_store_cpsr           8
#define armfn_spsr_restore         9
#define armfn_indirect_arm        10
#define armfn_indirect_thumb      11
#define armfn_indirect_dual_arm   12
#define armfn_indirect_dual_thumb 13
#define armfn_debug_trace         14
#define armfn_gbaup_idle_pass_thumb 15   /* speed9b, Nspire */

#define STORE_TBL_OFF     0x118
#define SPSR_RAM_OFF      0x100

#define write32(value)                                                        \
  *((u32 *)translation_ptr) = value;                                          \
  translation_ptr += 4                                                        \
  
#ifdef NSPIRE_BUILD
/* speed10b: every branch offset the translation encodes, its place logged
 * (nspire_rel_offset, below nspire_block_tail_state): a RAM block's copy
 * (cpu_threaded.c "RAM blocks kept across Partial SMC's flushes") moves
 * those of its branches that leave the block. */
static u32 nspire_rel_offset(u32 source, u32 target);
#define arm_relative_offset(source, offset)                                   \
  nspire_rel_offset((u32)(source), (u32)(offset))                            \

#else
#define arm_relative_offset(source, offset)                                   \
  (((((u32)offset - (u32)source) - 8) >> 2) & 0xFFFFFF)                       \

#endif


#define reg_a0          ARMREG_R0
#define reg_a1          ARMREG_R1
#define reg_a2          ARMREG_R2

/* scratch0 is shared with flags, be careful! */
#define reg_s0          ARMREG_R9
#define reg_base        ARMREG_R11
#define reg_flags       ARMREG_R9

#define reg_cycles      ARMREG_R12

#define reg_rv          ARMREG_R0

#define reg_rm          ARMREG_R0
#define reg_rn          ARMREG_R1
#define reg_rs          ARMREG_R14
#define reg_rd          ARMREG_R0


/* Register allocation layout for ARM and Thumb:
 * Map from a GBA register to a host ARM register. -1 means load it
 * from memory into one of the temp registers.

 * The following registers are chosen based on statistical analysis
 * of a few games (see below), but might not be the best ones. Results
 * vary tremendously between ARM and Thumb (for obvious reasons), so
 * two sets are used. Take care to not call any function which can
 * overwrite any of these registers from the dynarec - only call
 * trusted functions in arm_stub.S which know how to save/restore
 * them and know how to transfer them to the C functions it calls
 * if necessary.

 * The following define the actual registers available for allocation.
 * As registers are freed up add them to this list.

 * Note that r15 is linked to the a0 temp reg - this register will
 * be preloaded with a constant upon read, and used to link to
 * indirect branch functions upon write.
 */

#define reg_x0         ARMREG_R3
#define reg_x1         ARMREG_R4
#define reg_x2         ARMREG_R5
#define reg_x3         ARMREG_R6
#define reg_x4         ARMREG_R7
#define reg_x5         ARMREG_R8

#define mem_reg        (~0U)

/*

ARM register usage (38.775138% ARM instructions):
r00: 18.263814% (-- 18.263814%)
r12: 11.531477% (-- 29.795291%)
r09: 11.500162% (-- 41.295453%)
r14: 9.063440% (-- 50.358893%)
r06: 7.837682% (-- 58.196574%)
r01: 7.401049% (-- 65.597623%)
r07: 6.778340% (-- 72.375963%)
r05: 5.445009% (-- 77.820973%)
r02: 5.427288% (-- 83.248260%)
r03: 5.293743% (-- 88.542003%)
r04: 3.601103% (-- 92.143106%)
r11: 3.207311% (-- 95.350417%)
r10: 2.334864% (-- 97.685281%)
r08: 1.708207% (-- 99.393488%)
r15: 0.311270% (-- 99.704757%)
r13: 0.295243% (-- 100.000000%)

Thumb register usage (61.224862% Thumb instructions):
r00: 34.788858% (-- 34.788858%)
r01: 26.564083% (-- 61.352941%)
r03: 10.983500% (-- 72.336441%)
r02: 8.303127% (-- 80.639567%)
r04: 4.900381% (-- 85.539948%)
r05: 3.941292% (-- 89.481240%)
r06: 3.257582% (-- 92.738822%)
r07: 2.644851% (-- 95.383673%)
r13: 1.408824% (-- 96.792497%)
r08: 0.906433% (-- 97.698930%)
r09: 0.679693% (-- 98.378623%)
r10: 0.656446% (-- 99.035069%)
r12: 0.453668% (-- 99.488737%)
r14: 0.248909% (-- 99.737646%)
r11: 0.171066% (-- 99.908713%)
r15: 0.091287% (-- 100.000000%)

*/

u32 arm_register_allocation[] =
{
  reg_x0,       /* GBA r0  */
  reg_x1,       /* GBA r1  */
  mem_reg,      /* GBA r2  */
  mem_reg,      /* GBA r3  */
  mem_reg,      /* GBA r4  */
  mem_reg,      /* GBA r5  */
  reg_x2,       /* GBA r6  */
  mem_reg,      /* GBA r7  */
  mem_reg,      /* GBA r8  */
  reg_x3,       /* GBA r9  */
  mem_reg,      /* GBA r10 */
  mem_reg,      /* GBA r11 */
  reg_x4,       /* GBA r12 */
  mem_reg,      /* GBA r13 */
  reg_x5,       /* GBA r14 */
  reg_a0,       /* GBA r15 */

  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
};

u32 thumb_register_allocation[] =
{
  reg_x0,       /* GBA r0  */
  reg_x1,       /* GBA r1  */
  reg_x2,       /* GBA r2  */
  reg_x3,       /* GBA r3  */
  reg_x4,       /* GBA r4  */
  reg_x5,       /* GBA r5  */
  mem_reg,      /* GBA r6  */
  mem_reg,      /* GBA r7  */
  mem_reg,      /* GBA r8  */
  mem_reg,      /* GBA r9  */
  mem_reg,      /* GBA r10 */
  mem_reg,      /* GBA r11 */
  mem_reg,      /* GBA r12 */
  mem_reg,      /* GBA r13 */
  mem_reg,      /* GBA r14 */
  reg_a0,       /* GBA r15 */

  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
  mem_reg,
};

#define arm_imm_lsl_to_rot(value)                                             \
  (32 - value)                                                                \

u32 arm_disect_imm_32bit(u32 imm, u32 *stores, u32 *rotations)
{
  u32 store_count = 0;
  u32 left_shift = 0;

  /* Otherwise it'll return 0 things to store because it'll never
   * find anything. */
  if(imm == 0)
  {
    rotations[0] = 0;
    stores[0] = 0;
    return 1;
  }

  /* Find chunks of non-zero data at 2 bit alignments. */
  while(1)
  {
    for(; left_shift < 32; left_shift += 2)
    {
      if((imm >> left_shift) & 0x03)
        break;
    }

    /* We've hit the end of the useful data. */
    if(left_shift == 32)
      return store_count;

    /* Hit the end, it might wrap back around to the beginning. */
    if(left_shift >= 24)
    {
      /* Make a mask for the residual bits. IE, if we have
       * 5 bits of data at the end we can wrap around to 3
       * bits of data in the beginning. Thus the first
       * thing, after being shifted left, has to be less
       * than 111b, 0x7, or (1 << 3) - 1.
       */
      u32 top_bits = 32 - left_shift;
      u32 residual_bits = 8 - top_bits;
      u32 residual_mask = (1 << residual_bits) - 1;

      if((store_count > 1) && (left_shift > 24) &&
       ((stores[0] << ((32 - rotations[0]) & 0x1F)) < residual_mask))
      {
        /* Then we can throw out the last bit and tack it on
         * to the first bit. */
        stores[0] =
         (stores[0] << ((top_bits + (32 - rotations[0])) & 0x1F)) |
         ((imm >> left_shift) & 0xFF);
        rotations[0] = top_bits;

        return store_count;
      }
      else
      {
        /* There's nothing to wrap over to in the beginning */
        stores[store_count] = (imm >> left_shift) & 0xFF;
        rotations[store_count] = (32 - left_shift) & 0x1F;
        return store_count + 1;
      }
      break;
    }

    stores[store_count] = (imm >> left_shift) & 0xFF;
    rotations[store_count] = (32 - left_shift) & 0x1F;

    store_count++;
    left_shift += 8;
  }
}

#if __ARM_ARCH >= 7
  #define arm_load_imm_32bit(ireg, imm)                                       \
  {                                                                           \
    ARM_MOVW(0, ireg, (imm));                                                 \
    if ((imm) >> 16) {                                                        \
      ARM_MOVT(0, ireg, ((imm) >> 16));                                       \
    }                                                                         \
  }
#else
  #define arm_load_imm_32bit(ireg, imm)                                       \
  {                                                                           \
    u32 stores[4];                                                            \
    u32 rotations[4];                                                         \
    u32 store_count = arm_disect_imm_32bit(imm, stores, rotations);           \
    u32 i;                                                                    \
                                                                              \
    ARM_MOV_REG_IMM(0, ireg, stores[0], rotations[0]);                        \
                                                                              \
    for(i = 1; i < store_count; i++)                                          \
    {                                                                         \
      ARM_ORR_REG_IMM(0, ireg, ireg, stores[i], rotations[i]);                \
    }                                                                         \
  }
#endif


#define generate_load_pc(ireg, new_pc)                                        \
  arm_load_imm_32bit(ireg, (new_pc))                                          \

#define generate_load_imm(ireg, imm, imm_ror)                                 \
  ARM_MOV_REG_IMM(0, ireg, imm, imm_ror)                                      \



#define generate_shift_left(ireg, imm)                                        \
  ARM_MOV_REG_IMMSHIFT(0, ireg, ireg, ARMSHIFT_LSL, imm)                      \

#define generate_shift_right(ireg, imm)                                       \
  ARM_MOV_REG_IMMSHIFT(0, ireg, ireg, ARMSHIFT_LSR, imm)                      \

#define generate_shift_right_arithmetic(ireg, imm)                            \
  ARM_MOV_REG_IMMSHIFT(0, ireg, ireg, ARMSHIFT_ASR, imm)                      \

#define generate_rotate_right(ireg, imm)                                      \
  ARM_MOV_REG_IMMSHIFT(0, ireg, ireg, ARMSHIFT_ROR, imm)                      \

#define generate_add(ireg_dest, ireg_src)                                     \
  ARM_ADD_REG_REG(0, ireg_dest, ireg_dest, ireg_src)                          \

#define generate_sub(ireg_dest, ireg_src)                                     \
  ARM_SUB_REG_REG(0, ireg_dest, ireg_dest, ireg_src)                          \

#define generate_or(ireg_dest, ireg_src)                                      \
  ARM_ORR_REG_REG(0, ireg_dest, ireg_dest, ireg_src)                          \

#define generate_xor(ireg_dest, ireg_src)                                     \
  ARM_EOR_REG_REG(0, ireg_dest, ireg_dest, ireg_src)                          \

#define generate_add_imm(ireg, imm, imm_ror)                                  \
  ARM_ADD_REG_IMM(0, ireg, ireg, imm, imm_ror)                                \

#define generate_sub_imm(ireg, imm, imm_ror)                                  \
  ARM_SUB_REG_IMM(0, ireg, ireg, imm, imm_ror)                                \

#define generate_xor_imm(ireg, imm, imm_ror)                                  \
  ARM_EOR_REG_IMM(0, ireg, ireg, imm, imm_ror)                                \

#define generate_add_reg_reg_imm(ireg_dest, ireg_src, imm, imm_ror)           \
  ARM_ADD_REG_IMM(0, ireg_dest, ireg_src, imm, imm_ror)                       \

#define generate_and_imm(ireg, imm, imm_ror)                                  \
  ARM_AND_REG_IMM(0, ireg, ireg, imm, imm_ror)                                \

#define generate_mov(ireg_dest, ireg_src)                                     \
  if(ireg_dest != ireg_src)                                                   \
  {                                                                           \
    ARM_MOV_REG_REG(0, ireg_dest, ireg_src);                                  \
  }                                                                           \

/* Calls functions present in the rom/ram cache (near) */
#define generate_function_call(function_location)                             \
  ARM_BL(0, arm_relative_offset(translation_ptr, function_location))          \

/* Calls functions that might be far, via the function table at reg_base */
#define generate_function_far_call(function_number)                           \
  generate_load_memreg(ARMREG_LR, function_number + (u32)REG_USERDEF);        \
  ARM_BLX(0, ARMREG_LR)                                                       \

/* The branch target is to be filled in later (thus a 0 for now) */

#define generate_branch_filler(condition_code, writeback_location)            \
  (writeback_location) = translation_ptr;                                     \
  ARM_B_COND(0, condition_code, 0)                                            \

#define generate_update_pc(new_pc)                                            \
  generate_load_pc(reg_a0, new_pc)                                            \

#define generate_cycle_update()                                               \
  if(cycle_count)                                                             \
  {                                                                           \
    if(cycle_count >> 8)                                                      \
    {                                                                         \
      ARM_ADD_REG_IMM(0, reg_cycles, reg_cycles, (cycle_count >> 8) & 0xFF,   \
       arm_imm_lsl_to_rot(8));                                                \
    }                                                                         \
    ARM_ADD_REG_IMM(0, reg_cycles, reg_cycles, (cycle_count & 0xFF), 0);      \
    cycle_count = 0;                                                          \
  }                                                                           \

#define generate_branch_patch_conditional(dest, offset)                       \
  *((u32 *)(dest)) = (*((u32 *)dest) & 0xFF000000) |                          \
   arm_relative_offset(dest, offset)                                          \


#define generate_branch_patch_unconditional(dest, offset)                     \
  *((u32 *)(dest)) = (*((u32 *)dest) & 0xFF000000) |                          \
   arm_relative_offset(dest, offset)                                          \

/* A different function is called for idle updates because of the relative
 * location of the embedded PC. The idle version could be optimized to put
 * the CPU into halt mode too, however.
 */

#ifdef NSPIRE_BUILD
/* speed9b (2026-11-18): an idle loop whose pass main.c checks (Advance
 * Wars', nspire_idle_pass_code: its code as expected) calls
 * arm_update_gba_idle_pass_thumb (arm_stub.S; function table entry
 * armfn_gbaup_idle_pass_thumb, the same two words as the idle stub's call),
 * any other the idle stub as before. */
u32 arm_update_gba_idle_pass_thumb(u32 pc);
bool nspire_idle_pass_code(u32 new_pc);

static u8 *nspire_idle_call(u8 *translation_ptr, u32 new_pc, u32 thumb);
#define nspire_idle_mode_arm    0
#define nspire_idle_mode_thumb  1

#define generate_branch_idle_eliminate(writeback_location, new_pc, mode)      \
  translation_ptr = nspire_idle_call(translation_ptr, new_pc,                 \
   nspire_idle_mode_##mode);                                                  \
  write32(new_pc);                                                            \
  generate_branch_filler(ARMCOND_AL, writeback_location)                      \

#else
#define generate_branch_idle_eliminate(writeback_location, new_pc, mode)      \
  generate_function_far_call(armfn_gbaup_idle_##mode);                        \
  write32(new_pc);                                                            \
  generate_branch_filler(ARMCOND_AL, writeback_location)                      \

#endif

#ifdef NSPIRE_BUILD
/* core-16 (2026-11-15): an idle loop whose body is proven pure at
 * translation calls arm_update_gba_pure_thumb (arm_stub.S) instead: its
 * update_gba runs the next events too (without the body) while nothing the
 * body reads can have changed (main.c).
 *
 * Pure: a Thumb loop in ROM jumping back within its block (new_pc <= pc),
 * at most NSPIRE_IDLE_PURE_MAX instructions before the branch, each a load
 * or register arithmetic on r0-r7 (no store, push/pop, ldm/stm, branch,
 * swi, high-register or SP write), and no register or flag read before the
 * body writes it is written anywhere in the body: the body then ends in
 * the same registers and flags at every pass while memory is unchanged.
 * The loads' addresses are the same at every pass; whether they read
 * plain memory is checked as it runs: their calls are pointed at
 * mem_dispatch_load_*_pure (arm_stub.S, REG_IDLE_TAINT). */
u32 arm_update_gba_pure_thumb(u32 pc);
void mem_dispatch_load_u8(void);
void mem_dispatch_load_s8(void);
void mem_dispatch_load_u16(void);
void mem_dispatch_load_s16(void);
void mem_dispatch_load_u32(void);
void mem_dispatch_load_u8_pure(void);
void mem_dispatch_load_s8_pure(void);
void mem_dispatch_load_u16_pure(void);
void mem_dispatch_load_s16_pure(void);
void mem_dispatch_load_u32_pure(void);

#define NSPIRE_IDLE_PURE_MAX 16
/* pure idle branches translated (tests/arm idle_test) */
u32 nspire_idle_pure_count;
/* the scan's bits: r0-r7 (0-7), SP (13), N Z C V (16-19) */
#define IDLE_R(r)  (1u << (r))
#define IDLE_SP    (1u << 13)
#define IDLE_N     (1u << 16)
#define IDLE_Z     (1u << 17)
#define IDLE_C     (1u << 18)
#define IDLE_V     (1u << 19)
#define IDLE_NZ    (IDLE_N | IDLE_Z)
#define IDLE_NZCV  (IDLE_N | IDLE_Z | IDLE_C | IDLE_V)

/* What a Thumb opcode reads, surely writes and may write (C of a shift by
 * a register), and whether it is a load (1; 2: a pc-relative one, which
 * translate_thumb_instruction may turn into a constant); false: not
 * allowed in a pure body. */
static bool nspire_idle_op(u32 op, u32 *rd_set, u32 *wr_set, u32 *may_set,
 u32 *load)
{
  u32 rd3 = IDLE_R(op & 7), rs3 = IDLE_R((op >> 3) & 7);
  u32 rn3 = IDLE_R((op >> 6) & 7), rd8 = IDLE_R((op >> 8) & 7);
  u32 r = 0, w = 0, m = 0;

  *load = 0;

  switch(op >> 11)
  {
    case 0x00:                                     /* LSL rd, rs, #imm */
      r = rs3; w = rd3 | IDLE_NZ | (((op >> 6) & 31) ? IDLE_C : 0);
      break;
    case 0x01: case 0x02:                          /* LSR, ASR #imm */
      r = rs3; w = rd3 | IDLE_NZ | IDLE_C;
      break;
    case 0x03:                                     /* ADD/SUB rd, rs, rn/#imm */
      r = rs3 | ((op & 0x400) ? 0 : rn3); w = rd3 | IDLE_NZCV;
      break;
    case 0x04:                                     /* MOV rd, #imm */
      w = rd8 | IDLE_NZ;
      break;
    case 0x05:                                     /* CMP rd, #imm */
      r = rd8; w = IDLE_NZCV;
      break;
    case 0x06: case 0x07:                          /* ADD/SUB rd, #imm */
      r = rd8; w = rd8 | IDLE_NZCV;
      break;
    case 0x08:
      if(op & 0x400)                               /* high registers, BX */
        return false;
      r = rd3 | rs3;
      switch((op >> 6) & 15)
      {
        case 0: case 1: case 12: case 14:          /* AND EOR ORR BIC */
          w = rd3 | IDLE_NZ; break;
        case 2: case 3: case 4: case 7:            /* LSL LSR ASR ROR rs */
          w = rd3 | IDLE_NZ; m = IDLE_C; break;
        case 5: case 6:                            /* ADC SBC */
          r |= IDLE_C; w = rd3 | IDLE_NZCV; break;
        case 8:                                    /* TST */
          w = IDLE_NZ; break;
        case 9:                                    /* NEG */
          r = rs3; w = rd3 | IDLE_NZCV; break;
        case 10: case 11:                          /* CMP CMN */
          w = IDLE_NZCV; break;
        case 13:                                   /* MUL */
          w = rd3 | IDLE_NZ; m = IDLE_C; break;
        default:                                   /* MVN */
          r = rs3; w = rd3 | IDLE_NZ; break;
      }
      break;
    case 0x09:                                     /* LDR rd, [pc, #imm] */
      w = rd8; *load = 2;
      break;
    case 0x0A: case 0x0B:                          /* [rb, ro]: loads only */
      if(((op >> 9) & 7) < 3)
        return false;
      r = rs3 | rn3; w = rd3; *load = 1;
      break;
    case 0x0D: case 0x0F: case 0x11:               /* LDR, LDRB, LDRH #imm */
      r = rs3; w = rd3; *load = 1;
      break;
    case 0x13:                                     /* LDR rd, [sp, #imm] */
      r = IDLE_SP; w = rd8; *load = 1;
      break;
    case 0x14:                                     /* ADD rd, pc, #imm */
      w = rd8;
      break;
    case 0x15:                                     /* ADD rd, sp, #imm */
      r = IDLE_SP; w = rd8;
      break;
    default:
      return false;
  }
  *rd_set = r;
  *wr_set = w;
  *may_set = w | m;
  return true;
}

/* The flags a condition (ARM numbering, 14 always) reads. */
static u32 nspire_idle_cond_flags(u32 cond)
{
  static const u32 flags[8] = { IDLE_Z, IDLE_C, IDLE_N, IDLE_V,
   IDLE_C | IDLE_Z, IDLE_N | IDLE_V, IDLE_N | IDLE_Z | IDLE_V, 0 };
  return flags[(cond >> 1) & 7];
}

/* The body's translated code, [code, code_end): every load call ("bl
 * mem_dispatch_load_*" whose PC word is a body instruction's: since
 * core-17 the ROM cache's table of them, nspire_pcs, newest first) is
 * counted and, if patch, pointed at its _pure entry. */
typedef struct
{
  u32 host;
  u32 pc;
} nspire_mem_pc_type;
static const nspire_mem_pc_type *nspire_rom_pcs_newest(u32 *count);

static u32 nspire_idle_patch_loads(u8 *code, u8 *code_end, u32 new_pc,
 u32 pc, bool patch)
{
  static void (*const plain[5])(void) = { mem_dispatch_load_u8,
   mem_dispatch_load_s8, mem_dispatch_load_u16, mem_dispatch_load_s16,
   mem_dispatch_load_u32 };
  static void (*const pure[5])(void) = { mem_dispatch_load_u8_pure,
   mem_dispatch_load_s8_pure, mem_dispatch_load_u16_pure,
   mem_dispatch_load_s16_pure, mem_dispatch_load_u32_pure };
  u32 calls = 0, k, n;
  const nspire_mem_pc_type *e = nspire_rom_pcs_newest(&n);

  for(; n && e->host > (u32)code; e++, n--)
  {
    u32 target;
    u32 *word = (u32 *)(e->host - 4);     /* the call                      */
    if(e->host > (u32)code_end || e->pc < new_pc || e->pc >= pc ||
     (word[0] & 0xFF000000) != 0xEB000000)
      continue;
    target = (u32)word + 8 + (((s32)(word[0] << 8)) >> 6);
    for(k = 0; k < 5; k++)
    {
      if(target == (u32)plain[k])
      {
        calls++;
        if(patch)
          word[0] = 0xEB000000 |
           ((((u32)pure[k] - ((u32)word + 8)) >> 2) & 0xFFFFFF);
      }
    }
  }
  return calls;
}

/* The branch at pc (to new_pc, under cond) closes a pure idle loop. page:
 * pc's 32 KB page as translation reads it; code, code_end: the body's
 * translated code (block_data's offsets of new_pc and pc). If so, the
 * body's loads are pointed at the _pure dispatch. */
static bool nspire_idle_body_pure(const u8 *page, u32 block_start, u32 new_pc,
 u32 pc, u32 cond, u8 *code, u8 *code_end)
{
  u32 rd[NSPIRE_IDLE_PURE_MAX], wr[NSPIRE_IDLE_PURE_MAX], may, load;
  u32 n, i, written = 0, defined = 0, loads = 0;

  if(pc < 0x08000000 || pc >= 0x0E000000 || new_pc < block_start ||
   new_pc > pc || (new_pc & 1) || (new_pc >> 15) != (pc >> 15) ||
   (cheat_master_hook >= new_pc && cheat_master_hook <= pc))
    return false;
  n = (pc - new_pc) / 2;
  if(n > NSPIRE_IDLE_PURE_MAX)
    return false;
  for(i = 0; i < n; i++)
  {
    u32 at = new_pc + i * 2, op = address16(page, at & 0x7FFF);
    if(!nspire_idle_op(op, &rd[i], &wr[i], &may, &load))
      return false;
    written |= may;
    /* translate_thumb_instruction's constant: ROM, the word's page */
    if(load == 2)
      load = ((((at & ~2) + (op & 0xFF) * 4 + 4 + 4) >> 15) != (at >> 15));
    loads += load;
  }
  /* A value read before the body surely wrote it is the one from before
   * the body: it must be one the body never writes. */
  for(i = 0; i < n; i++)
  {
    if(rd[i] & ~defined & written)
      return false;
    defined |= wr[i];
  }
  if(nspire_idle_cond_flags(cond) & ~defined & written)
    return false;
  /* Every load through a dispatch call that can be checked as it runs. */
  if(nspire_idle_patch_loads(code, code_end, new_pc, pc, false) != loads)
    return false;
  nspire_idle_patch_loads(code, code_end, new_pc, pc, true);
  nspire_idle_pure_count++;
  return true;
}

/* Taken only for Thumb branches (thumb_conditional_branch, thumb_b), once
 * each: it patches the body's loads when it says yes. */
#define nspire_idle_pure(new_pc, cond)                                        \
  (!ram_region &&                                                             \
   (pc == idle_loop_target_pc || (new_pc) == idle_loop_target_pc) &&          \
   (new_pc) >= block_start_pc && (new_pc) <= pc &&                            \
   nspire_idle_body_pure(pc_address_block, block_start_pc, (new_pc), pc,      \
    (cond), block_data[((new_pc) - block_start_pc) / 2].block_offset,         \
    block_data[block_data_position].block_offset))                            \

#define generate_branch_idle_pure(writeback_location, new_pc)                 \
  ARM_BL(0, arm_relative_offset(translation_ptr,                              \
   (u32)arm_update_gba_pure_thumb));                                          \
  write32(new_pc);                                                            \
  generate_branch_filler(ARMCOND_AL, writeback_location)                      \

/* The loop's exit (a conditional branch's fall-through): not armed. */
#define generate_idle_disarm()                                                \
  ARM_MOV_REG_IMM(0, reg_a0, 0, 0);                                           \
  ARM_STR_IMM(0, reg_a0, reg_base, (REG_IDLE_ARMED * 4))                      \

#endif

#ifdef NSPIRE_BUILD
/* The cycle check at a block's exit, in 3 words instead of 6 (a fifth of
 * the translated code; the ARM926's instruction cache is 16 KB): a shared
 * stub (arm_stub.S, arm_check_update_*) returns past the PC word while
 * cycles are left, else updates the GBA from there. */
void arm_check_update_arm(void);
void arm_check_update_thumb(void);

#define generate_branch_update(writeback_location, new_pc, mode)              \
  ARM_BL(0, arm_relative_offset(translation_ptr, (u32)arm_check_update_##mode)); \
  write32(new_pc);                                                            \
  generate_branch_filler(ARMCOND_AL, writeback_location)                      \

#else
#define generate_branch_update(writeback_location, new_pc, mode)              \
  ARM_MOV_REG_IMMSHIFT(0, reg_a0, reg_cycles, ARMSHIFT_LSR, 31);              \
  /* If counter is negative, skip the update call (2 insts) */                \
  ARM_ADD_REG_IMMSHIFT(0, ARMREG_PC, ARMREG_PC, reg_a0, ARMSHIFT_LSL, 3);     \
  write32(new_pc);                                                            \
  generate_function_far_call(armfn_gbaup_##mode);   /* 2 instructions */      \
  generate_branch_filler(ARMCOND_AL, writeback_location)                      \

#endif


#define generate_branch_no_cycle_update(writeback_location, new_pc, mode)     \
  if(pc == idle_loop_target_pc || (new_pc) == idle_loop_target_pc)            \
  {                                                                           \
    generate_branch_idle_eliminate(writeback_location, new_pc, mode);         \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    generate_branch_update(writeback_location, new_pc, mode);                 \
  }                                                                           \

#define generate_branch_cycle_update(writeback_location, new_pc, mode)        \
  generate_cycle_update();                                                    \
  generate_branch_no_cycle_update(writeback_location, new_pc, mode)           \

/* a0 holds the destination */

#define generate_indirect_branch_no_cycle_update(type)                        \
  ARM_LDR_IMM(0, ARMREG_PC, reg_base, 4*(REG_USERDEF + armfn_indirect_##type));

#define generate_indirect_branch_cycle_update(type)                           \
  generate_cycle_update();                                                    \
  generate_indirect_branch_no_cycle_update(type)                              \

#define generate_indirect_branch_arm()                                        \
  {                                                                           \
    if(condition == 0x0E)                                                     \
    {                                                                         \
      generate_cycle_update();                                                \
    }                                                                         \
    generate_indirect_branch_no_cycle_update(arm);                            \
  }                                                                           \

#define generate_indirect_branch_dual()                                       \
  {                                                                           \
    if(condition == 0x0E)                                                     \
    {                                                                         \
      generate_cycle_update();                                                \
    }                                                                         \
    generate_indirect_branch_no_cycle_update(dual_arm);                       \
  }                                                                           \

#define arm_generate_store_reg(ireg, reg_index)                               \
{                                                                             \
  u32 store_dest = arm_register_allocation[reg_index];                        \
  if(store_dest != mem_reg)                                                   \
  {                                                                           \
    ARM_MOV_REG_REG(0, store_dest, ireg);                                     \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    ARM_STR_IMM(0, ireg, reg_base, (reg_index * 4));                          \
  }                                                                           \
}                                                                             \

#define thumb_generate_store_reg(ireg, reg_index)                             \
{                                                                             \
  u32 store_dest = thumb_register_allocation[reg_index];                      \
  if(store_dest != mem_reg)                                                   \
  {                                                                           \
    ARM_MOV_REG_REG(0, store_dest, ireg);                                     \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    ARM_STR_IMM(0, ireg, reg_base, (reg_index * 4));                          \
  }                                                                           \
}

#define generate_load_memreg(ireg, reg_index)                                 \
  ARM_LDR_IMM(0, ireg, reg_base, ((reg_index) * 4));                          \

#ifdef NSPIRE_BUILD
/* speed9b: the idle branch's call (one copy, out of line: the macro
   generate_branch_idle_eliminate is in every branch's expansion). */
static __attribute__((noinline)) u8 *nspire_idle_call(u8 *translation_ptr,
 u32 new_pc, u32 thumb)
{
  /* the same two words either way: the blocks after it where they were */
  u32 fn = thumb && nspire_idle_pass_code(new_pc) ?
   armfn_gbaup_idle_pass_thumb : thumb ? armfn_gbaup_idle_thumb :
   armfn_gbaup_idle_arm;
  generate_function_far_call(fn);
  return translation_ptr;
}
#endif

#define arm_generate_load_reg(ireg, reg_index)                                \
{                                                                             \
  u32 load_src = arm_register_allocation[reg_index];                          \
  if(load_src != mem_reg)                                                     \
  {                                                                           \
    ARM_MOV_REG_REG(0, ireg, load_src);                                       \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    ARM_LDR_IMM(0, ireg, reg_base, ((reg_index) * 4));                        \
  }                                                                           \
}                                                                             \

#define thumb_generate_load_reg(ireg, reg_index)                              \
{                                                                             \
  u32 load_src = thumb_register_allocation[reg_index];                        \
  if(load_src != mem_reg)                                                     \
  {                                                                           \
    ARM_MOV_REG_REG(0, ireg, load_src);                                       \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    ARM_LDR_IMM(0, ireg, reg_base, ((reg_index) * 4));                        \
  }                                                                           \
}                                                                             \

#define arm_generate_load_reg_pc(ireg, reg_index, pc_offset)                  \
  if(reg_index == 15)                                                         \
  {                                                                           \
    generate_load_pc(ireg, pc + pc_offset);                                   \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    arm_generate_load_reg(ireg, reg_index);                                   \
  }                                                                           \

#define thumb_generate_load_reg_pc(ireg, reg_index, pc_offset)                \
  if(reg_index == 15)                                                         \
  {                                                                           \
    generate_load_pc(ireg, pc + pc_offset);                                   \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    thumb_generate_load_reg(ireg, reg_index);                                 \
  }                                                                           \

u32 arm_prepare_store_reg(u32 scratch_reg, u32 reg_index)
{
  u32 reg_use = arm_register_allocation[reg_index];
  if(reg_use == mem_reg)
    return scratch_reg;

  return reg_use;
}

u32 thumb_prepare_store_reg(u32 scratch_reg, u32 reg_index)
{
  u32 reg_use = thumb_register_allocation[reg_index];
  if(reg_use == mem_reg)
    return scratch_reg;

  return reg_use;
}

u32 arm_prepare_load_reg(u8 **tptr, u32 scratch_reg, u32 reg_index)
{
  u32 reg_use = arm_register_allocation[reg_index];
  if(reg_use != mem_reg)
    return reg_use;

  u8 *translation_ptr = *tptr;
  ARM_LDR_IMM(0, scratch_reg, reg_base, (reg_index * 4));
  *tptr = translation_ptr;
  return scratch_reg;
}

u32 arm_prepare_load_reg_pc(u8 **tptr, u32 scratch_reg, u32 reg_index, u32 pc_value)
{
  if(reg_index == 15)
  {
    u8 *translation_ptr = *tptr;
    generate_load_pc(scratch_reg, pc_value);
    *tptr = translation_ptr;
    return scratch_reg;
  }
  return arm_prepare_load_reg(tptr, scratch_reg, reg_index);
}

u32 thumb_prepare_load_reg(u8 **tptr, u32 scratch_reg, u32 reg_index)
{
  u32 reg_use = thumb_register_allocation[reg_index];
  if(reg_use != mem_reg)
    return reg_use;

  u8 *translation_ptr = *tptr;
  ARM_LDR_IMM(0, scratch_reg, reg_base, (reg_index * 4));
  *tptr = translation_ptr;
  return scratch_reg;
}

u32 thumb_prepare_load_reg_pc(u8 **tptr, u32 scratch_reg, u32 reg_index, u32 pc_value)
{
  if(reg_index == 15)
  {
    u8 *translation_ptr = *tptr;
    generate_load_pc(scratch_reg, pc_value);
    *tptr = translation_ptr;
    return scratch_reg;
  }
  return thumb_prepare_load_reg(tptr, scratch_reg, reg_index);
}

#define arm_complete_store_reg(scratch_reg, reg_index)                        \
{                                                                             \
  if(arm_register_allocation[reg_index] == mem_reg)                           \
  {                                                                           \
    ARM_STR_IMM(0, scratch_reg, reg_base, (reg_index * 4));                   \
  }                                                                           \
}

#define thumb_complete_store_reg(scratch_reg, reg_index)                      \
{                                                                             \
  if(thumb_register_allocation[reg_index] == mem_reg)                         \
  {                                                                           \
    ARM_STR_IMM(0, scratch_reg, reg_base, (reg_index * 4));                   \
  }                                                                           \
}

#define block_prologue_size 0
#define generate_block_prologue()
#define generate_block_extra_vars_arm()
#define generate_block_extra_vars_thumb()

#define arm_complete_store_reg_pc_no_flags(scratch_reg, reg_index)            \
{                                                                             \
  if(reg_index == 15)                                                         \
  {                                                                           \
    generate_indirect_branch_arm();                                           \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    arm_complete_store_reg(scratch_reg, reg_index);                           \
  }                                                                           \
}                                                                             \

#define arm_complete_store_reg_pc_flags(scratch_reg, reg_index)               \
{                                                                             \
  if(reg_index == 15)                                                         \
  {                                                                           \
    if(condition == 0x0E)                                                     \
    {                                                                         \
      generate_cycle_update();                                                \
    }                                                                         \
    generate_function_far_call(armfn_spsr_restore);                           \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    arm_complete_store_reg(scratch_reg, reg_index);                           \
  }                                                                           \
}                                                                             \

/* It should be okay to still generate result flags, spsr will overwrite them.
 * This is pretty infrequent (returning from interrupt handlers, et al) so
 * probably not worth optimizing for.
 */

#define check_for_interrupts()                                                \
  if((io_registers[REG_IE] & io_registers[REG_IF]) &&                         \
   io_registers[REG_IME] && ((reg[REG_CPSR] & 0x80) == 0))                    \
  {                                                                           \
    REG_MODE(MODE_IRQ)[6] = pc + 4;                                           \
    REG_SPSR(MODE_IRQ) = reg[REG_CPSR];                                           \
    reg[REG_CPSR] = 0xD2;                                                     \
    pc = 0x00000018;                                                          \
    set_cpu_mode(MODE_IRQ);                                                   \
  }                                                                           \

#define arm_generate_store_reg_pc_no_flags(ireg, reg_index)                   \
  arm_generate_store_reg(ireg, reg_index);                                    \
  if(reg_index == 15)                                                         \
  {                                                                           \
    generate_indirect_branch_arm();                                           \
  }                                                                           \


NSPIRE_HOT u32 execute_spsr_restore_body(u32 pc)
{
  set_cpu_mode(cpu_modes[reg[REG_CPSR] & 0xF]);
  check_for_interrupts();

  return pc;
}

#define generate_save_flags()                                                 \
  ARM_MRS_CPSR(0, reg_flags)                                                  \

#define generate_restore_flags()                                              \
  ARM_MSR_REG(0, ARM_PSR_F, reg_flags, ARM_CPSR)                              \

#define condition_opposite_eq ARMCOND_NE
#define condition_opposite_ne ARMCOND_EQ
#define condition_opposite_cs ARMCOND_CC
#define condition_opposite_cc ARMCOND_CS
#define condition_opposite_mi ARMCOND_PL
#define condition_opposite_pl ARMCOND_MI
#define condition_opposite_vs ARMCOND_VC
#define condition_opposite_vc ARMCOND_VS
#define condition_opposite_hi ARMCOND_LS
#define condition_opposite_ls ARMCOND_HI
#define condition_opposite_ge ARMCOND_LT
#define condition_opposite_lt ARMCOND_GE
#define condition_opposite_gt ARMCOND_LE
#define condition_opposite_le ARMCOND_GT
#define condition_opposite_al ARMCOND_NV
#define condition_opposite_nv ARMCOND_AL

#define generate_branch(mode)                                                 \
{                                                                             \
  generate_branch_cycle_update(                                               \
   block_exits[block_exit_position].branch_source,                            \
   block_exits[block_exit_position].branch_target, mode);                     \
  block_exit_position++;                                                      \
}                                                                             \


#define generate_op_and_reg_immshift(_rd, _rn, _rm, shift_type, shift)        \
  ARM_AND_REG_IMMSHIFT(0, _rd, _rn, _rm, shift_type, shift)                   \

#define generate_op_orr_reg_immshift(_rd, _rn, _rm, shift_type, shift)        \
  ARM_ORR_REG_IMMSHIFT(0, _rd, _rn, _rm, shift_type, shift)                   \

#define generate_op_eor_reg_immshift(_rd, _rn, _rm, shift_type, shift)        \
  ARM_EOR_REG_IMMSHIFT(0, _rd, _rn, _rm, shift_type, shift)                   \

#define generate_op_bic_reg_immshift(_rd, _rn, _rm, shift_type, shift)        \
  ARM_BIC_REG_IMMSHIFT(0, _rd, _rn, _rm, shift_type, shift)                   \

#define generate_op_sub_reg_immshift(_rd, _rn, _rm, shift_type, shift)        \
  ARM_SUB_REG_IMMSHIFT(0, _rd, _rn, _rm, shift_type, shift)                   \

#define generate_op_rsb_reg_immshift(_rd, _rn, _rm, shift_type, shift)        \
  ARM_RSB_REG_IMMSHIFT(0, _rd, _rn, _rm, shift_type, shift)                   \

#define generate_op_sbc_reg_immshift(_rd, _rn, _rm, shift_type, shift)        \
  ARM_SBC_REG_IMMSHIFT(0, _rd, _rn, _rm, shift_type, shift)                   \

#define generate_op_rsc_reg_immshift(_rd, _rn, _rm, shift_type, shift)        \
  ARM_RSC_REG_IMMSHIFT(0, _rd, _rn, _rm, shift_type, shift)                   \

#define generate_op_add_reg_immshift(_rd, _rn, _rm, shift_type, shift)        \
  ARM_ADD_REG_IMMSHIFT(0, _rd, _rn, _rm, shift_type, shift)                   \

#define generate_op_adc_reg_immshift(_rd, _rn, _rm, shift_type, shift)        \
  ARM_ADC_REG_IMMSHIFT(0, _rd, _rn, _rm, shift_type, shift)                   \

#define generate_op_mov_reg_immshift(_rd, _rn, _rm, shift_type, shift)        \
  ARM_MOV_REG_IMMSHIFT(0, _rd, _rm, shift_type, shift)                        \

#define generate_op_mvn_reg_immshift(_rd, _rn, _rm, shift_type, shift)        \
  ARM_MVN_REG_IMMSHIFT(0, _rd, _rm, shift_type, shift)                        \


#define generate_op_and_reg_regshift(_rd, _rn, _rm, shift_type, _rs)          \
  ARM_AND_REG_REGSHIFT(0, _rd, _rn, _rm, shift_type, _rs)                     \

#define generate_op_orr_reg_regshift(_rd, _rn, _rm, shift_type, _rs)          \
  ARM_ORR_REG_REGSHIFT(0, _rd, _rn, _rm, shift_type, _rs)                     \

#define generate_op_eor_reg_regshift(_rd, _rn, _rm, shift_type, _rs)          \
  ARM_EOR_REG_REGSHIFT(0, _rd, _rn, _rm, shift_type, _rs)                     \

#define generate_op_bic_reg_regshift(_rd, _rn, _rm, shift_type, _rs)          \
  ARM_BIC_REG_REGSHIFT(0, _rd, _rn, _rm, shift_type, _rs)                     \

#define generate_op_sub_reg_regshift(_rd, _rn, _rm, shift_type, _rs)          \
  ARM_SUB_REG_REGSHIFT(0, _rd, _rn, _rm, shift_type, _rs)                     \

#define generate_op_rsb_reg_regshift(_rd, _rn, _rm, shift_type, _rs)          \
  ARM_RSB_REG_REGSHIFT(0, _rd, _rn, _rm, shift_type, _rs)                     \

#define generate_op_sbc_reg_regshift(_rd, _rn, _rm, shift_type, _rs)          \
  ARM_SBC_REG_REGSHIFT(0, _rd, _rn, _rm, shift_type, _rs)                     \

#define generate_op_rsc_reg_regshift(_rd, _rn, _rm, shift_type, _rs)          \
  ARM_RSC_REG_REGSHIFT(0, _rd, _rn, _rm, shift_type, _rs)                     \

#define generate_op_add_reg_regshift(_rd, _rn, _rm, shift_type, _rs)          \
  ARM_ADD_REG_REGSHIFT(0, _rd, _rn, _rm, shift_type, _rs)                     \

#define generate_op_adc_reg_regshift(_rd, _rn, _rm, shift_type, _rs)          \
  ARM_ADC_REG_REGSHIFT(0, _rd, _rn, _rm, shift_type, _rs)                     \

#define generate_op_mov_reg_regshift(_rd, _rn, _rm, shift_type, _rs)          \
  ARM_MOV_REG_REGSHIFT(0, _rd, _rm, shift_type, _rs)                          \

#define generate_op_mvn_reg_regshift(_rd, _rn, _rm, shift_type, _rs)          \
  ARM_MVN_REG_REGSHIFT(0, _rd, _rm, shift_type, _rs)                          \


#define generate_op_and_imm(_rd, _rn)                                         \
  ARM_AND_REG_IMM(0, _rd, _rn, imm, imm_ror)                                  \

#define generate_op_orr_imm(_rd, _rn)                                         \
  ARM_ORR_REG_IMM(0, _rd, _rn, imm, imm_ror)                                  \

#define generate_op_eor_imm(_rd, _rn)                                         \
  ARM_EOR_REG_IMM(0, _rd, _rn, imm, imm_ror)                                  \

#define generate_op_bic_imm(_rd, _rn)                                         \
  ARM_BIC_REG_IMM(0, _rd, _rn, imm, imm_ror)                                  \

#define generate_op_sub_imm(_rd, _rn)                                         \
  ARM_SUB_REG_IMM(0, _rd, _rn, imm, imm_ror)                                  \

#define generate_op_rsb_imm(_rd, _rn)                                         \
  ARM_RSB_REG_IMM(0, _rd, _rn, imm, imm_ror)                                  \

#define generate_op_sbc_imm(_rd, _rn)                                         \
  ARM_SBC_REG_IMM(0, _rd, _rn, imm, imm_ror)                                  \

#define generate_op_rsc_imm(_rd, _rn)                                         \
  ARM_RSC_REG_IMM(0, _rd, _rn, imm, imm_ror)                                  \

#define generate_op_add_imm(_rd, _rn)                                         \
  ARM_ADD_REG_IMM(0, _rd, _rn, imm, imm_ror)                                  \

#define generate_op_adc_imm(_rd, _rn)                                         \
  ARM_ADC_REG_IMM(0, _rd, _rn, imm, imm_ror)                                  \

#define generate_op_mov_imm(_rd, _rn)                                         \
  ARM_MOV_REG_IMM(0, _rd, imm, imm_ror)                                       \

#define generate_op_mvn_imm(_rd, _rn)                                         \
  ARM_MVN_REG_IMM(0, _rd, imm, imm_ror)                                       \


#define generate_op_reg_immshift_lflags(name, _rd, _rn, _rm, st, shift)       \
  ARM_##name##_REG_IMMSHIFT(0, _rd, _rn, _rm, st, shift)                      \

#define generate_op_reg_immshift_aflags(name, _rd, _rn, _rm, st, shift)       \
  ARM_##name##_REG_IMMSHIFT(0, _rd, _rn, _rm, st, shift)                      \

#define generate_op_reg_immshift_aflags_load_c(name, _rd, _rn, _rm, st, sh)   \
  ARM_##name##_REG_IMMSHIFT(0, _rd, _rn, _rm, st, sh)                         \

#define generate_op_reg_immshift_uflags(name, _rd, _rm, shift_type, shift)    \
  ARM_##name##_REG_IMMSHIFT(0, _rd, _rm, shift_type, shift)                   \

#define generate_op_reg_immshift_tflags(name, _rn, _rm, shift_type, shift)    \
  ARM_##name##_REG_IMMSHIFT(0, _rn, _rm, shift_type, shift)                   \


#define generate_op_reg_regshift_lflags(name, _rd, _rn, _rm, shift_type, _rs) \
  ARM_##name##_REG_REGSHIFT(0, _rd, _rn, _rm, shift_type, _rs)                \

#define generate_op_reg_regshift_aflags(name, _rd, _rn, _rm, st, _rs)         \
  ARM_##name##_REG_REGSHIFT(0, _rd, _rn, _rm, st, _rs)                        \

#define generate_op_reg_regshift_aflags_load_c(name, _rd, _rn, _rm, st, _rs)  \
  ARM_##name##_REG_REGSHIFT(0, _rd, _rn, _rm, st, _rs)                        \

#define generate_op_reg_regshift_uflags(name, _rd, _rm, shift_type, _rs)      \
  ARM_##name##_REG_REGSHIFT(0, _rd, _rm, shift_type, _rs)                     \

#define generate_op_reg_regshift_tflags(name, _rn, _rm, shift_type, _rs)      \
  ARM_##name##_REG_REGSHIFT(0, _rn, _rm, shift_type, _rs)                     \


#define generate_op_imm_lflags(name, _rd, _rn)                                \
  ARM_##name##_REG_IMM(0, _rd, _rn, imm, imm_ror)                             \

#define generate_op_imm_aflags(name, _rd, _rn)                                \
  ARM_##name##_REG_IMM(0, _rd, _rn, imm, imm_ror)                             \

#define generate_op_imm_aflags_load_c(name, _rd, _rn)                         \
  ARM_##name##_REG_IMM(0, _rd, _rn, imm, imm_ror)                             \

#define generate_op_imm_uflags(name, _rd)                                     \
  ARM_##name##_REG_IMM(0, _rd, imm, imm_ror)                                  \

#define generate_op_imm_tflags(name, _rn)                                     \
  ARM_##name##_REG_IMM(0, _rn, imm, imm_ror)                                  \


#define generate_op_ands_reg_immshift(_rd, _rn, _rm, shift_type, shift)       \
  generate_op_reg_immshift_lflags(ANDS, _rd, _rn, _rm, shift_type, shift)     \

#define generate_op_orrs_reg_immshift(_rd, _rn, _rm, shift_type, shift)       \
  generate_op_reg_immshift_lflags(ORRS, _rd, _rn, _rm, shift_type, shift)     \

#define generate_op_eors_reg_immshift(_rd, _rn, _rm, shift_type, shift)       \
  generate_op_reg_immshift_lflags(EORS, _rd, _rn, _rm, shift_type, shift)     \

#define generate_op_bics_reg_immshift(_rd, _rn, _rm, shift_type, shift)       \
  generate_op_reg_immshift_lflags(BICS, _rd, _rn, _rm, shift_type, shift)     \

#define generate_op_subs_reg_immshift(_rd, _rn, _rm, shift_type, shift)       \
  generate_op_reg_immshift_aflags(SUBS, _rd, _rn, _rm, shift_type, shift)     \

#define generate_op_rsbs_reg_immshift(_rd, _rn, _rm, shift_type, shift)       \
  generate_op_reg_immshift_aflags(RSBS, _rd, _rn, _rm, shift_type, shift)     \

#define generate_op_sbcs_reg_immshift(_rd, _rn, _rm, st, shift)               \
  generate_op_reg_immshift_aflags_load_c(SBCS, _rd, _rn, _rm, st, shift)      \

#define generate_op_rscs_reg_immshift(_rd, _rn, _rm, st, shift)               \
  generate_op_reg_immshift_aflags_load_c(RSCS, _rd, _rn, _rm, st, shift)      \

#define generate_op_adds_reg_immshift(_rd, _rn, _rm, shift_type, shift)       \
  generate_op_reg_immshift_aflags(ADDS, _rd, _rn, _rm, shift_type, shift)     \

#define generate_op_adcs_reg_immshift(_rd, _rn, _rm, st, shift)               \
  generate_op_reg_immshift_aflags_load_c(ADCS, _rd, _rn, _rm, st, shift)      \

#define generate_op_movs_reg_immshift(_rd, _rn, _rm, shift_type, shift)       \
  generate_op_reg_immshift_uflags(MOVS, _rd, _rm, shift_type, shift)          \

#define generate_op_mvns_reg_immshift(_rd, _rn, _rm, shift_type, shift)       \
  generate_op_reg_immshift_uflags(MVNS, _rd, _rm, shift_type, shift)          \

/* The reg operand is in reg_rm, not reg_rn like expected, so rsbs isn't
 * being used here. When rsbs is fully inlined it can be used with the
 * apropriate operands.
 */

#define generate_op_neg_reg_immshift(_rd, _rn, _rm, shift_type, shift)        \
{                                                                             \
  generate_load_imm(reg_rn, 0, 0);                                            \
  generate_op_subs_reg_immshift(_rd, reg_rn, _rm, ARMSHIFT_LSL, 0);           \
}                                                                             \

#define generate_op_muls_reg_immshift(_rd, _rn, _rm, shift_type, shift)       \
  ARM_MULS(0, _rd, _rn, _rm);                                                 \

#define generate_op_cmp_reg_immshift(_rd, _rn, _rm, shift_type, shift)        \
  generate_op_reg_immshift_tflags(CMP, _rn, _rm, shift_type, shift)           \

#define generate_op_cmn_reg_immshift(_rd, _rn, _rm, shift_type, shift)        \
  generate_op_reg_immshift_tflags(CMN, _rn, _rm, shift_type, shift)           \

#define generate_op_tst_reg_immshift(_rd, _rn, _rm, shift_type, shift)        \
  generate_op_reg_immshift_tflags(TST, _rn, _rm, shift_type, shift)           \

#define generate_op_teq_reg_immshift(_rd, _rn, _rm, shift_type, shift)        \
  generate_op_reg_immshift_tflags(TEQ, _rn, _rm, shift_type, shift)           \


#define generate_op_ands_reg_regshift(_rd, _rn, _rm, shift_type, _rs)         \
  generate_op_reg_regshift_lflags(ANDS, _rd, _rn, _rm, shift_type, _rs)       \

#define generate_op_orrs_reg_regshift(_rd, _rn, _rm, shift_type, _rs)         \
  generate_op_reg_regshift_lflags(ORRS, _rd, _rn, _rm, shift_type, _rs)       \

#define generate_op_eors_reg_regshift(_rd, _rn, _rm, shift_type, _rs)         \
  generate_op_reg_regshift_lflags(EORS, _rd, _rn, _rm, shift_type, _rs)       \

#define generate_op_bics_reg_regshift(_rd, _rn, _rm, shift_type, _rs)         \
  generate_op_reg_regshift_lflags(BICS, _rd, _rn, _rm, shift_type, _rs)       \

#define generate_op_subs_reg_regshift(_rd, _rn, _rm, shift_type, _rs)         \
  generate_op_reg_regshift_aflags(SUBS, _rd, _rn, _rm, shift_type, _rs)       \

#define generate_op_rsbs_reg_regshift(_rd, _rn, _rm, shift_type, _rs)         \
  generate_op_reg_regshift_aflags(RSBS, _rd, _rn, _rm, shift_type, _rs)       \

#define generate_op_sbcs_reg_regshift(_rd, _rn, _rm, st, _rs)                 \
  generate_op_reg_regshift_aflags_load_c(SBCS, _rd, _rn, _rm, st, _rs)        \

#define generate_op_rscs_reg_regshift(_rd, _rn, _rm, st, _rs)                 \
  generate_op_reg_regshift_aflags_load_c(RSCS, _rd, _rn, _rm, st, _rs)        \

#define generate_op_adds_reg_regshift(_rd, _rn, _rm, shift_type, _rs)         \
  generate_op_reg_regshift_aflags(ADDS, _rd, _rn, _rm, shift_type, _rs)       \

#define generate_op_adcs_reg_regshift(_rd, _rn, _rm, st, _rs)                 \
  generate_op_reg_regshift_aflags_load_c(ADCS, _rd, _rn, _rm, st, _rs)        \

#define generate_op_movs_reg_regshift(_rd, _rn, _rm, shift_type, _rs)         \
  generate_op_reg_regshift_uflags(MOVS, _rd, _rm, shift_type, _rs)            \

#define generate_op_mvns_reg_regshift(_rd, _rn, _rm, shift_type, _rs)         \
  generate_op_reg_regshift_uflags(MVNS, _rd, _rm, shift_type, _rs)            \

#define generate_op_cmp_reg_regshift(_rd, _rn, _rm, shift_type, _rs)          \
  generate_op_reg_regshift_tflags(CMP, _rn, _rm, shift_type, _rs)             \

#define generate_op_cmn_reg_regshift(_rd, _rn, _rm, shift_type, _rs)          \
  generate_op_reg_regshift_tflags(CMN, _rn, _rm, shift_type, _rs)             \

#define generate_op_tst_reg_regshift(_rd, _rn, _rm, shift_type, _rs)          \
  generate_op_reg_regshift_tflags(TST, _rn, _rm, shift_type, _rs)             \

#define generate_op_teq_reg_regshift(_rd, _rn, _rm, shift_type, _rs)          \
  generate_op_reg_regshift_tflags(TEQ, _rn, _rm, shift_type, _rs)             \


#define generate_op_ands_imm(_rd, _rn)                                        \
  generate_op_imm_lflags(ANDS, _rd, _rn)                                      \

#define generate_op_orrs_imm(_rd, _rn)                                        \
  generate_op_imm_lflags(ORRS, _rd, _rn)                                      \

#define generate_op_eors_imm(_rd, _rn)                                        \
  generate_op_imm_lflags(EORS, _rd, _rn)                                      \

#define generate_op_bics_imm(_rd, _rn)                                        \
  generate_op_imm_lflags(BICS, _rd, _rn)                                      \

#define generate_op_subs_imm(_rd, _rn)                                        \
  generate_op_imm_aflags(SUBS, _rd, _rn)                                      \

#define generate_op_rsbs_imm(_rd, _rn)                                        \
  generate_op_imm_aflags(RSBS, _rd, _rn)                                      \

#define generate_op_sbcs_imm(_rd, _rn)                                        \
  generate_op_imm_aflags_load_c(SBCS, _rd, _rn)                               \

#define generate_op_rscs_imm(_rd, _rn)                                        \
  generate_op_imm_aflags_load_c(RSCS, _rd, _rn)                               \

#define generate_op_adds_imm(_rd, _rn)                                        \
  generate_op_imm_aflags(ADDS, _rd, _rn)                                      \

#define generate_op_adcs_imm(_rd, _rn)                                        \
  generate_op_imm_aflags_load_c(ADCS, _rd, _rn)                               \

#define generate_op_movs_imm(_rd, _rn)                                        \
  generate_op_imm_uflags(MOVS, _rd)                                           \

#define generate_op_mvns_imm(_rd, _rn)                                        \
  generate_op_imm_uflags(MVNS, _rd)                                           \

#define generate_op_cmp_imm(_rd, _rn)                                         \
  generate_op_imm_tflags(CMP, _rn)                                            \

#define generate_op_cmn_imm(_rd, _rn)                                         \
  generate_op_imm_tflags(CMN, _rn)                                            \

#define generate_op_tst_imm(_rd, _rn)                                         \
  generate_op_imm_tflags(TST, _rn)                                            \

#define generate_op_teq_imm(_rd, _rn)                                         \
  generate_op_imm_tflags(TEQ, _rn)                                            \


#define arm_prepare_load_rn_yes()                                             \
  u32 _rn = arm_prepare_load_reg_pc(&translation_ptr, reg_rn, rn, pc + 8)     \

#define arm_prepare_load_rn_no()                                              \

#define arm_prepare_store_rd_yes()                                            \
  u32 _rd = arm_prepare_store_reg(reg_rd, rd)                                 \

#define arm_prepare_store_rd_no()                                             \

#define arm_complete_store_rd_yes(flags_op)                                   \
  arm_complete_store_reg_pc_##flags_op(_rd, rd)                               \

#define arm_complete_store_rd_no(flags_op)                                    \

#define arm_generate_op_reg(name, load_op, store_op, flags_op)                \
  u32 shift_type = (opcode >> 5) & 0x03;                                      \
  arm_decode_data_proc_reg(opcode);                                           \
  arm_prepare_load_rn_##load_op();                                            \
  arm_prepare_store_rd_##store_op();                                          \
                                                                              \
  if((opcode >> 4) & 0x01)                                                    \
  {                                                                           \
    u32 rs = ((opcode >> 8) & 0x0F);                                          \
    u32 _rs = arm_prepare_load_reg(&translation_ptr, reg_rs, rs);             \
    u32 _rm = arm_prepare_load_reg_pc(&translation_ptr, reg_rm, rm, pc + 12); \
    generate_op_##name##_reg_regshift(_rd, _rn, _rm, shift_type, _rs);        \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    u32 shift_imm = ((opcode >> 7) & 0x1F);                                   \
    u32 _rm = arm_prepare_load_reg_pc(&translation_ptr, reg_rm, rm, pc + 8);  \
    generate_op_##name##_reg_immshift(_rd, _rn, _rm, shift_type, shift_imm);  \
  }                                                                           \
  arm_complete_store_rd_##store_op(flags_op)                                  \

#define arm_generate_op_reg_flags(name, load_op, store_op, flags_op)          \
  arm_generate_op_reg(name, load_op, store_op, flags_op)                      \

/* imm will be loaded by the called function if necessary. */

#define arm_generate_op_imm(name, load_op, store_op, flags_op)                \
  arm_decode_data_proc_imm(opcode);                                           \
  arm_prepare_load_rn_##load_op();                                            \
  arm_prepare_store_rd_##store_op();                                          \
  generate_op_##name##_imm(_rd, _rn);                                         \
  arm_complete_store_rd_##store_op(flags_op)                                  \

#define arm_generate_op_imm_flags(name, load_op, store_op, flags_op)          \
  arm_generate_op_imm(name, load_op, store_op, flags_op)                      \

#define arm_data_proc(name, type, flags_op)                                   \
{                                                                             \
  arm_generate_op_##type(name, yes, yes, flags_op);                           \
}                                                                             \

#define arm_data_proc_test(name, type)                                        \
{                                                                             \
  arm_generate_op_##type(name, yes, no, no);                                  \
}                                                                             \

#define arm_data_proc_unary(name, type, flags_op)                             \
{                                                                             \
  arm_generate_op_##type(name, no, yes, flags_op);                            \
}                                                                             \


#define arm_multiply_add_no_flags_no()                                        \
  ARM_MUL(0, _rd, _rm, _rs)                                                   \

#define arm_multiply_add_yes_flags_no()                                       \
  u32 _rn = arm_prepare_load_reg(&translation_ptr, reg_a2, rn);               \
  ARM_MLA(0, _rd, _rm, _rs, _rn)                                              \

#define arm_multiply_add_no_flags_yes()                                       \
  ARM_MULS(0, _rd, _rm, _rs)                                                  \

#define arm_multiply_add_yes_flags_yes()                                      \
  u32 _rn = arm_prepare_load_reg(&translation_ptr, reg_a2, rn);               \
  ARM_MLAS(0, _rd, _rm, _rs, _rn);                                            \


#define arm_multiply(add_op, flags)                                           \
{                                                                             \
  arm_decode_multiply();                                                      \
  u32 _rm = arm_prepare_load_reg(&translation_ptr, reg_a0, rm);               \
  u32 _rs = arm_prepare_load_reg(&translation_ptr, reg_a1, rs);               \
  u32 _rd = arm_prepare_store_reg(reg_a0, rd);                                \
  arm_multiply_add_##add_op##_flags_##flags();                                \
  arm_complete_store_reg(_rd, rd);                                            \
}                                                                             \


#define arm_multiply_long_name_s64     SMULL
#define arm_multiply_long_name_u64     UMULL
#define arm_multiply_long_name_s64_add SMLAL
#define arm_multiply_long_name_u64_add UMLAL


#define arm_multiply_long_flags_no(name)                                      \
  ARM_##name(0, _rdlo, _rdhi, _rm, _rs)                                       \

#define arm_multiply_long_flags_yes(name)                                     \
  ARM_##name##S(0, _rdlo, _rdhi, _rm, _rs);                                   \


#define arm_multiply_long_add_no(name)                                        \

#define arm_multiply_long_add_yes(name)                                       \
  arm_prepare_load_reg(&translation_ptr, reg_a0, rdlo);                       \
  arm_prepare_load_reg(&translation_ptr, reg_a1, rdhi)                        \


#define arm_multiply_long_op(flags, name)                                     \
  arm_multiply_long_flags_##flags(name)                                       \

#define arm_multiply_long(name, add_op, flags)                                \
{                                                                             \
  arm_decode_multiply_long();                                                 \
  u32 _rm = arm_prepare_load_reg(&translation_ptr, reg_a2, rm);               \
  u32 _rs = arm_prepare_load_reg(&translation_ptr, reg_rs, rs);               \
  u32 _rdlo = (rdlo == rdhi) ? reg_a0 : arm_prepare_store_reg(reg_a0, rdlo);  \
  u32 _rdhi = arm_prepare_store_reg(reg_a1, rdhi);                            \
  arm_multiply_long_add_##add_op(name);                                       \
  arm_multiply_long_op(flags, arm_multiply_long_name_##name);                 \
  arm_complete_store_reg(_rdlo, rdlo);                                        \
  arm_complete_store_reg(_rdhi, rdhi);                                        \
}                                                                             \

#define arm_psr_read_cpsr()                                                   \
{                                                                             \
  u32 _rd = arm_prepare_store_reg(reg_a0, rd);                                \
  generate_load_memreg(_rd, REG_CPSR);                                        \
  generate_save_flags();                                                      \
  ARM_BIC_REG_IMM(0, _rd, _rd, 0xF0, arm_imm_lsl_to_rot(24));                 \
  ARM_AND_REG_IMM(0, reg_flags, reg_flags, 0xF0, arm_imm_lsl_to_rot(24));     \
  ARM_ORR_REG_REG(0, _rd, _rd, reg_flags);                                    \
  arm_complete_store_reg(_rd, rd)                                             \
}

#define arm_psr_read_spsr()                                                   \
{                                                                             \
  u32 _rd = arm_prepare_store_reg(reg_a0, rd);                                \
  ARM_ADD_REG_IMM(0, reg_a0, reg_base, SPSR_RAM_OFF >> 2, 30);                \
  ARM_LDR_IMM(0, reg_a1, reg_base, CPU_MODE * 4);                             \
  ARM_AND_REG_IMM(0, reg_a1, reg_a1, 0xF, 0);                                 \
  ARM_LDR_REG_REG_SHIFT(0, _rd, reg_a0, reg_a1, ARMSHIFT_LSL, 2);             \
  arm_complete_store_reg(_rd, rd);                                            \
}

#define arm_psr_read(op_type, psr_reg)                                        \
  arm_psr_read_##psr_reg()                                                    \

/* This function's okay because it's called from an ASM function that can
 * wrap it correctly.
 */

NSPIRE_HOT u32 execute_store_cpsr_body(u32 _cpsr, u32 store_mask, u32 address)
{
  reg[REG_CPSR] = _cpsr;
  if(store_mask & 0xFF)
  {
    set_cpu_mode(cpu_modes[_cpsr & 0xF]);
    if((io_registers[REG_IE] & io_registers[REG_IF]) &&
     io_registers[REG_IME] && ((_cpsr & 0x80) == 0))
    {
      REG_MODE(MODE_IRQ)[6] = address + 4;
      REG_SPSR(MODE_IRQ) = _cpsr;
      reg[REG_CPSR] = 0xD2;
      set_cpu_mode(MODE_IRQ);
      return 0x00000018;
    }
  }

  return 0;
}

#ifdef NSPIRE_GBA_PROFILE
/* Model-only (tests/arm GBA_PROFILE=1): every GBA instruction the translated
 * code runs calls this (TRACE_INSTRUCTIONS' hook), counted by its PC (Thumb
 * ones with bit 0 set) while nspire_gba_prof_on; perf.c prints the table. */
#define TRACE_INSTRUCTIONS
u32 nspire_gba_prof_on;
u32 nspire_gba_prof[1 << 17][2];
static void trace_instruction(u32 pc, u32 mode)
{
  u32 key = pc | (mode ? 0 : 1), h;
  if (!nspire_gba_prof_on)
    return;
  h = (key * 2654435761u) >> 15;
  while (nspire_gba_prof[h][0] && nspire_gba_prof[h][0] != key)
    h = (h + 1) & ((1 << 17) - 1);
  nspire_gba_prof[h][0] = key;
  nspire_gba_prof[h][1]++;
}
#else
static void trace_instruction(u32 pc, u32 mode)
{
  if (mode)
    printf("Executed arm %x\n", pc);
  else
    printf("Executed thumb %x\n", pc);
  #ifdef TRACE_REGISTERS
  print_regs();
  #endif
}
#endif

#ifdef TRACE_INSTRUCTIONS
  #define emit_trace_instruction(pc, mode, regt)   \
  {                                                \
    unsigned i;                                    \
    for (i = 0; i < 15; i++) {                     \
      if (regt[i] != mem_reg) {                    \
        ARM_STR_IMM(0, regt[i], reg_base, (i*4));  \
      }                                            \
    }                                              \
    generate_save_flags();                         \
    ARM_STMDB_WB(0, ARMREG_SP, 0x500C);            \
    arm_load_imm_32bit(reg_a0, pc);                \
    arm_load_imm_32bit(reg_a1, mode);              \
    generate_function_far_call(armfn_debug_trace); \
    ARM_LDMIA_WB(0, ARMREG_SP, 0x500C);            \
    generate_restore_flags();                      \
  }

  #define emit_trace_thumb_instruction(pc)         \
    emit_trace_instruction(pc, 0, thumb_register_allocation)

  #define emit_trace_arm_instruction(pc)           \
    emit_trace_instruction(pc, 1, arm_register_allocation)
#else
  #define emit_trace_thumb_instruction(pc)
  #define emit_trace_arm_instruction(pc)
#endif

#define arm_psr_load_new_reg()                                                \
  arm_generate_load_reg(reg_a0, rm)                                           \

#define arm_psr_load_new_imm()                                                \
  generate_load_imm(reg_a0, imm, imm_ror)                                     \

#define arm_psr_store_cpsr()                                                  \
  generate_function_far_call(armfn_store_cpsr);                               \
  write32(cpsr_masks[psr_pfield][0]);                                         \
  write32(cpsr_masks[psr_pfield][1]);                                         \
  write32(pc);                                                                \

#define arm_psr_store_spsr()                                                  \
  arm_load_imm_32bit(reg_a1, spsr_masks[psr_pfield]);                         \
  ARM_LDR_IMM(0, reg_a2, reg_base, (CPU_MODE * 4));                           \
  ARM_AND_REG_IMM(0, reg_a2, reg_a2, 0xF, 0);                                 \
  ARM_ADD_REG_IMMSHIFT(0, ARMREG_LR, reg_base, reg_a2, ARMSHIFT_LSL, 2);      \
  ARM_AND_REG_IMMSHIFT(0, reg_a0, reg_a0, reg_a1, ARMSHIFT_LSL, 0);           \
  ARM_LDR_IMM(0, reg_a2, ARMREG_LR, SPSR_RAM_OFF);                            \
  ARM_BIC_REG_IMMSHIFT(0, reg_a2, reg_a2, reg_a1, ARMSHIFT_LSL, 0);           \
  ARM_ORR_REG_IMMSHIFT(0, reg_a0, reg_a0, reg_a2, ARMSHIFT_LSL, 0);           \
  ARM_STR_IMM(0, reg_a0, ARMREG_LR, SPSR_RAM_OFF);                            \

#define arm_psr_store(op_type, psr_reg)                                       \
  arm_psr_load_new_##op_type();                                               \
  arm_psr_store_##psr_reg()                                                   \


#define arm_psr(op_type, transfer_type, psr_reg)                              \
{                                                                             \
  arm_decode_psr_##op_type(opcode);                                           \
  arm_psr_##transfer_type(op_type, psr_reg);                                  \
}                                                                             \

/* We use USAT + ROR to map addresses to the handler table. For ARMv5 we use
   the table -1 entry to map any out of range/unaligned access, and some fun
   math/logical tricks to avoid using USAT */

#if __ARM_ARCH >= 6
  #define mem_calc_region(abits)                                              \
    if (abits) {                                                              \
      ARM_MOV_REG_IMMSHIFT(0, reg_a2, reg_a0, ARMSHIFT_ROR, abits)            \
      ARM_USAT_ASR(0, reg_a2, 4, reg_a2, 24-abits, ARMCOND_AL);               \
    } else {                                                                  \
      ARM_USAT_ASR(0, reg_a2, 4, reg_a0, 24, ARMCOND_AL);                     \
    }
#else
  #define mem_calc_region(abits)                                              \
    if (abits) {                                                              \
      ARM_ORR_REG_IMMSHIFT(0, reg_a2, reg_a0, reg_a0, ARMSHIFT_LSL, 32-abits);\
      ARM_MOV_REG_IMMSHIFT(0, reg_a2, reg_a2, ARMSHIFT_LSR, 24);              \
    } else {                                                                  \
      ARM_MOV_REG_IMMSHIFT(0, reg_a2, reg_a0, ARMSHIFT_LSR, 24);              \
    }                                                                         \
    ARM_RSB_REG_IMM(0, ARMREG_LR, reg_a2, 15, 0);                             \
    ARM_ORR_REG_IMMSHIFT(0, reg_a2, reg_a2, ARMREG_LR, ARMSHIFT_ASR, 31);
#endif

#ifdef NSPIRE_BUILD
/* The TI-Nspire's ARM926 has a 16 KB instruction cache: the region lookup
 * is one shared stub per access type (arm_stub.S, mem_dispatch), called
 * with "bl"; the translated code is a quarter smaller. The translation
 * caches are in the program image, near the stubs (bl reaches 32 MB). */
void mem_dispatch_store_u8(void);
void mem_dispatch_store_u16(void);
void mem_dispatch_store_u32(void);
void mem_dispatch_store_u32_safe(void);
void mem_dispatch_load_u8(void);
void mem_dispatch_load_s8(void);
void mem_dispatch_load_u16(void);
void mem_dispatch_load_s16(void);
void mem_dispatch_load_u32(void);

#define generate_mem_dispatch(name)                                           \
  ARM_BL(0, arm_relative_offset(translation_ptr, (u32)(name)));               \

#define generate_store_call_u8()        generate_mem_dispatch(mem_dispatch_store_u8)
#define generate_store_call_u16()       generate_mem_dispatch(mem_dispatch_store_u16)
#define generate_store_call_u32()       generate_mem_dispatch(mem_dispatch_store_u32)
#define generate_store_call_u32_safe()  generate_mem_dispatch(mem_dispatch_store_u32_safe)
#define generate_load_call_u8()         generate_mem_dispatch(mem_dispatch_load_u8)
#define generate_load_call_s8()         generate_mem_dispatch(mem_dispatch_load_s8)
#define generate_load_call_u16()        generate_mem_dispatch(mem_dispatch_load_u16)
#define generate_load_call_s16()        generate_mem_dispatch(mem_dispatch_load_s16)
#define generate_load_call_u32()        generate_mem_dispatch(mem_dispatch_load_u32)

/* core-17 (2026-11-15): no PC word after a memory access's "bl
 * mem_dispatch_*" (every access fetched it with its cache line, never ran
 * it); the handlers return with "bx lr". The few paths that need the GBA PC
 * (the SMC flush, BIOS and open-bus loads, an I/O store's alert: arm_stub.S
 * mem_pc_r2, unread_pc) ask nspire_mem_pc for the word the call used to
 * have, by the call's return address. Each cache has a table of (return
 * address, word), appended as the code is, so in rising address: the ROM
 * cache's grows down from the cache's top (translate_block_* keep the code
 * below it: mem_pc_full), the RAM cache's is nspire_pcs.ram. A flush empties
 * its cache's table (the ROM one keeps the entries of the code below the
 * watermark: the BIOS's SWI entry) and the lookups' small cache, but only
 * when the next translation starts (mem_pc_settle): the flushed code can
 * still be running then (a store's DMA flushes from inside its handler,
 * which returns into it) and is intact until something is translated over
 * it, which then runs instead. Tail code (core-11) appends too: one entry
 * needs at least its 4-byte bl.
 * NSPIRE_PCWORD_CHECK (tests/arm PCWORD_CHECK=1): the words are emitted as
 * before, the handlers return past them, and every lookup is checked
 * against the word. nspire_mem_pc_type (with nspire_idle_patch_loads):
 * host, the call's return address; pc, the word it used to have. */
#define NSPIRE_RAM_PCS  8192
#define NSPIRE_PC_CACHE 64
/* the entries the RAM table may hold (tests/arm RAMPCS=1, perf rampcs=N:
   fewer, so that its limit's flushes come often) */
#ifdef NSPIRE_TEST_RAM_PCS
extern u32 nspire_test_ram_pcs;
#define NSPIRE_RAM_PCS_LIMIT nspire_test_ram_pcs
#else
#define NSPIRE_RAM_PCS_LIMIT NSPIRE_RAM_PCS
#endif

/* 17 x 4 KB in all, so that the .bss after it keeps its place in a 4 KB
 * way (see nspire_block_tail_state). */
struct
{
  u32 ram_count;
  u32 rom_count;
  u32 rom_kept;                           /* below the watermark           */
  u32 misses;                             /* lookups not found (never)     */
  u32 pending;                            /* flushed: 1 RAM, 2 ROM         */
  u32 ram_flushes;                        /* since the program started     */
  u32 rom_flushes;
  u32 unused[1];
  nspire_mem_pc_type cache[NSPIRE_PC_CACHE];
  nspire_mem_pc_type ram[NSPIRE_RAM_PCS];
  u8 pad[4096 - 32 - NSPIRE_PC_CACHE * sizeof(nspire_mem_pc_type)];
} nspire_pcs;

#ifdef NSPIRE_PCWORD_CHECK
u32 nspire_pcword_check_ok, nspire_pcword_check_bad, nspire_pcword_check_hit;
#endif
#ifdef NSPIRE_TEST_RAM_PCS
u32 nspire_test_ram_pcs = NSPIRE_RAM_PCS;
#endif

/* tests/arm and gpsp_perf.txt.tns's "dynarec:" line: entries RAM (0), ROM
 * (1), kept (2), lookups not found (3: never, or a PC table is wrong), RAM
 * cache flushes (4), ROM cache flushes (5), the ROM cache's bytes in use,
 * code and table (6), RAM blocks copied from the kept ones (7), the kept
 * blocks' store emptied (8). */
u32 nspire_mem_pc_stats(u32 which)
{
  switch(which)
  {
    case 0: return nspire_pcs.ram_count;
    case 1: return nspire_pcs.rom_count;
    case 2: return nspire_pcs.rom_kept;
    case 3: return nspire_pcs.misses;
    case 4: return nspire_pcs.ram_flushes;
    case 5: return nspire_pcs.rom_flushes;
    /* speed10b: RAM blocks copied from the kept blocks' store, and how
       often the store was emptied (cpu_threaded.c) */
    case 7: return reg[REG_RAM_KEEP_USES];
    case 8: return reg[REG_RAM_KEEP_CLEARS];
    default:
      return (u32)(rom_translation_ptr - rom_translation_cache) +
       nspire_pcs.rom_count * sizeof(nspire_mem_pc_type);
  }
}

/* The ROM table: entry i at nspire_rom_pcs_top - 1 - i. */
#define nspire_rom_pcs_top                                                    \
  ((nspire_mem_pc_type *)(rom_translation_cache + ROM_TRANSLATION_CACHE_SIZE))

static void nspire_mem_pc_add(u8 *host, u32 pc)
{
  nspire_mem_pc_type *e;
  if((u32)host - (u32)rom_translation_cache < ROM_TRANSLATION_CACHE_SIZE)
    e = nspire_rom_pcs_top - 1 - nspire_pcs.rom_count++;
  else
    e = &nspire_pcs.ram[nspire_pcs.ram_count++];
  e->host = (u32)host;
  e->pc = pc;
}

/* The ROM table from its newest entry (falling hosts), count entries. */
static const nspire_mem_pc_type *nspire_rom_pcs_newest(u32 *count)
{
  *count = nspire_pcs.rom_count;
  return nspire_rom_pcs_top - nspire_pcs.rom_count;
}

/* The GBA PC word of the access whose call returns to host (arm_stub.S). */
u32 nspire_mem_pc(u32 host)
{
  /* The small cache's slot by the offset from where the cache's blocks
   * start (the watermark, NSPIRE_RAM_CACHE_START): the same at every
   * placement of the speed model's (jit=, ramjit=), so are the
   * instructions run. */
  u32 rom_off = host - (u32)rom_translation_cache;
  u32 base = rom_off >= ROM_TRANSLATION_CACHE_SIZE ?
   (u32)ram_translation_cache + NSPIRE_RAM_CACHE_START :
   rom_off >= rom_cache_watermark ?
   (u32)rom_translation_cache + rom_cache_watermark :
   (u32)rom_translation_cache;
  nspire_mem_pc_type *slot =
   &nspire_pcs.cache[((host - base) >> 2) & (NSPIRE_PC_CACHE - 1)];
  u32 pc;

  if(slot->host == host)
  {
    pc = slot->pc;
#ifdef NSPIRE_PCWORD_CHECK
    nspire_pcword_check_hit++;
#endif
  }
  else
  {
    const nspire_mem_pc_type *e;
    u32 lo = 0, hi, mid;
    s32 dir;
    if(rom_off < ROM_TRANSLATION_CACHE_SIZE)
    {
      /* descending from e[0], the newest */
      hi = nspire_pcs.rom_count;
      e = nspire_rom_pcs_top - hi;
      dir = -1;
    }
    else
    {
      hi = nspire_pcs.ram_count;
      e = nspire_pcs.ram;
      dir = 1;
    }
    /* the first index whose host is not below (ascending) or above
       (descending) the one looked for */
    while(lo < hi)
    {
      mid = (lo + hi) >> 1;
      if((dir > 0) ? (e[mid].host < host) : (e[mid].host > host))
        lo = mid + 1;
      else
        hi = mid;
    }
    if(lo < ((dir > 0) ? nspire_pcs.ram_count : nspire_pcs.rom_count) &&
     e[lo].host == host)
    {
      pc = e[lo].pc;
      slot->host = host;
      slot->pc = pc;
    }
    else
    {
      nspire_pcs.misses++;
      pc = reg[REG_PC];
    }
  }
#ifdef NSPIRE_PCWORD_CHECK
  if(pc == *(u32 *)host)
    nspire_pcword_check_ok++;
  else
    nspire_pcword_check_bad++;
#endif
  return pc;
}

#define mem_pc_cache_clear()                                                  \
  memset(nspire_pcs.cache, 0, sizeof(nspire_pcs.cache))

#define mem_pc_init()                                                         \
  nspire_pcs.ram_count = 0;                                                   \
  nspire_pcs.rom_count = 0;                                                   \
  nspire_pcs.rom_kept = 0;                                                    \
  nspire_pcs.pending = 0;                                                     \
  mem_pc_cache_clear()

#define mem_pc_flush_ram()                                                    \
  nspire_pcs.pending |= 1;                                                    \
  nspire_pcs.ram_flushes++

#define mem_pc_flush_rom()                                                    \
  nspire_pcs.pending |= 2;                                                    \
  nspire_pcs.rom_flushes++

/* At the start of translate_block_*: the flushes' resets. */
#define mem_pc_settle()                                                       \
  if(nspire_pcs.pending)                                                      \
  {                                                                           \
    if(nspire_pcs.pending & 1)                                                \
      nspire_pcs.ram_count = 0;                                               \
    if(nspire_pcs.pending & 2)                                                \
      nspire_pcs.rom_count = nspire_pcs.rom_kept;                             \
    nspire_pcs.pending = 0;                                                   \
    mem_pc_cache_clear();                                                     \
  }                                                                           \

/* init_bios_hooks: its code starts at the watermark, and stays. */
#define mem_pc_bios_start()                                                   \
  mem_pc_settle();                                                            \
  while(nspire_pcs.rom_count && (nspire_rom_pcs_top -                         \
   nspire_pcs.rom_count)->host >=                                             \
   (u32)&rom_translation_cache[rom_cache_watermark])                          \
    nspire_pcs.rom_count--;                                                   \
  mem_pc_cache_clear()

#define mem_pc_bios_end()                                                     \
  nspire_pcs.rom_kept = nspire_pcs.rom_count

/* translate_block_*'s check after each instruction (with the cache limit):
 * room for the tails' entries (at most one per 4 bytes of tail code) and
 * 64 more (one instruction makes at most 16; the next block's first one
 * comes before its first check). ROM: code and table apart by the usual
 * threshold for the rest of the block's code. */
#define mem_pc_full(ram_region, translation_ptr)                              \
  ((ram_region) ?                                                             \
   (nspire_pcs.ram_count + 64 + nspire_block_tail_bytes / 4 >                 \
    NSPIRE_RAM_PCS_LIMIT) :                                                   \
   ((translation_ptr) + 3 * nspire_block_tail_bytes + 64 * 8 +                \
    TRANSLATION_CACHE_LIMIT_THRESHOLD >                                       \
    (u8 *)(nspire_rom_pcs_top - nspire_pcs.rom_count)))                       \

#ifdef NSPIRE_PCWORD_CHECK
#define generate_mem_pc(value)                                                \
  nspire_mem_pc_add(translation_ptr, (value));                                \
  write32(value)                                                              \

#else
#define generate_mem_pc(value)                                                \
  nspire_mem_pc_add(translation_ptr, (value))                                 \

#endif
#else
#define generate_mem_pc(value)                                                \
  write32(value)                                                              \

#define generate_load_call_byte(tblnum)                                       \
  mem_calc_region(0);                                                         \
  generate_add_imm(reg_a2, (STORE_TBL_OFF + 68*tblnum + 4) >> 2, 0);          \
  ARM_LDR_REG_REG_SHIFT(0, reg_a2, reg_base, reg_a2, 0, 2);                   \
  ARM_BLX(0, reg_a2);                                                         \

#define generate_load_call_mbyte(tblnum, abits)                               \
  mem_calc_region(abits);                                                     \
  generate_add_imm(reg_a2, (STORE_TBL_OFF + 68*tblnum + 4) >> 2, 0);          \
  ARM_LDR_REG_REG_SHIFT(0, reg_a2, reg_base, reg_a2, 0, 2);                   \
  ARM_BLX(0, reg_a2);                                                         \

#define generate_store_call(tblnum)                                           \
  mem_calc_region(0);                                                         \
  generate_add_imm(reg_a2, (STORE_TBL_OFF + 68*tblnum + 4) >> 2, 0);          \
  ARM_LDR_REG_REG_SHIFT(0, reg_a2, reg_base, reg_a2, 0, 2);                   \
  ARM_BLX(0, reg_a2);                                                         \


#define generate_store_call_u8()        generate_store_call(0)
#define generate_store_call_u16()       generate_store_call(1)
#define generate_store_call_u32()       generate_store_call(2)
#define generate_store_call_u32_safe()  generate_store_call(3)
#define generate_load_call_u8()         generate_load_call_byte(4)
#define generate_load_call_s8()         generate_load_call_byte(5)
#define generate_load_call_u16()        generate_load_call_mbyte(6, 1)
#define generate_load_call_s16()        generate_load_call_mbyte(7, 1)
#define generate_load_call_u32()        generate_load_call_mbyte(8, 2)
#endif


#define arm_access_memory_load(mem_type)                                      \
  cycle_count += 2;                                                           \
  generate_load_call_##mem_type();                                            \
  generate_mem_pc(pc);                                                        \
  arm_generate_store_reg_pc_no_flags(reg_rv, rd)                              \

#define arm_access_memory_store(mem_type)                                     \
  cycle_count++;                                                              \
  arm_generate_load_reg_pc(reg_a1, rd, 12);                                   \
  generate_store_call_##mem_type();                                           \
  generate_mem_pc((pc + 4))                                                   \

/* Calculate the address into a0 from _rn, _rm */

#define arm_access_memory_adjust_reg_sh_up(ireg)                              \
  ARM_ADD_REG_IMMSHIFT(0, ireg, _rn, _rm, ((opcode >> 5) & 0x03),             \
   ((opcode >> 7) & 0x1F))                                                    \

#define arm_access_memory_adjust_reg_sh_down(ireg)                            \
  ARM_SUB_REG_IMMSHIFT(0, ireg, _rn, _rm, ((opcode >> 5) & 0x03),             \
   ((opcode >> 7) & 0x1F))                                                    \

#define arm_access_memory_adjust_reg_up(ireg)                                 \
  ARM_ADD_REG_REG(0, ireg, _rn, _rm)                                          \

#define arm_access_memory_adjust_reg_down(ireg)                               \
  ARM_SUB_REG_REG(0, ireg, _rn, _rm)                                          \

#define arm_access_memory_adjust_imm(op, ireg)                                \
{                                                                             \
  u32 stores[4];                                                              \
  u32 rotations[4];                                                           \
  u32 store_count = arm_disect_imm_32bit(offset, stores, rotations);          \
                                                                              \
  if(store_count > 1)                                                         \
  {                                                                           \
    ARM_##op##_REG_IMM(0, ireg, _rn, stores[0], rotations[0]);                \
    ARM_##op##_REG_IMM(0, ireg, ireg, stores[1], rotations[1]);               \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    ARM_##op##_REG_IMM(0, ireg, _rn, stores[0], rotations[0]);                \
  }                                                                           \
}                                                                             \

#define arm_access_memory_adjust_imm_up(ireg)                                 \
  arm_access_memory_adjust_imm(ADD, ireg)                                     \

#define arm_access_memory_adjust_imm_down(ireg)                               \
  arm_access_memory_adjust_imm(SUB, ireg)                                     \


#define arm_access_memory_pre(type, direction)                                \
  arm_access_memory_adjust_##type##_##direction(reg_a0)                       \

#define arm_access_memory_pre_wb(type, direction)                             \
  arm_access_memory_adjust_##type##_##direction(reg_a0);                      \
  arm_generate_store_reg(reg_a0, rn)                                          \

#define arm_access_memory_post(type, direction)                               \
  u32 _rn_dest = arm_prepare_store_reg(reg_a1, rn);                           \
  if(_rn != reg_a0)                                                           \
  {                                                                           \
    arm_generate_load_reg(reg_a0, rn);                                        \
  }                                                                           \
  arm_access_memory_adjust_##type##_##direction(_rn_dest);                    \
  arm_complete_store_reg(_rn_dest, rn)                                        \


#define arm_data_trans_reg(adjust_op, direction)                              \
  arm_decode_data_trans_reg();                                                \
  u32 _rn = arm_prepare_load_reg_pc(&translation_ptr, reg_a0, rn, pc + 8);    \
  u32 _rm = arm_prepare_load_reg(&translation_ptr, reg_a1, rm);               \
  arm_access_memory_##adjust_op(reg_sh, direction)                            \

#define arm_data_trans_imm(adjust_op, direction)                              \
  arm_decode_data_trans_imm();                                                \
  u32 _rn = arm_prepare_load_reg_pc(&translation_ptr, reg_a0, rn, pc + 8);    \
  arm_access_memory_##adjust_op(imm, direction)                               \


#define arm_data_trans_half_reg(adjust_op, direction)                         \
  arm_decode_half_trans_r();                                                  \
  u32 _rn = arm_prepare_load_reg_pc(&translation_ptr, reg_a0, rn, pc + 8);    \
  u32 _rm = arm_prepare_load_reg(&translation_ptr, reg_a1, rm);               \
  arm_access_memory_##adjust_op(reg, direction)                               \

#define arm_data_trans_half_imm(adjust_op, direction)                         \
  arm_decode_half_trans_of();                                                 \
  u32 _rn = arm_prepare_load_reg_pc(&translation_ptr, reg_a0, rn, pc + 8);    \
  arm_access_memory_##adjust_op(imm, direction)                               \


#define arm_access_memory(access_type, direction, adjust_op, mem_type,        \
 offset_type)                                                                 \
{                                                                             \
  arm_data_trans_##offset_type(adjust_op, direction);                         \
  arm_access_memory_##access_type(mem_type);                                  \
}                                                                             \


#define word_bit_count(word)                                                  \
  (bit_count[word >> 8] + bit_count[word & 0xFF])                             \

#ifdef NSPIRE_BUILD
/* Block transfers (push/pop, ldm/stm) on IWRAM, where GBA games keep their
 * stack: one check that all the words are in IWRAM, then one load or store
 * per register, instead of a call through the memory dispatch for each
 * register (about 16 instructions apiece; every function call and return,
 * and every interrupt, does some). Anything else runs the per-register code
 * as before, which follows: another region, words running past the end of
 * IWRAM's mirror, or a store whose last word holds translated code (the
 * per-register code checks that word for self-modifying code, and stores
 * the same values again).
 * nspire_block_fast is 0 unless IWRAM's data is NSPIRE_IWRAM_DATA below
 * reg_base, as arm_stub.S's .bss lays it out (checked in init_emitter).
 *
 * The check is a call (2026-11-15, core-12): "bl nspire_block_check_K ; b
 * <per-register code>", the loads or stores after it. arm_stub.S's
 * nspire_block_check (loads, push {..., lr}) and nspire_block_check_tag
 * (the other stores: the last word's tag is checked there now, before the
 * stores, where it was after them; the per-register code stores the same
 * values either way and IWRAM's stores never read the old ones) make the
 * tests the 9 + 4 inline words made, keep the flags and return past the b
 * with r0 = the host address of the first word, or to the b. Entries are 8
 * bytes apart, K = 1-16 words. r9 is kept; r0-r2 and lr are clobbered (no
 * GBA register lives in them; ARM's store write-back loads r2 after). */
#define NSPIRE_IWRAM_DATA 0xA0000
static u32 nspire_block_fast;

void nspire_block_check(void);
void nspire_block_check_tag(void);

#define nspire_block_fast_check(words, entries)                               \
  ARM_BL(0, arm_relative_offset(translation_ptr,                              \
   (u32)(entries) + 8 * ((words) - 1)));                                      \
  generate_branch_filler(ARMCOND_AL, fast_slow)                               \

#define nspire_block_fast_store(allocation, reg_index, word_offset)           \
{                                                                             \
  u32 fast_src = allocation[reg_index];                                       \
  if(fast_src != mem_reg)                                                     \
  {                                                                           \
    ARM_STR_IMM(0, fast_src, reg_a0, (word_offset));                          \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    ARM_LDR_IMM(0, reg_a1, reg_base, (reg_index) * 4);                        \
    ARM_STR_IMM(0, reg_a1, reg_a0, (word_offset));                            \
  }                                                                           \
}                                                                             \

#define nspire_block_fast_load(allocation, reg_index, word_offset)            \
{                                                                             \
  u32 fast_dst = allocation[reg_index];                                       \
  if(fast_dst != mem_reg)                                                     \
  {                                                                           \
    ARM_LDR_IMM(0, fast_dst, reg_a0, (word_offset));                          \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    ARM_LDR_IMM(0, reg_a1, reg_a0, (word_offset));                            \
    ARM_STR_IMM(0, reg_a1, reg_base, (reg_index) * 4);                        \
  }                                                                           \
}                                                                             \

/* After the fast path, on past the per-register code (joined at
 * nspire_block_fast_join), which the check's b goes to. This inline form
 * is for a block with NSPIRE_BLOCK_TAILS transfers already (see
 * nspire_block_tail_type); the others end with the last load or store. */
#define nspire_block_fast_end_inline()                                        \
  generate_branch_filler(ARMCOND_AL, fast_done);                              \
  generate_branch_patch_unconditional(fast_slow, translation_ptr)             \

#define nspire_block_fast_end_tail()                                          \

/* The per-register code out of line (2026-11-15, core-11): a transfer that
 * takes the fast path (nearly all) used to run "str r9, [REG_SAVE]" and a
 * taken "b" over the per-register code, and fetched past its dead words.
 * Now the per-register code goes after the block's exits, in the order of
 * the transfers: "str r9, [REG_SAVE]", the same code with the same PC
 * words, "b" back to where the fast path continues. r9 holds the aligned
 * base at the check's b (no GBA register lives in r9, the check keeps it).
 * The flags are the GBA's there (the check keeps them; before core-12 the
 * tail started with "msr cpsr_f, lr"). The cycles are counted at the
 * transfer, once per register as before. Up to NSPIRE_BLOCK_TAILS a block;
 * translation_cache_limit is checked with nspire_block_tail_bytes, a bound
 * of their size (6 words a register: the PC as a 4-word constant, a
 * write-back of 3 words and the extra word for pop {pc} or push {lr} fit in
 * the 16 more). Translation never nests before the tails are out (the
 * linked exits translate after them). */
#define NSPIRE_BLOCK_TAILS 32

typedef struct
{
  u8 *slow;                               /* the check's b                 */
  u8 *join;
  u32 pc;
  u32 opcode;
  u32 kind;
} nspire_block_tail_type;

/* 4 KB in all, so that the .bss after it keeps its place in a 4 KB way (the
 * caches see the layout modulo 4 KB: 904 bytes moved the renderer's data and
 * cost Dewford's drawing 0.1 ms a frame in the model). */
/* speed10b: and the places of the branch offsets encoded since the RAM
 * block being made started (nspire_rel_offset; reloc_n past
 * NSPIRE_RELOC_MAX: too many, the block isn't kept), in the padding. */
#define NSPIRE_RELOC_MAX 800
static struct
{
  u32 count;
  u32 bytes;
  nspire_block_tail_type tails[NSPIRE_BLOCK_TAILS];
  u32 reloc_n;
  u32 reloc[NSPIRE_RELOC_MAX];
  u8 pad[4096 - 8 - NSPIRE_BLOCK_TAILS * sizeof(nspire_block_tail_type) -
   4 - NSPIRE_RELOC_MAX * 4];
} nspire_block_tail_state;

#define nspire_block_tails      nspire_block_tail_state.tails
#define nspire_block_tail_count nspire_block_tail_state.count
#define nspire_block_tail_bytes nspire_block_tail_state.bytes
#define nspire_reloc_n          nspire_block_tail_state.reloc_n
#define nspire_reloc            nspire_block_tail_state.reloc

/* arm_relative_offset: a B/BL's offset field from source to target (out of
 * line: one call where the expression was, the translator's code no
 * bigger), source logged. */
static __attribute__((noinline)) u32 nspire_rel_offset(u32 source, u32 target)
{
  u32 n = nspire_reloc_n;
  if(n < NSPIRE_RELOC_MAX)
    nspire_reloc[n] = source;
  if(n <= NSPIRE_RELOC_MAX)
    nspire_reloc_n = n + 1;
  return ((target - source - 8) >> 2) & 0xFFFFFF;
}

#define nspire_block_tail_arm_load_no       0
#define nspire_block_tail_arm_load_up       0
#define nspire_block_tail_arm_load_down     0
#define nspire_block_tail_arm_store_no      1
#define nspire_block_tail_arm_store_up      2
#define nspire_block_tail_arm_store_down    3
#define nspire_block_tail_thumb_load_no     4
#define nspire_block_tail_thumb_load_up     4
#define nspire_block_tail_thumb_load_pop_pc 5
#define nspire_block_tail_thumb_store_no    6
#define nspire_block_tail_thumb_store_up    6
#define nspire_block_tail_thumb_store_push_lr 7

#define nspire_block_tail_add(kind_name, registers)                           \
{                                                                             \
  nspire_block_tail_type *tail =                                              \
   &nspire_block_tails[nspire_block_tail_count++];                            \
  tail->slow = fast_slow;                                                     \
  tail->join = translation_ptr;                                               \
  tail->pc = pc;                                                              \
  tail->opcode = opcode;                                                      \
  tail->kind = nspire_block_tail_##kind_name;                                 \
  nspire_block_tail_bytes += 4 * (6 * (registers) + 16);                      \
}                                                                             \

#define generate_block_tails_start()                                          \
  nspire_block_tail_count = 0;                                                \
  nspire_block_tail_bytes = 0                                                 \

#define block_tails_bytes nspire_block_tail_bytes

#define nspire_block_fast_join()                                              \
  if(fast_done)                                                               \
  {                                                                           \
    generate_branch_patch_unconditional(fast_done, translation_ptr);          \
    fast_done = NULL;                                                         \
  }                                                                           \

#define nspire_block_fast_vars()                                              \
  u8 *fast_slow = NULL;                                                       \
  u8 *fast_done = NULL                                                        \

/* ARM: stores keep their write-back for after the stores (as the
 * per-register code does it with the last one); loads had it before. */
#define nspire_arm_block_fast_store_end(writeback_type)                       \
  arm_block_memory_writeback_post_store(writeback_type)                       \

#define nspire_arm_block_fast_load_end(writeback_type)                        \

/* The stores' check looks at the last word's tag, the loads' doesn't. */
#define nspire_arm_block_check_store nspire_block_check_tag
#define nspire_arm_block_check_load  nspire_block_check

#define nspire_arm_block_fast_store_pc(word_offset)                           \
  generate_load_pc(reg_a1, (pc + 12));                                        \
  ARM_STR_IMM(0, reg_a1, reg_a0, (word_offset))                               \

/* The PC last: r0 for the indirect branch that follows. */
#define nspire_arm_block_fast_load_pc(word_offset)                            \
  ARM_LDR_IMM(0, reg_a0, reg_a0, (word_offset));                              \
  arm_generate_store_reg(reg_a0, 15)                                          \

/* Fast path with the per-register code at the block's tail. */
#define nspire_arm_block_tail_ok()                                            \
  (nspire_block_fast && reg_list && (rn != 15) &&                             \
   (nspire_block_tail_count < NSPIRE_BLOCK_TAILS))                            \

#define nspire_arm_block_fast(access_type, writeback_type, end_type)          \
  if(nspire_block_fast && reg_list && (rn != 15))                             \
  {                                                                           \
    u32 fast_offset = 0, fast_i;                                              \
    nspire_block_fast_check(word_bit_count(reg_list),                         \
     nspire_arm_block_check_##access_type);                                   \
    for(fast_i = 0; fast_i < 16; fast_i++)                                    \
    {                                                                         \
      if((reg_list >> fast_i) & 0x01)                                         \
      {                                                                       \
        if(fast_i == 15)                                                      \
        {                                                                     \
          nspire_arm_block_fast_##access_type##_pc(fast_offset);              \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          nspire_block_fast_##access_type(arm_register_allocation, fast_i,    \
           fast_offset);                                                      \
        }                                                                     \
        fast_offset += 4;                                                     \
      }                                                                       \
    }                                                                         \
    nspire_arm_block_fast_##access_type##_end(writeback_type);                \
    nspire_block_fast_end_##end_type();                                       \
  }                                                                           \

#else
#define nspire_block_fast_vars()
#define nspire_block_fast_join()
#define nspire_arm_block_tail_ok() 0
#define nspire_arm_block_fast(access_type, writeback_type, end_type)
#define nspire_thumb_block_tail_ok() 0
#define nspire_thumb_block_fast(access_type, post_op, end_type)
#define nspire_block_tail_add(kind_name, registers)
#endif

/* TODO: Make these use cached registers. Implement iwram_stack_optimize. */

#define arm_block_memory_load()                                               \
  generate_load_call_u32();                                                   \
  generate_mem_pc((pc + 8));                                                  \
  arm_generate_store_reg(reg_rv, i)                                           \

#define arm_block_memory_store()                                              \
  arm_generate_load_reg_pc(reg_a1, i, 8);                                     \
  generate_store_call_u32_safe()                                              \

#define arm_block_memory_final_load(writeback_type)                           \
  arm_block_memory_load()                                                     \

#define arm_block_memory_final_store(writeback_type)                          \
  arm_generate_load_reg_pc(reg_a1, i, 12);                                    \
  arm_block_memory_writeback_post_store(writeback_type);                      \
  generate_store_call_u32();                                                  \
  generate_mem_pc((pc + 4))                                                   \

#define arm_block_memory_adjust_pc_store()                                    \

#define arm_block_memory_adjust_pc_load()                                     \
  if(reg_list & 0x8000)                                                       \
  {                                                                           \
    generate_indirect_branch_arm();                                           \
  }                                                                           \

#define arm_block_memory_offset_down_a()                                      \
  generate_sub_imm(reg_s0, ((word_bit_count(reg_list) * 4) - 4), 0)           \

#define arm_block_memory_offset_down_b()                                      \
  generate_sub_imm(reg_s0, (word_bit_count(reg_list) * 4), 0)                 \

#define arm_block_memory_offset_no()                                          \

#define arm_block_memory_offset_up()                                          \
  generate_add_imm(reg_s0, 4, 0)                                              \

#define arm_block_memory_writeback_down()                                     \
  arm_generate_load_reg(reg_a2, rn);                                          \
  generate_sub_imm(reg_a2, (word_bit_count(reg_list) * 4), 0);                \
  arm_generate_store_reg(reg_a2, rn)                                          \

#define arm_block_memory_writeback_up()                                       \
  arm_generate_load_reg(reg_a2, rn);                                          \
  generate_add_imm(reg_a2, (word_bit_count(reg_list) * 4), 0);                \
  arm_generate_store_reg(reg_a2, rn)                                          \

#define arm_block_memory_writeback_no()

/* Only emit writeback if the register is not in the list */

#define arm_block_memory_writeback_pre_load(writeback_type)                   \
  if(!((reg_list >> rn) & 0x01))                                              \
  {                                                                           \
    arm_block_memory_writeback_##writeback_type();                            \
  }                                                                           \

#define arm_block_memory_writeback_post_store(writeback_type)                 \
  arm_block_memory_writeback_##writeback_type()                               \

#define arm_block_memory_writeback_pre_store(writeback_type)

/* The per-register code: base (aligned) in REG_SAVE, flags as the GBA's. */
#define arm_block_memory_slow(access_type, writeback_type)                    \
  for(i = 0; i < 16; i++)                                                     \
  {                                                                           \
    if((reg_list >> i) & 0x01)                                                \
    {                                                                         \
      cycle_count++;                                                          \
      arm_generate_load_reg(reg_s0, REG_SAVE);                                \
      generate_add_reg_reg_imm(reg_a0, reg_s0, offset, 0);                    \
      if(reg_list & ~((2 << i) - 1))                                          \
      {                                                                       \
        arm_block_memory_##access_type();                                     \
        offset += 4;                                                          \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        arm_block_memory_final_##access_type(writeback_type);                 \
        break;                                                                \
      }                                                                       \
    }                                                                         \
  }                                                                           \

#define arm_block_memory(access_type, offset_type, writeback_type, s_bit)     \
{                                                                             \
  arm_decode_block_trans();                                                   \
  u32 offset = 0;                                                             \
  u32 i;                                                                      \
  nspire_block_fast_vars();                                                   \
                                                                              \
  arm_generate_load_reg(reg_s0, rn);                                          \
  arm_block_memory_offset_##offset_type();                                    \
  arm_block_memory_writeback_pre_##access_type(writeback_type);               \
  ARM_BIC_REG_IMM(0, reg_s0, reg_s0, 0x03, 0);                                \
  if(nspire_arm_block_tail_ok())                                              \
  {                                                                           \
    nspire_arm_block_fast(access_type, writeback_type, tail);                 \
    cycle_count += word_bit_count(reg_list);                                  \
    nspire_block_tail_add(arm_##access_type##_##writeback_type,               \
     word_bit_count(reg_list));                                               \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    arm_generate_store_reg(reg_s0, REG_SAVE);                                 \
    nspire_arm_block_fast(access_type, writeback_type, inline);               \
    arm_block_memory_slow(access_type, writeback_type);                       \
    nspire_block_fast_join();                                                 \
  }                                                                           \
                                                                              \
  arm_block_memory_adjust_pc_##access_type();                                 \
  (void)offset;                                                               \
}                                                                             \

#define arm_swap(type)                                                        \
{                                                                             \
  arm_decode_swap();                                                          \
  cycle_count += 3;                                                           \
  arm_generate_load_reg(reg_a0, rn);                                          \
  generate_load_call_##type();                                                \
  generate_mem_pc((pc + 8));                                                  \
  generate_mov(reg_a2, reg_rv);                                               \
  arm_generate_load_reg(reg_a0, rn);                                          \
  arm_generate_load_reg(reg_a1, rm);                                          \
  arm_generate_store_reg(reg_a2, rd);                                         \
  generate_store_call_##type();                                               \
  generate_mem_pc((pc + 4));                                                  \
}                                                                             \


#define thumb_generate_op_reg(name, _rd, _rs, _rn)                            \
  u32 __rm = thumb_prepare_load_reg(&translation_ptr, reg_rm, _rn);           \
  generate_op_##name##_reg_immshift(__rd, __rn, __rm, ARMSHIFT_LSL, 0)        \

#define thumb_generate_op_imm(name, _rd, _rs, imm_)                           \
{                                                                             \
  u32 imm_ror = 0;                                                            \
  generate_op_##name##_imm(__rd, __rn);                                       \
}                                                                             \


#define thumb_data_proc(type, name, op_type, _rd, _rs, _rn)                   \
{                                                                             \
  thumb_decode_##type();                                                      \
  u32 __rn = thumb_prepare_load_reg(&translation_ptr, reg_rn, _rs);           \
  u32 __rd = thumb_prepare_store_reg(reg_rd, _rd);                            \
  thumb_generate_op_##op_type(name, _rd, _rs, _rn);                           \
  thumb_complete_store_reg(__rd, _rd);                                        \
}                                                                             \

#define thumb_data_proc_test(type, name, op_type, _rd, _rs)                   \
{                                                                             \
  thumb_decode_##type();                                                      \
  u32 __rn = thumb_prepare_load_reg(&translation_ptr, reg_rn, _rd);           \
  thumb_generate_op_##op_type(name, 0, _rd, _rs);                             \
}                                                                             \

#define thumb_data_proc_unary(type, name, op_type, _rd, _rs)                  \
{                                                                             \
  thumb_decode_##type();                                                      \
  u32 __rd = thumb_prepare_store_reg(reg_rd, _rd);                            \
  thumb_generate_op_##op_type(name, _rd, 0, _rs);                             \
  thumb_complete_store_reg(__rd, _rd);                                        \
}                                                                             \


#define complete_store_reg_pc_thumb()                                         \
  if(rd == 15)                                                                \
  {                                                                           \
    generate_indirect_branch_cycle_update(thumb);                             \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    thumb_complete_store_reg(_rd, rd);                                        \
  }                                                                           \

#define thumb_data_proc_hi(name)                                              \
{                                                                             \
  thumb_decode_hireg_op();                                                    \
  u32 _rd = thumb_prepare_load_reg_pc(&translation_ptr, reg_rd, rd, pc + 4);  \
  u32 _rs = thumb_prepare_load_reg_pc(&translation_ptr, reg_rn, rs, pc + 4);  \
  generate_op_##name##_reg_immshift(_rd, _rd, _rs, ARMSHIFT_LSL, 0);          \
  complete_store_reg_pc_thumb();                                              \
}                                                                             \

#define thumb_data_proc_test_hi(name)                                         \
{                                                                             \
  thumb_decode_hireg_op();                                                    \
  u32 _rd = thumb_prepare_load_reg_pc(&translation_ptr, reg_rd, rd, pc + 4);  \
  u32 _rs = thumb_prepare_load_reg_pc(&translation_ptr, reg_rn, rs, pc + 4);  \
  generate_op_##name##_reg_immshift(0, _rd, _rs, ARMSHIFT_LSL, 0);            \
}                                                                             \

#define thumb_data_proc_mov_hi()                                              \
{                                                                             \
  thumb_decode_hireg_op();                                                    \
  u32 _rs = thumb_prepare_load_reg_pc(&translation_ptr, reg_rn, rs, pc + 4);  \
  u32 _rd = thumb_prepare_store_reg(reg_rd, rd);                              \
  ARM_MOV_REG_REG(0, _rd, _rs);                                               \
  complete_store_reg_pc_thumb();                                              \
}                                                                             \



#define thumb_load_pc(_rd)                                                    \
{                                                                             \
  thumb_decode_imm();                                                         \
  u32 __rd = thumb_prepare_store_reg(reg_rd, _rd);                            \
  generate_load_pc(__rd, (((pc & ~2) + 4) + (imm * 4)));                      \
  thumb_complete_store_reg(__rd, _rd);                                        \
}                                                                             \

#define thumb_load_sp(_rd)                                                    \
{                                                                             \
  thumb_decode_imm();                                                         \
  u32 __sp = thumb_prepare_load_reg(&translation_ptr, reg_a0, REG_SP);        \
  u32 __rd = thumb_prepare_store_reg(reg_a0, _rd);                            \
  ARM_ADD_REG_IMM(0, __rd, __sp, imm, arm_imm_lsl_to_rot(2));                 \
  thumb_complete_store_reg(__rd, _rd);                                        \
}                                                                             \

#define thumb_adjust_sp_up()                                                  \
  ARM_ADD_REG_IMM(0, _sp, _sp, imm, arm_imm_lsl_to_rot(2))                    \

#define thumb_adjust_sp_down()                                                \
  ARM_SUB_REG_IMM(0, _sp, _sp, imm, arm_imm_lsl_to_rot(2))                    \

#define thumb_adjust_sp(direction)                                            \
{                                                                             \
  thumb_decode_add_sp();                                                      \
  u32 _sp = thumb_prepare_load_reg(&translation_ptr, reg_a0, REG_SP);         \
  thumb_adjust_sp_##direction();                                              \
  thumb_complete_store_reg(_sp, REG_SP);                                      \
}                                                                             \

#define generate_op_lsl_reg(_rd, _rm, _rs)                                    \
  generate_op_movs_reg_regshift(_rd, 0, _rm, ARMSHIFT_LSL, _rs)               \

#define generate_op_lsr_reg(_rd, _rm, _rs)                                    \
  generate_op_movs_reg_regshift(_rd, 0, _rm, ARMSHIFT_LSR, _rs)               \

#define generate_op_asr_reg(_rd, _rm, _rs)                                    \
  generate_op_movs_reg_regshift(_rd, 0, _rm, ARMSHIFT_ASR, _rs)               \

#define generate_op_ror_reg(_rd, _rm, _rs)                                    \
  generate_op_movs_reg_regshift(_rd, 0, _rm, ARMSHIFT_ROR, _rs)               \


#define generate_op_lsl_imm(_rd, _rm)                                         \
  generate_op_movs_reg_immshift(_rd, 0, _rm, ARMSHIFT_LSL, imm)               \

#define generate_op_lsr_imm(_rd, _rm)                                         \
  generate_op_movs_reg_immshift(_rd, 0, _rm, ARMSHIFT_LSR, imm)               \

#define generate_op_asr_imm(_rd, _rm)                                         \
  generate_op_movs_reg_immshift(_rd, 0, _rm, ARMSHIFT_ASR, imm)               \

#define generate_op_ror_imm(_rd, _rm)                                         \
  generate_op_movs_reg_immshift(_rd, 0, _rm, ARMSHIFT_ROR, imm)               \


#define thumb_generate_shift_reg(op_type)                                     \
  u32 __rm = thumb_prepare_load_reg(&translation_ptr, reg_rd, rd);            \
  u32 __rs = thumb_prepare_load_reg(&translation_ptr, reg_rs, rs);            \
  generate_op_##op_type##_reg(__rd, __rm, __rs)                               \

#define thumb_generate_shift_imm(op_type)                                     \
  u32 __rs = thumb_prepare_load_reg(&translation_ptr, reg_rs, rs);            \
  generate_op_##op_type##_imm(__rd, __rs)                                     \


#define thumb_shift(decode_type, op_type, value_type)                         \
{                                                                             \
  thumb_decode_##decode_type();                                               \
  u32 __rd = thumb_prepare_store_reg(reg_rd, rd);                             \
  thumb_generate_shift_##value_type(op_type);                                 \
  thumb_complete_store_reg(__rd, rd);                                         \
}                                                                             \

/* Operation types: imm, mem_reg, mem_imm */

#define thumb_load_pc_pool_const(reg_rd, value)                               \
  u32 rgdst = thumb_prepare_store_reg(reg_a0, reg_rd);                        \
  generate_load_pc(rgdst, (value));                                           \
  thumb_complete_store_reg(rgdst, reg_rd)

#define thumb_access_memory_load(mem_type, _rd)                               \
  cycle_count += 2;                                                           \
  generate_load_call_##mem_type();                                            \
  generate_mem_pc(pc);                                                        \
  thumb_generate_store_reg(reg_rv, _rd)                                       \

#define thumb_access_memory_store(mem_type, _rd)                              \
  cycle_count++;                                                              \
  thumb_generate_load_reg(reg_a1, _rd);                                       \
  generate_store_call_##mem_type();                                           \
  generate_mem_pc((pc + 2))                                                   \

#define thumb_access_memory_generate_address_pc_relative(offset, _rb, _ro)    \
  generate_load_pc(reg_a0, (offset))                                          \

#define thumb_access_memory_generate_address_reg_imm(offset, _rb, _ro)        \
  u32 __rb = thumb_prepare_load_reg(&translation_ptr, reg_a0, _rb);           \
  ARM_ADD_REG_IMM(0, reg_a0, __rb, offset, 0)                                 \

#define thumb_access_memory_generate_address_reg_imm_sp(offset, _rb, _ro)     \
  u32 __rb = thumb_prepare_load_reg(&translation_ptr, reg_a0, _rb);           \
  ARM_ADD_REG_IMM(0, reg_a0, __rb, offset, arm_imm_lsl_to_rot(2))             \

#define thumb_access_memory_generate_address_reg_reg(offset, _rb, _ro)        \
  u32 __rb = thumb_prepare_load_reg(&translation_ptr, reg_a0, _rb);           \
  u32 __ro = thumb_prepare_load_reg(&translation_ptr, reg_a1, _ro);           \
  ARM_ADD_REG_REG(0, reg_a0, __rb, __ro)                                      \

#define thumb_access_memory(access_type, op_type, _rd, _rb, _ro,              \
 address_type, offset, mem_type)                                              \
{                                                                             \
  thumb_decode_##op_type();                                                   \
  thumb_access_memory_generate_address_##address_type(offset, _rb, _ro);      \
  thumb_access_memory_##access_type(mem_type, _rd);                           \
}                                                                             \

/* TODO: Make these use cached registers. Implement iwram_stack_optimize. */

#define thumb_block_address_preadjust_down()                                  \
  generate_sub_imm(reg_s0, (bit_count[reg_list] * 4), 0)                      \

#define thumb_block_address_preadjust_push_lr()                               \
  generate_sub_imm(reg_s0, ((bit_count[reg_list] + 1) * 4), 0)                \

#define thumb_block_address_preadjust_no()                                    \

#define thumb_block_address_postadjust_no(base_reg)                           \
  thumb_generate_store_reg(reg_s0, base_reg)                                  \

#define thumb_block_address_postadjust_up(base_reg)                           \
  generate_add_reg_reg_imm(reg_a0, reg_s0, (bit_count[reg_list] * 4), 0);     \
  thumb_generate_store_reg(reg_a0, base_reg)                                  \

#define thumb_block_address_postadjust_pop_pc(base_reg)                       \
  generate_add_reg_reg_imm(reg_a0, reg_s0,                                    \
   ((bit_count[reg_list] + 1) * 4), 0);                                       \
  thumb_generate_store_reg(reg_a0, base_reg)                                  \

#define thumb_block_address_postadjust_push_lr(base_reg)                      \
  thumb_generate_store_reg(reg_s0, base_reg)                                  \

#define thumb_block_memory_extra_no()                                         \

#define thumb_block_memory_extra_up()                                         \

#define thumb_block_memory_extra_down()                                       \

/* The slow PC load; the join comes before the branch (the PC in r0 either
 * way), thumb_block_memory_after_pop_pc. */
#define thumb_block_memory_extra_pop_pc()                                     \
  thumb_generate_load_reg(reg_s0, REG_SAVE);                                  \
  generate_add_reg_reg_imm(reg_a0, reg_s0, (bit_count[reg_list] * 4), 0);     \
  generate_load_call_u32();                                                   \
  generate_mem_pc((pc + 4))                                                   \

#define thumb_block_memory_after_no()                                         \

#define thumb_block_memory_after_up()                                         \

#define thumb_block_memory_after_push_lr()                                    \

#define thumb_block_memory_after_pop_pc()                                     \
  generate_indirect_branch_cycle_update(thumb)                                \

#define thumb_block_memory_extra_push_lr(base_reg)                            \
  thumb_generate_load_reg(reg_s0, REG_SAVE);                                  \
  generate_add_reg_reg_imm(reg_a0, reg_s0, (bit_count[reg_list] * 4), 0);     \
  thumb_generate_load_reg(reg_a1, REG_LR);                                    \
  generate_store_call_u32_safe()

#define thumb_block_memory_load()                                             \
  generate_load_call_u32();                                                   \
  generate_mem_pc((pc + 4));                                                  \
  thumb_generate_store_reg(reg_rv, i)                                         \

#define thumb_block_memory_store()                                            \
  thumb_generate_load_reg(reg_a1, i);                                         \
  generate_store_call_u32_safe()

#define thumb_block_memory_final_load()                                       \
  thumb_block_memory_load()                                                   \

#define thumb_block_memory_final_store()                                      \
  thumb_generate_load_reg(reg_a1, i);                                         \
  generate_store_call_u32();                                                  \
  generate_mem_pc((pc + 2))                                                   \

#define thumb_block_memory_final_no(access_type)                              \
  thumb_block_memory_final_##access_type()                                    \

#define thumb_block_memory_final_up(access_type)                              \
  thumb_block_memory_final_##access_type()                                    \

#define thumb_block_memory_final_down(access_type)                            \
  thumb_block_memory_final_##access_type()                                    \

#define thumb_block_memory_final_push_lr(access_type)                         \
  thumb_block_memory_##access_type()                                          \

#define thumb_block_memory_final_pop_pc(access_type)                          \
  thumb_block_memory_##access_type()                                          \

#ifdef NSPIRE_BUILD
/* Thumb block transfers on IWRAM (see nspire_block_fast_check). The words
 * after the listed registers: LR for push {..., lr}, the PC for pop {...,
 * pc}. The per-register code checks the last word of push (without LR) and
 * stmia for self-modifying code, never push {..., lr}'s; so does the check
 * (below). */
#define nspire_thumb_block_words_no      0
#define nspire_thumb_block_words_up      0
#define nspire_thumb_block_words_down    0
#define nspire_thumb_block_words_push_lr 1
#define nspire_thumb_block_words_pop_pc  1

/* The check: with the last word's tag for push and stmia (the last listed
 * register's word), without for push {..., lr} and the loads. */
#define nspire_thumb_block_check_store_no      nspire_block_check_tag
#define nspire_thumb_block_check_store_up      nspire_block_check_tag
#define nspire_thumb_block_check_store_down    nspire_block_check_tag
#define nspire_thumb_block_check_store_push_lr nspire_block_check
#define nspire_thumb_block_check_load_no       nspire_block_check
#define nspire_thumb_block_check_load_up       nspire_block_check
#define nspire_thumb_block_check_load_down     nspire_block_check
#define nspire_thumb_block_check_load_pop_pc   nspire_block_check

#define nspire_thumb_block_fast_tail_no(access_type, word_offset)             \

#define nspire_thumb_block_fast_tail_up(access_type, word_offset)             \

#define nspire_thumb_block_fast_tail_down(access_type, word_offset)           \

#define nspire_thumb_block_fast_tail_push_lr(access_type, word_offset)        \
  nspire_block_fast_store(thumb_register_allocation, REG_LR, (word_offset))   \

#define nspire_thumb_block_fast_tail_pop_pc(access_type, word_offset)         \
  ARM_LDR_IMM(0, reg_a0, reg_a0, (word_offset))                               \

#define nspire_thumb_block_tail_ok()                                          \
  (nspire_block_fast && reg_list &&                                           \
   (nspire_block_tail_count < NSPIRE_BLOCK_TAILS))                            \

#define nspire_thumb_block_fast(access_type, post_op, end_type)               \
  if(nspire_block_fast && reg_list)                                           \
  {                                                                           \
    u32 fast_offset = 0, fast_i;                                              \
    nspire_block_fast_check(bit_count[reg_list] +                             \
     nspire_thumb_block_words_##post_op,                                      \
     nspire_thumb_block_check_##access_type##_##post_op);                     \
    for(fast_i = 0; fast_i < 8; fast_i++)                                     \
    {                                                                         \
      if((reg_list >> fast_i) & 0x01)                                         \
      {                                                                       \
        nspire_block_fast_##access_type(thumb_register_allocation, fast_i,    \
         fast_offset);                                                        \
        fast_offset += 4;                                                     \
      }                                                                       \
    }                                                                         \
    nspire_thumb_block_fast_tail_##post_op(access_type, fast_offset);         \
    nspire_block_fast_end_##end_type();                                       \
  }                                                                           \

#endif

/* The per-register code: base (adjusted, aligned) in REG_SAVE. */
#define thumb_block_memory_slow(access_type, post_op)                         \
  for(i = 0; i < 8; i++)                                                      \
  {                                                                           \
    if((reg_list >> i) & 0x01)                                                \
    {                                                                         \
      cycle_count++;                                                          \
      thumb_generate_load_reg(reg_s0, REG_SAVE);                              \
      generate_add_reg_reg_imm(reg_a0, reg_s0, offset, 0);                    \
      if(reg_list & ~((2 << i) - 1))                                          \
      {                                                                       \
        thumb_block_memory_##access_type();                                   \
        offset += 4;                                                          \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        thumb_block_memory_final_##post_op(access_type);                      \
        break;                                                                \
      }                                                                       \
    }                                                                         \
  }                                                                           \
                                                                              \
  thumb_block_memory_extra_##post_op()                                        \

#define thumb_block_memory(access_type, pre_op, post_op, base_reg)            \
{                                                                             \
  thumb_decode_rlist();                                                       \
  u32 i;                                                                      \
  u32 offset = 0;                                                             \
  nspire_block_fast_vars();                                                   \
                                                                              \
  thumb_generate_load_reg(reg_s0, base_reg);                                  \
  ARM_BIC_REG_IMM(0, reg_s0, reg_s0, 0x03, 0);                                \
  thumb_block_address_preadjust_##pre_op();                                   \
  thumb_block_address_postadjust_##post_op(base_reg);                         \
  if(nspire_thumb_block_tail_ok())                                            \
  {                                                                           \
    nspire_thumb_block_fast(access_type, post_op, tail);                      \
    cycle_count += bit_count[reg_list];                                       \
    nspire_block_tail_add(thumb_##access_type##_##post_op,                    \
     bit_count[reg_list]);                                                    \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    thumb_generate_store_reg(reg_s0, REG_SAVE);                               \
    nspire_thumb_block_fast(access_type, post_op, inline);                    \
    thumb_block_memory_slow(access_type, post_op);                            \
    nspire_block_fast_join();                                                 \
  }                                                                           \
                                                                              \
  thumb_block_memory_after_##post_op();                                       \
  (void)offset;                                                               \
}                                                                             \

#ifdef NSPIRE_BUILD
/* One transfer's per-register code at the block's tail (see
 * nspire_block_tail_type); returns the new translation_ptr. */
static u8 *nspire_block_tail_emit(u8 *translation_ptr,
 const nspire_block_tail_type *tail)
{
  u32 opcode = tail->opcode;
  u32 pc = tail->pc;
  u32 cycle_count = 0;                    /* counted at the transfer       */
  u32 offset = 0;
  u32 i;
  u8 *join_branch;

  generate_branch_patch_unconditional(tail->slow, translation_ptr);
  arm_generate_store_reg(reg_s0, REG_SAVE);

  switch(tail->kind)
  {
    case nspire_block_tail_arm_load_no:
    {
      arm_decode_block_trans();
      arm_block_memory_slow(load, no);
      (void)rn;
      break;
    }
    case nspire_block_tail_arm_store_no:
    {
      arm_decode_block_trans();
      arm_block_memory_slow(store, no);
      (void)rn;
      break;
    }
    case nspire_block_tail_arm_store_up:
    {
      arm_decode_block_trans();
      arm_block_memory_slow(store, up);
      break;
    }
    case nspire_block_tail_arm_store_down:
    {
      arm_decode_block_trans();
      arm_block_memory_slow(store, down);
      break;
    }
    case nspire_block_tail_thumb_load_up:
    {
      thumb_decode_rlist();
      thumb_block_memory_slow(load, up);
      break;
    }
    case nspire_block_tail_thumb_load_pop_pc:
    {
      thumb_decode_rlist();
      thumb_block_memory_slow(load, pop_pc);
      break;
    }
    case nspire_block_tail_thumb_store_up:
    {
      thumb_decode_rlist();
      thumb_block_memory_slow(store, up);
      break;
    }
    case nspire_block_tail_thumb_store_push_lr:
    {
      thumb_decode_rlist();
      thumb_block_memory_slow(store, push_lr);
      break;
    }
  }

  generate_branch_filler(ARMCOND_AL, join_branch);
  generate_branch_patch_unconditional(join_branch, tail->join);
  (void)cycle_count;
  (void)offset;
  return translation_ptr;
}

/* After the block's exits: every transfer's per-register code. */
#define generate_block_tails()                                                \
{                                                                             \
  u32 tail_i;                                                                 \
  for(tail_i = 0; tail_i < nspire_block_tail_count; tail_i++)                 \
  {                                                                           \
    translation_ptr = nspire_block_tail_emit(translation_ptr,                 \
     &nspire_block_tails[tail_i]);                                            \
  }                                                                           \
  nspire_block_tail_count = 0;                                                \
  nspire_block_tail_bytes = 0;                                                \
}                                                                             \

#endif

#ifdef NSPIRE_BUILD
/* core-16: a pure idle loop's branch, and its exit disarmed. */
#define thumb_conditional_branch(condition)                                   \
{                                                                             \
  u32 idle_pure = nspire_idle_pure(                                           \
   block_exits[block_exit_position].branch_target, (opcode >> 8) & 0xF);      \
  generate_cycle_update();                                                    \
  generate_branch_filler(condition_opposite_##condition, backpatch_address);  \
  if(idle_pure)                                                               \
  {                                                                           \
    generate_branch_idle_pure(block_exits[block_exit_position].branch_source, \
     block_exits[block_exit_position].branch_target);                         \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    generate_branch_no_cycle_update(                                          \
     block_exits[block_exit_position].branch_source,                          \
     block_exits[block_exit_position].branch_target, thumb);                  \
  }                                                                           \
  generate_branch_patch_conditional(backpatch_address, translation_ptr);      \
  if(idle_pure)                                                               \
  {                                                                           \
    generate_idle_disarm();                                                   \
  }                                                                           \
  block_exit_position++;                                                      \
}                                                                             \

#else
#define thumb_conditional_branch(condition)                                   \
{                                                                             \
  generate_cycle_update();                                                    \
  generate_branch_filler(condition_opposite_##condition, backpatch_address);  \
  generate_branch_no_cycle_update(                                            \
   block_exits[block_exit_position].branch_source,                            \
   block_exits[block_exit_position].branch_target, thumb);                    \
  generate_branch_patch_conditional(backpatch_address, translation_ptr);      \
  block_exit_position++;                                                      \
}                                                                             \

#endif


#define arm_conditional_block_header()                                        \
  generate_cycle_update();                                                    \
  /* This will choose the opposite condition */                               \
  condition ^= 0x01;                                                          \
  generate_branch_filler(condition, backpatch_address)                        \

#define arm_b()                                                               \
  generate_branch(arm)                                                        \

#define arm_bl()                                                              \
  generate_update_pc((pc + 4));                                               \
  arm_generate_store_reg(reg_a0, REG_LR);                                     \
  generate_branch(arm)                                                        \

#define arm_bx()                                                              \
  arm_decode_branchx(opcode);                                                 \
  arm_generate_load_reg_pc(reg_a0, rn, 8);                                    \
  generate_indirect_branch_dual();                                            \

#define arm_swi()                                                             \
  generate_function_far_call(armfn_swi_arm);                                  \
  write32((pc + 4));                                                          \
  generate_branch(arm)                                                        \

#ifdef NSPIRE_BUILD
#define thumb_b()                                                             \
{                                                                             \
  if(nspire_idle_pure(block_exits[block_exit_position].branch_target, 14))   \
  {                                                                           \
    generate_cycle_update();                                                  \
    generate_branch_idle_pure(block_exits[block_exit_position].branch_source, \
     block_exits[block_exit_position].branch_target);                         \
    block_exit_position++;                                                    \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    generate_branch(thumb)                                                    \
  }                                                                           \
}                                                                             \

#else
#define thumb_b()                                                             \
  generate_branch(thumb)                                                      \

#endif

#define thumb_bl()                                                            \
  generate_update_pc(((pc + 2) | 0x01));                                      \
  thumb_generate_store_reg(reg_a0, REG_LR);                                   \
  generate_branch(thumb)                                                      \

#define thumb_blh()                                                           \
{                                                                             \
  thumb_decode_branch();                                                      \
  u32 offlo = (offset * 2) & 0xFF;                                            \
  u32 offhi = (offset * 2) >> 8;                                              \
  generate_update_pc(((pc + 2) | 0x01));                                      \
  thumb_generate_load_reg(reg_a1, REG_LR);                                    \
  thumb_generate_store_reg(reg_a0, REG_LR);                                   \
  generate_add_reg_reg_imm(reg_a0, reg_a1, offlo, 0);                         \
  if (offhi) {                                                                \
    generate_add_reg_reg_imm(reg_a0, reg_a0, offhi, arm_imm_lsl_to_rot(8));   \
  }                                                                           \
  generate_indirect_branch_cycle_update(thumb);                               \
}                                                                             \

#define thumb_bx()                                                            \
{                                                                             \
  thumb_decode_hireg_op();                                                    \
  thumb_generate_load_reg_pc(reg_a0, rs, 4);                                  \
  generate_indirect_branch_cycle_update(dual_thumb);                          \
}                                                                             \

#define thumb_process_cheats()                                                \
  generate_function_far_call(armfn_cheat_thumb);

#define arm_process_cheats()                                                  \
  generate_function_far_call(armfn_cheat_arm);

#define thumb_swi()                                                           \
  generate_function_far_call(armfn_swi_thumb);                              \
  write32((pc + 2));                                                          \
  /* We're in ARM mode now */                                                 \
  generate_branch(arm)                                                        \

// Use software division
void *div6, *divarm7;
#define arm_hle_div(cpu_mode)                                                 \
  cycle_count += 11 + 32;                                                     \
  generate_function_call(div6);
#define arm_hle_div_arm(cpu_mode)                                             \
  cycle_count += 14 + 32;                                                     \
  generate_function_call(divarm7);

#define generate_translation_gate(type)                                       \
  generate_update_pc(pc);                                                     \
  generate_indirect_branch_no_cycle_update(type)                              \


extern u32 st_handler_functions[4][17];
extern u32 ld_handler_functions[5][17];
extern u32 ld_swap_handler_functions[5][17];

// Tables used by the memory handlers (placed near reg_base)
extern u32 ld_lookup_tables[5][17];
extern u32 st_lookup_tables[4][17];
#ifdef NSPIRE_BUILD
// mem_dispatch's 256-entry tables, indexed by the address's top byte
// (arm_stub.S; stores u8, u16, u32, u32_safe, loads u8, s8, u16, s16, u32)
extern u32 *nspire_mem_tables[9];
#endif

void init_emitter(bool must_swap) {
  int i;

#ifdef NSPIRE_BUILD
  nspire_block_fast = (u8 *)reg - (iwram + 0x8000) == NSPIRE_IWRAM_DATA;
#endif

#ifdef NSPIRE_BUILD
  // mem_dispatch's 256-entry tables (arm_stub.S): entry b is the 17-entry
  // table's entry b + 1 for regions 0x00-0x0F, its "-1" entry above.
  {
    const u32 *ld = must_swap ? &ld_swap_handler_functions[0][0] : &ld_handler_functions[0][0];
    int t;
    for (t = 0; t < 9; t++)
    {
      const u32 *src = t < 4 ? st_handler_functions[t] : ld + 17 * (t - 4);
      u32 *dst = nspire_mem_tables[t];
      for (i = 0; i < 256; i++)
        dst[i] = src[i < 16 ? i + 1 : 0];
    }
  }
#else
  // Generate handler table
  memcpy(st_lookup_tables, st_handler_functions, sizeof(st_lookup_tables));
  // Issue faster paths if swapping is not required
  if (must_swap)
    memcpy(ld_lookup_tables, ld_swap_handler_functions, sizeof(ld_lookup_tables));
  else
    memcpy(ld_lookup_tables, ld_handler_functions, sizeof(ld_lookup_tables));
#endif

  rom_cache_watermark = INITIAL_ROM_WATERMARK;
#ifdef NSPIRE_BUILD
  /* core-17: the ROM cache is made anew from its start */
  nspire_pcs.rom_count = 0;
  nspire_pcs.rom_kept = 0;
  nspire_pcs.pending &= ~2u;
  mem_pc_cache_clear();
#endif
  u8 *translation_ptr = (u8*)&rom_translation_cache[0];

  // Generate ARMv5+ division code, uses a mix of libgcc and some open bioses.
  // This is meant for ARMv5 or higher, uses CLZ

  // Invert operands for SWI 7 (divarm)
  divarm7 = translation_ptr;
  ARM_MOV_REG_REG(0, reg_a2, reg_x0);
  ARM_MOV_REG_REG(0, reg_x0, reg_x1);
  ARM_MOV_REG_REG(0, reg_x1, reg_a2);

  div6 = translation_ptr;
  // Save flags before using them
  generate_save_flags();
  // Stores result and remainder signs 
  ARM_ANDS_REG_IMM(0, reg_a2, reg_x1, 0x80, arm_imm_lsl_to_rot(24));
  ARM_EOR_REG_IMMSHIFT(0, reg_a2, reg_a2, reg_x0, ARMSHIFT_ASR, 1);

  // Make numbers positive if they are negative
  ARM_RSB_REG_IMM_COND(0, reg_x1, reg_x1, 0, 0, ARMCOND_MI);
  ARM_TST_REG_REG(0, reg_x0, reg_x0);
  ARM_RSB_REG_IMM_COND(0, reg_x0, reg_x0, 0, 0, ARMCOND_MI);

  // Calculates the number of iterations to division, and jumps to unrolled code
  ARM_CLZ(0, reg_a0, reg_x0);
  ARM_CLZ(0, reg_a1, reg_x1);
  ARM_SUBS_REG_REG(0, reg_a0, reg_a1, reg_a0);          // Align and check if a<b
  ARM_RSB_REG_IMM(0, reg_a0, reg_a0, 31, 0);
  ARM_MOV_REG_IMM_COND(0, reg_a0, 32, 0, ARMCOND_MI);   // Cap to 32 (skip division)
  ARM_ADD_REG_IMMSHIFT(0, reg_a0, reg_a0, reg_a0, ARMSHIFT_LSL, 1);
  ARM_MOV_REG_IMM(0, reg_a1, 0, 0);
  ARM_ADD_REG_IMMSHIFT(0, ARMREG_PC, ARMREG_PC, reg_a0, ARMSHIFT_LSL, 2);
  ARM_NOP(0);

  for (i = 31; i >= 0; i--) {
    ARM_CMP_REG_IMMSHIFT(0, reg_x0, reg_x1, ARMSHIFT_LSL, i);
    ARM_ADC_REG_REG(0, reg_a1, reg_a1, reg_a1);
    ARM_SUB_REG_IMMSHIFT_COND(0, reg_x0, reg_x0, reg_x1, ARMSHIFT_LSL, i, ARMCOND_HS);
  }

  ARM_MOV_REG_REG(0, reg_x1, reg_x0);
  ARM_MOV_REG_REG(0, reg_x0, reg_a1);
  // Negate result if sign is negative
  ARM_SHLS_IMM(0, reg_a2, reg_a2, 1);
  ARM_RSB_REG_IMM_COND(0, reg_x0, reg_x0, 0, 0, ARMCOND_HS);
  ARM_RSB_REG_IMM_COND(0, reg_x1, reg_x1, 0, 0, ARMCOND_MI);

  // Register R3 stores the abs(r0/r1), store it in the right reg/mem-reg
  generate_load_memreg(reg_a2, REG_CPSR);
  ARM_TST_REG_IMM8(0, reg_a2, 0x20);
  arm_generate_store_reg(reg_a1, 3 /* r3 */);
  ARM_MOV_REG_REG_COND(0, reg_x3, reg_a1, ARMCOND_NE);

  // Return and continue regular emulation
  generate_restore_flags();
  ARM_BX(0, ARMREG_LR);

  // Now generate BIOS hooks
  rom_cache_watermark = (u32)(translation_ptr - rom_translation_cache);
  init_bios_hooks();

  // Intialize function table
  reg[REG_USERDEF + armfn_gbaup_idle_arm]   = (u32)arm_update_gba_idle_arm;
  reg[REG_USERDEF + armfn_gbaup_idle_thumb] = (u32)arm_update_gba_idle_thumb;
#ifdef NSPIRE_BUILD
  reg[REG_USERDEF + armfn_gbaup_idle_pass_thumb] =
   (u32)arm_update_gba_idle_pass_thumb;
#endif
  reg[REG_USERDEF + armfn_gbaup_arm]   = (u32)arm_update_gba_arm;
  reg[REG_USERDEF + armfn_gbaup_thumb] = (u32)arm_update_gba_thumb;
  reg[REG_USERDEF + armfn_swi_arm]   = (u32)execute_swi_arm;
  reg[REG_USERDEF + armfn_swi_thumb] = (u32)execute_swi_thumb;
  reg[REG_USERDEF + armfn_cheat_arm]   = (u32)arm_cheat_hook;
  reg[REG_USERDEF + armfn_cheat_thumb] = (u32)thumb_cheat_hook;
  reg[REG_USERDEF + armfn_store_cpsr]   = (u32)execute_store_cpsr;
  reg[REG_USERDEF + armfn_spsr_restore] = (u32)execute_spsr_restore;
  reg[REG_USERDEF + armfn_indirect_arm]   = (u32)arm_indirect_branch_arm;
  reg[REG_USERDEF + armfn_indirect_thumb] = (u32)arm_indirect_branch_thumb;
  reg[REG_USERDEF + armfn_indirect_dual_arm]   = (u32)arm_indirect_branch_dual_arm;
  reg[REG_USERDEF + armfn_indirect_dual_thumb] = (u32)arm_indirect_branch_dual_thumb;
  reg[REG_USERDEF + armfn_debug_trace] = (u32)trace_instruction;
}

u32 execute_arm_translate_internal(u32 cycles, void *regptr);
u32 execute_arm_translate(u32 cycles) {
  return execute_arm_translate_internal(cycles, &reg[0]);
}

#endif
