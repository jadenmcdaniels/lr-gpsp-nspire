/* PC tests for the parts of the Nspire port that work on the emulator's
 * state and files: compressed ROMs (nspire_gbz.c and the paging in
 * gba_memory.c), zipped ROMs (nspire_zip.c), save state slots, packed or
 * stored (nspire_states.c), and in-game saves, plain or gzipped
 * (nspire_backup.c), with what the save manager does to them. run.sh makes
 * the test files and runs these.
 *
 *   core_tests DIR   DIR holds arm.gba.tns, arm.gbz.tns, big.gba.tns,
 *                    big.gbz.tns, sram.gba.tns and the zips, made by run.sh;
 *                    states and saves go in there too. ROMs are named as on
 *                    the calculator. */

#include "common.h"
#include "savestate.h"
#include "nspire_states.h"
#include "nspire_zip.h"
#include "nspire_frontend.h"
#include "nspire_gbz.h"
#include <malloc.h>
#include "nspire_paths.h"
#include "nspire_rom_convert.h"

#include <utime.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>
#include "../../nspire_fastdeflate.h"
extern int nspire_gbz_test_no_stream;

extern u32 gamepak_buffer_count;
extern u8 *gamepak_buffers[32];

static int failures, checks;
static char dir[512];

#define CHECK(cond, ...)                                      \
  do                                                          \
  {                                                           \
    checks++;                                                 \
    if (!(cond))                                              \
    {                                                         \
      failures++;                                             \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
      printf(__VA_ARGS__);                                    \
      printf("\n");                                           \
    }                                                         \
  } while (0)

extern char unit_documents_dir[600];
void nspire_file_list_untouch(void);

static void path_in_dir(char *out, const char *name)
{
  snprintf(out, 600, "%s/%s", dir, name);
}

/* A file in the saves folder (DIR/zgpSP_files: DIR is the documents here). */
static void path_in_saves(char *out, const char *name)
{
  nspire_save_file(out, 600, name);
}

static u8 *read_file(const char *path, long *size)
{
  FILE *f = fopen(path, "rb");
  u8 *data;
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  *size = ftell(f);
  fseek(f, 0, SEEK_SET);
  data = (u8 *)malloc(*size + 1);
  if (fread(data, 1, *size, f) != (size_t)*size)
  {
    free(data);
    data = NULL;
  }
  fclose(f);
  return data;
}

static long file_size(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static int dir_exists(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int load_rom(const char *name)
{
  char path[600];
  path_in_dir(path, name);
  if (load_gamepak(NULL, path, FEAT_AUTODETECT, FEAT_AUTODETECT, SERIAL_MODE_AUTO) != 0)
    return 0;
  reset_gba();
  return 1;
}

static void run_frames(int frames)
{
  while (frames-- > 0)
    execute_arm(execute_cycles);
}

/* A fingerprint of the machine: registers, RAM, VRAM and the picture. */
static u32 machine_hash(void)
{
  u32 h = crc32(0L, (const Bytef *)reg, sizeof(u32) * 18);
  h = crc32(h, ewram, 256 * 1024);
  h = crc32(h, iwram + 0x8000, 32 * 1024);
  h = crc32(h, vram, sizeof(vram));
  h = crc32(h, (const Bytef *)gba_screen_pixels, GBA_SCREEN_PITCH * GBA_SCREEN_HEIGHT * 2);
  return h;
}

/* Every page of the compressed ROM reads the same as the plain one, with a
 * 2 MB ROM buffer so most of the 8 MB ROM is paged in from the file. */
static void test_gbz_pages(void)
{
  char raw_path[600];
  long raw_size, page;
  u8 *raw;
  u32 plain_size;
  int pass;

  path_in_dir(raw_path, "big.gba.tns");
  raw = read_file(raw_path, &raw_size);
  CHECK(raw != NULL, "big.gba.tns missing");
  if (!raw)
    return;

  /* pass 2 (fast17): the .gbz with the load's read-ahead (gbz_load_ahead),
   * small so that it is read again often, kept on while pages are read in
   * and out of order */
  for (pass = 0; pass < 3; pass++)
  {
    const char *name = pass ? "big.gbz.tns" : "big.gba.tns";
    int bad = 0;
    gbz_load_ahead = pass == 2 ? 3 * GBZ_PAGE_SIZE + 1000 : 0;
    CHECK(load_rom(name), "%s doesn't load", name);
    if (pass == 0)
      plain_size = gamepak_size;
    else
      CHECK(gamepak_size == plain_size, "gbz size %u, plain %u", gamepak_size, plain_size);
    CHECK(gamepak_must_swap(), "%s: ROM should be paged (buffer %u MB)", name, gamepak_buffer_count);

    /* In and out of order, so pages get evicted and read back. */
    for (page = 0; page < (raw_size + 0x7FFF) / 0x8000; page++)
    {
      long p = (page * 37) % ((raw_size + 0x7FFF) / 0x8000);
      long bytes = raw_size - p * 0x8000 < 0x8000 ? raw_size - p * 0x8000 : 0x8000;
      u8 *data = load_gamepak_page((u32)p);
      if (memcmp(data, raw + p * 0x8000, bytes) != 0)
        bad++;
      else if (bytes < 0x8000)
      {
        long i;
        for (i = bytes; i < 0x8000; i++)
          if (data[i])
          {
            bad++;
            break;
          }
      }
    }
    CHECK(bad == 0, "%s%s: %d pages differ", name, pass == 2 ? " (read ahead)" : "", bad);
  }
  gbz_load_ahead_end();
  free(raw);
}

/* fast17: the load's read-ahead is given back when the load is
 * over, also when the ROM is bigger than the ROM buffer (the load stops
 * before the last page): the heap after a load with it is the heap after
 * one without it (the ROM's pages are the buffer's, made before). */
static void test_gbz_ahead_heap(void)
{
  size_t before, plain, ahead;

  CHECK(load_rom("big.gbz.tns"), "big.gbz.tns doesn't load");   /* (the gbz's own state settles) */
  before = mallinfo2().uordblks;
  CHECK(load_rom("big.gbz.tns"), "big.gbz.tns doesn't load");
  gbz_load_ahead_end();
  plain = mallinfo2().uordblks - before;
  CHECK(gamepak_must_swap(), "big.gbz.tns: the ROM is bigger than the buffer here");
  before = mallinfo2().uordblks;
  gbz_load_ahead = 256 * 1024;
  CHECK(load_rom("big.gbz.tns"), "big.gbz.tns doesn't load with the read-ahead");
  gbz_load_ahead_end();
  ahead = mallinfo2().uordblks - before;
  CHECK(ahead == plain, "the heap after a load with the read-ahead: %+ld bytes, without: %+ld",
        (long)ahead, (long)plain);
}

/* The test ROM runs the same from the compressed ROM, and passes. */
static void test_gbz_runs(void)
{
  u32 hash_plain, hash_gbz, result_plain;

  CHECK(load_rom("arm.gba.tns"), "arm.gba.tns doesn't load");
  run_frames(120);
  hash_plain = machine_hash();
  result_plain = reg[12];
  CHECK(load_rom("arm.gbz.tns"), "arm.gbz.tns doesn't load");
  run_frames(120);
  hash_gbz = machine_hash();
  CHECK(hash_plain == hash_gbz, "plain %08x, gbz %08x", hash_plain, hash_gbz);
  nspire_gbz_test_no_stream = 1;
  CHECK(load_rom("arm.gbz.tns"), "gbz loads without a persistent inflate stream");
  run_frames(120);
  nspire_gbz_test_no_stream = 0;
  CHECK(hash_plain == machine_hash(), "per-page inflate fallback preserves execution");
  /* The core's own accuracy, not the port's: just reported. */
  printf("  jsmolka arm test: %s %u\n", result_plain ? "fails at test" : "passes", result_plain);
  CHECK(strcmp(gamepak_code, "") != 0, "game code empty");
}

static void test_game_base(void)
{
  char out[512], want[600];
  nspire_game_name("/documents/gba/Pokemon.gba.tns", out, sizeof(out));
  CHECK(!strcmp(out, "Pokemon.gba"), "%s", out);
  nspire_game_name("/documents/gba/Pokemon.gbz.tns", out, sizeof(out));
  CHECK(!strcmp(out, "Pokemon.gba"), "%s", out);
  nspire_game_name("Zelda.GBZ.tns", out, sizeof(out));
  CHECK(!strcmp(out, "Zelda.gba"), "%s", out);
  nspire_game_name("/a.b/game", out, sizeof(out));
  CHECK(!strcmp(out, "game"), "%s", out);
  /* A USB drive's games have no ".tns": the same names as the calculator's
   * copies, so both share one save and one set of states. */
  nspire_game_name("usb:/GAMES/Pokemon.gba", out, sizeof(out));
  CHECK(!strcmp(out, "Pokemon.gba"), "%s", out);
  nspire_game_name("usb:/Pokemon.GBA", out, sizeof(out));
  CHECK(!strcmp(out, "Pokemon.gba"), "%s", out);
  nspire_game_name("usb:/Pokemon.gbz", out, sizeof(out));
  CHECK(!strcmp(out, "Pokemon.gba"), "%s", out);
  nspire_game_name("usb:/Pokemon.gba.tns", out, sizeof(out));
  CHECK(!strcmp(out, "Pokemon.gba"), "%s", out);
  nspire_game_name("usb:/Old Game.agb", out, sizeof(out));
  CHECK(!strcmp(out, "Old Game.agb"), "%s", out);
  /* The files go in the saves folder; earlier builds had them by the ROM. */
  nspire_game_base("/documents/gba/Pokemon.gbz.tns", out, sizeof(out));
  snprintf(want, sizeof(want), "%s/zgpSP_files/Pokemon.gba", dir);
  CHECK(!strcmp(out, want), "%s", out);
  nspire_game_old_base("/documents/gba/Pokemon.gbz.tns", out, sizeof(out));
  CHECK(!strcmp(out, "/documents/gba/Pokemon.gba"), "%s", out);
}

static void remove_states(void)
{
  char path[600];
  int slot;
  for (slot = 0; slot < STATE_SLOTS; slot++)
  {
    states_file_name(slot, path, sizeof(path));
    remove(path);
  }
}

static void test_states(void)
{
  char rom[600], path[600];
  u32 hash_saved, hash_after;
  long size;
  void *snapshot;

  CHECK(load_rom("arm.gba.tns"), "arm.gba.tns doesn't load");
  path_in_dir(rom, "arm.gba.tns");
  states_open_game(rom);
  remove_states();
  states_open_game(rom);
  CHECK(states_count() == 0, "count %d", states_count());
  CHECK(states_newest() == -1, "newest %d", states_newest());
  CHECK(states_next_new_slot() == 0, "next %d", states_next_new_slot());
  CHECK(states_load(3) == 0, "empty slot should load as 0");

  run_frames(30);
  CHECK(states_save(0) == 1, "save 0");
  states_file_name(0, path, sizeof(path));
  size = file_size(path);
  CHECK(size > 0 && size < 100 * 1024, "slot 0 is %ld bytes", size);
  printf("  a state is %ld bytes (416 KB raw)\n", size);
  run_frames(1);
  hash_saved = machine_hash();
  run_frames(40);
  CHECK(states_load(0) == 1, "load 0");
  run_frames(1);
  hash_after = machine_hash();
  CHECK(hash_saved == hash_after, "after loading: %08x, saved %08x", hash_after, hash_saved);

  /* Order and auto-increment. */
  CHECK(states_save(5) == 1, "save 5");
  CHECK(states_save(2) == 1, "save 2");
  CHECK(states_newest() == 2, "newest %d", states_newest());
  CHECK(states_next_new_slot() == 6, "next %d", states_next_new_slot());
  states_open_game(rom);   /* the order survives a rescan */
  CHECK(states_count() == 3, "count %d", states_count());
  CHECK(states_newest() == 2, "newest after rescan %d", states_newest());
  CHECK(states_exists(5) && !states_exists(4), "exists");

  /* SAV-2: a new slot is written through a .tmp (a reset mid-write left a
   * partial state with the newest counter); a partial file never counts as
   * the newest one. */
  {
    char tmp[620];
    FILE *f;
    long cut;

    states_file_name(21, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    mkdir(tmp, 0755);   /* the .tmp can't be made */
    CHECK(states_save(21) == 0 && file_size(path) < 0 && !states_exists(21),
          "a new slot isn't written under its own name");
    rmdir(tmp);
    CHECK(states_save(22) == 1, "save 22");
    states_file_name(22, path, sizeof(path));
    cut = file_size(path) / 2;
    f = fopen(path, "r+b");
    CHECK(f && truncate(path, cut) == 0, "cut slot 22");
    if (f)
      fclose(f);
    states_open_game(rom);
    CHECK(states_exists(22) && states_newest() != 22, "a partial slot isn't the newest: %d", states_newest());
    remove(path);
    states_open_game(rom);
  }

  /* A full set wraps to the first free slot. */
  CHECK(states_save(999) == 1, "save 999");
  CHECK(states_next_new_slot() == 1, "next after 999: %d", states_next_new_slot());

  /* gpSP's older states (the raw 416 KB) still load. */
  {
    u8 *raw = (u8 *)calloc(1, GBA_STATE_MEM_SIZE);
    FILE *f;
    run_frames(10);
    gba_save_state(raw);
    run_frames(1);
    hash_saved = machine_hash();
    states_file_name(7, path, sizeof(path));
    f = fopen(path, "wb");
    fwrite(raw, 1, GBA_STATE_MEM_SIZE, f);
    fclose(f);
    free(raw);
    states_open_game(rom);
    CHECK(states_exists(7), "legacy state not found");
    CHECK(states_newest() == 999, "a legacy state counts as old: newest %d", states_newest());
    run_frames(25);
    CHECK(states_load(7) == 1, "legacy load");
    run_frames(1);
    CHECK(machine_hash() == hash_saved, "legacy state differs after load");
  }

  /* A damaged file fails cleanly. */
  {
    FILE *f;
    states_file_name(8, path, sizeof(path));
    f = fopen(path, "wb");
    fputs("GPSZ garbage", f);
    fclose(f);
    states_open_game(rom);
    CHECK(states_load(8) == -1, "damaged state should fail");
    f = fopen(path, "wb");
    fputs("not a state", f);
    fclose(f);
    CHECK(states_load(8) == -1, "foreign file should fail");
  }

  /* Names: unpadded numbers only, like gpSP's. */
  {
    FILE *f;
    char name[600];
    snprintf(name, sizeof(name), "%s/arm.gba012.svs.tns", dir);
    f = fopen(name, "wb");
    fclose(f);
    snprintf(name, sizeof(name), "%s/arm.gba1000.svs.tns", dir);
    f = fopen(name, "wb");
    fclose(f);
    states_open_game(rom);
    CHECK(!states_exists(12), "012 shouldn't be a slot");
    CHECK(states_count() == 6, "count %d", states_count());
  }

  /* The compressed ROM shares the plain one's states. */
  path_in_dir(rom, "arm.gbz.tns");
  states_open_game(rom);
  CHECK(states_exists(0) && states_exists(999), "gbz ROM sees the gba's states");

  snapshot = states_snapshot();
  run_frames(1);
  hash_saved = machine_hash();
  run_frames(20);
  CHECK(states_restore(snapshot), "restore snapshot");
  run_frames(1);
  CHECK(machine_hash() == hash_saved, "snapshot differs");
  free(snapshot);

  /* Compress saves off: states stored as they are, behind the header. */
  path_in_dir(rom, "arm.gba.tns");
  states_open_game(rom);
  {
    long bytes;
    int compressed, newest;

    nspire_compress_saves = 0;
    run_frames(5);
    CHECK(states_save(3) == 1, "save 3 stored");
    nspire_compress_saves = 1;
    states_file_name(3, path, sizeof(path));
    /* the state, then its picture (packed, a few KB) */
    CHECK(file_size(path) > 32 + GBA_STATE_MEM_SIZE && file_size(path) < 32 + GBA_STATE_MEM_SIZE + 32 * 1024,
          "a stored state is %ld bytes", file_size(path));
    {
      static u16 pic[STATE_PIC_W * STATE_PIC_H];
      CHECK(states_picture(3, pic), "the stored state has a picture");
    }
    CHECK(states_file_info(3, &bytes, &compressed) && bytes == file_size(path) && !compressed,
          "slot 3: %ld bytes, compressed %d", bytes, compressed);
    run_frames(1);
    hash_saved = machine_hash();
    run_frames(30);
    CHECK(states_load(3) == 1, "load the stored state");
    run_frames(1);
    CHECK(machine_hash() == hash_saved, "stored state differs after load");
    newest = states_newest();
    CHECK(newest == 3, "the stored state is the newest: %d", newest);

    /* The save manager packs and unpacks them, keeping the order. */
    CHECK(states_set_compressed(3, 1) == 1, "compress slot 3");
    CHECK(states_file_info(3, &bytes, &compressed) && compressed && bytes < 100 * 1024,
          "slot 3 packed: %ld bytes, compressed %d", bytes, compressed);
    CHECK(states_newest() == 3, "still the newest: %d", states_newest());
    run_frames(30);
    CHECK(states_load(3) == 1, "load it packed");
    run_frames(1);
    CHECK(machine_hash() == hash_saved, "packed state differs after load");
    CHECK(states_set_compressed(3, 0) == 1 && file_size(path) > 32 + GBA_STATE_MEM_SIZE, "uncompress slot 3");
    {
      static u16 pic[STATE_PIC_W * STATE_PIC_H];
      CHECK(states_picture(3, pic), "the picture survives packing and unpacking");
    }
    CHECK(states_set_compressed(3, 0) == 1, "uncompressing it again is fine");
    states_open_game(rom);
    CHECK(states_newest() == 3, "the order survives: newest %d", states_newest());

    /* An older build's state gets the header, and stays older. */
    CHECK(states_set_compressed(7, 0) == 1, "an older state is stored already");
    CHECK(states_set_compressed(7, 1) == 1, "compress the older state");
    CHECK(states_file_info(7, &bytes, &compressed) && compressed, "the older state packed");
    states_open_game(rom);
    CHECK(states_newest() == 3, "the older state stays older: newest %d", states_newest());
    run_frames(10);
    CHECK(states_load(7) == 1, "the older state loads packed");

    CHECK(states_delete(3) == 1 && !states_exists(3) && file_size(path) < 0, "delete slot 3");
    CHECK(states_delete(3) == 0, "deleting an empty slot");
    CHECK(states_newest() != 3, "a deleted slot isn't the newest");
  }

  /* R6: a reset between removing the old state and the rename left only
   * the new one's .tmp: the slot is that one at the next listing; a .tmp
   * next to its state (a write that didn't finish) is dropped, and so is a
   * lone one that isn't whole. */
  {
    char tmp[620];
    long whole;
    FILE *f;

    run_frames(5);
    CHECK(states_save(5) == 1, "save 5 again");
    states_file_name(5, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    whole = file_size(path);
    rename(path, tmp);
    states_open_game(rom);
    CHECK(states_exists(5) && file_size(path) == whole && file_size(tmp) < 0,
          "a lone whole .tmp is the slot's state: %ld", file_size(path));
    run_frames(1);
    hash_saved = machine_hash();
    run_frames(20);
    CHECK(states_load(5) == 1, "it loads");
    f = fopen(tmp, "wb");
    fputs("half a state", f);
    fclose(f);
    states_open_game(rom);
    CHECK(states_exists(5) && file_size(path) == whole && file_size(tmp) < 0, "a .tmp beside its state is dropped");
    rename(path, tmp);
    CHECK(truncate(tmp, whole / 2) == 0, "cut");
    states_open_game(rom);
    CHECK(!states_exists(5) && file_size(tmp) < 0, "a lone partial .tmp is dropped, the slot empty");
  }

  remove_states();
}

/* The menus' look-ups (states_peek_soon, states_picture_soon): what was
 * read is kept and answers with no files; a key (states_yield) stops a
 * game's slots being read after one, and the next call goes on; files moved
 * (nspire_file_list_touched) drop it; a game's start takes the kept slots
 * (states_open_game_kept), the open game's own as they are now. */
static int yields;
static void copy_file(const char *from, const char *to);

static int yield_now(void)
{
  yields++;
  return 1;
}

/* r19-snd: a state saved over the game's frames (states_save_begin, then
 * states_save_step(1) a part a frame as save_job_frame: the take, the
 * packing, the file's create, its write in 8 KB parts, its close, the
 * replace's steps) is the same file as one saved at once, but its header's
 * count and time; stopped between its parts (the battery out: nothing but
 * the files on the calculator kept), the slot's old state is what loads. */
static void test_states_stepwise(void)
{
  char rom[600], path_a[600], path_b[600], tmp[620];
  long size_a, size_b, size_old;
  u8 *a, *b, *old;
  int r, parts = 0, os_parts = 0;
  u32 hash_old;

  CHECK(load_rom("arm.gba.tns"), "arm.gba.tns doesn't load");
  path_in_dir(rom, "arm.gba.tns");
  states_open_game(rom);
  remove_states();
  states_open_game(rom);
  run_frames(30);
  CHECK(states_save(0) == 1, "stepwise: save 0 at once");
  CHECK(states_save_begin(1) == 1, "stepwise: save 1 begun");
  while ((r = states_save_step(1)) == STATES_SAVE_BUSY)
  {
    parts++;
    if (states_save_next_is_file())
      os_parts++;
  }
  CHECK(r == STATES_SAVE_DONE, "stepwise: save 1 done (%d)", r);
  states_file_name(0, path_a, sizeof(path_a));
  states_file_name(1, path_b, sizeof(path_b));
  a = read_file(path_a, &size_a);
  b = read_file(path_b, &size_b);
  /* (the header, 32 bytes: magic, version, the count, sizes, flags, the picture's size, the time) */
  CHECK(a && b && size_a == size_b && size_a > 32, "stepwise: sizes %ld and %ld", size_a, size_b);
  if (a && b && size_a == size_b && size_a > 32)
    CHECK(!memcmp(a, b, 8) && !memcmp(a + 12, b + 12, 16) && !memcmp(a + 32, b + 32, (size_t)size_a - 32),
          "stepwise: the same bytes but the count and the time");
  printf("  a state saved over %d parts (%d of them OS calls), %ld bytes\n", parts, os_parts, size_b);
  CHECK(os_parts >= 3 + (int)(size_b / 8192), "stepwise: the write in 8 KB parts (%d OS parts)", os_parts);
  free(a);
  free(b);

  /* the battery out mid-job: the old state of slot 1 stays */
  old = read_file(path_b, &size_old);
  hash_old = 0;
  run_frames(1);
  CHECK(states_load(1) == 1, "stepwise: slot 1 loads");
  run_frames(1);
  hash_old = machine_hash();
  run_frames(20);
  for (int stop = 1; stop < parts; stop += stop < parts - 9 ? 7 : 1)
  {
    int k;
    CHECK(states_save_begin(1) == 1, "stepwise: begun again");
    for (k = 0; k < stop && states_save_step(1) == STATES_SAVE_BUSY; k++)
      ;
    states_drop_job_for_test();   /* (nothing finished: the files as a reset leaves them) */
    states_open_game(rom);        /* (gpSP again: the folder read, the slot's file recovered) */
    snprintf(tmp, sizeof(tmp), "%s.tmp", path_b);
    {
      long sz;
      u8 *now = read_file(path_b, &sz);
      int same = now && old && sz == size_old && !memcmp(now, old, (size_t)sz);
      u8 *fresh = NULL;
      /* (the old state, or, stopped after the replace's last needed step, the new one whole) */
      if (!same && now)
        CHECK(states_load(1) == 1, "stepwise: stopped after %d parts: slot 1 still loads", stop);
      else
        CHECK(same, "stepwise: stopped after %d parts: the old state kept", stop);
      free(now);
      free(fresh);
    }
    remove(tmp);
  }
  CHECK(states_load(1) == 1, "stepwise: slot 1 loads at the end");
  free(old);
  (void)hash_old;

  /* stored (Compress saves off): 416 KB, the write in 52 parts, the same bytes */
  nspire_compress_saves = 0;
  CHECK(states_save(2) == 1, "stepwise: save 2 at once, stored");
  CHECK(states_save_begin(3) == 1, "stepwise: save 3 begun, stored");
  os_parts = 0;
  while ((r = states_save_step(1)) == STATES_SAVE_BUSY)
    os_parts += states_save_next_is_file();
  CHECK(r == STATES_SAVE_DONE, "stepwise: save 3 done");
  states_file_name(2, path_a, sizeof(path_a));
  states_file_name(3, path_b, sizeof(path_b));
  a = read_file(path_a, &size_a);
  b = read_file(path_b, &size_b);
  CHECK(a && b && size_a == size_b && size_a > 400 * 1024, "stepwise stored: sizes %ld and %ld", size_a, size_b);
  if (a && b && size_a == size_b && size_a > 32)
    CHECK(!memcmp(a, b, 8) && !memcmp(a + 12, b + 12, 16) && !memcmp(a + 32, b + 32, (size_t)size_a - 32),
          "stepwise stored: the same bytes but the count and the time");
  CHECK(os_parts >= (int)(size_b / 8192), "stepwise stored: the write in 8 KB parts (%d OS parts)", os_parts);
  free(a);
  free(b);
  nspire_compress_saves = 1;
  /* (r19-bank) the timing file's line of a save in a game: its OS calls, the longest, the bank */
  {
    char tpath[600];
    long tsize;
    u8 *text;
    states_timing_flush();
    path_in_saves(tpath, "gpsp_states_time.txt.tns");
    text = read_file(tpath, &tsize);
    if (text)
      text[tsize] = 0;   /* (read_file leaves a byte for it) */
    CHECK(text && strstr((char *)text, " OS calls, the longest ") && strstr((char *)text, "; bank 0 ms (learned 0)"),
          "stepwise: the timing line says the calls and the bank");
    free(text);
  }
}

/* the state is taken at a frame's end after the key; the game changed in
 * between (Restart: nspire_host_main.c ACTION_RESTART; a bad jump: game_stopped) must not be what goes
 * into the slot. Key then restart: the game before the restart; key then a bad jump: the slot's old
 * state; a state taken before the bad jump: that one. */
static int slot_bytes_are(int slot, const u8 *bytes, long size)
{
  char path[600];
  long sz;
  u8 *now;
  int same;
  states_file_name(slot, path, sizeof(path));
  now = read_file(path, &sz);
  same = now && bytes && sz == size && !memcmp(now, bytes, (size_t)size);
  free(now);
  return same;
}

static int job_frames(void)
{
  int r, n = 0;
  do
  {
    run_frames(1);
    r = states_save_step(1);
    n++;
  } while (r == STATES_SAVE_BUSY && n < 2000);
  return r;
}

/* a mark in the game's memory where arm.gba never writes (its last KB of EWRAM: a reset clears it) */
#define MARK_AT (256 * 1024 - 1024)
static void mark_game(int v)
{
  memset(ewram + MARK_AT, v, 1024);
}

static int marked(int v)
{
  for (int i = 0; i < 1024; i++)
    if (ewram[MARK_AT + i] != (u8)v)
      return 0;
  return 1;
}

static void test_states_changes(void)
{
  char rom[600], path[600];
  long old_size;
  u8 *old;
  int slot, r;

  CHECK(load_rom("arm.gba.tns"), "arm.gba.tns doesn't load");
  path_in_dir(rom, "arm.gba.tns");
  states_open_game(rom);
  remove_states();
  states_open_game(rom);
  run_frames(30);
  while (states_save_result(&slot) != STATES_SAVE_IDLE)
    ;   /* (the tests before: theirs, never read) */

  /* the key, two frames, Restart (states_save_before_change, then reset_gba, as do_hotkeys) */
  mark_game(0x11);
  CHECK(states_save(1) == 1, "changes: slot 1's old state");
  run_frames(20);
  mark_game(0x22);
  CHECK(states_save_begin(1) == 1 && states_save_next_is_take(), "changes: the key (the take to come)");
  run_frames(2);
  states_save_before_change(STATES_CHANGE_KEEP);
  reset_gba();
  CHECK(!marked(0x22), "changes: the restart cleared the game's memory");
  r = job_frames();
  CHECK(r == STATES_SAVE_DONE, "changes: the save after the restart done (%d)", r);
  CHECK(states_save_result(&slot) == STATES_SAVE_DONE && slot == 1, "changes: its result, slot %d", slot + 1);
  run_frames(5);
  CHECK(states_load(1) == 1, "changes: slot 1 loads");
  CHECK(marked(0x22), "changes: key then restart: the slot has the game before the restart");

  /* the key, a frame, a bad jump (game_stopped: before_change BROKEN, then the rest finished): the old state */
  run_frames(10);
  mark_game(0x33);
  CHECK(states_save(2) == 1, "changes: slot 2's old state");
  states_file_name(2, path, sizeof(path));
  old = read_file(path, &old_size);
  run_frames(10);
  mark_game(0x44);
  CHECK(states_save_begin(2) == 1, "changes: the key for slot 2");
  run_frames(1);
  mark_game(0xA5);   /* (the game in pieces) */
  states_save_before_change(STATES_CHANGE_BROKEN);
  states_save_finish();
  CHECK(!states_save_busy(), "changes: no save left after the bad jump");
  CHECK(slot_bytes_are(2, old, old_size), "changes: key then bad jump: slot 2 keeps its old state");
  CHECK(states_save_result(&slot) == STATES_SAVE_IDLE, "changes: the dropped save says nothing");
  free(old);
  CHECK(states_load(2) == 1 && marked(0x33), "changes: slot 2 loads its old state");

  /* a state taken before the bad jump: written, the game as it was at the take */
  run_frames(10);
  mark_game(0x55);
  CHECK(states_save_begin(3) == 1, "changes: the key for slot 3");
  run_frames(1);
  CHECK(states_save_step(1) == STATES_SAVE_BUSY && !states_save_next_is_take(), "changes: slot 3's state taken");
  run_frames(1);
  mark_game(0x5A);
  states_save_before_change(STATES_CHANGE_BROKEN);
  states_save_finish();
  CHECK(states_save_result(&slot) == STATES_SAVE_DONE && slot == 3, "changes: slot 3 written");
  CHECK(states_load(3) == 1 && marked(0x55), "changes: taken before the bad jump: the game as it was then");
  remove_states();
}

/* a second save asked for in a game while one is under way (the key twice,
 * the key during an autosave) is queued, not the first one finished at once with the interrupts masked:
 * both whole, in their order, each the game at its take; the queue written at once by whatever finishes
 * saves (the quit: states_save_finish); a slot asked for twice before its take is one save. */
static int slot_file_exists(int slot)
{
  char path[600];
  states_file_name(slot, path, sizeof(path));
  return file_size(path) > 0;
}

/* frames with the job's parts at their ends until none is left; the results in their order */
static int queue_frames(int *slots, int *tags, int most)
{
  int n = 0, slot, r, guard = 0;
  while (states_save_busy() && guard++ < 4000)
  {
    run_frames(1);
    states_save_step(1);
    while ((r = states_save_result(&slot)) != STATES_SAVE_IDLE && n < most)
    {
      tags[n] = states_save_result_tag();
      slots[n++] = r == STATES_SAVE_DONE ? slot : -1 - slot;
    }
  }
  while ((r = states_save_result(&slot)) != STATES_SAVE_IDLE && n < most)
  {
    tags[n] = states_save_result_tag();
    slots[n++] = r == STATES_SAVE_DONE ? slot : -1 - slot;
  }
  return n;
}

static void test_states_queue(void)
{
  char rom[600];
  int slots[8], tags[8], n;

  CHECK(load_rom("arm.gba.tns"), "arm.gba.tns doesn't load");
  path_in_dir(rom, "arm.gba.tns");
  states_open_game(rom);
  remove_states();
  states_open_game(rom);
  run_frames(30);
  n = queue_frames(slots, tags, 8);   /* (the tests before: their results, never read) */

  /* the key, its take, the key again (an autosave's tag): the first goes on, the second after it */
  mark_game(0x61);
  CHECK(states_save_begin(4) == 1, "queue: save 4 begun");
  run_frames(1);
  CHECK(states_save_step(1) == STATES_SAVE_BUSY && !states_save_next_is_take(), "queue: 4's state taken");
  run_frames(1);
  mark_game(0x62);
  CHECK(states_save_begin_as(5, 1) == 1, "queue: save 5 asked for");
  CHECK(!slot_file_exists(4) && states_save_busy(), "queue: save 4 not finished at once for it");
  CHECK(states_next_new_slot() == 6, "queue: the next new slot after the ones under way: %d", states_next_new_slot());
  CHECK(states_exists_soon(5) && !states_exists(9), "queue: slot 5 known as saved");
  n = queue_frames(slots, tags, 8);
  CHECK(n == 2 && slots[0] == 4 && slots[1] == 5 && tags[0] == 0 && tags[1] == 1,
        "queue: both written, in order (%d: %d/%d %d/%d)", n, slots[0], tags[0], n > 1 ? slots[1] : 99, tags[1]);
  CHECK(states_load(4) == 1 && marked(0x61), "queue: slot 4 the game at its take");
  CHECK(states_load(5) == 1 && marked(0x62), "queue: slot 5 the game at its own take");

  /* the quit in between: both written at once, in order */
  run_frames(5);
  mark_game(0x63);
  CHECK(states_save_begin(6) == 1, "queue: save 6 begun");
  run_frames(1);
  states_save_step(1);
  mark_game(0x64);
  CHECK(states_save_begin(7) == 1 && !slot_file_exists(6), "queue: save 7 asked for, 6 under way");
  CHECK(states_save_finish() == STATES_SAVE_DONE && !states_save_busy(), "queue: the quit's finish");
  n = queue_frames(slots, tags, 8);
  CHECK(n == 2 && slots[0] == 6 && slots[1] == 7, "queue: quit in between: both written (%d)", n);
  CHECK(states_load(6) == 1 && marked(0x63), "queue: slot 6 whole");
  CHECK(states_load(7) == 1 && marked(0x64), "queue: slot 7 whole");

  /* the same slot twice before its take: one save, the game at its take */
  run_frames(5);
  mark_game(0x65);
  CHECK(states_save_begin(8) == 1 && states_save_begin(8) == 1, "queue: slot 8 asked for twice");
  n = queue_frames(slots, tags, 8);
  CHECK(n == 1 && slots[0] == 8, "queue: one save of slot 8 (%d)", n);
  CHECK(states_load(8) == 1 && marked(0x65), "queue: slot 8 loads");

  /* a save queued behind one, then Restart: both now, each the game before the restart */
  run_frames(5);
  mark_game(0x71);
  CHECK(states_save_begin(9) == 1, "queue: save 9 begun");
  run_frames(1);
  states_save_step(1);
  mark_game(0x72);
  CHECK(states_save_begin(10) == 1, "queue: save 10 asked for");
  states_save_before_change(STATES_CHANGE_KEEP);
  CHECK(!states_save_busy(), "queue: Restart with a save queued: all written now");
  reset_gba();
  run_frames(5);
  n = queue_frames(slots, tags, 8);
  CHECK(n == 2 && slots[0] == 9 && slots[1] == 10, "queue: restart: both written in order (%d)", n);
  CHECK(states_load(9) == 1 && marked(0x71), "queue: restart: slot 9 the game at its take");
  CHECK(states_load(10) == 1 && marked(0x72), "queue: restart: slot 10 the game before the restart");
  remove_states();
}

/* A load key between two saves keyed back to back (after the first one's last
 * part, before the second's take: states_load's settle() looked at sj.step only) loaded first, and the queued
 * save then took the game just loaded into its slot; now the load finishes the queued save first, the game as
 * it was then (the queue's design: when the save before it is written). 5. a slot asked for again before its
 * take keeps the later request's word ("Saved" after an autosave), under way or queued. */
static int step_to_done(void)
{
  int r = STATES_SAVE_BUSY, guard = 0;
  while (r == STATES_SAVE_BUSY && guard++ < 4000)
  {
    run_frames(1);
    r = states_save_step(1);
  }
  return r;
}

static void test_review2_saves(void)
{
  char rom[600];
  int slots[8], tags[8], n;

  CHECK(load_rom("arm.gba.tns"), "arm.gba.tns doesn't load");
  path_in_dir(rom, "arm.gba.tns");
  states_open_game(rom);
  remove_states();
  states_open_game(rom);
  run_frames(30);
  n = queue_frames(slots, tags, 8);   /* (the tests before: their results) */

  /* the state a load key loads (slot 20), and slot 23's old state */
  mark_game(0x81);
  CHECK(states_save(20) == 1, "key queue: slot 20, the one the load key loads");
  run_frames(5);
  mark_game(0x84);
  CHECK(states_save(23) == 1, "key queue: slot 23's old state");
  run_frames(5);

  /* new slots: the key (21), its take, the key again (22); 21 written; the load key; then 22 */
  mark_game(0x82);
  CHECK(states_save_begin(21) == 1, "key queue: the key for 21");
  run_frames(1);
  CHECK(states_save_step(1) == STATES_SAVE_BUSY && !states_save_next_is_take(), "key queue: 21's state taken");
  run_frames(1);
  mark_game(0x83);
  CHECK(states_save_begin(22) == 1, "key queue: the key again, 22 queued");
  CHECK(step_to_done() == STATES_SAVE_DONE && states_save_busy() && states_save_next_is_take(),
        "key queue: 21 written, 22's take still to come");
  CHECK(states_load(20) == 1 && marked(0x81), "key queue: the load key: slot 20 loaded");
  n = queue_frames(slots, tags, 8);
  CHECK(n == 2 && slots[0] == 21 && slots[1] == 22, "key queue: both written, in order (%d)", n);
  CHECK(states_load(21) == 1 && marked(0x82), "key queue: slot 21 the game at its key");
  CHECK(states_load(22) == 1 && marked(0x83),
        "key queue: slot 22 the game at its key (when 21 was written), not the loaded one");

  /* a fixed slot: 23 keyed twice, the second after the first's take; the load key between */
  run_frames(5);
  mark_game(0x85);
  CHECK(states_save_begin(23) == 1, "key queue: the key for 23");
  run_frames(1);
  CHECK(states_save_step(1) == STATES_SAVE_BUSY && !states_save_next_is_take(), "key queue: 23's state taken");
  run_frames(1);
  mark_game(0x86);
  CHECK(states_save_begin(23) == 1, "key queue: the key for 23 again, queued");
  CHECK(step_to_done() == STATES_SAVE_DONE && states_save_busy(), "key queue: 23 written once, the second to come");
  CHECK(states_load(20) == 1 && marked(0x81), "key queue: the load key again");
  n = queue_frames(slots, tags, 8);
  CHECK(n == 2 && slots[0] == 23 && slots[1] == 23, "key queue: 23 written twice (%d)", n);
  CHECK(states_load(23) == 1 && marked(0x86), "key queue: fixed slot 23 the game at the second key, not the loaded one");

  /* 5: the word of the later request: an autosave, then the key, before its take; and queued */
  run_frames(5);
  CHECK(states_save_begin_as(30, 1) == 1 && states_save_begin_as(30, 0) == 1, "key queue: 30 autosaved, then keyed");
  n = queue_frames(slots, tags, 8);
  CHECK(n == 1 && slots[0] == 30 && tags[0] == 0, "key queue: one save of 30, \"Saved\" (%d, tag %d)", n, tags[0]);
  run_frames(5);
  CHECK(states_save_begin(31) == 1, "key queue: the key for 31");
  run_frames(1);
  states_save_step(1);
  CHECK(states_save_begin_as(32, 1) == 1 && states_save_begin_as(32, 0) == 1, "key queue: 32 autosaved, then keyed, queued");
  n = queue_frames(slots, tags, 8);
  CHECK(n == 2 && slots[1] == 32 && tags[1] == 0, "key queue: the queued 32 says \"Saved\" (%d, tag %d)", n, tags[1]);
  remove_states();
}

static void test_states_kept(void)
{
  static u16 pic[STATE_PIC_W * STATE_PIC_H], pic2[STATE_PIC_W * STATE_PIC_H];
  char rom[600], other[600], from[600], to[600];
  int count = 0, newest = -1, has, slot, calls;
  long when, bytes;
  int compressed;

  CHECK(load_rom("arm.gba.tns"), "arm.gba.tns doesn't load");
  path_in_dir(rom, "arm.gba.tns");
  path_in_dir(other, "kept.gba.tns");   /* a game with no ROM: only its states are looked at */
  states_open_game(rom);
  remove_states();
  states_open_game(rom);
  for (slot = 3; slot <= 6; slot++)
  {
    run_frames(3);
    CHECK(states_save(slot) == 1, "save %d", slot);
    char name[64];
    states_file_name(slot, from, sizeof(from));
    snprintf(name, sizeof(name), "kept.gba%d.svs.tns", slot);
    path_in_saves(to, name);
    copy_file(from, to);
  }
  CHECK(states_file_info(5, &bytes, &compressed) == 1 && compressed, "slot 5's info from memory");
  states_file_name(5, from, sizeof(from));
  CHECK(bytes == file_size(from), "slot 5: %ld bytes, the file %ld", bytes, file_size(from));
  CHECK(states_picture_soon(6, pic, 0) == 1, "a state just saved has its picture kept");

  CHECK(states_peek_soon(other, &count, &newest, pic2, &when, 0) == -1, "another game: nothing kept yet");
  states_yield = yield_now;
  for (calls = 1; (has = states_peek_soon(other, &count, &newest, pic2, &when, 1)) < 0 && calls < 10; calls++)
    ;
  states_yield = NULL;
  CHECK(calls == 4 && yields == 3, "a key stops the read after each slot: %d calls, %d yields", calls, yields);
  CHECK(has == 1 && count == 4 && newest == 6, "the other game: picture %d, %d states, newest %d", has, count,
        newest);
  CHECK(!memcmp(pic, pic2, sizeof(pic)), "its newest's picture is the file's");
  memset(pic2, 0, sizeof(pic2));
  CHECK(states_peek_soon(other, &count, &newest, pic2, &when, 0) == 1 && count == 4 && newest == 6 &&
            !memcmp(pic, pic2, sizeof(pic)),
        "the other game again, from memory");
  nspire_file_list_touched();
  CHECK(states_peek_soon(other, &count, &newest, pic2, &when, 0) == -1, "files moved: read again");
  CHECK(states_peek_when(other, &count, &newest, pic2, &when) == 1 && newest == 6, "read again whole");
  CHECK(states_peek_soon(rom, &count, &newest, pic2, &when, 0) == -1, "the open game's slots too");
  states_open_game(rom);

  /* the open game changes; it is kept as it is when another one opens */
  run_frames(3);
  CHECK(states_save(7) == 1, "save 7");
  CHECK(states_peek_soon(rom, &count, &newest, pic2, &when, 0) == 1 && count == 5 && newest == 7,
        "the open game, as it is: %d states, newest %d", count, newest);
  states_open_game_kept(other);
  CHECK(states_count() == 4 && states_newest() == 6 && states_exists(3), "the other game opened from memory");
  CHECK(states_peek_soon(rom, &count, &newest, pic2, &when, 0) == 1 && count == 5 && newest == 7,
        "the game left, kept with its new state: %d states, newest %d", count, newest);
  states_open_game_kept(rom);
  CHECK(states_count() == 5 && states_newest() == 7, "back to it from memory: %d, newest %d", states_count(),
        states_newest());
  CHECK(states_delete(7) == 1 && states_newest() == 6, "delete 7");
  states_open_game_kept(other);
  CHECK(states_peek_when(rom, &count, &newest, pic2, &when) == 1 && count == 4 && newest == 6,
        "kept without the deleted state: %d states, newest %d", count, newest);

  states_open_game(other);
  remove_states();
  states_open_game(rom);
  remove_states();
  states_open_game(rom);
}

/* SAV-5: bson_read_int32_array reads exactly the words asked for. */
static void test_bson_array(void)
{
  u8 doc[64];
  u32 words[2], n, i;
  u32 guard[6];

  memset(doc, 0, sizeof(doc));
  doc[4] = BSON_TYPE_ARR;
  doc[5] = 'k';
  n = 4 + 2 * 8 + 1;
  memcpy(doc + 7, &n, 4);
  for (i = 0; i < 2; i++)
  {
    u32 v = 0x11111111u * (i + 1);
    doc[11 + i * 8] = BSON_TYPE_INT32;
    doc[12 + i * 8] = '0';
    doc[13 + i * 8] = '0' + i;
    memcpy(doc + 11 + i * 8 + 4, &v, 4);
  }
  n = 40;
  memcpy(doc, &n, 4);
  memset(guard, 0xAA, sizeof(guard));
  CHECK(bson_read_int32_array(doc, "k", guard, 2) && guard[0] == 0x11111111u && guard[1] == 0x22222222u &&
        guard[2] == 0xAAAAAAAAu, "bson array of 2 words");
  n = 4 + 100 * 8 + 1;   /* a damaged length */
  memcpy(doc + 7, &n, 4);
  memset(guard, 0xAA, sizeof(guard));
  CHECK(!bson_read_int32_array(doc, "k", guard, 2) && guard[0] == 0xAAAAAAAAu, "a damaged array length is refused");
  n = 4 + 2 * 8 + 1;
  memcpy(doc + 7, &n, 4);
  CHECK(!bson_read_int32_array(doc, "absent", words, 2), "an absent array is refused");
}

/* Corrupt BSON used to escape the fixed state buffer before any field
 * validation ran. A rejected state must leave the running game untouched. */
static void test_bson_structure(void)
{
  u8 *saved = calloc(1, GBA_STATE_MEM_SIZE);
  u8 *bad = malloc(GBA_STATE_MEM_SIZE);
  unsigned i;
  u32 before = machine_hash(), word = 0;
  CHECK(saved && bad, "allocate BSON cases");
  if (!saved || !bad) { free(saved); free(bad); return; }
  gba_save_state(saved);
  for (i = 0; i < 6; i++)
  {
    u8 *p;
    memcpy(bad, saved, GBA_STATE_MEM_SIZE);
    switch (i)
    {
    case 0: bad[GBA_STATE_MEM_SIZE - 1] = 1; break;
    case 1: memset(bad + 5, 'x', GBA_STATE_MEM_SIZE - 5); break;
    case 2: bad[4] = 0x7f; break;
    case 3:
      p = (u8 *)bson_find_key(bad, "cpu");
      memset(p, 0xff, 4); break;
    case 4:
      p = (u8 *)bson_find_key(bson_find_key(bad, "memory"), "iwram");
      memset(p, 0xff, 4); break;
    default: bad[4] = 0; break; /* early end, trailing data */
    }
    CHECK(!gba_load_state(bad), "corrupt BSON case %u rejected", i);
    CHECK(machine_hash() == before, "corrupt BSON case %u changed machine", i);
  }
  CHECK(!bson_read_int32(saved, "cpu", &word), "document is not an integer");
  CHECK(!bson_find_document(saved, "info-magic"), "integer is not a document");
  CHECK(!bson_check_bytes(saved, "cpu", 32), "document is not a byte array");
  CHECK(!bson_check_int32_array(saved, "cpu", 32), "document is not an integer array");
  CHECK(!gba_load_state(NULL), "null state rejected");
  free(saved);
  free(bad);
}

static void state_set_word(u8 *p, u32 value)
{
  p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24;
}

/* Structurally correct states can still contain the wrong array sizes or
 * unsafe runtime indexes. Reject them before changing any subsystem. */
cpu_alert_type function_cc write_io_register16(u32 address, u32 value);

static void test_state_validation(void)
{
  u8 *saved = calloc(1, GBA_STATE_MEM_SIZE), *bad = malloc(GBA_STATE_MEM_SIZE);
  u8 *after = calloc(1, GBA_STATE_MEM_SIZE);
  unsigned i;
  CHECK(saved && bad && after, "allocate state-validation cases");
  if (!saved || !bad || !after) { free(saved); free(bad); free(after); return; }
  CHECK(load_rom("arm.gba.tns"), "state-validation ROM");
  run_frames(3);
  gba_save_state(saved);
  for (i = 0; i < 18; i++)
  {
    u8 *doc, *field;
    memcpy(bad, saved, GBA_STATE_MEM_SIZE);
    doc = (u8 *)bson_find_document(bad, "cpu");
    state_set_word((u8 *)bson_find_key(doc, "bus-value"), 0xabcdef01);
    if (i < 4)
    {
      char key[2] = { '0' + i, 0 };
      doc = (u8 *)bson_find_document(bson_find_document(bad, "dma"), key);
      field = (u8 *)bson_find_key(doc, "dsc");
      field[-4] = 'x'; /* missing field in each channel, including 1-3 */
    }
    else if (i == 4)
    {
      u8 *a = (u8 *)bson_find_key(doc, "regs"), *b = (u8 *)bson_find_key(doc, "spsr");
      memcpy(a - 5, "spsr", 4); memcpy(b - 5, "regs", 4);
    }
    else if (i == 5)
    {
      u8 *a, *b;
      doc = (u8 *)bson_find_document(bad, "memory");
      a = (u8 *)bson_find_key(doc, "iwram"); b = (u8 *)bson_find_key(doc, "ewram");
      memcpy(a - 6, "ewram", 5); memcpy(b - 6, "iwram", 5);
    }
    else if (i == 6)
      state_set_word((u8 *)bson_find_key(doc, "regs") + 8 + CPU_MODE * 8, 255);
    else if (i == 7 || i == 8)
    {
      doc = (u8 *)bson_find_document(bson_find_document(bad, "timers"), "3");
      state_set_word((u8 *)bson_find_key(doc, i == 7 ? "prescale" : "reload"), i == 7 ? 32 : 0);
    }
    else if (i == 9)
    {
      doc = (u8 *)bson_find_document(bson_find_document(bad, "dma"), "2");
      state_set_word((u8 *)bson_find_key(doc, "dst-dir"), 4);
    }
    else if (i == 10 || i == 11)
    {
      doc = (u8 *)bson_find_document(bad, "backup");
      state_set_word((u8 *)bson_find_key(doc, i == 10 ? "flash-bank-num" : "eeprom-addr"), 0xffffffff);
    }
    else if (i == 12)
    {
      doc = (u8 *)bson_find_document(bad, "memory");
      field = (u8 *)bson_find_key(doc, "ioregs") + 5 + REG_VCOUNT * 2;
      field[0] = 255;
    }
    else if (i == 13)
    {
      doc = (u8 *)bson_find_document(bad, "input");
      doc[-7] = BSON_TYPE_BIN; /* payload must not be interpreted as a document */
    }
    else if (i >= 15)
    {
      doc = (u8 *)bson_find_document(bad, "backup");
      state_set_word((u8 *)bson_find_key(doc, "eeprom-mode"), EEPROM_WRITE_ADDRESS_MODE);
      state_set_word((u8 *)bson_find_key(doc, i == 15 ? "eeprom-addr" :
                                     i == 16 ? "eeprom-counter" : "eeprom-size"),
                     i == 15 ? 0x1ffff : i == 16 ? 14 : 0xffffffff);
    }
    else
    {
      field = (u8 *)bson_find_key(doc, "regs");
      field[4] = BSON_TYPE_DOC; /* wrong element type inside an array */
    }
    CHECK(!gba_load_state(bad), "invalid state case %u rejected", i);
    memset(after, 0, GBA_STATE_MEM_SIZE);
    gba_save_state(after);
    CHECK(!memcmp(saved, after, GBA_STATE_MEM_SIZE), "invalid state case %u changed machine", i);
  }
  /* Wait-state tables are derived from WAITCNT, and must follow a load. */
  write_io_register16(REG_WAITCNT * 2, 0x4317);
  gba_save_state(saved);
  {
    u32 want = ws_cyc_nseq[8][1];
    write_io_register16(REG_WAITCNT * 2, 0);
    CHECK(gba_load_state(saved), "valid state with WAITCNT loads");
    CHECK(ws_cyc_nseq[8][1] == want, "wait-state table restored with WAITCNT");
  }
  free(saved); free(bad); free(after);
}

static void test_cheat_bounds(void)
{
  char many[1024];
  unsigned i, at = 0;
  CHECK(load_rom("arm.gba.tns"), "cheat-test ROM");
  cheat_clear();
  CHECK(cheat_parse(0, "  32000000   0011\n32000001 0022  ") == CheatNoError,
        "CodeBreaker whitespace");
  process_cheats();
  CHECK(ewram[0] == 0x11 && ewram[1] == 0x22, "whitespace codes both applied");
  CHECK(cheat_parse(0, "32000000 0099 32000001 bad") != CheatNoError, "partial code rejected");
  ewram[0] = ewram[1] = 0;
  process_cheats();
  CHECK(ewram[0] == 0x11 && ewram[1] == 0x22, "rejected code leaves previous code intact");
  cheat_clear();
  CHECK(cheat_parse(0, "52000000 0003 91223344 5566") == CheatNoError,
        "super-code payload is not an encryption opcode");
  process_cheats();
  CHECK(!memcmp(ewram, "\x91\x22\x33\x44\x55\x66", 6), "six super-code bytes");
  CHECK(cheat_parse(0, "52000000 0004 11223344 5566") != CheatNoError, "truncated super-code rejected");
  CHECK(cheat_parse(0, "42000000 0001") != CheatNoError, "missing slide parameters rejected");
  CHECK(cheat_parse(0, "32000000 12345") != CheatNoError, "overlong value rejected");
  CHECK(cheat_parse(MAX_CHEATS, "32000000 0001") == CheatErrorTooMany, "slot bound");
  CHECK(cheat_parse(0, NULL) != CheatNoError && cheat_parse(0, "  ") != CheatNoError, "empty code rejected");
  cheat_clear();
  CHECK(cheat_parse(0, "10000080 0000") == CheatNoError && cheat_master_hook == 0x08000080,
        "master hook installed");
  CHECK(cheat_parse(0, "32000000 0001") == CheatNoError && cheat_master_hook == 0xffffffff,
        "replacing master removes its hook");
  for (i = 0; i < MAX_CHEAT_CODES - 1; i++)
    at += snprintf(many + at, sizeof(many) - at, "32000000 0000 ");
  snprintf(many + at, sizeof(many) - at, "52000000 0001");
  CHECK(cheat_parse(0, many) != CheatNoError, "super-code at last physical slot has no payload");
  cheat_clear();
}

static void test_serial_packet_bounds(void)
{
  u8 *allocation = calloc(1, 8 + 255 * 2 + 1), *pkt;
  unsigned i;
  CHECK(allocation != NULL, "serial packet allocation");
  if (!allocation) return;
  pkt = allocation + 1;
  serialproto_reset();
  memcpy(pkt, "MPK1", 4); pkt[7] = 1;
  serialpoke_net_receive(pkt, 24, 1);
  CHECK((serialpoke_states() & 0xf0) == 0x10, "unaligned Pokemon packet accepted");
  serialpoke_net_receive(pkt, 24, 4);
  serialpoke_net_receive(pkt, 24, 65535);
  CHECK((serialpoke_states() & 0xf0) == 0x10, "invalid peer ids ignored");
  /* RCNT selects GPIO vs serial; its low bits are not SIOCNT's mode. */
  write_ioreg(REG_SIOCNT, 0x2000); write_ioreg(REG_RCNT, 0x8000);
  write_rcnt(0);
  CHECK(serialpoke_states() == 0 && (read_ioreg(REG_SIOCNT) & 8),
        "return from GPIO to multiplayer resets protocol and ready bits");

  serial_mode = SERIAL_MODE_SERIAL_AW1; netplay_client_id = 0;
  serialproto_reset();
  memcpy(pkt, "MAW1", 4); pkt[4] = 0x4f; pkt[5] = 0xff; pkt[6] = 0; pkt[7] = 255;
  for (i = 0; i < 255; i++) { pkt[8 + i * 2] = i; pkt[9 + i * 2] = 255 - i; }
  serialaw_net_receive(pkt, 8 + 255 * 2, 1);
  serialaw_net_receive(pkt, 8 + 255 * 2, 4);
  write_ioreg(REG_SIOMLT_SEND, 0x7fff);
  for (i = 0; i < 256; i++)
  {
    u16 want = i ? ((i - 1) << 8) | (256 - i) : 0x4fff;
    serialaw_master_send();
    CHECK(read_ioreg(REG_SIOMULTI1) == want, "AW word %u: got %04x expected %04x", i,
          read_ioreg(REG_SIOMULTI1), want);
  }
  serialaw_master_send();
  CHECK(read_ioreg(REG_SIOMULTI1) == 0x7fff, "full AW packet drained once");
  free(allocation);
  /* A tightly allocated odd-word packet leaves no spare halfword for a
   * word-sized read of its final element. */
  allocation = calloc(1, 15);
  CHECK(allocation != NULL, "short serial allocation");
  if (allocation)
  {
    pkt = allocation + 1;
    serialproto_reset();
    memcpy(pkt, "MAW1", 4); pkt[4] = 0x4f; pkt[5] = 0xff; pkt[7] = 3;
    serialaw_net_receive(pkt, 14, 1);
    free(allocation);
  }
  /* A damaged guest can stream sync words indefinitely. The transport's
   * one-byte word count must bound both collection and stack packing. */
  serialproto_reset();
  write_ioreg(REG_SIOMLT_SEND, 0x5678); serialaw_master_send();
  write_ioreg(REG_SIOMLT_SEND, 0x8001); serialaw_master_send();
  for (i = 0; i < 1024; i++) serialaw_master_send();
  write_ioreg(REG_SIOMLT_SEND, 0x8000); serialaw_master_send();
  CHECK(1, "overlong AW sync burst discarded without overflow");
  serialproto_reset(); serial_mode = SERIAL_MODE_AUTO;
}

void rfu_test_state(unsigned state);

static void test_rfu_command_bounds(void)
{
  unsigned state, words, i;
  for (state = 1; state <= 3; state += 2)
    for (words = 0; words <= 255; words++)
    {
      unsigned bytes = words > 1 ? 4 : 1;
      rfu_reset();
      rfu_transfer(0x494e); rfu_transfer(0xb0bb8001);
      rfu_test_state(state);
      rfu_transfer(0x99660024 | (words << 8));
      for (i = 0; i < words; i++)
        rfu_transfer(i ? 0x11223344 : bytes << (state == 3 ? 8 : 0));
      u32 response = rfu_transfer(0);
      CHECK(response == ((words > 24) ? 0x996601ee : 0x996600a4),
            "RFU %s %u command words: %08x", state == 1 ? "host" : "client", words, response);
    }
  rfu_reset();
}

/* Restarting between the two non-replacing renames must bring back the
 * previous completed slot/save, instead of quietly starting from blank. */
static void test_save_rollback(void)
{
  char rom[600], path[600], old[640];
  CHECK(load_rom("arm.gba.tns"), "rollback ROM");
  path_in_dir(rom, "arm.gba.tns");
  states_open_game(rom);
  CHECK(states_save(4), "rollback state saved");
  states_file_name(4, path, sizeof(path));
  snprintf(old, sizeof(old), "%s.rollback", path);
  CHECK(rename(path, old) == 0, "interrupt after moving previous state");
  states_open_game(rom);
  CHECK(states_exists(4) && access(old, F_OK) != 0 && states_load(4), "state rollback recovered and loads");
  remove_states();

  path_in_dir(path, "rollback.sav.tns");
  snprintf(old, sizeof(old), "%s.rollback", path);
  backup_type = BACKUP_SRAM;
  gamepak_backup[0] = 0xa5;
  CHECK(save_backup(path), "rollback cartridge save written");
  CHECK(rename(path, old) == 0, "interrupt after moving previous cartridge save");
  gamepak_backup[0] = 0;
  CHECK(load_backup(path) && gamepak_backup[0] == 0xa5 && access(old, F_OK) != 0,
        "cartridge rollback recovered and loads");
  remove(path);
}

extern int nspire_fastdeflate_fail_alloc;   /* nspire_fastdeflate.c, NSPIRE_ALLOC_HOOK */

/* No memory for fastdeflate's work: 0 (the caller keeps what it had), then fine again. */
static void test_fastdeflate_oom(void)
{
  static unsigned char in[4096], out[FASTDEFLATE_BOUND(4096)];
  nspire_fastdeflate_fail_alloc = 1;
  CHECK(fastdeflate(out, sizeof(out), in, sizeof(in)) == 0, "fastdeflate: no memory gives 0");
  CHECK(fastdeflate_picture(out, sizeof(out), in, sizeof(in), 480) == 0, "fastdeflate_picture: no memory gives 0");
  nspire_fastdeflate_fail_alloc = 0;
  CHECK(fastdeflate(out, sizeof(out), in, sizeof(in)) > 0, "fastdeflate works again after");
}

extern int nspire_states_fail_alloc;   /* nspire_states.c, NSPIRE_ALLOC_HOOK: its own mallocs "fail" */

/* A slot's state, unpacked (GBA_STATE_MEM_SIZE bytes into raw): 1. */
static int slot_raw(int slot, u8 *raw)
{
  char path[600];
  long size;
  u8 *file;
  uLongf n = GBA_STATE_MEM_SIZE;
  u32 packed, flags;
  int ok = 0;

  states_file_name(slot, path, sizeof(path));
  file = read_file(path, &size);
  if (file && size > 32 && !memcmp(file, "GPSZ", 4))
  {
    memcpy(&packed, file + 16, 4);
    memcpy(&flags, file + 20, 4);
    if (flags & 2)
      ok = packed == GBA_STATE_MEM_SIZE && (memcpy(raw, file + 32, GBA_STATE_MEM_SIZE), 1);
    else
      ok = uncompress(raw, &n, file + 32, packed) == Z_OK && n == GBA_STATE_MEM_SIZE;
  }
  free(file);
  return ok;
}

/* save17 with the kept buffers a save, a load and the compress
 * toggle need no memory of their own (every malloc of theirs fails here),
 * and the bytes go round: save -> load -> save gives the same state, and so
 * do stored -> packed -> stored. Without them, no memory is what a save and
 * a load say, each its own cause; a file too big to be a state is damaged,
 * not "no memory"; another build's header is "not a state it can read". */
static void test_states_memory(void)
{
  char rom[600], path[600];
  static u8 a[GBA_STATE_MEM_SIZE], b[GBA_STATE_MEM_SIZE];
  FILE *f;

  CHECK(load_rom("arm.gba.tns"), "arm.gba.tns doesn't load");
  path_in_dir(rom, "arm.gba.tns");
  states_open_game(rom);
  remove_states();
  states_open_game(rom);
  run_frames(30);
  CHECK(states_keep_buffers() == 1, "memory: the buffers are kept");
  nspire_states_fail_alloc = 1;
  CHECK(states_save(1) == 1, "memory: a save with no memory of its own");
  CHECK(states_load(1) == 1, "memory: a load with no memory of its own");
  CHECK(states_save(2) == 1, "memory: a save after it");
  CHECK(slot_raw(1, a) && slot_raw(2, b) && !memcmp(a, b, GBA_STATE_MEM_SIZE),
        "memory: save -> load -> save, the same state bytes");
  CHECK(states_set_compressed(2, 0) == 1 && states_load(2) == 1 && slot_raw(2, b) && !memcmp(a, b, GBA_STATE_MEM_SIZE),
        "memory: stored, it loads, the same bytes");
  CHECK(states_set_compressed(2, 1) == 1 && states_load(2) == 1 && slot_raw(2, b) && !memcmp(a, b, GBA_STATE_MEM_SIZE),
        "memory: packed again, it loads, the same bytes");

  states_drop_buffers();
  CHECK(states_save(3) == 0 && states_failure(STATES_SAVING)->why == STATES_WHY_MEMORY &&
        !strcmp(states_failure_words(STATES_SAVING), "not enough memory"), "memory: without them a save says memory: %s",
        states_failure_words(STATES_SAVING));
  CHECK(states_load(1) == -1 && states_failure(STATES_LOADING)->why == STATES_WHY_MEMORY,
        "memory: and a load: %s", states_failure_words(STATES_LOADING));
  nspire_states_fail_alloc = 0;
  CHECK(states_load(1) == 1 && states_failure(STATES_SAVING)->why == STATES_WHY_MEMORY,
        "memory: a load that works leaves the save's cause as it was: %s", states_failure_words(STATES_SAVING));

  /* a file bigger than any state: damaged, never read */
  states_file_name(4, path, sizeof(path));
  f = fopen(path, "wb");
  if (f)
  {
    static u8 junk[600 * 1024];
    memset(junk, 0x5a, sizeof(junk));
    fwrite(junk, 1, sizeof(junk), f);
    fclose(f);
  }
  states_open_game(rom);
  CHECK(states_load(4) == -1 && states_failure(STATES_LOADING)->why == STATES_WHY_DAMAGED,
        "memory: a 600 KB file is damaged, not memory: %s", states_failure_words(STATES_LOADING));
  /* another build's header: not a state it can read */
  f = fopen(path, "wb");
  if (f)
  {
    u32 head[8] = { 0, 99, 1, GBA_STATE_MEM_SIZE, 0, 1, 0, 0 };
    memcpy(head, "GPSZ", 4);
    fwrite(head, 1, sizeof(head), f);
    fclose(f);
  }
  CHECK(states_load(4) == -1 && states_failure(STATES_LOADING)->why == STATES_WHY_FOREIGN &&
        !strcmp(states_failure_words(STATES_LOADING), "not a state it can read"),
        "memory: another version's state says so: %s", states_failure_words(STATES_LOADING));
  remove(path);
  remove_states();
}

/* A slot's header word at offset at (28: reserved[1], the save time). */
static u32 header_word(int slot, int at)
{
  char path[600];
  u32 w = 0;
  FILE *f;
  states_file_name(slot, path, sizeof(path));
  f = fopen(path, "rb");
  if (f)
  {
    if (fseek(f, at, SEEK_SET) != 0 || fread(&w, 4, 1, f) != 1)
      w = 0;
    fclose(f);
  }
  return w;
}

static void set_header_word(int slot, int at, u32 w)
{
  char path[600];
  FILE *f;
  states_file_name(slot, path, sizeof(path));
  f = fopen(path, "r+b");
  if (f && fseek(f, at, SEEK_SET) == 0)
    fwrite(&w, 4, 1, f);
  if (f)
    fclose(f);
}

static void set_file_time(int slot, time_t when)
{
  char path[600];
  struct utimbuf t;
  states_file_name(slot, path, sizeof(path));
  t.actime = t.modtime = when;
  utime(path, &t);
}

/* save17: a state's save time in its header (reserved[1]): what states_time says, kept when it is packed
 * or stored (a new file: the calculator can't set a file's time); a header without it (r16's and older:
 * 0) says its file's time, and packing it keeps that one. */
static void test_states_saved_time(void)
{
  char rom[600];
  u32 at;
  time_t now;

  CHECK(load_rom("arm.gba.tns"), "arm.gba.tns doesn't load");
  path_in_dir(rom, "arm.gba.tns");
  states_open_game(rom);
  remove_states();
  states_open_game(rom);
  run_frames(10);
  now = time(NULL);
  CHECK(states_save(1) == 1, "saved time: save 1");
  at = header_word(1, 28);
  CHECK(at != 0 && (time_t)at >= now - 1 && (time_t)at <= time(NULL), "saved time: in the header: %u (now %ld)", at,
        (long)now);
  set_file_time(1, 1000000000);
  states_open_game(rom);
  CHECK(states_time(1) == (long)at, "saved time: the header's over the file's: %ld", states_time(1));
  CHECK(states_set_compressed(1, 0) == 1 && header_word(1, 28) == at && states_time(1) == (long)at,
        "saved time: stored, it stays: %u, %ld", header_word(1, 28), states_time(1));
  CHECK(states_set_compressed(1, 1) == 1 && header_word(1, 28) == at, "saved time: packed again, it stays");
  states_open_game(rom);
  CHECK(states_time(1) == (long)at && states_load(1) == 1, "saved time: read again, the same, and it loads");
  /* r16's header: no save time, the file's */
  set_header_word(1, 28, 0);
  set_file_time(1, 1600000000);
  states_open_game(rom);
  CHECK(states_time(1) == 1600000000L, "saved time: an older header says its file's: %ld", states_time(1));
  CHECK(states_set_compressed(1, 0) == 1 && header_word(1, 28) == 1600000000u,
        "saved time: stored, its file's time kept in it: %u", header_word(1, 28));
  states_open_game(rom);
  CHECK(states_time(1) == 1600000000L && states_load(1) == 1, "saved time: and that is its time: %ld", states_time(1));
  /* a wrong clock's time in the header (before 2020, or later than tomorrow): the file's instead */
  set_header_word(1, 28, 1000);
  set_file_time(1, 1650000000);
  states_open_game(rom);
  CHECK(states_time(1) == 1650000000L, "saved time: a 1970 header time not used: %ld", states_time(1));
  set_header_word(1, 28, (u32)time(NULL) + 3 * 86400);
  set_file_time(1, 1650000000);
  states_open_game(rom);
  CHECK(states_time(1) == 1650000000L, "saved time: one 3 days ahead not used: %ld", states_time(1));
  remove_states();
}

static void test_backup(void)
{
  char path[600], plain[600];
  long size;
  u8 *saved = (u8 *)malloc(sizeof(gamepak_backup));
  FILE *f;

  CHECK(load_rom("sram.gba.tns"), "sram.gba.tns doesn't load");
  run_frames(60);
  CHECK(backup_type == BACKUP_SRAM, "backup type %d", backup_type);
  memcpy(saved, gamepak_backup, sizeof(gamepak_backup));

  /* In-game saves stay plain .sav files. */
  path_in_dir(path, "sram.sav.tns");
  CHECK(save_backup(path) == 1, "save_backup");
  size = file_size(path);
  CHECK(size == 0x8000, "the save is %ld bytes", size);
  memset(gamepak_backup, 0x5A, sizeof(gamepak_backup));
  backup_type = BACKUP_UNKN;
  CHECK(load_backup(path) == 1, "load_backup");
  CHECK(backup_type == BACKUP_SRAM, "type after load %d", backup_type);
  CHECK(memcmp(saved, gamepak_backup, 0x8000) == 0, "SRAM differs after load");

  /* A file of no save size is refused, and leaves a blank save. */
  path_in_dir(plain, "odd.sav.tns");
  f = fopen(plain, "wb");
  fwrite(saved, 1, 1000, f);
  fclose(f);
  CHECK(load_backup(plain) == 0, "odd size should fail");
  CHECK(gamepak_backup[0] == 0xFF, "blank after a bad save");

  /* The game's own save files: gzipped with Compress saves on, plain with
   * it off, the other form removed; loading takes the newer. */
  {
    char gz[600];
    long bytes;
    int compressed;
    u8 *data;
    struct utimbuf times;

    memcpy(gamepak_backup, saved, 0x8000);
    backup_type = BACKUP_SRAM;
    path_in_saves(plain, "sram.gba.sav.tns");
    path_in_saves(gz, "sram.gba.sav.gz.tns");
    remove(plain);
    remove(gz);
    nspire_compress_saves = 1;
    update_backup_force();
    CHECK(file_size(gz) > 0 && file_size(gz) < 0x8000 && file_size(plain) < 0,
          "gzipped: %ld bytes, plain %ld", file_size(gz), file_size(plain));
    data = read_file(gz, &bytes);
    CHECK(data && bytes > 2 && data[0] == 0x1F && data[1] == 0x8B, "a gzip file");
    free(data);
    memset(gamepak_backup, 0x5A, sizeof(gamepak_backup));
    nspire_load_cartridge_backup();
    CHECK(!memcmp(gamepak_backup, saved, 0x8000) && backup_type == BACKUP_SRAM, "the gzipped save loads");
    CHECK(nspire_backup_info(&bytes, &compressed) && compressed && bytes == file_size(gz), "info: gzipped");

    nspire_compress_saves = 0;
    update_backup_force();
    CHECK(file_size(plain) == 0x8000 && file_size(gz) < 0, "plain again: %ld, gz %ld", file_size(plain),
          file_size(gz));
    nspire_compress_saves = 1;

    /* Both there: the newer one counts. */
    CHECK(nspire_backup_set_compressed(1) == 1, "save manager: compress");
    f = fopen(plain, "wb");
    memset(gamepak_backup, 0x11, 0x8000);
    fwrite(gamepak_backup, 1, 0x8000, f);
    fclose(f);
    times.actime = times.modtime = 1000000;
    utime(gz, &times);
    times.actime = times.modtime = 2000000;
    utime(plain, &times);
    nspire_load_cartridge_backup();
    CHECK(gamepak_backup[0] == 0x11, "the newer plain save loads");
    utime(gz, &times);
    times.actime = times.modtime = 1000000;
    utime(plain, &times);
    nspire_load_cartridge_backup();
    CHECK(!memcmp(gamepak_backup, saved, 0x8000), "the newer gzipped save loads");

    /* A damaged gzip falls back to the plain one. */
    f = fopen(gz, "wb");
    fputs("not gzip", f);
    fclose(f);
    nspire_load_cartridge_backup();
    CHECK(gamepak_backup[0] == 0x11, "damaged gzip: the plain save loads");
    {
      char bad[640];
      snprintf(bad, sizeof(bad), "%s.bad", gz);
      update_backup_force();
      CHECK(file_size(gz) == 8 && file_size(bad) < 0,
            "damaged gzip: preserved at its original path until the game saves (%ld)", file_size(gz));
      remove(bad);
      /* ... and with no plain save to fall back on: blank memory, and the
       * unreadable file is still there to mend after leaving. */
      remove(plain);
      f = fopen(gz, "wb");
      fputs("not gzip", f);
      fclose(f);
      nspire_load_cartridge_backup();
      update_backup_force();
      CHECK(file_size(gz) == 8 && gamepak_backup[0] == 0xFF,
            "damaged gzip alone: original kept (%ld)", file_size(gz));
      remove(bad);
      remove(gz);
      remove(plain);
    }

    /* The save manager: uncompress, delete (not written back when leaving,
     * until the game saves again). */
    memcpy(gamepak_backup, saved, 0x8000);
    remove(plain);
    remove(gz);
    nspire_backup_mark_dirty();
    update_backup_force();
    CHECK(nspire_backup_set_compressed(0) == 1 && file_size(plain) == 0x8000 && file_size(gz) < 0,
          "save manager: uncompress");
    CHECK(nspire_backup_delete() == 1 && file_size(plain) < 0, "save manager: delete");
    CHECK(!nspire_backup_info(&bytes, &compressed), "no save after deleting");
    update_backup_force();
    CHECK(file_size(plain) < 0 && file_size(gz) < 0, "a deleted save isn't written back when leaving");
    nspire_backup_mark_dirty();
    update_backup_force();
    CHECK(file_size(gz) > 0, "the game saved again: it's back");
    remove(gz);

    /* R6: a save goes to a .tmp and takes the name once whole. A write
     * that fails (here: the .tmp can't be made) leaves the save there as
     * it was; a .tmp left by a reset mid-write is dropped; one left
     * between the old one's removal and the rename is the save. */
    {
      char tmp[620];
      snprintf(tmp, sizeof(tmp), "%s.tmp", gz);
      memcpy(gamepak_backup, saved, 0x8000);
      nspire_compress_saves = 1;
      nspire_backup_mark_dirty();
      update_backup_force();
      bytes = file_size(gz);
      CHECK(bytes > 0 && file_size(tmp) < 0, "written, no .tmp left: %ld", bytes);
      mkdir(tmp, 0755);   /* the next write can't make its .tmp */
      memset(gamepak_backup, 0x22, 0x8000);
      update_backup_force();
      rmdir(tmp);
      memset(gamepak_backup, 0x5A, 0x8000);
      nspire_load_cartridge_backup();
      CHECK(file_size(gz) == bytes && !memcmp(gamepak_backup, saved, 0x8000),
            "a failed write leaves the save as it was");
      f = fopen(tmp, "wb");
      fputs("half a sa", f);   /* a reset in the .tmp's write */
      fclose(f);
      nspire_load_cartridge_backup();
      CHECK(!memcmp(gamepak_backup, saved, 0x8000) && file_size(tmp) < 0, "a partial .tmp is dropped");
      rename(gz, tmp);   /* a reset after the old one's removal */
      memset(gamepak_backup, 0x5A, 0x8000);
      nspire_load_cartridge_backup();
      CHECK(!memcmp(gamepak_backup, saved, 0x8000) && file_size(gz) == bytes && file_size(tmp) < 0,
            "a whole .tmp without its save is the save");
      remove(gz);

      /* SAV-4: a partial .tmp with no save is dropped, not taken. */
      f = fopen(tmp, "wb");
      fwrite(saved, 1, 5000, f);
      fclose(f);
      nspire_load_cartridge_backup();
      CHECK(file_size(tmp) < 0 && file_size(gz) < 0 && file_size(plain) < 0,
            "a partial .tmp of no save size isn't taken");
    }

    /* SAV-1: a save file that doesn't load (here 0x4000 bytes) is kept
     * as it is on leaving, in Compress or Uncompress, and set aside as
     * .bad once the game really saves. */
    {
      char bad[620];
      u8 *odd = (u8 *)malloc(0x4000), *back = (u8 *)malloc(0x4000);
      int i;

      for (i = 0; i < 0x4000; i++)
        odd[i] = (u8)(i * 7 + 1);
      snprintf(bad, sizeof(bad), "%s.bad", plain);
      remove(bad);
      remove(gz);
      f = fopen(plain, "wb");
      fwrite(odd, 1, 0x4000, f);
      fclose(f);
      nspire_load_cartridge_backup();
      backup_type = BACKUP_SRAM;
      nspire_compress_saves = 1;
      update_backup_force();
      nspire_backup_set_compressed(1);
      CHECK(file_size(plain) == 0x4000 && file_size(gz) < 0, "unreadable save kept on leaving: %ld", file_size(plain));
      f = fopen(plain, "rb");
      CHECK(f && fread(back, 1, 0x4000, f) == 0x4000 && !memcmp(back, odd, 0x4000), "unreadable save bytes unchanged");
      if (f)
        fclose(f);
      nspire_backup_mark_dirty();
      update_backup_force();
      CHECK(file_size(bad) == 0x4000 && file_size(gz) > 0, "set aside as .bad, then written");
      remove(gz);
      remove(bad);
      remove(plain);
      free(odd);
      free(back);
    }
  }
  free(saved);
}

/* ---- Leaving a game writes its save only when it changed ---- */

extern int nspire_backup_check_failures;   /* nspire_backup.c, NSPIRE_BACKUP_CHECK */

#define OLD_TIME 1000000

static char sav_plain[600], sav_gz[600];

static void put_file(const char *path, u32 size, int fill)
{
  u8 *data = (u8 *)malloc(size);
  FILE *f = fopen(path, "wb");
  memset(data, fill, size);
  if (f)
  {
    fwrite(data, 1, size, f);
    fclose(f);
  }
  free(data);
}

/* An old time on the file: written again, it gets the time now. */
static void age(const char *path)
{
  struct utimbuf times;
  times.actime = times.modtime = OLD_TIME;
  utime(path, &times);
}

static int rewritten(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0 && st.st_mtime != OLD_TIME;
}

static int unchanged(const char *path)
{
  struct stat st;
  return stat(path, &st) == 0 && st.st_mtime == OLD_TIME;
}

/* The save as a game's start reads it: the byte at offset. */
static int reloaded(u32 offset)
{
  memset(gamepak_backup, 0, sizeof(gamepak_backup));
  nspire_load_cartridge_backup();
  return gamepak_backup[offset];
}

/* Bits to the EEPROM as a game's DMA sends them, the first one first. */
static void eeprom_bits(u32 bits, int count)
{
  while (count-- > 0)
    write_eeprom(0x0D000000, (bits >> count) & 1);
}

/* A frame's end as the frame loop has it (2026-11-17, final/pace6): the
 * countdown to the game's save, then the next part of a save being written
 * (nspire_host_main.c save_job_frame: a frame each, nspire_backup.c). */
static void frames_backup(int n)
{
  while (n-- > 0)
  {
    update_backup();
    nspire_backup_step();
  }
}

/* A leave and quit()'s second one: nothing written. */
static void check_skips(const char *path, const char *what)
{
  age(path);
  update_backup_force();
  update_backup_force();
  CHECK(unchanged(path), "%s: leaving writes nothing", what);
}

/* the game's save due again while its last one is still being written (its
 * parts waiting behind a state's: save_job_frame steps one job a frame) waits for it: not the last one
 * finished at once with the interrupts masked. Both whole, in order; the quit in between writes the
 * newest. */
static int sav_holds(const char *path, u32 offset, int v)
{
  long size;
  u8 *data = read_file(path, &size);
  int r = data && size == 0x8000 && data[offset] == (u8)v;
  free(data);
  return r;
}

static void test_backup_due_again(void)
{
  int i;

  CHECK(load_rom("sram.gba.tns"), "sram.gba.tns doesn't load");
  path_in_saves(sav_plain, "sram.gba.sav.tns");
  path_in_saves(sav_gz, "sram.gba.sav.gz.tns");
  remove(sav_plain);
  remove(sav_gz);
  nspire_compress_saves = 0;
  update_backup_flag = 1;
  put_file(sav_plain, 0x8000, 0x33);
  nspire_load_cartridge_backup();
  CHECK(gamepak_backup[0x200] == 0x33, "due again: the old save loads");

  /* the game saves (0x44); its write starts; the frames' parts go to a state (none stepped here) */
  write_backup(0x200, 0x44);
  for (i = 0; i < 11; i++)
    update_backup();
  CHECK(nspire_backup_busy() && sav_holds(sav_plain, 0x200, 0x33), "due again: the first write under way");
  /* the game saves again (0x55) and its write comes due */
  write_backup(0x200, 0x55);
  for (i = 0; i < 11; i++)
    update_backup();
  CHECK(nspire_backup_busy() && sav_holds(sav_plain, 0x200, 0x33),
        "due again: the first write not finished at once for the second");
  /* the parts at the frames' ends: the first written, then the second */
  {
    int seen_first = 0;
    for (i = 0; i < 200 && (nspire_backup_busy() || !sav_holds(sav_plain, 0x200, 0x55)); i++)
    {
      nspire_backup_step();
      seen_first |= sav_holds(sav_plain, 0x200, 0x44);
      update_backup();
    }
    CHECK(seen_first, "due again: the first save written whole first");
  }
  CHECK(!nspire_backup_busy() && sav_holds(sav_plain, 0x200, 0x55), "due again: the second save written after it");
  CHECK(reloaded(0x200) == 0x55, "due again: the newest loads");

  /* the quit in between: the newest written */
  write_backup(0x200, 0x66);
  for (i = 0; i < 11; i++)
    update_backup();
  write_backup(0x200, 0x77);
  for (i = 0; i < 11; i++)
    update_backup();
  CHECK(nspire_backup_busy() && sav_holds(sav_plain, 0x200, 0x55), "due again: two saves wanted, none written");
  update_backup_force();
  CHECK(!nspire_backup_busy() && sav_holds(sav_plain, 0x200, 0x77) && reloaded(0x200) == 0x77,
        "due again: the quit in between writes the newest");
  nspire_compress_saves = 1;
  remove(sav_plain);
  remove(sav_gz);
}

/* the game's save written a part a frame (8 KB) and stopped after any part
 * (the battery out): at the next start the old save loads (or, stopped past the new one's rename, the new
 * one whole), never a cut one of a save's size: plain and gzipped, with an older file of the same form, of
 * the other form, or none; nothing of the cut write left to be taken later. */
static void file_put(const char *path, int v)
{
  put_file(path, 0x8000, v);
}

static int part_count;
static void cut_case(int compressed, int older, int stop)
{
  char part[620], tmp[620];
  const char *name = compressed ? sav_gz : sav_plain;
  int i, k, v;

  remove(sav_plain);
  remove(sav_gz);
  /* the old save: older 1 the same form, 2 the other, 0 none */
  nspire_compress_saves = older == 2 ? !compressed : compressed;
  memset(gamepak_backup, 0x33, 0x8000);
  backup_type = BACKUP_SRAM;
  if (older)
  {
    file_put(sav_plain, 0x33);
    nspire_load_cartridge_backup();
    if (nspire_compress_saves)
      update_backup_force();   /* (packed: written as the .gz) */
    age(nspire_compress_saves ? sav_gz : sav_plain);   /* (written long before) */
  }
  else
  {
    nspire_load_cartridge_backup();
    memset(gamepak_backup, 0x33, 0x8000);
    backup_type = BACKUP_SRAM;
  }
  nspire_compress_saves = compressed;
  /* the game saves 0x44; its write starts at the countdown's end, then a part a frame */
  for (i = 0; i < 0x8000; i += 0x100)
    write_backup((u32)i, 0x44);
  for (i = 0; i < 11 && !nspire_backup_busy(); i++)
    update_backup();
  CHECK(nspire_backup_busy(), "cut: the write began");
  for (k = 0; k < stop && nspire_backup_busy(); k++)
    nspire_backup_step();
  if (stop == 1000)
    part_count = k;
  nspire_backup_drop_job_for_test();
  /* gpSP again */
  memset(gamepak_backup, 0, sizeof(gamepak_backup));
  nspire_load_cartridge_backup();
  v = gamepak_backup[0x100];
  if (older)
    CHECK((v == 0x33 || v == 0x44) && backup_type == BACKUP_SRAM && gamepak_backup[0x7F00] == v,
          "cut (%s, older %d, after %d parts): the old save (or the new whole): %02x %02x type %d",
          compressed ? "gz" : "plain", older, stop, v, gamepak_backup[0x7F00], backup_type);
  else
    CHECK(v == 0x44 ? gamepak_backup[0x7F00] == 0x44 && backup_type == BACKUP_SRAM : !file_size(name) || file_size(name) < 0,
          "cut (%s, no older, after %d parts): nothing, or the new whole: %02x %02x, %ld bytes",
          compressed ? "gz" : "plain", stop, v, gamepak_backup[0x7F00], file_size(name));
  snprintf(part, sizeof(part), "%s.part", name);
  snprintf(tmp, sizeof(tmp), "%s.tmp", name);
  CHECK(file_size(part) < 0 && file_size(tmp) < 0, "cut (%s, older %d, after %d parts): nothing left of it",
        compressed ? "gz" : "plain", older, stop);
}

static void test_backup_cut(void)
{
  CHECK(load_rom("sram.gba.tns"), "sram.gba.tns doesn't load");
  path_in_saves(sav_plain, "sram.gba.sav.tns");
  path_in_saves(sav_gz, "sram.gba.sav.gz.tns");
  update_backup_flag = 1;
  for (int compressed = 0; compressed < 2; compressed++)
    for (int older = 0; older < 3; older++)
    {
      part_count = 0;
      cut_case(compressed, older, 1000);   /* (its parts: counted, the write whole) */
      for (int stop = 1; stop < part_count; stop++)
        cut_case(compressed, older, stop);
    }
  printf("  the game's save in %d parts (gzipped), the battery out after each\n", part_count);
  /* an older build's (r19's) plain .tmp cut at its first 8 KB part, a .sav.gz beside it: dropped */
  {
    char tmp[620];
    remove(sav_plain);
    remove(sav_gz);
    nspire_compress_saves = 1;
    memset(gamepak_backup, 0x33, 0x8000);
    backup_type = BACKUP_SRAM;
    file_put(sav_plain, 0x33);
    nspire_load_cartridge_backup();
    update_backup_force();
    age(sav_gz);
    snprintf(tmp, sizeof(tmp), "%s.tmp", sav_plain);
    put_file(tmp, 0x2000, 0x44);
    memset(gamepak_backup, 0, sizeof(gamepak_backup));
    nspire_load_cartridge_backup();
    CHECK(gamepak_backup[0x100] == 0x33 && gamepak_backup[0x7F00] == 0x33 && backup_type == BACKUP_SRAM &&
          file_size(tmp) < 0 && file_size(sav_plain) < 0, "cut: an older build's short .tmp dropped, the .gz loads");
  }
  nspire_compress_saves = 1;
  remove(sav_plain);
  remove(sav_gz);
}

/* The game's save written in one go (linked here: save_packed_linked, stdio's buffer, its close the
 * write) that fails left its .part until the game's next load; now removed at once. The .part a link to
 * /dev/full: the close fails (no space). The link brought up as frontend_tests' test_link does (host_stubs.c's
 * cable). */
#include "nspire_link.h"
extern int unit_cable_on, unit_cable_in_len, unit_watch_seen, unit_usb_plugged;
extern uint8_t unit_cable_in[64];
extern uint32_t unit_cable_nonce;
extern u32 unit_ticks;
void unit_cable_clear(void);

static void core_link_frames(int n)
{
  while (n-- > 0)
  {
    nspire_link_frame_begin();
    nspire_link_frame_end();
    unit_ticks += NSPIRE_TICK_HZ / 60;
  }
}

static int core_link_up(void)
{
  static const u8 head[8] = { 1, 0, 16, 0, 0, 0, 0, 0 };
  u8 *body = unit_cable_in + 8;
  memcpy(gamepak_code, "BPEE", 5);
  serial_mode = SERIAL_MODE_RFU;
  nspire_link_device = LINK_DEVICE_AUTO;
  nspire_link_apply_device();
  unit_cable_nonce = 1;
  unit_cable_on = 1;
  unit_usb_plugged = 1;
  unit_watch_seen = 2;
  unit_cable_clear();
  core_link_frames(1);
  unit_watch_seen = 1;
  memset(unit_cable_in, 0, sizeof(unit_cable_in));
  memcpy(unit_cable_in, head, 8);
  memcpy(body, "GPLK", 4);
  body[4] = 1;
  body[6] = SERIAL_MODE_RFU;
  body[8] = 2;
  memcpy(body + 12, "BPEE", 4);
  unit_cable_in_len = 24;
  core_link_frames(1);
  return nspire_link_state() == LINK_LINKED;
}

static void test_review2_part(void)
{
  char part[620];
  struct stat st;
  int linked;

  CHECK(load_rom("sram.gba.tns"), "sram.gba.tns doesn't load");
  path_in_saves(sav_plain, "sram.gba.sav.tns");
  path_in_saves(sav_gz, "sram.gba.sav.gz.tns");
  remove(sav_plain);
  remove(sav_gz);
  update_backup_flag = 1;
  nspire_compress_saves = 1;
  put_file(sav_plain, 0x8000, 0x33);
  nspire_load_cartridge_backup();
  update_backup_force();
  CHECK(file_size(sav_gz) > 0, "one-go save: the old save, gzipped");
  write_backup(0x100, 0x44);
  snprintf(part, sizeof(part), "%s.part", sav_gz);
  remove(part);
  CHECK(symlink("/dev/full", part) == 0, "one-go save: the .part a link to /dev/full");
  linked = core_link_up();
  CHECK(linked, "one-go save: linked (%s)", nspire_link_status());
  if (linked)
  {
    update_backup_force();   /* (linked: written at once, save_packed_linked) */
    CHECK(lstat(part, &st) != 0, "one-go save: a one-go write that failed leaves no .part");
  }
  nspire_link_disconnect();
  core_link_frames(2);
  remove(part);
  serial_mode = SERIAL_MODE_AUTO;
  CHECK(reloaded(0x100) == 0x33, "one-go save: the old save loads");
  remove(sav_plain);
  remove(sav_gz);
}

static void test_backup_leave(void)
{
  int i, k;

  CHECK(load_rom("sram.gba.tns"), "sram.gba.tns doesn't load");
  path_in_saves(sav_plain, "sram.gba.sav.tns");
  path_in_saves(sav_gz, "sram.gba.sav.gz.tns");
  remove(sav_plain);
  remove(sav_gz);
  nspire_compress_saves = 1;
  update_backup_flag = 1;
  nspire_backup_check_failures = 0;

  /* SRAM, a plain save with Compress saves on: the first leave writes the
   * .sav.gz (the form changes), then nothing while the game doesn't save. */
  put_file(sav_plain, 0x8000, 0x33);
  nspire_load_cartridge_backup();
  CHECK(backup_type == BACKUP_SRAM && gamepak_backup[0] == 0x33, "SRAM: the plain save loads");
  update_backup_force();
  CHECK(file_size(sav_gz) > 0 && file_size(sav_plain) < 0, "SRAM: leaving packs it: gz %ld, plain %ld",
        file_size(sav_gz), file_size(sav_plain));
  check_skips(sav_gz, "SRAM, unchanged since written");
  CHECK(reloaded(0) == 0x33, "SRAM: the packed save loads");
  check_skips(sav_gz, "SRAM, unchanged since loaded");

  /* The game saves: leaving writes it, once. */
  write_backup(0x100, 0x42);
  update_backup_force();
  CHECK(rewritten(sav_gz), "SRAM, changed: leaving writes it");
  check_skips(sav_gz, "SRAM, written when leaving");
  CHECK(reloaded(0x100) == 0x42 && gamepak_backup[0x101] == 0x33, "SRAM: the change is in the file");
  age(sav_gz);
  write_backup(0x5555, 0x12);   /* SRAM at a flash command's address */
  update_backup_force();
  CHECK(rewritten(sav_gz) && reloaded(0x5555) == 0x12 && backup_type == BACKUP_SRAM,
        "SRAM at 0x5555, changed: leaving writes it");

  /* The periodic write (Automatic) puts it in sync: leaving then writes
   * nothing. */
  age(sav_gz);
  write_backup(0x101, 0x43);
  frames_backup(40);   /* (12 to the write, then its parts) */
  CHECK(rewritten(sav_gz), "SRAM: the game's save written while it runs");
  check_skips(sav_gz, "SRAM, written while it ran");
  CHECK(reloaded(0x101) == 0x43, "SRAM: the periodic write is in the file");

  /* Leaving before the periodic write: written when leaving. */
  age(sav_gz);
  write_backup(0x102, 0x44);
  update_backup();
  update_backup_force();
  CHECK(rewritten(sav_gz), "SRAM: changed, not written yet: leaving writes it");
  CHECK(reloaded(0x102) == 0x44, "SRAM: that change is in the file");

  /* Toggling Compress saves writes the other form. */
  nspire_compress_saves = 0;
  update_backup_force();
  CHECK(file_size(sav_plain) == 0x8000 && file_size(sav_gz) < 0, "Compress saves off: plain %ld, gz %ld",
        file_size(sav_plain), file_size(sav_gz));
  check_skips(sav_plain, "SRAM plain, unchanged");
  CHECK(reloaded(0x102) == 0x44, "SRAM: the plain save loads");
  check_skips(sav_plain, "SRAM plain, unchanged since loaded");
  nspire_compress_saves = 1;
  update_backup_force();
  CHECK(file_size(sav_gz) > 0 && file_size(sav_plain) < 0, "Compress saves on again: gz %ld, plain %ld",
        file_size(sav_gz), file_size(sav_plain));

  /* The save manager's conversion writes the other form; leaving then
   * writes the form Compress saves asks for, if that's the other. */
  CHECK(nspire_backup_set_compressed(0) == 1 && file_size(sav_plain) == 0x8000 && file_size(sav_gz) < 0,
        "save manager: uncompress");
  update_backup_force();
  CHECK(file_size(sav_gz) > 0 && file_size(sav_plain) < 0, "after uncompress, Compress saves on: packed again");
  nspire_compress_saves = 0;
  CHECK(nspire_backup_set_compressed(0) == 1 && file_size(sav_plain) == 0x8000, "save manager: uncompress");
  check_skips(sav_plain, "after uncompress, Compress saves off");
  CHECK(nspire_backup_set_compressed(1) == 1 && file_size(sav_gz) > 0 && file_size(sav_plain) < 0,
        "save manager: compress");
  update_backup_force();
  CHECK(file_size(sav_plain) == 0x8000 && file_size(sav_gz) < 0, "after compress, Compress saves off: plain");
  nspire_compress_saves = 1;
  update_backup_force();

  /* Both forms there (an older plain one): the newer loads, and leaving
   * writes it and removes the other. */
  age(sav_gz);
  put_file(sav_plain, 0x8000, 0x11);
  age(sav_plain);
  CHECK(reloaded(0x102) == 0x44, "both forms: the newer (packed) loads");
  update_backup_force();
  CHECK(rewritten(sav_gz) && file_size(sav_plain) < 0, "both forms: leaving writes it, the plain one goes");
  check_skips(sav_gz, "SRAM, one form again");

  /* The ROM manager renamed the running game, and the new name has a save
   * already (so its save didn't move): leaving writes the game's save by
   * the new name, as before. */
  {
    char rom[sizeof(gamepak_filename)], renamed_plain[600], renamed_gz[600];

    snprintf(rom, sizeof(rom), "%s", gamepak_filename);
    snprintf(gamepak_filename, sizeof(gamepak_filename), "%.400s/renamed.gba.tns", dir);
    nspire_backup_renamed();   /* as nspire_rom_manager.c does */
    path_in_saves(renamed_plain, "renamed.gba.sav.tns");
    path_in_saves(renamed_gz, "renamed.gba.sav.gz.tns");
    put_file(renamed_plain, 0x8000, 0x77);
    update_backup_force();
    CHECK(file_size(renamed_gz) > 0 && file_size(renamed_plain) < 0, "renamed: leaving writes it by the new name");
    CHECK(reloaded(0x102) == 0x44 && gamepak_backup[0] == 0x33, "renamed: the game's save is by the new name");
    check_skips(renamed_gz, "renamed, written");
    remove(renamed_gz);
    snprintf(gamepak_filename, sizeof(gamepak_filename), "%s", rom);
  }

  /* Deleted in the save manager: nothing written until the game saves. */
  CHECK(nspire_backup_delete() == 1 && file_size(sav_gz) < 0, "save manager: delete");
  update_backup_force();
  update_backup_force();
  CHECK(file_size(sav_gz) < 0 && file_size(sav_plain) < 0, "deleted: leaving writes nothing");
  nspire_backup_renamed();
  update_backup_force();
  CHECK(file_size(sav_gz) < 0 && file_size(sav_plain) < 0, "deleted, then renamed: leaving writes nothing");
  write_backup(0x103, 0x45);
  update_backup_force();
  CHECK(file_size(sav_gz) > 0, "deleted, then the game saved: written when leaving");
  check_skips(sav_gz, "deleted, saved, left");

  /* The periodic write fails (a folder has the file's name): no write is
   * pending any more, but leaving writes it. */
  write_backup(0x104, 0x46);
  remove(sav_gz);
  mkdir(sav_gz, 0755);
  frames_backup(40);   /* (12 to the write, then its parts) */
  CHECK(dir_exists(sav_gz), "a failed write: the folder is still there");
  remove(sav_gz);
  update_backup_force();
  CHECK(file_size(sav_gz) > 0 && reloaded(0x104) == 0x46, "a failed write: leaving writes it");
  check_skips(sav_gz, "a failed write, then left");

  /* The same while the file was in sync: the game saved, the save manager
   * wrote the other form (in sync), then the periodic write failed. */
  write_backup(0x105, 0x47);
  nspire_compress_saves = 0;
  CHECK(nspire_backup_set_compressed(0) == 1 && file_size(sav_plain) == 0x8000 && file_size(sav_gz) < 0,
        "a write pending: the save manager uncompresses it");
  remove(sav_plain);
  mkdir(sav_plain, 0755);
  frames_backup(40);   /* (12 to the write, then its parts) */
  CHECK(dir_exists(sav_plain), "a failed write after the conversion: the folder is still there");
  remove(sav_plain);
  update_backup_force();
  CHECK(file_size(sav_plain) == 0x8000 && reloaded(0x105) == 0x47,
        "a failed write after the conversion: leaving writes it");
  nspire_compress_saves = 1;

  /* A game without a save file: the first leave writes one. */
  remove(sav_gz);
  remove(sav_plain);
  nspire_load_cartridge_backup();
  CHECK(gamepak_backup[0] == 0xFF, "no save: blank");
  backup_type = BACKUP_SRAM;   /* as the game's own type (backup_type_reset) */
  update_backup_force();
  CHECK(file_size(sav_gz) > 0, "no save: the first leave writes one");
  check_skips(sav_gz, "no save, then left once");

  /* Flash, 128 KB: a byte programmed, a sector erased. */
  remove(sav_gz);
  put_file(sav_plain, 0x20000, 0x5A);
  nspire_load_cartridge_backup();
  CHECK(backup_type == BACKUP_FLASH && nspire_backup_set_compressed(1) == 1, "flash: loaded, packed");
  check_skips(sav_gz, "flash, unchanged");
  write_backup(0x5555, 0xAA);
  write_backup(0x2AAA, 0x55);
  write_backup(0x5555, 0xA0);
  write_backup(0x1234, 0x21);
  CHECK(gamepak_backup[0x1234] == 0x21, "flash: a byte programmed");
  update_backup_force();
  CHECK(rewritten(sav_gz), "flash, programmed: leaving writes it");
  CHECK(reloaded(0x1234) == 0x21 && backup_type == BACKUP_FLASH, "flash: the byte is in the file");
  check_skips(sav_gz, "flash, unchanged since loaded");
  write_backup(0x5555, 0xAA);
  write_backup(0x2AAA, 0x55);
  write_backup(0x5555, 0x80);
  write_backup(0x5555, 0xAA);
  write_backup(0x2AAA, 0x55);
  write_backup(0x1000, 0x30);
  CHECK(gamepak_backup[0x1234] == 0xFF, "flash: the sector erased");
  update_backup_force();
  CHECK(rewritten(sav_gz), "flash, erased: leaving writes it");
  CHECK(reloaded(0x1234) == 0xFF && gamepak_backup[0x2000] == 0x5A, "flash: the erase is in the file");

  /* The flash ID probe Pokemon games make at boot changes no byte: nothing
   * written, by the periodic write (Automatic) or when leaving (Exit
   * only). */
  for (i = 0; i < 2; i++)
  {
    update_backup_flag = !i;
    age(sav_gz);
    write_backup(0x5555, 0xAA);
    write_backup(0x2AAA, 0x55);
    write_backup(0x5555, 0x90);
    CHECK(read_backup(0x0000) == 0xC2 && read_backup(0x0001) == 0x09, "flash ID probe: the ID reads");
    write_backup(0x5555, 0xAA);
    write_backup(0x2AAA, 0x55);
    write_backup(0x5555, 0xF0);
    write_backup(0x0000, 0xF0);   /* (a lone "leave ID mode") */
    write_backup(0x5555, 0xF0);
    CHECK(read_backup(0x2000) == 0x5A, "flash ID probe: the memory reads again");
    frames_backup(40);
    CHECK(unchanged(sav_gz), "flash ID probe (%s): nothing written while the game runs",
          i ? "Exit only" : "Automatic");
    check_skips(sav_gz, i ? "flash ID probe, Exit only" : "flash ID probe, Automatic");
  }
  update_backup_flag = 1;

  /* The whole chip erased. */
  age(sav_gz);
  write_backup(0x5555, 0xAA);
  write_backup(0x2AAA, 0x55);
  write_backup(0x5555, 0x80);
  write_backup(0x5555, 0xAA);
  write_backup(0x2AAA, 0x55);
  write_backup(0x5555, 0x10);
  CHECK(gamepak_backup[0x2000] == 0xFF && gamepak_backup[0x1FFFF] == 0xFF, "flash: the chip erased");
  update_backup_force();
  CHECK(rewritten(sav_gz), "flash, chip erased: leaving writes it");
  CHECK(reloaded(0x2000) == 0xFF && gamepak_backup[0x1FFFF] == 0xFF && backup_type == BACKUP_FLASH,
        "flash: the chip erase is in the file");

  /* EEPROM, 8 KB: a write cut off after its address (its 8 bytes are
   * cleared first), then a whole one. */
  remove(sav_gz);
  put_file(sav_plain, 0x2000, 0x5A);
  nspire_load_cartridge_backup();
  CHECK(backup_type == BACKUP_EEPROM && eeprom_size == EEPROM_8_KBYTE && nspire_backup_set_compressed(1) == 1,
        "EEPROM: loaded, packed");
  check_skips(sav_gz, "EEPROM, unchanged");
  eeprom_bits(2, 2);       /* write */
  eeprom_bits(5, 14);      /* block 5: bytes 40-47 */
  CHECK(gamepak_backup[40] == 0 && gamepak_backup[47] == 0 && gamepak_backup[48] == 0x5A,
        "EEPROM: the block cleared by the cut-off write");
  update_backup_force();
  CHECK(rewritten(sav_gz), "EEPROM, cut-off write: leaving writes it");
  CHECK(reloaded(40) == 0 && gamepak_backup[39] == 0x5A, "EEPROM: the cleared block is in the file");
  check_skips(sav_gz, "EEPROM, unchanged since loaded");
  eeprom_bits(2, 2);
  eeprom_bits(6, 14);      /* block 6: bytes 48-55 */
  eeprom_bits(0x12345678, 32);
  eeprom_bits(0x9ABCDEF0, 32);
  eeprom_bits(0, 1);       /* the end bit */
  CHECK(gamepak_backup[48] == 0x12 && gamepak_backup[55] == 0xF0, "EEPROM: a block written");
  update_backup_force();
  CHECK(rewritten(sav_gz), "EEPROM, written: leaving writes it");
  CHECK(reloaded(48) == 0x12 && gamepak_backup[55] == 0xF0, "EEPROM: the block is in the file");

  CHECK(nspire_backup_check_failures == 0, "the save memory changed without being marked (%d times)",
        nspire_backup_check_failures);

  /* The check itself: a change nothing marks is seen. */
  printf("(on purpose: a change to the save memory that isn't marked; the check says so)\n");
  fflush(stdout);
  age(sav_gz);
  gamepak_backup[1] ^= 0xFF;
  update_backup_force();
  CHECK(nspire_backup_check_failures == 1 && unchanged(sav_gz), "the check sees an unmarked change: %d",
        nspire_backup_check_failures);
  nspire_backup_check_failures = 0;

  remove(sav_gz);
  remove(sav_plain);
}

/* Zipped ROMs: which file is the ROM, that it runs like the plain one,
 * damaged ones, sharing saves, and a big one with a small ROM buffer. */
static void test_zip(void)
{
  char path[600], name[256], rom[600];
  u32 hash_plain;
  long raw_size, page;
  u8 *raw;
  int bad = 0;

  path_in_dir(path, "arm.zip.tns");
  CHECK(zip_is_zip(path), "arm.zip.tns is a zip");
  path_in_dir(path, "arm.gba.tns");
  CHECK(!zip_is_zip(path), "arm.gba.tns isn't");
  path_in_dir(path, "arm.zip.tns");
  CHECK(zip_rom_name(path, name, sizeof(name)) && !strcmp(name, "arm.gba"), "the ROM in arm.zip: %s", name);
  path_in_dir(path, "agb.zip.tns");
  CHECK(zip_rom_name(path, name, sizeof(name)) && !strcmp(name, "game.agb"), ".agb is a ROM too: %s", name);
  /* ZIP-4: a zip with no .gba/.agb is not a ROM, whatever its biggest file is. */
  path_in_dir(path, "noext.zip.tns");
  CHECK(!zip_rom_name(path, name, sizeof(name)), "a zip of a .bin and a note has no ROM (named %s)", name);
  path_in_dir(path, "text.zip.tns");
  CHECK(!zip_rom_name(path, name, sizeof(name)), "a zip of text has no ROM");

  CHECK(load_rom("arm.gba.tns"), "arm.gba.tns doesn't load");
  run_frames(120);
  hash_plain = machine_hash();
  CHECK(load_rom("arm.zip.tns"), "arm.zip.tns doesn't load: %s", nspire_rom_load_error);
  run_frames(120);
  CHECK(machine_hash() == hash_plain, "the zipped ROM runs differently");
  CHECK(load_rom("stored.zip.tns"), "stored.zip.tns doesn't load");
  run_frames(120);
  CHECK(machine_hash() == hash_plain, "the stored zip runs differently");
  CHECK(load_rom("agb.zip.tns"), "agb.zip.tns doesn't load");
  CHECK(!load_rom("noext.zip.tns") && nspire_rom_load_error, "a zip with no .gba/.agb is refused");

  CHECK(!load_rom("bad.zip.tns") && nspire_rom_load_error, "a damaged zip is refused: %s",
        nspire_rom_load_error ? nspire_rom_load_error : "(no reason)");
  CHECK(!load_rom("text.zip.tns") && nspire_rom_load_error, "a zip without a ROM is refused");
  CHECK(load_rom("arm.zip.tns"), "a good zip loads after a bad one");

  /* Its saves are the unzipped ROM's. */
  path_in_dir(path, "arm.zip.tns");
  nspire_game_base(path, name, sizeof(name));
  path_in_saves(rom, "arm.gba");
  CHECK(!strcmp(name, rom), "zip base name %s", name);
  path_in_dir(rom, "arm.gba.tns");
  states_open_game(rom);
  remove_states();
  CHECK(states_save(4) == 1, "save 4 from the zip");
  states_open_game(path);
  CHECK(states_exists(4), "the zip sees the gba's state");
  remove_states();

  /* 8 MB zipped with a 2 MB ROM buffer: the buffer grows, nothing pages. */
  path_in_dir(path, "big.gba.tns");
  raw = read_file(path, &raw_size);
  CHECK(gamepak_buffer_count == 2, "the buffer is %u MB", gamepak_buffer_count);
  CHECK(load_rom("big.zip.tns"), "big.zip.tns doesn't load: %s", nspire_rom_load_error);
  CHECK(gamepak_buffer_count >= 8 && !gamepak_must_swap(), "buffer %u MB, swaps %d", gamepak_buffer_count,
        gamepak_must_swap());
  for (page = 0; raw && page < (raw_size + 0x7FFF) / 0x8000; page++)
  {
    long bytes = raw_size - page * 0x8000 < 0x8000 ? raw_size - page * 0x8000 : 0x8000;
    if (memcmp(gamepak_buffers[page / 32] + (page % 32) * 0x8000, raw + page * 0x8000, bytes))
      bad++;
  }
  CHECK(raw && bad == 0, "%d pages of the big zip differ", bad);
  free(raw);
}

/* ---- Where files go (nspire_paths.c) ---- */

static void write_text(const char *path, const char *text)
{
  FILE *f = fopen(path, "wb");
  if (f)
  {
    fputs(text, f);
    fclose(f);
  }
}

static int file_is(const char *path, const char *text)
{
  long size;
  u8 *data = read_file(path, &size);
  int same = data && size == (long)strlen(text) && !memcmp(data, text, size);
  free(data);
  return same;
}

static void copy_file(const char *from, const char *to)
{
  long size;
  u8 *data = read_file(from, &size);
  FILE *f = fopen(to, "wb");
  if (data && f)
    fwrite(data, 1, size, f);
  if (f)
    fclose(f);
  free(data);
}

/* ZIP-1 and ZIP-2: files that aren't whole. A plain ROM that is empty, too
 * short or cut off; one that is shorter than its blocks (the rest is zeros,
 * not what the buffer held); a .gbz with a damaged page, at load and while
 * it plays. */
static void write_bytes(const char *path, const u8 *data, long size)
{
  FILE *f = fopen(path, "wb");
  if (f)
  {
    fwrite(data, 1, size, f);
    fclose(f);
  }
}

static void flip_byte(const char *path, long offset)
{
  FILE *f = fopen(path, "r+b");
  int c;
  if (!f)
    return;
  fseek(f, offset, SEEK_SET);
  c = fgetc(f);
  fseek(f, offset, SEEK_SET);
  fputc(c ^ 0xFF, f);
  fclose(f);
}

static void test_damaged_files(void)
{
  char path[600];
  u8 *fill = (u8 *)malloc(1024 * 1024), *junk;
  long i, tail_bad = 0, size;
  u8 head[4];
  FILE *f;

  memset(fill, 0xEE, 1024 * 1024);
  path_in_dir(path, "empty.gba.tns");
  write_bytes(path, fill, 0);
  CHECK(!load_rom("empty.gba.tns") && nspire_rom_load_error, "an empty .gba is refused");
  path_in_dir(path, "short.gba.tns");
  write_bytes(path, fill, 100);
  CHECK(!load_rom("short.gba.tns") && nspire_rom_load_error, "a 100-byte .gba is refused");
  CHECK(gamepak_size == 0, "...and no game is left (size %u)", gamepak_size);

  /* A ROM much shorter than its 1 MB block: the buffer holds 0xEE from the
   * last load; what the file doesn't fill must be zeros. */
  path_in_dir(path, "fill.gba.tns");
  write_bytes(path, fill, 1024 * 1024);
  CHECK(load_rom("fill.gba.tns"), "a 1 MB .gba loads");
  path_in_dir(path, "part.gba.tns");
  memset(fill, 0x55, 100000);
  write_bytes(path, fill, 100000);
  CHECK(load_rom("part.gba.tns"), "a 100000-byte .gba loads");
  for (i = 100000; i < 1024 * 1024; i++)
    tail_bad += gamepak_buffers[0][i] != 0;
  CHECK(tail_bad == 0, "%ld bytes past the end of the file aren't zero", tail_bad);

  /* A .gbz with a flipped byte in a deflated page. */
  path_in_dir(path, "arm.gbz.tns");
  junk = read_file(path, &size);
  free(junk);
  path_in_dir(path, "dmg.gbz.tns");
  {
    char from[600];
    path_in_dir(from, "arm.gbz.tns");
    copy_file(from, path);
  }
  f = fopen(path, "rb");
  fseek(f, 32, SEEK_SET);
  if (fread(head, 1, 4, f) != 4)
    head[0] = head[1] = head[2] = 0;
  fclose(f);
  flip_byte(path, (head[0] | head[1] << 8 | head[2] << 16) + 8);
  CHECK(!load_rom("dmg.gbz.tns") && nspire_rom_load_error && strstr(nspire_rom_load_error, "damaged"),
        "a .gbz with a damaged page is refused: %s", nspire_rom_load_error ? nspire_rom_load_error : "(loaded)");
  CHECK(gamepak_size == 0, "...and no game is left (size %u)", gamepak_size);

  /* The same past the loaded 2 MB: it loads, and says so when the page is
   * swapped in. */
  {
    char from[600];
    u8 *data;
    path_in_dir(from, "big.gbz.tns");
    path_in_dir(path, "dmg2.gbz.tns");
    copy_file(from, path);
    f = fopen(path, "rb");
    fseek(f, 32 + 4 * 70, SEEK_SET);
    if (fread(head, 1, 4, f) != 4)
      head[0] = head[1] = head[2] = 0;
    fclose(f);
    flip_byte(path, (head[0] | head[1] << 8 | head[2] << 16) + 8);
    CHECK(load_rom("dmg2.gbz.tns"), "a .gbz damaged past the buffer loads: %s", nspire_rom_load_error);
    CHECK(!nspire_rom_page_damaged, "nothing damaged yet");
    data = load_gamepak_page(69);
    CHECK(data && !nspire_rom_page_damaged, "a good page swaps in quietly");
    data = load_gamepak_page(70);
    CHECK(data && nspire_rom_page_damaged && data[0] == 0xFF, "the damaged page is flagged (%u)",
          nspire_rom_page_damaged);
    nspire_rom_page_damaged = 0;
    remove(path);
  }
  free(fill);
  CHECK(load_rom("arm.gba.tns"), "a good ROM loads after these");
}

/* ZIP-6: two different games with one name don't share saves. */
static void test_same_name_games(void)
{
  char a[700], b[700], arm[600], base_a[700], base_b[700], base[700], other[700];
  u8 *rom;
  long size;

  path_in_dir(arm, "arm.gba.tns");
  rom = read_file(arm, &size);
  CHECK(rom != NULL, "arm.gba.tns reads");
  if (!rom)
    return;
  snprintf(a, sizeof(a), "%s/idA", dir);
  snprintf(b, sizeof(b), "%s/idB", dir);
  mkdir(a, 0755);
  mkdir(b, 0755);
  snprintf(a, sizeof(a), "%s/idA/rom.gba.tns", dir);
  snprintf(b, sizeof(b), "%s/idB/rom.gba.tns", dir);
  memcpy(rom + 0xAC, "AAAA", 4);
  write_bytes(a, rom, size);
  memcpy(rom + 0xAC, "BB.B", 4);
  write_bytes(b, rom, size);

  nspire_game_identify(a);
  path_in_saves(other, "rom.gba.id.tns");
  remove(other);
  nspire_game_identify(a);
  nspire_game_claim();
  CHECK(file_size(other) == 4, "the first game's id is written when it starts");
  nspire_game_base(a, base_a, sizeof(base_a));
  path_in_saves(other, "rom.gba");
  CHECK(!strcmp(base_a, other), "the first game of a name keeps it: %s", base_a);
  nspire_game_identify(b);
  nspire_game_base(b, base_b, sizeof(base_b));
  path_in_saves(other, "rom.gba~BB_B");
  CHECK(!strcmp(base_b, other), "another game of that name gets its own files: %s", base_b);
  nspire_game_identify(a);
  nspire_game_base(a, base, sizeof(base));
  CHECK(!strcmp(base, base_a), "the first game is back on its own: %s", base);

  /* A zip, a .gbz and a .gba of one game share (arm.gba's code). */
  path_in_dir(other, "arm.gba.tns");
  nspire_game_identify(other);
  nspire_game_base(other, base_a, sizeof(base_a));
  path_in_dir(other, "arm.gbz.tns");
  nspire_game_identify(other);
  nspire_game_base(other, base, sizeof(base));
  CHECK(!strcmp(base, base_a), "the .gbz shares: %s", base);
  path_in_dir(other, "arm.zip.tns");
  nspire_game_identify(other);
  nspire_game_base(other, base, sizeof(base));
  CHECK(!strcmp(base, base_a), "the zip shares: %s", base);
  nspire_game_identify("/nonexistent/none.gba.tns");   /* unreadable: no change of names */
  nspire_game_base("/nonexistent/none.gba.tns", base, sizeof(base));
  path_in_saves(other, "none.gba");
  CHECK(!strcmp(base, other), "an unreadable ROM: the plain name: %s", base);
  free(rom);
}

static void test_paths(void)
{
  char path[1300], want[1300], old[600], saved[1300];
  unsigned i;

  /* The OS's form of the current folder, joined with a name as gpSP does. */
  nspire_normalize_path(path, sizeof(path), "\\documents\\gba\\/Pokemon.zip.tns");
  CHECK(!strcmp(path, "/documents/gba/Pokemon.zip.tns"), "normalized: %s", path);
  nspire_normalize_path(path, sizeof(path), "\\documents\\gba\\");
  CHECK(!strcmp(path, "/documents/gba"), "normalized folder: %s", path);
  nspire_normalize_path(path, sizeof(path), "/");
  CHECK(!strcmp(path, "/"), "normalized root: %s", path);

  /* Game orders: one file per ROM folder, named after the folder. */
  snprintf(old, sizeof(old), "%s/gba", dir);
  nspire_order_file(path, sizeof(path), old);
  path_in_saves(want, "gpsp_order_gba.txt.tns");
  CHECK(!strcmp(path, want), "order file %s", path);
  snprintf(old, sizeof(old), "%s/gba/rpg/", dir);
  nspire_order_file(path, sizeof(path), old);
  path_in_saves(want, "gpsp_order_gba_rpg.txt.tns");
  CHECK(!strcmp(path, want), "order file %s", path);
  nspire_order_file(path, sizeof(path), dir);
  path_in_saves(want, "gpsp_order.txt.tns");
  CHECK(!strcmp(path, want), "documents' order file %s", path);
  nspire_order_file(path, sizeof(path), "\\documents\\gba\\");
  path_in_saves(want, "gpsp_order_gba.txt.tns");
  CHECK(!strcmp(path, want), "the OS's form of the folder: %s", path);

  /* What earlier builds left in a ROM folder moves in; other emulators'
   * saves, ROMs and other programs' files stay. From gpSP's own folder,
   * its settings files (older builds' too), last folder and BIOS move in. */
  {
    static const char *const moved[] = { "Game.gba.sav.tns", "Game.gba.sav.gz.tns", "Game.gba3.svs.tns",
                                         "Game.gba.settings.tns", "Zip.agb12.svs.tns" };
    static const char *const program[] = { "gpsp_qol.cfg.tns", "gpsp.cfg.tns", "Game.gba.cfg.tns",
                                           "Game.cfg.tns", "..cfg.tns", "gba_bios.bin.tns", "gpsp_romdir.tns" };
    static const char *const stay[] = { "Other.gbc.sav.tns", "Game.gba.tns", "notes.txt.tns", "ndless.cfg.tns",
                                        "Other.cfg.tns" };
    static const char *const *const lists[] = { moved, program, stay };
    static const unsigned sizes[] = { 5, 7, 5 };
    unsigned l;

    snprintf(old, sizeof(old), "%s/oldgba", dir);
    mkdir(old, 0755);
    for (l = 0; l < 3; l++)
      for (i = 0; i < sizes[l]; i++)
      {
        snprintf(path, sizeof(path), "%s/%s", old, lists[l][i]);
        write_text(path, lists[l][i]);
      }
    snprintf(path, sizeof(path), "%s/gpsp_order.txt.tns", old);
    write_text(path, "B.gba.tns\nA.gba.tns\n");
    /* One the saves folder has already wins. */
    snprintf(path, sizeof(path), "%s/Keep.gba.sav.tns", old);
    write_text(path, "old");
    path_in_saves(saved, "Keep.gba.sav.tns");
    write_text(saved, "new");

    nspire_move_old_files(old, 0);
    for (i = 0; i < sizes[0]; i++)
    {
      snprintf(path, sizeof(path), "%s/%s", old, moved[i]);
      path_in_saves(saved, moved[i]);
      CHECK(file_size(path) < 0 && file_is(saved, moved[i]), "%s moved in", moved[i]);
      remove(saved);
    }
    for (i = 0; i < sizes[1]; i++)
    {
      snprintf(path, sizeof(path), "%s/%s", old, program[i]);
      CHECK(file_is(path, program[i]), "%s moves only from gpSP's folder", program[i]);
    }
    path_in_saves(saved, "gpsp_order_oldgba.txt.tns");
    CHECK(file_is(saved, "B.gba.tns\nA.gba.tns\n"), "the folder's game order moved in");
    remove(saved);
    snprintf(path, sizeof(path), "%s/Keep.gba.sav.tns", old);
    path_in_saves(saved, "Keep.gba.sav.tns");
    CHECK(file_is(path, "old") && file_is(saved, "new"), "a save the folder has already isn't replaced");
    remove(path);
    remove(saved);

    nspire_move_old_files(old, 1);
    for (i = 0; i < sizes[1]; i++)
    {
      snprintf(path, sizeof(path), "%s/%s", old, program[i]);
      path_in_saves(saved, program[i]);
      CHECK(file_size(path) < 0 && file_is(saved, program[i]), "%s moved in from gpSP's folder", program[i]);
      remove(saved);
    }
    for (i = 0; i < sizes[2]; i++)
    {
      snprintf(path, sizeof(path), "%s/%s", old, stay[i]);
      CHECK(file_is(path, stay[i]), "%s stays", stay[i]);
      remove(path);
    }
    rmdir(old);
  }

  /* The saves folder is My Documents/zgpSP_files (the z lists it below the
   * game folders). The ones before it, gpSP_saves and zgpSP_saves, move in
   * whole and go;
   * gpSP's settings files in My Documents itself move in, others' stay. */
  {
    static const char *const moved[] = { "Game.gba.sav.gz.tns", "Game.gba14.svs.tns", "gpsp_qol.cfg.tns",
                                         "gpsp_order_gba.txt.tns", "gpsp_romdir.tns", "gba_bios.bin.tns" };
    static const char *const from_docs[] = { "..cfg.tns", "gpsp.cfg.tns", "Root.gba.cfg.tns" };
    static const char *const stay[] = { "notes.txt.tns", "Other.cfg.tns", "Game.gbc.sav.tns" };
    char old_dir[700];

    path_in_saves(path, "");
    snprintf(want, sizeof(want), "%s/zgpSP_files", dir);
    CHECK(!strcmp(path, want), "the saves folder: %s", path);

    snprintf(old_dir, sizeof(old_dir), "%s/gpSP_saves", dir);
    mkdir(old_dir, 0755);
    for (i = 0; i < 6; i++)
    {
      snprintf(path, sizeof(path), "%s/%s", old_dir, moved[i]);
      write_text(path, moved[i]);
    }
    snprintf(path, sizeof(path), "%s/Keep.gba.sav.tns", old_dir);
    write_text(path, "old");
    path_in_saves(saved, "Keep.gba.sav.tns");
    write_text(saved, "new");
    for (i = 0; i < 3; i++)
    {
      path_in_dir(path, from_docs[i]);
      write_text(path, from_docs[i]);
      path_in_dir(path, stay[i]);
      write_text(path, stay[i]);
    }

    nspire_move_old_documents_files();
    for (i = 0; i < 6; i++)
    {
      snprintf(path, sizeof(path), "%s/%s", old_dir, moved[i]);
      path_in_saves(saved, moved[i]);
      CHECK(file_size(path) < 0 && file_is(saved, moved[i]), "%s moved in from gpSP_saves", moved[i]);
      remove(saved);
    }
    snprintf(path, sizeof(path), "%s/Keep.gba.sav.tns", old_dir);
    path_in_saves(saved, "Keep.gba.sav.tns");
    CHECK(file_is(path, "old") && file_is(saved, "new"), "gpSP_saves: one the saves folder has already stays");
    CHECK(dir_exists(old_dir), "gpSP_saves stays while a file is left in it");
    for (i = 0; i < 3; i++)
    {
      path_in_dir(path, from_docs[i]);
      path_in_saves(saved, from_docs[i]);
      CHECK(file_size(path) < 0 && file_is(saved, from_docs[i]), "%s moved in from My Documents", from_docs[i]);
      remove(saved);
      path_in_dir(path, stay[i]);
      CHECK(file_is(path, stay[i]), "%s stays in My Documents", stay[i]);
      remove(path);
    }

    snprintf(path, sizeof(path), "%s/Keep.gba.sav.tns", old_dir);
    remove(path);
    remove(saved);
    nspire_move_old_documents_files();
    CHECK(!dir_exists(old_dir), "gpSP_saves goes when it's empty");
    path_in_saves(path, "");
    CHECK(dir_exists(path), "the saves folder is there");
    nspire_move_old_documents_files();   /* nothing to do: nothing changes */
    CHECK(!dir_exists(old_dir) && dir_exists(path), "a second start changes nothing");
  }

  /* zgpSP_saves (the folder's name before zgpSP_files) moves in whole and
   * goes, the same as gpSP_saves; a file both have stays the newer folder's. */
  {
    static const char *const moved[] = { "Game.gba.sav.gz.tns", "Game.gba3.svs.tns", "gpsp_qol.cfg.tns",
                                         "gba_bios.bin.tns" };
    char old_dir[700];

    snprintf(old_dir, sizeof(old_dir), "%s/zgpSP_saves", dir);
    mkdir(old_dir, 0755);
    for (i = 0; i < 4; i++)
    {
      snprintf(path, sizeof(path), "%s/%s", old_dir, moved[i]);
      write_text(path, moved[i]);
    }
    nspire_move_old_documents_files();
    for (i = 0; i < 4; i++)
    {
      snprintf(path, sizeof(path), "%s/%s", old_dir, moved[i]);
      path_in_saves(saved, moved[i]);
      CHECK(file_size(path) < 0 && file_is(saved, moved[i]), "%s moved in from zgpSP_saves", moved[i]);
      remove(saved);
    }
    CHECK(!dir_exists(old_dir), "zgpSP_saves goes once everything moved");
  }

  /* After the moves (nspire_tidy_save_folder): gpsp_qol.cfg.tns becomes
   * gpsp.cfg.tns when there is none; the BIOS moves in from My
   * Documents, else from a folder in it; one in the folder already stays. */
  {
    char sub[700], bios[700];

    path_in_saves(path, "gpsp_qol.cfg.tns");
    write_text(path, "qol");
    path_in_saves(saved, "gpsp.cfg.tns");
    write_text(saved, "older format");
    nspire_tidy_save_folder();
    CHECK(file_size(path) < 0 && file_is(saved, "qol"), "gpsp_qol.cfg.tns renamed gpsp.cfg.tns, over an older format");
    remove(saved);
    {
      char cur[300];
      memset(cur, 'c', 288);
      cur[288] = 0;
      write_text(saved, cur);
      write_text(path, "qol");
      nspire_tidy_save_folder();
      CHECK(file_size(path) < 0 && file_is(saved, cur), "SAV-3: this build's gpsp.cfg.tns kept, the old-named file dropped");
      remove(saved);
    }
    write_text(path, "qol");
    nspire_tidy_save_folder();
    CHECK(file_size(path) < 0 && file_is(saved, "qol"), "gpsp_qol.cfg.tns renamed gpsp.cfg.tns when there is none");
    nspire_tidy_save_folder();
    CHECK(file_is(saved, "qol"), "a second start leaves gpsp.cfg.tns alone");
    remove(saved);

    path_in_saves(bios, "gba_bios.bin.tns");
    path_in_dir(path, "gba_bios.bin.tns");
    write_text(path, "bios at the top");
    nspire_tidy_save_folder();
    CHECK(file_size(path) < 0 && file_is(bios, "bios at the top"), "the BIOS moved in from My Documents");
    remove(bios);

    snprintf(sub, sizeof(sub), "%s/gba", dir);
    mkdir(sub, 0755);
    snprintf(path, sizeof(path), "%s/GBA_BIOS.BIN.tns", sub);
    write_text(path, "bios in a folder");
    nspire_tidy_save_folder();
    CHECK(file_size(path) < 0 && file_is(bios, "bios in a folder"), "the BIOS moved in from a folder (any case)");

    write_text(path, "another");
    nspire_tidy_save_folder();
    CHECK(file_is(path, "another") && file_is(bios, "bios in a folder"), "a BIOS already in the folder stays");
    remove(path);
    remove(bios);
    rmdir(sub);
  }

  /* The OS's file list is read again at exit only when a name it had at
   * start is gone or something else changed; a file only added doesn't. */
  {
    path_in_saves(path, "Game.gba.sav.gz.tns");
    write_text(path, "save");
    path_in_saves(saved, "Game.gba4.svs.tns");
    nspire_file_list_untouch();
    nspire_file_list_start();
    CHECK(!nspire_file_list_changed(), "nothing changed");
    write_text(saved, "a new state");
    write_text(path, "the save written again");
    CHECK(!nspire_file_list_changed(), "a new state and a save written again: no refresh");
    remove(path);
    nspire_file_gone(path);   /* (the calculator's remove tells it: nspire_fswrap.c) */
    CHECK(nspire_file_list_changed(), "a file gone: refresh");
    write_text(path, "save");
    CHECK(!nspire_file_list_changed(), "back again: no refresh");
    remove(saved);
    nspire_file_gone(saved);
    CHECK(!nspire_file_list_changed(), "a state made in this run gone again: no refresh");
    {
      char tmp[700];
      snprintf(tmp, sizeof(tmp), "%s.tmp", path);
      write_text(tmp, "x");
      remove(tmp);
      nspire_file_gone(tmp);
      CHECK(!nspire_file_list_changed(), "a .tmp (not a .tns: the OS never lists it) gone: no refresh");
    }
    nspire_file_list_touched();
    CHECK(nspire_file_list_changed(), "touched (a move elsewhere): refresh");
    remove(path);
    remove(saved);
  }

  /* A game's files under another name (a zip's ROM named game.bin). */
  path_in_saves(path, "game.bin.sav.tns");
  write_text(path, "sram");
  path_in_saves(path, "game.bin7.svs.tns");
  write_text(path, "state");
  path_in_saves(old, "game.bin");
  path_in_saves(want, "game.gba");
  nspire_move_game_files(old, want);
  path_in_saves(path, "game.gba.sav.tns");
  path_in_saves(saved, "game.gba7.svs.tns");
  CHECK(file_is(path, "sram") && file_is(saved, "state"), "a game's files renamed");
  remove(path);
  remove(saved);
}

/* ---- Compressing and uncompressing ROMs (nspire_rom_convert.c) ---- */

static unsigned long progress_last, progress_total;
static int progress_backwards;

static unsigned long progress_stop_at;   /* the job is stopped there (0: never) */

static int note_progress(unsigned long done, unsigned long total)
{
  if (done < progress_last)
    progress_backwards = 1;
  progress_last = done;
  progress_total = total;
  return progress_stop_at && done >= progress_stop_at;
}

/* Reads a .gbz's pages: the pages it has stored as they are and deflated
 * (counted), and whether all of it unpacks to want. */
static int gbz_is(const char *path, const u8 *want, long want_size, int *stored, int *packed)
{
  static u8 page[GBZ_PAGE_SIZE];
  RFILE *file = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
  gbz_rom *rom = file ? gbz_open(file) : NULL;
  int same = rom && gbz_rom_size(rom) == (u32)want_size;
  u8 head[40];
  u32 i, pages = (u32)(want_size + GBZ_PAGE_SIZE - 1) / GBZ_PAGE_SIZE;

  *stored = *packed = 0;
  /* (fast17: then again in order with the load's read-ahead, to the last
   * page, where it lets its memory go; then page 0 again without it) */
  for (i = 0; same && i < 2 * pages + 1; i++)
  {
    u32 at = i < 2 * pages ? i % pages : 0;
    u32 n = (u32)want_size - at * GBZ_PAGE_SIZE < GBZ_PAGE_SIZE ? (u32)want_size - at * GBZ_PAGE_SIZE : GBZ_PAGE_SIZE;
    u8 *p = page + n;
    gbz_load_ahead = i >= pages && i < 2 * pages ? 256 * 1024 : 0;
    same = gbz_read_page(rom, at, page) == 0 && !memcmp(page, want + at * GBZ_PAGE_SIZE, n);
    while (same && p < page + GBZ_PAGE_SIZE)
      same = *p++ == 0;   /* the last page's end: zeros, as tools/gbz.py pads it */
  }
  gbz_load_ahead = 0;
  gbz_close(rom);
  /* The page lengths, from the table: 32768 is stored. */
  for (i = 0; same && i < pages; i++)
  {
    u32 a, b;
    filestream_seek(file, 32 + 4 * i, RETRO_VFS_SEEK_POSITION_START);
    filestream_read(file, head, 8);
    a = head[0] | head[1] << 8 | head[2] << 16 | (u32)head[3] << 24;
    b = head[4] | head[5] << 8 | head[6] << 16 | (u32)head[7] << 24;
    if (b - a == GBZ_PAGE_SIZE)
      (*stored)++;
    else
      (*packed)++;
  }
  /* The header's CRC is the ROM's (tools/gbz.py checks it). */
  filestream_seek(file, 20, RETRO_VFS_SEEK_POSITION_START);
  if (same && filestream_read(file, head, 4) == 4)
    same = (head[0] | head[1] << 8 | head[2] << 16 | (u32)head[3] << 24) ==
           (u32)crc32(crc32(0, NULL, 0), want, (uInt)want_size);
  if (file)
    filestream_close(file);
  return same;
}

/* Whether path unpacks (zip, .gbz) or reads (plain) to the bytes of want. */
static int rom_is(const char *path, const char *want)
{
  long size, want_size;
  u8 *expected = read_file(want, &want_size), *got = NULL;
  int same = 0;

  if (zip_is_zip(path))
  {
    zip_rom *z = zip_open(path);
    if (z && expected && zip_size(z) == (u32)want_size)
    {
      got = (u8 *)malloc(want_size);
      same = got && !zip_read(z, got, (u32)want_size) && !zip_check(z) && !memcmp(got, expected, want_size);
    }
    zip_close(z);
  }
  else if (rom_kind(path) == ROM_GBZ)
  {
    int stored, packed;
    same = expected && gbz_is(path, expected, want_size, &stored, &packed);
  }
  else
  {
    got = read_file(path, &size);
    same = got && expected && size == want_size && !memcmp(got, expected, size);
  }
  free(expected);
  free(got);
  return same;
}

static void test_rom_convert(void)
{
  char conv[600], arm[600], gbz[600], path[1300], out[1300], saved[1300];
  const char *why;

  path_in_dir(arm, "arm.gba.tns");
  path_in_dir(gbz, "arm.gbz.tns");
  snprintf(conv, sizeof(conv), "%s/conv", dir);
  mkdir(conv, 0755);

  /* .gba -> .gbz -> .gba */
  snprintf(path, sizeof(path), "%s/Arm.gba.tns", conv);
  copy_file(arm, path);
  CHECK(rom_kind(path) == ROM_PLAIN, "a plain ROM");
  progress_last = progress_total = 0;
  progress_backwards = 0;
  why = rom_compress(path, out, sizeof(out), note_progress);
  CHECK(!why, "compress: %s", why);
  snprintf(saved, sizeof(saved), "%s/Arm.gbz.tns", conv);
  CHECK(!strcmp(out, saved) && file_size(path) < 0, "compress: %s made, the .gba gone", out);
  CHECK(rom_kind(out) == ROM_GBZ && rom_is(out, arm), "compress: the .gbz reads as the ROM");
  CHECK(file_size(out) < file_size(arm), "compress: smaller (%ld of %ld)", file_size(out), file_size(arm));
  CHECK(!progress_backwards && progress_total == 2ul * file_size(arm) && progress_last == progress_total,
        "compress: progress to the end (%lu of %lu)", progress_last, progress_total);
  CHECK(rom_compress(out, path, sizeof(path), NULL) != NULL, "compress: a .gbz isn't compressed again");

  why = rom_uncompress(saved, out, sizeof(out), NULL);
  snprintf(path, sizeof(path), "%s/Arm.gba.tns", conv);
  CHECK(!why && !strcmp(out, path) && file_size(saved) < 0, "uncompress: %s", why ? why : out);
  CHECK(rom_is(path, arm), "uncompress: the ROM as it was");
  CHECK(rom_uncompress(path, out, sizeof(out), NULL) != NULL, "uncompress: a .gba isn't");
  remove(path);

  /* The same file as tools/gbz.py's, but for how the pages are deflated:
   * the same name, size, CRC and pages. The 8 MB ROM has pages that
   * deflate and pages (random bytes) that are stored as they are. */
  {
    char big[600], pc[600], tmp[1400];
    u8 *want;
    long want_size;
    int stored, packed;
    path_in_dir(big, "big.zip.tns");
    snprintf(path, sizeof(path), "%s/Big Game (USA).zip.tns", conv);
    copy_file(big, path);
    CHECK(rom_zip_gbz_name(path, out, sizeof(out)) == 1, "a zip's .gbz name: %s", out);
    snprintf(saved, sizeof(saved), "%s/big.gbz.tns", conv);
    CHECK(!strcmp(out, saved), "a zip's .gbz is named after the ROM in it, as gbz.py names it: %s", out);
    why = rom_compress(path, out, sizeof(out), NULL);
    CHECK(!why && !strcmp(out, saved) && file_size(path) < 0, "zip -> gbz: %s", why ? why : out);
    path_in_dir(big, "big.gba.tns");
    want = read_file(big, &want_size);
    CHECK(want && gbz_is(saved, want, want_size, &stored, &packed) && stored > 0 && packed > 0,
          "zip -> gbz: reads as the ROM, %d pages stored and %d deflated", stored, packed);
    path_in_dir(pc, "big.gbz.tns");
    CHECK(file_size(saved) > 0 && file_size(saved) < file_size(pc) * 102 / 100,
          "zip -> gbz: about tools/gbz.py's size (%ld, gbz.py's %ld)", file_size(saved), file_size(pc));
    free(want);

    /* Its .gbz there already: rom_zip_gbz_name says so, nothing changes. */
    path_in_dir(big, "big.zip.tns");
    copy_file(big, path);
    CHECK(rom_zip_gbz_name(path, out, sizeof(out)) == -1 && rom_compress(path, out, sizeof(out), NULL) != NULL &&
          file_size(path) > 0, "the .gbz there already: nothing changed");
    remove(saved);

    /* Stopped half way: nothing changed, no temporary file. */
    progress_last = progress_total = 0;
    progress_stop_at = 3 * 1024 * 1024;
    why = rom_compress(path, out, sizeof(out), note_progress);
    progress_stop_at = 0;
    snprintf(tmp, sizeof(tmp), "%s.tmp", saved);
    CHECK(why == rom_convert_cancelled && file_size(path) > 0 && file_size(saved) < 0 && file_size(tmp) < 0,
          "stopped: nothing changed: %s", why ? why : "made!");
    remove(path);
  }

  /* A zip whose ROM isn't a .gba isn't for gpSP to convert on its own;
   * the ROM manager makes it Game.gbz, and its saves follow. */
  snprintf(path, sizeof(path), "%s/NoGba.zip.tns", conv);
  path_in_dir(saved, "agb.zip.tns");
  copy_file(saved, path);
  CHECK(rom_zip_gbz_name(path, out, sizeof(out)) == 0, "a zip's game.agb: not converted on its own");
  path_in_saves(saved, "game.agb.sav.tns");
  write_text(saved, "sram");
  why = rom_compress(path, out, sizeof(out), NULL);
  snprintf(saved, sizeof(saved), "%s/game.gbz.tns", conv);
  CHECK(!why && !strcmp(out, saved) && rom_is(out, arm) && file_size(path) < 0, "zip's game.agb -> gbz: %s",
        why ? why : out);
  path_in_saves(saved, "game.gba.sav.tns");
  CHECK(file_is(saved, "sram"), "zip's game.agb -> gbz: its save follows");
  remove(saved);
  remove(out);

  /* ZIP-3: a zip with more than the ROM in it (arm.zip: a note and the ROM)
   * stays when its .gbz is made; one with only the ROM goes. */
  snprintf(path, sizeof(path), "%s/Pack.zip.tns", conv);
  path_in_dir(saved, "arm.zip.tns");
  copy_file(saved, path);
  CHECK(zip_file_count(path) == 2, "arm.zip lists %d files", zip_file_count(path));
  why = rom_compress(path, out, sizeof(out), NULL);
  snprintf(saved, sizeof(saved), "%s/arm.gbz.tns", conv);
  CHECK(!why && !strcmp(out, saved) && rom_is(out, arm), "a two-file zip -> gbz: %s", why ? why : out);
  CHECK(file_size(path) > 0 && rom_kind(path) == ROM_ZIP && rom_convert_kept_original,
        "a two-file zip stays (size %ld, kept %d)", file_size(path), rom_convert_kept_original);
  remove(path);
  remove(out);
  /* Uncompressing a two-file zip keeps it too. */
  snprintf(path, sizeof(path), "%s/Pack.zip.tns", conv);
  path_in_dir(saved, "arm.zip.tns");
  copy_file(saved, path);
  why = rom_uncompress(path, out, sizeof(out), NULL);
  CHECK(!why && rom_is(out, arm) && file_size(path) > 0 && rom_convert_kept_original, "a two-file zip -> gba: stays: %s",
        why ? why : out);
  remove(path);
  remove(out);
  snprintf(path, sizeof(path), "%s/One.zip.tns", conv);
  path_in_dir(saved, "stored.zip.tns");
  copy_file(saved, path);
  CHECK(zip_file_count(path) == 1, "stored.zip lists %d files", zip_file_count(path));
  why = rom_compress(path, out, sizeof(out), NULL);
  CHECK(!why && file_size(path) < 0 && !rom_convert_kept_original, "a one-file zip goes: %s", why ? why : out);
  remove(out);

  /* .gbz both ways */
  snprintf(path, sizeof(path), "%s/Packed.gbz.tns", conv);
  copy_file(gbz, path);
  why = rom_uncompress(path, out, sizeof(out), NULL);
  snprintf(saved, sizeof(saved), "%s/Packed.gba.tns", conv);
  CHECK(!why && !strcmp(out, saved) && rom_is(out, arm) && file_size(path) < 0, "gbz -> gba: %s", why);
  why = rom_compress(saved, out, sizeof(out), NULL);
  CHECK(!why && !strcmp(out, path) && rom_is(out, arm) && file_size(saved) < 0, "gba -> gbz: %s", why);
  remove(path);

  /* A file with the new name already there: nothing changes. */
  snprintf(path, sizeof(path), "%s/Dup.gba.tns", conv);
  copy_file(arm, path);
  snprintf(saved, sizeof(saved), "%s/Dup.gbz.tns", conv);
  write_text(saved, "mine");
  CHECK(rom_compress(path, out, sizeof(out), NULL) != NULL && rom_is(path, arm) && file_is(saved, "mine"),
        "the .gbz there already: nothing changed");
  remove(path);
  remove(saved);

  /* A zip whose ROM isn't a .gba: the .gba gets the zip's ROM's name, and
   * its saves follow. */
  snprintf(path, sizeof(path), "%s/NoGba.zip.tns", conv);
  path_in_dir(saved, "agb.zip.tns");
  copy_file(saved, path);
  path_in_saves(saved, "game.agb.sav.tns");
  write_text(saved, "sram");
  why = rom_uncompress(path, out, sizeof(out), NULL);
  snprintf(saved, sizeof(saved), "%s/game.gba.tns", conv);
  CHECK(!why && !strcmp(out, saved) && rom_is(out, arm), "a zip's game.agb: %s", why ? why : out);
  path_in_saves(saved, "game.gba.sav.tns");
  CHECK(file_is(saved, "sram"), "a zip's game.agb: its save follows");
  remove(saved);
  remove(out);

  /* A damaged zip: refused, nothing left behind. */
  snprintf(path, sizeof(path), "%s/Bad.zip.tns", conv);
  path_in_dir(saved, "bad.zip.tns");
  copy_file(saved, path);
  why = rom_uncompress(path, out, sizeof(out), NULL);
  snprintf(saved, sizeof(saved), "%s/arm.gba.tns", conv);
  CHECK(why && file_size(path) > 0 && file_size(saved) < 0, "a damaged zip: %s", why ? why : "uncompressed!");
  snprintf(saved, sizeof(saved), "%s/arm.gba.tns.tmp", conv);
  CHECK(file_size(saved) < 0, "a damaged zip: no temporary file left");
  remove(path);
  rmdir(conv);
}

/* main.c's halted_cycles (the link cable's look ahead: nspire_link.c):
 * how long the halted CPU surely stays so, from line 100 (500 cycles before
 * its hblank, or from its hblank 500 before line 101). */
extern s32 video_count;
extern timer_type timer[4];

static void halted_at(u32 dispstat, u32 line, u32 ie)
{
  unsigned i;
  reg[CPU_HALT_STATE] = CPU_HALT;
  write_ioreg(REG_DISPSTAT, dispstat);
  write_ioreg(REG_VCOUNT, line);
  write_ioreg(REG_IE, ie);
  write_ioreg(REG_IF, 0);
  video_count = 500;
  for (i = 0; i < 4; i++)
  {
    timer[i].status = TIMER_INACTIVE;
    timer[i].irq = 0;
    dma[i].start_type = DMA_INACTIVE;
    dma[i].irq = 0;
  }
}

static void dma_to(unsigned i, u32 start, u32 dest, u32 direction, u32 length, u32 size32)
{
  dma[i].start_type = start;
  dma[i].dest_address = dest;
  dma[i].dest_direction = direction;
  dma[i].length = length;
  dma[i].length_type = size32 ? DMA_32BIT : DMA_16BIT;
  dma[i].repeat_type = DMA_REPEAT;
  dma[i].irq = 0;
}

static void test_halted_cycles(void)
{
  /* From line 100's hdraw: line 101 starts in 772, the frame's end
   * (line 228) 127 lines after that. */
  const u32 line101 = 500 + 272, end = line101 + 127 * 1232;

  halted_at(0, 100, 0);
  CHECK(halted_cycles(0) == end, "nothing to wake it: the frame's end, %u", halted_cycles(0));
  reg[CPU_HALT_STATE] = CPU_ACTIVE;
  CHECK(halted_cycles(0) == 0, "running: 0, %u", halted_cycles(0));
  halted_at(0, 100, IRQ_VBLANK);
  CHECK(halted_cycles(0) == end, "vblank off in DISPSTAT: %u", halted_cycles(0));
  halted_at(0x08, 100, IRQ_VBLANK);
  CHECK(halted_cycles(0) == line101 + 59 * 1232, "vblank: line 160's start, %u", halted_cycles(0));
  halted_at(0x0A, 100, IRQ_VBLANK);
  CHECK(halted_cycles(0) == 500 + 59 * 1232, "vblank from the hblank: %u", halted_cycles(0));
  halted_at(0x08, 170, IRQ_VBLANK);
  CHECK(halted_cycles(0) == line101 + 57 * 1232, "vblank past: the frame's end, %u", halted_cycles(0));
  halted_at(0x10, 100, IRQ_HBLANK);
  CHECK(halted_cycles(0) == 500, "hblank: this line's, %u", halted_cycles(0));
  halted_at(0x12, 100, IRQ_HBLANK);
  CHECK(halted_cycles(0) == 1460, "hblank from the hblank: the next line's, %u", halted_cycles(0));
  halted_at(0x20 | (120 << 8), 100, IRQ_VCOUNT);
  CHECK(halted_cycles(0) == line101 + 19 * 1232, "vcount 120: its start, %u", halted_cycles(0));
  halted_at(0x20 | (50 << 8), 100, IRQ_VCOUNT);
  CHECK(halted_cycles(0) == end, "vcount 50: next frame, %u", halted_cycles(0));
  halted_at(0, 100, IRQ_VBLANK);
  write_ioreg(REG_IF, IRQ_VBLANK);
  CHECK(halted_cycles(0) == 0, "an interrupt waiting: 0, %u", halted_cycles(0));

  halted_at(0, 100, 0);
  CHECK(halted_cycles(1234) == 1234, "the transfer under way: %u", halted_cycles(1234));

  halted_at(0, 100, IRQ_TIMER2);
  timer[2].status = TIMER_PRESCALE;
  timer[2].count = 3000;
  CHECK(halted_cycles(0) == end, "timer 2 without its interrupt: %u", halted_cycles(0));
  timer[2].irq = 1;
  CHECK(halted_cycles(0) == 3000, "timer 2's overflow: %u", halted_cycles(0));
  halted_at(0, 100, IRQ_TIMER3);
  timer[2].status = TIMER_PRESCALE;
  timer[2].count = 3000;
  timer[3].status = TIMER_CASCADE;
  timer[3].irq = 1;
  CHECK(halted_cycles(0) == 3000, "timer 3 cascading: timer 2's overflow, %u", halted_cycles(0));
  timer[2].count = 0;
  CHECK(halted_cycles(0) == 0, "a timer past its overflow: 0, %u", halted_cycles(0));

  /* DMAs: into the display's registers, the sound's FIFO or RAM, or into
   * the serial port's or with their interrupt on. */
  halted_at(0, 100, 0);
  dma_to(0, DMA_START_HBLANK, 0x4000010, DMA_RELOAD, 2, 0);
  CHECK(halted_cycles(0) == end, "hblank DMA to the scroll: %u", halted_cycles(0));
  dma_to(0, DMA_START_HBLANK, 0x4000128, DMA_FIXED, 1, 0);
  CHECK(halted_cycles(0) == 500, "hblank DMA to SIOCNT: this hblank, %u", halted_cycles(0));
  write_ioreg(REG_VCOUNT, 170);
  CHECK(halted_cycles(0) == line101 + 57 * 1232, "... not in vblank: %u", halted_cycles(0));
  halted_at(0, 100, 0);
  dma_to(3, DMA_START_HBLANK, 0x3007F00, DMA_INCREMENT, 0x40, 0);
  CHECK(halted_cycles(0) == end, "hblank DMA through RAM: %u", halted_cycles(0));
  dma_to(3, DMA_START_HBLANK, 0x3FFFF00, DMA_INCREMENT, 0x40, 0);
  CHECK(halted_cycles(0) == 500, "... on into the registers: %u", halted_cycles(0));
  halted_at(0, 100, IRQ_DMA2);
  dma_to(2, DMA_START_VBLANK, 0x6000000, DMA_INCREMENT, 0x100, 1);
  CHECK(halted_cycles(0) == end, "vblank DMA to VRAM: %u", halted_cycles(0));
  dma[2].irq = 1;
  CHECK(halted_cycles(0) == line101 + 59 * 1232, "... with its interrupt: line 160, %u", halted_cycles(0));
  halted_at(0, 100, 0);
  timer[0].status = TIMER_PRESCALE;
  timer[0].count = 2000;
  dma_to(1, DMA_START_SPECIAL, 0x40000A0, DMA_FIXED, 4, 1);
  CHECK(halted_cycles(0) == end, "sound DMA to its FIFO: %u", halted_cycles(0));
  dma[1].dest_address = 0x4000120;
  CHECK(halted_cycles(0) == 2000, "... to SIOMULTI: timer 0's overflow, %u", halted_cycles(0));
  halted_at(0, 100, 0);
  reg[CPU_HALT_STATE] = CPU_ACTIVE;
}

cpu_alert_type function_cc write_io_register16(u32 address, u32 value);
cpu_alert_type function_cc write_io_register32(u32 address, u32 value);

/* A timer's control written while it runs (gba_memory.c trigger_timer):
 * count-up, the prescaler and the IRQ change, the count goes on. Fire
 * Emblem's link writes TM3CNT as 32 bits (reload -136, control 0xFFFF:
 * count-up), then 0xC3 (prescaler 1024, IRQ); left counting up, timer 3
 * never ends and the link stops. */
static void test_timer_control(void)
{
  CHECK(load_rom("arm.gba.tns"), "load arm.gba");
  write_io_register32(0x10C, 0xFFFFFF78);
  CHECK(timer[3].status == TIMER_CASCADE, "started counting up");
  write_io_register16(0x10E, 0xC3);
  CHECK(timer[3].status == TIMER_PRESCALE && timer[3].prescale == 10 && timer[3].irq == 1 &&
        timer[3].count == 136 << 10, "then the prescaler: status %u prescale %u count %d",
        (unsigned)timer[3].status, (unsigned)timer[3].prescale, (int)timer[3].count);

  timer[3].count = 1000 << 10;
  write_io_register16(0x10E, 0xC3);
  CHECK(timer[3].count == 1000 << 10, "the same control again doesn't restart it");
  write_io_register16(0x10E, 0x81);
  CHECK(timer[3].prescale == 6 && timer[3].count == 1000 << 6 && timer[3].irq == 0,
        "prescaler 64, no IRQ: the ticks left kept");
  write_io_register16(0x10E, 0);
  CHECK(timer[3].status == TIMER_INACTIVE, "stopped");
}

/* core-04: without audio, sound_timer only counts FIFO pops and
 * sound_flush_pops applies them in closed form. The old per-pop loop is
 * kept here as the reference: N timer overflows (at one or two steps) run
 * both ways, then the sound part of a state is written both ways and the
 * bytes compared. Steps around 1.0, fractions near 1.0, random ones. */
#ifdef NSPIRE_NO_AUDIO
static void sound_pops_old(direct_sound_struct *ds, u32 step, u32 n)
{
  while (n--)
  {
    u32 frac = ds->fifo_fractional, index = ds->buffer_index;
    ds->fifo_base = (ds->fifo_base + 1) % 32;
    while (frac <= 0xFFFFFF)
    {
      frac += step;
      index = (index + 2) % BUFFER_SIZE;
    }
    ds->buffer_index = index;
    ds->fifo_fractional = frac & 0xFFFFFF;
  }
}

#endif

/* also test_sound_fifo_dma's */
static u32 pops_rand_state = 12345;
static u32 pops_rand(void)
{
  pops_rand_state = pops_rand_state * 1103515245u + 12345u;
  return pops_rand_state >> 8;
}

#ifdef NSPIRE_NO_AUDIO

static unsigned sound_state_with(u32 frac, u32 index, u32 step1, u32 n1, u32 step2, u32 n2,
                                 int old, u8 *out)
{
  u32 i;
  reset_sound();
  direct_sound_channel[0].fifo_fractional = frac;
  direct_sound_channel[0].buffer_index = index;
  dma[1].direct_sound_channel = DMA_NO_DIRECT_SOUND;
  dma[2].direct_sound_channel = DMA_NO_DIRECT_SOUND;
  if (old)
  {
    sound_pops_old(&direct_sound_channel[0], step1, n1);
    sound_pops_old(&direct_sound_channel[0], step2, n2);
  }
  else
  {
    for (i = 0; i < n1; i++)
      sound_timer(step1, 0);
    for (i = 0; i < n2; i++)
      sound_timer(step2, 0);
  }
  return sound_write_savestate(out);
}

static void test_sound_pops(void)
{
  static u8 a[4096], b[4096];
  static const u32 steps[] = { 0xFFFFFF, 0x1000000, 0x1000001, 0xFFFFFE, 0x1000002,
                               0x800000, 0x7FFFFF, 0x2000001, 0x555555, 0x123, 0x80000000 };
  static const u32 fracs[] = { 0, 1, 0xFFFFFF, 0xFFFFFE, 0x800000, 0xFFFF00 };
  u32 si, fi, r;
  int bad = 0, cases = 0;

  for (si = 0; si < sizeof(steps) / sizeof(steps[0]); si++)
    for (fi = 0; fi < sizeof(fracs) / sizeof(fracs[0]); fi++)
    {
      u32 n = steps[si] < 0x1000 ? 3 : 1000 + si * 37;
      u32 step2 = steps[(si + 1) % (sizeof(steps) / sizeof(steps[0]))];
      u32 n2 = step2 < 0x1000 ? 2 : 300;
      unsigned la = sound_state_with(fracs[fi], 0xFFF0, steps[si], n, step2, n2, 1, a);
      unsigned lb = sound_state_with(fracs[fi], 0xFFF0, steps[si], n, step2, n2, 0, b);
      cases++;
      if (la != lb || memcmp(a, b, la))
      {
        if (bad++ < 5)
          printf("  step %06x frac %06x n %u: state bytes differ\n", steps[si], fracs[fi], n);
      }
    }

  for (r = 0; r < 3000; r++)
  {
    u32 frac = pops_rand() & 0xFFFFFF;
    u32 step1 = (pops_rand() & 0x1FFFFFF) | 0x100000;   /* 1/16 .. 2.0 */
    u32 step2 = (r & 1) ? step1 : ((pops_rand() & 0x3FFFFFF) | 0x10000);
    u32 n1 = pops_rand() % 2000, n2 = pops_rand() % 200;
    u32 index = pops_rand() & 0xFFFE;
    unsigned la = sound_state_with(frac, index, step1, n1, step2, n2, 1, a);
    unsigned lb = sound_state_with(frac, index, step1, n1, step2, n2, 0, b);
    cases++;
    if (la != lb || memcmp(a, b, la))
    {
      if (bad++ < 5)
        printf("  random %u: step %08x/%08x frac %06x n %u/%u: state bytes differ\n", r, step1,
               step2, frac, n1, n2);
    }
  }
  CHECK(bad == 0, "%d of %d pop cases write a different state", bad, cases);

  /* A timer start (adjust_sound_buffer) sets the index anew: the counted
   * pops must reach the fraction first. */
  reset_sound();
  dma[1].direct_sound_channel = DMA_NO_DIRECT_SOUND;
  dma[2].direct_sound_channel = DMA_NO_DIRECT_SOUND;
  write_io_register16(0x102, 0);
  timer[0].direct_sound_channels = TIMER_DS_CHANNEL_BOTH;
  for (r = 0; r < 7; r++)
    sound_timer(0x555555, 0);
  CHECK(direct_sound_channel[0].pops == 7, "7 pops counted: %u", direct_sound_channel[0].pops);
  write_io_register16(0x102, 0x80);   /* timer 0 starts: adjust_sound_buffer */
  /* 22 steps of 0x555555 pass 7.0 (21 are 0x6FFFFF9 short): 0x55554E left. */
  CHECK(direct_sound_channel[0].pops == 0 && direct_sound_channel[0].fifo_fractional == 0x55554E,
        "a timer start applies the pops first: %u left, frac %06x",
        direct_sound_channel[0].pops, direct_sound_channel[0].fifo_fractional);
  write_io_register16(0x102, 0);
  reset_sound();
}
#endif

/* core-06: dma_sound_fifo's short path against dma_transfer. Random sound
 * DMAs (sources in every region, at region, mirror and 32 KB page ends, all
 * strides, repeat and IRQ bits, odd FIFO tops, a few that are not sound
 * DMAs at all) run both ways from the same state; the FIFOs, the DMA
 * channels, the I/O registers (CNT_H, IF), reg[], dma_bus_val, the cycles
 * added and the result must be the same. */
extern u32 dma_bus_val;

typedef struct
{
  dma_transfer_type dma[4];
  direct_sound_struct ds[2];
  u16 io[512];
  u32 reg[64];
  u32 bus;
  int cycles;
  cpu_alert_type ret;
} fifo_dma_state;

static void fifo_state_save(fifo_dma_state *s)
{
  memcpy(s->dma, dma, sizeof(s->dma));
  memcpy(s->ds, direct_sound_channel, sizeof(s->ds));
  memcpy(s->io, io_registers, sizeof(s->io));
  memcpy(s->reg, reg, sizeof(s->reg));
  s->bus = dma_bus_val;
}

static void fifo_state_load(const fifo_dma_state *s)
{
  memcpy(dma, s->dma, sizeof(s->dma));
  memcpy(direct_sound_channel, s->ds, sizeof(s->ds));
  memcpy(io_registers, s->io, sizeof(s->io));
  memcpy(reg, s->reg, sizeof(s->reg));
  dma_bus_val = s->bus;
}

static u32 fifo_source(void)
{
  static const u32 regions[] = { 0x02, 0x02, 0x02, 0x03, 0x03, 0x03, 0x08, 0x08, 0x09, 0x0A,
                                 0x0C, 0x0D, 0x00, 0x01, 0x04, 0x05, 0x06, 0x07, 0x0E, 0x0F };
  static const u32 ends[] = { 0xFFFFF0, 0xFFFFF4, 0xFFFFFC, 0x3FFF0, 0x3FFF8, 0x7FF0, 0x7FF4,
                              0x7FFC, 0x7FEC, 0xFFF0, 0x0, 0x10, 0x17FF8, 0x1FFFF4 };
  u32 r = pops_rand(), addr = regions[r % 20] << 24;
  switch ((r >> 5) & 3)
  {
    case 0: addr |= ends[pops_rand() % 14]; break;
    case 1: addr |= pops_rand() & 0x3FFFF; break;
    default: addr |= pops_rand() & 0xFFFFFF; break;
  }
  if ((pops_rand() & 15) == 0)
    addr |= pops_rand() << 28;   /* high bits dma_transfer masks off */
  if ((pops_rand() & 7) == 0)
    addr |= pops_rand() & 3;     /* low bits likewise */
  return addr;
}

static void test_sound_fifo_dma(void)
{
  static const u32 dests[] = { 0x40000A0, 0x40000A4, 0x40000A0, 0x40000A4, 0x40000A2,
                               0x40000A7, 0xF40000A0, 0x40000A8, 0x40000AC, 0x40000A1 };
  fifo_dma_state start, a, b;
  u32 r, i;
  int bad = 0, cases = 0, usual = 0;

  CHECK(load_rom("arm.gba.tns"), "load arm.gba");
  for (i = 0; i < 0x40000; i++)
    ewram[i] = pops_rand();
  for (i = 0; i < 0x8000; i++)
    iwram[0x8000 + i] = pops_rand();

  for (r = 0; r < 20000; r++)
  {
    unsigned n = 1 + (pops_rand() & 1);
    dma_transfer_type *d = &dma[n];
    int odd = (pops_rand() & 7) == 0;

    for (i = 0; i < 2; i++)
    {
      u32 k;
      for (k = 0; k < 32; k++)
        direct_sound_channel[i].fifo[k] = pops_rand();
      direct_sound_channel[i].fifo_top = pops_rand() & 31;
      direct_sound_channel[i].fifo_base = pops_rand() & 31;
    }
    for (i = 0; i < 512; i++)
      io_registers[i] = pops_rand();
    dma_bus_val = pops_rand() ^ (pops_rand() << 16);

    d->source_address = fifo_source();
    d->dest_address = dests[odd ? pops_rand() % 10 : pops_rand() % 4];
    d->length = odd && (pops_rand() & 1) ? pops_rand() % 9 : 4;
    d->repeat_type = pops_rand() & 1;
    d->direct_sound_channel = d->dest_address == 0x40000A4;
    d->source_direction = (pops_rand() & 3) == 0 ? pops_rand() & 3 : ((pops_rand() & 1) ? 0 : 2);
    d->dest_direction = odd && (pops_rand() & 1) ? pops_rand() & 3 : DMA_FIXED;
    d->length_type = odd && (pops_rand() & 1) ? DMA_16BIT : DMA_32BIT;
    d->start_type = odd && (pops_rand() & 1) ? pops_rand() % 5 : DMA_START_SPECIAL;
    d->irq = pops_rand() & 1;
    if (!odd && d->source_direction != DMA_DECREMENT && d->source_direction != DMA_RELOAD &&
        ((d->source_address >> 24) & 15) - 2 < 2)
      usual++;

    fifo_state_save(&start);
    a.cycles = b.cycles = pops_rand() & 0xFFF;
    a.ret = dma_transfer(n, &a.cycles);
    fifo_state_save(&a);
    fifo_state_load(&start);
    b.ret = dma_sound_fifo(n, &b.cycles);
    fifo_state_save(&b);
    cases++;
    if (a.ret != b.ret || a.cycles != b.cycles || a.bus != b.bus ||
        memcmp(a.dma, b.dma, sizeof(a.dma)) || memcmp(a.ds, b.ds, sizeof(a.ds)) ||
        memcmp(a.io, b.io, sizeof(a.io)) || memcmp(a.reg, b.reg, sizeof(a.reg)))
    {
      if (bad++ < 5)
        printf("  case %u: chan %u src %08x dst %08x len %u dir %u/%u size %u start %u: differ"
               " (cycles %d/%d bus %08x/%08x)\n", r, n, start.dma[n].source_address,
               start.dma[n].dest_address, start.dma[n].length, start.dma[n].source_direction,
               start.dma[n].dest_direction, start.dma[n].length_type, start.dma[n].start_type,
               a.cycles, b.cycles, a.bus, b.bus);
    }
  }
  CHECK(bad == 0, "%d of %d sound FIFO DMAs differ from dma_transfer", bad, cases);
  CHECK(usual > 3000,"the usual case (EWRAM/IWRAM, +1 or 0) came %d times", usual);
  reg[CPU_HALT_STATE] = CPU_ACTIVE;
  for (i = 0; i < 4; i++)
  {
    dma[i].start_type = DMA_INACTIVE;
    dma[i].direct_sound_channel = DMA_NO_DIRECT_SOUND;
  }
  reset_sound();
}

static int trim_test_cache(void)
{
  return gamepak_buffer_count <= 2;
}

/* A 32 MB game's upper third mirror must not retain pointers to ROM
 * blocks reclaimed for save memory when the next, smaller game loads. */
static void test_save_memory_rom_mappings(void)
{
  char path[600], source[600];
  FILE *f;
  u32 page, hash, before;
  int stale = 0;

  CHECK(load_rom("arm.gba.tns"), "save room: reference ROM loads");
  run_frames(30);
  hash = machine_hash();
  path_in_dir(source, "arm.gba.tns");
  path_in_dir(path, "save-room-32.gba.tns");
  copy_file(source, path);
  f = fopen(path, "r+b");
  CHECK(f != NULL, "save room: create 32 MB ROM");
  if (!f)
    return;
  CHECK(fseek(f, 32 * 1024 * 1024 - 1, SEEK_SET) == 0 && fputc(0, f) != EOF,
        "save room: extend ROM");
  fclose(f);
  CHECK(load_rom("save-room-32.gba.tns"), "save room: 32 MB ROM loads");
  before = gamepak_buffer_count;
  for (page = 1024 - before * 32; page < 1024; page++)
    load_gamepak_page(page);
  CHECK(memory_map_read[0xDFF8000 / 0x8000] != NULL,
        "save room: upper third mirror mapped before switch");
  nspire_rom_prepare_saves = trim_test_cache;
  CHECK(load_rom("arm.gba.tns"), "save room: smaller ROM loads with cache reclaimed");
  nspire_rom_prepare_saves = NULL;
  CHECK(before > 2 && gamepak_buffer_count == 2, "save room: cache shrank from %u to %u MB", before,
        gamepak_buffer_count);
  for (page = 0xD000000 / 0x8000; page < 0xE000000 / 0x8000; page++)
    stale += memory_map_read[page] != NULL;
  CHECK(stale == 0, "save room: %d stale upper-mirror pointers after switch", stale);
  run_frames(30);
  CHECK(machine_hash() == hash, "save room: smaller game runs identically after shrinking");
  remove(path);
}

extern u32 rtc_status, rtc_state;

static void rtc_tx_start(void)
{
  write_memory16(0x080000C6, 7);
  write_memory16(0x080000C4, 1); /* CS=0, SCK=1 */
  write_memory16(0x080000C4, 5); /* CS=1, SCK=1: enter RTC_COMMAND */
}

static void rtc_tx_end(void)
{
  write_memory16(0x080000C4, 0); /* CS=0: back to RTC_IDLE */
}

static void rtc_tx_bit(int bit)
{
  u16 d = (u16)((bit & 1) << 1);
  write_memory16(0x080000C4, (u16)(4 | d)); /* CLK=0 */
  write_memory16(0x080000C4, (u16)(5 | d)); /* CLK=1 (rising edge) */
}

static void rtc_tx_cmd(u8 cmd)
{
  int i;
  for (i = 7; i >= 0; i--)
    rtc_tx_bit((cmd >> i) & 1);
}

static void rtc_tx_data(u8 val)
{
  int i;
  for (i = 0; i < 8; i++)
    rtc_tx_bit((val >> i) & 1);
}

static u8 rtc_rx_data(void)
{
  u8 out = 0;
  int i;
  write_memory16(0x080000C6, 5); /* CLK/CS out, DAT in */
  for (i = 0; i < 8; i++)
  {
    write_memory16(0x080000C4, 4);
    write_memory16(0x080000C4, 5);
    out |= (u8)(((read_memory16(0x080000C4) >> 1) & 1) << i);
  }
  return out;
}

static void test_rtc_gpio(void)
{
  char path[600];
  u8 *saved = (u8 *)calloc(1, GBA_STATE_MEM_SIZE);
  u8 *doc;
  u32 page;
  u8 st;

  CHECK(saved != NULL, "allocate RTC state buffer");
  if (!saved)
    return;

  path_in_dir(path, "big.gbz.tns");
  CHECK(load_gamepak(NULL, path, FEAT_ENABLE, FEAT_AUTODETECT, SERIAL_MODE_AUTO) == 0,
        "big.gbz.tns loads with RTC enabled");
  reset_gba();
  CHECK(gamepak_must_swap(), "big.gbz.tns pages with 2 MB ROM buffer");
  CHECK(rtc_status == 0x40, "reset_gba sets rtc_status=0x40 (got 0x%02x)", (unsigned)rtc_status);

  write_memory16(0x080000C8, 1); /* GPIO readable */

  /* WRITE_STATUS (0x62) LSB-first: 0x00, then asymmetric 0x48, then 0x40. */
  rtc_tx_start();
  rtc_tx_cmd(0x62);
  rtc_tx_data(0x00);
  rtc_tx_end();
  CHECK(rtc_status == 0x00, "WRITE_STATUS 0x00 (got 0x%02x)", (unsigned)rtc_status);

  /* RESET (0x60) completes without a data byte and sets rtc_status = 0x40. */
  rtc_tx_start();
  rtc_tx_cmd(0x60);
  CHECK(rtc_status == 0x40 && rtc_state == 1, "RESET 0x60 sets 0x40 and IDLE (got 0x%02x, state %u)",
        (unsigned)rtc_status, (unsigned)rtc_state);
  rtc_tx_end();

  rtc_tx_start();
  rtc_tx_cmd(0x62);
  rtc_tx_data(0x48);
  rtc_tx_end();
  CHECK(rtc_status == 0x48, "WRITE_STATUS 0x48 LSB-first (got 0x%02x)", (unsigned)rtc_status);

  rtc_tx_start();
  rtc_tx_cmd(0x63);
  st = rtc_rx_data();
  rtc_tx_end();
  CHECK(st == 0x48, "READ_STATUS reads back 0x48 (got 0x%02x)", (unsigned)st);

  rtc_tx_start();
  rtc_tx_cmd(0x62);
  rtc_tx_data(0x40);
  rtc_tx_end();
  CHECK(rtc_status == 0x40, "WRITE_STATUS 0x40 LSB-first stays 0x40 (got 0x%02x)", (unsigned)rtc_status);

  rtc_tx_start();
  rtc_tx_cmd(0x63);
  st = rtc_rx_data();
  rtc_tx_end();
  CHECK(st == 0x40, "READ_STATUS reads back 0x40 (got 0x%02x)", (unsigned)st);

  /* init_memory() on reset_gba() restores rtc_status=0x40 and clears GPIO ROM regs. */
  rtc_tx_start();
  rtc_tx_cmd(0x62);
  rtc_tx_data(0x00);
  rtc_tx_end();
  reset_gba();
  CHECK(rtc_status == 0x40 && read_memory16(0x080000C8) == 0,
        "reset_gba restores rtc_status=0x40 and clears GPIO ROM regs");

  /* memory_read_savestate() repairs legacy rtc-status=2 and updates GPIO ROM regs. */
  write_memory16(0x080000C8, 1);
  write_memory16(0x080000C6, 7);
  write_memory16(0x080000C4, 5);
  gba_save_state(saved);
  doc = (u8 *)bson_find_document(saved, "backup");
  CHECK(doc != NULL, "backup document present in state");
  if (doc)
    state_set_word((u8 *)bson_find_key(doc, "rtc-status"), 2);
  reset_gba();
  CHECK(read_memory16(0x080000C8) == 0, "GPIO regs cleared before state load");
  CHECK(gba_load_state(saved), "legacy rtc-status=2 state loads");
  CHECK(rtc_status == 0x40, "legacy rtc-status=2 repaired to 0x40 (got 0x%02x)", (unsigned)rtc_status);
  CHECK(read_memory16(0x080000C4) == 5 && read_memory16(0x080000C6) == 7 && read_memory16(0x080000C8) == 1,
        "GPIO regs restored in ROM page 0 after state load");

  /* Evicting and reloading ROM page 0 preserves active GPIO regs via update_gpio_romregs(). */
  for (page = 1; page <= 100; page++)
    load_gamepak_page(page);
  CHECK(memory_map_read[0x8000000 >> 15] == NULL, "ROM page 0 evicted");
  CHECK(read_memory16(0x080000C4) == 5 && read_memory16(0x080000C6) == 7 && read_memory16(0x080000C8) == 1,
        "page 0 reload preserves GPIO regs");

  free(saved);
}

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    fprintf(stderr, "usage: core_tests DIR\n");
    return 2;
  }
  snprintf(dir, sizeof(dir), "%s", argv[1]);
  snprintf(unit_documents_dir, sizeof(unit_documents_dir), "%s/", dir);
  nspire_save_dir_make();

  nspire_rom_buffer_size_choice = 2;
  init_gamepak_buffer();
  init_sound();
  memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));
  gba_screen_pixels = (u16 *)calloc(1, GBA_SCREEN_BUFFER_SIZE);

  printf("the halted CPU's time\n");
  test_halted_cycles();
  printf("game base names\n");
  test_game_base();
  printf("compressed ROM pages\n");
  test_gbz_pages();
  test_gbz_ahead_heap();
  printf("compressed ROM runs\n");
  test_gbz_runs();
  printf("damaged files\n");
  test_damaged_files();
  printf("same-name games\n");
  test_same_name_games();
  printf("save states\n");
  test_states();
  test_states_stepwise();
  test_states_changes();
  test_states_queue();
  test_review2_saves();
  test_states_kept();
  test_save_rollback();
  printf("in-game saves\n");
  test_bson_array();
  test_bson_structure();
  test_state_validation();
  test_cheat_bounds();
  test_serial_packet_bounds();
  test_rfu_command_bounds();
  test_fastdeflate_oom();
  test_states_memory();
  test_states_saved_time();
  test_backup();
  printf("in-game saves written when leaving only if changed\n");
  test_backup_leave();
  test_backup_due_again();
  test_backup_cut();
  test_review2_part();
  printf("where files go\n");
  test_paths();
  printf("compressing ROMs\n");
  test_rom_convert();
  printf("timer control\n");
  test_timer_control();
#ifdef NSPIRE_NO_AUDIO
  printf("sound FIFO pops\n");
  test_sound_pops();
#endif
  printf("sound FIFO DMA\n");
  test_sound_fifo_dma();
  printf("RTC and GPIO\n");
  test_rtc_gpio();
  printf("zipped ROMs\n");
  test_zip();   /* last: the big zip grows the ROM buffer */
  test_save_memory_rom_mappings();

  memory_term();
  printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
