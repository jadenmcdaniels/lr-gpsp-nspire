// The CX II's scaled/blended 3:2 picture (nspire_blend.c,
// nspire_blend_turn_aspect) in ARM assembly: its 53 groups of 3 source
// lines -> 4 picture lines, with or without interframe blending. The same
// math as the C version (aspect_groups_mixed / aspect_groups), pixel for
// pixel (blend_test):
//
//   void nspire_p_groups(u16 *dst, const u16 *src, const u16 *prev_or_null);
//
// dst: the turned screen buffer (240 pixels an LCD line, 5:6:5; landscape
// pixel (x, y) at position 239 - y of LCD line x), word aligned; src, prev:
// 240x160 frames in the core's 5:6:5, word aligned (prev only read; NULL: no
// interframe blending). Group g's 4 picture lines (landscape 14 + 4g to
// 17 + 4g) are positions 222 - 4g to 225 - 4g of every LCD line: two aligned
// words (byte 444 - 8g), the bottom two pixels in the first.
//
// Per 6-pixel column (two 3x3 blocks -> two 4x4, 8 LCD lines): the 3 source
// words of each line (averaged with prev's, rounded up as gpsp_frame_mixing
// does) are put on the stack. Each block then goes through its 3 source
// columns, one LCD line or two for each, reading a column's 3 pixels as
// halfwords from the bottom up (P0 = line 2, P1 = line 1, P2 = line 0, as
// the LCD line has them), unpacking them (000000gg gggg0000 rrrrr000
// 000bbbbb: green 21-26, red 11-15, blue 0-4) and carrying 4 partial sums
// from a column to the next:
//   column a: A0 = 3 P0 + 4, A2 = 3 P2 + 4, K1 = 7 P1 + 4 P0, K2 = 7 P1 + 4 P2
//   column b: Q1 = 7 (2 M1 + M0) + 16, Q2 = 7 (2 M1 + M2) + 16, 5 M0, 5 M2
// so (3 a + 5 b + 4) / 8 and (14 c + 7 h + 7 v + 4 d + 16) / 32 are sums of
// two of them (the same integers as the C code's, so the same results).
// A corner pixel of the block is the source pixel as it was read. Each LCD
// line's 4 pixels are stored as one STM (one burst: the screen buffers
// aren't cached), at the addresses and in the order the C version stores
// them. The last source line (landscape line 226) and line 13 stay in C
// (nspire_blend.c, aspect_last_line).
//
// A flat column skips the blends (P_FLAT; blend_landscape.S has the same):
// its 6x3 source pixels one colour, and with interframe blending the
// previous frame's 6x3 one colour too. Every blend of equal pixels is that
// pixel ((3a + 5a + 4) >> 3 = a and (14a + 7a + 7a + 4a + 16) >> 5 = a in
// every colour field; the corners are copies), and the LCD word of a pixel
// twice is the source word of that pixel twice (pack_pair(unpack(p),
// unpack(p)) = p | p << 16: this LCD has the core's 5:6:5). So the
// column's 8 LCD lines are its one source word (averaged with prev's: each
// half alone, so both halves stay the same), stored as the same 8 two-word
// STMs at the same addresses in the same order. What every column pays is a
// corner test, one compare of the first line's first two pixels with the
// last line's last two turned by 16 bits (2 instructions; the last line's
// word is loaded there instead of later); the full test (9 words equal, one
// equal to itself turned by 16 bits; then prev's 9) is out of line. The
// blocks of one colour in other columns (blend_landscape.S) are not here.
//
// Registers: r0 dst, r1 src, r2 prev, r3 the unpacked mask, r12 4 x 1 in
// each colour; r4-r11, lr work. The stack holds the 3x3 source words, the
// column end and the group count. Loads and stores are words at word
// addresses or halfwords at even stack offsets: nothing unaligned.

	.arch	armv5te
	.arm
	.text
	.align	2

// 1: the flat column's shortcut; 0 (-DP_FLAT=0): every column blended, the
// code as it was before the shortcut (to measure what it gains).
#ifndef P_FLAT
#define P_FLAT 1
#endif

#define SRC_LINE  480		/* bytes per source line (GBA_SCREEN_PITCH 240) */
#define DST_LINE  480		/* bytes per LCD line (240 pixels) */
#define GROUPS    53		/* GBA_SCREEN_HEIGHT / 3 */
#define FIRST     444		/* group 0's bottom pixel: 2 (239 - (14 + 3)) */
#define COLUMNS   (320 * DST_LINE)	/* a group goes through every LCD line */
#define G_BITS    0x07E00000	/* green, unpacked */

#define BUF       0		/* 3 lines x 3 words: the column's source pixels */
#define COL_END   36		/* src at the end of the group's line */
#define GROUP_N   40		/* groups left */
#define FRAME     48

// raw: a 5:6:5 pixel (16 bits, zero extended; kept) -> dst, unpacked, as
// nspire_blend.c's unpack. dst may be raw.
.macro UNPACK dst, raw
	orr	\dst, \raw, \raw, lsl #16
	and	\dst, \dst, r3
.endm

// out = the LCD word of unpacked pixels lo (lower address) and hi, as
// nspire_blend.c's pack_pair: lo ^ (lo >> 16) ^ (hi << 16) ^ ((lo ^ hi) & G)
// (the four parts don't overlap). tmp is clobbered; out may be lo or hi.
.macro PACK out, lo, hi, tmp
	eor	\tmp, \lo, \hi
	and	\tmp, \tmp, #G_BITS
	eor	\tmp, \tmp, \lo, lsr #16
	eor	\tmp, \tmp, \hi, lsl #16
	eor	\out, \tmp, \lo
.endm

// The same with lo a pixel as read (raw; pack_pair(unpack(raw), hi)): raw
// is the low half, hi's red and blue go up 16 bits, its green stays.
.macro PACK_RAW_LO out, raw, hi, tmp
	and	\tmp, \hi, #G_BITS
	orr	\tmp, \tmp, \hi, lsl #16
	orr	\out, \tmp, \raw
.endm

// The same with hi a pixel as read (pack_pair(lo, unpack(raw))).
.macro PACK_RAW_HI out, lo, raw, tmp
	bic	\tmp, \lo, #G_BITS
	orr	\tmp, \tmp, \lo, lsr #16
	orr	\out, \tmp, \raw, lsl #16
.endm

// One 3x3 block -> 4 LCD lines of 4 pixels. ca, cb, cc: the byte offsets
// of its 3 source columns in a line of the stack's words. r0 is at the
// first LCD line's 4 pixels; it ends at the next block's. No flags changed.
.macro BLOCK ca, cb, cc
	// -- column a: P0 P1 P2 -> LCD line [P0, b35(P0,P1)] [b35(P2,P1), P2]
	ldrh	r4, [sp, #(BUF + 24 + \ca)]
	ldrh	r5, [sp, #(BUF + 12 + \ca)]
	ldrh	r6, [sp, #(BUF + \ca)]
	UNPACK	r7, r4			// P0 (r4: as read)
	UNPACK	r8, r5			// P1
	UNPACK	r9, r6			// P2 (r6: as read)
	add	r5, r8, r8, lsl #2	// 5 P1
	rsb	r8, r8, r8, lsl #3	// 7 P1
	add	r10, r8, r7, lsl #2	// K1 = 7 P1 + 4 P0
	add	r11, r8, r9, lsl #2	// K2 = 7 P1 + 4 P2
	add	r7, r7, r7, lsl #1
	add	r7, r7, r12		// A0 = 3 P0 + 4
	add	r9, r9, r9, lsl #1
	add	r9, r9, r12		// A2 = 3 P2 + 4
	add	r8, r7, r5
	and	r8, r3, r8, lsr #3	// b35(P0, P1)
	add	r5, r9, r5
	and	r5, r3, r5, lsr #3	// b35(P2, P1)
	PACK_RAW_LO r4, r4, r8, lr
	PACK_RAW_HI r6, r5, r6, lr
	stmia	r0, {r4, r6}
	add	r0, r0, #DST_LINE
	// -- column b: M0 M1 M2 (carried: A0 r7, A2 r9, K1 r10, K2 r11)
	//    -> [b35(P0,M0), X(M1,M0,P1,P0)] [X(M1,M2,P1,P2), b35(P2,M2)]
	ldrh	r4, [sp, #(BUF + 24 + \cb)]
	ldrh	r5, [sp, #(BUF + 12 + \cb)]
	ldrh	r6, [sp, #(BUF + \cb)]
	UNPACK	r4, r4			// M0
	UNPACK	r5, r5			// M1
	UNPACK	r6, r6			// M2
	add	r8, r4, r4, lsl #2	// B0 = 5 M0
	add	r4, r4, r5, lsl #1
	rsb	r4, r4, r4, lsl #3
	add	r4, r4, r12, lsl #2	// Q1 = 7 (2 M1 + M0) + 16
	add	lr, r6, r6, lsl #2	// B2 = 5 M2
	add	r6, r6, r5, lsl #1
	rsb	r6, r6, r6, lsl #3
	add	r6, r6, r12, lsl #2	// Q2 = 7 (2 M1 + M2) + 16
	add	r7, r7, r8
	and	r7, r3, r7, lsr #3	// b35(P0, M0)
	add	r9, r9, lr
	and	r9, r3, r9, lsr #3	// b35(P2, M2)
	add	r10, r10, r4
	and	r10, r3, r10, lsr #5	// X(M1, M0, P1, P0)
	add	r11, r11, r6
	and	r11, r3, r11, lsr #5	// X(M1, M2, P1, P2)
	PACK	r7, r7, r10, r5
	PACK	r10, r11, r9, r5
	stmia	r0, {r7, r10}
	add	r0, r0, #DST_LINE
	// -- column c: L0 L1 L2 (carried: B0 r8, B2 lr, Q1 r4, Q2 r6)
	//    -> [b35(L0,M0), X(M1,M0,L1,L0)] [X(M1,M2,L1,L2), b35(L2,M2)]
	//       [L0, b35(L0,L1)] [b35(L2,L1), L2]
	ldrh	r5, [sp, #(BUF + 24 + \cc)]
	ldrh	r7, [sp, #(BUF + 12 + \cc)]
	ldrh	r9, [sp, #(BUF + \cc)]
	UNPACK	r10, r5			// L0 (r5: as read)
	UNPACK	r11, r7			// L1
	UNPACK	r7, r9			// L2 (r9: as read)
	add	r4, r4, r10, lsl #2	// Q1 + 4 L0
	add	r10, r10, r10, lsl #1
	add	r10, r10, r12		// C0 = 3 L0 + 4
	add	r6, r6, r7, lsl #2	// Q2 + 4 L2
	add	r7, r7, r7, lsl #1
	add	r7, r7, r12		// C2 = 3 L2 + 4
	add	r8, r8, r10
	and	r8, r3, r8, lsr #3	// b35(L0, M0)
	add	lr, lr, r7
	and	lr, r3, lr, lsr #3	// b35(L2, M2)
	add	r4, r4, r11, lsl #3
	sub	r4, r4, r11
	and	r4, r3, r4, lsr #5	// X(M1, M0, L1, L0)
	add	r6, r6, r11, lsl #3
	sub	r6, r6, r11
	and	r6, r3, r6, lsr #5	// X(M1, M2, L1, L2)
	add	r11, r11, r11, lsl #2	// 5 L1
	add	r10, r10, r11
	and	r10, r3, r10, lsr #3	// b35(L0, L1)
	add	r7, r7, r11
	and	r7, r3, r7, lsr #3	// b35(L2, L1)
	PACK	r4, r8, r4, r11
	PACK	r6, r6, lr, r11
	stmia	r0, {r4, r6}
	add	r0, r0, #DST_LINE
	PACK_RAW_LO r5, r5, r10, r11
	PACK_RAW_HI r9, r7, r9, r11
	stmia	r0, {r5, r9}
	add	r0, r0, #DST_LINE
.endm

// average_up(a, b) into a (lr: 0x7BEF7BEF); t clobbered.
.macro MIX a, b, t
	eor	\t, \a, \b
	and	\t, lr, \t, lsr #1
	orr	\a, \a, \b
	sub	\a, \a, \t
.endm

// A flat column: r4 = r5 = its source word (the same pixel in both halves,
// mixed already), stored over the column's 8 LCD lines of 4 pixels as the
// blocks' 8 STMs; r0 ends at the next column, as after two BLOCKs.
.macro FLAT
	.rept	8
	stmia	r0, {r4, r5}
	add	r0, r0, #DST_LINE
	.endr
.endm

// speed10: nspire_p_groups_n(dst + FIRST - 8 g (bytes), src + 3 g lines,
// prev + 3 g lines or NULL, n): groups g to g + n - 1 only (n >= 1), for
// nspire_blend.c's runs of groups that changed. nspire_p_groups is all 53.
	.global	nspire_p_groups
	.type	nspire_p_groups, %function
nspire_p_groups:
	add	r0, r0, #FIRST
	mov	r3, #GROUPS
	.global	nspire_p_groups_n
	.type	nspire_p_groups_n, %function
nspire_p_groups_n:
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
	ldr	lr, .Lavg
#if P_FLAT
	ldr	r11, [r1, #(2 * SRC_LINE + 8)]
	ldmia	r1, {r4, r5, r6}
	cmp	r4, r11, ror #16	// the column's corners: pixels 0, 1 = 5, 4 of the last line?
	beq	.Lmix_flat		// then look at the rest (back here if not flat)
.Lmix_blend:				// r4-r6: the first line; r11: the last line's word 2
#else
	ldmia	r1, {r4, r5, r6}
#endif
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
#if P_FLAT
#define LAST2 r11			/* loaded for the corner test */
#else
#define LAST2 r6
	ldr	r6, [r1, #(2 * SRC_LINE + 8)]
#endif
	ldr	r7, [r2, #(2 * SRC_LINE)]
	ldr	r8, [r2, #(2 * SRC_LINE + 4)]
	ldr	r9, [r2, #(2 * SRC_LINE + 8)]
	MIX	r4, r7, r10
	MIX	r5, r8, r10
	MIX	LAST2, r9, r10
	str	r4, [sp, #(BUF + 24)]
	str	r5, [sp, #(BUF + 28)]
	str	LAST2, [sp, #(BUF + 32)]
	BLOCK	0, 2, 4
	BLOCK	6, 8, 10
.Lmix_next:
	ldr	r11, [sp, #COL_END]
	add	r1, r1, #12
	add	r2, r2, #12
	cmp	r1, r11
	bne	.Lmix_column
	sub	r0, r0, #COLUMNS	// r0 went through the LCD lines, r1 r2 one source line
	sub	r0, r0, #8		// the next group: 4 pixels lower in each
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
#if P_FLAT
	ldr	lr, [r1, #(2 * SRC_LINE + 8)]
	ldmia	r1, {r4, r5, r6}
	cmp	r4, lr, ror #16		// the column's corners, as above
	beq	.Lplain_flat		// then look at the rest (back here if not flat)
.Lplain_blend:				// r4-r6: the first line; lr: the last line's word 2
#else
	ldmia	r1, {r4, r5, r6}
	ldr	lr, [r1, #(2 * SRC_LINE + 8)]
#endif
	ldr	r11, [r1, #(2 * SRC_LINE + 4)]
	ldr	r10, [r1, #(2 * SRC_LINE)]
	ldr	r7, [r1, #SRC_LINE]
	ldr	r8, [r1, #(SRC_LINE + 4)]
	ldr	r9, [r1, #(SRC_LINE + 8)]
	stmia	sp, {r4-r11, lr}
	BLOCK	0, 2, 4
	BLOCK	6, 8, 10
.Lplain_next:
	ldr	r11, [sp, #COL_END]
	add	r1, r1, #12
	cmp	r1, r11
	bne	.Lplain_column
	sub	r0, r0, #COLUMNS
	sub	r0, r0, #8
	add	r1, r1, #(2 * SRC_LINE)
	ldr	r4, [sp, #GROUP_N]
	subs	r4, r4, #1
	str	r4, [sp, #GROUP_N]
	bne	.Lplain_group

.Ldone:
	add	sp, sp, #FRAME
	ldmfd	sp!, {r4-r11, pc}

#if P_FLAT
// The column's corner test held (the first line's word 0, r4, is the last
// line's word 2, r11, turned; r4-r6 and r11 are kept for .Lmix_blend): r4
// its own turn (so the last line's word 2 is r4 too), the other 7 words r4,
// then the previous frame's 9 words equal and one colour. The first line
// has its own branch back: in a town or on a route a quarter of the columns
// get here without being flat (ground with a few other pixels), half of
// them not flat in the first line. A conditional load that isn't made
// leaves the flags at "not equal": no compare reads a stale register. No
// compare waits for a load (the word loaded last isn't the one compared
// next).
.Lmix_flat:
	cmp	r4, r4, ror #16
	cmpeq	r4, r5
	cmpeq	r4, r6
	bne	.Lmix_blend		// the first line isn't one colour
	ldr	r7, [r1, #SRC_LINE]
	ldr	r8, [r1, #(SRC_LINE + 4)]
	ldr	r9, [r1, #(SRC_LINE + 8)]
	cmp	r4, r7
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
	MIX	r4, r7, r10		// one mix (lr: as .Lmix_column left it): both halves the mixed pixel
	mov	r5, r4
	FLAT
	b	.Lmix_next

// The same without interframe blending (r4-r6 and lr are kept for
// .Lplain_blend, which loads the other words again); r5 is r4 at the end.
.Lplain_flat:
	cmp	r4, r4, ror #16
	cmpeq	r4, r5
	cmpeq	r4, r6
	bne	.Lplain_blend
	ldr	r7, [r1, #SRC_LINE]
	ldr	r8, [r1, #(SRC_LINE + 4)]
	ldr	r9, [r1, #(SRC_LINE + 8)]
	cmp	r4, r7
	cmpeq	r4, r8
	cmpeq	r4, r9
	ldreq	r7, [r1, #(2 * SRC_LINE)]
	ldreq	r8, [r1, #(2 * SRC_LINE + 4)]
	cmpeq	r4, r7
	cmpeq	r4, r8
	bne	.Lplain_blend
	FLAT
	b	.Lplain_next
#endif

	.align	2
.Lmask:	.word	0x07E0F81F	// UNPACKED_MASK
.Lones4:
	.word	0x00802004	// UNPACKED_ONES << 2
.Lavg:	.word	0x7BEF7BEF
	.size	nspire_p_groups, . - nspire_p_groups
	.size	nspire_p_groups_n, . - nspire_p_groups_n
