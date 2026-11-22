@ The speed model's .rodata and .data start a 4 KB way (the
@ model runs the calculator's layout): mirror_layout.py's ordering file
@ places each variable at the calculator build's address modulo 4 KB by
@ offsets from the start of its output section, which must be a way's
@ start for that to hold (.text has update_gba's 4 KB alignment, .bss
@ arm_stub.S's).
.section .rodata.layout_anchor,"a"
.balign 4096
.global layout_anchor_rodata
layout_anchor_rodata:
  .word 0
.section .data.layout_anchor,"aw"
.balign 4096
.global layout_anchor_data
layout_anchor_data:
  .word 0
