/* speed10: the 3:2 pictures without the groups the screen buffer holds
 * already (nspire_video_present.c calls nspire_reuse_picture in the place of
 * nspire_blend_turn_aspect / nspire_blend_aspect_landscape), from the ids
 * of the frames' lines (nspire_reuse.c, nspire_line_ids). In the
 * frontend's files (not LTO's): the game's data stays where it was. */

#include "common.h"
#include "video.h"
#include "nspire_frontend.h"

#include <string.h>

#define LINES GBA_SCREEN_HEIGHT

extern u32 nspire_line_reuse_on;
extern u32 nspire_os_long_sleeps;   /* (nspire_os.c: the OS's waits longer than a tick) */

/* Group g of the 3:2 picture (source lines 3g to 3g + 2, interframe
 * blended with the frame before or not) is a function of those lines'
 * pixels; group 53 is the last line (and line 13, black). What a screen
 * buffer's group was made from is kept as the ids of its lines in the frame
 * and in the frame blended with (the frame's own when not blended, or when
 * the two are the same lines: blending a line with itself gives the line,
 * average_up(a, a) = a, so the group is the unblended one). A group whose
 * ids (none 0) are those the buffer's group was made from is that picture
 * already: it isn't made again. The groups made are made in runs, each
 * run blended or not as its groups need, by the same code
 * (nspire_blend_aspect_groups / nspire_blend_landscape_groups) as the
 * whole picture. The bands of text are outside the groups, and the
 * buffers are written by nothing else but the menus and the link's
 * screen, after which nspire_present_forget_bands forgets them here too.
 *
 * (In case anything else writes there unforeseen, the OS in its time
 * most of all.) The groups are forgotten after the OS's waits longer than
 * a tick (nspire_os_long_sleeps) and the saves' file calls (nspire_host_main.c
 * save_job_frame); and each picture makes FRESH groups whatever their keys,
 * the next ones of its buffer's each time: anything else heals in
 * GROUPS / FRESH = 14 pictures into a buffer, 42 pictures (0.7 s at 60
 * pictures a second). */
#define GROUPS (LINES / 3 + 1)
#define SCREEN_KEYS 3
#define FRESH 4
static struct
{
  const u16 *buf;
  u32 kind, fresh;
  u32 key[GROUPS][6];
} pictures[SCREEN_KEYS];
static u32 picture_next, os_seen;

void nspire_reuse_pictures_forget(void)
{
  memset(pictures, 0, sizeof(pictures));
}

/* (out of line, rarely run: nspire_reuse_picture's place in a way is
 * pinned, nspire_lineio.ld) The OS waited long: every group forgotten. A
 * buffer not seen before: the place of the one seen longest ago. */
static __attribute__((noinline)) void pictures_os_waited(void)
{
  os_seen = nspire_os_long_sleeps;
  nspire_reuse_pictures_forget();
}

static __attribute__((noinline)) u32 picture_new(u16 *dst)
{
  u32 i = picture_next;
  picture_next = (picture_next + 1) % SCREEN_KEYS;
  pictures[i].buf = dst;
  pictures[i].kind = 0;
  return i;
}

/* (the groups not known: made whole, or the other orientation's) */
static __attribute__((noinline)) void picture_kind(u32 i, u32 kind)
{
  memset(pictures[i].key, 0, sizeof(pictures[i].key));
  pictures[i].kind = kind;
}

static __attribute__((noinline)) void picture_whole(u32 i, u16 *dst, const u16 *src, const u16 *prev, int portrait)
{
  pictures[i].kind = 0;
  if (portrait)
    nspire_blend_turn_aspect(dst, src, prev);
  else
    nspire_blend_aspect_landscape(dst, src, prev);
}

void nspire_reuse_picture(u16 *dst, const u16 *src, const u16 *prev, int portrait)
{
  const u32 *ic = nspire_line_ids(src), *ip = prev ? nspire_line_ids(prev) : NULL;
  u32 kind = 1 + !!portrait, i, g, run, run_mixed, fresh;
  u32 (*key)[6];

  if (nspire_os_long_sleeps != os_seen)
    pictures_os_waited();
  for (i = 0; i < SCREEN_KEYS && pictures[i].buf != dst; i++)
    ;
  if (i == SCREEN_KEYS)
    i = picture_new(dst);
  if (!nspire_line_reuse_on || !ic || (prev && !ip) || !nspire_lines_kept_last())
  {
    /* the lines not known, or none kept (every line drawn anew: a new id,
     * in no group's key): all of it, the groups not known after (kind 0:
     * the keys cleared when next used; keeping them cost more than it
     * saved, in every such picture, for a buffer's next picture only) */
    picture_whole(i, dst, src, prev, portrait);
    return;
  }
  if (pictures[i].kind != kind)
    picture_kind(i, kind);
  fresh = pictures[i].fresh;
  pictures[i].fresh = fresh + FRESH >= GROUPS ? 0 : fresh + FRESH;
  key = pictures[i].key;
  run = 0;
  run_mixed = 0;
  for (g = 0; g <= GROUPS; g++)
  {
    u32 lines = g == GROUPS - 1 ? 1 : 3, k[6], l, known = 1, same = 1, make = 0, mixed = 0;
    if (g < GROUPS)
    {
      for (l = 0; l < lines; l++)
      {
        u32 c = ic[3 * g + l], p = prev ? ip[3 * g + l] : c;
        known &= c != 0 && p != 0;
        same &= c == p;
        k[l] = c;
        k[3 + l] = p;
      }
      for (; l < 3; l++)
        k[l] = k[3 + l] = 0;
      mixed = prev && !(known && same);
      if (!mixed)
        for (l = 0; l < 3; l++)
          k[3 + l] = k[l];
      make = !known || g - fresh < FRESH || memcmp(k, key[g], sizeof(k));
      if (make)
        memcpy(key[g], k, sizeof(k));
      if (!known)
        memset(key[g], 0, sizeof(k));
    }
    /* a run of groups to make ends at a group not to make, at one blended
     * the other way, and before the last line */
    if (run && (!make || mixed != run_mixed || g >= GROUPS - 1))
    {
      u32 first = g - run;
      if (portrait)
        nspire_blend_aspect_groups(dst, src, run_mixed ? prev : NULL, first, run);
      else
        nspire_blend_landscape_groups(dst, src, run_mixed ? prev : NULL, first, run);
      run = 0;
    }
    if (g == GROUPS - 1)
    {
      if (make)
      {
        if (portrait)
          nspire_blend_aspect_last(dst, src, mixed ? prev : NULL);
        else
          nspire_blend_landscape_last(dst, src, mixed ? prev : NULL);
      }
      break;
    }
    if (make)
    {
      run++;
      run_mixed = mixed;
    }
  }
}
