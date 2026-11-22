@ Layout experiments (the model runs the calculator's layout):
@ empty space that moves what the linker places after it, nothing else.
@ tests/layout_moves.sh links it first into a copy of the calculator build
@ (gpsp_libretro.elf) and has the speed model copy that layout:
@ PAD_TEXT bytes of code (the C code that isn't pinned moves: the renderer,
@ the picture output, the memory handlers...; update_gba's stretch, arm_stub.S
@ and the caches keep their 4 KB ways), PAD_BSS bytes of .bss (the data
@ outside arm_stub.S's pinned block moves).
#ifndef PAD_TEXT
#define PAD_TEXT 0
#endif
#ifndef PAD_BSS
#define PAD_BSS 0
#endif
.text
.global layout_pad_text
layout_pad_text:
  .space PAD_TEXT + 4
@ Its own section: first in the calculator's .bss (the first object
@ linked), before LTO's data, which it moves; in the model's own link
@ (tests/arm/Makefile PAD=) the ordering file (mirror_layout.py) puts it
@ after arm_stub.S's 4 KB-aligned block, just before LTO's data.
.section .bss.layout_pad,"aw",%nobits
.global layout_pad_bss
layout_pad_bss:
  .space PAD_BSS + 4
