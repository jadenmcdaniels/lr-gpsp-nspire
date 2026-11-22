/* The game's code and data at fixed places in the caches' ways (the
   pins below; out-06 for the per-line I/O path). Passed to
   the calculator's link as --section-ordering-file (Makefile): ld puts
   these statements first in Ndless's .text, .data and .bss, before its own
   patterns, so crt0 has to come first here (the program starts at 0).

   .text: each ". = ..." moves to the next address at OFFSET in a 4 KB
   instruction cache way (.text starts at 0, a way); the gaps (zeros) are
   never run and cost nothing in the packed file. First the entry stub
   (nspire/zehn_entry.S, tools/zehn_pack.py) and the per-line I/O code
   (common.h NSPIRE_LINEIO: the wipes and Advance Wars run it every line),
   then every function the speed model runs and the whole hot stretch
   (update_gba ... arm_stub.S's nspire_hot_end), each at the place 2b8d4d6
   (731,447 B, whose speed table is the reference) had it, in its order:
   written by tools/pin_layout.py, checked by tests/calc_checks.py. The
   hot stretch's lines never jump a way (">= . ?"): a function that grew
   pushes the next a few bytes, the stretch stays whole (nspire_sram.c).
   Then the rest of the game's code (LTO's), and after this file the
   frontend's (built without LTO): nothing the frontend does moves the
   game's code (tests/layout_pinned.py).

   .data and .bss: LTO's data (the game's only: the frontend isn't in LTO)
   first, in one block at a fixed offset in a way, then arm_stub.S's tables
   at 2b8d4d6's offset; the frontend's data after. The two offsets (and the
   pad at the start of .bss) were chosen with the speed model: the data's
   order inside LTO's block can't be set (one section a partition, the
   section anchors kept), so the offsets are the knobs. */
.text : {
  *crt0.o(.text)
  KEEP(*(.text.zehn_entry))
  . = ALIGN(. + 0x1000 - 0x268, 0x1000) - 0x1000 + 0x268;
  *(.text.lineio.nspire_video_write_line_regs)
  . = ALIGN(. + 0x1000 - 0xa48, 0x1000) - 0x1000 + 0xa48;
  *(.text.lineio.write_io_register16)
  . = ALIGN(. + 0x1000 - 0xad8, 0x1000) - 0x1000 + 0xad8;
  *(.text.lineio.dma_transfer)
  /* pins: tools/pin_layout.py */
  . = ALIGN(. + 0x1000 - 0x8d0, 0x1000) - 0x1000 + 0x8d0;
  *(.text.read_memory8)
  . = ALIGN(. + 0x1000 - 0xe1c, 0x1000) - 0x1000 + 0xe1c;
  *(.text.read_memory16)
  . = ALIGN(. + 0x1000 - 0x358, 0x1000) - 0x1000 + 0x358;
  *(.text.read_memory32)
  . = ALIGN(. + 0x1000 - 0x9ec, 0x1000) - 0x1000 + 0x9ec;
  *(.text.write_io_register8)
  . = ALIGN(. + 0x1000 - 0xa74, 0x1000) - 0x1000 + 0xa74;
  *(.text.write_io_register32)
  . = ALIGN(. + 0x1000 - 0xb7c, 0x1000) - 0x1000 + 0xb7c;
  *(.text.write_memory32)
  . = ALIGN(. + 0x1000 - 0xcf8, 0x1000) - 0x1000 + 0xcf8;
  *(.text.dma_tf_loop32)
  . = ALIGN(. + 0x1000 - 0x128, 0x1000) - 0x1000 + 0x128;
  *(.text.write_memory8)
  . = ALIGN(. + 0x1000 - 0x52c, 0x1000) - 0x1000 + 0x52c;
  *(.text.write_memory16)
  . = ALIGN(. + 0x1000 - 0xdf8, 0x1000) - 0x1000 + 0xdf8;
  *(.text.dma_tf_loop16)
  . = ALIGN(. + 0x1000 - 0x050, 0x1000) - 0x1000 + 0x050;
  *(.text.enter_vblank)
  . = ALIGN(. + 0x1000 - 0x178, 0x1000) - 0x1000 + 0x178;
  *(.text.trigger_dma)
  . = ALIGN(. + 0x1000 - 0x4a0, 0x1000) - 0x1000 + 0x4a0;
  *(.text.gbc_sound_advance_silent)
  . = ALIGN(. + 0x1000 - 0x74c, 0x1000) - 0x1000 + 0x74c;
  *(.text.sound_flush_pops)
  . = ALIGN(. + 0x1000 - 0x83c, 0x1000) - 0x1000 + 0x83c;
  *(.text.render_gbc_sound)
  . = ALIGN(. + 0x1000 - 0x80c, 0x1000) - 0x1000 + 0x80c;
  *(.text.nspire_idle_body_pure.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xf34, 0x1000) - 0x1000 + 0xf34;
  *(.text.nspire_mem_pc)
  . = ALIGN(. + 0x1000 - 0x09c, 0x1000) - 0x1000 + 0x09c;
  *(.text.nspire_block_tail_emit.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x66c, 0x1000) - 0x1000 + 0x66c;
  *(.text.process_cheats)
  . = ALIGN(. + 0x1000 - 0xa28, 0x1000) - 0x1000 + 0xa28;
  *(.text.translate_block_arm)
  . = ALIGN(. + 0x1000 - 0x3fc, 0x1000) - 0x1000 + 0x3fc;
  *(.text.translate_block_thumb)
  . = ALIGN(. + 0x1000 - 0xd90, 0x1000) - 0x1000 + 0xd90;
  *(.text.flush_translation_cache_ram)
  . = ALIGN(. + 0x1000 - 0x008, 0x1000) - 0x1000 + 0x008;
  *(.text.partial_flush_ram_full_dma)
  . = ALIGN(. + 0x1000 - 0x13c, 0x1000) - 0x1000 + 0x13c;
  *(.text.arm_disect_imm_32bit.part.0)
  . = ALIGN(. + 0x1000 - 0x1fc, 0x1000) - 0x1000 + 0x1fc;
  *(.text.arm_prepare_load_reg_pc.part.0)
  . = ALIGN(. + 0x1000 - 0xf5c, 0x1000) - 0x1000 + 0xf5c;
  *(.text._ZL16render_w_effectsjjPtjPK19layer_render_struct)
  . = ALIGN(. + 0x1000 - 0xabc, 0x1000) - 0x1000 + 0xabc;
  *(.text._Z20render_scanline_textItL8rendtype1ELb1EEvjjjPvPKt)
  . = ALIGN(. + 0x1000 - 0x348, 0x1000) - 0x1000 + 0x348;
  *(.text._Z20render_scanline_textItL8rendtype1ELb0EEvjjjPvPKt)
  . = ALIGN(. + 0x1000 - 0x874, 0x1000) - 0x1000 + 0x874;
  *(.text._Z22render_scanline_affineItL8rendtype1ELb1EEvjjjPvPKt)
  . = ALIGN(. + 0x1000 - 0x91c, 0x1000) - 0x1000 + 0x91c;
  *(.text._Z22render_scanline_affineItL8rendtype1ELb0EEvjjjPvPKt)
  . = ALIGN(. + 0x1000 - 0x9c4, 0x1000) - 0x1000 + 0x9c4;
  *(.text._Z20render_scanline_textItL8rendtype0ELb1EEvjjjPvPKt)
  . = ALIGN(. + 0x1000 - 0x1fc, 0x1000) - 0x1000 + 0x1fc;
  *(.text._Z20render_scanline_textItL8rendtype0ELb0EEvjjjPvPKt)
  . = ALIGN(. + 0x1000 - 0x9d8, 0x1000) - 0x1000 + 0x9d8;
  *(.text._ZL9order_objj.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xe7c, 0x1000) - 0x1000 + 0xe7c;
  *(.text._Z18tile_render_layersIL8rendtype0ELS0_0EtEvjjPT1_j)
  . = ALIGN(. + 0x1000 - 0x300, 0x1000) - 0x1000 + 0x300;
  *(.text._ZL21render_windowobj_passPtjj.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x4ac, 0x1000) - 0x1000 + 0x4ac;
  *(.text._Z18tile_render_layersIL8rendtype1ELS0_1EtEvjjPT1_j)
  . = ALIGN(. + 0x1000 - 0x93c, 0x1000) - 0x1000 + 0x93c;
  *(.text._Z18tile_render_layersIL8rendtype1ELS0_2EjEvjjPT1_j)
  . = ALIGN(. + 0x1000 - 0xc7c, 0x1000) - 0x1000 + 0xc7c;
  *(.text._Z18tile_render_layersIL8rendtype2ELS0_2EjEvjjPT1_j)
  . = ALIGN(. + 0x1000 - 0x5f0, 0x1000) - 0x1000 + 0x5f0;
  *(.text.nspire_present_frame)
  . = ALIGN(. + 0x1000 - 0x4d8, 0x1000) - 0x1000 + 0x4d8;
  *(.text._ZL27render_scanline_conditionaljjPtj.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x66c, 0x1000) - 0x1000 + 0x66c;
  *(.text._Z20render_window_n_passIXadL_ZL21render_windowout_passPtjjEELj1EEvS0_jj.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xb44, 0x1000) - 0x1000 + 0xb44;
  *(.text.update_scanline)
  . = ALIGN(. + 0x1000 - 0x3c4, 0x1000) - 0x1000 + 0x3c4;
  *(.text._Z22render_scanline_affineItL8rendtype0ELb1EEvjjjPvPKt)
  . = ALIGN(. + 0x1000 - 0x468, 0x1000) - 0x1000 + 0x468;
  *(.text._Z22render_scanline_affineItL8rendtype0ELb0EEvjjjPvPKt)
  . = ALIGN(. + 0x1000 - 0x510, 0x1000) - 0x1000 + 0x510;
  *(.text._Z20render_scanline_textIjL8rendtype1ELb1EEvjjjPvPKt)
  . = ALIGN(. + 0x1000 - 0xda4, 0x1000) - 0x1000 + 0xda4;
  *(.text._Z20render_scanline_textIjL8rendtype1ELb0EEvjjjPvPKt)
  . = ALIGN(. + 0x1000 - 0x2a8, 0x1000) - 0x1000 + 0x2a8;
  *(.text._Z22render_scanline_affineIjL8rendtype1ELb1EEvjjjPvPKt)
  . = ALIGN(. + 0x1000 - 0x350, 0x1000) - 0x1000 + 0x350;
  *(.text._Z22render_scanline_affineIjL8rendtype1ELb0EEvjjjPvPKt)
  . = ALIGN(. + 0x1000 - 0x3f8, 0x1000) - 0x1000 + 0x3f8;
  *(.text._Z20render_scanline_textIjL8rendtype2ELb1EEvjjjPvPKt)
  . = ALIGN(. + 0x1000 - 0xf08, 0x1000) - 0x1000 + 0xf08;
  *(.text._Z20render_scanline_textIjL8rendtype2ELb0EEvjjjPvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x76c, 0x1000) - 0x1000 + 0x76c;
  *(.text._Z22render_scanline_affineIjL8rendtype2ELb1EEvjjjPvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x814, 0x1000) - 0x1000 + 0x814;
  *(.text._Z22render_scanline_affineIjL8rendtype2ELb0EEvjjjPvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xd30, 0x1000) - 0x1000 + 0xd30;
  *(.text._Z24render_affine_backgroundItL8rendtype1ELb1ELb0ELb0ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xf18, 0x1000) - 0x1000 + 0xf18;
  *(.text._Z24render_affine_backgroundItL8rendtype1ELb1ELb0ELb0ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x134, 0x1000) - 0x1000 + 0x134;
  *(.text._Z24render_affine_backgroundItL8rendtype1ELb1ELb0ELb1ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x348, 0x1000) - 0x1000 + 0x348;
  *(.text._Z24render_affine_backgroundItL8rendtype1ELb1ELb0ELb1ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x584, 0x1000) - 0x1000 + 0x584;
  *(.text._Z24render_affine_backgroundItL8rendtype1ELb1ELb1ELb0ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x680, 0x1000) - 0x1000 + 0x680;
  *(.text._Z24render_affine_backgroundItL8rendtype1ELb1ELb1ELb0ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x794, 0x1000) - 0x1000 + 0x794;
  *(.text._Z24render_affine_backgroundItL8rendtype1ELb1ELb1ELb1ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x8ac, 0x1000) - 0x1000 + 0x8ac;
  *(.text._Z24render_affine_backgroundItL8rendtype1ELb1ELb1ELb1ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x9dc, 0x1000) - 0x1000 + 0x9dc;
  *(.text._Z24render_affine_backgroundItL8rendtype1ELb0ELb0ELb0ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xadc, 0x1000) - 0x1000 + 0xadc;
  *(.text._Z24render_affine_backgroundItL8rendtype1ELb0ELb0ELb0ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xc14, 0x1000) - 0x1000 + 0xc14;
  *(.text._Z24render_affine_backgroundItL8rendtype1ELb0ELb0ELb1ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xd2c, 0x1000) - 0x1000 + 0xd2c;
  *(.text._Z24render_affine_backgroundItL8rendtype1ELb0ELb0ELb1ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xe94, 0x1000) - 0x1000 + 0xe94;
  *(.text._Z24render_affine_backgroundItL8rendtype1ELb0ELb1ELb0ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xf64, 0x1000) - 0x1000 + 0xf64;
  *(.text._Z24render_affine_backgroundItL8rendtype1ELb0ELb1ELb0ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x05c, 0x1000) - 0x1000 + 0x05c;
  *(.text._Z24render_affine_backgroundItL8rendtype1ELb0ELb1ELb1ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x130, 0x1000) - 0x1000 + 0x130;
  *(.text._Z24render_affine_backgroundItL8rendtype1ELb0ELb1ELb1ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x22c, 0x1000) - 0x1000 + 0x22c;
  *(.text._Z24render_affine_backgroundItL8rendtype0ELb1ELb0ELb0ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x3e8, 0x1000) - 0x1000 + 0x3e8;
  *(.text._Z24render_affine_backgroundItL8rendtype0ELb1ELb0ELb0ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x5d0, 0x1000) - 0x1000 + 0x5d0;
  *(.text._Z24render_affine_backgroundItL8rendtype0ELb1ELb0ELb1ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x7a4, 0x1000) - 0x1000 + 0x7a4;
  *(.text._Z24render_affine_backgroundItL8rendtype0ELb1ELb0ELb1ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x99c, 0x1000) - 0x1000 + 0x99c;
  *(.text._Z24render_affine_backgroundItL8rendtype0ELb1ELb1ELb0ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xa70, 0x1000) - 0x1000 + 0xa70;
  *(.text._Z24render_affine_backgroundItL8rendtype0ELb1ELb1ELb0ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xb54, 0x1000) - 0x1000 + 0xb54;
  *(.text._Z24render_affine_backgroundItL8rendtype0ELb1ELb1ELb1ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xc30, 0x1000) - 0x1000 + 0xc30;
  *(.text._Z24render_affine_backgroundItL8rendtype0ELb1ELb1ELb1ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xd28, 0x1000) - 0x1000 + 0xd28;
  *(.text._Z24render_affine_backgroundItL8rendtype0ELb0ELb0ELb0ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xe10, 0x1000) - 0x1000 + 0xe10;
  *(.text._Z24render_affine_backgroundItL8rendtype0ELb0ELb0ELb0ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xf30, 0x1000) - 0x1000 + 0xf30;
  *(.text._Z24render_affine_backgroundItL8rendtype0ELb0ELb0ELb1ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x02c, 0x1000) - 0x1000 + 0x02c;
  *(.text._Z24render_affine_backgroundItL8rendtype0ELb0ELb0ELb1ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x170, 0x1000) - 0x1000 + 0x170;
  *(.text._Z24render_affine_backgroundItL8rendtype0ELb0ELb1ELb0ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x228, 0x1000) - 0x1000 + 0x228;
  *(.text._Z24render_affine_backgroundItL8rendtype0ELb0ELb1ELb0ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x308, 0x1000) - 0x1000 + 0x308;
  *(.text._Z24render_affine_backgroundItL8rendtype0ELb0ELb1ELb1ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x3c4, 0x1000) - 0x1000 + 0x3c4;
  *(.text._Z24render_affine_backgroundItL8rendtype0ELb0ELb1ELb1ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x4a8, 0x1000) - 0x1000 + 0x4a8;
  *(.text._Z24render_affine_backgroundIjL8rendtype1ELb1ELb0ELb0ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x68c, 0x1000) - 0x1000 + 0x68c;
  *(.text._Z24render_affine_backgroundIjL8rendtype1ELb1ELb0ELb0ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x894, 0x1000) - 0x1000 + 0x894;
  *(.text._Z24render_affine_backgroundIjL8rendtype1ELb1ELb0ELb1ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xaa4, 0x1000) - 0x1000 + 0xaa4;
  *(.text._Z24render_affine_backgroundIjL8rendtype1ELb1ELb0ELb1ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xccc, 0x1000) - 0x1000 + 0xccc;
  *(.text._Z24render_affine_backgroundIjL8rendtype1ELb1ELb1ELb0ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xdc8, 0x1000) - 0x1000 + 0xdc8;
  *(.text._Z24render_affine_backgroundIjL8rendtype1ELb1ELb1ELb0ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xedc, 0x1000) - 0x1000 + 0xedc;
  *(.text._Z24render_affine_backgroundIjL8rendtype1ELb1ELb1ELb1ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0xff4, 0x1000) - 0x1000 + 0xff4;
  *(.text._Z24render_affine_backgroundIjL8rendtype1ELb1ELb1ELb1ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x124, 0x1000) - 0x1000 + 0x124;
  *(.text._Z24render_affine_backgroundIjL8rendtype1ELb0ELb0ELb0ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x224, 0x1000) - 0x1000 + 0x224;
  *(.text._Z24render_affine_backgroundIjL8rendtype1ELb0ELb0ELb0ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x35c, 0x1000) - 0x1000 + 0x35c;
  *(.text._Z24render_affine_backgroundIjL8rendtype1ELb0ELb0ELb1ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x474, 0x1000) - 0x1000 + 0x474;
  *(.text._Z24render_affine_backgroundIjL8rendtype1ELb0ELb0ELb1ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x5dc, 0x1000) - 0x1000 + 0x5dc;
  *(.text._Z24render_affine_backgroundIjL8rendtype1ELb0ELb1ELb0ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x6ac, 0x1000) - 0x1000 + 0x6ac;
  *(.text._Z24render_affine_backgroundIjL8rendtype1ELb0ELb1ELb0ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x7a4, 0x1000) - 0x1000 + 0x7a4;
  *(.text._Z24render_affine_backgroundIjL8rendtype1ELb0ELb1ELb1ELb0EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x878, 0x1000) - 0x1000 + 0x878;
  *(.text._Z24render_affine_backgroundIjL8rendtype1ELb0ELb1ELb1ELb1EEvjjjPKhjS2_PvPKt.lto_priv.0)
  . = ALIGN(. + 0x1000 - 0x974, 0x1000) - 0x1000 + 0x974;
  *(.text._Z24render_affine_backgroundIjL8rendtype2ELb1ELb0ELb0ELb0EEvjjjPKhjS2_PvPKt)
  . = ALIGN(. + 0x1000 - 0xb5c, 0x1000) - 0x1000 + 0xb5c;
  *(.text._Z24render_affine_backgroundIjL8rendtype2ELb1ELb0ELb0ELb1EEvjjjPKhjS2_PvPKt)
  . = ALIGN(. + 0x1000 - 0xd68, 0x1000) - 0x1000 + 0xd68;
  *(.text._Z24render_affine_backgroundIjL8rendtype2ELb1ELb0ELb1ELb0EEvjjjPKhjS2_PvPKt)
  . = ALIGN(. + 0x1000 - 0xf7c, 0x1000) - 0x1000 + 0xf7c;
  *(.text._Z24render_affine_backgroundIjL8rendtype2ELb1ELb0ELb1ELb1EEvjjjPKhjS2_PvPKt)
  . = ALIGN(. + 0x1000 - 0x1a8, 0x1000) - 0x1000 + 0x1a8;
  *(.text._Z24render_affine_backgroundIjL8rendtype2ELb1ELb1ELb0ELb0EEvjjjPKhjS2_PvPKt)
  . = ALIGN(. + 0x1000 - 0x2a8, 0x1000) - 0x1000 + 0x2a8;
  *(.text._Z24render_affine_backgroundIjL8rendtype2ELb1ELb1ELb0ELb1EEvjjjPKhjS2_PvPKt)
  . = ALIGN(. + 0x1000 - 0x3c0, 0x1000) - 0x1000 + 0x3c0;
  *(.text._Z24render_affine_backgroundIjL8rendtype2ELb1ELb1ELb1ELb0EEvjjjPKhjS2_PvPKt)
  . = ALIGN(. + 0x1000 - 0x4dc, 0x1000) - 0x1000 + 0x4dc;
  *(.text._Z24render_affine_backgroundIjL8rendtype2ELb1ELb1ELb1ELb1EEvjjjPKhjS2_PvPKt)
  . = ALIGN(. + 0x1000 - 0x610, 0x1000) - 0x1000 + 0x610;
  *(.text._Z24render_affine_backgroundIjL8rendtype2ELb0ELb0ELb0ELb0EEvjjjPKhjS2_PvPKt)
  . = ALIGN(. + 0x1000 - 0x718, 0x1000) - 0x1000 + 0x718;
  *(.text._Z24render_affine_backgroundIjL8rendtype2ELb0ELb0ELb0ELb1EEvjjjPKhjS2_PvPKt)
  . = ALIGN(. + 0x1000 - 0x858, 0x1000) - 0x1000 + 0x858;
  *(.text._Z24render_affine_backgroundIjL8rendtype2ELb0ELb0ELb1ELb0EEvjjjPKhjS2_PvPKt)
  . = ALIGN(. + 0x1000 - 0x98c, 0x1000) - 0x1000 + 0x98c;
  *(.text._Z24render_affine_backgroundIjL8rendtype2ELb0ELb0ELb1ELb1EEvjjjPKhjS2_PvPKt)
  . = ALIGN(. + 0x1000 - 0xaf8, 0x1000) - 0x1000 + 0xaf8;
  *(.text._Z24render_affine_backgroundIjL8rendtype2ELb0ELb1ELb0ELb0EEvjjjPKhjS2_PvPKt)
  . = ALIGN(. + 0x1000 - 0xbd0, 0x1000) - 0x1000 + 0xbd0;
  *(.text._Z24render_affine_backgroundIjL8rendtype2ELb0ELb1ELb0ELb1EEvjjjPKhjS2_PvPKt)
  . = ALIGN(. + 0x1000 - 0xcd0, 0x1000) - 0x1000 + 0xcd0;
  *(.text._Z24render_affine_backgroundIjL8rendtype2ELb0ELb1ELb1ELb0EEvjjjPKhjS2_PvPKt)
  . = ALIGN(. + 0x1000 - 0xdbc, 0x1000) - 0x1000 + 0xdbc;
  *(.text._Z24render_affine_backgroundIjL8rendtype2ELb0ELb1ELb1ELb1EEvjjjPKhjS2_PvPKt)
  . = ALIGN(. + 0x1000 - 0x710, 0x1000) - 0x1000 + 0x710;
  *(.text._Z13render_objectIjL8rendtype1ELb1ELb0EEvijPT_jjtPKt.constprop.0)
  . = ALIGN(. + 0x1000 - 0xac4, 0x1000) - 0x1000 + 0xac4;
  *(.text._Z13render_objectIjL8rendtype1ELb1ELb1EEvijPT_jjtPKt.constprop.0)
  . = ALIGN(. + 0x1000 - 0xe98, 0x1000) - 0x1000 + 0xe98;
  *(.text._Z13render_objectItL8rendtype1ELb1ELb0EEvijPT_jjtPKt.constprop.0)
  . = ALIGN(. + 0x1000 - 0x254, 0x1000) - 0x1000 + 0x254;
  *(.text._Z13render_objectItL8rendtype1ELb1ELb1EEvijPT_jjtPKt.constprop.0)
  . = ALIGN(. + 0x1000 - 0x64c, 0x1000) - 0x1000 + 0x64c;
  *(.text._Z13render_objectIjL8rendtype1ELb0ELb0EEvijPT_jjtPKt.constprop.0)
  . = ALIGN(. + 0x1000 - 0x9ec, 0x1000) - 0x1000 + 0x9ec;
  *(.text._Z13render_objectIjL8rendtype1ELb0ELb1EEvijPT_jjtPKt.constprop.0)
  . = ALIGN(. + 0x1000 - 0xdf4, 0x1000) - 0x1000 + 0xdf4;
  *(.text._Z13render_objectItL8rendtype1ELb0ELb0EEvijPT_jjtPKt.constprop.0)
  . = ALIGN(. + 0x1000 - 0x17c, 0x1000) - 0x1000 + 0x17c;
  *(.text._Z13render_objectItL8rendtype1ELb0ELb1EEvijPT_jjtPKt.constprop.0)
  . = ALIGN(. + 0x1000 - 0x56c, 0x1000) - 0x1000 + 0x56c;
  *(.text._Z20render_scanline_objsIjL8rendtype2EEvjjjPvPKt.constprop.0)
  . = ALIGN(. + 0x1000 - 0x4a8, 0x1000) - 0x1000 + 0x4a8;
  *(.text._Z20render_scanline_objsIjL8rendtype1EEvjjjPvPKt.constprop.0)
  . = ALIGN(. + 0x1000 - 0x310, 0x1000) - 0x1000 + 0x310;
  *(.text._Z20render_scanline_objsItL8rendtype1EEvjjjPvPKt.constprop.0)
  . = ALIGN(. + 0x1000 - 0xbf0, 0x1000) - 0x1000 + 0xbf0;
  *(.text._Z13render_objectItL8rendtype3ELb0ELb1EEvijPT_jjtPKt.constprop.0)
  . = ALIGN(. + 0x1000 - 0xff4, 0x1000) - 0x1000 + 0xff4;
  *(.text._Z13render_objectItL8rendtype3ELb0ELb0EEvijPT_jjtPKt.constprop.0)
  . = ALIGN(. + 0x1000 - 0x6a8, 0x1000) - 0x1000 + 0x6a8;
  *(.text._Z20render_scanline_objsItL8rendtype3EEvjjjPvPKt.constprop.0)
  . = ALIGN(. + 0x1000 - 0x334, 0x1000) - 0x1000 + 0x334;
  *(.text._Z13render_objectItL8rendtype0ELb0ELb1EEvijPT_jjtPKt.constprop.0)
  . = ALIGN(. + 0x1000 - 0x830, 0x1000) - 0x1000 + 0x830;
  *(.text._Z13render_objectItL8rendtype0ELb0ELb0EEvijPT_jjtPKt.constprop.0)
  . = ALIGN(. + 0x1000 - 0xcc4, 0x1000) - 0x1000 + 0xcc4;
  *(.text._Z13render_objectItL8rendtype0ELb1ELb0EEvijPT_jjtPKt.constprop.0.isra.0)
  . = ALIGN(. + 0x1000 - 0x120, 0x1000) - 0x1000 + 0x120;
  *(.text._Z13render_objectItL8rendtype0ELb1ELb1EEvijPT_jjtPKt.constprop.0.isra.0)
  . = ALIGN(. + 0x1000 - 0x9ac, 0x1000) - 0x1000 + 0x9ac;
  *(.text._Z20render_scanline_objsItL8rendtype0EEvjjjPvPKt.constprop.0)
  . = ALIGN(. + 0x1000 - 0x85c, 0x1000) - 0x1000 + 0x85c;
  *(.text._Z13render_objectItL8rendtype3ELb1ELb0EEvijPT_jjtPKt.constprop.0.isra.0)
  . = ALIGN(. + 0x1000 - 0xc14, 0x1000) - 0x1000 + 0xc14;
  *(.text._Z13render_objectItL8rendtype3ELb1ELb1EEvijPT_jjtPKt.constprop.0.isra.0)
  . = ALIGN(. + 0x1000 - 0xb24, 0x1000) - 0x1000 + 0xb24;
  *(.text.dma_sound_fifo.isra.0)
  . = ALIGN(. + 0x1000 - 0x768, 0x1000) - 0x1000 + 0x768;
  *scanline_fill16.o(.text)
  . = ALIGN(. + 0x1000 - 0x7c8, 0x1000) - 0x1000 + 0x7c8;
  *blend_landscape.o(.text)
  . = ALIGN(. + 0x1000 - 0x580, 0x1000) - 0x1000 + 0x580;
  *blend_portrait.o(.text)
  . = ALIGN(. + 0x1000 - 0xd40, 0x1000) - 0x1000 + 0xd40;
  *libc.a:libc_a-strcmp.o(.text)
  . = ALIGN(. + 0x1000 - 0xf64, 0x1000) - 0x1000 + 0xf64;
  *libgcc.a:_udivsi3.o(.text)
  . = ALIGN(. + 0x1000 - 0x170, 0x1000) - 0x1000 + 0x170;
  *libgcc.a:_divsi3.o(.text)
  . = ALIGN(. + 0x1000 - 0x3b4, 0x1000) - 0x1000 + 0x3b4;
  *libgcc.a:_aeabi_ldivmod.o(.text)
  . = ALIGN(. + 0x1000 - 0x488, 0x1000) - 0x1000 + 0x488;
  *libgcc.a:_aeabi_uldivmod.o(.text)
  . = ALIGN(. + 0x1000 - 0x000, 0x1000) - 0x1000 + 0x000;
  *(.text.update_gba)
  . = . - (. & 0xfff) + 0xd40 >= . ? . - (. & 0xfff) + 0xd40 : .;
  *(.text.sound_timer)
  . = . - (. & 0xfff) + 0x078 >= . ? . - (. & 0xfff) + 0x078 : .;
  /* speed10: Speed 10a's palette and VRAM _watch forms (arm_stub.S's
     .text.hot.gpsp.watch) in the place of the MSR and SPSR return C bodies
     340950b's fast paths made cold (pinned below, after the stretch): the
     stretch stays within 16 KB, four pages, 340950b's fast paths where 10b
     had them and everything after the stretch where 10a had it */
  *arm_stub.o(.text.hot.gpsp.watch)
  . = . - (. & 0xfff) + 0x30c >= . ? . - (. & 0xfff) + 0x30c : .;
  *(.text.block_lookup_translate_thumb)
  . = . - (. & 0xfff) + 0x4d0 >= . ? . - (. & 0xfff) + 0x4d0 : .;
  *(.text.block_lookup_address_thumb)
  . = . - (. & 0xfff) + 0x778 >= . ? . - (. & 0xfff) + 0x778 : .;
  *(.text.block_lookup_translate_arm)
  . = . - (. & 0xfff) + 0x944 >= . ? . - (. & 0xfff) + 0x944 : .;
  *(.text.block_lookup_address_arm)
  . = . - (. & 0xfff) + 0xbf4 >= . ? . - (. & 0xfff) + 0xbf4 : .;
  *(.text.set_cpu_mode)
  . = . - (. & 0xfff) + 0xccc >= . ? . - (. & 0xfff) + 0xccc : .;
  *(.text.check_and_raise_interrupts)
  . = . - (. & 0xfff) + 0xdf4 >= . ? . - (. & 0xfff) + 0xdf4 : .;
  *(.text.nspire_flush_lines)
  . = . - (. & 0xfff) + 0x200 >= . ? . - (. & 0xfff) + 0x200 : .;
  *arm_stub.o(.text.hot.gpsp)
  . = ALIGN(. + 0x1000 - 0xf40, 0x1000) - 0x1000 + 0xf40;
  *libsyscalls.a:stdlib.o(.text.malloc)
  . = ALIGN(. + 0x1000 - 0xf98, 0x1000) - 0x1000 + 0xf98;
  *libsyscalls.a:stdlib.o(.text.free)
  . = ALIGN(. + 0x1000 - 0x238, 0x1000) - 0x1000 + 0x238;
  *libsyscalls.a:stdlib.o(.text._fstat)
  . = ALIGN(. + 0x1000 - 0x30c, 0x1000) - 0x1000 + 0x30c;
  *libsyscalls.a:stdlib.o(.text._write)
  /* speed10: the MSR and SPSR return C bodies, out of the hot stretch (run
     only for an interrupt to raise or FIQ/Abort/Undefined modes since
     340950b's fast paths; their place there is the _watch forms'), in this
     gap */
  . = ALIGN(. + 0x1000 - 0x388, 0x1000) - 0x1000 + 0x388;
  *(.text.execute_spsr_restore_body)
  . = ALIGN(. + 0x1000 - 0x51c, 0x1000) - 0x1000 + 0x51c;
  *(.text.execute_store_cpsr_body)
  . = ALIGN(. + 0x1000 - 0x758, 0x1000) - 0x1000 + 0x758;
  *libsyscalls.a:stdlib.o(.text._isatty)
  . = ALIGN(. + 0x1000 - 0x8ac, 0x1000) - 0x1000 + 0x8ac;
  *libsyscalls.a:stdlib.o(.text._gettimeofday)
  . = ALIGN(. + 0x1000 - 0x8e8, 0x1000) - 0x1000 + 0x8e8;
  *libsyscalls.a:stdlib.o(.text._malloc_r)
  . = ALIGN(. + 0x1000 - 0xba8, 0x1000) - 0x1000 + 0xba8;
  *libc.a:libc_a-strlen-stub.o(.text.strlen)
  . = ALIGN(. + 0x1000 - 0xc04, 0x1000) - 0x1000 + 0xc04;
  *libc.a:libc_a-memset.o(.text.memset)
  . = ALIGN(. + 0x1000 - 0xf7c, 0x1000) - 0x1000 + 0xf7c;
  *libc.a:libc_a-memmove.o(.text.memmove)
  . = ALIGN(. + 0x1000 - 0x938, 0x1000) - 0x1000 + 0x938;
  *libc.a:libc_a-vfiprintf.o(.text._vfiprintf_r)
  . = ALIGN(. + 0x1000 - 0xf60, 0x1000) - 0x1000 + 0xf60;
  *libc.a:libc_a-wsetup.o(.text.__swsetup_r)
  . = ALIGN(. + 0x1000 - 0xbfc, 0x1000) - 0x1000 + 0xbfc;
  *libc.a:libc_a-errno.o(.text.__errno)
  . = ALIGN(. + 0x1000 - 0xc0c, 0x1000) - 0x1000 + 0xc0c;
  *libc.a:libc_a-memcpy-stub.o(.text.memcpy)
  . = ALIGN(. + 0x1000 - 0xde4, 0x1000) - 0x1000 + 0xde4;
  *libc.a:libc_a-svfiprintf.o(.text._svfiprintf_r)
  . = ALIGN(. + 0x1000 - 0x1d4, 0x1000) - 0x1000 + 0x1d4;
  *libc.a:libc_a-writer.o(.text._write_r)
  . = ALIGN(. + 0x1000 - 0x978, 0x1000) - 0x1000 + 0x978;
  *libc.a:libc_a-makebuf.o(.text.__smakebuf_r)
  . = ALIGN(. + 0x1000 - 0xc84, 0x1000) - 0x1000 + 0xc84;
  *libc.a:libc_a-snprintf.o(.text.snprintf)
  . = ALIGN(. + 0x1000 - 0xf30, 0x1000) - 0x1000 + 0xf30;
  *libc.a:libc_a-stdio.o(.text.__swrite)
  . = ALIGN(. + 0x1000 - 0xfbc, 0x1000) - 0x1000 + 0xfbc;
  *libc.a:libc_a-fflush.o(.text.__sflush_r)
  . = ALIGN(. + 0x1000 - 0x1bc, 0x1000) - 0x1000 + 0x1bc;
  *libc.a:libc_a-fflush.o(.text._fflush_r)
  . = ALIGN(. + 0x1000 - 0x280, 0x1000) - 0x1000 + 0x280;
  *libc.a:libc_a-time.o(.text.time)
  . = ALIGN(. + 0x1000 - 0x360, 0x1000) - 0x1000 + 0x360;
  *libc.a:libc_a-sprint_r.o(.text.__sprint_r)
  . = ALIGN(. + 0x1000 - 0x390, 0x1000) - 0x1000 + 0x390;
  *libc.a:libc_a-fstatr.o(.text._fstat_r)
  . = ALIGN(. + 0x1000 - 0x3d0, 0x1000) - 0x1000 + 0x3d0;
  *libc.a:libc_a-memchr-stub.o(.text.memchr)
  . = ALIGN(. + 0x1000 - 0x4f0, 0x1000) - 0x1000 + 0x4f0;
  *libc.a:libc_a-isattyr.o(.text._isatty_r)
  . = ALIGN(. + 0x1000 - 0xa48, 0x1000) - 0x1000 + 0xa48;
  *libc.a:libc_a-gettimeofdayr.o(.text._gettimeofday_r)
  . = ALIGN(. + 0x1000 - 0xa88, 0x1000) - 0x1000 + 0xa88;
  *libc.a:libc_a-fvwrite.o(.text.__sfvwrite_r)
  . = ALIGN(. + 0x1000 - 0x83c, 0x1000) - 0x1000 + 0x83c;
  *libc.a:libc_a-printf.o(.text.printf)
  . = ALIGN(. + 0x1000 - 0xa3c, 0x1000) - 0x1000 + 0xa3c;
  *libgcc.a:_udivmoddi4.o(.text.__udivmoddi4)
  /* end of pins */
  /* speed10 (nspire_reuse.c, nspire_pictures.c: the frontend's, but run
     for every line, every DMA into VRAM or the palette, every picture): at
     the places in a way they had in the build whose speed table is
     speed10a's (any other moved the renderer's misses by up
     to 0.3 ms a frame in a scene) */
  . = ALIGN(. + 0x1000 - 0x0e4, 0x1000) - 0x1000 + 0x0e4;
  *nspire_reuse.o(.text.dma_from)
  . = ALIGN(. + 0x1000 - 0x188, 0x1000) - 0x1000 + 0x188;
  *nspire_reuse.o(.text.obj_lines_changed)
  . = ALIGN(. + 0x1000 - 0x1f4, 0x1000) - 0x1000 + 0x1f4;
  *nspire_reuse.o(.text.new_frame)
  . = ALIGN(. + 0x1000 - 0x300, 0x1000) - 0x1000 + 0x300;
  *nspire_reuse.o(.text.keep_line)
  . = ALIGN(. + 0x1000 - 0x354, 0x1000) - 0x1000 + 0x354;
  *nspire_reuse.o(.text.frame_begin)
  . = ALIGN(. + 0x1000 - 0x3ac, 0x1000) - 0x1000 + 0x3ac;
  *nspire_reuse.o(.text.held_new)
  . = ALIGN(. + 0x1000 - 0x3ec, 0x1000) - 0x1000 + 0x3ec;
  *nspire_reuse.o(.text.snap_equal)
  . = ALIGN(. + 0x1000 - 0x520, 0x1000) - 0x1000 + 0x520;
  *nspire_reuse.o(.text.probe_line)
  . = ALIGN(. + 0x1000 - 0x64c, 0x1000) - 0x1000 + 0x64c;
  *nspire_reuse.o(.text.stamp)
  . = ALIGN(. + 0x1000 - 0x728, 0x1000) - 0x1000 + 0x728;
  *nspire_reuse.o(.text.nspire_video_dma)
  . = ALIGN(. + 0x1000 - 0xa0c, 0x1000) - 0x1000 + 0xa0c;
  *nspire_reuse.o(.text.nspire_line_oam)
  . = ALIGN(. + 0x1000 - 0xc80, 0x1000) - 0x1000 + 0xc80;
  *nspire_reuse.o(.text.nspire_line_reuse)
  . = ALIGN(. + 0x1000 - 0xfe8, 0x1000) - 0x1000 + 0xfe8;
  *nspire_reuse.o(.text.backdrop_only)
  . = ALIGN(. + 0x1000 - 0x000, 0x1000) - 0x1000 + 0x000;
  *nspire_pictures.o(.text.nspire_reuse_picture)
  . = ALIGN(. + 0x1000 - 0x404, 0x1000) - 0x1000 + 0x404;   /* (the game's code where it was) */
  *.ltrans.o(.text .text.*)
}
/* The data's places (Advance Wars
   moved up to 19% with them): LTO's .data block at 0x0f0 in a 4 KB way,
   arm_stub.S's tables at 0xed0 (2b8d4d6's), LTO's .bss 0x3c0 after a way's
   start (arm_stub.S's .bss, the GBA's memory, starts a way of its own).
   The best of those tried with the speed model. In an output section "."
   counts from its start, hence ALIGN(0x1000) then "+=". */
.data : {
  . = ALIGN(0x1000);
  . += 0x0f0;
  *.ltrans.o(.rodata .rodata.* .data .data.*)
  . = ALIGN(0x1000);
  . += 0xed0;
  *arm_stub.o(.rodata .data)
}
.bss : {
  . += 0x3c0;
  *.ltrans.o(.bss .bss.*)
  *arm_stub.o(.bss)
}
