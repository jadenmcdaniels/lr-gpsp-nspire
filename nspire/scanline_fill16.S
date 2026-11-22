@ Fast u16 scanline fill for ARM926EJ-S (32-bit stmia stores pairs of identical RGB565).
@ void video_fill_u16_strip_arm(u16 *dst, u32 nh, u32 value);
@ r0 = dst, r1 = halfword count (0 allowed), r2 = u16 in low 16 bits
@ dst only has to be halfword aligned: video.cc fills from window edges,
@ which can be odd pixels. stmia needs a word address (the calculator
@ checks alignment and crashes otherwise), so an odd pixel is stored first.

.arch armv5te
.arm
.align 2
.global video_fill_u16_strip_arm
.type video_fill_u16_strip_arm, %function

video_fill_u16_strip_arm:
	cmp	r1, #0
	moveq	pc, lr

	stmdb	sp!, {r4, r5, r6, lr}

	ldr	r12, .Lffff
	and	r12, r2, r12
	orr	r2, r12, r12, lsl #16
	tst	r0, #2			@ odd pixel: store it alone
	strneh	r2, [r0], #2
	subne	r1, r1, #1
	mov	r3, r2
	mov	r4, r2
	mov	r5, r2

1:	cmp	r1, #8
	blo	2f
	stmia	r0!, {r2, r3, r4, r5}
	sub	r1, r1, #8
	b	1b

2:	movs	r6, r1
	beq	9f
3:	strh	r2, [r0], #2
	subs	r6, r6, #1
	bne	3b

9:	ldmia	sp!, {r4, r5, r6, pc}

.align 2
.Lffff:
	.word	0xffff

@ (r19-fx) video.cc merge_blend's alpha pass (OBJ_BLEND and BLEND_ONLY) in ARM:
@ void video_merge_alpha_arm(u16 *dst, const u32 *src, u32 n, u32 ab_low,
@                            const u16 *pal);
@ r0 dst, r1 src (pixel pairs: bits 0-8 the top's palette index, 9 1st
@ target, 11 ST object; 16-24 the pixel below's, 26 2nd target), r2 n > 0,
@ r3 EVA | EVB << 8 | low << 16 (each factor at most 16; low: the top's bits
@ that blend with a 2nd target below, 0x800 ST objects, 0x200 1st targets),
@ [sp] pal (palette_ram_converted, RGB565). A blended pixel is
@ ((top * EVA + below * EVB) >> 4) in 0G0R0B form, each overflowing field
@ set to all ones, as merge_blend; any other pixel the top's colour.
@ Blended 25 instructions, the rest 9 (10 with both bit sets; GCC's
@ merge_blend 29 and 9). The loop's head is in lr: the blend returns there.
.align 2
.global video_merge_alpha_arm
.type video_merge_alpha_arm, %function

video_merge_alpha_arm:
	ldr	r12, [sp]
	stmdb	sp!, {r4, r5, r6, r7, r8, r9, r10, r11, lr}
	and	r4, r3, #0xFF		@ EVA
	mov	r5, r3, lsr #8
	and	r5, r5, #0xFF		@ EVB
	mov	r3, r3, lsr #16		@ the top's blending bits
	ldr	r7, .Lblnd		@ 0G0R0B mask
	ldr	r8, .Lovf4		@ the fields' overflow bits, << 4
	@ r6 (and r3): the bits a blended pair has all of (bics: none left);
	@ ST objects and 1st targets both: two masks, the dual loop
	cmp	r3, #0xA00
	ldreq	r6, .Lmst		@ 0x04000800
	ldreq	r3, .Lm1t		@ 0x04000200
	adreq	lr, 6f
	beq	7f
	orr	r6, r3, #0x04000000
	cmp	r3, #0
	mvneq	r6, #0			@ (no bits: never; pp's bits 12-15 are 0)
	adr	lr, 1f
7:	mov	pc, lr

1:	ldr	r9, [r1], #4
	bics	r11, r6, r9		@ below a 2nd target and the top's bit
	mov	r10, r9, lsl #23
	mov	r10, r10, lsr #22	@ the top's index * 2
	ldrh	r10, [r12, r10]
	beq	4f
	strh	r10, [r0], #2
	subs	r2, r2, #1
	bne	1b
	ldmia	sp!, {r4, r5, r6, r7, r8, r9, r10, r11, pc}

6:	ldr	r9, [r1], #4
	bics	r11, r6, r9		@ an ST object over a 2nd target
	bicnes	r11, r3, r9		@ or a 1st target over one
	mov	r10, r9, lsl #23
	mov	r10, r10, lsr #22
	ldrh	r10, [r12, r10]
	beq	4f
	strh	r10, [r0], #2
	subs	r2, r2, #1
	bne	6b
	ldmia	sp!, {r4, r5, r6, r7, r8, r9, r10, r11, pc}

4:	mov	r11, r9, lsl #7
	mov	r11, r11, lsr #22	@ the pixel below's index * 2
	ldrh	r11, [r12, r11]
	orr	r10, r10, r10, lsl #16
	and	r10, r10, r7
	mul	r9, r10, r4
	orr	r11, r11, r11, lsl #16
	and	r11, r11, r7
	mla	r9, r11, r5, r9
	ands	r11, r9, r8
	and	r10, r7, r9, lsr #4
	beq	2f
	mov	r11, r11, lsr #4	@ overflow bits: G 27, R 16, B 5
	and	r9, r11, #0x08000000
	sub	r11, r11, r9
	sub	r11, r11, r11, lsr #5	@ R, B: bits 11-15, 0-4
	add	r11, r11, r9
	sub	r11, r11, r9, lsr #6	@ G: bits 21-26
	orr	r10, r10, r11
2:	orr	r10, r10, r10, lsr #16
	strh	r10, [r0], #2
	subs	r2, r2, #1
	movne	pc, lr			@ back to the loop it came from
	ldmia	sp!, {r4, r5, r6, r7, r8, r9, r10, r11, pc}

.align 2
.Lmst:
	.word	0x04000800
.Lm1t:
	.word	0x04000200
.Lblnd:
	.word	0x07E0F81F
.Lovf4:
	.word	0x80100200

@ (r19-fx) The same for merge_blend<OBJ_BLEND, true> (no effect but ST
@ objects: Emerald's fog and underwater), with the products looked up:
@ void video_merge_obj_tab_arm(u16 *dst, const u32 *src, u32 n,
@                              const u32 *tab, const u16 *pal);
@ tab[i] = the OBJ palette's entry i in 0G0R0B form times EVA, tab[256 + i]
@ the BG palette's entry i times EVB (video.cc builds it for a palette
@ generation and BLDALPHA). A blended pixel's top is an ST object (index
@ 0x100 | i, bit 0x800) and the pixel below never one (render_object's
@ stacked store keeps a non-object below: index < 0x100). 17 instructions
@ a blended pixel.
.align 2
.global video_merge_obj_tab_arm
.type video_merge_obj_tab_arm, %function

video_merge_obj_tab_arm:
	ldr	r12, [sp]
	stmdb	sp!, {r4, r5, r6, r7, r8, r9, r10, r11, lr}
	add	r4, r3, #1024		@ the BG half
	ldr	r7, .Lblnd
	ldr	r8, .Lovf4
	ldr	r9, [r1], #4		@ (each pair is loaded a pixel ahead: the
					@ last reads src[n], never used)
1:	tst	r9, #0x800		@ top: an ST object
	tstne	r9, #0x04000000		@ below: a 2nd target
	beq	5f
	and	r10, r9, #0xFF
	mov	r11, r9, lsr #16
	ldr	r10, [r3, r10, lsl #2]
	and	r11, r11, #0xFF
	ldr	r11, [r4, r11, lsl #2]
	ldr	r9, [r1], #4
	add	r10, r10, r11
	ands	r11, r10, r8
	and	r10, r7, r10, lsr #4
	bne	4f
2:	orr	r10, r10, r10, lsr #16
3:	strh	r10, [r0], #2
	subs	r2, r2, #1
	bne	1b
	ldmia	sp!, {r4, r5, r6, r7, r8, r9, r10, r11, pc}

4:	mov	r11, r11, lsr #4
	and	r6, r11, #0x08000000
	sub	r5, r11, r6
	sub	r11, r11, r5, lsr #5
	sub	r11, r11, r6, lsr #6
	orr	r10, r10, r11
	b	2b

5:	mov	r10, r9, lsl #23
	ldr	r9, [r1], #4
	mov	r10, r10, lsr #22
	ldrh	r10, [r12, r10]
	b	3b
