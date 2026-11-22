/* Lines of the picture drawn in batches (video.h; a speed change of
 * 2026-10-16). update_gba (main.c) hands each line over at its HBlank;
 * the renderer (update_scanline) draws the waiting ones when the game next
 * changes what the picture is made of, or at VBlank. */
#include "common.h"
#include "main.h"
#include "video.h"
#include "nspire_os.h"

/* nspire_lines_waiting lines from first_waiting on are ready to draw;
 * while there are any, the dynarec's
 * stores to palette, VRAM and OAM go through arm_stub.S's *_flush handlers
 * (st_flush_functions), which draw them before storing. Display register
 * writes (write_io_register16), C stores (write_memory) and DMAs into those
 * areas check nspire_lines_waiting themselves. */
u32 nspire_lines_waiting;
static u32 first_waiting;

/* Writes to the display registers the renderer only reads as it draws a
 * line (BG control and scroll, 0x08-0x1F; windows, mosaic and blending,
 * 0x40-0x57) don't draw the waiting lines first: the lines are split into
 * segments, each with a copy of those registers as they were (Emerald's
 * battle wipes set the windows by HBlank DMA every line, which drew the
 * picture a line at a time). Writes to anything else still draw first, so
 * the other registers (DISPCNT, the affine ones and their reference points)
 * stay as they are while lines wait. */
#define SEG_REGS_A (0x08 / 2)   /* io register index, 12 halfwords */
#define SEG_REGS_B (0x40 / 2)   /* 12 halfwords */
#define SEG_HALVES 12
#define MAX_SEGMENTS 160
struct line_segment
{
  u32 lines;
  u16 a[SEG_HALVES], b[SEG_HALVES];
};
static struct line_segment segments[MAX_SEGMENTS];
static u32 closed_segments;   /* segments before the open one */
static u32 open_lines;        /* waiting lines in the open segment (live registers) */
u32 nspire_batch_lines = 1;   /* 0: each line at its HBlank (tests compare) */

#if defined(HAVE_DYNAREC) && defined(ARM_ARCH)
extern u32 *nspire_mem_tables[9];
extern u32 st_handler_functions[4][17];
extern u32 st_flush_functions[4][3];

/* Regions 0x05-0x07 of the four store tables mem_dispatch reads
 * (arm_stub.S, indexed by the region; the 17-entry st_handler_functions
 * have them at 6-8, after their "-1" entry). */
static void store_handlers(int flush)
{
  int t, i;
  for (t = 0; t < 4; t++)
    for (i = 0; i < 3; i++)
      nspire_mem_tables[t][5 + i] = flush ? st_flush_functions[t][i] : st_handler_functions[t][6 + i];
}
#else
#define store_handlers(flush) ((void)0)
#endif

/* video.h: the OS's tick a USB pad waits for (nspire_frameskip.c sets it;
 * the lines pay a load and a branch while nothing is watched). While
 * watched (speed9a): each line handed over looks at the tick timer's count
 * inline (nspire_os.h nspire_os_watch_at: two loads, the count's an uncached
 * read, and a compare) and calls nspire_os_watch_line only as the tick
 * nears; each line drawn calls it, and it makes the same compare first (in
 * nspire_flush_lines the inline one would move arm_stub.S's hot part, which
 * starts where it ends, a 256-byte step in its cache way). */
int nspire_os_watching;
#define os_watch_near()                                                       \
  do {                                                                        \
    if (__builtin_expect(nspire_os_watching, 0) &&                           \
        *nspire_os_watch_at.reg - 1u < nspire_os_watch_at.value)              \
      nspire_os_watch_line();                                                 \
  } while (0)
#define os_watch() do { if (__builtin_expect(nspire_os_watching, 0)) nspire_os_watch_line(); } while (0)

/* A frame that isn't shown: nothing to do (update_scanline would return at
 * once, video.cc, and its timing wrapper times nothing then). */
NSPIRE_HOT void nspire_video_line_ready(u32 vcount)
{
  os_watch_near();
  if (skip_next_frame)
    return;
  if (!nspire_batch_lines)
  {
    update_scanline();
    return;
  }
  if (!nspire_lines_waiting)
  {
    first_waiting = vcount;
    store_handlers(1);
  }
  nspire_lines_waiting++;
  open_lines++;
}

static void copy_regs(u16 *dst, const u16 *src)
{
  u32 *d = (u32 *)dst;
  const u32 *s = (const u32 *)src;
  d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; d[4] = s[4]; d[5] = s[5];
}

/* Before a write to one of the segments' registers (0x08-0x1F, 0x40-0x57):
 * the open segment's lines keep the registers as they are now. */
/* Pinned in a 4 KB cache way (common.h NSPIRE_LINEIO, nspire_lineio.ld). */
NSPIRE_LINEIO(nspire_video_write_line_regs)
void nspire_video_write_line_regs(void)
{
  struct line_segment *seg;

  if (!open_lines)
    return;
  if (closed_segments == MAX_SEGMENTS)
  {
    nspire_flush_lines();
    return;
  }
  seg = &segments[closed_segments++];
  seg->lines = open_lines;
  copy_regs(seg->a, io_registers + SEG_REGS_A);
  copy_regs(seg->b, io_registers + SEG_REGS_B);
  open_lines = 0;
}

static void draw_lines(u32 first, u32 n)
{
  u32 i;
  for (i = 0; i < n; i++)
  {
    write_ioreg(REG_VCOUNT, first + i);
    update_scanline();
    os_watch();
  }
}

/* The renderers read the line number from REG_VCOUNT. */
NSPIRE_HOT void nspire_flush_lines(void)
{
  u32 n = nspire_lines_waiting, vcount = read_ioreg(REG_VCOUNT), first = first_waiting, i;

  if (!n)
    return;
  nspire_lines_waiting = 0;
  store_handlers(0);
  if (closed_segments)
  {
    u16 live_a[SEG_HALVES], live_b[SEG_HALVES];
    copy_regs(live_a, io_registers + SEG_REGS_A);
    copy_regs(live_b, io_registers + SEG_REGS_B);
    for (i = 0; i < closed_segments; i++)
    {
      copy_regs(io_registers + SEG_REGS_A, segments[i].a);
      copy_regs(io_registers + SEG_REGS_B, segments[i].b);
      draw_lines(first, segments[i].lines);
      first += segments[i].lines;
    }
    copy_regs(io_registers + SEG_REGS_A, live_a);
    copy_regs(io_registers + SEG_REGS_B, live_b);
    closed_segments = 0;
  }
  draw_lines(first, open_lines);
  open_lines = 0;
  write_ioreg(REG_VCOUNT, vcount);
}

/* A reset or a loaded state: the waiting lines belong to a picture that
 * isn't there any more. */
void nspire_video_lines_reset(void)
{
  nspire_reuse_reset();   /* speed10: no line known; the watched stores (nspire_reuse.c) */
  nspire_lines_waiting = 0;
  closed_segments = open_lines = 0;
  store_handlers(0);
}
