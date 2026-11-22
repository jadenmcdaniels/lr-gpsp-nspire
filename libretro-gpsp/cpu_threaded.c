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

// Not-so-important todo:
// - stm reglist writeback when base is in the list needs adjustment
// - block memory needs psr swapping and user mode reg swapping

#include "common.h"
#if defined(VITA)
#include <psp2/kernel/sysmem.h>
#include <stdio.h>
#elif defined(PS2)
#include <kernel.h>
#endif

u8 *last_rom_translation_ptr = NULL;
u8 *last_ram_translation_ptr = NULL;

#if defined(MMAP_JIT_CACHE)
u8* rom_translation_cache;
u8* ram_translation_cache;
u8 *rom_translation_ptr;
u8 *ram_translation_ptr;
#elif defined(VITA)
u8* rom_translation_cache;
u8* ram_translation_cache;
u8 *rom_translation_ptr;
u8 *ram_translation_ptr;
int sceBlock;
#elif defined(_3DS) 
u8* rom_translation_cache_ptr;
u8* ram_translation_cache_ptr;
u8 *rom_translation_ptr = rom_translation_cache;
u8 *ram_translation_ptr = ram_translation_cache;
#else
u8 *rom_translation_ptr = rom_translation_cache;
u8 *ram_translation_ptr = ram_translation_cache;
#endif
/* Note, see stub files for more cache definitions */

#ifdef NSPIRE_HOST
/* The speed model (tests/arm/perf.c ramjit=N) starts the RAM blocks N
 * bytes into their cache, to see what the blocks' place in the
 * instruction cache does to a scene; 0 everywhere else. */
u32 nspire_ram_cache_start;
#define NSPIRE_RAM_CACHE_START nspire_ram_cache_start
#else
#define NSPIRE_RAM_CACHE_START 0
#endif

u32 iwram_code_min = ~0U;
u32 iwram_code_max =  0U;
u32 ewram_code_min = ~0U;
u32 ewram_code_max =  0U;

#define INITIAL_ROM_WATERMARK   16   // To avoid NULL aliasing
u32 rom_cache_watermark = INITIAL_ROM_WATERMARK;

u8 *bios_swi_entrypoint = NULL;

// Contains an offset table to rom_translation cache area
// It features a chaining linked list for collisions
// The rom area has a small header section that contains:
//  - PC value for the entry
//  - Offset to the next entry (if any)
typedef struct
{
  u32 pc_value;
  u32 next_entry;
} hashhdr_type;

u32 rom_branch_hash[ROM_BRANCH_HASH_SIZE];

typedef struct
{
  u8 *block_offset;
  u16 flag_data;
  u8 condition;
  u8 update_cycles;
} block_data_type;

typedef struct
{
  u32 branch_target;
  u8 *branch_source;
} block_exit_type;

// Div (6) and DivArm (7)
#define is_div_swi(swinum) (((swinum) & 0xFE) == 0x06)

#define arm_decode_data_proc_reg(opcode)                                      \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_data_proc_imm(opcode)                                      \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 imm = opcode & 0xFF;                                                    \
  u32 imm_ror = ((opcode >> 8) & 0x0F) * 2                                    \

#define arm_decode_psr_reg(opcode)                                            \
  u32 psr_pfield = ((opcode >> 16) & 1) | ((opcode >> 18) & 2);               \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_psr_imm(opcode)                                            \
  u32 psr_pfield = ((opcode >> 16) & 1) | ((opcode >> 18) & 2);               \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 imm = opcode & 0xFF;                                                    \
  u32 imm_ror = ((opcode >> 8) & 0x0F) * 2                                    \

#define arm_decode_branchx(opcode)                                            \
  u32 rn = opcode & 0x0F                                                      \

#define arm_decode_multiply()                                                 \
  u32 rd = (opcode >> 16) & 0x0F;                                             \
  u32 rn = (opcode >> 12) & 0x0F;                                             \
  u32 rs = (opcode >> 8) & 0x0F;                                              \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_multiply_long()                                            \
  u32 rdhi = (opcode >> 16) & 0x0F;                                           \
  u32 rdlo = (opcode >> 12) & 0x0F;                                           \
  u32 rs = (opcode >> 8) & 0x0F;                                              \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_swap()                                                     \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_half_trans_r()                                             \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_half_trans_of()                                            \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 offset = ((opcode >> 4) & 0xF0) | (opcode & 0x0F)                       \

#define arm_decode_data_trans_imm()                                           \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 offset = opcode & 0x0FFF                                                \

#define arm_decode_data_trans_reg()                                           \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 rd = (opcode >> 12) & 0x0F;                                             \
  u32 rm = opcode & 0x0F                                                      \

#define arm_decode_block_trans()                                              \
  u32 rn = (opcode >> 16) & 0x0F;                                             \
  u32 reg_list = opcode & 0xFFFF                                              \

#define arm_decode_branch()                                                   \
  s32 offset = ((s32)(opcode & 0xFFFFFF) << 8) >> 6                           \

#define thumb_decode_shift()                                                  \
  u32 imm = (opcode >> 6) & 0x1F;                                             \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_add_sub()                                                \
  u32 rn = (opcode >> 6) & 0x07;                                              \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_add_sub_imm()                                            \
  u32 imm = (opcode >> 6) & 0x07;                                             \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_imm()                                                    \
  u32 imm = opcode & 0xFF                                                     \

#define thumb_decode_alu_op()                                                 \
  u32 rs = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_hireg_op()                                               \
  u32 rs = (opcode >> 3) & 0x0F;                                              \
  u32 rd = ((opcode >> 4) & 0x08) | (opcode & 0x07)                           \

#define thumb_decode_mem_reg()                                                \
  u32 ro = (opcode >> 6) & 0x07;                                              \
  u32 rb = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_mem_imm()                                                \
  u32 imm = (opcode >> 6) & 0x1F;                                             \
  u32 rb = (opcode >> 3) & 0x07;                                              \
  u32 rd = opcode & 0x07                                                      \

#define thumb_decode_add_sp()                                                 \
  u32 imm = opcode & 0x7F                                                     \

#define thumb_decode_rlist()                                                  \
  u32 reg_list = opcode & 0xFF                                                \

#define thumb_decode_branch_cond()                                            \
  s32 offset = (s8)(opcode & 0xFF)                                            \

#define thumb_decode_branch()                                                 \
  u32 offset = opcode & 0x07FF                                                \

/* Include the right emitter headers */
#if defined(MIPS_ARCH)
  #include "mips/mips_emit.h"
#elif defined(ARM_ARCH)
  #include "arm/arm_emit.h"
#elif defined(ARM64_ARCH)
  #include "arm/arm64_emit.h"
#else
  #include "x86/x86_emit.h"
#endif

/* Code an emitter puts after a block's exits (arm_emit.h, NSPIRE: block
   transfers' per-register code); block_tails_bytes bounds what is pending. */
#ifndef generate_block_tails
  #define generate_block_tails_start()
  #define generate_block_tails()
  #define block_tails_bytes 0
#endif

/* An emitter's tables of the code it emits (arm_emit.h, NSPIRE: the PC of
   each memory access, core-17), emptied with their cache when the next
   translation starts (mem_pc_settle). */
#ifndef mem_pc_full
  #define mem_pc_full(ram_region, translation_ptr) 0
  #define mem_pc_init()
  #define mem_pc_flush_ram()
  #define mem_pc_flush_rom()
  #define mem_pc_settle()
  #define mem_pc_bios_start()
  #define mem_pc_bios_end()
#endif

/* Cache invalidation */

#ifdef NSPIRE_LIBRETRO
void nspire_jit_cache_sync(void *base, void *end);
#endif

#if defined(PSP)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    sceKernelDcacheWritebackRange(baseaddr, ((char*)endptr) - ((char*)baseaddr));
    sceKernelIcacheInvalidateRange(baseaddr, ((char*)endptr) - ((char*)baseaddr));
  }
#elif defined(PS2)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    FlushCache(0);   // Dcache flush
    FlushCache(2);   // Icache invalidate
  }
#elif defined(VITA)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    sceKernelSyncVMDomain(sceBlock, baseaddr, ((char*)endptr) - ((char*)baseaddr) + 64);
  }
#elif defined(_3DS)
  #include "3ds/3ds_utils.h"
  void platform_cache_sync(void *baseaddr, void *endptr) {
    ctr_flush_invalidate_cache();
  }
#elif defined(ARM_ARCH) && defined(NSPIRE_LIBRETRO)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    if (baseaddr && endptr && endptr > baseaddr)
      nspire_jit_cache_sync(baseaddr, endptr);
  }
#elif defined(ARM_ARCH) || defined(ARM64_ARCH)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    /* GCC builtin works with newlib/Ndless; __clear_cache is not always linked. */
    __builtin___clear_cache((char *)baseaddr, (char *)endptr);
  }
#elif defined(MIPS_ARCH)
  void platform_cache_sync(void *baseaddr, void *endptr) {
    __builtin___clear_cache(baseaddr, endptr);
  }
#else
  /* x86 CPUs have icache consistency checks */
  void platform_cache_sync(void *baseaddr, void *endptr) {}
#endif

void translate_icache_sync() {
    // Cache emitted code can only grow
    if (last_rom_translation_ptr < rom_translation_ptr) {
        platform_cache_sync(last_rom_translation_ptr, rom_translation_ptr);
        last_rom_translation_ptr = rom_translation_ptr;
    }
    if (last_ram_translation_ptr < ram_translation_ptr) {
        platform_cache_sync(last_ram_translation_ptr, ram_translation_ptr);
        last_ram_translation_ptr = ram_translation_ptr;
    }
}

/* End of Cache invalidation */


#define check_pc_region(pc)                                                   \
  new_pc_region = (pc >> 15);                                                 \
  if(new_pc_region != pc_region)                                              \
  {                                                                           \
    pc_region = new_pc_region;                                                \
    pc_address_block = memory_map_read[new_pc_region];                        \
                                                                              \
    if(!pc_address_block)                                                     \
      pc_address_block = load_gamepak_page(pc_region & 0x3FF);                \
  }                                                                           \

#define translate_arm_instruction()                                           \
  check_pc_region(pc);                                                        \
  opcode = address32(pc_address_block, (pc & 0x7FFF));                        \
  condition = block_data[block_data_position].condition;                      \
                                                                              \
  if((condition != last_condition) || (condition >= 0x20))                    \
  {                                                                           \
    if((last_condition & 0x0F) != 0x0E)                                       \
    {                                                                         \
      generate_branch_patch_conditional(backpatch_address, translation_ptr);  \
    }                                                                         \
                                                                              \
    last_condition = condition;                                               \
                                                                              \
    condition &= 0x0F;                                                        \
                                                                              \
    if(condition != 0x0E)                                                     \
    {                                                                         \
      arm_conditional_block_header();                                         \
    }                                                                         \
  }                                                                           \
  emit_trace_arm_instruction(pc);                                             \
                                                                              \
  switch((opcode >> 20) & 0xFF)                                               \
  {                                                                           \
    case 0x00:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], -rm */                                            \
          arm_access_memory(store, down, post, u16, half_reg);                \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* MUL rd, rm, rs */                                                \
          arm_multiply(no, no);                                               \
          cycle_count += 2;  /* variable 1..4, pick 2 as an aprox. */         \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* AND rd, rn, reg_op */                                              \
        arm_data_proc(and, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x01:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* MULS rd, rm, rs */                                             \
            arm_multiply(no, yes);                                            \
            cycle_count += 2;  /* variable 1..4, pick 2 as an aprox. */       \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], -rm */                                          \
            arm_access_memory(load, down, post, u16, half_reg);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], -rm */                                         \
            arm_access_memory(load, down, post, s8, half_reg);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], -rm */                                         \
            arm_access_memory(load, down, post, s16, half_reg);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ANDS rd, rn, reg_op */                                             \
        arm_data_proc(ands, reg_flags, flags);                                \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x02:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], -rm */                                            \
          arm_access_memory(store, down, post, u16, half_reg);                \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* MLA rd, rm, rs, rn */                                            \
          arm_multiply(yes, no);                                              \
          cycle_count += 3;  /* variable 2..5, pick 3 as an aprox. */         \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* EOR rd, rn, reg_op */                                              \
        arm_data_proc(eor, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x03:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* MLAS rd, rm, rs, rn */                                         \
            arm_multiply(yes, yes);                                           \
            cycle_count += 3;  /* variable 2..5, pick 3 as an aprox. */       \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], -rm */                                          \
            arm_access_memory(load, down, post, u16, half_reg);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], -rm */                                         \
            arm_access_memory(load, down, post, s8, half_reg);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], -rm */                                         \
            arm_access_memory(load, down, post, s16, half_reg);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* EORS rd, rn, reg_op */                                             \
        arm_data_proc(eors, reg_flags, flags);                                \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x04:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn], -imm */                                             \
        arm_access_memory(store, down, post, u16, half_imm);                  \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* SUB rd, rn, reg_op */                                              \
        arm_data_proc(sub, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x05:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn], -imm */                                         \
            arm_access_memory(load, down, post, u16, half_imm);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], -imm */                                        \
            arm_access_memory(load, down, post, s8, half_imm);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], -imm */                                        \
            arm_access_memory(load, down, post, s16, half_imm);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* SUBS rd, rn, reg_op */                                             \
        arm_data_proc(subs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x06:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn], -imm */                                             \
        arm_access_memory(store, down, post, u16, half_imm);                  \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* RSB rd, rn, reg_op */                                              \
        arm_data_proc(rsb, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x07:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn], -imm */                                         \
            arm_access_memory(load, down, post, u16, half_imm);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], -imm */                                        \
            arm_access_memory(load, down, post, s8, half_imm);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], -imm */                                        \
            arm_access_memory(load, down, post, s16, half_imm);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* RSBS rd, rn, reg_op */                                             \
        arm_data_proc(rsbs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x08:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], +rm */                                            \
          arm_access_memory(store, up, post, u16, half_reg);                  \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* UMULL rd, rm, rs */                                              \
          arm_multiply_long(u64, no, no);                                     \
          cycle_count += 3;  /* this is an aproximation :P */                 \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADD rd, rn, reg_op */                                              \
        arm_data_proc(add, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x09:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* UMULLS rdlo, rdhi, rm, rs */                                   \
            arm_multiply_long(u64, no, yes);                                  \
            cycle_count += 3;  /* this is an aproximation :P */               \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], +rm */                                          \
            arm_access_memory(load, up, post, u16, half_reg);                 \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], +rm */                                         \
            arm_access_memory(load, up, post, s8, half_reg);                  \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], +rm */                                         \
            arm_access_memory(load, up, post, s16, half_reg);                 \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADDS rd, rn, reg_op */                                             \
        arm_data_proc(adds, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0A:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], +rm */                                            \
          arm_access_memory(store, up, post, u16, half_reg);                  \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* UMLAL rd, rm, rs */                                              \
          arm_multiply_long(u64_add, yes, no);                                \
          cycle_count += 3;  /* Between 2 and 5 cycles? */                    \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADC rd, rn, reg_op */                                              \
        arm_data_proc(adc, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0B:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* UMLALS rdlo, rdhi, rm, rs */                                   \
            arm_multiply_long(u64_add, yes, yes);                             \
            cycle_count += 3;  /* Between 2 and 5 cycles? */                  \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], +rm */                                          \
            arm_access_memory(load, up, post, u16, half_reg);                 \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], +rm */                                         \
            arm_access_memory(load, up, post, s8, half_reg);                  \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], +rm */                                         \
            arm_access_memory(load, up, post, s16, half_reg);                 \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADCS rd, rn, reg_op */                                             \
        arm_data_proc(adcs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0C:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], +imm */                                           \
          arm_access_memory(store, up, post, u16, half_imm);                  \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* SMULL rd, rm, rs */                                              \
          arm_multiply_long(s64, no, no);                                     \
          cycle_count += 2;  /* Between 1 and 4 cycles? */                    \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* SBC rd, rn, reg_op */                                              \
        arm_data_proc(sbc, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0D:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* SMULLS rdlo, rdhi, rm, rs */                                   \
            arm_multiply_long(s64, no, yes);                                  \
            cycle_count += 2;  /* Between 1 and 4 cycles? */                  \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], +imm */                                         \
            arm_access_memory(load, up, post, u16, half_imm);                 \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], +imm */                                        \
            arm_access_memory(load, up, post, s8, half_imm);                  \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], +imm */                                        \
            arm_access_memory(load, up, post, s16, half_imm);                 \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* SBCS rd, rn, reg_op */                                             \
        arm_data_proc(sbcs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0E:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn], +imm */                                           \
          arm_access_memory(store, up, post, u16, half_imm);                  \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* SMLAL rd, rm, rs */                                              \
          arm_multiply_long(s64_add, yes, no);                                \
          cycle_count += 3;  /* Between 2 and 5 cycles? */                    \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* RSC rd, rn, reg_op */                                              \
        arm_data_proc(rsc, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x0F:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 0:                                                             \
            /* SMLALS rdlo, rdhi, rm, rs */                                   \
            arm_multiply_long(s64_add, yes, yes);                             \
            cycle_count += 3;  /* Between 2 and 5 cycles? */                  \
            break;                                                            \
                                                                              \
          case 1:                                                             \
            /* LDRH rd, [rn], +imm */                                         \
            arm_access_memory(load, up, post, u16, half_imm);                 \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn], +imm */                                        \
            arm_access_memory(load, up, post, s8, half_imm);                  \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn], +imm */                                        \
            arm_access_memory(load, up, post, s16, half_imm);                 \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* RSCS rd, rn, reg_op */                                             \
        arm_data_proc(rscs, reg, flags);                                      \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x10:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn - rm] */                                            \
          arm_access_memory(store, down, pre, u16, half_reg);                 \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* SWP rd, rm, [rn] */                                              \
          arm_swap(u32);                                                      \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MRS rd, cpsr */                                                    \
        arm_psr(reg, read, cpsr);                                             \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x11:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn - rm] */                                          \
            arm_access_memory(load, down, pre, u16, half_reg);                \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn - rm] */                                         \
            arm_access_memory(load, down, pre, s8, half_reg);                 \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn - rm] */                                         \
            arm_access_memory(load, down, pre, s16, half_reg);                \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* TST rd, rn, reg_op */                                              \
        arm_data_proc_test(tst, reg_flags);                                   \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x12:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn - rm]! */                                             \
        arm_access_memory(store, down, pre_wb, u16, half_reg);                \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        if(opcode & 0x10)                                                     \
        {                                                                     \
          /* BX rn */                                                         \
          arm_bx();                                                           \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* MSR cpsr, rm */                                                  \
          arm_psr(reg, store, cpsr);                                          \
        }                                                                     \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x13:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn - rm]! */                                         \
            arm_access_memory(load, down, pre_wb, u16, half_reg);             \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn - rm]! */                                        \
            arm_access_memory(load, down, pre_wb, s8, half_reg);              \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn - rm]! */                                        \
            arm_access_memory(load, down, pre_wb, s16, half_reg);             \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* TEQ rd, rn, reg_op */                                              \
        arm_data_proc_test(teq, reg_flags);                                   \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x14:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        if(opcode & 0x20)                                                     \
        {                                                                     \
          /* STRH rd, [rn - imm] */                                           \
          arm_access_memory(store, down, pre, u16, half_imm);                 \
        }                                                                     \
        else                                                                  \
        {                                                                     \
          /* SWPB rd, rm, [rn] */                                             \
          arm_swap(u8);                                                       \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MRS rd, spsr */                                                    \
        arm_psr(reg, read, spsr);                                             \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x15:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn - imm] */                                         \
            arm_access_memory(load, down, pre, u16, half_imm);                \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn - imm] */                                        \
            arm_access_memory(load, down, pre, s8, half_imm);                 \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn - imm] */                                        \
            arm_access_memory(load, down, pre, s16, half_imm);                \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* CMP rn, reg_op */                                                  \
        arm_data_proc_test(cmp, reg);                                         \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x16:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn - imm]! */                                            \
        arm_access_memory(store, down, pre_wb, u16, half_imm);                \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MSR spsr, rm */                                                    \
        arm_psr(reg, store, spsr);                                            \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x17:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn - imm]! */                                        \
            arm_access_memory(load, down, pre_wb, u16, half_imm);             \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn - imm]! */                                       \
            arm_access_memory(load, down, pre_wb, s8, half_imm);              \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn - imm]! */                                       \
            arm_access_memory(load, down, pre_wb, s16, half_imm);             \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* CMN rd, rn, reg_op */                                              \
        arm_data_proc_test(cmn, reg);                                         \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x18:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn + rm] */                                              \
        arm_access_memory(store, up, pre, u16, half_reg);                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ORR rd, rn, reg_op */                                              \
        arm_data_proc(orr, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x19:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn + rm] */                                          \
            arm_access_memory(load, up, pre, u16, half_reg);                  \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn + rm] */                                         \
            arm_access_memory(load, up, pre, s8, half_reg);                   \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn + rm] */                                         \
            arm_access_memory(load, up, pre, s16, half_reg);                  \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ORRS rd, rn, reg_op */                                             \
        arm_data_proc(orrs, reg_flags, flags);                                \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1A:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn + rm]! */                                             \
        arm_access_memory(store, up, pre_wb, u16, half_reg);                  \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MOV rd, reg_op */                                                  \
        arm_data_proc_unary(mov, reg, no_flags);                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1B:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn + rm]! */                                         \
            arm_access_memory(load, up, pre_wb, u16, half_reg);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn + rm]! */                                        \
            arm_access_memory(load, up, pre_wb, s8, half_reg);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn + rm]! */                                        \
            arm_access_memory(load, up, pre_wb, s16, half_reg);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MOVS rd, reg_op */                                                 \
        arm_data_proc_unary(movs, reg_flags, flags);                          \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1C:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn + imm] */                                             \
        arm_access_memory(store, up, pre, u16, half_imm);                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* BIC rd, rn, reg_op */                                              \
        arm_data_proc(bic, reg, no_flags);                                    \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1D:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn + imm] */                                         \
            arm_access_memory(load, up, pre, u16, half_imm);                  \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn + imm] */                                        \
            arm_access_memory(load, up, pre, s8, half_imm);                   \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn + imm] */                                        \
            arm_access_memory(load, up, pre, s16, half_imm);                  \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* BICS rd, rn, reg_op */                                             \
        arm_data_proc(bics, reg_flags, flags);                                \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1E:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        /* STRH rd, [rn + imm]! */                                            \
        arm_access_memory(store, up, pre_wb, u16, half_imm);                  \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MVN rd, reg_op */                                                  \
        arm_data_proc_unary(mvn, reg, no_flags);                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x1F:                                                                \
      if((opcode & 0x90) == 0x90)                                             \
      {                                                                       \
        switch((opcode >> 5) & 0x03)                                          \
        {                                                                     \
          case 1:                                                             \
            /* LDRH rd, [rn + imm]! */                                        \
            arm_access_memory(load, up, pre_wb, u16, half_imm);               \
            break;                                                            \
                                                                              \
          case 2:                                                             \
            /* LDRSB rd, [rn + imm]! */                                       \
            arm_access_memory(load, up, pre_wb, s8, half_imm);                \
            break;                                                            \
                                                                              \
          case 3:                                                             \
            /* LDRSH rd, [rn + imm]! */                                       \
            arm_access_memory(load, up, pre_wb, s16, half_imm);               \
            break;                                                            \
        }                                                                     \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* MVNS rd, rn, reg_op */                                             \
        arm_data_proc_unary(mvns, reg_flags, flags);                          \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x20:                                                                \
      /* AND rd, rn, imm */                                                   \
      arm_data_proc(and, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x21:                                                                \
      /* ANDS rd, rn, imm */                                                  \
      arm_data_proc(ands, imm_flags, flags);                                  \
      break;                                                                  \
                                                                              \
    case 0x22:                                                                \
      /* EOR rd, rn, imm */                                                   \
      arm_data_proc(eor, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x23:                                                                \
      /* EORS rd, rn, imm */                                                  \
      arm_data_proc(eors, imm_flags, flags);                                  \
      break;                                                                  \
                                                                              \
    case 0x24:                                                                \
      /* SUB rd, rn, imm */                                                   \
      arm_data_proc(sub, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x25:                                                                \
      /* SUBS rd, rn, imm */                                                  \
      arm_data_proc(subs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x26:                                                                \
      /* RSB rd, rn, imm */                                                   \
      arm_data_proc(rsb, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x27:                                                                \
      /* RSBS rd, rn, imm */                                                  \
      arm_data_proc(rsbs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x28:                                                                \
      /* ADD rd, rn, imm */                                                   \
      arm_data_proc(add, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x29:                                                                \
      /* ADDS rd, rn, imm */                                                  \
      arm_data_proc(adds, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x2A:                                                                \
      /* ADC rd, rn, imm */                                                   \
      arm_data_proc(adc, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x2B:                                                                \
      /* ADCS rd, rn, imm */                                                  \
      arm_data_proc(adcs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x2C:                                                                \
      /* SBC rd, rn, imm */                                                   \
      arm_data_proc(sbc, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x2D:                                                                \
      /* SBCS rd, rn, imm */                                                  \
      arm_data_proc(sbcs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x2E:                                                                \
      /* RSC rd, rn, imm */                                                   \
      arm_data_proc(rsc, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x2F:                                                                \
      /* RSCS rd, rn, imm */                                                  \
      arm_data_proc(rscs, imm, flags);                                        \
      break;                                                                  \
                                                                              \
    case 0x30 ... 0x31:                                                       \
      /* TST rn, imm */                                                       \
      arm_data_proc_test(tst, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x32:                                                                \
      /* MSR cpsr, imm */                                                     \
      arm_psr(imm, store, cpsr);                                              \
      break;                                                                  \
                                                                              \
    case 0x33:                                                                \
      /* TEQ rn, imm */                                                       \
      arm_data_proc_test(teq, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x34 ... 0x35:                                                       \
      /* CMP rn, imm */                                                       \
      arm_data_proc_test(cmp, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x36:                                                                \
      /* MSR spsr, imm */                                                     \
      arm_psr(imm, store, spsr);                                              \
      break;                                                                  \
                                                                              \
    case 0x37:                                                                \
      /* CMN rn, imm */                                                       \
      arm_data_proc_test(cmn, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x38:                                                                \
      /* ORR rd, rn, imm */                                                   \
      arm_data_proc(orr, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x39:                                                                \
      /* ORRS rd, rn, imm */                                                  \
      arm_data_proc(orrs, imm_flags, flags);                                  \
      break;                                                                  \
                                                                              \
    case 0x3A:                                                                \
      /* MOV rd, imm */                                                       \
      arm_data_proc_unary(mov, imm, no_flags);                                \
      break;                                                                  \
                                                                              \
    case 0x3B:                                                                \
      /* MOVS rd, imm */                                                      \
      arm_data_proc_unary(movs, imm_flags, flags);                            \
      break;                                                                  \
                                                                              \
    case 0x3C:                                                                \
      /* BIC rd, rn, imm */                                                   \
      arm_data_proc(bic, imm, no_flags);                                      \
      break;                                                                  \
                                                                              \
    case 0x3D:                                                                \
      /* BICS rd, rn, imm */                                                  \
      arm_data_proc(bics, imm_flags, flags);                                  \
      break;                                                                  \
                                                                              \
    case 0x3E:                                                                \
      /* MVN rd, imm */                                                       \
      arm_data_proc_unary(mvn, imm, no_flags);                                \
      break;                                                                  \
                                                                              \
    case 0x3F:                                                                \
      /* MVNS rd, imm */                                                      \
      arm_data_proc_unary(mvns, imm_flags, flags);                            \
      break;                                                                  \
                                                                              \
    case 0x40:                                                                \
      /* STR rd, [rn], -imm */                                                \
      arm_access_memory(store, down, post, u32, imm);                         \
      break;                                                                  \
                                                                              \
    case 0x41:                                                                \
      /* LDR rd, [rn], -imm */                                                \
      arm_access_memory(load, down, post, u32, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x42:                                                                \
      /* STRT rd, [rn], -imm */                                               \
      arm_access_memory(store, down, post, u32, imm);                         \
      break;                                                                  \
                                                                              \
    case 0x43:                                                                \
      /* LDRT rd, [rn], -imm */                                               \
      arm_access_memory(load, down, post, u32, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x44:                                                                \
      /* STRB rd, [rn], -imm */                                               \
      arm_access_memory(store, down, post, u8, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x45:                                                                \
      /* LDRB rd, [rn], -imm */                                               \
      arm_access_memory(load, down, post, u8, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x46:                                                                \
      /* STRBT rd, [rn], -imm */                                              \
      arm_access_memory(store, down, post, u8, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x47:                                                                \
      /* LDRBT rd, [rn], -imm */                                              \
      arm_access_memory(load, down, post, u8, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x48:                                                                \
      /* STR rd, [rn], +imm */                                                \
      arm_access_memory(store, up, post, u32, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x49:                                                                \
      /* LDR rd, [rn], +imm */                                                \
      arm_access_memory(load, up, post, u32, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x4A:                                                                \
      /* STRT rd, [rn], +imm */                                               \
      arm_access_memory(store, up, post, u32, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x4B:                                                                \
      /* LDRT rd, [rn], +imm */                                               \
      arm_access_memory(load, up, post, u32, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x4C:                                                                \
      /* STRB rd, [rn], +imm */                                               \
      arm_access_memory(store, up, post, u8, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x4D:                                                                \
      /* LDRB rd, [rn], +imm */                                               \
      arm_access_memory(load, up, post, u8, imm);                             \
      break;                                                                  \
                                                                              \
    case 0x4E:                                                                \
      /* STRBT rd, [rn], +imm */                                              \
      arm_access_memory(store, up, post, u8, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x4F:                                                                \
      /* LDRBT rd, [rn], +imm */                                              \
      arm_access_memory(load, up, post, u8, imm);                             \
      break;                                                                  \
                                                                              \
    case 0x50:                                                                \
      /* STR rd, [rn - imm] */                                                \
      arm_access_memory(store, down, pre, u32, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x51:                                                                \
      /* LDR rd, [rn - imm] */                                                \
      arm_access_memory(load, down, pre, u32, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x52:                                                                \
      /* STR rd, [rn - imm]! */                                               \
      arm_access_memory(store, down, pre_wb, u32, imm);                       \
      break;                                                                  \
                                                                              \
    case 0x53:                                                                \
      /* LDR rd, [rn - imm]! */                                               \
      arm_access_memory(load, down, pre_wb, u32, imm);                        \
      break;                                                                  \
                                                                              \
    case 0x54:                                                                \
      /* STRB rd, [rn - imm] */                                               \
      arm_access_memory(store, down, pre, u8, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x55:                                                                \
      /* LDRB rd, [rn - imm] */                                               \
      arm_access_memory(load, down, pre, u8, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x56:                                                                \
      /* STRB rd, [rn - imm]! */                                              \
      arm_access_memory(store, down, pre_wb, u8, imm);                        \
      break;                                                                  \
                                                                              \
    case 0x57:                                                                \
      /* LDRB rd, [rn - imm]! */                                              \
      arm_access_memory(load, down, pre_wb, u8, imm);                         \
      break;                                                                  \
                                                                              \
    case 0x58:                                                                \
      /* STR rd, [rn + imm] */                                                \
      arm_access_memory(store, up, pre, u32, imm);                            \
      break;                                                                  \
                                                                              \
    case 0x59:                                                                \
      /* LDR rd, [rn + imm] */                                                \
      arm_access_memory(load, up, pre, u32, imm);                             \
      break;                                                                  \
                                                                              \
    case 0x5A:                                                                \
      /* STR rd, [rn + imm]! */                                               \
      arm_access_memory(store, up, pre_wb, u32, imm);                         \
      break;                                                                  \
                                                                              \
    case 0x5B:                                                                \
      /* LDR rd, [rn + imm]! */                                               \
      arm_access_memory(load, up, pre_wb, u32, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x5C:                                                                \
      /* STRB rd, [rn + imm] */                                               \
      arm_access_memory(store, up, pre, u8, imm);                             \
      break;                                                                  \
                                                                              \
    case 0x5D:                                                                \
      /* LDRB rd, [rn + imm] */                                               \
      arm_access_memory(load, up, pre, u8, imm);                              \
      break;                                                                  \
                                                                              \
    case 0x5E:                                                                \
      /* STRB rd, [rn + imm]! */                                              \
      arm_access_memory(store, up, pre_wb, u8, imm);                          \
      break;                                                                  \
                                                                              \
    case 0x5F:                                                                \
      /* LDRBT rd, [rn + imm]! */                                             \
      arm_access_memory(load, up, pre_wb, u8, imm);                           \
      break;                                                                  \
                                                                              \
    case 0x60:                                                                \
      /* STR rd, [rn], -rm */                                                 \
      arm_access_memory(store, down, post, u32, reg);                         \
      break;                                                                  \
                                                                              \
    case 0x61:                                                                \
      /* LDR rd, [rn], -rm */                                                 \
      arm_access_memory(load, down, post, u32, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x62:                                                                \
      /* STRT rd, [rn], -rm */                                                \
      arm_access_memory(store, down, post, u32, reg);                         \
      break;                                                                  \
                                                                              \
    case 0x63:                                                                \
      /* LDRT rd, [rn], -rm */                                                \
      arm_access_memory(load, down, post, u32, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x64:                                                                \
      /* STRB rd, [rn], -rm */                                                \
      arm_access_memory(store, down, post, u8, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x65:                                                                \
      /* LDRB rd, [rn], -rm */                                                \
      arm_access_memory(load, down, post, u8, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x66:                                                                \
      /* STRBT rd, [rn], -rm */                                               \
      arm_access_memory(store, down, post, u8, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x67:                                                                \
      /* LDRBT rd, [rn], -rm */                                               \
      arm_access_memory(load, down, post, u8, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x68:                                                                \
      /* STR rd, [rn], +rm */                                                 \
      arm_access_memory(store, up, post, u32, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x69:                                                                \
      /* LDR rd, [rn], +rm */                                                 \
      arm_access_memory(load, up, post, u32, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x6A:                                                                \
      /* STRT rd, [rn], +rm */                                                \
      arm_access_memory(store, up, post, u32, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x6B:                                                                \
      /* LDRT rd, [rn], +rm */                                                \
      arm_access_memory(load, up, post, u32, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x6C:                                                                \
      /* STRB rd, [rn], +rm */                                                \
      arm_access_memory(store, up, post, u8, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x6D:                                                                \
      /* LDRB rd, [rn], +rm */                                                \
      arm_access_memory(load, up, post, u8, reg);                             \
      break;                                                                  \
                                                                              \
    case 0x6E:                                                                \
      /* STRBT rd, [rn], +rm */                                               \
      arm_access_memory(store, up, post, u8, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x6F:                                                                \
      /* LDRBT rd, [rn], +rm */                                               \
      arm_access_memory(load, up, post, u8, reg);                             \
      break;                                                                  \
                                                                              \
    case 0x70:                                                                \
      /* STR rd, [rn - rm] */                                                 \
      arm_access_memory(store, down, pre, u32, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x71:                                                                \
      /* LDR rd, [rn - rm] */                                                 \
      arm_access_memory(load, down, pre, u32, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x72:                                                                \
      /* STR rd, [rn - rm]! */                                                \
      arm_access_memory(store, down, pre_wb, u32, reg);                       \
      break;                                                                  \
                                                                              \
    case 0x73:                                                                \
      /* LDR rd, [rn - rm]! */                                                \
      arm_access_memory(load, down, pre_wb, u32, reg);                        \
      break;                                                                  \
                                                                              \
    case 0x74:                                                                \
      /* STRB rd, [rn - rm] */                                                \
      arm_access_memory(store, down, pre, u8, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x75:                                                                \
      /* LDRB rd, [rn - rm] */                                                \
      arm_access_memory(load, down, pre, u8, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x76:                                                                \
      /* STRB rd, [rn - rm]! */                                               \
      arm_access_memory(store, down, pre_wb, u8, reg);                        \
      break;                                                                  \
                                                                              \
    case 0x77:                                                                \
      /* LDRB rd, [rn - rm]! */                                               \
      arm_access_memory(load, down, pre_wb, u8, reg);                         \
      break;                                                                  \
                                                                              \
    case 0x78:                                                                \
      /* STR rd, [rn + rm] */                                                 \
      arm_access_memory(store, up, pre, u32, reg);                            \
      break;                                                                  \
                                                                              \
    case 0x79:                                                                \
      /* LDR rd, [rn + rm] */                                                 \
      arm_access_memory(load, up, pre, u32, reg);                             \
      break;                                                                  \
                                                                              \
    case 0x7A:                                                                \
      /* STR rd, [rn + rm]! */                                                \
      arm_access_memory(store, up, pre_wb, u32, reg);                         \
      break;                                                                  \
                                                                              \
    case 0x7B:                                                                \
      /* LDR rd, [rn + rm]! */                                                \
      arm_access_memory(load, up, pre_wb, u32, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x7C:                                                                \
      /* STRB rd, [rn + rm] */                                                \
      arm_access_memory(store, up, pre, u8, reg);                             \
      break;                                                                  \
                                                                              \
    case 0x7D:                                                                \
      /* LDRB rd, [rn + rm] */                                                \
      arm_access_memory(load, up, pre, u8, reg);                              \
      break;                                                                  \
                                                                              \
    case 0x7E:                                                                \
      /* STRB rd, [rn + rm]! */                                               \
      arm_access_memory(store, up, pre_wb, u8, reg);                          \
      break;                                                                  \
                                                                              \
    case 0x7F:                                                                \
      /* LDRBT rd, [rn + rm]! */                                              \
      arm_access_memory(load, up, pre_wb, u8, reg);                           \
      break;                                                                  \
                                                                              \
    case 0x80:                                                                \
      /* STMDA rn, rlist */                                                   \
      arm_block_memory(store, down_a, no, no);                                \
      break;                                                                  \
                                                                              \
    case 0x81:                                                                \
      /* LDMDA rn, rlist */                                                   \
      arm_block_memory(load, down_a, no, no);                                 \
      break;                                                                  \
                                                                              \
    case 0x82:                                                                \
      /* STMDA rn!, rlist */                                                  \
      arm_block_memory(store, down_a, down, no);                              \
      break;                                                                  \
                                                                              \
    case 0x83:                                                                \
      /* LDMDA rn!, rlist */                                                  \
      arm_block_memory(load, down_a, down, no);                               \
      break;                                                                  \
                                                                              \
    case 0x84:                                                                \
      /* STMDA rn, rlist^ */                                                  \
      arm_block_memory(store, down_a, no, yes);                               \
      break;                                                                  \
                                                                              \
    case 0x85:                                                                \
      /* LDMDA rn, rlist^ */                                                  \
      arm_block_memory(load, down_a, no, yes);                                \
      break;                                                                  \
                                                                              \
    case 0x86:                                                                \
      /* STMDA rn!, rlist^ */                                                 \
      arm_block_memory(store, down_a, down, yes);                             \
      break;                                                                  \
                                                                              \
    case 0x87:                                                                \
      /* LDMDA rn!, rlist^ */                                                 \
      arm_block_memory(load, down_a, down, yes);                              \
      break;                                                                  \
                                                                              \
    case 0x88:                                                                \
      /* STMIA rn, rlist */                                                   \
      arm_block_memory(store, no, no, no);                                    \
      break;                                                                  \
                                                                              \
    case 0x89:                                                                \
      /* LDMIA rn, rlist */                                                   \
      arm_block_memory(load, no, no, no);                                     \
      break;                                                                  \
                                                                              \
    case 0x8A:                                                                \
      /* STMIA rn!, rlist */                                                  \
      arm_block_memory(store, no, up, no);                                    \
      break;                                                                  \
                                                                              \
    case 0x8B:                                                                \
      /* LDMIA rn!, rlist */                                                  \
      arm_block_memory(load, no, up, no);                                     \
      break;                                                                  \
                                                                              \
    case 0x8C:                                                                \
      /* STMIA rn, rlist^ */                                                  \
      arm_block_memory(store, no, no, yes);                                   \
      break;                                                                  \
                                                                              \
    case 0x8D:                                                                \
      /* LDMIA rn, rlist^ */                                                  \
      arm_block_memory(load, no, no, yes);                                    \
      break;                                                                  \
                                                                              \
    case 0x8E:                                                                \
      /* STMIA rn!, rlist^ */                                                 \
      arm_block_memory(store, no, up, yes);                                   \
      break;                                                                  \
                                                                              \
    case 0x8F:                                                                \
      /* LDMIA rn!, rlist^ */                                                 \
      arm_block_memory(load, no, up, yes);                                    \
      break;                                                                  \
                                                                              \
    case 0x90:                                                                \
      /* STMDB rn, rlist */                                                   \
      arm_block_memory(store, down_b, no, no);                                \
      break;                                                                  \
                                                                              \
    case 0x91:                                                                \
      /* LDMDB rn, rlist */                                                   \
      arm_block_memory(load, down_b, no, no);                                 \
      break;                                                                  \
                                                                              \
    case 0x92:                                                                \
      /* STMDB rn!, rlist */                                                  \
      arm_block_memory(store, down_b, down, no);                              \
      break;                                                                  \
                                                                              \
    case 0x93:                                                                \
      /* LDMDB rn!, rlist */                                                  \
      arm_block_memory(load, down_b, down, no);                               \
      break;                                                                  \
                                                                              \
    case 0x94:                                                                \
      /* STMDB rn, rlist^ */                                                  \
      arm_block_memory(store, down_b, no, yes);                               \
      break;                                                                  \
                                                                              \
    case 0x95:                                                                \
      /* LDMDB rn, rlist^ */                                                  \
      arm_block_memory(load, down_b, no, yes);                                \
      break;                                                                  \
                                                                              \
    case 0x96:                                                                \
      /* STMDB rn!, rlist^ */                                                 \
      arm_block_memory(store, down_b, down, yes);                             \
      break;                                                                  \
                                                                              \
    case 0x97:                                                                \
      /* LDMDB rn!, rlist^ */                                                 \
      arm_block_memory(load, down_b, down, yes);                              \
      break;                                                                  \
                                                                              \
    case 0x98:                                                                \
      /* STMIB rn, rlist */                                                   \
      arm_block_memory(store, up, no, no);                                    \
      break;                                                                  \
                                                                              \
    case 0x99:                                                                \
      /* LDMIB rn, rlist */                                                   \
      arm_block_memory(load, up, no, no);                                     \
      break;                                                                  \
                                                                              \
    case 0x9A:                                                                \
      /* STMIB rn!, rlist */                                                  \
      arm_block_memory(store, up, up, no);                                    \
      break;                                                                  \
                                                                              \
    case 0x9B:                                                                \
      /* LDMIB rn!, rlist */                                                  \
      arm_block_memory(load, up, up, no);                                     \
      break;                                                                  \
                                                                              \
    case 0x9C:                                                                \
      /* STMIB rn, rlist^ */                                                  \
      arm_block_memory(store, up, no, yes);                                   \
      break;                                                                  \
                                                                              \
    case 0x9D:                                                                \
      /* LDMIB rn, rlist^ */                                                  \
      arm_block_memory(load, up, no, yes);                                    \
      break;                                                                  \
                                                                              \
    case 0x9E:                                                                \
      /* STMIB rn!, rlist^ */                                                 \
      arm_block_memory(store, up, up, yes);                                   \
      break;                                                                  \
                                                                              \
    case 0x9F:                                                                \
      /* LDMIB rn!, rlist^ */                                                 \
      arm_block_memory(load, up, up, yes);                                    \
      break;                                                                  \
                                                                              \
    case 0xA0 ... 0xAF:                                                       \
    {                                                                         \
      /* B offset */                                                          \
      arm_b();                                                                \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xB0 ... 0xBF:                                                       \
    {                                                                         \
      /* BL offset */                                                         \
      arm_bl();                                                               \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xF0 ... 0xFF:                                                       \
    {                                                                         \
      u32 swinum = (opcode >> 16) & 0xFF;                                     \
      if (swinum == 6) {                                                      \
        cycle_count += 64;   /* Big under-estimation here */                  \
        arm_hle_div(arm);                                                     \
      }                                                                       \
      else if (swinum == 7) {                                                 \
        cycle_count += 64;   /* Big under-estimation here */                  \
        arm_hle_div_arm(arm);                                                 \
      }                                                                       \
      else {                                                                  \
        arm_swi();                                                            \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
  }                                                                           \
                                                                              \
  pc += 4                                                                     \

#define arm_flag_status()                                                     \

#define translate_thumb_instruction()                                         \
  flag_status = block_data[block_data_position].flag_data;                    \
  check_pc_region(pc);                                                        \
  last_opcode = opcode;                                                       \
  opcode = address16(pc_address_block, (pc & 0x7FFF));                        \
  emit_trace_thumb_instruction(pc);                                           \
  u8 hiop = opcode >> 8;                                                      \
                                                                              \
  switch(hiop)                                                                \
  {                                                                           \
    case 0x00 ... 0x07:                                                       \
      /* LSL rd, rs, imm */                                                   \
      thumb_shift(shift, lsl, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x08 ... 0x0F:                                                       \
      /* LSR rd, rs, imm */                                                   \
      thumb_shift(shift, lsr, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x10 ... 0x17:                                                       \
      /* ASR rd, rs, imm */                                                   \
      thumb_shift(shift, asr, imm);                                           \
      break;                                                                  \
                                                                              \
    case 0x18 ... 0x19:                                                       \
      /* ADD rd, rs, rn */                                                    \
      thumb_data_proc(add_sub, adds, reg, rd, rs, rn);                        \
      break;                                                                  \
                                                                              \
    case 0x1A ... 0x1B:                                                       \
      /* SUB rd, rs, rn */                                                    \
      thumb_data_proc(add_sub, subs, reg, rd, rs, rn);                        \
      break;                                                                  \
                                                                              \
    case 0x1C ... 0x1D:                                                       \
      /* ADD rd, rs, imm */                                                   \
      thumb_data_proc(add_sub_imm, adds, imm, rd, rs, imm);                   \
      break;                                                                  \
                                                                              \
    case 0x1E ... 0x1F:                                                       \
      /* SUB rd, rs, imm */                                                   \
      thumb_data_proc(add_sub_imm, subs, imm, rd, rs, imm);                   \
      break;                                                                  \
                                                                              \
    /* MOV r0..7, imm */                                                      \
    case 0x20: thumb_data_proc_unary(imm, movs, imm, 0, imm); break;          \
    case 0x21: thumb_data_proc_unary(imm, movs, imm, 1, imm); break;          \
    case 0x22: thumb_data_proc_unary(imm, movs, imm, 2, imm); break;          \
    case 0x23: thumb_data_proc_unary(imm, movs, imm, 3, imm); break;          \
    case 0x24: thumb_data_proc_unary(imm, movs, imm, 4, imm); break;          \
    case 0x25: thumb_data_proc_unary(imm, movs, imm, 5, imm); break;          \
    case 0x26: thumb_data_proc_unary(imm, movs, imm, 6, imm); break;          \
    case 0x27: thumb_data_proc_unary(imm, movs, imm, 7, imm); break;          \
                                                                              \
    /* CMP r0, imm */                                                         \
    case 0x28: thumb_data_proc_test(imm, cmp, imm, 0, imm); break;            \
    case 0x29: thumb_data_proc_test(imm, cmp, imm, 1, imm); break;            \
    case 0x2A: thumb_data_proc_test(imm, cmp, imm, 2, imm); break;            \
    case 0x2B: thumb_data_proc_test(imm, cmp, imm, 3, imm); break;            \
    case 0x2C: thumb_data_proc_test(imm, cmp, imm, 4, imm); break;            \
    case 0x2D: thumb_data_proc_test(imm, cmp, imm, 5, imm); break;            \
    case 0x2E: thumb_data_proc_test(imm, cmp, imm, 6, imm); break;            \
    case 0x2F: thumb_data_proc_test(imm, cmp, imm, 7, imm); break;            \
                                                                              \
    /* ADD r0..7, imm */                                                      \
    case 0x30: thumb_data_proc(imm, adds, imm, 0, 0, imm); break;             \
    case 0x31: thumb_data_proc(imm, adds, imm, 1, 1, imm); break;             \
    case 0x32: thumb_data_proc(imm, adds, imm, 2, 2, imm); break;             \
    case 0x33: thumb_data_proc(imm, adds, imm, 3, 3, imm); break;             \
    case 0x34: thumb_data_proc(imm, adds, imm, 4, 4, imm); break;             \
    case 0x35: thumb_data_proc(imm, adds, imm, 5, 5, imm); break;             \
    case 0x36: thumb_data_proc(imm, adds, imm, 6, 6, imm); break;             \
    case 0x37: thumb_data_proc(imm, adds, imm, 7, 7, imm); break;             \
                                                                              \
    /* SUB r0..7, imm */                                                      \
    case 0x38: thumb_data_proc(imm, subs, imm, 0, 0, imm); break;             \
    case 0x39: thumb_data_proc(imm, subs, imm, 1, 1, imm); break;             \
    case 0x3A: thumb_data_proc(imm, subs, imm, 2, 2, imm); break;             \
    case 0x3B: thumb_data_proc(imm, subs, imm, 3, 3, imm); break;             \
    case 0x3C: thumb_data_proc(imm, subs, imm, 4, 4, imm); break;             \
    case 0x3D: thumb_data_proc(imm, subs, imm, 5, 5, imm); break;             \
    case 0x3E: thumb_data_proc(imm, subs, imm, 6, 6, imm); break;             \
    case 0x3F: thumb_data_proc(imm, subs, imm, 7, 7, imm); break;             \
                                                                              \
    case 0x40:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* AND rd, rs */                                                    \
          thumb_data_proc(alu_op, ands, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* EOR rd, rs */                                                    \
          thumb_data_proc(alu_op, eors, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* LSL rd, rs */                                                    \
          thumb_shift(alu_op, lsl, reg);                                      \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* LSR rd, rs */                                                    \
          thumb_shift(alu_op, lsr, reg);                                      \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x41:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* ASR rd, rs */                                                    \
          thumb_shift(alu_op, asr, reg);                                      \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* ADC rd, rs */                                                    \
          thumb_data_proc(alu_op, adcs, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* SBC rd, rs */                                                    \
          thumb_data_proc(alu_op, sbcs, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* ROR rd, rs */                                                    \
          thumb_shift(alu_op, ror, reg);                                      \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x42:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* TST rd, rs */                                                    \
          thumb_data_proc_test(alu_op, tst, reg, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* NEG rd, rs */                                                    \
          thumb_data_proc_unary(alu_op, neg, reg, rd, rs);                    \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* CMP rd, rs */                                                    \
          thumb_data_proc_test(alu_op, cmp, reg, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* CMN rd, rs */                                                    \
          thumb_data_proc_test(alu_op, cmn, reg, rd, rs);                     \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x43:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* ORR rd, rs */                                                    \
          thumb_data_proc(alu_op, orrs, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* MUL rd, rs */                                                    \
          thumb_data_proc(alu_op, muls, reg, rd, rs, rd);                     \
          cycle_count += 2;  /* Between 1 and 4 extra cycles */               \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* BIC rd, rs */                                                    \
          thumb_data_proc(alu_op, bics, reg, rd, rd, rs);                     \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* MVN rd, rs */                                                    \
          thumb_data_proc_unary(alu_op, mvns, reg, rd, rs);                   \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x44:                                                                \
      /* ADD rd, rs */                                                        \
      thumb_data_proc_hi(add);                                                \
      break;                                                                  \
                                                                              \
    case 0x45:                                                                \
      /* CMP rd, rs */                                                        \
      thumb_data_proc_test_hi(cmp);                                           \
      break;                                                                  \
                                                                              \
    case 0x46:                                                                \
      /* MOV rd, rs */                                                        \
      thumb_data_proc_mov_hi();                                               \
      break;                                                                  \
                                                                              \
    case 0x47:                                                                \
      /* BX rs */                                                             \
      thumb_bx();                                                             \
      break;                                                                  \
                                                                              \
    case 0x48 ... 0x4F:                                                       \
      /* LDR r0..7, [pc + imm] */                                             \
      {                                                                       \
        thumb_decode_imm();                                                   \
        u32 rdreg = (hiop & 7);                                               \
        u32 aoff = (pc & ~2) + (imm*4) + 4;                                   \
        /* ROM + same page -> optimize as const load */                       \
        if (!ram_region && (((aoff + 4) >> 15) == (pc >> 15))) {              \
          u32 value = address32(pc_address_block, (aoff & 0x7FFF));           \
          thumb_load_pc_pool_const(rdreg, value);                             \
        } else {                                                              \
          thumb_access_memory(load, imm, rdreg, 0, 0, pc_relative, aoff, u32);\
        }                                                                     \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x50 ... 0x51:                                                       \
      /* STR rd, [rb + ro] */                                                 \
      thumb_access_memory(store, mem_reg, rd, rb, ro, reg_reg, 0, u32);       \
      break;                                                                  \
                                                                              \
    case 0x52 ... 0x53:                                                       \
      /* STRH rd, [rb + ro] */                                                \
      thumb_access_memory(store, mem_reg, rd, rb, ro, reg_reg, 0, u16);       \
      break;                                                                  \
                                                                              \
    case 0x54 ... 0x55:                                                       \
      /* STRB rd, [rb + ro] */                                                \
      thumb_access_memory(store, mem_reg, rd, rb, ro, reg_reg, 0, u8);        \
      break;                                                                  \
                                                                              \
    case 0x56 ... 0x57:                                                       \
      /* LDSB rd, [rb + ro] */                                                \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, s8);         \
      break;                                                                  \
                                                                              \
    case 0x58 ... 0x59:                                                       \
      /* LDR rd, [rb + ro] */                                                 \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, u32);        \
      break;                                                                  \
                                                                              \
    case 0x5A ... 0x5B:                                                       \
      /* LDRH rd, [rb + ro] */                                                \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, u16);        \
      break;                                                                  \
                                                                              \
    case 0x5C ... 0x5D:                                                       \
      /* LDRB rd, [rb + ro] */                                                \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, u8);         \
      break;                                                                  \
                                                                              \
    case 0x5E ... 0x5F:                                                       \
      /* LDSH rd, [rb + ro] */                                                \
      thumb_access_memory(load, mem_reg, rd, rb, ro, reg_reg, 0, s16);        \
      break;                                                                  \
                                                                              \
    case 0x60 ... 0x67:                                                       \
      /* STR rd, [rb + imm] */                                                \
      thumb_access_memory(store, mem_imm, rd, rb, 0, reg_imm, (imm * 4),      \
       u32);                                                                  \
      break;                                                                  \
                                                                              \
    case 0x68 ... 0x6F:                                                       \
      /* LDR rd, [rb + imm] */                                                \
      thumb_access_memory(load, mem_imm, rd, rb, 0, reg_imm, (imm * 4), u32); \
      break;                                                                  \
                                                                              \
    case 0x70 ... 0x77:                                                       \
      /* STRB rd, [rb + imm] */                                               \
      thumb_access_memory(store, mem_imm, rd, rb, 0, reg_imm, imm, u8);       \
      break;                                                                  \
                                                                              \
    case 0x78 ... 0x7F:                                                       \
      /* LDRB rd, [rb + imm] */                                               \
      thumb_access_memory(load, mem_imm, rd, rb, 0, reg_imm, imm, u8);        \
      break;                                                                  \
                                                                              \
    case 0x80 ... 0x87:                                                       \
      /* STRH rd, [rb + imm] */                                               \
      thumb_access_memory(store, mem_imm, rd, rb, 0, reg_imm,                 \
       (imm * 2), u16);                                                       \
      break;                                                                  \
                                                                              \
    case 0x88 ... 0x8F:                                                       \
      /* LDRH rd, [rb + imm] */                                               \
      thumb_access_memory(load, mem_imm, rd, rb, 0, reg_imm, (imm * 2), u16); \
      break;                                                                  \
                                                                              \
    /* STR r0..7, [sp + imm] */                                               \
    case 0x90:                                                                \
      thumb_access_memory(store, imm, 0, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x91:                                                                \
      thumb_access_memory(store, imm, 1, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x92:                                                                \
      thumb_access_memory(store, imm, 2, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x93:                                                                \
      thumb_access_memory(store, imm, 3, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x94:                                                                \
      thumb_access_memory(store, imm, 4, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x95:                                                                \
      thumb_access_memory(store, imm, 5, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x96:                                                                \
      thumb_access_memory(store, imm, 6, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
    case 0x97:                                                                \
      thumb_access_memory(store, imm, 7, 13, 0, reg_imm_sp, imm, u32);        \
      break;                                                                  \
                                                                              \
    /* LDR r0..7, [sp + imm] */                                               \
    case 0x98:                                                                \
      thumb_access_memory(load, imm, 0, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x99:                                                                \
      thumb_access_memory(load, imm, 1, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9A:                                                                \
      thumb_access_memory(load, imm, 2, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9B:                                                                \
      thumb_access_memory(load, imm, 3, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9C:                                                                \
      thumb_access_memory(load, imm, 4, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9D:                                                                \
      thumb_access_memory(load, imm, 5, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9E:                                                                \
      thumb_access_memory(load, imm, 6, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
    case 0x9F:                                                                \
      thumb_access_memory(load, imm, 7, 13, 0, reg_imm_sp, imm, u32);         \
      break;                                                                  \
                                                                              \
    /* ADD r0..7, pc, +imm */                                                 \
    case 0xA0: thumb_load_pc(0); break;                                       \
    case 0xA1: thumb_load_pc(1); break;                                       \
    case 0xA2: thumb_load_pc(2); break;                                       \
    case 0xA3: thumb_load_pc(3); break;                                       \
    case 0xA4: thumb_load_pc(4); break;                                       \
    case 0xA5: thumb_load_pc(5); break;                                       \
    case 0xA6: thumb_load_pc(6); break;                                       \
    case 0xA7: thumb_load_pc(7); break;                                       \
                                                                              \
    /* ADD r0..7, sp, +imm */                                                 \
    case 0xA8: thumb_load_sp(0); break;                                       \
    case 0xA9: thumb_load_sp(1); break;                                       \
    case 0xAA: thumb_load_sp(2); break;                                       \
    case 0xAB: thumb_load_sp(3); break;                                       \
    case 0xAC: thumb_load_sp(4); break;                                       \
    case 0xAD: thumb_load_sp(5); break;                                       \
    case 0xAE: thumb_load_sp(6); break;                                       \
    case 0xAF: thumb_load_sp(7); break;                                       \
                                                                              \
    case 0xB0 ... 0xB3:                                                       \
      if((opcode >> 7) & 0x01)                                                \
      {                                                                       \
        /* ADD sp, -imm */                                                    \
        thumb_adjust_sp(down);                                                \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        /* ADD sp, +imm */                                                    \
        thumb_adjust_sp(up);                                                  \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0xB4:                                                                \
      /* PUSH rlist */                                                        \
      thumb_block_memory(store, down, no, 13);                                \
      break;                                                                  \
                                                                              \
    case 0xB5:                                                                \
      /* PUSH rlist, lr */                                                    \
      thumb_block_memory(store, push_lr, push_lr, 13);                        \
      break;                                                                  \
                                                                              \
    case 0xBC:                                                                \
      /* POP rlist */                                                         \
      thumb_block_memory(load, no, up, 13);                                   \
      break;                                                                  \
                                                                              \
    case 0xBD:                                                                \
      /* POP rlist, pc */                                                     \
      thumb_block_memory(load, no, pop_pc, 13);                               \
      break;                                                                  \
                                                                              \
    case 0xC0:                                                                \
      /* STMIA r0!, rlist */                                                  \
      thumb_block_memory(store, no, up, 0);                                   \
      break;                                                                  \
                                                                              \
    case 0xC1:                                                                \
      /* STMIA r1!, rlist */                                                  \
      thumb_block_memory(store, no, up, 1);                                   \
      break;                                                                  \
                                                                              \
    case 0xC2:                                                                \
      /* STMIA r2!, rlist */                                                  \
      thumb_block_memory(store, no, up, 2);                                   \
      break;                                                                  \
                                                                              \
    case 0xC3:                                                                \
      /* STMIA r3!, rlist */                                                  \
      thumb_block_memory(store, no, up, 3);                                   \
      break;                                                                  \
                                                                              \
    case 0xC4:                                                                \
      /* STMIA r4!, rlist */                                                  \
      thumb_block_memory(store, no, up, 4);                                   \
      break;                                                                  \
                                                                              \
    case 0xC5:                                                                \
      /* STMIA r5!, rlist */                                                  \
      thumb_block_memory(store, no, up, 5);                                   \
      break;                                                                  \
                                                                              \
    case 0xC6:                                                                \
      /* STMIA r6!, rlist */                                                  \
      thumb_block_memory(store, no, up, 6);                                   \
      break;                                                                  \
                                                                              \
    case 0xC7:                                                                \
      /* STMIA r7!, rlist */                                                  \
      thumb_block_memory(store, no, up, 7);                                   \
      break;                                                                  \
                                                                              \
    case 0xC8:                                                                \
      /* LDMIA r0!, rlist */                                                  \
      thumb_block_memory(load, no, up, 0);                                    \
      break;                                                                  \
                                                                              \
    case 0xC9:                                                                \
      /* LDMIA r1!, rlist */                                                  \
      thumb_block_memory(load, no, up, 1);                                    \
      break;                                                                  \
                                                                              \
    case 0xCA:                                                                \
      /* LDMIA r2!, rlist */                                                  \
      thumb_block_memory(load, no, up, 2);                                    \
      break;                                                                  \
                                                                              \
    case 0xCB:                                                                \
      /* LDMIA r3!, rlist */                                                  \
      thumb_block_memory(load, no, up, 3);                                    \
      break;                                                                  \
                                                                              \
    case 0xCC:                                                                \
      /* LDMIA r4!, rlist */                                                  \
      thumb_block_memory(load, no, up, 4);                                    \
      break;                                                                  \
                                                                              \
    case 0xCD:                                                                \
      /* LDMIA r5!, rlist */                                                  \
      thumb_block_memory(load, no, up, 5);                                    \
      break;                                                                  \
                                                                              \
    case 0xCE:                                                                \
      /* LDMIA r6!, rlist */                                                  \
      thumb_block_memory(load, no, up, 6);                                    \
      break;                                                                  \
                                                                              \
    case 0xCF:                                                                \
      /* LDMIA r7!, rlist */                                                  \
      thumb_block_memory(load, no, up, 7);                                    \
      break;                                                                  \
                                                                              \
    case 0xD0:                                                                \
      /* BEQ label */                                                         \
      thumb_conditional_branch(eq);                                           \
      break;                                                                  \
                                                                              \
    case 0xD1:                                                                \
      /* BNE label */                                                         \
      thumb_conditional_branch(ne);                                           \
      break;                                                                  \
                                                                              \
    case 0xD2:                                                                \
      /* BCS label */                                                         \
      thumb_conditional_branch(cs);                                           \
      break;                                                                  \
                                                                              \
    case 0xD3:                                                                \
      /* BCC label */                                                         \
      thumb_conditional_branch(cc);                                           \
      break;                                                                  \
                                                                              \
    case 0xD4:                                                                \
      /* BMI label */                                                         \
      thumb_conditional_branch(mi);                                           \
      break;                                                                  \
                                                                              \
    case 0xD5:                                                                \
      /* BPL label */                                                         \
      thumb_conditional_branch(pl);                                           \
      break;                                                                  \
                                                                              \
    case 0xD6:                                                                \
      /* BVS label */                                                         \
      thumb_conditional_branch(vs);                                           \
      break;                                                                  \
                                                                              \
    case 0xD7:                                                                \
      /* BVC label */                                                         \
      thumb_conditional_branch(vc);                                           \
      break;                                                                  \
                                                                              \
    case 0xD8:                                                                \
      /* BHI label */                                                         \
      thumb_conditional_branch(hi);                                           \
      break;                                                                  \
                                                                              \
    case 0xD9:                                                                \
      /* BLS label */                                                         \
      thumb_conditional_branch(ls);                                           \
      break;                                                                  \
                                                                              \
    case 0xDA:                                                                \
      /* BGE label */                                                         \
      thumb_conditional_branch(ge);                                           \
      break;                                                                  \
                                                                              \
    case 0xDB:                                                                \
      /* BLT label */                                                         \
      thumb_conditional_branch(lt);                                           \
      break;                                                                  \
                                                                              \
    case 0xDC:                                                                \
      /* BGT label */                                                         \
      thumb_conditional_branch(gt);                                           \
      break;                                                                  \
                                                                              \
    case 0xDD:                                                                \
      /* BLE label */                                                         \
      thumb_conditional_branch(le);                                           \
      break;                                                                  \
                                                                              \
    case 0xDF:                                                                \
    {                                                                         \
      u32 swinum = opcode & 0xFF;                                             \
      if (swinum == 6) {                                                      \
        cycle_count += 64;   /* Big under-estimation here */                  \
        arm_hle_div(thumb);                                                   \
      }                                                                       \
      else if (swinum == 7) {                                                 \
        cycle_count += 64;   /* Big under-estimation here */                  \
        arm_hle_div_arm(thumb);                                               \
      }                                                                       \
      else {                                                                  \
        thumb_swi();                                                          \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xE0 ... 0xE7:                                                       \
    {                                                                         \
      /* B label */                                                           \
      thumb_b();                                                              \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xF0 ... 0xF7:                                                       \
    {                                                                         \
      /* (low word) BL label */                                               \
      /* This should possibly generate code if not in conjunction with a BLH  \
         next, but I don't think anyone will do that. */                      \
      break;                                                                  \
    }                                                                         \
                                                                              \
    case 0xF8 ... 0xFF:                                                       \
    {                                                                         \
      /* (high word) BL label */                                              \
      /* This might not be preceeding a BL low word (Golden Sun 2), if so     \
         it must be handled like an indirect branch. */                       \
      if((last_opcode >= 0xF000) && (last_opcode < 0xF800))                   \
      {                                                                       \
        thumb_bl();                                                           \
      }                                                                       \
      else                                                                    \
      {                                                                       \
        thumb_blh();                                                          \
      }                                                                       \
      break;                                                                  \
    }                                                                         \
  }                                                                           \
                                                                              \
  pc += 2                                                                     \

#define thumb_flag_modifies_all()                                             \
  flag_status |= 0xFF                                                         \

#define thumb_flag_modifies_zn()                                              \
  flag_status |= 0xCC                                                         \

#define thumb_flag_modifies_znc()                                             \
  flag_status |= 0xEE                                                         \

#define thumb_flag_modifies_zn_maybe_c()                                      \
  flag_status |= 0xCE                                                         \

#define thumb_flag_modifies_c()                                               \
  flag_status |= 0x22                                                         \

#define thumb_flag_requires_c()                                               \
  flag_status |= 0x200                                                        \

#define thumb_flag_requires_all()                                             \
  flag_status |= 0xF00                                                        \

#define thumb_flag_status()                                                   \
{                                                                             \
  u16 flag_status = 0;                                                        \
  switch((opcode >> 8) & 0xFF)                                                \
  {                                                                           \
    /* left shift by imm */                                                   \
    case 0x00 ... 0x07:                                                       \
      thumb_flag_modifies_zn();                                               \
      if(((opcode >> 6) & 0x1F) != 0)                                         \
      {                                                                       \
        thumb_flag_modifies_c();                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    /* right shift by imm */                                                  \
    case 0x08 ... 0x17:                                                       \
      thumb_flag_modifies_znc();                                              \
      break;                                                                  \
                                                                              \
    /* add, subtract */                                                       \
    case 0x18 ... 0x1F:                                                       \
      thumb_flag_modifies_all();                                              \
      break;                                                                  \
                                                                              \
    /* mov reg, imm */                                                        \
    case 0x20 ... 0x27:                                                       \
      thumb_flag_modifies_zn();                                               \
      break;                                                                  \
                                                                              \
    /* cmp reg, imm; add, subtract */                                         \
    case 0x28 ... 0x3F:                                                       \
      thumb_flag_modifies_all();                                              \
      break;                                                                  \
                                                                              \
    case 0x40:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* AND rd, rs */                                                    \
          thumb_flag_modifies_zn();                                           \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* EOR rd, rs */                                                    \
          thumb_flag_modifies_zn();                                           \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* LSL rd, rs */                                                    \
          thumb_flag_modifies_zn_maybe_c();                                   \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* LSR rd, rs */                                                    \
          thumb_flag_modifies_zn_maybe_c();                                   \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    case 0x41:                                                                \
      switch((opcode >> 6) & 0x03)                                            \
      {                                                                       \
        case 0x00:                                                            \
          /* ASR rd, rs */                                                    \
          thumb_flag_modifies_zn_maybe_c();                                   \
          break;                                                              \
                                                                              \
        case 0x01:                                                            \
          /* ADC rd, rs */                                                    \
          thumb_flag_modifies_all();                                          \
          thumb_flag_requires_c();                                            \
          break;                                                              \
                                                                              \
        case 0x02:                                                            \
          /* SBC rd, rs */                                                    \
          thumb_flag_modifies_all();                                          \
          thumb_flag_requires_c();                                            \
          break;                                                              \
                                                                              \
        case 0x03:                                                            \
          /* ROR rd, rs */                                                    \
          thumb_flag_modifies_zn_maybe_c();                                   \
          break;                                                              \
      }                                                                       \
      break;                                                                  \
                                                                              \
    /* TST, NEG, CMP, CMN */                                                  \
    case 0x42:                                                                \
      thumb_flag_modifies_all();                                              \
      break;                                                                  \
                                                                              \
    /* ORR, MUL, BIC, MVN */                                                  \
    case 0x43:                                                                \
      thumb_flag_modifies_zn();                                               \
      break;                                                                  \
                                                                              \
    case 0x45:                                                                \
      /* CMP rd, rs */                                                        \
      thumb_flag_modifies_all();                                              \
      break;                                                                  \
                                                                              \
    /* mov might change PC (fall through if so) */                            \
    case 0x46:                                                                \
      if((opcode & 0xFF87) != 0x4687)                                         \
        break;                                                                \
                                                                              \
    /* branches (can change PC) */                                            \
    case 0x47:                                                                \
    case 0xBD:                                                                \
    case 0xD0 ... 0xE7:                                                       \
    case 0xF0 ... 0xFF:                                                       \
      thumb_flag_requires_all();                                              \
      break;                                                                  \
  }                                                                           \
  block_data[block_data_position].flag_data = flag_status;                    \
}                                                                             \

// I/EWRAM memory tagging
// Code emitted in the RAM cache has tags (16 bit values) in the mirror tag ram
// that indicate that the address contains code. The following values are used:
// 0x0000 : this is just data (never translated)
// 0x00XX : not used (since first byte is zero)
// 0x0101 : this is code that is not the start of a translated block
// 0xXXXX : this is the start of a translated block, high tags counting down by 2
//          LSB is always set (we decrement by two) to ensure both bytes != 0
//
// The tag value is an index to a `ramtag_type` structure that sits at the end
// of the RAM CACHE (grows like a stack).
//
// INITIAL_TOP_TAG must not be 0xFFFF: partial_flush_ram_inner's left scan treats
// halfword 0xFFFF (SMC_TAG_UB16) as "stop without clearing". The first allocated
// block used to get 0xFFFF, so an interior SMC flush left that header + stale
// ramtag offsets (wrong tiles until something forced retranslate). Start at 0xFFFD.

#define LAST_TAG_NUM       0x0101
#define INITIAL_TOP_TAG    0xFFFD
#define CODE_TAG_BLOCK16   0x0101
#define CODE_TAG_BLOCK32   0x01010101

#define VALID_TAG(tagn) (tagn > LAST_TAG_NUM)

#define allocate_tag_arm(location) {   \
  location[0] = ram_block_tag;         \
  /* Could be another thumb inst */    \
  if (!location[1])                    \
    location[1] = CODE_TAG_BLOCK16;    \
  ram_block_tag -= 2;                  \
}

#define allocate_tag_thumb(location) { \
  location[0] = ram_block_tag;         \
  ram_block_tag -= 2;                  \
}

typedef struct
{
  u32 offset_arm;     // Cache offset to the ARM-mode compiled block
  u32 offset_thumb;   // Cache offset to the Thumb-mode compiled block
} ramtag_type;

static u32 ram_block_tag = INITIAL_TOP_TAG;

inline static ramtag_type* get_ram_tag(u16 tagval) {
  ramtag_type *tbl = (ramtag_type*)&ram_translation_cache[RAM_TRANSLATION_CACHE_SIZE];
  s16 tgidx = (s16)(tagval);
  return &tbl[tgidx >> 1];  /* Since LSB is always 1 and thus unused */
}

#if defined(NSPIRE_BUILD) && defined(ARM_ARCH)
/* speed10b (2026-11-18, made exact 2026-11-19): RAM blocks kept across
 * Partial SMC's flushes.
 *
 * A CPU store over translated RAM code flushes the whole RAM cache (the
 * stubs' SMC path, write_epilogue): every tag cleared, every block made
 * again as it next runs. Golden Sun rewrites a few words of its IWRAM mixer
 * whenever its mode changes (0x03000800), 1,197 flushes in 2,400 frames, and
 * translated the same ~430 blocks 23,600 times (15.8 MB of code, 0.32 ms of
 * each frame).
 *
 * The RAM cache is the old one: the same code at the same places, the same
 * tags, tag table, PC table and code bounds, flushed where it was (an SMC
 * store, its size, its PC table). Only how a block's code gets there
 * changes (Golden Sun cx2: 3.06 -> 2.86 ms a frame in the model). From the
 * first SMC flush on (Partial SMC, nspire_dynarec_ram_policy 0, the
 * default), each RAM block translated is also kept, with what its
 * translation read and left, in a store in the cache's free part (a ring
 * between RAM_KEEP_LO and RAM_KEEP_HI, emptied when the cache's code or
 * tag table would reach it; ram_keep_ent). When a block is to be made and
 * the store has one of its key (PC and mode) whose GBA bytes RAM still has,
 * and, where the translation reads it, the same scratch bit (scan_block
 * leaves the last instruction's block_data[].update_cycles as an earlier
 * translation left it when the scan ends there, and the translate loop
 * reads it), the kept code is copied to where the translation would write
 * it (ram_keep_use): its branches out of the block moved by the distance
 * (the places of every branch offset the translation encoded:
 * arm_relative_offset logs them), its PC table entries added there, the
 * tags, code bounds and scratch set as the scan and translate loop set
 * them. That is what the translation would write, byte for byte: what it
 * reads is the bytes, the key, the scratch bit, the ROM blocks its exits
 * link to (a ROM flush empties the store) and settings that only a whole
 * flush changes. The copy is made only where none of the translation's
 * checks would have flushed the cache on the way (the most room and PC
 * entries they needed, kept: ram_keep_full), else the block is translated
 * and flushes there; and only where its code stays clear of the store. A
 * span across a mirror's end isn't kept (the bytes compared wouldn't be the
 * ones read).
 *
 * tests/arm/run.sh: KEEP_CHECK=1 translates every block that would be
 * copied and compares it word for word with the copy (and the PC entries,
 * tags, bounds, scratch; every predicted flush is checked to happen);
 * KEEP_TRACE=1 builds with and without the keeping (NOKEEP=1) print the RAM
 * cache's code, tags, tag table and PC table and every block made as each
 * frame ends, compared line by line at 16, 32 KB, full size and with a
 * small PC table (RAMPCS=1). bc1a00a's first form used the kept code where
 * it was made and followed the old cache by counts: not exact (its real
 * cache started over and remade the old cache's blocks from other inputs;
 * its room test used a block's final size); these checks fail on it.
 *
 * Memory: none added: the store is in the RAM cache, its 128-slot table and
 * the scratch bitmap in arm_stub.S's nspire_ram_keep_table (the st/ld_lookup
 * tables' space, unused since core-10), the branch log in
 * nspire_block_tail_state's padding, the state in reg[] words 53-62. */
#ifdef NSPIRE_NO_RAM_KEEP
#define RAM_KEEP_ON          0   /* (tests/arm NOKEEP=1: as before speed10b) */
#else
#define RAM_KEEP_ON          (!nspire_dynarec_ram_policy)
#endif
#define RAM_KEEP_SLOTS       128
#define ram_keep_table       nspire_ram_keep_table
extern u32 nspire_ram_keep_table[RAM_KEEP_SLOTS + 32];   /* arm_stub.S */
/* the store: entries at positions (bytes written since the start) up to
   W; those from V on, and within the ring's size of W, are there */
#define ram_keep_w           reg[REG_RAM_KEEP_W]
#define ram_keep_v           reg[REG_RAM_KEEP_V]
#define ram_keep_ctx         reg[REG_RAM_KEEP_CTX]
/* the block being made: its instructions, whether its translation reads
   the last one's update_cycles as the scan found it, the most room and PC
   table entries its checks needed */
#define ram_keep_n           reg[REG_RAM_KEEP_N]
#define ram_keep_dep         reg[REG_RAM_KEEP_DEP]
#define ram_keep_need        reg[REG_RAM_KEEP_NEED]
#define ram_keep_need_pcs    reg[REG_RAM_KEEP_NEED_PCS]
#define ram_keep_uses        reg[REG_RAM_KEEP_USES]
#define ram_keep_clears      reg[REG_RAM_KEEP_CLEARS]
/* 1 from the first SMC flush on: before, nothing is kept or looked for (a
   game whose code never changes pays one test a translation) */
#define ram_keep_armed       reg[REG_RAM_KEEP_ARMED]
/* the update_cycles of the block being made, a bit an instruction (1024) */
#define ram_keep_bits_now    (nspire_ram_keep_table + RAM_KEEP_SLOTS)
/* The store's ring, in the cache's offsets [LO, HI), a power of two in
   size: below it the code of all but the largest spans between flushes
   (Golden Sun's are a few KB; 102 KB in the 384 KB cache), above it the
   tag table of up to SIZE / 16 / 8 tags. Code that would come within
   RAM_KEEP_MARGIN of LO (the room a block's checks leave for one
   instruction and its exits: TRANSLATION_CACHE_LIMIT_THRESHOLD, as the
   cache's own limit), or a tag table that would reach HI, empties it. */
#define RAM_KEEP_HI          (RAM_TRANSLATION_CACHE_SIZE -                    \
                              RAM_TRANSLATION_CACHE_SIZE / 16)
#define RAM_KEEP_ROOM        (RAM_KEEP_HI - 4 * TRANSLATION_CACHE_LIMIT_THRESHOLD)
#define RAM_KEEP_CAP                                                          \
  (RAM_KEEP_ROOM >= 262144 ? 262144 : RAM_KEEP_ROOM >= 131072 ? 131072 :     \
   RAM_KEEP_ROOM >= 65536 ? 65536 : RAM_KEEP_ROOM >= 32768 ? 32768 :         \
   RAM_KEEP_ROOM >= 16384 ? 16384 : RAM_KEEP_ROOM >= 8192 ? 8192 : 4096)
#define RAM_KEEP_LO          (RAM_KEEP_HI - RAM_KEEP_CAP)
#define RAM_KEEP_MARGIN      TRANSLATION_CACHE_LIMIT_THRESHOLD

typedef struct
{
  u32 key;          /* pc | thumb */
  u32 end;          /* the GBA code [key & ~1, end), within one mirror */
  u32 next;         /* this slot's older entry (position + 1; 0: none) */
  u32 size;         /* the entry's bytes */
  u32 code_at;      /* where its code was made */
  u32 code_size;
  u32 need;         /* the most room its translation's checks needed */
  u32 need_pcs;     /* and PC table entries */
  u16 n;            /* its instructions */
  u16 dep;          /* 1: made with the last one's update_cycles as found */
  u16 pcs;          /* its PC table entries */
  u16 relocs;       /* its branches out of the block */
} ram_keep_ent;     /* then the GBA bytes (padded to a word), the
                       update_cycles the translation left (a bit an
                       instruction), the PC entries (offset in the code, PC
                       word), the relocations (u16 word indices, padded),
                       the code */

#define ram_keep_ent_bits(e)                                                  \
  ((u32 *)((u8 *)((e) + 1) + (((e)->end - ((e)->key & ~1U) + 3) & ~3U)))
#define ram_keep_ent_pcs(e)                                                   \
  (ram_keep_ent_bits(e) + ((e)->n + 31) / 32)
#define ram_keep_ent_relocs(e)                                                \
  ((u16 *)(ram_keep_ent_pcs(e) + 2 * (e)->pcs))
#define ram_keep_ent_code(e)                                                  \
  ((u32 *)(ram_keep_ent_relocs(e) + (((e)->relocs + 1) & ~1U)))

static u32 ram_keep_slot(u32 key)
{
  return (key * 2654435761u) >> 25;
}

extern block_data_type block_data[];   /* (below: the translation's scratch) */

static ram_keep_ent *ram_keep_at(u32 pos)
{
  return (ram_keep_ent *)&ram_translation_cache[RAM_KEEP_LO +
   (pos & (RAM_KEEP_CAP - 1))];
}

/* the entry at pos still there (not emptied, not written over) */
static int ram_keep_live(u32 pos)
{
  return pos - ram_keep_v < ram_keep_w - ram_keep_v &&
         ram_keep_w - pos <= RAM_KEEP_CAP;
}

static void ram_keep_clear(void)
{
  if (ram_keep_v != ram_keep_w)
    ram_keep_clears++;
  ram_keep_v = ram_keep_w;
}

/* where the tags' table ends now (it grows down from the cache's top) */
static u8 *ram_keep_tags_low(void)
{
  return &ram_translation_cache[RAM_TRANSLATION_CACHE_SIZE -
   (0x10000 - ram_block_tag) / 2 * sizeof(ramtag_type)];
}

static u8 *ram_keep_bytes(u32 pc)
{
  return pc >= 0x03000000 ? &iwram[0x8000 + (pc & 0x7FFF)] : &ewram[pc & 0x3FFFF];
}

/* scan_block's tags and translate's update_pc_limits for the span (within
   one mirror: its offsets rise with its PCs) */
static void ram_keep_tags(u32 start, u32 end, u32 width)
{
  u32 n = (end - start) / width, lo, hi, i;
  u8 *t;
  if (start >= 0x03000000)
  {
    lo = start & 0x7FFF;
    hi = (end - width) & 0x7FFF;
    t = &iwram[lo];
    iwram_code_min = MIN(lo, iwram_code_min);
    iwram_code_max = MAX(hi, iwram_code_max);
  }
  else
  {
    lo = start & 0x3FFFF;
    hi = (end - width) & 0x3FFFF;
    t = &ewram[0x40000 + lo];
    ewram_code_min = MIN(lo, ewram_code_min);
    ewram_code_max = MAX(hi, ewram_code_max);
  }
  if (width == 4)
  {
    for (i = 0; i < n; i++, t += 4)
      if (!address32(t, 0))
        address32(t, 0) = CODE_TAG_BLOCK32;
  }
  else
  {
    for (i = 0; i < n; i++, t += 2)
      if (!address16(t, 0))
        address16(t, 0) = CODE_TAG_BLOCK16;
  }
}

#ifdef NSPIRE_KEEP_CHECK
/* tests/arm KEEP_CHECK=1: a block that would be copied is copied (the
   calculator's ram_keep_copy), what the copy made kept here, the copy
   undone, the block translated and compared with it (ram_keep_check); a
   predicted flush checked to happen. Also counted: copies with a branch
   into the ROM cache (an SWI's to bios_swi_entrypoint). */
u32 nspire_keep_check_ok, nspire_keep_check_bad, nspire_keep_check_flush_ok,
 nspire_keep_check_flush_bad, nspire_keep_check_skipped, nspire_keep_check_rom;
static u32 ram_keep_expect;       /* 1 a copy, 2 a flush */
static struct
{
  u32 key, end, n, dep, need, need_pcs, size, pcs;
  u8 *at;
  u32 code[16384];
  nspire_mem_pc_type pc[8192];
  u32 tags[1024];
  u32 bounds[4];
  u8 scratch[1024];
} ram_keep_made;
#endif
#if defined(NSPIRE_KEEP_CHECK) || defined(NSPIRE_KEEP_TRACE)
/* (tests/arm) ROM flushes that found kept blocks, the store started over
   at 2^31 */
u32 nspire_keep_rom_clears, nspire_keep_w_resets;
#endif
#ifdef NSPIRE_KEEP_TRACE
static void ram_trace_made(u32 key, u32 end, u32 n, u32 dep, u32 bit);
static void ram_trace_event(u32 v);
#endif

/* A kept block for this key whose bytes RAM still has (and the scratch
   bit its translation read), or NULL. */
static ram_keep_ent *ram_keep_find(u32 key)
{
  u32 at = ram_keep_table[ram_keep_slot(key)], k, start = key & ~1U;
  for (k = 0; at && k < 64; k++)
  {
    ram_keep_ent *e;
    if (!ram_keep_live(at - 1))
      return NULL;    /* (older ones are gone too) */
    e = ram_keep_at(at - 1);
    at = e->next;
    if (e->key == key &&
        !memcmp(ram_keep_bytes(start), e + 1, e->end - start) &&
        (!e->dep || (block_data[e->n - 1].update_cycles & 1) ==
          ((ram_keep_ent_bits(e)[(e->n - 1) >> 5] >> ((e->n - 1) & 31)) & 1)))
      return e;
  }
  return NULL;
}

/* The kept block e at dest as its translation writes it: the code (the
   branches out of the block moved by the distance), its PC table entries,
   the tags and code bounds, the scratch it leaves. */
static void ram_keep_copy(const ram_keep_ent *e, u8 *dest)
{
  const u32 *bits = ram_keep_ent_bits(e), *pcs = ram_keep_ent_pcs(e);
  const u16 *rel = ram_keep_ent_relocs(e);
  u32 *d = (u32 *)dest, i;
  s32 delta = (s32)(e->code_at - (u32)dest) >> 2;
  nspire_mem_pc_type *t = &nspire_pcs.ram[nspire_pcs.ram_count];

  memcpy(d, ram_keep_ent_code(e), e->code_size);
  for (i = 0; i < e->relocs; i++)
    d[rel[i]] = (d[rel[i]] & 0xFF000000) | ((d[rel[i]] + delta) & 0xFFFFFF);
  for (i = 0; i < e->pcs; i++)
  {
    t[i].host = (u32)dest + pcs[2 * i];
    t[i].pc = pcs[2 * i + 1];
  }
  nspire_pcs.ram_count += e->pcs;
  ram_keep_tags(e->key & ~1U, e->end, (e->key & 1) ? 2 : 4);
  for (i = 0; i < e->n; i++)
    block_data[i].update_cycles = (bits[i >> 5] >> (i & 31)) & 1;
  ram_translation_ptr = dest + e->code_size;
}

#ifdef NSPIRE_KEEP_CHECK
/* KEEP_CHECK: the copy made, what it made kept (ram_keep_made), then
   undone (the tags, bounds, scratch, PC table count and code pointer as
   they were) for the translation, which ram_keep_check compares. */
static void ram_keep_check_copy(const ram_keep_ent *e, u8 *dest)
{
  u32 start = e->key & ~1U, width = (e->key & 1) ? 2 : 4, i, pc;
  u32 pcs0 = nspire_pcs.ram_count, bounds[4];
  static u32 tags[1024];
  static u8 scratch[1024];
  const u16 *rel = ram_keep_ent_relocs(e);
  const u32 *code = ram_keep_ent_code(e);

  for (pc = start, i = 0; pc < e->end; pc += width, i++)
  {
    u8 *t = pc >= 0x03000000 ? &iwram[pc & 0x7FFF] :
                               &ewram[0x40000 + (pc & 0x3FFFF)];
    tags[i] = width == 4 ? address32(t, 0) : address16(t, 0);
  }
  bounds[0] = iwram_code_min;
  bounds[1] = iwram_code_max;
  bounds[2] = ewram_code_min;
  bounds[3] = ewram_code_max;
  for (i = 0; i < 1024; i++)
    scratch[i] = block_data[i].update_cycles;

  ram_keep_copy(e, dest);

  ram_keep_made.key = e->key;
  ram_keep_made.end = e->end;
  ram_keep_made.n = e->n;
  ram_keep_made.dep = e->dep;
  ram_keep_made.need = e->need;
  ram_keep_made.need_pcs = e->need_pcs;
  ram_keep_made.at = dest;
  ram_keep_made.size = ram_translation_ptr - dest;
  memcpy(ram_keep_made.code, dest, ram_keep_made.size);
  ram_keep_made.pcs = nspire_pcs.ram_count - pcs0;
  for (i = 0; i < ram_keep_made.pcs; i++)
  {
    ram_keep_made.pc[i].host = nspire_pcs.ram[pcs0 + i].host - (u32)dest;
    ram_keep_made.pc[i].pc = nspire_pcs.ram[pcs0 + i].pc;
  }
  for (pc = start, i = 0; pc < e->end; pc += width, i++)
  {
    u8 *t = pc >= 0x03000000 ? &iwram[pc & 0x7FFF] :
                               &ewram[0x40000 + (pc & 0x3FFFF)];
    ram_keep_made.tags[i] = width == 4 ? address32(t, 0) : address16(t, 0);
    if (width == 4)
      address32(t, 0) = tags[i];
    else
      address16(t, 0) = tags[i];
  }
  ram_keep_made.bounds[0] = iwram_code_min;
  ram_keep_made.bounds[1] = iwram_code_max;
  ram_keep_made.bounds[2] = ewram_code_min;
  ram_keep_made.bounds[3] = ewram_code_max;
  iwram_code_min = bounds[0];
  iwram_code_max = bounds[1];
  ewram_code_min = bounds[2];
  ewram_code_max = bounds[3];
  for (i = 0; i < 1024; i++)
  {
    ram_keep_made.scratch[i] = block_data[i].update_cycles;
    block_data[i].update_cycles = scratch[i];
  }
  nspire_pcs.ram_count = pcs0;
  ram_translation_ptr = dest;

  /* (a branch into the ROM cache among the moved ones) */
  for (i = 0; i < e->relocs; i++)
  {
    u32 w = code[rel[i]];
    u32 target = e->code_at + rel[i] * 4 + 8 + ((s32)(w << 8) >> 6);
    if (target - (u32)rom_translation_cache < ROM_TRANSLATION_CACHE_SIZE)
    {
      nspire_keep_check_rom++;
      break;
    }
  }
}
#endif

/* translate_block_*(pc, ram)'s first step (RAM_KEEP_TRY): 0 nothing kept
   (translate and keep it), 1 the kept block copied where the translation
   would write it (done), 2 one kept but not copied here (translate, which
   flushes if the copy wouldn't fit: not kept again). (In translate_block_*,
   not in the lookups: their code is pinned in the hot stretch.) */
static __attribute__((noinline)) u32 ram_keep_use(u32 key)
{
  ram_keep_ent *e = ram_keep_find(key);
  u8 *dest = ram_translation_ptr;
  u8 *limit = &ram_translation_cache[RAM_TRANSLATION_CACHE_SIZE -
   TRANSLATION_CACHE_LIMIT_THRESHOLD - (0x10000 - ram_block_tag) / 2 *
   sizeof(ramtag_type)];

  if (!e)
    return 0;
  /* translate_block_*'s check after one of its instructions would fail
     (translation_cache_limit, mem_pc_full): it flushes there */
  if (dest + e->need > limit ||
      nspire_pcs.ram_count + e->need_pcs + 64 > NSPIRE_RAM_PCS_LIMIT)
  {
#ifdef NSPIRE_KEEP_CHECK
    ram_keep_expect = 2;
#endif
    return 2;
  }
  /* the copy's code (at most need and the exits' room) clear of the store */
  if (dest + e->need + RAM_KEEP_MARGIN > &ram_translation_cache[RAM_KEEP_LO])
    return 2;
#ifdef NSPIRE_KEEP_CHECK
  if (e->code_size <= sizeof(ram_keep_made.code) && e->n <= 1024 &&
      e->pcs <= 8192)
  {
    ram_keep_check_copy(e, dest);
    ram_keep_expect = 1;
  }
  else
    nspire_keep_check_skipped++;
  return 2;
#endif
  ram_keep_copy(e, dest);
  ram_keep_uses++;
#ifdef NSPIRE_KEEP_TRACE
  ram_trace_made(key, e->end, e->n, e->dep,
   (ram_keep_ent_bits(e)[(e->n - 1) >> 5] >> ((e->n - 1) & 31)) & 1);
#endif
  return 1;
}
#endif

// This function will return a pointer to a translated block of code. If it
// doesn't exist it will translate it, if it does it will pass it back.

// type should be "arm", "thumb", or "dual." For arm or thumb the PC should
// be a real PC, for dual the least significant bit will determine if it's
// ARM or Thumb mode.

#define block_lookup_address_pc_arm()                                         \
  u32 thumb = 0;                                                              \
  pc &= ~0x03

#define block_lookup_address_pc_thumb()                                       \
  u32 thumb = 1;                                                              \
  pc &= ~0x01                                                                 \


#define block_lookup_translate_builder(type)                                  \
NSPIRE_HOT u8 function_cc *block_lookup_translate_##type(u32 pc)              \
{                                                                             \
  u8 pcregion = (pc >> 24);                                                   \
  u16 *location;                                                              \
  u32 block_tag;                                                              \
                                                                              \
  block_lookup_address_pc_##type();                                           \
                                                                              \
  switch(pcregion)                                                            \
  {                                                                           \
    case 0x2:                                                                 \
    case 0x3:                                                                 \
    {                                                                         \
      u16* tagp = (pcregion == 2) ? (u16 *)(ewram + (pc & 0x3FFFF) + 0x40000) \
                                  : (u16 *)(iwram + (pc & 0x7FFF));           \
      ramtag_type* trentry;                                                   \
      /* Allocate a tag if not a valid one, and initialize header */          \
      if (!VALID_TAG(*tagp)) {                                                \
        allocate_tag_##type(tagp);                                            \
        trentry = get_ram_tag(*tagp);                                         \
        trentry->offset_arm = 0;                                              \
        trentry->offset_thumb = 0;                                            \
      } else {                                                                \
        trentry = get_ram_tag(*tagp);                                         \
      }                                                                       \
                                                                              \
      if (!trentry->offset_##type) {                                          \
        bool result;                                                          \
        u8 *blkptr = ram_translation_ptr + block_prologue_size;               \
        trentry->offset_##type = blkptr - ram_translation_cache;              \
        result = translate_block_##type(pc, true);                              \
                                                                              \
        if (result)                                                           \
          return blkptr;                                                      \
      } else {                                                                \
        return &ram_translation_cache[trentry->offset_##type];                \
      }                                                                       \
      return NULL;                                                            \
    }                                                                         \
                                                                              \
    case 0x0:                                                                 \
    case 0x8 ... 0xD:                                                         \
    {                                                                         \
      u32 key = pc | thumb;                                                   \
      u32 hash_target = ((key * 2654435761U) >> (32 - ROM_BRANCH_HASH_BITS))  \
                                              & (ROM_BRANCH_HASH_SIZE - 1);   \
                                                                              \
      hashhdr_type *bhdr;                                                     \
      u32 blk_offset = rom_branch_hash[hash_target];                          \
      u32 *blk_offset_addr = &rom_branch_hash[hash_target];                   \
      while(blk_offset)                                                       \
      {                                                                       \
        bhdr = (hashhdr_type*)&rom_translation_cache[blk_offset];             \
        if(bhdr->pc_value == key)                                             \
          return &rom_translation_cache[                                      \
                  blk_offset + sizeof(hashhdr_type) + block_prologue_size];   \
                                                                              \
        blk_offset = bhdr->next_entry;                                        \
        blk_offset_addr = &bhdr->next_entry;                                  \
      }                                                                       \
                                                                              \
      { /* Not found, go ahead and translate, and backfill the hash table */  \
        u8 *blkptr;                                                           \
        bool result;                                                          \
        bhdr = (hashhdr_type*)rom_translation_ptr;                            \
        bhdr->pc_value = key;                                                 \
        bhdr->next_entry = 0;                                                 \
        *blk_offset_addr = (u32)(rom_translation_ptr - rom_translation_cache);\
        rom_translation_ptr += sizeof(hashhdr_type);                          \
        blkptr = rom_translation_ptr + block_prologue_size;                   \
        result = translate_block_##type(pc, false);                           \
                                                                              \
        if (result)                                                           \
          return blkptr;                                                      \
      }                                                                       \
      return NULL;                                                            \
    }                                                                         \
  }                                                                           \
                                                                              \
  /* Do not return NULL since it could indeed happen that some branch         \
     points to some random place (perhaps due to being garbage). This can     \
     happen when especulatively compiling code in RAM. Perhaps the game       \
     patches these instructions later, which would trigger a flush */         \
  return (u8*)(~0);                                                           \
}                                                                             \

block_lookup_translate_builder(arm);
block_lookup_translate_builder(thumb);

u8 function_cc *block_lookup_address_dual(u32 pc)
{
  u32 thumb = pc & 0x01;
  if(thumb) {
    pc &= ~1;
    reg[REG_CPSR] |= 0x20;
    return block_lookup_address_thumb(pc);
  } else {
    pc = (pc + 2) & ~0x03;
    reg[REG_CPSR] &= ~0x20;
    return block_lookup_address_arm(pc);
  }
}

#ifdef NSPIRE_BUILD
/* The blocks last found at ROM and BIOS addresses, which arm_stub.S's
 * branches look up first (cached_lookup): the hash table behind
 * block_lookup_address_* is 256 KB, so nearly every lookup there missed the
 * 8 KB data cache, and games look blocks up at every return from a
 * function and every interrupt. Key: the PC, Thumb ones with bit 0 set;
 * entry (key ^ (key >> 12)) & 2047 (BLOCK_CACHE_SIZE - 1; arm_stub.S
 * cached_lookup). Translated RAM code isn't kept here, any write can take it
 * away (arm_stub.S finds them from their tags). Emptied with the ROM cache
 * (keys ~1: no PC).
 * (2026-11-17, speed9b) Was (key >> 1) & 1023, 1024 entries: the BIOS's IRQ
 * vector 0x18 and Advance Wars' idle loop's return 0x08038819 shared entry
 * 12 and threw each other out at every interrupt (316 lookups in C a frame
 * of its 333 ROM/BIOS ones). Bits 12 up folded in, and twice the entries
 * (an ARM and a Thumb key of one PC side by side): the stub's index is as
 * many instructions as before, and the 8 KB more moves the .bss after it
 * by two whole ways of either cache. */
#define BLOCK_CACHE_SIZE 2048
u32 nspire_block_cache[BLOCK_CACHE_SIZE][2] __attribute__((aligned(32)));

/* Empty: ~1, which no key is (ARM keys are multiples of 4, Thumb ones odd):
 * arm_stub.S probes keys from 0x0E000000 up too (since speed9b), and a Thumb
 * branch to 0xFFFFFFFE has the key ~0. */
#define BLOCK_CACHE_EMPTY (~1U)
static void block_cache_clear(void)
{
  unsigned i;
  for (i = 0; i < BLOCK_CACHE_SIZE; i++)
    nspire_block_cache[i][0] = BLOCK_CACHE_EMPTY;
}

static inline void block_cache_put(u32 key, u8 *block)
{
  u32 *entry = nspire_block_cache[(key ^ (key >> 12)) & (BLOCK_CACHE_SIZE - 1)];
  if ((key >> 24) - 2 < 2 || key >= 0x0E000000)   /* RAM, or no ROM */
    return;
#ifdef NSPIRE_LOOKUP_COUNT
  {
    extern u32 nspire_lookup_evict[3];   /* empty slot, another key, this key */
    nspire_lookup_evict[entry[0] == BLOCK_CACHE_EMPTY ? 0 : entry[0] != key ? 1 : 2]++;
    if (entry[0] != BLOCK_CACHE_EMPTY && entry[0] != key)
    {
      /* the pairs that evict each other (perf prints the commonest) */
      extern u32 nspire_lookup_pairs[256][3];
      u32 a = entry[0] < key ? entry[0] : key, b = entry[0] < key ? key : entry[0], i;
      for (i = 0; i < 256 && nspire_lookup_pairs[i][2] &&
                  (nspire_lookup_pairs[i][0] != a || nspire_lookup_pairs[i][1] != b); i++)
        ;
      if (i < 256)
      {
        nspire_lookup_pairs[i][0] = a;
        nspire_lookup_pairs[i][1] = b;
        nspire_lookup_pairs[i][2]++;
      }
    }
  }
#endif
  entry[0] = key;
  entry[1] = (u32)block;
}
#else
#define block_cache_clear()
#define block_cache_put(key, block)
#endif

#ifdef NSPIRE_LIBRETRO
/* arm_stub.S: back to whatever called execute_arm_translate. */
extern u8 arm_return_to_main[];

/* No code can run at pc (the lookup's (u8*)~0: no memory, I/O, save
 * memory...), or it couldn't be translated: stop there (cpu.h) rather than
 * jump to that marker, which crashed the calculator. The lookups get the
 * PC with its low bits as the branch left them (Thumb bit, ARM + 2). */
static u8 *nspire_stop_at(u32 pc)
{
  nspire_bad_jump = 1;
  nspire_bad_jump_pc = pc;
  return arm_return_to_main;
}
#endif

#ifdef NSPIRE_LOOKUP_COUNT
/* Model-only (tests/arm/perf.c, make ... LOOKUP_COUNT=1): the lookups that
 * reach C, [thumb][0 ROM/BIOS, 1 IWRAM/EWRAM, 2 other]. No build of the
 * calculator or of the speed model has them. */
u32 nspire_lookup_count[2][3], nspire_lookup_evict[3], nspire_lookup_pairs[256][3];
#define lookup_count(thumb, pc)                                               \
  nspire_lookup_count[thumb][((pc) >> 24) - 2 < 2 ? 1 :                       \
                             (pc) < 0x0E000000 ? 0 : 2]++
#else
#define lookup_count(thumb, pc)
#endif

NSPIRE_HOT u8 function_cc *block_lookup_address_arm(u32 pc)
{
  unsigned i;
  lookup_count(0, pc);
  for (i = 0; i < 4; i++) {
    u8 *ret = block_lookup_translate_arm(pc);
    if (ret) {
#ifdef NSPIRE_LIBRETRO
      if (ret == (u8 *)(~0))
        return nspire_stop_at(pc & ~3);
#endif
      translate_icache_sync();
      block_cache_put(pc & ~3, ret);
      return ret;
    }
  }

  printf("bad jump %x (%x)\n", pc, reg[REG_PC]);
  fflush(stdout);
#ifdef NSPIRE_LIBRETRO
  return nspire_stop_at(pc & ~3);
#else
  return NULL;
#endif
}

NSPIRE_HOT u8 function_cc *block_lookup_address_thumb(u32 pc)
{
  unsigned i;
  lookup_count(1, pc);
  for (i = 0; i < 4; i++) {
    u8 *ret = block_lookup_translate_thumb(pc);
    if (ret) {
#ifdef NSPIRE_LIBRETRO
      if (ret == (u8 *)(~0))
        return nspire_stop_at(pc & ~1);
#endif
      translate_icache_sync();
      block_cache_put((pc & ~1) | 1, ret);
      return ret;
    }
  }
  printf("bad jump %x (%x)\n", pc, reg[REG_PC]);
  fflush(stdout);
#ifdef NSPIRE_LIBRETRO
  return nspire_stop_at(pc & ~1);
#else
  return NULL;
#endif
}

// Potential exit point: If the rd field is pc for instructions is 0x0F,
// the instruction is b/bl/bx, or the instruction is ldm with PC in the
// register list.
// All instructions with upper 3 bits less than 100b have an rd field
// except bx, where the bits must be 0xF there anyway, multiplies,
// which cannot have 0xF in the corresponding fields, and msr, which
// has 0x0F there but doesn't end things (therefore must be special
// checked against). Because MSR and BX overlap both are checked for.

#define arm_exit_point                                                        \
 (((opcode < 0x8000000) && ((opcode & 0x000F000) == 0x000F000) &&             \
  ((opcode & 0xDB0F000) != 0x120F000)) ||                                     \
  ((opcode & 0x12FFF10) == 0x12FFF10) ||                                      \
  ((opcode & 0x8108000) == 0x8108000) ||                                      \
  ((opcode >= 0xA000000) && (opcode < 0xF000000)) ||                          \
  ((opcode >= 0xF000000) && (!is_div_swi((opcode >> 16) & 0xFF))))            \

#define arm_opcode_branch                                                     \
  ((opcode & 0xE000000) == 0xA000000)                                         \

#define arm_opcode_swi                                                        \
  ((opcode & 0xF000000) == 0xF000000)                                         \

#define arm_opcode_unconditional_branch                                       \
  (condition == 0x0E)                                                         \

#define arm_load_opcode()                                                     \
  opcode = address32(pc_address_block, (block_end_pc & 0x7FFF));              \
  condition = opcode >> 28;                                                   \
                                                                              \
  opcode &= 0xFFFFFFF;                                                        \
                                                                              \
  block_end_pc += 4                                                           \

#define arm_branch_target()                                                   \
  branch_target = (block_end_pc + 4 + (((s32)(opcode & 0xFFFFFF) << 8) >> 6)) \

// Contiguous conditional block flags modification - it will set 0x20 in the
// condition's bits if this instruction modifies flags. Taken from the CPU
// switch so it'd better be right this time.

#define arm_set_condition(_condition)                                         \
  block_data[block_data_position].condition = _condition;                     \
  switch((opcode >> 20) & 0xFF)                                               \
  {                                                                           \
    case 0x01:                                                                \
    case 0x03:                                                                \
    case 0x09:                                                                \
    case 0x0B:                                                                \
    case 0x0D:                                                                \
    case 0x0F:                                                                \
      if((((opcode >> 5) & 0x03) == 0) || ((opcode & 0x90) != 0x90))          \
        block_data[block_data_position].condition |= 0x20;                    \
      break;                                                                  \
                                                                              \
    case 0x05:                                                                \
    case 0x07:                                                                \
    case 0x11:                                                                \
    case 0x13:                                                                \
    case 0x15 ... 0x17:                                                       \
    case 0x19:                                                                \
    case 0x1B:                                                                \
    case 0x1D:                                                                \
    case 0x1F:                                                                \
      if((opcode & 0x90) != 0x90)                                             \
        block_data[block_data_position].condition |= 0x20;                    \
      break;                                                                  \
                                                                              \
    case 0x12:                                                                \
      if(((opcode & 0x90) != 0x90) && !(opcode & 0x10))                       \
        block_data[block_data_position].condition |= 0x20;                    \
      break;                                                                  \
                                                                              \
    case 0x21:                                                                \
    case 0x23:                                                                \
    case 0x25:                                                                \
    case 0x27:                                                                \
    case 0x29:                                                                \
    case 0x2B:                                                                \
    case 0x2D:                                                                \
    case 0x2F ... 0x37:                                                       \
    case 0x39:                                                                \
    case 0x3B:                                                                \
    case 0x3D:                                                                \
    case 0x3F:                                                                \
      block_data[block_data_position].condition |= 0x20;                      \
    break;                                                                    \
  }                                                                           \

#define arm_instruction_width 4

#define arm_base_cycles()                                                     \
  cycle_count += def_seq_cycles[pc >> 24][1]                                  \

// For now this just sets a variable that says flags should always be
// computed.

#define arm_dead_flag_eliminate()                                             \
  flag_status = 0xF                                                           \

// The following Thumb instructions can exit:
// b, bl, bx, swi, pop {... pc}, and mov pc, ..., the latter being a hireg
// op only. Rather simpler to identify than the ARM set.

#define thumb_exit_point                                                      \
  (((opcode >= 0xD000) && (opcode < 0xDF00)) ||                               \
   (((opcode & 0xFF00) == 0xDF00) &&                                          \
    (!is_div_swi(opcode & 0xFF))) ||                                          \
   ((opcode >= 0xE000) && (opcode < 0xE800)) ||                               \
   ((opcode & 0xFF00) == 0x4700) ||                                           \
   ((opcode & 0xFF00) == 0xBD00) ||                                           \
   ((opcode & 0xFF87) == 0x4687) ||                                           \
   ((opcode >= 0xF800)))                                                      \

#define thumb_opcode_branch                                                   \
  (((opcode >= 0xD000) && (opcode < 0xDF00)) ||                               \
   ((opcode >= 0xE000) && (opcode < 0xE800)) ||                               \
   (opcode >= 0xF800))                                                        \

#define thumb_opcode_swi                                                      \
  ((opcode & 0xFF00) == 0xDF00)                                               \

#define thumb_opcode_unconditional_branch                                     \
  ((opcode < 0xD000) || (opcode >= 0xDF00))                                   \

#define thumb_load_opcode()                                                   \
  last_opcode = opcode;                                                       \
  opcode = address16(pc_address_block, (block_end_pc & 0x7FFF));              \
                                                                              \
  block_end_pc += 2                                                           \

#define thumb_branch_target()                                                 \
  if(opcode < 0xE000)                                                         \
  {                                                                           \
    branch_target = block_end_pc + 2 + ((s8)(opcode & 0xFF) * 2);             \
  }                                                                           \
  else                                                                        \
                                                                              \
  if(opcode < 0xF800)                                                         \
  {                                                                           \
    branch_target = block_end_pc + 2 + ((s32)((opcode & 0x7FF) << 21) >> 20); \
  }                                                                           \
  else                                                                        \
  {                                                                           \
    if((last_opcode >= 0xF000) && (last_opcode < 0xF800))                     \
    {                                                                         \
      branch_target =                                                         \
       (block_end_pc + ((s32)((last_opcode & 0x07FF) << 21) >> 9) +           \
       ((opcode & 0x07FF) * 2));                                              \
    }                                                                         \
    else                                                                      \
    {                                                                         \
      goto no_direct_branch;                                                  \
    }                                                                         \
  }                                                                           \

#define thumb_set_condition(_condition)                                       \

#define thumb_instruction_width 2

#define thumb_base_cycles()                                                   \
  cycle_count += def_seq_cycles[pc >> 24][0]                                  \

// Here's how this works: each instruction has three different sets of flag
// attributes, each consisiting of a 4bit mask describing how that instruction
// interacts with the 4 main flags (N/Z/C/V).
// The first set, in bits 0:3, is the set of flags the instruction may
// modify. After this pass this is changed to the set of flags the instruction
// should modify - if the bit for the corresponding flag is not set then code
// does not have to be generated to calculate the flag for that instruction.

// The second set, in bits 7:4, is the set of flags that the instruction must
// modify (ie, for shifts by the register values the instruction may not
// always modify the C flag, and thus the C bit won't be set here).

// The third set, in bits 11:8, is the set of flags that the instruction uses
// in its computation, or the set of flags that will be needed after the
// instruction is done. For any instructions that change the PC all of the
// bits should be set because it is (for now) unknown what flags will be
// needed after it arrives at its destination. Instructions that use the
// carry flag as input will have it set as well.

// The algorithm is a simple liveness analysis procedure: It starts at the
// bottom of the instruction stream and sets a "currently needed" mask to
// the flags needed mask of the current instruction. Then it moves down
// an instruction, ANDs that instructions "should generate" mask by the
// "currently needed" mask, then ANDs the "currently needed" mask by
// the 1's complement of the instruction's "must generate" mask, and ORs
// the "currently needed" mask by the instruction's "flags needed" mask.

#define thumb_dead_flag_eliminate()                                           \
{                                                                             \
  u32 needed_mask = 0xff;                                                     \
                                                                              \
  while(--block_data_position >= 0)                                           \
  {                                                                           \
    flag_status = block_data[block_data_position].flag_data;                  \
    block_data[block_data_position].flag_data =                               \
     (flag_status & needed_mask);                                             \
    needed_mask &= ~((flag_status >> 4) & 0x0F);                              \
    needed_mask |= flag_status >> 8;                                          \
  }                                                                           \
}                                                                             \

#define MAX_BLOCK_SIZE   1024   // 2/4KiB blocks max
#define MAX_EXITS          32   // This covers 99% blocks

block_data_type block_data[MAX_BLOCK_SIZE];
block_exit_type block_exits[MAX_EXITS];

#define smc_write_arm_yes() {                                                 \
  intptr_t offset = (pc < 0x03000000) ? 0x40000 : -0x8000;                    \
  if(address32(pc_address_block, (block_end_pc & 0x7FFF) + offset) == 0)      \
  {                                                                           \
    address32(pc_address_block, (block_end_pc & 0x7FFF) + offset) =           \
      CODE_TAG_BLOCK32;                                                       \
  }                                                                           \
}

#define smc_write_thumb_yes() {                                               \
  intptr_t offset = (pc < 0x03000000) ? 0x40000 : -0x8000;                    \
  if(address16(pc_address_block, (block_end_pc & 0x7FFF) + offset) == 0)      \
  {                                                                           \
    address16(pc_address_block, (block_end_pc & 0x7FFF) + offset) =           \
      CODE_TAG_BLOCK16;                                                       \
  }                                                                           \
}

#define smc_write_arm_no()                                                    \

#define smc_write_thumb_no()                                                  \

/* core-13: true when the instruction that ended the block (scan_block's
 * break with no earlier exit to block_end_pc) leaves it on every path, so
 * the translation gate after it is dead code. Thumb: every unconditional
 * exit point does: b and bl/blh (generate_branch / indirect), bx, pop {pc}
 * and mov pc (indirect branch, also after the per-register code's join),
 * swi but the HLE divide (not an exit point). ARM: only an unconditional
 * B, BL, exact BX or SWI (not 6/7); the other exit points (rd = 15 forms,
 * ldm with pc) keep the gate, and a conditional last instruction's skip
 * is patched to land on it. */
#define thumb_block_end_exits 1

#define arm_block_end_exits                                                   \
  ((condition == 0x0E) &&                                                     \
   (arm_opcode_branch || ((opcode & 0xFFFFFF0) == 0x12FFF10) ||               \
    (arm_opcode_swi && !is_div_swi((opcode >> 16) & 0xFF))))                  \

#ifdef NSPIRE_GATE_TRAP
/* Debug builds (tests/arm GATE_TRAP=1): an undefined instruction where a
 * gate was left out; it must never run. */
#define generate_translation_gate_skipped() write32(0xE7F000F0)
#else
#define generate_translation_gate_skipped()
#endif

#define scan_block(type, smc_write_op)                                        \
{                                                                             \
  __label__ block_end;                                                        \
  /* Find the end of the block */                                             \
  do                                                                          \
  {                                                                           \
    check_pc_region(block_end_pc);                                            \
    smc_write_##type##_##smc_write_op();                                      \
    type##_load_opcode();                                                     \
    type##_flag_status();                                                     \
                                                                              \
    if(type##_exit_point)                                                     \
    {                                                                         \
      /* Branch/branch with link */                                           \
      if(type##_opcode_branch)                                                \
      {                                                                       \
        __label__ no_direct_branch;                                           \
        type##_branch_target();                                               \
        block_exits[block_exit_position].branch_target = branch_target;       \
        block_exit_position++;                                                \
                                                                              \
        /* Give the branch target macro somewhere to bail if it turns out to  \
           be an indirect branch (ala malformed Thumb bl) */                  \
        no_direct_branch:;                                                    \
      }                                                                       \
                                                                              \
      /* SWI branches to the BIOS, unless it's an HLE call, then it is        \
         not parsed as an exit_point but rather an "instruction" of sorts. */ \
      if(type##_opcode_swi)                                                   \
      {                                                                       \
        block_exits[block_exit_position].branch_target = 0x00000008;          \
        block_exit_position++;                                                \
      }                                                                       \
                                                                              \
      type##_set_condition(condition | 0x10);                                 \
                                                                              \
      /* Only unconditional branches can end the block. */                    \
      if(type##_opcode_unconditional_branch)                                  \
      {                                                                       \
        /* Check to see if any prior block exits branch after here,           \
           if so don't end the block. Starts from the top and works           \
           down because the most recent branch is most likely to              \
           join after the end (if/then form) */                               \
        for(i = block_exit_position - 2; i >= 0; i--)                         \
        {                                                                     \
          if(block_exits[i].branch_target == block_end_pc)                    \
            break;                                                            \
        }                                                                     \
                                                                              \
        if(i < 0)                                                             \
        {                                                                     \
          block_end_exits = type##_block_end_exits;                           \
          break;                                                              \
        }                                                                     \
      }                                                                       \
      if(block_exit_position == MAX_EXITS)                                    \
        break;                                                                \
    }                                                                         \
    else                                                                      \
    {                                                                         \
      type##_set_condition(condition);                                        \
    }                                                                         \
                                                                              \
    for(i = 0; i < translation_gate_targets; i++)                             \
    {                                                                         \
      if(block_end_pc == translation_gate_target_pc[i])                       \
        goto block_end;                                                       \
    }                                                                         \
                                                                              \
    block_data[block_data_position].update_cycles = 0;                        \
    block_data_position++;                                                    \
    if((block_data_position == MAX_BLOCK_SIZE) ||                             \
     (block_end_pc == 0x3007FF0) || (block_end_pc == 0x203FFFF0))             \
    {                                                                         \
      break;                                                                  \
    }                                                                         \
  } while(1);                                                                 \
                                                                              \
  block_end:;                                                                 \
}                                                                             \

#define arm_fix_pc()                                                          \
  pc &= ~0x03                                                                 \

#define thumb_fix_pc()                                                        \
  pc &= ~0x01                                                                 \

#define update_pc_limits()                                                    \
if (ram_region) {                                                             \
  if (pc >= 0x3000000) {                                                      \
    iwram_code_min = MIN(pc & 0x7FFF, iwram_code_min);                        \
    iwram_code_max = MAX(pc & 0x7FFF, iwram_code_max);                        \
  } else {                                                                    \
    ewram_code_min = MIN(pc & 0x3FFFF, ewram_code_min);                       \
    ewram_code_max = MAX(pc & 0x3FFFF, ewram_code_max);                       \
  }                                                                           \
}                                                                             \

#if defined(NSPIRE_BUILD) && defined(ARM_ARCH)
static void ram_keep_begin(void);
static void ram_keep_scanned(u32 start, u32 end, u32 width, s32 exits, s32 scanned);
static void ram_keep_full(u8 *ptr, u8 *start, u32 pcs0);
static void ram_keep_end(u32 key, u32 end, u8 *start, u32 pcs0, u32 ctx0, u32 found);

/* translate_block_*'s hooks: one test each while nothing is kept (keep_on
   0: a ROM block, Classic SMC, before the first SMC flush), the work out
   of line (the translation loop's code stays small: its fetches cost the
   ROM translations) */
#define RAM_KEEP_START(thumbbit)                                              \
  u8 *keep_start = ram_translation_ptr;                                       \
  u32 keep_pcs0 = nspire_pcs.ram_count, keep_ctx = ram_keep_ctx;              \
  u32 keep_found = 0;                                                         \
  const u32 keep_on = ram_region && RAM_KEEP_ON && ram_keep_armed;            \
  if (keep_on)                                                                \
    ram_keep_begin()

/* At a RAM block's translation: the kept one's code copied, if any. */
#define RAM_KEEP_TRY(thumbbit)                                                \
  if (keep_on && (keep_found = ram_keep_use(pc | (thumbbit))) == 1)           \
    return true

/* After the scan and the internal targets (ram_keep_scanned). */
#define RAM_KEEP_SCANNED(width)                                               \
  if (keep_on)                                                                \
    ram_keep_scanned(block_start_pc, block_end_pc, (width),                   \
     block_exit_position, block_data_position)

/* With translate_block_*'s check after each instruction (ram_keep_full):
   the room and PC entries it needs, the store kept clear. */
#define RAM_KEEP_FULL_CHECK()                                                 \
  if (keep_on)                                                                \
    ram_keep_full(translation_ptr, keep_start, keep_pcs0)

#define RAM_KEEP_RECORD(thumbbit)                                             \
  if (keep_on)                                                                \
    ram_keep_end(block_start_pc | (thumbbit), block_end_pc, keep_start,       \
     keep_pcs0, keep_ctx, keep_found)
#else
#define RAM_KEEP_START(thumbbit)
#define RAM_KEEP_TRY(thumbbit)
#define RAM_KEEP_SCANNED(width)
#define RAM_KEEP_FULL_CHECK()
#define RAM_KEEP_RECORD(thumbbit)
#endif

#ifdef NSPIRE_KEEP_TRACE
/* tests/arm KEEP_TRACE=1: every RAM block made, by what it was made from
   (ram_trace_made), with and without the keeping */
static void ram_trace_scanned(u32 start, u32 end, u32 width, s32 exits, s32 scanned);
static void ram_trace_done(u32 key, u32 end);
#define RAM_TRACE_SCANNED(width)                                              \
  if (ram_region)                                                             \
    ram_trace_scanned(block_start_pc, block_end_pc, (width),                  \
     block_exit_position, block_data_position)
#define RAM_TRACE_DONE(thumbbit)                                              \
  if (ram_region)                                                             \
    ram_trace_done(block_start_pc | (thumbbit), block_end_pc)
#else
#define RAM_TRACE_SCANNED(width)
#define RAM_TRACE_DONE(thumbbit)
#endif

bool translate_block_arm(u32 pc, bool ram_region)
{
  u32 opcode = 0;
  u32 last_opcode;
  u32 condition;
  u32 last_condition;
  u32 pc_region = (pc >> 15);
  u32 new_pc_region;
  u8 *pc_address_block = memory_map_read[pc_region];
  u32 block_start_pc = pc;
  u32 block_end_pc = pc;
  u32 block_exit_position = 0;
  s32 block_data_position = 0;
  u32 external_block_exit_position = 0;
  u32 branch_target;
  u32 cycle_count = 0;
  u8 *translation_target;
  u8 *backpatch_address = NULL;
  u8 *translation_ptr = NULL;
  u8 *translation_cache_limit = NULL;
  s32 i;
  u32 flag_status;
  u32 block_end_exits = 0;
  block_exit_type external_block_exits[MAX_EXITS];
  generate_block_extra_vars_arm();
  arm_fix_pc();

  if(!pc_address_block)
    pc_address_block = load_gamepak_page(pc_region & 0x3FF);

  mem_pc_settle();
  RAM_KEEP_START(0);
  RAM_KEEP_TRY(0);
  if (ram_region) {
    translation_ptr = ram_translation_ptr;
    translation_cache_limit = &ram_translation_cache[
       RAM_TRANSLATION_CACHE_SIZE - TRANSLATION_CACHE_LIMIT_THRESHOLD
       - (0x10000 - ram_block_tag) / 2 * sizeof(ramtag_type)];
  } else {
    translation_ptr = rom_translation_ptr;
    translation_cache_limit =
     rom_translation_cache + ROM_TRANSLATION_CACHE_SIZE -
     TRANSLATION_CACHE_LIMIT_THRESHOLD;
  }

  generate_block_prologue();
  generate_block_tails_start();

  /* This is a function because it's used a lot more than it might seem (all
     of the data processing functions can access it), and its expansion was
     massacreing the compiler. */

  if(ram_region)
  {
    scan_block(arm, yes);
  }
  else
  {
    scan_block(arm, no);
  }

  for(i = 0; i < block_exit_position; i++)
  {
    branch_target = block_exits[i].branch_target;

    if((branch_target > block_start_pc) &&
     (branch_target < block_end_pc))
    {
      block_data[(branch_target - block_start_pc) /
       arm_instruction_width].update_cycles = 1;
    }
  }
  RAM_KEEP_SCANNED(arm_instruction_width);
  RAM_TRACE_SCANNED(arm_instruction_width);

  arm_dead_flag_eliminate();

  block_exit_position = 0;
  block_data_position = 0;

  last_condition = 0x0E;

  while(pc != block_end_pc)
  {
    block_data[block_data_position].block_offset = translation_ptr;
    arm_base_cycles();

    if (pc == cheat_master_hook)
    {
      arm_process_cheats();
    }

    update_pc_limits();
    translate_arm_instruction();
    block_data_position++;

    /* If it went too far the cache needs to be flushed and the process
       restarted. Because we might already be nested several stages in
       a simple recursive call here won't work, it has to pedal out to
       the beginning. */

    RAM_KEEP_FULL_CHECK();
    if(translation_ptr + block_tails_bytes > translation_cache_limit ||
     mem_pc_full(ram_region, translation_ptr)) {
      if (ram_region)
        flush_translation_cache_ram();
      else
        flush_translation_cache_rom();
      return false;
    }

    /* If the next instruction is a block entry point update the
       cycle counter and update */
    if (pc != block_end_pc &&
        block_data[block_data_position].update_cycles)
    {
      generate_cycle_update();
    }
  }

  /* This can happen if the last instruction is *not* inconditional */
  if ((last_condition & 0x0F) != 0x0E) {
    generate_branch_patch_conditional(backpatch_address, translation_ptr);
  }

  /* The translation gate, for a block that can run off its end (a gate
     target, MAX_BLOCK_SIZE, MAX_EXITS, the special PCs, a conditional or
     non-branching last instruction); none after one that always leaves
     (core-13, arm_block_end_exits). */
  if(!block_end_exits)
  {
    generate_translation_gate(arm);
  }
  else
  {
    generate_translation_gate_skipped();
  }

  for(i = 0; i < block_exit_position; i++)
  {
    branch_target = block_exits[i].branch_target;

    if((branch_target >= block_start_pc) && (branch_target < block_end_pc))
    {
      /* Internal branch, patch to recorded address */
      translation_target =
       block_data[(branch_target - block_start_pc) /
        arm_instruction_width].block_offset;

      generate_branch_patch_unconditional(block_exits[i].branch_source,
       translation_target);
    }
    else
    {
      /* Linked directly (patched once the target is translated): BIOS and
       * cart ROM; with Classic SMC also IW/EW RAM, as a full flush drops
       * every block on a RAM write. Partial SMC drops only the written
       * block, so a direct link into RAM code could run stale code: those
       * exits go through the PC lookup below. */
#if defined(NSPIRE_LIBRETRO)
      if ((nspire_dynarec_ram_policy
            && GPSP_PC_RELATIVE_TRANS_REGION(branch_target))
          || (!nspire_dynarec_ram_policy
              && (branch_target < 0x00004000u
                  || (branch_target >= 0x08000000u
                      && branch_target < 0x0E000000u))))
#else
      if (branch_target < 0x00004000u
          || (branch_target >= 0x08000000u && branch_target < 0x0E000000u))
#endif
      {
        external_block_exits[external_block_exit_position].branch_target =
         branch_target;
        external_block_exits[external_block_exit_position].branch_source =
         block_exits[i].branch_source;
        external_block_exit_position++;
      }
      else
      {
        /* Not linked: the branch looks its target up by PC, as BX does.
         * Its placeholder (B +0) would run the next host instructions
         * instead, so Partial SMC froze or crashed games whose IWRAM code
         * branches between its blocks (Pokemon Emerald at boot). */
        generate_branch_patch_unconditional(block_exits[i].branch_source,
         translation_ptr);
        generate_update_pc(branch_target);
        generate_indirect_branch_no_cycle_update(arm);
      }
    }
  }

  generate_block_tails();

  if (ram_region)
    ram_translation_ptr = translation_ptr;
  else
    rom_translation_ptr = translation_ptr;

  for(i = 0; i < external_block_exit_position; i++)
  {
    branch_target = external_block_exits[i].branch_target;
    if(branch_target == 0x00000008)
      translation_target = bios_swi_entrypoint;
    else
      translation_target = block_lookup_translate_arm(branch_target);
#if defined(NSPIRE_LIBRETRO)
    if (GPSP_TRANSLATION_TARGET_INVALID(translation_target))
      return false;
#else
    if (!translation_target)
      return false;
#endif
    generate_branch_patch_unconditional(
      external_block_exits[i].branch_source, translation_target);
  }

  RAM_KEEP_RECORD(0);
  RAM_TRACE_DONE(0);
  return true;
}

bool translate_block_thumb(u32 pc, bool ram_region)
{
  u32 opcode = 0;
  u32 last_opcode;
  u32 condition;
  u32 pc_region = (pc >> 15);
  u32 new_pc_region;
  u8 *pc_address_block = memory_map_read[pc_region];
  u32 block_start_pc = pc;
  u32 block_end_pc = pc;
  u32 block_exit_position = 0;
  s32 block_data_position = 0;
  u32 external_block_exit_position = 0;
  u32 branch_target;
  u32 cycle_count = 0;
  u8 *translation_target;
  u8 *backpatch_address = NULL;
  u8 *translation_ptr = NULL;
  u8 *translation_cache_limit = NULL;
  s32 i;
  u32 flag_status;
  u32 block_end_exits = 0;
  block_exit_type external_block_exits[MAX_EXITS];
  generate_block_extra_vars_thumb();
  thumb_fix_pc();

  if(!pc_address_block)
    pc_address_block = load_gamepak_page(pc_region & 0x3FF);

  mem_pc_settle();
  RAM_KEEP_START(1);
  RAM_KEEP_TRY(1);
  if (ram_region) {
    translation_ptr = ram_translation_ptr;
    translation_cache_limit = &ram_translation_cache[
       RAM_TRANSLATION_CACHE_SIZE - TRANSLATION_CACHE_LIMIT_THRESHOLD
       - (0x10000 - ram_block_tag) / 2 * sizeof(ramtag_type)];
  } else {
    translation_ptr = rom_translation_ptr;
    translation_cache_limit = &rom_translation_cache[
       ROM_TRANSLATION_CACHE_SIZE - TRANSLATION_CACHE_LIMIT_THRESHOLD];
  }

  generate_block_prologue();
  generate_block_tails_start();

  /* This is a function because it's used a lot more than it might seem (all
     of the data processing functions can access it), and its expansion was
     massacreing the compiler. */

  if(ram_region)
  {
    scan_block(thumb, yes);
  }
  else
  {
    scan_block(thumb, no);
  }

  for(i = 0; i < block_exit_position; i++)
  {
    branch_target = block_exits[i].branch_target;

    if((branch_target > block_start_pc) &&
     (branch_target < block_end_pc))
    {
      block_data[(branch_target - block_start_pc) /
       thumb_instruction_width].update_cycles = 1;
    }
  }
  RAM_KEEP_SCANNED(thumb_instruction_width);
  RAM_TRACE_SCANNED(thumb_instruction_width);

  thumb_dead_flag_eliminate();

  block_exit_position = 0;
  block_data_position = 0;

  while(pc != block_end_pc)
  {
    block_data[block_data_position].block_offset = translation_ptr;
    thumb_base_cycles();

    if (pc == cheat_master_hook)
    {
      thumb_process_cheats();
    }

    update_pc_limits();
    translate_thumb_instruction();
    block_data_position++;

    /* If it went too far the cache needs to be flushed and the process
       restarted. Because we might already be nested several stages in
       a simple recursive call here won't work, it has to pedal out to
       the beginning. */

    RAM_KEEP_FULL_CHECK();
    if(translation_ptr + block_tails_bytes > translation_cache_limit ||
     mem_pc_full(ram_region, translation_ptr)) {
      if (ram_region)
        flush_translation_cache_ram();
      else
        flush_translation_cache_rom();
      return false;
    }

    /* If the next instruction is a block entry point update the
       cycle counter and update */
    if (pc != block_end_pc &&
        block_data[block_data_position].update_cycles)
    {
      generate_cycle_update();
    }
  }

  /* The translation gate, for a block that can run off its end; none after
     one whose last instruction always leaves (core-13,
     thumb_block_end_exits). */
  if(!block_end_exits)
  {
    generate_translation_gate(thumb);
  }
  else
  {
    generate_translation_gate_skipped();
  }

  for(i = 0; i < block_exit_position; i++)
  {
    branch_target = block_exits[i].branch_target;

    if((branch_target >= block_start_pc) && (branch_target < block_end_pc))
    {
      /* Internal branch, patch to recorded address */
      translation_target =
       block_data[(branch_target - block_start_pc) /
        thumb_instruction_width].block_offset;

      generate_branch_patch_unconditional(block_exits[i].branch_source,
       translation_target);
    }
    else
    {
#if defined(NSPIRE_LIBRETRO)
      if ((nspire_dynarec_ram_policy
            && GPSP_PC_RELATIVE_TRANS_REGION(branch_target))
          || (!nspire_dynarec_ram_policy
              && (branch_target < 0x00004000u
                  || (branch_target >= 0x08000000u
                      && branch_target < 0x0E000000u))))
#else
      if (branch_target < 0x00004000u
          || (branch_target >= 0x08000000u && branch_target < 0x0E000000u))
#endif
      {
        external_block_exits[external_block_exit_position].branch_target =
         branch_target;
        external_block_exits[external_block_exit_position].branch_source =
         block_exits[i].branch_source;
        external_block_exit_position++;
      }
      else
      {
        /* Not linked: the branch looks its target up by PC, as BX does.
         * Its placeholder (B +0) would run the next host instructions
         * instead, so Partial SMC froze or crashed games whose IWRAM code
         * branches between its blocks (Pokemon Emerald at boot). */
        generate_branch_patch_unconditional(block_exits[i].branch_source,
         translation_ptr);
        generate_update_pc(branch_target);
        generate_indirect_branch_no_cycle_update(thumb);
      }
    }
  }

  generate_block_tails();

  if (ram_region)
    ram_translation_ptr = translation_ptr;
  else
    rom_translation_ptr = translation_ptr;

  for(i = 0; i < external_block_exit_position; i++)
  {
    branch_target = external_block_exits[i].branch_target;
    if(branch_target == 0x00000008)
      translation_target = bios_swi_entrypoint;
    else
      translation_target = block_lookup_translate_thumb(branch_target);
#if defined(NSPIRE_LIBRETRO)
    if (GPSP_TRANSLATION_TARGET_INVALID(translation_target))
      return false;
#else
    if (!translation_target)
      return false;
#endif
    generate_branch_patch_unconditional(
      external_block_exits[i].branch_source, translation_target);
  }

  RAM_KEEP_RECORD(1);
  RAM_TRACE_DONE(1);
  return true;
}

void init_bios_hooks(void)
{
  // Pre-generate this entry point so that we can safely invoke fast
  // SWI calls from ROM and RAM regardless of cache flushes.
  rom_translation_ptr = &rom_translation_cache[rom_cache_watermark];
  last_rom_translation_ptr = rom_translation_ptr;
  mem_pc_bios_start();
  bios_swi_entrypoint = block_lookup_address_arm(0x8);
  rom_cache_watermark = (u32)(rom_translation_ptr - rom_translation_cache);
  mem_pc_bios_end();
}

/* The GBA's part of a RAM flush: every tag cleared (as little memory as
   the code's bounds allow), the bounds and the tags' numbering reset. */
static void flush_ram_tags(void)
{
  // Proceed to clean the SMC area if needed
  // (also try to memset as little as possible for performance)
  if (iwram_code_max) {
    if(iwram_code_max > iwram_code_min) {
      iwram_code_min &= ~15U;
      iwram_code_max = MIN(iwram_code_max + 8, 0x8000);
      memset(&iwram[iwram_code_min], 0, iwram_code_max - iwram_code_min);
    } else
      memset(iwram, 0, 0x8000);
  }

  if (ewram_code_max) {
    if(ewram_code_max > ewram_code_min) {
      ewram_code_min &= ~15U;
      ewram_code_max = MIN(ewram_code_max + 8, 0x40000);
      memset(&ewram[0x40000 + ewram_code_min], 0, ewram_code_max - ewram_code_min);
    } else
      memset(&ewram[0x40000], 0, 0x40000);
  }

  iwram_code_min = ~0U;
  iwram_code_max =  0U;
  ewram_code_min = ~0U;
  ewram_code_max =  0U;
  ram_block_tag = INITIAL_TOP_TAG;
}

void flush_translation_cache_ram(void)
{
#ifdef NSPIRE_LIBRETRO
  u8 *ram_jit_hi = ram_translation_ptr;
#endif
  /* Flushes RAM caches avoiding doing too much work (ie. wiping unused memory) */
  flush_ram_count++;
  /*printf("ram flush %d (pc %x), %x to %x, %x to %x\n",
   flush_ram_count, reg[REG_PC], iwram_code_min, iwram_code_max,
   ewram_code_min, ewram_code_max);*/

  last_ram_translation_ptr = ram_translation_cache + NSPIRE_RAM_CACHE_START;
  ram_translation_ptr = ram_translation_cache + NSPIRE_RAM_CACHE_START;
  mem_pc_flush_ram();

  flush_ram_tags();
#ifdef NSPIRE_KEEP_TRACE
  ram_trace_event(0xF1000000);
#endif
#ifdef NSPIRE_LIBRETRO
  /* Legacy gpSP invalidated the JIT buffer; I-cache may retain stale code. */
  if (ram_jit_hi > ram_translation_cache)
    nspire_jit_cache_sync(ram_translation_cache, ram_jit_hi);
#endif
}

#if defined(NSPIRE_BUILD) && defined(ARM_ARCH)
/* speed10b: the flush of an SMC store (arm_stub.S's stores' path and
   write_epilogue): the whole flush (the kept blocks' store stays: what
   they were made from is checked when one is copied); with Partial SMC the
   first one starts the keeping. */
void nspire_ram_flush_smc(void)
{
  if (RAM_KEEP_ON)
    ram_keep_armed = 1;
  flush_translation_cache_ram();
}

/* RAM_KEEP_START's: the store kept clear of the tag table's next entry
   (each new tag's, written by the lookup before it translates) and of this
   block's first instruction; the counts started. */
static __attribute__((noinline)) void ram_keep_begin(void)
{
#ifdef NSPIRE_KEEP_CHECK
  /* a copy or a flush expected, and the translation ended neither way:
     it failed (a copy was predicted) or flushed (as predicted) */
  if (ram_keep_expect == 1)
  {
    if (nspire_keep_check_bad < 8)
      fprintf(stderr, "kept-block check: %08x predicted to fit, its translation flushed\n",
              (unsigned)ram_keep_made.key);
    nspire_keep_check_bad++;
  }
  else if (ram_keep_expect == 2)
    nspire_keep_check_flush_ok++;
  ram_keep_expect = 0;
#endif
  nspire_reloc_n = 0;
  ram_keep_need = 0;
  ram_keep_need_pcs = 0;
  if (ram_keep_tags_low() - sizeof(ramtag_type) <
       &ram_translation_cache[RAM_KEEP_HI] ||
      ram_translation_ptr + RAM_KEEP_MARGIN > &ram_translation_cache[RAM_KEEP_LO])
    ram_keep_clear();
}

/* After the scan and the internal targets: what the translation will
   leave in block_data[].update_cycles, and whether it read the last
   instruction's as the scan found it. scan_block resets an instruction's
   update_cycles only after its exit tests, so a block whose last
   instruction ends the scan there (an exit, a gate) keeps that entry as an
   earlier translation left it, and the translate loop reads it (the cycle
   update before the last instruction): the code depends on it. */
static __attribute__((noinline)) void ram_keep_scanned(u32 start, u32 end, u32 width,
 s32 exits, s32 scanned)
{
  u32 kn = (end - start) / width, ki, klast = 0;
  for (ki = 0; ki < (u32)exits; ki++)
    if (block_exits[ki].branch_target == end - width && end - width > start)
      klast = 1;
  ram_keep_n = kn;
  ram_keep_dep = scanned == (s32)kn - 1 && !klast;
  for (ki = 0; ki < (kn + 31) / 32; ki++)
    ram_keep_bits_now[ki] = 0;
  for (ki = 0; ki < kn; ki++)
    ram_keep_bits_now[ki >> 5] |= (u32)(block_data[ki].update_cycles & 1) << (ki & 31);
}

/* With translate_block_*'s check after each instruction: the most room
   and PC entries it has needed (it fails when start + need passes the
   limit or the entries the table's size), and the store kept clear of the
   code. */
static __attribute__((noinline)) void ram_keep_full(u8 *ptr, u8 *start, u32 pcs0)
{
  u32 need = (ptr - start) + block_tails_bytes;
  u32 need_pcs = (nspire_pcs.ram_count - pcs0) + nspire_block_tail_bytes / 4;
  if (need > ram_keep_need)
    ram_keep_need = need;
  if (need_pcs > ram_keep_need_pcs)
    ram_keep_need_pcs = need_pcs;
  if (ptr + block_tails_bytes + RAM_KEEP_MARGIN >
       &ram_translation_cache[RAM_KEEP_LO])
    ram_keep_clear();
}

#ifdef NSPIRE_KEEP_CHECK
/* The block just translated against what the copy made there
   (ram_keep_check_copy): the same bytes, PC entries, tags, code bounds,
   scratch left, instructions and scratch read, room needed. */
static void ram_keep_check(u32 key, u8 *code_lo, u32 pcs0)
{
  u32 size = ram_translation_ptr - code_lo, i, pc, start = key & ~1U;
  u32 width = (key & 1) ? 2 : 4;
  const char *why = NULL;

  if (ram_keep_made.key != key || code_lo != ram_keep_made.at)
    why = "key or place";
  else if (ram_keep_made.size != size)
    why = "size";
  else if (memcmp(code_lo, ram_keep_made.code, size))
    why = "code";
  else if (ram_keep_made.pcs != nspire_pcs.ram_count - pcs0)
    why = "PC entries' count";
  else if (ram_keep_made.n != ram_keep_n || ram_keep_made.dep != ram_keep_dep)
    why = "instructions or scratch read";
  else if (ram_keep_made.need != ram_keep_need ||
           ram_keep_made.need_pcs != ram_keep_need_pcs)
    why = "room needed";
  for (i = 0; !why && i < ram_keep_made.pcs; i++)
  {
    const nspire_mem_pc_type *x = &nspire_pcs.ram[pcs0 + i];
    if (x->host - (u32)code_lo != ram_keep_made.pc[i].host ||
        x->pc != ram_keep_made.pc[i].pc)
      why = "PC entries";
  }
  for (pc = start, i = 0; !why && pc < ram_keep_made.end; pc += width, i++)
  {
    u8 *t = pc >= 0x03000000 ? &iwram[pc & 0x7FFF] :
                               &ewram[0x40000 + (pc & 0x3FFFF)];
    if ((width == 4 ? address32(t, 0) : address16(t, 0)) != ram_keep_made.tags[i])
      why = "tags";
  }
  if (!why && (ram_keep_made.bounds[0] != iwram_code_min ||
               ram_keep_made.bounds[1] != iwram_code_max ||
               ram_keep_made.bounds[2] != ewram_code_min ||
               ram_keep_made.bounds[3] != ewram_code_max))
    why = "code bounds";
  for (i = 0; !why && i < 1024; i++)
    if (block_data[i].update_cycles != ram_keep_made.scratch[i])
      why = "scratch left";
  if (!why)
    nspire_keep_check_ok++;
  else
  {
    if (nspire_keep_check_bad < 8)
      fprintf(stderr, "kept-block check: %08x %08x-%08x at +%u: %s differ\n",
              (unsigned)key, (unsigned)start, (unsigned)ram_keep_made.end,
              (unsigned)(code_lo - ram_translation_cache), why);
    nspire_keep_check_bad++;
  }
}
#endif

/* The block made (not copied): checked (KEEP_CHECK), or kept. */
static __attribute__((noinline)) void ram_keep_end(u32 key, u32 end, u8 *code_lo,
 u32 pcs0, u32 ctx0, u32 found)
{
  u32 start = key & ~1U, len = end - start, n = ram_keep_n, size, i, pos;
  u32 code_size = ram_translation_ptr - code_lo, cands = 0, relocs = 0;
  u32 npcs = nspire_pcs.ram_count - pcs0, mirror = start >= 0x03000000 ?
   ~0x7FFFU : ~0x3FFFFU;
  ram_keep_ent *e;
  u32 *bits, *pcs, *code;
  u16 *rel;
  u32 slot = ram_keep_slot(key);
#ifdef NSPIRE_KEEP_CHECK
  if (ram_keep_expect == 1)
    ram_keep_check(key, code_lo, pcs0);
  else if (ram_keep_expect == 2)
  {
    if (nspire_keep_check_bad < 8)
      fprintf(stderr, "kept-block check: %08x predicted to flush, its translation didn't\n",
              (unsigned)key);
    nspire_keep_check_bad++;
    nspire_keep_check_flush_bad++;
  }
  ram_keep_expect = 0;
#endif
  /* one there already; a ROM flush while its exits were linked; a span
     across a mirror's end (the bytes compared wouldn't be the ones read);
     more branches than the log holds */
  if (found || ctx0 != ram_keep_ctx || ((start ^ (end - 1)) & mirror) ||
      nspire_reloc_n > NSPIRE_RELOC_MAX || code_size / 4 > 0xFFFF)
    return;
  /* the store clear of the code and of the tag table's next entry */
  if (ram_translation_ptr + RAM_KEEP_MARGIN > &ram_translation_cache[RAM_KEEP_LO] ||
      ram_keep_tags_low() - sizeof(ramtag_type) < &ram_translation_cache[RAM_KEEP_HI])
    return;
  for (i = 0; i < nspire_reloc_n; i++)
    if (nspire_reloc[i] - (u32)code_lo < code_size)
      cands++;
  size = sizeof(ram_keep_ent) + ((len + 3) & ~3U) + (n + 31) / 32 * 4 +
   npcs * 8 + ((cands + 1) & ~1U) * 2 + code_size;
  if (size > RAM_KEEP_CAP / 4)
    return;
  /* (positions compare modulo 2^32: the store starts over at 2^31) */
  if (ram_keep_w >= 0x80000000u)
  {
    ram_keep_clear();
    ram_keep_w = 0;
    ram_keep_v = 0;
    memset(ram_keep_table, 0, RAM_KEEP_SLOTS * 4);
#if defined(NSPIRE_KEEP_CHECK) || defined(NSPIRE_KEEP_TRACE)
    nspire_keep_w_resets++;
#endif
  }
  pos = ram_keep_w;
  if ((pos & (RAM_KEEP_CAP - 1)) + size > RAM_KEEP_CAP)
    pos += RAM_KEEP_CAP - (pos & (RAM_KEEP_CAP - 1));   /* (the ring's start) */
  e = ram_keep_at(pos);
  e->key = key;
  e->end = end;
  e->code_at = (u32)code_lo;
  e->code_size = code_size;
  e->need = ram_keep_need;
  e->need_pcs = ram_keep_need_pcs;
  e->n = n;
  e->dep = ram_keep_dep;
  e->pcs = npcs;
  memcpy(e + 1, ram_keep_bytes(start), len);
  bits = ram_keep_ent_bits(e);
  for (i = 0; i < (n + 31) / 32; i++)
    bits[i] = ram_keep_bits_now[i];
  pcs = ram_keep_ent_pcs(e);
  for (i = 0; i < npcs; i++)
  {
    pcs[2 * i] = nspire_pcs.ram[pcs0 + i].host - (u32)code_lo;
    pcs[2 * i + 1] = nspire_pcs.ram[pcs0 + i].pc;
  }
  /* the branches out of the block (each place once, rising) */
  e->relocs = 0;
  rel = ram_keep_ent_relocs(e);
  for (i = 0; i < nspire_reloc_n; i++)
  {
    u32 at = nspire_reloc[i], w, target, k, idx;
    if (at - (u32)code_lo >= code_size || (at & 3))
      continue;
    w = *(u32 *)at;
    if (((w >> 25) & 7) != 5)
      continue;
    target = at + 8 + ((s32)(w << 8) >> 6);
    if (target - (u32)code_lo <= code_size)
      continue;   /* (in the block: the same distance anywhere) */
    idx = (at - (u32)code_lo) >> 2;
    for (k = relocs; k && rel[k - 1] > idx; k--)
      ;
    if (k && rel[k - 1] == idx)
      continue;
    memmove(&rel[k + 1], &rel[k], (relocs - k) * 2);
    rel[k] = idx;
    relocs++;
  }
  e->relocs = relocs;
  code = ram_keep_ent_code(e);
  memcpy(code, code_lo, code_size);
  e->size = (u8 *)code + code_size - (u8 *)e;
  e->next = ram_keep_table[slot];
  ram_keep_table[slot] = pos + 1;
  ram_keep_w = pos + e->size;
}
#endif

#if defined(NSPIRE_KEEP_CHECK) || defined(NSPIRE_KEEP_TRACE)
/* tests/arm (perf keepw=N): the store's positions from N on (empty), so
   that a run crosses 2^31 */
void nspire_keep_test_w(u32 w)
{
  ram_keep_w = w;
  ram_keep_v = w;
}
#endif

#ifdef NSPIRE_KEEP_TRACE
/* tests/arm KEEP_TRACE=1: a hash of the events that decide what the RAM
   cache's code is (each block made: its tag and mode, PC, end, bytes and
   the scratch bit read; each flush), kept as made with and without the
   keeping; and as each frame ends (nspire_keep_trace), the cache's code
   and tables. A block made again from the same inputs for the same tag
   and mode (a tag's offset 0) is no new event. */
static u32 ram_trace_hash = 2166136261u, ram_trace_events;
static u32 ram_trace_n, ram_trace_dep, ram_trace_bit;
static u32 ram_trace_map[0x10000];

static u32 ram_trace_mix(u32 h, u32 v)
{
  return (h ^ v) * 16777619u;
}

static void ram_trace_event(u32 v)
{
  ram_trace_hash = ram_trace_mix(ram_trace_hash, v);
  ram_trace_events++;
}

static void ram_trace_scanned(u32 start, u32 end, u32 width, s32 exits, s32 scanned)
{
  u32 kn = (end - start) / width, ki, klast = 0;
  for (ki = 0; ki < (u32)exits; ki++)
    if (block_exits[ki].branch_target == end - width && end - width > start)
      klast = 1;
  ram_trace_n = kn;
  ram_trace_dep = scanned == (s32)kn - 1 && !klast;
  ram_trace_bit = block_data[kn - 1].update_cycles & 1;
}

static void ram_trace_made(u32 key, u32 end, u32 n, u32 dep, u32 bit)
{
  u32 pc = key & ~1U, h = 2166136261u, a, i;
  u16 *tagp = pc >= 0x03000000 ? (u16 *)&iwram[pc & 0x7FFF] :
                                 (u16 *)&ewram[0x40000 + (pc & 0x3FFFF)];
  u8 *bytes = pc >= 0x03000000 ? &iwram[0x8000] : ewram;
  u32 mask = pc >= 0x03000000 ? 0x7FFF : 0x3FFFF;
  h = ram_trace_mix(h, key);
  h = ram_trace_mix(h, end);
  h = ram_trace_mix(h, n);
  h = ram_trace_mix(h, dep ? 2 + bit : 0);
  for (a = pc; a != end; a++)
    h = ram_trace_mix(h, bytes[a & mask]);
  if (!VALID_TAG(*tagp))
  {
    ram_trace_event(h ^ 0xBAD);
    return;
  }
  i = ((INITIAL_TOP_TAG - *tagp) / 2) * 2 + (key & 1);
  if (i < 0x10000 && ram_trace_map[i] == h)
    return;
  if (i < 0x10000)
    ram_trace_map[i] = h;
  ram_trace_event(ram_trace_mix(h, *tagp));
}

static void ram_trace_done(u32 key, u32 end)
{
  ram_trace_made(key, end, ram_trace_n, ram_trace_dep, ram_trace_bit);
}

/* tests/arm perf (KEEP_TRACE=1), as each frame ends */
void nspire_keep_trace(char *text, u32 size)
{
  u32 tags = 2166136261u, code = 2166136261u, i;
  u32 lo = RAM_TRANSLATION_CACHE_SIZE - (0x10000 - ram_block_tag) / 2 *
   sizeof(ramtag_type);
  for (i = 0; i < 0x8000; i += 4)
    tags = ram_trace_mix(tags, *(u32 *)&iwram[i]);
  for (i = 0; i < 0x40000; i += 4)
    tags = ram_trace_mix(tags, *(u32 *)&ewram[0x40000 + i]);
  tags = ram_trace_mix(tags, iwram_code_min);
  tags = ram_trace_mix(tags, iwram_code_max);
  tags = ram_trace_mix(tags, ewram_code_min);
  tags = ram_trace_mix(tags, ewram_code_max);
  tags = ram_trace_mix(tags, ram_block_tag);
  for (i = NSPIRE_RAM_CACHE_START;
       &ram_translation_cache[i] < ram_translation_ptr; i += 4)
    code = ram_trace_mix(code, *(u32 *)&ram_translation_cache[i]);
  code = ram_trace_mix(code, ram_translation_ptr - ram_translation_cache);
  for (i = lo; i < RAM_TRANSLATION_CACHE_SIZE; i += 4)
    code = ram_trace_mix(code, *(u32 *)&ram_translation_cache[i]);
  code = ram_trace_mix(code, nspire_pcs.pending & 1);
  for (i = 0; !(nspire_pcs.pending & 1) && i < nspire_pcs.ram_count; i++)
  {
    code = ram_trace_mix(code, nspire_pcs.ram[i].host - (u32)ram_translation_cache);
    code = ram_trace_mix(code, nspire_pcs.ram[i].pc);
  }
  snprintf(text, size, "keep-trace: blocks %08x (%u), tags %08x, cache %08x, flushes %u",
           (unsigned)ram_trace_hash, (unsigned)ram_trace_events, (unsigned)tags,
           (unsigned)code, (unsigned)nspire_pcs.ram_flushes);
}
#endif

void flush_translation_cache_rom(void)
{
#if defined(NSPIRE_BUILD) && defined(ARM_ARCH)
  /* speed10b: kept RAM blocks link into the ROM blocks */
  ram_keep_ctx++;
#if defined(NSPIRE_KEEP_CHECK) || defined(NSPIRE_KEEP_TRACE)
  if (ram_keep_v != ram_keep_w)
    nspire_keep_rom_clears++;
#endif
  ram_keep_clear();
#endif
#ifdef NSPIRE_LIBRETRO
  u8 *rom_jit_hi = rom_translation_ptr;
#endif
  /* We flush the generated code except for everything below the watermark. */
  last_rom_translation_ptr = &rom_translation_cache[rom_cache_watermark];
  rom_translation_ptr      = &rom_translation_cache[rom_cache_watermark];
  mem_pc_flush_rom();

  memset(rom_branch_hash, 0, sizeof(rom_branch_hash));
  block_cache_clear();
#ifdef NSPIRE_LIBRETRO
  {
    u8 *rom_jit_lo = &rom_translation_cache[rom_cache_watermark];
    if (rom_jit_hi > rom_jit_lo)
      nspire_jit_cache_sync(rom_jit_lo, rom_jit_hi);
  }
#endif
}

void init_dynarec_caches(void)
{
  /* Initialize caches so that we can start initalizing the emitter. */
  rom_translation_ptr = last_rom_translation_ptr = &rom_translation_cache[0];
  memset(rom_branch_hash, 0, sizeof(rom_branch_hash));
  block_cache_clear();
  mem_pc_init();

  ram_translation_ptr = last_ram_translation_ptr = &ram_translation_cache[NSPIRE_RAM_CACHE_START];
#if defined(NSPIRE_BUILD) && defined(ARM_ARCH)
  ram_keep_w = 0;
  ram_keep_v = 0;
  ram_keep_armed = 0;
  memset(ram_keep_table, 0, RAM_KEEP_SLOTS * 4);
#endif
  memset(iwram, 0, 0x8000);
  memset(&ewram[0x40000], 0, 0x40000);

  ewram_code_min = 0;
  ewram_code_max = 0x40000;
  iwram_code_min = 0;
  iwram_code_max = 0x8000;
}

void flush_dynarec_caches(void)
{
  /* Flush ROM and RAM caches. */
  flush_translation_cache_rom();
  ewram_code_min = 0;
  ewram_code_max = 0x40000;
  iwram_code_min = 0;
  iwram_code_max = 0x8000;
  flush_translation_cache_ram();
}

/*
 * Partial dynarec SMC invalidation (TempGBA / gpsp-partial-flush) plus
 * lr-gpsp-amcc patch-8: UB sentinel on the left chain, dynamic translation
 * gates on CPU/store path only (not DMA). See:
 *   https://github.com/andymcca/gpsp-partial-flush/
 *   https://github.com/andymcca/lr-gpsp-amcc/commits/patch-8/
 */
#define SMC_TAG_UB16 0xFFFFu

/* ARM PC of the instruction before the written word; clamped into IW/EW RAM. */
static u32 partial_flush_translation_gate_pc(u32 address)
{
  u32 region = address >> 24;

  if (region == 0x03)
  {
    u32 off = address & 0x7FFF;
    u32 align = off & ~3u;
    if (align < 4)
      return 0x03000000u;
    return 0x03000000u + align - 4u;
  }
  if (region == 0x02)
  {
    u32 off = address & 0x3FFFFu;
    u32 align = off & ~3u;
    if (align < 4)
      return 0x02000000u;
    return 0x02000000u + align - 4u;
  }
  return (address & ~3u) - 4u;
}

static void translation_gate_try_register(u32 address)
{
  u32 gate_pc = partial_flush_translation_gate_pc(address);
  u32 i;

  if ((address >> 24) != 0x02 && (address >> 24) != 0x03)
    return;

  for (i = 0; i < translation_gate_targets; i++)
  {
    if (translation_gate_target_pc[i] == gate_pc)
      return;
  }
  if (translation_gate_targets >= MAX_TRANSLATION_GATES)
    return;
  translation_gate_target_pc[translation_gate_targets++] = gate_pc;
}

static void partial_flush_ram_inner(u32 address, int register_dynamic_gate)
{
  u8 *smc_data;
  u8 *const ewram_smc_data = &ewram[0x40000];
  u8 *const iwram_smc_data = iwram;
  u8 *smc_data_area;
  u8 *smc_data_area_end;
  u8 *smc_data_right;
  u32 region = address >> 24;
  u16 v;

  switch (region)
  {
    case 0x02:
      smc_data = ewram_smc_data + (address & 0x3FFFE);
      smc_data_area = ewram_smc_data;
      smc_data_area_end = ewram_smc_data + 0x40000;
      break;
    case 0x03:
      smc_data = iwram_smc_data + (address & 0x7FFE);
      smc_data_area = iwram_smc_data;
      smc_data_area_end = iwram_smc_data + 0x8000;
      break;
    default:
      return;
  }

  smc_data_right = smc_data;
  *((u16 *)smc_data) = 0;
#ifdef NSPIRE_KEEP_TRACE
  ram_trace_event(0xF2000000 ^ address);
#endif

  if (register_dynamic_gate)
    translation_gate_try_register(address);

  while (1)
  {
    smc_data -= 2;
    if (smc_data < smc_data_area)
      smc_data = smc_data_area_end - 2;
    v = *(u16 *)smc_data;
    if (v == 0 || v == (u16)SMC_TAG_UB16)
      break;
    *(u16 *)smc_data = 0;
  }

  smc_data = smc_data_right;
  while (1)
  {
    smc_data += 2;
    if (smc_data == smc_data_area_end)
      smc_data = smc_data_area;
    v = *(u16 *)smc_data;
    if (v == 0)
      break;
    *(u16 *)smc_data = 0;
  }
}

void partial_flush_ram_full(u32 address)
{
#ifdef NSPIRE_LIBRETRO
  if (nspire_dynarec_ram_policy)
  {
    /* Not flush_translation_cache_ram() alone: after a prior flush,
     * iwram_code_max / ewram_code_max are 0 so SMC mirror memset is skipped
     * and tags stay stale while ram_translation_ptr resets (white screen). */
    flush_dynarec_caches();
    (void)address;
    return;
  }
#endif
  partial_flush_ram_inner(address, 1);
}

void partial_flush_ram_full_dma(u32 address)
{
#ifdef NSPIRE_LIBRETRO
  if (nspire_dynarec_ram_policy)
  {
    flush_dynarec_caches();
    (void)address;
    return;
  }
#endif
  partial_flush_ram_inner(address, 0);
}

