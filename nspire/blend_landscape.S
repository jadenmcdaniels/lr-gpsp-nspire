// The older CX's scaled/blended 3:2 picture (nspire_blend.c,
// nspire_blend_aspect_landscape) in ARM assembly: its 53 groups of 3 source
// lines -> 4 LCD lines, with or without interframe blending. The same math
// as the C version (l_groups_mixed / l_groups), pixel for pixel (blend_test):
//
//   void nspire_l_groups(u32 *dst_line13, const u16 *src, const u16 *prev_or_null);
//
// dst_line13: the screen buffer's line 13 (320 pixels a line, 5:5:5 BGR),
// word aligned; src, prev: 240x160 frames in the core's 5:6:5, word aligned
// (prev only read; NULL: no interframe blending).
//
// Per 6-pixel column (two 3x3 blocks -> two 4x4): the 3 source words of each
// line (averaged with prev's, rounded up as gpsp_frame_mixing does) are put
// on the stack; each block then reads its pixels as halfwords, unpacks them
// into notaz's spread form (000000gg ggg00000 0bbbbb00 000rrrrr: red 0-4,
// blue 10-14, the top 5 bits of green 21-25) and goes down its source lines
// one at a time, carrying 4 partial sums from a line to the next:
//   line 0: A0 = 3 p00 + 4, A2 = 3 p02 + 4, K1 = 7 p01 + 4 p00, K2 = 7 p01 + 4 p02
//   line 1: Q1 = 7 (2 p11 + p10) + 16, Q2 = 7 (2 p11 + p12) + 16, 5 p10, 5 p12
// so (3 a + 5 b + 4) / 8 and (14 c + 7 h + 7 v + 4 d + 16) / 32 are sums of
// two of them (the same integers as the C code's, so the same results).
// The second block goes up its lines (line 3 first): the 3x3 -> 4x4 math is
// the same upside down, and the pointer ends where the next column starts.
// Each output line's 4 pixels are stored as one STM (one burst: the screen
// buffers aren't cached).
//
// One colour skips the blends: every blend of equal pixels is that pixel
// ((3a + 5a + 4) >> 3 = a and (14a + 7a + 7a + 4a + 16) >> 5 = a in every
// colour field; the corners are copies).
// - A flat column (its 6x3 source pixels one colour, and with interframe
//   blending the previous frame's 6x3 one colour too): its 8x4 LCD pixels
//   are the one mixed pixel, stored as the same 8 two-word STMs.
// - In another column, a flat block (its 3x3 pixels, mixed, one colour):
//   its 4x4 LCD pixels are that pixel, stored as the block's 4 STMs.
// The test that every column or block pays is a corner test, one compare
// of a word of its first line with one of its last line turned by 16 bits
// (a column: its first two pixels against the last line's last two; a
// block: the two pixels of its whole word against the same two a line
// apart, swapped): an edge across or along nearly always fails it. The
// full test (all words equal, one equal to itself turned by 16 bits; a
// block's third pixels) is out of line.
//
// Registers: r0 dst, r1 src, r2 prev, r3 L_MASK, r12 4 L_ONES; r4-r11, lr
// work. The stack holds the 3x3 source words, the column end and the group
// count. Flags: the flat tests and the column loop's compare (after the
// blocks) set them; BLOCK doesn't, and the second block's corner test,
// made before the first block, relies on that.

	.arch	armv5te
	.arm
	.text
	.align	2

#define SRC_LINE  480		/* bytes per source line (GBA_SCREEN_PITCH 240) */
#define DST_LINE  640		/* bytes per LCD line (320 pixels) */
#define GROUPS    53		/* GBA_SCREEN_HEIGHT / 3 */
#define G_BITS    0x03E00000	/* green in the spread form */

#define BUF       0		/* 3 lines x 3 words: the column's source pixels */
#define COL_END   36		/* src at the end of the group's line */
#define GROUP_N   40		/* groups left */
#define FRAME     48

// raw: a 5:6:5 pixel (16 bits, zero extended; clobbered) -> dst, spread.
.macro UNPACK dst, raw
	and	\dst, r3, \raw, lsl #15		// green (bits 6-10) -> 21-25
	orr	\dst, \dst, \raw, lsr #11	// red -> 0-4
	mov	\raw, \raw, lsl #27
	orr	\dst, \dst, \raw, lsr #17	// blue -> 10-14
.endm

// out = the LCD word of spread pixels lo (lower address) and hi, as
// nspire_blend.c's pack_pair: lo ^ (lo >> 16) ^ (hi << 16) ^ ((lo ^ hi) & G)
// (the four parts don't overlap). tmp is clobbered; out may be hi.
.macro PACK out, lo, hi, tmp
	eor	\tmp, \lo, \hi
	and	\tmp, \tmp, #G_BITS
	eor	\tmp, \tmp, \lo, lsr #16
	eor	\tmp, \tmp, \hi, lsl #16
	eor	\out, \tmp, \lo
.endm

// One 3x3 block -> 4x4. f, m, l: the stack offsets of its first, middle and
// last source line (BUF + 12 n); c0-c2: its columns' byte offsets in a line;
// step: add (lines 0, 1, 2, 3) or sub (lines 3, 2, 1, 0). r0 is at the
// first line's 4 pixels; it ends 8 bytes past the last line's.
.macro BLOCK f, m, l, c0, c1, c2, step
	// -- first line: P0 P1 P2 -> output line [P0, l35(P0,P1)] [l35(P2,P1), P2]
	ldrh	r4, [sp, #(\f + \c0)]
	ldrh	r5, [sp, #(\f + \c1)]
	ldrh	r6, [sp, #(\f + \c2)]
	UNPACK	r7, r4			// P0
	UNPACK	r8, r5			// P1
	UNPACK	r9, r6			// P2
	add	r4, r7, r7, lsl #1
	add	r4, r4, r12		// A0 = 3 P0 + 4
	add	r6, r9, r9, lsl #1
	add	r6, r6, r12		// A2 = 3 P2 + 4
	add	r5, r8, r8, lsl #2	// 5 P1
	rsb	r8, r8, r8, lsl #3	// 7 P1
	add	r10, r8, r7, lsl #2	// K1 = 7 P1 + 4 P0
	add	r11, r8, r9, lsl #2	// K2 = 7 P1 + 4 P2
	add	r8, r4, r5
	and	r8, r3, r8, lsr #3	// l35(P0, P1)
	add	r5, r6, r5
	and	r5, r3, r5, lsr #3	// l35(P2, P1)
	PACK	r7, r7, r8, lr
	PACK	r9, r5, r9, lr
	stmia	r0, {r7, r9}
	\step	r0, r0, #DST_LINE
	// -- middle line: M0 M1 M2 (carried: A0 r4, A2 r6, K1 r10, K2 r11)
	ldrh	r5, [sp, #(\m + \c0)]
	ldrh	r7, [sp, #(\m + \c1)]
	ldrh	r8, [sp, #(\m + \c2)]
	UNPACK	r9, r5			// M0
	UNPACK	r5, r7			// M1
	UNPACK	r7, r8			// M2
	add	r8, r9, r9, lsl #2	// B0 = 5 M0
	add	r9, r9, r5, lsl #1
	rsb	r9, r9, r9, lsl #3
	add	r9, r9, r12, lsl #2	// Q1 = 7 (2 M1 + M0) + 16
	add	lr, r7, r7, lsl #2	// B2 = 5 M2
	add	r7, r7, r5, lsl #1
	rsb	r7, r7, r7, lsl #3
	add	r7, r7, r12, lsl #2	// Q2 = 7 (2 M1 + M2) + 16
	add	r4, r4, r8
	and	r4, r3, r4, lsr #3	// l35(P0, M0)
	add	r6, r6, lr
	and	r6, r3, r6, lsr #3	// l35(P2, M2)
	add	r10, r10, r9
	and	r10, r3, r10, lsr #5	// X(M1, P1, M0, P0)
	add	r11, r11, r7
	and	r11, r3, r11, lsr #5	// X(M1, M2, P1, P2)
	PACK	r4, r4, r10, r5
	PACK	r10, r11, r6, r5
	stmia	r0, {r4, r10}
	\step	r0, r0, #DST_LINE
	// -- last line: L0 L1 L2 (carried: B0 r8, B2 lr, Q1 r9, Q2 r7)
	ldrh	r4, [sp, #(\l + \c0)]
	ldrh	r5, [sp, #(\l + \c1)]
	ldrh	r6, [sp, #(\l + \c2)]
	UNPACK	r10, r4			// L0
	UNPACK	r11, r5			// L1
	UNPACK	r4, r6			// L2
	add	r5, r10, r10, lsl #1
	add	r5, r5, r12		// 3 L0 + 4
	add	r6, r4, r4, lsl #1
	add	r6, r6, r12		// 3 L2 + 4
	add	r8, r8, r5
	and	r8, r3, r8, lsr #3	// l35(L0, M0)
	add	lr, lr, r6
	and	lr, r3, lr, lsr #3	// l35(L2, M2)
	add	r9, r9, r10, lsl #2
	add	r9, r9, r11, lsl #3
	sub	r9, r9, r11
	and	r9, r3, r9, lsr #5	// X(M1, L1, M0, L0)
	add	r7, r7, r4, lsl #2
	add	r7, r7, r11, lsl #3
	sub	r7, r7, r11
	and	r7, r3, r7, lsr #5	// X(M1, M2, L1, L2)
	add	r11, r11, r11, lsl #2	// 5 L1
	add	r5, r5, r11
	and	r5, r3, r5, lsr #3	// l35(L0, L1)
	add	r6, r6, r11
	and	r6, r3, r6, lsr #3	// l35(L2, L1)
	PACK	r8, r8, r9, r11
	PACK	r9, r7, lr, r11
	stmia	r0, {r8, r9}
	\step	r0, r0, #DST_LINE
	PACK	r7, r10, r5, r11
	PACK	r8, r6, r4, r11
	stmia	r0!, {r7, r8}
.endm

// average_up(a, b) into a (lr: 0x7BEF7BEF); t clobbered.
.macro MIX a, b, t
	eor	\t, \a, \b
	and	\t, lr, \t, lsr #1
	orr	\a, \a, \b
	sub	\a, \a, \t
.endm

// Both blocks of a column (from the stack), r0 to the next column. p: mix or
// plain (the labels). r4, r6: words 0 and 2 of the column's first or last
// line; w0, w2: those of the other one of the two. A block whose corner
// test holds goes out of line (BLOCKS_FLAT); the second block's test is
// made before the first block (the registers are its to use) and kept by
// the flags.
.macro COLUMN p, w0, w2
	cmp	r4, \w0, ror #16
	beq	.L\p\()_b0
.L\p\()_b0_blend:
	cmp	r6, \w2, ror #16
	BLOCK	(BUF + 0), (BUF + 12), (BUF + 24), 0, 2, 4, add
	beq	.L\p\()_b1
.L\p\()_b1_blend:
	BLOCK	(BUF + 24), (BUF + 12), (BUF + 0), 6, 8, 10, sub
.endm

// w: a source word, the same pixel in both halves -> o1 = o2 = the LCD word
// of that pixel twice: unpacked (as UNPACK: red from the upper half's copy)
// and packed with itself (PACK with lo = hi: lo ^ lo >> 16 ^ lo << 16).
// t1, t2 clobbered.
.macro SPREAD2 w, t1, t2, o1, o2
	and	\t2, r3, \w, lsl #15		// green
	orr	\t2, \t2, \w, lsr #27		// red
	mov	\t1, \w, lsl #27
	orr	\t2, \t2, \t1, lsr #17		// blue
	eor	\o1, \t2, \t2, lsr #16
	eor	\o1, \o1, \t2, lsl #16
	mov	\o2, \o1
.endm

// COLUMN's blocks whose corner test held, from the stack. p, w2: as COLUMN.
// .L<p>_b0: the first block (pixels 0-2 of the 3 lines); r4 is word 0 of
// its first or last line, equal to the other's turned. One colour (r4 its
// own turn, the middle line's word 0 the same, the lines' third pixels
// that pixel): its 4x4 LCD pixels stored as BLOCK's 4 STMs, r0 as BLOCK
// leaves it, then the second block's corner test; else back to the blend.
// r6 and w2 are kept for that test; r5, r7, r9, r10 clobbered.
// .L<p>_b1: the second block (pixels 3-5) the same, upward as BLOCK goes.
.macro BLOCKS_FLAT p, w2
.L\p\()_b0:
	cmp	r4, r4, ror #16
	ldreq	r5, [sp, #(BUF + 12)]
	cmpeq	r4, r5
	bne	.L\p\()_b0_blend
	ldrh	r5, [sp, #(BUF + 4)]		// the lines' third pixels
	ldrh	r7, [sp, #(BUF + 16)]
	ldrh	r9, [sp, #(BUF + 28)]
	cmp	r5, r4, lsr #16
	cmpeq	r7, r4, lsr #16
	cmpeq	r9, r4, lsr #16
	bne	.L\p\()_b0_blend
	SPREAD2	r4, r5, r7, r9, r10
	stmia	r0, {r9, r10}
	add	r0, r0, #DST_LINE
	stmia	r0, {r9, r10}
	add	r0, r0, #DST_LINE
	stmia	r0, {r9, r10}
	add	r0, r0, #DST_LINE
	stmia	r0!, {r9, r10}
	cmp	r6, \w2, ror #16
	bne	.L\p\()_b1_blend
.L\p\()_b1:
	ldr	r4, [sp, #(BUF + 8)]		// the first line's word 2
	ldr	r5, [sp, #(BUF + 20)]
	cmp	r4, r4, ror #16
	cmpeq	r4, r5
	bne	.L\p\()_b1_blend
	ldrh	r5, [sp, #(BUF + 6)]		// the lines' fourth pixels
	ldrh	r7, [sp, #(BUF + 18)]
	ldrh	r9, [sp, #(BUF + 30)]
	cmp	r5, r4, lsr #16
	cmpeq	r7, r4, lsr #16
	cmpeq	r9, r4, lsr #16
	bne	.L\p\()_b1_blend
	SPREAD2	r4, r5, r7, r9, r10
	stmia	r0, {r9, r10}
	sub	r0, r0, #DST_LINE
	stmia	r0, {r9, r10}
	sub	r0, r0, #DST_LINE
	stmia	r0, {r9, r10}
	sub	r0, r0, #DST_LINE
	stmia	r0!, {r9, r10}
	b	.L\p\()_next
.endm

// A flat column: r4 is its source word (the same pixel in both halves, mixed
// already), stored over the column's 8x4 LCD pixels, each line as the
// blocks' two 2-word STMs; r0 to the next column. r5-r8 clobbered.
.macro FLAT
	SPREAD2	r4, r5, r6, r7, r8
	stmia	r0!, {r7, r8}
	stmia	r0, {r7, r8}
	add	r0, r0, #(DST_LINE - 8)
	stmia	r0!, {r7, r8}
	stmia	r0, {r7, r8}
	add	r0, r0, #(DST_LINE - 8)
	stmia	r0!, {r7, r8}
	stmia	r0, {r7, r8}
	add	r0, r0, #(DST_LINE - 8)
	stmia	r0!, {r7, r8}
	stmia	r0!, {r7, r8}
	sub	r0, r0, #(3 * DST_LINE)
.endm

// speed10: nspire_l_groups_n(dst's line 13 + 4 g, src + 3 g lines, prev +
// 3 g lines or NULL, n): groups g to g + n - 1 only (n >= 1), for
// nspire_blend.c's runs of groups that changed. nspire_l_groups is all 53.
	.global	nspire_l_groups
	.type	nspire_l_groups, %function
nspire_l_groups:
	mov	r3, #GROUPS
	.global	nspire_l_groups_n
	.type	nspire_l_groups_n, %function
nspire_l_groups_n:
	stmfd	sp!, {r4-r11, lr}
	sub	sp, sp, #FRAME
	str	r3, [sp, #GROUP_N]
	ldr	r3, .Lmask
	ldr	r12, .Lones4
	cmp	r2, #0
	beq	.Lplain_group

.Lmix_group:
	add	r4, r1, #SRC_LINE
	str	r4, [sp, #COL_END]
.Lmix_column:
	ldr	r7, [r1, #(2 * SRC_LINE + 8)]
	ldmia	r1, {r4, r5, r6}
	cmp	r4, r7, ror #16		// the column's corners: pixels 0, 1 = 5, 4 of the last line?
	beq	.Lmix_flat		// then look at the rest (back here if not flat)
.Lmix_blend:
	ldr	lr, .Lavg
	ldmia	r2, {r7, r8, r9}
	MIX	r4, r7, r10
	MIX	r5, r8, r10
	MIX	r6, r9, r10
	stmia	sp, {r4, r5, r6}
	ldr	r4, [r1, #SRC_LINE]
	ldr	r5, [r1, #(SRC_LINE + 4)]
	ldr	r6, [r1, #(SRC_LINE + 8)]
	ldr	r7, [r2, #SRC_LINE]
	ldr	r8, [r2, #(SRC_LINE + 4)]
	ldr	r9, [r2, #(SRC_LINE + 8)]
	MIX	r4, r7, r10
	MIX	r5, r8, r10
	MIX	r6, r9, r10
	str	r4, [sp, #(BUF + 12)]
	str	r5, [sp, #(BUF + 16)]
	str	r6, [sp, #(BUF + 20)]
	ldr	r4, [r1, #(2 * SRC_LINE)]
	ldr	r5, [r1, #(2 * SRC_LINE + 4)]
	ldr	r6, [r1, #(2 * SRC_LINE + 8)]
	ldr	r7, [r2, #(2 * SRC_LINE)]
	ldr	r8, [r2, #(2 * SRC_LINE + 4)]
	ldr	r9, [r2, #(2 * SRC_LINE + 8)]
	MIX	r4, r7, r10
	MIX	r5, r8, r10
	MIX	r6, r9, r10
	str	r4, [sp, #(BUF + 24)]
	str	r5, [sp, #(BUF + 28)]
	str	r6, [sp, #(BUF + 32)]
	ldr	r7, [sp, #(BUF + 0)]
	ldr	r8, [sp, #(BUF + 8)]
	COLUMN	mix, r7, r8		// r4, r6: the last line's words 0 and 2; r7, r8: the first's
.Lmix_next:
	ldr	r11, [sp, #COL_END]
	add	r1, r1, #12
	add	r2, r2, #12
	cmp	r1, r11
	bne	.Lmix_column
	add	r0, r0, #(3 * DST_LINE)	// r0 went one LCD line, r1 r2 one source line
	add	r1, r1, #(2 * SRC_LINE)
	add	r2, r2, #(2 * SRC_LINE)
	ldr	r4, [sp, #GROUP_N]
	subs	r4, r4, #1
	str	r4, [sp, #GROUP_N]
	bne	.Lmix_group
	b	.Ldone

.Lplain_group:
	add	r4, r1, #SRC_LINE
	str	r4, [sp, #COL_END]
.Lplain_column:
	ldmia	r1, {r4, r5, r6}
	ldr	r7, [r1, #SRC_LINE]
	ldr	r8, [r1, #(SRC_LINE + 4)]
	ldr	r9, [r1, #(SRC_LINE + 8)]
	ldr	lr, [r1, #(2 * SRC_LINE + 8)]
	ldr	r10, [r1, #(2 * SRC_LINE)]
	ldr	r11, [r1, #(2 * SRC_LINE + 4)]
	cmp	r4, lr, ror #16		// the column's corners: pixels 0, 1 = 5, 4 of the last line?
	beq	.Lplain_flat		// then look at the rest (back here if not flat)
.Lplain_blend:
	stmia	sp, {r4-r11, lr}
	COLUMN	plain, r10, lr		// r4, r6: the first line's words 0 and 2; r10, lr: the last's
.Lplain_next:
	ldr	r11, [sp, #COL_END]
	add	r1, r1, #12
	cmp	r1, r11
	bne	.Lplain_column
	add	r0, r0, #(3 * DST_LINE)
	add	r1, r1, #(2 * SRC_LINE)
	ldr	r4, [sp, #GROUP_N]
	subs	r4, r4, #1
	str	r4, [sp, #GROUP_N]
	bne	.Lplain_group

.Ldone:
	add	sp, sp, #FRAME
	ldmfd	sp!, {r4-r11, pc}

// The column's corner test held (the first line's word 0, r4, is the last
// line's word 2 turned; r4-r6 are kept for .Lmix_blend): r4 its own turn
// (so the last line's word 2 is r4 too), the other 7 words r4, then the
// previous frame's 9 words equal and one colour.
.Lmix_flat:
	cmp	r4, r4, ror #16
	cmpeq	r4, r5
	cmpeq	r4, r6
	ldreq	r7, [r1, #SRC_LINE]
	ldreq	r8, [r1, #(SRC_LINE + 4)]
	ldreq	r9, [r1, #(SRC_LINE + 8)]
	cmpeq	r4, r7
	cmpeq	r4, r8
	cmpeq	r4, r9
	ldreq	r7, [r1, #(2 * SRC_LINE)]
	ldreq	r8, [r1, #(2 * SRC_LINE + 4)]
	cmpeq	r4, r7
	cmpeq	r4, r8
	bne	.Lmix_blend
	ldmia	r2, {r7, r8, r9}
	cmp	r7, r7, ror #16
	cmpeq	r7, r8
	cmpeq	r7, r9
	ldreq	r8, [r2, #SRC_LINE]
	ldreq	r9, [r2, #(SRC_LINE + 4)]
	ldreq	r10, [r2, #(SRC_LINE + 8)]
	cmpeq	r7, r8
	cmpeq	r7, r9
	cmpeq	r7, r10
	ldreq	r8, [r2, #(2 * SRC_LINE)]
	ldreq	r9, [r2, #(2 * SRC_LINE + 4)]
	ldreq	r10, [r2, #(2 * SRC_LINE + 8)]
	cmpeq	r7, r8
	cmpeq	r7, r9
	cmpeq	r7, r10
	bne	.Lmix_blend
	ldr	lr, .Lavg
	MIX	r4, r7, r10		// one mix: both halves the mixed pixel
	FLAT
	b	.Lmix_next

// The same without interframe blending: the 9 words are in r4-r11, lr.
.Lplain_flat:
	cmp	r4, r4, ror #16
	cmpeq	r4, r5
	cmpeq	r4, r6
	cmpeq	r4, r7
	cmpeq	r4, r8
	cmpeq	r4, r9
	cmpeq	r4, r10
	cmpeq	r4, r11
	bne	.Lplain_blend
	FLAT
	b	.Lplain_next

// The blocks of a column that isn't flat, when their corner tests held.
	BLOCKS_FLAT	mix, r8
	BLOCKS_FLAT	plain, lr

	.align	2
.Lmask:	.word	0x03E07C1F	// L_MASK
.Lones4:
	.word	0x00801004	// L_ONES << 2
.Lavg:	.word	0x7BEF7BEF
	.size	nspire_l_groups, . - nspire_l_groups
	.size	nspire_l_groups_n, . - nspire_l_groups_n
