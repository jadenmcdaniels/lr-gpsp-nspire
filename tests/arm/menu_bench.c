/*
 * What the menus' own CPU work costs on the calculator, step by step, for
 * the menu speed work (menus and previews, fast): the kit and
 * the screens (nspire_ui.c, nspire_ui_screens.c, compiled as for the
 * calculator), each step between two perf_frame_mark calls so the ARM926
 * model (tools/calc_speed/arm926.c) counts its cycles. The file system's
 * part is tests/libbench.sh's (menu-* paths).
 *
 *   menu_bench STATE.svs.tns
 *
 * STATE: one of this build's states with a picture (GPSZ, packed): its
 * picture is the game's frame (scaled up to 240x160) and the one unpacked.
 * Prints "step NAME" for each step in order; tests/arm/menu_bench.sh runs
 * it under the CX II and CX models and prints the milliseconds.
 */

#include "nspire_ui_screens.h"
#include "nspire_states.h"
#include "nspire_frontend.h"

#include <stdio.h>
#include <string.h>
#include <zlib.h>

u16 nspire_landscape[320 * 240] __attribute__((aligned(32)));
static u16 lcd[320 * 240] __attribute__((aligned(32)));

void __attribute__((noinline)) perf_frame_mark(void)
{
  __asm__ volatile ("" ::: "memory");
}

/* As on the CX II (nspire_video_ui.c), without the flip itself. */
void flip_screen_ui(void)
{
  nspire_turn_rect_565(lcd, nspire_landscape, 320, 0, 0, 320, 240);
}

static void step(const char *name)
{
  perf_frame_mark();
  printf("step %s\n", name);
}

int main(int argc, char **argv)
{
  static const char *const games[] = { "Advance Wars (USA) (Rev 1).gbz.tns",
                                       "Fire Emblem - The Sacred Stones (USA, Australia).gbz.tns",
                                       "Golden Sun (USA, Europe).gbz.tns",
                                       "Legend of Zelda, The - The Minish Cap (USA).gbz.tns",
                                       "Pokemon - Emerald Version (USA, Europe).gbz.tns" };
  static const char *const actions[] = { "C", "Compress", "U", "Uncompress", "del", "Delete", NULL };
  static u16 frame[240 * 160], paused[STATE_PIC_W * STATE_PIC_H], pic[STATE_PIC_W * STATE_PIC_H];
  static u16 small[120 * 80];
  static u8 file[1024 * 1024];
  static struct ui_menu_view mv;
  static struct ui_list_view lv;
  static struct ui_list_details details;
  static struct ui_manager_view sv;
  const u8 *block = NULL;
  u32 block_size = 0;
  uLongf raw_size = sizeof(pic);
  FILE *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
  size_t n = f ? fread(file, 1, sizeof(file), f) : 0;
  int i;

  if (f)
    fclose(f);
  if (n > 32 && !memcmp(file, "GPSZ", 4))
  {
    u32 packed, flags, size;
    memcpy(&packed, file + 16, 4);
    memcpy(&flags, file + 20, 4);
    memcpy(&size, file + 24, 4);
    if ((flags & 4) && 32 + packed + size <= n)
    {
      block = file + 32 + packed;
      block_size = size;
    }
  }
  if (!block || uncompress((Bytef *)pic, &raw_size, block + 8, block_size - 8) != Z_OK)
  {
    printf("menu_bench: %s has no picture\n", argc > 1 ? argv[1] : "(no state)");
    return 1;
  }
  ui_scale(frame, 240, 160, pic, STATE_PIC_W, STATE_PIC_H, STATE_PIC_W);
  ui_defer_present = 1;

  perf_frame_mark();   /* the model counts from here */
  step("menu opens: the backdrop (the game at 320x213)");
  ui_backdrop_make(frame, 240);
  step("menu opens: the paused picture (144x96)");
  ui_scale(paused, STATE_PIC_W, STATE_PIC_H, frame, 240, 160, 240);

  mv.page = -1;
  mv.title = "Pokemon: Emerald";
  mv.status = "";
  for (i = 0; i < 6; i++)
    snprintf(mv.rows[i].label, sizeof(mv.rows[i].label), "%s",
             (const char *[]){ "Resume", "Save", "Load", "Link", "Settings", "Quit" }[i]);
  mv.count = 6;
  mv.picked = 0;
  step("main menu drawn (Resume: the paused game)");
  ui_menu_draw(&mv);
  step("turned onto the LCD (every draw)");
  flip_screen_ui();
  step("main menu drawn again (Resume: a status line changed)");
  mv.status = "Saved to slot 36.";
  ui_menu_draw(&mv);
  mv.status = "";

  mv.picked = 2;
  mv.slot_count = 3;
  for (i = 0; i < 3; i++)
  {
    mv.slots[i].picture = paused;
    snprintf(mv.slots[i].name, sizeof(mv.slots[i].name), "Slot %d", 35 + i);
    snprintf(mv.slots[i].when, sizeof(mv.slots[i].when), "5 min");
    snprintf(mv.slots[i].when_long, sizeof(mv.slots[i].when_long), "5 min ago");
  }
  step("main menu drawn (Load: 3 slots' pictures)");
  ui_menu_draw(&mv);

  mv.picked = 0;
  mv.slot_count = 0;
  mv.other = 1;
  mv.other_count = 4;
  mv.other_picture = paused;
  snprintf(mv.other_title, sizeof(mv.other_title), "Advance Wars");
  snprintf(mv.other_facts, sizeof(mv.other_facts), "Yesterday");
  step("main menu drawn (Resume: another game)");
  ui_menu_draw(&mv);

  step("a state's picture unpacked (zlib)");
  raw_size = sizeof(pic);
  uncompress((Bytef *)pic, &raw_size, block + 8, block_size - 8);

  memcpy(details.picture, pic, sizeof(pic));
  details.ready = details.has_picture = 1;
  details.states = 18;
  details.newest = 35;
  details.bytes = 7097893;
  snprintf(details.facts, sizeof(details.facts), "Yesterday");
  lv.folder = "gba";
  lv.games = 5;
  for (i = 0; i < 5; i++)
  {
    lv.rows[i].file = games[i];
    lv.rows[i].number = i + 1;
    lv.rows[i].picture = pic;
    lv.rows[i].looked_up = 1;
  }
  lv.picked_row = 4;
  lv.marked_row = lv.dir_picked = -1;
  lv.dir_count = 1;
  lv.status = "";
  lv.details = &details;
  step("game list drawn (5 games' pictures)");
  ui_list_draw(&lv);

  step("a picture scaled for a manager (120x80)");
  ui_scale(small, 120, 80, pic, STATE_PIC_W, STATE_PIC_H, STATE_PIC_W);

  sv.title = "Save manager";
  sv.name = "saves";
  snprintf(sv.subtitle, sizeof(sv.subtitle), "19 files, 1.0 MB");
  sv.count = 19;
  for (i = 0; i < UI_MANAGER_ROWS; i++)
  {
    snprintf(sv.rows[i].label, sizeof(sv.rows[i].label), "Slot %d", 20 + i);
    snprintf(sv.rows[i].size, sizeof(sv.rows[i].size), "%d KB", 40 + i);
  }
  sv.picked = 2;
  sv.picture = small;
  snprintf(sv.head, sizeof(sv.head), "Slot 22 " UI_DOT " Yesterday");
  snprintf(sv.facts[0], sizeof(sv.facts[0]), "44 KB");
  snprintf(sv.facts[1], sizeof(sv.facts[1]), "Compressed");
  sv.actions = actions;
  sv.status = "";
  sv.percent = -1;
  step("save manager drawn");
  ui_manager_draw(&sv);
  perf_frame_mark();
  return 0;
}
