/* speed10: lines drawn again only when what they're made of changed.
 *
 * A line's picture is a function of what the renderer reads for it: the
 * display registers 0x00-0x57 but DISPSTAT and VCOUNT (as the line's
 * segment has them: nspire_lines.c draws a segment with its copies in
 * place), the affine reference points, VRAM, palette RAM, OAM and the
 * object lists video.cc's order_obj makes from it (OAM, the video mode's
 * class, DISPCNT's H-blank bit, the sprite limit), and two settings (the
 * sprite limit, the host's blending off). A line whose inputs are those it
 * was drawn with last time is that picture again: update_scanline
 * (nspire_render_time.c's, around video.cc's) doesn't draw it when the
 * frame buffer it draws into holds that picture, or copies it from the
 * other buffer (interframe blending draws the frames into two buffers in
 * turn) when that one does.
 *
 * What a line was drawn with, cheaply:
 * - VRAM and palette RAM: nspire_video_gen (reg[]'s word 31), one up at
 *   every write that changes a value there (arm_stub.S's store handlers,
 *   the dynarec's stores; a DMA that would, nspire_video_dma; a C store, a
 *   cheat's, gba_memory.c). In frames that stamp no line it is negative,
 *   and every VRAM store counts, its old value not read.
 *   A reset or a loaded state counts as a change; so does a new value of
 *   the two settings (looked at a frame's first line: the menus change
 *   them between frames).
 * - OAM: an object generation per line, one up on the lines of an object
 *   that changed, as it was and as it is (nspire_line_oam, called when
 *   OAM was written, before order_obj: OAM against a copy of it).
 * - the registers and reference points: a copy of them for each line, as
 *   the last frame drew it.
 * Each line drawn gets an id; held_id[] says which id each line of each
 * frame buffer holds (0: not known), and the picture output reads them
 * (nspire_line_ids): two frames' lines with the same id (not 0) are the
 * same pixels. An object window's line writes its scratch into the next
 * line (video.cc render_scanline_objs, PIXCOPY): that line's id goes.
 *
 * In the frontend's files (not LTO's): the game's data stays where it
 * was (nspire_lineio.ld). */

#include "common.h"
#include "video.h"
#include "gba_memory.h"

#include <string.h>

/* What every line reads, in one cache line (the game's data takes the
 * data cache between lines: Advance Wars' interrupt every line), and the
 * rest in the next. */
static struct
{
  u32 stamping;             /* 1: this frame stamps its lines (and looks for lines to
                               keep), PROBE: one in PROBE, 0: none; nspire_reuse_lines,
                               nspire_render_time.c's */
  u32 serial;               /* the frame's */
  u32 gen_end;              /* nspire_video_gen at the end of the frame before */
  s32 last_line;            /* the frame's last line looked at (-1: a new frame is next) */
  u32 line_h;               /* held_id[] of the line being drawn (nspire_line_reuse) */
  const u16 *held_buf[2];   /* the frame buffers held_id[] are of */
  u32 held_last;
  u32 next_id, kept_this, kept_8, fruitless, cfg_seen, kept_last;
  u32 window, probe_at, ids_clear;
} hot __attribute__((aligned(32))) = { .stamping = 1, .serial = 1, .last_line = -1 };
extern u32 nspire_reuse_lines __attribute__((alias("hot")));
u32 nspire_line_reuse_on = 1;   /* 0: every line drawn (tests, perf reuse=0) */
u32 nspire_reuse_probe = 1;     /* 0: every frame stamps, 2: none (perf probe=N) */
#ifdef NSPIRE_LINE_CHECK
static int check_keep;   /* the line being drawn would have been kept */
#endif
#define stamping hot.stamping
#define serial hot.serial
#define last_line hot.last_line
#define line_h hot.line_h
#define held_last hot.held_last
#define next_id hot.next_id
#define kept_this hot.kept_this
#define kept_8 hot.kept_8
#define fruitless hot.fruitless
#define cfg_seen hot.cfg_seen

#ifdef NSPIRE_LINE_CHECK
u32 nspire_line_check_ok, nspire_line_check_bad, nspire_line_check_copy;
#endif

extern int sprite_limit;
extern u32 nspire_gba_blend_off;

#define LINES GBA_SCREEN_HEIGHT

/* A line's row: its stamp and its snapshot of the registers and reference
 * points, the words that change most (the scrolls) first, in the stamp's
 * cache line, so a line that scrolled is turned down there. */
enum
{
  ROW_GEN,      /* nspire_video_gen it was drawn with */
  ROW_OBJGEN,   /* obj_gen[] */
  ROW_ID,       /* the picture (0: none) */
  ROW_SERIAL,   /* the frame it was drawn or kept in */
  ROW_SCROLL,   /* io words 4-7: the BGs' scrolls (0x10-0x1F) */
  ROW_AFFINE = ROW_SCROLL + 4,   /* the 4 reference points */
  ROW_IO0 = ROW_AFFINE + 4,      /* io words 0, 2, 3 (DISPCNT, BGxCNT) */
  ROW_IO8 = ROW_IO0 + 3,         /* io words 8-21 (0x20-0x57) */
  ROW_WORDS = 32                 /* (29 used) */
};
static u32 rows[LINES][ROW_WORDS] __attribute__((aligned(32)));
static u32 obj_gen[LINES];
static u32 held_id[2][LINES];   /* the picture (stamp id) each line of each frame buffer holds */

/* Frames that stamp their lines (and look for lines to keep), stamping 1:
 * every one, but where fewer than KEPT_MIN lines a frame (backdrops not
 * counted: keeping them saves nothing) were kept over 8 frames (the game
 * changes VRAM or the palette every frame, a scroll every line's
 * registers): then two in a row every 8 frames are probes (stamping PROBE:
 * one line in PROBE stamped, in the second counted if it could have been
 * kept), until such a second one would have kept KEPT_MIN (one in PROBE of
 * them). The other frames (stamping 0) don't look at their lines (update_scanline
 * calls nspire_line_reuse only for the last), nor at what DMAs or OAM
 * writes change (nspire_video_dma, nspire_line_oam: a change, as far as
 * any stamp knows), nor VRAM stores (arm_stub.S, REG_VIDEO_GEN's sign).
 * (Tried: probes spaced out to 16, 32, 64 frames while fruitless, all lines
 * stamped: the speed model's Fire Emblem intro 12.86 ms a frame at 8,
 * 12.80, 12.97, 13.31; Golden Sun's 11.55, 11.63, 11.63, 11.74.) */
#define KEPT_MIN 32
#define PROBE 4   /* a probe frame looks at one line in 4 */

static u16 oam_seen[512] __attribute__((aligned(4)));
static u32 oam_key_seen = ~0u;

static inline u32 cfg_now(void)
{
  return (u32)sprite_limit | nspire_gba_blend_off << 8;
}

/* After a frame's last line (lines come 0 to 159 in each drawn frame):
 * the next frame's serial, whether it stamps (so what comes between the
 * two, the VBlank's DMAs and OAM, is looked at as the next frame needs). */
static __attribute__((noinline)) void new_frame(void)
{
  if (++serial == 0)
  {
    serial = 1;
    nspire_video_gen++;   /* (every 2 years: no old stamp matches a new frame) */
  }
  if (!fruitless)
  {
    kept_8 += kept_this;
    if (++hot.window == 8)
    {
      if (kept_8 < 8 * KEPT_MIN)
      {
        fruitless = 1;
        hot.probe_at = serial + 6;
      }
      kept_8 = hot.window = 0;
    }
  }
  else if (serial == hot.probe_at + 2)
  {
    /* (kept_this: the probe's second frame's, its lines one in PROBE) */
    if (kept_this * PROBE >= KEPT_MIN)
      fruitless = 0;
    else
      hot.probe_at = serial + 6;
  }
  hot.kept_last = kept_this;
  kept_this = 0;
  stamping = nspire_reuse_probe == 2 ? 0 : !nspire_reuse_probe || !fruitless ? 1 : serial - hot.probe_at < 2 ? PROBE : 0;
  /* the generation's sign: arm_stub.S compares the VRAM values its stores
   * replace only while lines are stamped (the reads take data cache lines
   * the game's own data then loses); a new sign is a new generation */
  nspire_video_gen = (nspire_video_gen & 0x7FFFFFFFu) | (stamping ? 0 : 0x80000000u);
  if (stamping != 1 && !hot.ids_clear)
  {
    /* the next frame's lines aren't all looked at (update_scanline calls
     * nspire_line_reuse for its last and, in a probe, one in PROBE):
     * none known */
    memset(held_id, 0, sizeof(held_id));
    hot.ids_clear = 1;
  }
  else if (stamping == 1)
    hot.ids_clear = 0;
  hot.gen_end = nspire_video_gen;
}

/* A frame's first line: the settings (changed between frames, by the
 * menus) as the frame before had them, or every line changed. */
static __attribute__((noinline)) void frame_begin(s32 last)
{
  u32 cfg = cfg_now();
  if (last >= 0)
    new_frame();   /* (a frame that didn't get to its last line) */
  if (cfg != cfg_seen)
  {
    cfg_seen = cfg;
    nspire_video_gen++;
  }
}

/* A frame buffer not seen before takes the entry of the one not drawn
 * last, its lines not known. */
static __attribute__((noinline)) u32 held_new(const u16 *buf)
{
  u32 h = held_last ^ 1;
  hot.held_buf[h] = buf;
  memset(held_id[h], 0, sizeof(held_id[h]));
  return h;
}

/* held_id[]'s entry for the frame buffer drawn into. */
static inline u32 held_for(const u16 *buf)
{
  u32 h = hot.held_buf[0] == buf ? 0 : hot.held_buf[1] == buf ? 1 : held_new(buf);
  held_last = h;
  return h;
}

/* How many lines the frame drawn last kept (backdrops not counted): with
 * none, the 3:2 picture is made whole without looking at its groups
 * (nspire_pictures.c: nearly every line is a new picture then). */
u32 nspire_lines_kept_last(void)
{
  return hot.kept_last;
}

const u32 *nspire_line_ids(const u16 *frame)
{
  if (frame && hot.held_buf[0] == frame)
    return held_id[0];
  if (frame && hot.held_buf[1] == frame)
    return held_id[1];
  return NULL;
}

/* The registers and reference points now against row r's snapshot, the
 * scrolls first. */
static int snap_equal(const u32 *r)
{
  const u32 *io = (const u32 *)io_registers;
  u32 d, i;
  if ((r[ROW_SCROLL] ^ io[4]) | (r[ROW_SCROLL + 1] ^ io[5]) | (r[ROW_SCROLL + 2] ^ io[6]) |
      (r[ROW_SCROLL + 3] ^ io[7]))
    return 0;
  if ((r[ROW_AFFINE] ^ (u32)affine_reference_x[0]) | (r[ROW_AFFINE + 1] ^ (u32)affine_reference_y[0]) |
      (r[ROW_AFFINE + 2] ^ (u32)affine_reference_x[1]) | (r[ROW_AFFINE + 3] ^ (u32)affine_reference_y[1]))
    return 0;
  d = (r[ROW_IO0] ^ io[0]) | (r[ROW_IO0 + 1] ^ io[2]) | (r[ROW_IO0 + 2] ^ io[3]);
  for (i = 0; i < 14; i++)
    d |= r[ROW_IO8 + i] ^ io[8 + i];
  return !d;
}

/* Row r's snapshot of the registers and reference points as they are. */
static inline void snap_take(u32 *r)
{
  const u32 *io = (const u32 *)io_registers;
#if defined(__arm__)
  /* with LDM/STM: the line's code small */
  __asm__ volatile("add r0, %1, #16\n\t"
                   "ldmia r0, {r2-r5}\n\t"
                   "add r0, %0, #(4 * 4)\n\t"
                   "stmia r0, {r2-r5}\n\t"
                   "ldr r2, [%1]\n\t"
                   "ldr r3, [%1, #8]\n\t"
                   "ldr r4, [%1, #12]\n\t"
                   "add r0, %0, #(12 * 4)\n\t"
                   "stmia r0!, {r2-r4}\n\t"
                   "add r1, %1, #32\n\t"
                   "ldmia r1!, {r2-r9}\n\t"
                   "stmia r0!, {r2-r9}\n\t"
                   "ldmia r1, {r2-r7}\n\t"
                   "stmia r0, {r2-r7}"
                   :
                   : "r"(r), "r"(io)
                   : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "memory");
#else
  u32 i;
  for (i = 0; i < 4; i++)
    r[ROW_SCROLL + i] = io[4 + i];
  r[ROW_IO0] = io[0];
  r[ROW_IO0 + 1] = io[2];
  r[ROW_IO0 + 2] = io[3];
  for (i = 0; i < 14; i++)
    r[ROW_IO8 + i] = io[8 + i];
#endif
  r[ROW_AFFINE] = affine_reference_x[0];
  r[ROW_AFFINE + 1] = affine_reference_y[0];
  r[ROW_AFFINE + 2] = affine_reference_x[1];
  r[ROW_AFFINE + 3] = affine_reference_y[1];
}

/* Line y can be kept if its registers are its snapshot's and a frame
 * buffer holds its picture: then it's left as it is or copied from the
 * other buffer. */
/* Whether line y, with the registers as they are, shows the backdrop only
 * (forced blank, no layer on, or none on in the windows that cover it):
 * drawing it is a fill, cheaper than a copy, and keeping it saves nothing
 * worth its stamps. Not sure: no. (video.cc in_window_y's test.) */
static int backdrop_only(u32 y)
{
  u32 d = io_registers[REG_DISPCNT], layers = (d >> 8) & 0x1F, on, n;
  if (d & 0x80)
    return 1;
  if (!(d & 0xE000))
    return !layers;
  on = io_registers[REG_WINOUT] & 0x1F;   /* (outside them) */
  if (d & 0x8000)
    on |= (io_registers[REG_WINOUT] >> 8) & 0x1F;
  for (n = 0; n < 2; n++)
    if (d & (0x2000 << n))
    {
      u32 v = io_registers[REG_WIN0V + n], top = v >> 8, bottom = v & 0xFF;
      if (top <= 227 && (bottom > 227 || (top > bottom ? y > top || y <= bottom : y >= top && y < bottom)))
        on |= (io_registers[REG_WININ] >> (8 * n)) & 0x1F;
    }
  return !(on & layers);
}

static __attribute__((noinline)) int keep_line(u32 y, u32 *r, u32 h)
{
  u32 id = r[ROW_ID];
  int copy, cheap;
  if (!nspire_line_reuse_on)
    return 0;
  copy = held_id[h][y] != id;
  if (copy && (!hot.held_buf[h ^ 1] || held_id[h ^ 1][y] != id))
    return 0;
  if (!snap_equal(r))
    return 0;
  /* a backdrop: drawn rather than copied, and not counted as kept */
  cheap = backdrop_only(y);
  if (copy && cheap)
    return 0;
#ifdef NSPIRE_LINE_CHECK
  /* The check build draws it anyway and compares it with what this would
   * have left (nspire_line_check_after). */
  check_keep = 1 + cheap;
  return 0;
#else
  if (copy)
  {
    memcpy(gba_screen_pixels + y * GBA_SCREEN_PITCH, hot.held_buf[h ^ 1] + y * GBA_SCREEN_PITCH,
           GBA_SCREEN_WIDTH * sizeof(u16));
    held_id[h][y] = id;
  }
  r[ROW_SERIAL] = serial;
  kept_this += !cheap;
  return 1;
#endif
}

/* The stamp of line y, about to be drawn (with the registers it is drawn
 * with: nspire_flush_lines has them in place) into frame buffer h. */
static __attribute__((noinline)) void stamp(u32 y, u32 *r, u32 h)
{
  u32 id = ++next_id;
  r[ROW_GEN] = nspire_video_gen;
  r[ROW_OBJGEN] = obj_gen[y];
  r[ROW_ID] = id;
  r[ROW_SERIAL] = serial;
  snap_take(r);
  held_id[h][y] = id;
}

/* After line y (kept or about to be drawn into h): the object window's
 * scratch line (video.cc render_scanline_objs, PIXCOPY) is the next one,
 * not known any more; after the frame's last, the next frame. */
static inline void line_end(u32 y, u32 h, int drawn)
{
  if (drawn && (io_registers[REG_DISPCNT] & 0x8000) && y + 1 < LINES)
    held_id[h][y + 1] = 0;
#ifndef NSPIRE_LINE_CHECK
  if (y == LINES - 1)
  {
    new_frame();
    last_line = -1;
  }
#endif
}

/* A probe frame's line (one in PROBE, while fruitless): only counted if
 * it could have been kept (not kept: the lines between aren't looked at,
 * an object window's scratch could have been drawn over it), and stamped
 * (its id isn't held: the frame buffer's lines are all unknown). */
static __attribute__((noinline)) void probe_line(u32 y)
{
  u32 *r = rows[y];
  if (nspire_video_gen == hot.gen_end && r[ROW_ID] && r[ROW_GEN] == nspire_video_gen &&
      r[ROW_SERIAL] == serial - 1 && r[ROW_OBJGEN] == obj_gen[y] && snap_equal(r) && !backdrop_only(y))
    kept_this++;
  r[ROW_GEN] = nspire_video_gen;
  r[ROW_OBJGEN] = obj_gen[y];
  r[ROW_ID] = ++next_id;
  r[ROW_SERIAL] = serial;
  snap_take(r);
}

/* Whether line y of gba_screen_pixels is already its picture (left as it
 * is or copied from the other buffer): then update_scanline doesn't draw it.
 * Else it is stamped for the next frame and drawn. A line can be kept when
 * the frame before stamped it and nothing it is made of changed since. A
 * frame that doesn't stamp only says its lines aren't known (the rows of
 * the frame before are then 2 frames old: none matches the next frame). */
int nspire_line_reuse(u32 y)
{
  u32 *r, h;

  if (y >= LINES)
    return 0;   /* (never: the renderer draws lines 0-159) */
  if ((s32)y <= last_line || last_line < 0)
    frame_begin(last_line);
  last_line = y;
  if (stamping != 1)
  {
    if (stamping && !(y % PROBE))
      probe_line(y);
    line_end(y, 0, 0);   /* (and the last line: its frame's end) */
    return 0;
  }
  line_h = h = held_for(gba_screen_pixels);
  r = rows[y];
  /* (no stamp of the frame before has the generation if it changed since
   * its end: then the row isn't read) */
  if (nspire_video_gen == hot.gen_end && r[ROW_ID] && r[ROW_GEN] == nspire_video_gen &&
      r[ROW_SERIAL] == serial - 1 && r[ROW_OBJGEN] == obj_gen[y] && keep_line(y, r, h))
  {
    line_end(y, h, 0);
    return 1;
  }
#ifdef NSPIRE_LINE_CHECK
  if (check_keep)
    return 0;
#endif
  stamp(y, r, h);
  line_end(y, h, 1);
  return 0;
}

#ifdef NSPIRE_LINE_CHECK
static u16 check_line[2][GBA_SCREEN_WIDTH];

/* Before line y is drawn: the buffer's lines y and y + 1 as they are. */
void nspire_line_check_before(u32 y)
{
  memcpy(check_line, gba_screen_pixels + y * GBA_SCREEN_PITCH, sizeof(check_line));
}

/* Line y was drawn just now: if it would have been kept, the picture kept
 * (the buffer's line before, or the other buffer's) must be the one drawn. */
void nspire_line_check_after(u32 y)
{
  u32 *r = rows[y], h = line_h;
  if (check_keep)
  {
    const u16 *ref = held_id[h][y] == r[ROW_ID] ? check_line[0] : hot.held_buf[h ^ 1] + y * GBA_SCREEN_PITCH;
    kept_this += check_keep == 1;   /* (2: a backdrop's, not counted) */
    check_keep = 0;
    if (!memcmp(ref, gba_screen_pixels + y * GBA_SCREEN_PITCH, GBA_SCREEN_WIDTH * sizeof(u16)))
      nspire_line_check_ok++;
    else
      nspire_line_check_bad++;
    nspire_line_check_copy += held_id[h][y] != r[ROW_ID];
    held_id[h][y] = r[ROW_ID];
    r[ROW_SERIAL] = serial;
    /* not drawn, it wouldn't have written an object window's scratch */
    if (y + 1 < LINES)
      memcpy(gba_screen_pixels + (y + 1) * GBA_SCREEN_PITCH, check_line[1], sizeof(check_line[1]));
  }
  if (y == LINES - 1)
  {
    new_frame();
    last_line = -1;
  }
}
#endif

/* The address of the byte the DMA reads at GBA address s (dma_read_*:
 * EWRAM, IWRAM, the cartridge's loaded pages), or NULL. */
static const u8 *dma_from(u32 s)
{
  const u8 *page;
  s &= 0x0FFFFFFF;
  switch (s >> 24)
  {
  case 0x02:
    return ewram + (s & 0x3FFFF);
  case 0x03:
    return iwram + 0x8000 + (s & 0x7FFF);
  case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C:
    page = memory_map_read[s >> 15];
    return page && (s & 0x1FFFFFF) < gamepak_size ? page + (s & 0x7FFF) : NULL;
  default:
    return NULL;
  }
}

/* The byte of palette RAM or VRAM a DMA writes at GBA address t (the VRAM
 * mirror as dma_write_vram), or NULL for any other region. */
static u8 *dma_to(u32 t)
{
  t &= 0x0FFFFFFF;
  if (t >> 24 == 0x05)
    return (u8 *)palette_ram + (t & 0x3FF);
  if (t >> 24 == 0x06)
  {
    t &= 0x1FFFF;
    return vram + (t >= 0x18000 ? t - 0x8000 : t);
  }
  return NULL;
}

/* A DMA about to run (gba_memory.c dma_transfer) that writes into palette
 * RAM or VRAM: one more in nspire_video_gen unless every unit it writes
 * there is the value already there. Its source is read as the DMA will; a
 * source of any other kind, or a page not loaded, counts as a change. Units
 * outside palette RAM and VRAM (a DMA crossing into OAM or I/O) aren't
 * looked at. Both counting up within one source page and one destination
 * area (a palette's or a tile set's copy): a word loop. */
void nspire_video_dma(const void *dmach, u32 src, u32 dst, u32 bytes)
{
  static const s32 stride[4] = { 1, -1, 0, 1 };
  const dma_transfer_type *d = (const dma_transfer_type *)dmach;
  u32 unit = d->length_type == DMA_16BIT ? 2 : 4, n = bytes / unit, i;
  s32 sstep, dstep;
  const u8 *from;
  u8 *to;

  if (d->source_direction >= 3 || !n)
    return;   /* (copies nothing) */
  if (!stamping)
    goto changed;   /* (no line is stamped this frame: not worth looking) */
  sstep = stride[d->source_direction] * (s32)unit;
  dstep = stride[d->dest_direction] * (s32)unit;
  if (sstep > 0 && dstep > 0)
  {
    u32 se = src + bytes - 1, de = dst + bytes - 1;
    from = dma_from(src);
    to = dma_to(dst);
    /* (one array each: the ends' bytes where the starts' run takes them;
     * a cartridge page for the cartridge, whose pages are apart) */
    if (from && to && dma_from(se) == from + bytes - 1 && dma_to(de) == to + bytes - 1 &&
        ((src & 0x0FFFFFFF) >> 24 < 0x08 || src >> 15 == se >> 15))
    {
      if (unit == 4)
      {
        const u32 *a = (const u32 *)from, *b = (const u32 *)to;
        for (i = 0; i < n; i++)
          if (a[i] != b[i])
            goto changed;
      }
      else
      {
        const u16 *a = (const u16 *)from, *b = (const u16 *)to;
        for (i = 0; i < n; i++)
          if (a[i] != b[i])
            goto changed;
      }
      return;
    }
  }
  for (i = 0; i < n; i++, src += sstep, dst += dstep)
  {
    from = dma_from(src);
    to = dma_to(dst);
    if (!to)
      continue;
    if (!from || (unit == 2 ? *(const u16 *)from != *(const u16 *)to : *(const u32 *)from != *(const u32 *)to))
      goto changed;
  }
  return;
changed:
  nspire_video_gen++;
}

/* The lines an object covers, with its attributes a0 and a1 (as order_obj
 * places it, whether it shows or not: a superset), get a new object
 * generation. */
static void obj_lines_changed(u32 a0, u32 a1)
{
  static const u8 height[4][4] = { { 8, 16, 32, 64 }, { 8, 8, 16, 32 }, { 16, 32, 32, 64 }, { 64, 64, 64, 64 } };
  s32 h = height[a0 >> 14 & 3][a1 >> 14 & 3], y = a0 & 0xFF, end;
  if (a0 & 0x200)
    h *= 2;
  if (y > 160)
    y -= 256;
  end = y + h > LINES ? LINES : y + h;
  for (y = y < 0 ? 0 : y; y < end; y++)
    obj_gen[y]++;
}

/* OAM was written (update_scanline, before video.cc orders the objects
 * again): the lines of every object that changed, as it was and as it is;
 * of every affine object, if an affine parameter changed; every line, if
 * order_obj's other inputs did. */
void nspire_line_oam(u32 video_mode)
{
  u32 key = (video_mode >= 3) | (io_registers[REG_DISPCNT] & 0x20) | (u32)sprite_limit << 8;
  u32 o, affine = 0;

  if (!stamping)
  {
    /* (no line is stamped this frame: the next call counts every line
     * changed and takes OAM's copy then) */
    oam_key_seen = ~0u;
    return;
  }
  if (key != oam_key_seen)
  {
    for (o = 0; o < LINES; o++)
      obj_gen[o]++;
    oam_key_seen = key;
    memcpy(oam_seen, oam_ram, sizeof(oam_seen));
    return;
  }
  /* (an object's two words: attributes 0 and 1; attribute 2 and the
   * affine parameter in the top half. Only what changed is copied.) */
  for (o = 0; o < 128; o++)
  {
    const u32 *now = (const u32 *)oam_ram + o * 2;
    u32 *was = (u32 *)oam_seen + o * 2, d0 = now[0] ^ was[0], d1 = now[1] ^ was[1];
    if (d0 | d1)
    {
      if (d0 | (d1 & 0xFFFF))
      {
        obj_lines_changed(was[0] & 0xFFFF, was[0] >> 16);
        obj_lines_changed(now[0] & 0xFFFF, now[0] >> 16);
      }
      affine |= d1 >> 16;
      was[0] = now[0];
      was[1] = now[1];
    }
  }
  /* an affine parameter changed: every affine object's lines (one whose
   * attribute 0 changed had its lines, as it was and as it is, above) */
  if (affine)
    for (o = 0; o < 128; o++)
      if (oam_ram[o * 4] & 0x100)
        obj_lines_changed(oam_ram[o * 4], oam_ram[o * 4 + 1]);
}

/* A reset or a loaded state (nspire_video_lines_reset): no line is known. */
void nspire_reuse_reset(void)
{
  memset(rows, 0, sizeof(rows));
  memset(held_id, 0, sizeof(held_id));
  hot.held_buf[0] = hot.held_buf[1] = NULL;
  oam_key_seen = ~0u;
  last_line = -1;
  stamping = 1;
  fruitless = kept_8 = hot.window = hot.ids_clear = 0;
  nspire_video_gen &= 0x7FFFFFFFu;   /* (stamping: compared) */
  nspire_video_gen++;
}
