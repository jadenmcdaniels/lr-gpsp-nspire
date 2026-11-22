/*
 * gpSP's speed harness: plays a game from a save state the way the
 * calculator does a frame (the core with its ARM dynarec, the renderer, and
 * the picture turned into a screen buffer by the same code the calculator
 * runs), built with the calculator's flags. tests/calc_speed.sh runs it
 * under qemu-arm with the ARM926 model (tools/calc_speed/arm926.c), which
 * counts the cycles of each frame; perf_frame_mark marks the frames.
 *
 *   perf ROM BIOS STATE|- FRAMES KEYS|- DRAW [SETTING=VALUE...]
 *
 * DRAW: 1 draws every frame, 0 none, N one frame in N (fast forward
 * drawing fewer). Settings (default: the reference, 2026-10-14): rombuf=16
 * policy=0 (Partial SMC) scale=1 (scaled/blended 3:2)
 * mix=1 (interframe blending) sprlim=1 (no sprite limit); batch=0 draws
 * each line at its HBlank instead of in batches (video.h). KEYS as in
 * replay.c. Prints a hash of every 50th frame (every=N: every Nth) and of
 * the last one, so a change that should draw the same can be checked with
 * the same run;
 * ppm=DIR also writes the screen as shown at those frames (DIR/sNNNNN.ppm,
 * turned back to landscape); shown=1 prints a hash of the screen buffer
 * last shown at those frames ("shown N HASH", before the frame's line; all
 * 320x240 pixels, either LCD's layout); dump=DIR writes the dynarec's code at the end;
 * flush=N empties the dynarec's caches at the start of frame N (as loading
 * a state does), to see what translating everything again costs;
 * state=PATH writes the machine's state after the last frame.
 */

#include "common.h"
#include "nspire.h"
#include "nspire_gui_video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

extern u32 nspire_rom_buffer_size_choice, nspire_dynarec_ram_policy;
extern u32 nspire_frame_mix_choice, nspire_sprlim_choice, screen_scale;
extern u32 resolution_width, resolution_height;
extern u16 *gba_screen_pixels;
extern u32 execute_cycles;
extern int sprite_limit;
extern char nspire_fps_text[];
extern u16 nspire_fps_color;
void init_gamepak_buffer(void);
void nspire_present_frame(void);
void nspire_video_mix_reset(void);
void nspire_video_frame_start(void);

/* Called at the start of every frame; the model splits its count here. */
void __attribute__((noinline)) perf_frame_mark(void)
{
  __asm__ volatile ("" ::: "memory");
}

struct key_span { int from, to; u32 keys; };
static struct key_span spans[1024];
static int nspans;

static int read_keys(const char *path)
{
  static const char *names[] = { "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L" };
  char line[256];
  FILE *f;

  if (!strcmp(path, "-"))
    return 1;
  f = fopen(path, "r");
  if (!f)
    return 0;
  while (fgets(line, sizeof(line), f) && nspans < 1024)
  {
    char *tok, *save;
    int from, to;
    u32 keys = 0;
    unsigned i;

    if (line[0] == '#' || sscanf(line, "%d %d", &from, &to) != 2)
      continue;
    strtok_r(line, " \t\r\n", &save);
    strtok_r(NULL, " \t\r\n", &save);
    while ((tok = strtok_r(NULL, " \t\r\n", &save)))
    {
      for (i = 0; i < 10 && strcmp(tok, names[i]); i++)
        ;
      if (i == 10)
      {
        fprintf(stderr, "unknown key %s\n", tok);
        fclose(f);
        return 0;
      }
      keys |= 1u << i;
    }
    spans[nspans].from = from;
    spans[nspans].to = to;
    spans[nspans].keys = keys;
    nspans++;
  }
  fclose(f);
  return 1;
}

static u32 keys_at(int frame)
{
  u32 keys = 0;
  int i;
  for (i = 0; i < nspans; i++)
    if (frame >= spans[i].from && frame < spans[i].to)
      keys |= spans[i].keys;
  return keys;
}

/* A state as gpSP writes it (see replay.c). */
static int load_state_file(const char *path)
{
  u8 *file, *raw;
  long size;
  int ok = 0;
  FILE *f = fopen(path, "rb");

  if (!f)
    return 0;
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  file = malloc(size);
  raw = malloc(GBA_STATE_MEM_SIZE);
  if (file && raw && fread(file, 1, size, f) == (size_t)size)
  {
    if (size > 32 && !memcmp(file, "GPSZ", 4))
    {
      u32 packed_size, flags;
      uLongf out = GBA_STATE_MEM_SIZE;
      memcpy(&packed_size, file + 16, 4);
      memcpy(&flags, file + 20, 4);
      if (flags & 2)
        ok = packed_size == GBA_STATE_MEM_SIZE && size >= 32 + GBA_STATE_MEM_SIZE &&
             (memcpy(raw, file + 32, GBA_STATE_MEM_SIZE), 1);
      else
        ok = uncompress(raw, &out, file + 32, packed_size) == Z_OK && out == GBA_STATE_MEM_SIZE;
    }
    else if (size == GBA_STATE_MEM_SIZE)
      ok = (memcpy(raw, file, GBA_STATE_MEM_SIZE), 1);
  }
  fclose(f);
  if (ok)
    ok = gba_load_state(raw);
  free(file);
  free(raw);
  return ok;
}

/* Out of line: the speed model counts it apart (calc_speed.py's
 * 'harness'), not as game time; the calculator has no such work. */
static __attribute__((noinline)) u32 frame_hash(void)
{
  u32 h = 2166136261u;
  const u8 *p = (const u8 *)gba_screen_pixels;
  u32 i;
  for (i = 0; i < 240 * 160 * 2; i++)
    h = (h ^ p[i]) * 16777619u;
  return h;
}

/* The screen buffer last shown, whole (shown=1). */
static __attribute__((noinline)) u32 shown_hash(void)
{
  u32 h = 2166136261u;
  const u8 *p = (const u8 *)nspire_displayed_screen;
  u32 i;
  for (i = 0; i < 320 * 240 * 2; i++)
    h = (h ^ p[i]) * 16777619u;
  return h;
}

/* The screen buffer last shown, turned back: LCD line x, position 239 - y
 * holds landscape pixel (x, y) (nspire_turn.c). */
static void write_screen_ppm(const char *dir, int frame)
{
  const u16 *lcd = (const u16 *)nspire_displayed_screen;
  char path[600];
  FILE *f;
  int x, y;

  snprintf(path, sizeof(path), "%s/s%05d.ppm", dir, frame);
  f = fopen(path, "wb");
  if (!f)
    return;
  fprintf(f, "P6\n320 240\n255\n");
  for (y = 0; y < 240; y++)
    for (x = 0; x < 320; x++)
    {
      u16 c = lcd[x * 240 + 239 - y];
      unsigned char rgb[3] = { (unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2),
                               (unsigned char)((c & 31) << 3) };
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
}

static const char *text_setting(int argc, char **argv, const char *name)
{
  size_t n = strlen(name);
  int i;
  for (i = 7; i < argc; i++)
    if (!strncmp(argv[i], name, n) && argv[i][n] == '=')
      return argv[i] + n + 1;
  return NULL;
}

static int setting(int argc, char **argv, const char *name, int value)
{
  size_t n = strlen(name);
  int i;
  for (i = 7; i < argc; i++)
    if (!strncmp(argv[i], name, n) && argv[i][n] == '=')
      value = atoi(argv[i] + n + 1);
  return value;
}

#ifdef PERF_AUDIO
#include "../audio/perf_audio.h"   /* AUDIO=1: drains the mixer each frame */
#endif

extern u32 rom_cache_watermark, nspire_ram_cache_start;

static int run(int argc, char **argv);

#ifdef NSPIRE_KEEP_TRACE
/* speed10b's trace builds (KEEP_TRACE=1): the RTC's clock stands still, so
 * that two runs' whole states compare (Emerald's holds the seconds). */
#include <time.h>
time_t time(time_t *t)
{
  if (t)
    *t = 1795000000;
  return 1795000000;
}
#endif

/* Layout knobs (the hot code's cache sets against the rest; nothing the
 * game sees changes, nor the instructions run): stack=N moves the stack N
 * bytes down (on the calculator the program sits wherever the OS's malloc
 * put it, so its distance to the stack is anyone's guess), jit=N starts
 * the ROM blocks N bytes further into their cache, ramjit=N the RAM
 * blocks. tests/calc_speed.py --layouts runs a scene at several. */
int main(int argc, char **argv)
{
  /* The stack starts below the command line, so its place in the data
   * cache would follow the length of the file names: put it at the same
   * offset in a 4 KB way every time, less stack=N. */
  char here;
  unsigned want = (0u - ((unsigned)setting(argc, argv, "stack", 0) & ~7u)) & 0xFFFu;
  unsigned pad = ((unsigned)(uintptr_t)&here - want) & 0xFF8u;
  volatile char *below = __builtin_alloca(pad + 8);

  below[0] = 0;
  return run(argc, argv);
}

static __attribute__((noinline)) int run(int argc, char **argv)
{
  int frames, frame, draw_every, hash_every, flush_at, shown;
  const char *ppm_dir;

  if (argc < 7)
  {
    fprintf(stderr, "usage: perf ROM BIOS STATE|- FRAMES KEYS|- DRAW [SETTING=VALUE...]\n");
    return 2;
  }
  frames = atoi(argv[4]);
  draw_every = atoi(argv[6]);
  nspire_rom_buffer_size_choice = setting(argc, argv, "rombuf", 16);
  nspire_dynarec_ram_policy = setting(argc, argv, "policy", 0);
#ifdef NSPIRE_TEST_RAM_PCS
  {
    /* (make ... RAMPCS=1) rampcs=N: the RAM PC table's limit */
    extern u32 nspire_test_ram_pcs;
    nspire_test_ram_pcs = setting(argc, argv, "rampcs", 8192);
  }
#endif
  screen_scale = setting(argc, argv, "scale", scaled_aspect);
  nspire_frame_mix_choice = setting(argc, argv, "mix", 1);
  nspire_sprlim_choice = setting(argc, argv, "sprlim", 1);
  sprite_limit = nspire_sprlim_choice ? 0 : 1;
  ppm_dir = text_setting(argc, argv, "ppm");
  hash_every = setting(argc, argv, "every", 50);
  shown = setting(argc, argv, "shown", 0);
  flush_at = setting(argc, argv, "flush", 0);
  nspire_batch_lines = setting(argc, argv, "batch", 1);
  nspire_lcd_portrait = setting(argc, argv, "portrait", 1);   /* 0: an older CX's landscape LCD */
  /* The calculator's link (nspire_link.c) writes these; never written
   * here, LTO would take them as constants and leave the code that reads
   * them (update_gba's serial updates) out of the harness, which then
   * runs other hot code than the calculator. At run time this writes the
   * values they have (no link). */
  netplay_client_id = setting(argc, argv, "client", 0);
  netplay_num_clients = netplay_client_id ? 0 : setting(argc, argv, "clients", 0);
  cable_linked = setting(argc, argv, "cable", 0);
  /* (2026-11-17, final/speed8) the lines' check for a pad's OS tick (video.h):
   * written by the frontend on the calculator, never here, so LTO would fold
   * the check away; 0 at run time (nothing watched) */
  nspire_os_watching = setting(argc, argv, "oswatch", 0);
  {
    /* speed10: reuse=0 draws every line (nspire_lines.c) */
    extern u32 nspire_line_reuse_on, nspire_reuse_probe;
    nspire_line_reuse_on = setting(argc, argv, "reuse", 1);
    nspire_reuse_probe = setting(argc, argv, "probe", 1);
  }
  {
    extern int perf_os_clobber;   /* (perf_stubs.c: the watch's call trashes r0-r3, r12, the flags) */
    extern uint32_t perf_tt_count;
    perf_os_clobber = setting(argc, argv, "osclobber", 0);
    perf_tt_count = perf_os_clobber ? 1 : 21;
  }
  if (!read_keys(argv[5]))
  {
    fprintf(stderr, "can't read keys %s\n", argv[5]);
    return 2;
  }

  nspire_ram_cache_start = (u32)setting(argc, argv, "ramjit", 0) & ~3u;
#ifdef PERF_AUDIO
  perf_audio_init(argc, argv);
#endif
  init_gamepak_buffer();
  init_sound();
  gba_screen_pixels = calloc(1, GBA_SCREEN_BUFFER_SIZE);
  if (load_bios((char *)argv[2]) != 0)
  {
    fprintf(stderr, "can't read the BIOS %s\n", argv[2]);
    return 2;
  }
  if (load_gamepak(NULL, argv[1], FEAT_AUTODETECT, FEAT_AUTODETECT, SERIAL_MODE_AUTO) != 0)
  {
    fprintf(stderr, "can't load %s\n", argv[1]);
    return 2;
  }
  reset_gba();
#if defined(NSPIRE_KEEP_CHECK) || defined(NSPIRE_KEEP_TRACE)
  {
    /* keepw=N: the kept blocks' store positions from N (2^31 crossed) */
    void nspire_keep_test_w(u32 w);
    unsigned w = (unsigned)setting(argc, argv, "keepw", 0);
    if (w)
      nspire_keep_test_w(w);
  }
#endif
  /* Loading the state empties the caches down to the watermark. */
  if (setting(argc, argv, "jit", 0))
  {
    rom_cache_watermark += (u32)setting(argc, argv, "jit", 0) & ~3u;
    flush_translation_cache_rom();
  }
  memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
  /* sav=PATH: an in-game save (plain or gzipped) as the cartridge's
   * backup, from power-on (to compare with the rival, tests/rival). */
  if (text_setting(argc, argv, "sav"))
  {
    gzFile g = gzopen(text_setting(argc, argv, "sav"), "rb");
    if (!g || gzread(g, gamepak_backup, sizeof(gamepak_backup)) <= 0)
    {
      fprintf(stderr, "can't read the save\n");
      return 2;
    }
    gzclose(g);
  }
  if (strcmp(argv[3], "-") && !load_state_file(argv[3]))
  {
    fprintf(stderr, "can't load the state %s\n", argv[3]);
    return 2;
  }

  /* The picture as the calculator shows it in a game, with the counter. */
  resolution_width = 240;
  resolution_height = 160;
  nspire_video_mix_reset();
  strcpy(nspire_fps_text, "fps: 60/60");   /* the readout as it is drawn (2026-11-12: "fps: " first) */
  nspire_fps_color = 0x07E0;

#ifdef NSPIRE_LOOKUP_COUNT
  /* Model-only (make ... LOOKUP_COUNT=1): block lookups reaching C, a
   * frame, from frame countfrom=N (default 301) to the last. */
  int count_from = setting(argc, argv, "countfrom", 301);
  if (count_from < 1) count_from = 1;
  extern u32 nspire_lookup_count[2][3], nspire_lookup_evict[3];
  u32 count_at[2][3], evict_at[3];
#endif

#ifdef NSPIRE_GBA_PROFILE
  /* Model-only (make ... GBA_PROFILE=1): the GBA instructions run, by PC,
   * from frame proffrom=N (default 31) to the last; the top proftop=N
   * (default 80) a frame, and IWRAM and EWRAM into profdir=DIR. */
  extern u32 nspire_gba_prof_on, nspire_gba_prof[1 << 17][2];
  int prof_from = setting(argc, argv, "proffrom", 31);
#endif

  for (frame = 1; frame <= frames; frame++)
  {
#ifdef NSPIRE_GBA_PROFILE
    if (frame == prof_from)
      nspire_gba_prof_on = 1;
#endif
#ifdef NSPIRE_LOOKUP_COUNT
    if (frame == count_from)
    {
      memcpy(count_at, nspire_lookup_count, sizeof(count_at));
      memcpy(evict_at, nspire_lookup_evict, sizeof(evict_at));
    }
#endif
    perf_frame_mark();
    if (frame == flush_at)
      flush_dynarec_caches();
    skip_next_frame = draw_every == 0 || (frame % draw_every) != 0;
    if (!skip_next_frame)
      nspire_video_frame_start();
    write_ioreg(REG_P1, (~keys_at(frame)) & 0x3FF);
    execute_arm_translate(execute_cycles);
    if (nspire_bad_jump)
    {
      printf("frame %d stopped at %08X\n", frame, (unsigned)nspire_bad_jump_pc);
      return 3;
    }
    if (!skip_next_frame)
      nspire_present_frame();
#ifdef PERF_AUDIO
    perf_audio_frame(frame, frame % hash_every == 0 || frame == frames);
#endif
    if (frame % hash_every == 0 || frame == frames)
    {
      if (shown)
        printf("shown %d %08x\n", frame, (unsigned)shown_hash());
      printf("frame %d %08x\n", frame, (unsigned)frame_hash());
#ifdef NSPIRE_KEEP_TRACE
      {
        /* speed10b's trace build (make ... KEEP_TRACE=1): the RAM cache's
         * blocks made, tags, code and tables as the frame ends */
        void nspire_keep_trace(char *text, u32 size);
        char line[160];
        nspire_keep_trace(line, sizeof(line));
        printf("%s\n", line);
      }
#endif
      if (ppm_dir)
        write_screen_ppm(ppm_dir, frame);
    }
  }
  perf_frame_mark();
#ifdef PERF_AUDIO
  perf_audio_finish();
#endif
#ifdef NSPIRE_LINE_CHECK
  {
    /* speed10's check build (make ... LINE_CHECK=1): every line kept or
     * copied as it was was drawn too, and compared */
    extern u32 nspire_line_check_ok, nspire_line_check_bad, nspire_line_check_copy;
    printf("line check: %u lines kept as drawn (%u of them copied), %u not\n",
           (unsigned)nspire_line_check_ok, (unsigned)nspire_line_check_copy, (unsigned)nspire_line_check_bad);
  }
#endif
#ifdef NSPIRE_IDLE_CHECK
  {
    /* core-16's check build (make ... IDLE_CHECK=1): the events update_gba
     * would have run itself, each checked against the loop's real pass. */
    extern u32 nspire_idle_check_ok, nspire_idle_check_bad;
    extern u32 nspire_idle_pass_check_ok, nspire_idle_pass_check_bad, nspire_idle_pass_check_cost_bad;
    printf("idle check: %u repeats as the body ran, %u not\n",
           (unsigned)nspire_idle_check_ok, (unsigned)nspire_idle_check_bad);
    /* speed9b: Advance Wars' checked idle pass (main.c idle_pass_*) */
    printf("idle pass check: %u repeats as the pass ran, %u not; %u passes of another cost\n",
           (unsigned)nspire_idle_pass_check_ok, (unsigned)nspire_idle_pass_check_bad,
           (unsigned)nspire_idle_pass_check_cost_bad);
  }
#endif
#if defined(NSPIRE_KEEP_CHECK) || defined(NSPIRE_KEEP_TRACE)
  {
    /* speed10b's check and trace builds: ROM flushes that emptied kept
     * blocks, the store started over at 2^31 (keepw=) */
    extern u32 nspire_keep_rom_clears, nspire_keep_w_resets;
    fprintf(stderr, "kept blocks: emptied by a ROM flush %u times, started over at 2^31 %u\n",
            (unsigned)nspire_keep_rom_clears, (unsigned)nspire_keep_w_resets);
  }
#endif
#ifdef NSPIRE_KEEP_CHECK
  {
    /* speed10b's check build (make ... KEEP_CHECK=1): the RAM blocks that
     * would have been copied from the kept ones, translated and compared;
     * the flushes predicted where one wasn't copied (stderr, as below) */
    extern u32 nspire_keep_check_ok, nspire_keep_check_bad,
     nspire_keep_check_flush_ok, nspire_keep_check_flush_bad,
     nspire_keep_check_skipped;
    extern u32 nspire_keep_check_rom;
    fprintf(stderr, "kept-block check: %u blocks as made, %u not; %u predicted flushes"
            " as made, %u not; %u not checked; %u copies linking into the ROM cache\n",
            (unsigned)nspire_keep_check_ok, (unsigned)nspire_keep_check_bad,
            (unsigned)nspire_keep_check_flush_ok, (unsigned)nspire_keep_check_flush_bad,
            (unsigned)nspire_keep_check_skipped, (unsigned)nspire_keep_check_rom);
  }
#endif
#ifdef NSPIRE_PCWORD_CHECK
  {
    /* core-17's check build (make ... PCWORD_CHECK=1): every lookup of a
     * memory access's PC against the word the code still has. */
    extern u32 nspire_pcword_check_ok, nspire_pcword_check_bad, nspire_pcword_check_hit;
    u32 nspire_mem_pc_stats(u32 which);
    /* stderr: calc_speed.py takes stdout's last line as the frame hash */
    fprintf(stderr, "pc word check: %u lookups as the word (%u from the small cache),"
           " %u not, %u not found; table entries ROM %u (kept %u), RAM %u\n",
           (unsigned)nspire_pcword_check_ok, (unsigned)nspire_pcword_check_hit,
           (unsigned)nspire_pcword_check_bad,
           (unsigned)nspire_mem_pc_stats(3), (unsigned)nspire_mem_pc_stats(1),
           (unsigned)nspire_mem_pc_stats(2), (unsigned)nspire_mem_pc_stats(0));
  }
#endif
  {
    /* The dynarec's caches: as gpsp_perf.txt.tns's
     * "dynarec:" line, plus the ROM table's entries; stderr, as above. */
    u32 nspire_mem_pc_stats(u32 which);
    fprintf(stderr, "dynarec: pc lookups not found %u, cache flushes ROM %u RAM %u,"
            " ROM cache %u bytes in use (PC table %u entries); RAM blocks copied %u, kept emptied %u\n",
            (unsigned)nspire_mem_pc_stats(3), (unsigned)nspire_mem_pc_stats(5),
            (unsigned)nspire_mem_pc_stats(4), (unsigned)nspire_mem_pc_stats(6),
            (unsigned)nspire_mem_pc_stats(1), (unsigned)nspire_mem_pc_stats(7),
            (unsigned)nspire_mem_pc_stats(8));
  }

#ifdef NSPIRE_GBA_PROFILE
  {
    static u32 order[1 << 17];
    u32 n = 0, i, j, top = setting(argc, argv, "proftop", 80);
    double total = 0, nf = frames - prof_from + 1;
    const char *dir = text_setting(argc, argv, "profdir");
    for (i = 0; i < (1 << 17); i++)
      if (nspire_gba_prof[i][0])
      {
        order[n++] = i;
        total += nspire_gba_prof[i][1];
      }
    for (i = 1; i < n; i++)   /* by count, most first (insertion, a few thousand) */
    {
      u32 v = order[i];
      for (j = i; j > 0 && nspire_gba_prof[order[j - 1]][1] < nspire_gba_prof[v][1]; j--)
        order[j] = order[j - 1];
      order[j] = v;
    }
    fprintf(stderr, "gba profile: %.0f instructions a frame, %u PCs\n", total / nf, (unsigned)n);
    for (i = 0; i < n && i < top; i++)
      fprintf(stderr, "gba %08x %s %10.1f %5.2f%%\n", (unsigned)(nspire_gba_prof[order[i]][0] & ~1u),
              nspire_gba_prof[order[i]][0] & 1 ? "T" : "A", nspire_gba_prof[order[i]][1] / nf,
              100.0 * nspire_gba_prof[order[i]][1] / total);
    if (dir)
    {
      char path[600];
      FILE *f;
      snprintf(path, sizeof(path), "%s/iwram.bin", dir);
      if ((f = fopen(path, "wb"))) { fwrite(&iwram[0x8000], 1, 0x8000, f); fclose(f); }
      snprintf(path, sizeof(path), "%s/ewram.bin", dir);
      if ((f = fopen(path, "wb"))) { fwrite(ewram, 1, 0x40000, f); fclose(f); }
      snprintf(path, sizeof(path), "%s/prof.txt", dir);
      if ((f = fopen(path, "w")))
      {
        for (i = 0; i < n; i++)
          fprintf(f, "%08x %s %.1f\n", (unsigned)(nspire_gba_prof[order[i]][0] & ~1u),
                  nspire_gba_prof[order[i]][0] & 1 ? "T" : "A", nspire_gba_prof[order[i]][1] / nf);
        fclose(f);
      }
    }
  }
#endif
#ifdef NSPIRE_LOOKUP_COUNT
  if (frames >= count_from)
  {
    int n = frames - count_from + 1, t, k;
    static const char *mode[] = { "arm", "thumb" }, *where[] = { "ROM/BIOS", "IWRAM/EWRAM", "other" };
    printf("block lookups in C a frame, frames %d-%d:", count_from, frames);
    for (t = 0; t < 2; t++)
      for (k = 0; k < 3; k++)
        printf(" %s %s %.1f", mode[t], where[k], (nspire_lookup_count[t][k] - count_at[t][k]) / (double)n);
    {
      extern u32 nspire_lookup_pairs[256][3];
      int i, j, best;
      for (j = 0; j < 12; j++)
      {
        best = -1;
        for (i = 0; i < 256; i++)
          if (nspire_lookup_pairs[i][2] && (best < 0 || nspire_lookup_pairs[i][2] > nspire_lookup_pairs[best][2]))
            best = i;
        if (best < 0)
          break;
        fprintf(stderr, "evicting pair %08x %08x: %u\n", (unsigned)nspire_lookup_pairs[best][0],
                (unsigned)nspire_lookup_pairs[best][1], (unsigned)nspire_lookup_pairs[best][2]);
        nspire_lookup_pairs[best][2] = 0;
      }
    }
    printf("; ROM/BIOS blocks put in the cache: into an empty slot %.1f, over another key %.1f,"
           " over the same key %.1f\n", (nspire_lookup_evict[0] - evict_at[0]) / (double)n,
           (nspire_lookup_evict[1] - evict_at[1]) / (double)n, (nspire_lookup_evict[2] - evict_at[2]) / (double)n);
  }
#endif
  /* state=PATH: the machine's state after the last frame (gba_save_state,
   * raw), to compare two builds byte for byte (a change that must not
   * change the game: core-06). After the frames, so it times nothing. */
  if (text_setting(argc, argv, "state"))
  {
    u8 *raw = calloc(1, GBA_STATE_MEM_SIZE);
    FILE *f = fopen(text_setting(argc, argv, "state"), "wb");
    if (raw && f)
    {
      gba_save_state(raw);
      fwrite(raw, 1, GBA_STATE_MEM_SIZE, f);
    }
    if (f)
      fclose(f);
    free(raw);
  }

  /* dump=DIR: the dynarec's code, to read with objdump (-b binary -m arm
   * --adjust-vma=ADDR, the address printed). */
  if (text_setting(argc, argv, "dump"))
  {
    char path[600];
    FILE *f;
    snprintf(path, sizeof(path), "%s/rom_cache.bin", text_setting(argc, argv, "dump"));
    if ((f = fopen(path, "wb")))
    {
      fwrite(rom_translation_cache, 1, rom_translation_ptr - rom_translation_cache, f);
      fclose(f);
      printf("rom cache at %08x, %u bytes\n", (unsigned)(uintptr_t)rom_translation_cache,
             (unsigned)(rom_translation_ptr - rom_translation_cache));
    }
    snprintf(path, sizeof(path), "%s/ram_cache.bin", text_setting(argc, argv, "dump"));
    if ((f = fopen(path, "wb")))
    {
      fwrite(ram_translation_cache, 1, ram_translation_ptr - ram_translation_cache, f);
      fclose(f);
      printf("ram cache at %08x, %u bytes\n", (unsigned)(uintptr_t)ram_translation_cache,
             (unsigned)(ram_translation_ptr - ram_translation_cache));
    }
  }
  return 0;
}
