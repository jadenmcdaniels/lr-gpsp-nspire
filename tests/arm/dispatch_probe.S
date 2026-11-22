@ tests/arm/dispatch_test.c's way into arm_stub.S's mem_dispatch_* stubs.
@
@ u32 dispatch_call(void (*stub)(void), u32 address)
@   runs the stub as translated code calls it (bl, reg_base = r11 = reg,
@   r1 = the value for a store; since core-17 no PC word after the call)
@   and returns the r2 it read its table entry with: the entry's word index
@   from reg. The test points every table entry at dispatch_probe first,
@   which records r0-r2 and returns as the real handlers do (bx lr).

  .text
  .arm
  .align 2

  .global dispatch_call
dispatch_call:
  push {r4-r11, lr}
  ldr r11, =reg
  mov r3, r0
  mov r0, r1
  ldr r1, =0x5A17C0DE
  mov r2, #0
  blx r3
  pop {r4-r11, pc}

  .global dispatch_probe
dispatch_probe:
  ldr r3, =dispatch_probe_regs
  stmia r3, {r0, r1, r2}
  mov r0, r2
  bx lr

  .ltorg

  .bss
  .align 2
  .global dispatch_probe_regs
dispatch_probe_regs:
  .space 12
