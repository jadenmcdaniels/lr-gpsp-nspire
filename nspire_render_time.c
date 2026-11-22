#include "common.h"
#include "main.h"
#include "video.h"
#include "nspire.h"
#include "nspire_frontend.h"

/* video.cc's update_scanline, renamed at compile time (Makefile) so this
 * wrapper can time it; video_cc_line_kept: the same for a line not drawn. */
extern void video_cc_update_scanline(void);
extern void video_cc_line_kept(void);

/* Out of line: the PC speed estimate (tests/calc_speed.py) counts drawing
 * time by this function. */
__attribute__((noinline)) void update_scanline(void)
{
  u32 start = 0, vcount, every;
  int look = 0;

  /* speed10 (nspire_reuse.c): a line made of what it was made of last time
   * is the picture it was (left in place or copied): not drawn. The lines
   * looked at: every one (nspire_reuse_lines 1), one in 4 (a probe), none
   * (0); the frame's last always. Here, not in video.cc: the renderer's
   * code keeps its places. OAM written: the lines of the objects that
   * changed first (video.cc orders them again after, either way). */
  if (!skip_next_frame)
  {
    vcount = read_ioreg(REG_VCOUNT);
    every = nspire_reuse_lines;
    if (reg[OAM_UPDATED])
      nspire_line_oam(read_ioreg(REG_DISPCNT) & 0x07);
    look = (every && !(vcount & (every - 1))) || vcount == GBA_SCREEN_HEIGHT - 1;
    if (look && nspire_line_reuse(vcount))
    {
      video_cc_line_kept();
      return;
    }
#ifdef NSPIRE_LINE_CHECK
    if (look)
      nspire_line_check_before(vcount);
#endif
  }

  /* Drawing time, for the detailed FPS counter and the speed test. The
   * timer's 30 us steps are coarse for a line, but even out over a frame. */
  if (nspire_perf_on && !skip_next_frame)
    start = nspire_ticks();
  video_cc_update_scanline();
  if (nspire_perf_on && !skip_next_frame)
    nspire_render_ticks += nspire_ticks() - start;
#ifdef NSPIRE_LINE_CHECK
  if (look)
    nspire_line_check_after(vcount);
#endif
}
