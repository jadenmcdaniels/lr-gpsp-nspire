@ tests/arm/block_check_test.c's way into arm_stub.S's nspire_block_check
@ and nspire_block_check_tag entries.
@
@ u32 block_check_call(u32 entry, u32 address, u32 flags, u32 *out)
@   runs the entry as the translated code calls it: reg_base = r11 = reg,
@   r9 = the GBA address, the host flags = flags (bits 31:27), then
@   "bl entry ; b slow". Returns 1 when it came back past the b (fast), 0 at
@   the b (slow); out[0] = r0, out[1] = the flags after, out[2] = r9 after.

  .text
  .arm
  .align 2

  .global block_check_call
block_check_call:
  push {r4-r11, lr}
  ldr r11, =reg
  mov r4, r3
  mov r5, r0
  mov r9, r1
  ldr r0, =0xDEAD0000
  ldr r1, =0xDEAD0001
  msr cpsr_f, r2
  blx r5
  b 1f                        @ slow: the stub returned to the b
  mrs r2, cpsr                @ fast: past it
  mov r3, #1
  b 2f
1:
  mrs r2, cpsr
  mov r3, #0
2:
  str r0, [r4]
  str r2, [r4, #4]
  str r9, [r4, #8]
  mov r0, r3
  pop {r4-r11, pc}

  .ltorg
