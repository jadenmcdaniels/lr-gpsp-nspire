@ The program's entry when tools/zehn_pack.py packed it (its docstring
@ says how): the absolute addresses Ndless's Zehn loader
@ would fix (its ADD_BASE relocations, 4 bytes each in the file, not
@ compressed: 42 KB) are fixed here instead, from a table the packer puts
@ in the compressed image at the start of .bss (a few KB once compressed).
@ First, the code's B and BL instructions get their offsets back: the packer
@ stores them as the target's word number (xz's ARM filter, B too), which
@ zlib packs better (the same call is then the same word everywhere; with a
@ condition only when 64 words away or more, and a target in the program's
@ first 63 words, which would read as a near one, as base + target:
@ tools/zehn_pack.py far_branch, -2.2 KB packed); and
@ its PC-relative LDRs (ldr rX, [pc, #+-imm]: a literal pool's word, any
@ condition) theirs: the packer stores the 13 bits U:imm12 as the literal's
@ address mod 8 KB (the same literal loaded twice is then the same word;
@ "#-0" is 4096, the one value no other offset gives). -16.8 KB packed
@ (r16).
@
@ Runs before anything else of the program, at the address Ndless loaded
@ it, so it uses no absolute address itself: adr, PC-relative loads and
@ branches only. The packer writes the stub's own offset in the program
@ (zehn_self), the table's (zehn_table) and the filtered stretch of code
@ (zehn_code_start, zehn_code_end: after this stub, which runs before it is
@ undone); all are 0 in the ELF, and then the stub only goes on to crt0's
@ _start (genzehn's own file runs unchanged). The table: for each place
@ in turn, (its offset - the last one's) / 4 as 7-bit groups, low first,
@ bit 7 set on all but the last; 0 ends it. Every place is word aligned
@ (the packer checks).
@
@ nspire_lineio.ld puts this in the gap after crt0 (KEEP: nothing calls
@ it), so nothing else in the program moves.

.arch armv5te
.arm
.section .text.zehn_entry,"ax",%progbits
.align 2
.global gpsp_zehn_entry
.type gpsp_zehn_entry, %function
gpsp_zehn_entry:
	push	{r0-r12, lr}		@ argc, argv and what crt0 saves for Ndless
	bl	gpsp_zehn_relocate	@ the first instruction after the push: tests/arm/zehn_test.c calls it
	cmp	r0, #0
	beq	2f
	@ The branches just rewritten are code: write the data cache back and
	@ empty it, empty the instruction cache, drain the write buffer (the
	@ ARM926's whole-cache flush, as Linux's arm926 code does it; Ndless's
	@ clear_cache before it called us is the same less the drain).
1:	mrc	p15, 0, r15, c7, c14, 3
	bne	1b
	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0
	mcr	p15, 0, r0, c7, c10, 4
2:	pop	{r0-r12, lr}
	b	_start
.size gpsp_zehn_entry, . - gpsp_zehn_entry

@ int gpsp_zehn_relocate(void): undoes the branch and literal filters, applies
@ the table, then zeroes it (it lies in .bss); returns 0 if the program wasn't
@ packed (nothing done), else 1. AAPCS: r4-r7 and lr kept.
.global gpsp_zehn_relocate
.type gpsp_zehn_relocate, %function
gpsp_zehn_relocate:
	ldr	r2, zehn_table
	cmp	r2, #0
	moveq	r0, #0
	bxeq	lr
	push	{r4-r7, lr}
	adr	r3, gpsp_zehn_entry
	ldr	r0, zehn_self
	sub	r3, r3, r0		@ r3 = where the program starts (its offset 0)
	ldr	r0, zehn_code_start
	ldr	r1, zehn_code_end
	add	r0, r0, r3
	add	r1, r1, r3
	mov	lr, #0x0600		@ lr: bit n - 1 set for the patterns' bits 27-24 = n:
	orr	lr, lr, #0x0010		@ 0101 (ldr), 1010 and 1011 (b, bl)
	cmp	r0, r1
	bhs	6f
5:	ldr	r6, [r0], #4		@ r0: the next word's place
	mov	r12, r6, lsr #24
	and	r12, r12, #0x0f
	movs	r12, lr, lsr r12	@ C = the last bit out: lr's bit n - 1 (for n = 0,
	bcs	9f			@ C as the cmp before left it: clear, r0 < r1)
8:	cmp	r0, r1
	blo	5b
	b	6f
9:	tst	r6, #0x08000000
	beq	10f			@ 0101: a load
	cmp	r6, #0xe0000000
	blo	11f			@ b/bl with a condition
	cmp	r6, #0xf0000000
	bhs	8b			@ blx: as it is
	sub	r12, r0, r3		@ b/bl: offset = word number - (its own + 2)
	mov	r12, r12, lsr #2	@ (r0 - r3 is its place + 4: its word number + 1)
	add	r12, r12, #1
	sub	r7, r6, r12
	bic	r7, r7, #0xff000000
	and	r6, r6, #0xff000000
	orr	r6, r6, r7
	str	r6, [r0, #-4]
	b	8b
11:	mov	r4, r6, lsl #8		@ b/bl with a condition: r4 = its 24 bits y, signed
	mov	r4, r4, asr #8
	add	r5, r4, #63
	cmp	r5, #126
	bls	8b			@ y within 63 words: a near one, as it is
	sub	r12, r0, r3
	mov	r12, r12, lsr #2
	add	r12, r12, #1		@ r12 = base: its word number + 2
	sub	r5, r6, r12
	mov	r5, r5, lsl #8
	mov	r5, r5, asr #8		@ d = y - base, 24 bits, signed
	add	r7, r5, #63
	cmp	r7, #126
	movls	r4, r5			@ d within 63: the target is the program's word d
	sub	r4, r4, r12		@ else word y: offset = target - base
	bic	r4, r4, #0xff000000
	and	r6, r6, #0xff000000
	orr	r6, r6, r4
	str	r6, [r0, #-4]
	b	8b
10:	and	r4, r6, #0x007f0000	@ ldr rX, [pc, #+-imm] (any condition):
	cmp	r4, #0x001f0000		@ (w & 0x0f7f0000) == 0x051f0000
	bne	8b
	mov	r4, r6, lsl #20		@ r4 = U:imm12, the literal's address mod 8 KB
	mov	r4, r4, lsr #20
	tst	r6, #0x00800000
	orrne	r4, r4, #0x1000
	sub	r5, r0, r3
	add	r5, r5, #4		@ its place + 8
	sub	r4, r4, r5		@ the offset, as 13 bits:
	mov	r4, r4, lsl #19
	movs	r4, r4, asr #19		@ 0..4095: +imm; -4095..-1: -imm; -4096: #-0
	bic	r6, r6, #0x00800000
	bic	r6, r6, #0x00000ff0
	bic	r6, r6, #0x0000000f
	orrpl	r6, r6, #0x00800000
	rsbmi	r4, r4, #0
	bic	r4, r4, #0x1000
	orr	r6, r6, r4
	str	r6, [r0, #-4]
	b	8b
6:	add	r2, r2, r3		@ r2 = the table
	mov	r4, r2			@ r4 = its start, to zero it after
	mov	r5, r3			@ r5 = the last place fixed
1:	mov	r0, #0			@ r0 = the next step, r1 = its bits' shift
	mov	r1, #0
2:	ldrb	r12, [r2], #1
	and	r6, r12, #0x7f
	orr	r0, r0, r6, lsl r1
	add	r1, r1, #7
	tst	r12, #0x80
	bne	2b
	cmp	r0, #0
	beq	3f
	add	r5, r5, r0, lsl #2
	ldr	r6, [r5]
	add	r6, r6, r3
	str	r6, [r5]
	b	1b
3:	mov	r0, #0			@ zero the table up to its end word
4:	str	r0, [r4], #4
	cmp	r4, r2
	blo	4b
	pop	{r4-r7, lr}
	mov	r0, #1
	bx	lr
	.word	0x4c525a47		@ "GZRL": the packer finds the next four words after it
zehn_self:
	.word	0
zehn_table:
	.word	0
zehn_code_start:
	.word	0
zehn_code_end:
	.word	0
.size gpsp_zehn_relocate, . - gpsp_zehn_relocate
