@ A small GBA ROM for tests/arm/dynarec_test.c: code copied into IWRAM and
@ EWRAM that branches between its own blocks (a call, a loop back to before
@ the block it is in, a jump back), then rewritten by DMA and by the CPU, as
@ games do with code they keep in RAM. gpSP's dynarec has to run all of it as
@ written, with either "Dynarec RAM / SMC" setting.
@
@ sum(n) adds up f(i) = K * i for i = n..1, so sum(10) is 55 * K. K is the
@ immediate of f's first instruction: 3 as copied, then 5 (DMA), then 7
@ (a CPU store). Results go to EWRAM at 0x0203FF00 (RESULTS):
@   +0  "DONE" once everything ran      +4  ARM in IWRAM, K = 3: 165
@   +8  after DMA, K = 5: 275           +12 after a CPU store, K = 7: 385
@   +16 Thumb in IWRAM, K = 3: 165      +20 after DMA, K = 5: 275
@   +24 ARM in EWRAM, K = 3: 165        +28 after DMA, K = 5: 275
@   +32 the step running (1-13), for a run that stops early
@ Block transfers that leave the dynarec's IWRAM fast path for the
@ per-register code (since core-11 at the block's tail, 2026-11-15):
@   +36 an ARM stmia whose last word rewrites translated IWRAM code (f's
@       K = 9): 495                     +40 a Thumb push doing it (K = 7): 385
@   +44 the ARM code with its stack in EWRAM: 495
@   +48 the Thumb code with its stack in EWRAM: 385
@   +52 an ldmia across IWRAM's end: its second word, IWRAM's first (the
@       copied "mov r0, r5", 0xE1A00005); an stmia across it writes them
@       back (IWRAM's first word holds translated code)
@   +56 the ARM code after that: 495
@   +60 a Thumb push {r1, lr} into the BIOS (ignored), then one into EWRAM,
@       an add after each: 2           +64 the EWRAM push's r1: 0x13131313
@       (an ignored store's return must not skip the tail's "b join")
@ A BIOS call from IWRAM code (speed10b's kept RAM blocks: an SWI's exit is
@ linked into the ROM cache, to the BIOS's SWI entry; a copy of the block
@ made before a flush moves that branch):
@   +68 ARM Sqrt(144) in IWRAM: 12      +72 again after a CPU store's
@       flush (the block made again, the same bytes): Sqrt(169): 13
@   +76 Thumb Sqrt(196): 14             +80 again after a flush: 15

	.arm
	.section .text
	.global _start
_start:
	b	main
	.space	0xC0 - 4		@ the rest of the header (gpSP doesn't check it)

	.equ	RESULTS, 0x0203FF00
	.equ	IWRAM_ARM, 0x03000000
	.equ	IWRAM_THUMB, 0x03000400
	.equ	EWRAM_ARM, 0x02010000
	.equ	IWRAM_SWI, 0x03000800
	.equ	IWRAM_SWI_THUMB, 0x03000900

@ ---- Copied to RAM (only relative branches inside) ----

	.align	2
arm_code_start:
arm_tail:				@ jumped back to from arm_sum
	mov	r0, r5
	ldmfd	sp!, {r4, r5, pc}
arm_sum:				@ r0 = n
	stmfd	sp!, {r4, r5, lr}
	mov	r4, r0
	mov	r5, #0
1:	mov	r0, r4
	bl	arm_f			@ another block in RAM
	add	r5, r5, r0		@ a block starts here (f returns to it)...
	subs	r4, r4, #1
	bne	1b			@ ...and loops back to before its start
	b	arm_tail
arm_f:					@ r0 = K * r0
	mov	r1, #3
	mul	r0, r1, r0
	bx	lr
arm_code_end:

	.thumb
	.align	2
thumb_code_start:
thumb_tail:
	mov	r0, r5
	pop	{r4, r5}
	pop	{r1}
	bx	r1
thumb_sum:				@ r0 = n
	push	{r4, r5, lr}
	mov	r4, r0
	mov	r5, #0
1:	mov	r0, r4
	bl	thumb_f
	add	r5, r5, r0
	sub	r4, #1
	bne	1b
	b	thumb_tail
thumb_f:				@ word-aligned (28 bytes in): step 9 rewrites the word
	mov	r1, #3
	mul	r0, r1
	bx	lr
	.align	2
thumb_code_end:

	.arm
	.align	2
arm_swi_start:				@ r0 = Sqrt(r0), the BIOS's
	swi	0x080000
	bx	lr
arm_swi_end:

	.thumb
	.align	2
thumb_swi_start:
	swi	8
	bx	lr
	.align	2
thumb_swi_end:

	.arm
	.align	2
main:
	ldr	sp, =0x03007F00
	ldr	r10, =RESULTS
	mov	r0, #0
	str	r0, [r10]

	@ 1: ARM code in IWRAM, called from the ROM
	mov	r0, #1
	str	r0, [r10, #32]
	ldr	r0, =arm_code_start
	ldr	r1, =IWRAM_ARM
	ldr	r2, =arm_code_end
	bl	copy
	ldr	r8, =IWRAM_ARM + arm_sum - arm_code_start
	mov	r0, #10
	mov	lr, pc
	bx	r8
	str	r0, [r10, #4]

	@ 2: f's K changed by DMA (gpSP's partial flush, or a full one)
	mov	r0, #2
	str	r0, [r10, #32]
	ldr	r0, =arm_mov_r1_5
	ldr	r1, =IWRAM_ARM + arm_f - arm_code_start
	mov	r2, #1
	bl	dma32
	mov	r0, #10
	mov	lr, pc
	bx	r8
	str	r0, [r10, #8]

	@ 3: f's K changed by a CPU store (gpSP flushes RAM code)
	mov	r0, #3
	str	r0, [r10, #32]
	ldr	r0, =IWRAM_ARM + arm_f - arm_code_start
	ldr	r1, =0xE3A01007		@ mov r1, #7
	str	r1, [r0]
	mov	r0, #10
	mov	lr, pc
	bx	r8
	str	r0, [r10, #12]

	@ 4: Thumb code in IWRAM
	mov	r0, #4
	str	r0, [r10, #32]
	ldr	r0, =thumb_code_start
	ldr	r1, =IWRAM_THUMB
	ldr	r2, =thumb_code_end
	bl	copy
	ldr	r8, =IWRAM_THUMB + thumb_sum - thumb_code_start + 1
	mov	r0, #10
	mov	lr, pc
	bx	r8
	str	r0, [r10, #16]

	@ 5: its K changed by a 16-bit DMA
	mov	r0, #5
	str	r0, [r10, #32]
	ldr	r0, =thumb_mov_r1_5
	ldr	r1, =IWRAM_THUMB + thumb_f - thumb_code_start
	mov	r2, #1
	bl	dma16
	mov	r0, #10
	mov	lr, pc
	bx	r8
	str	r0, [r10, #20]

	@ 6: ARM code in EWRAM
	mov	r0, #6
	str	r0, [r10, #32]
	ldr	r0, =arm_code_start
	ldr	r1, =EWRAM_ARM
	ldr	r2, =arm_code_end
	bl	copy
	ldr	r8, =EWRAM_ARM + arm_sum - arm_code_start
	mov	r0, #10
	mov	lr, pc
	bx	r8
	str	r0, [r10, #24]

	@ 7: its K changed by DMA
	mov	r0, #7
	str	r0, [r10, #32]
	ldr	r0, =arm_mov_r1_5
	ldr	r1, =EWRAM_ARM + arm_f - arm_code_start
	mov	r2, #1
	bl	dma32
	mov	r0, #10
	mov	lr, pc
	bx	r8
	str	r0, [r10, #28]

	@ 8: an ARM stmia from the ROM writes f's word before (the same) and
	@ f's first (mov r1, #9): the last word holds translated code
	mov	r0, #8
	str	r0, [r10, #32]
	ldr	r0, =IWRAM_ARM + arm_f - arm_code_start - 4
	ldr	r1, [r0]
	ldr	r2, =0xE3A01009		@ mov r1, #9
	stmia	r0, {r1, r2}
	ldr	r8, =IWRAM_ARM + arm_sum - arm_code_start
	mov	r0, #10
	mov	lr, pc
	bx	r8
	str	r0, [r10, #36]

	@ 9: a Thumb push from the ROM does it to the Thumb f (mov r1, #7)
	mov	r0, #9
	str	r0, [r10, #32]
	ldr	r0, =IWRAM_THUMB + thumb_f - thumb_code_start + 4
	ldr	r1, [r0, #-8]
	ldr	r2, [r0, #-4]
	bic	r2, r2, #0xFF
	orr	r2, r2, #7
	ldr	r12, =thumb_push_at + 1
	mov	lr, pc
	bx	r12
	ldr	r8, =IWRAM_THUMB + thumb_sum - thumb_code_start + 1
	mov	r0, #10
	mov	lr, pc
	bx	r8
	str	r0, [r10, #40]

	@ 10: the ARM code with its stack in EWRAM
	mov	r0, #10
	str	r0, [r10, #32]
	mov	r9, sp
	ldr	sp, =0x0203F000
	ldr	r8, =IWRAM_ARM + arm_sum - arm_code_start
	mov	r0, #10
	mov	lr, pc
	bx	r8
	mov	sp, r9
	str	r0, [r10, #44]

	@ 11: the Thumb code with its stack in EWRAM
	mov	r0, #11
	str	r0, [r10, #32]
	mov	r9, sp
	ldr	sp, =0x0203F000
	ldr	r8, =IWRAM_THUMB + thumb_sum - thumb_code_start + 1
	mov	r0, #10
	mov	lr, pc
	bx	r8
	mov	sp, r9
	str	r0, [r10, #48]

	@ 12: an ldmia across IWRAM's end (its second word is IWRAM's first,
	@ in the mirror), an stmia writing the same words back
	mov	r0, #12
	str	r0, [r10, #32]
	ldr	r0, =0x03007FFC
	ldmia	r0, {r1, r2}
	str	r2, [r10, #52]
	stmia	r0, {r1, r2}
	ldr	r8, =IWRAM_ARM + arm_sum - arm_code_start
	mov	r0, #10
	mov	lr, pc
	bx	r8
	str	r0, [r10, #56]

	@ 13: Thumb push {r1, lr} into the BIOS (stores ignored), then into
	@ EWRAM, in one block (two tails); the add after each has to run
	mov	r0, #13
	str	r0, [r10, #32]
	mov	r5, #0
	ldr	r0, =0x00000100
	ldr	r2, =0x0203E000
	ldr	r1, =0x13131313
	ldr	r12, =thumb_push_ignored + 1
	mov	lr, pc
	bx	r12
	str	r5, [r10, #60]
	ldr	r0, =0x0203E000 - 8
	ldr	r1, [r0]
	str	r1, [r10, #64]

	@ 14: ARM code calling the BIOS (Sqrt), made again after a flush
	mov	r0, #14
	str	r0, [r10, #32]
	ldr	r0, =arm_swi_start
	ldr	r1, =IWRAM_SWI
	ldr	r2, =arm_swi_end
	bl	copy
	ldr	r8, =IWRAM_SWI
	mov	r0, #144
	mov	lr, pc
	bx	r8
	str	r0, [r10, #68]
	bl	smc_flush
	mov	r0, #169
	mov	lr, pc
	bx	r8
	str	r0, [r10, #72]

	@ 15: the same in Thumb
	mov	r0, #15
	str	r0, [r10, #32]
	ldr	r0, =thumb_swi_start
	ldr	r1, =IWRAM_SWI_THUMB
	ldr	r2, =thumb_swi_end
	bl	copy
	ldr	r8, =IWRAM_SWI_THUMB + 1
	mov	r0, #196
	mov	lr, pc
	bx	r8
	str	r0, [r10, #76]
	bl	smc_flush
	mov	r0, #225
	mov	lr, pc
	bx	r8
	str	r0, [r10, #80]

	ldr	r0, =0x454E4F44		@ "DONE"
	str	r0, [r10]
done:
	b	done

@ A CPU store over translated code (f's first word, the same value): with
@ Partial SMC the whole RAM flush.
smc_flush:
	ldr	r0, =IWRAM_ARM + arm_f - arm_code_start
	ldr	r1, [r0]
	str	r1, [r0]
	bx	lr

@ Copies the words from r0 up to r2 to r1.
copy:
	ldr	r3, [r0], #4
	str	r3, [r1], #4
	cmp	r0, r2
	blo	copy
	bx	lr

@ DMA 3 now: r2 words or halfwords from r0 to r1.
dma32:
	orr	r2, r2, #0x84000000
	b	1f
dma16:
	orr	r2, r2, #0x80000000
1:	ldr	r3, =0x040000D4
	str	r0, [r3]
	str	r1, [r3, #4]
	str	r2, [r3, #8]
	bx	lr

@ Thumb: push {r1, r2} with sp = r0 (r1 to r0 - 8, r2 to r0 - 4).
	.thumb
	.align	2
thumb_push_at:
	mov	r3, sp
	mov	sp, r0
	push	{r1, r2}
	mov	sp, r3
	bx	lr

@ Thumb: push {r1, lr} with sp = r0, r5 += 1, the same with sp = r2,
@ r5 += 1 (step 13).
	.align	2
thumb_push_ignored:
	mov	r3, sp
	mov	sp, r0
	push	{r1, lr}
	add	r5, #1
	mov	sp, r2
	push	{r1, lr}
	add	r5, #1
	mov	sp, r3
	bx	lr

	.arm
	.align	2
arm_mov_r1_5:
	mov	r1, #5
thumb_mov_r1_5:
	.hword	0x2105			@ Thumb mov r1, #5
	.hword	0

	.ltorg
