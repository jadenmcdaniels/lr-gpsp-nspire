@ A small GBA ROM for tests/arm/idle_test.c (core-16, 2026-11-15): one
@ Thumb idle loop, wait (at 0x08000200; idle_test makes its bne at
@ 0x08000204 the idle loop's address), that the translator proves pure
@ (a load and a compare): it waits until the halfword at r1 equals r2.
@ update_gba may run the next events itself only while nothing the loop
@ reads can change, so:
@   +4  waiting for VCOUNT to reach 100 (an I/O load: never repeated), the
@       line right after: 100 (a repeat would never see it)
@   +8  waiting for a VBlank DMA to write a word in EWRAM (plain memory:
@       repeated until the DMA's event), the line right after: 160 (a
@       repeat past the DMA would see it a frame later, at line 0)
@   +0  "DONE" once both ran
@ Results go to EWRAM at 0x0203FF00 (RESULTS).

	.arm
	.section .text
	.global _start
_start:
	b	main
	.space	0xC0 - 4		@ the rest of the header (gpSP doesn't check it)

	.equ	RESULTS, 0x0203FF00
	.equ	FLAG, 0x0203FE00
	.equ	VCOUNT, 0x04000006
	.equ	DMA3SAD, 0x040000D4

main:
	ldr	r0, =main_thumb + 1
	bx	r0
	.pool

	.thumb
	.align	2
main_thumb:
	ldr	r3, =RESULTS
	@ 1: VCOUNT == 100
	ldr	r1, =VCOUNT
	mov	r2, #100
	bl	wait
	ldr	r1, =VCOUNT
	ldrh	r0, [r1]
	str	r0, [r3, #4]
	@ 2: a VBlank DMA (DMA3, one halfword, no repeat) writes FLAG
	ldr	r1, =FLAG
	mov	r0, #0
	strh	r0, [r1]
	ldr	r0, =DMA3SAD
	ldr	r2, =flag_value
	str	r2, [r0]		@ source
	str	r1, [r0, #4]		@ destination
	mov	r2, #1
	strh	r2, [r0, #8]		@ one halfword
	ldr	r2, =0x9000		@ enable, at VBlank, 16-bit
	strh	r2, [r0, #10]
	ldr	r2, =0x5678
	bl	wait
	ldr	r1, =VCOUNT
	ldrh	r0, [r1]
	str	r0, [r3, #8]
	ldr	r0, =0x454E4F44		@ "DONE"
	str	r0, [r3]
done:
	b	done
	.pool

	.org	0x200
	.align	2
wait:					@ until the halfword at r1 is r2
	ldrh	r0, [r1]
	cmp	r0, r2
	bne	wait			@ the idle loop's address: 0x08000204
	bx	lr

	.align	1
flag_value:
	.hword	0x5678
