/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef VIDEO_H
#define VIDEO_H

void update_scanline(void);
void video_reload_counters(void);

extern s32 affine_reference_x[2];
extern s32 affine_reference_y[2];

extern u16* gba_screen_pixels;

#ifdef NSPIRE_BUILD
/* Lines drawn in batches (nspire_lines.c): a line is
 * drawn when the game next changes what the picture is made of (display
 * registers, palette, VRAM, OAM, a DMA into them) or at VBlank, not at its
 * own HBlank, so the renderer and the game's code don't push each other out
 * of the instruction cache every line. Writers call nspire_video_write()
 * first: the waiting lines still see the old data. */
extern u32 nspire_lines_waiting;
extern u32 nspire_batch_lines;
void nspire_video_line_ready(u32 vcount);
void nspire_flush_lines(void);
void nspire_video_lines_reset(void);
void nspire_video_write_line_regs(void);
/* Display register io index r (0x08-0x1F, 0x40-0x57): kept per segment of
 * waiting lines instead of drawing them first (nspire_lines.c). */
#define nspire_line_reg(r) ((u32)((r) - 0x08 / 2) < 12 || (u32)((r) - 0x40 / 2) < 12)
#define nspire_video_write()                                                  \
  do { if (nspire_lines_waiting) nspire_flush_lines(); } while (0)
/* 2026-11-17 (final/speed8): the OS's tick a USB pad waits for, taken as
 * it comes (nspire_frameskip.c, "A pad's OS time when the OS's tick
 * comes"): while it's watched, each line handed over and each line drawn
 * looks at the time (nspire_lines.c). */
extern int nspire_os_watching;
void nspire_os_watch_line(void);
/* speed10 (nspire_reuse.c, lines drawn again only when what they're made
 * of changed): VRAM and palette RAM's generation, one up whenever a write
 * changes a value there (arm_stub.S's stores from the dynarec, a DMA that
 * would: nspire_video_dma, before it), or a C store there (a cheat); a
 * reset or a loaded state forgets every line. (reg[]'s word 31: arm_stub.S
 * counts into it at reg_base.)
 * A line whose display registers, objects and this generation are those it
 * was drawn with is not drawn again. */
#define nspire_video_gen (reg[REG_VIDEO_GEN])
void nspire_video_dma(const void *dmach, u32 src, u32 dst, u32 bytes);
int nspire_line_reuse(u32 vcount);
/* 1: every line of this frame is looked at; 4 (a probe), one in 4; 0,
 * none: nspire_line_reuse is called for those and the last only
 * (nspire_reuse.c: frames where none was kept lately) */
extern u32 nspire_reuse_lines;
void nspire_line_oam(u32 video_mode);
const u32 *nspire_line_ids(const u16 *frame);
u32 nspire_lines_kept_last(void);
void nspire_reuse_reset(void);
#ifdef NSPIRE_LINE_CHECK
void nspire_line_check_before(u32 vcount);
void nspire_line_check_after(u32 vcount);
#endif
#endif

#endif
