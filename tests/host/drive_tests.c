/* Real BOT/FatFs, stdio cookie, archive reader and GBA memory loader over
 * a fake removable disk. No fake ROM reads or fake filesystem parser. */
#include "common.h"
#include "nspire_drive.h"
#include "nspire_frontend.h"
#include "nspire_states.h"
#include "nspire_paths.h"
#include "nspire_places.h"
#include "nspire_drive_saves.h"
#include "streams/file_stream.h"
#include "usbms.h"
#include "tests/fake_drive.h"
#include <zlib.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

extern char unit_documents_dir[600], unit_osd_message[64];
extern int unit_osd_count;
extern u32 gamepak_buffer_count;
extern u8 *gamepak_buffers[32];
int nspire_usbms_active(void);
void *__real_malloc(size_t size);
static int mb_budget = -1;
void *__wrap_malloc(size_t size)
{
  if (size == 1024 * 1024 && mb_budget >= 0)
  {
    if (!mb_budget) return NULL;
    mb_budget--;
  }
  return __real_malloc(size);
}
static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)

static unsigned long memory_crc(unsigned bytes)
{
  unsigned long h = crc32(0, NULL, 0);
  for (unsigned i = 0; bytes; i++)
  {
    unsigned n = bytes > 1024 * 1024 ? 1024 * 1024 : bytes;
    h = crc32(h, gamepak_buffers[i], n); bytes -= n;
  }
  return h;
}
static unsigned long file_crc(const char *name, unsigned *size)
{
  FILE *f = fopen(name, "rb");
  unsigned char b[65536];
  unsigned long h = crc32(0, NULL, 0);
  size_t n;
  *size = 0;
  if (!f) return 0;
  while ((n = fread(b, 1, sizeof b, f))) { h = crc32(h, b, n); *size += n; }
  fclose(f);
  return h;
}
static void ready(const char *image)
{
  CHECK(fake_plug(image) == 0);
  nspire_drive_poll();
  CHECK(nspire_drive_state() == DRIVE_ATTACHED && nspire_usbms_active());
  nspire_drive_menu_tick();
  CHECK(nspire_drive_state() == DRIVE_READY);
}
static int load(const char *name)
{
  nspire_drive_game_begin(name);
  int okay = load_gamepak(NULL, name, FEAT_AUTODETECT, FEAT_AUTODETECT, SERIAL_MODE_AUTO) == 0;
  nspire_drive_game_end(okay);
  return okay;
}
int main(int argc, char **argv)
{
  if (argc != 4) return 2; /* IMAGE EXPECTED_ROM DOCUMENTS */
  unsigned bytes;
  unsigned long expected = file_crc(argv[2], &bytes);
  CHECK(bytes > 2 * 1024 * 1024 && bytes < 32 * 1024 * 1024);
  char same_name[700]; unsigned internal_bytes;
  snprintf(same_name, sizeof same_name, "%s/Test.gba", argv[3]);
  CHECK(file_crc(same_name, &internal_bytes) != expected && internal_bytes == bytes);
  snprintf(unit_documents_dir, sizeof unit_documents_dir, "%s/", argv[3]);
  nspire_save_dir_make();
  setenv("GPSP_USB_IMAGE", argv[1], 1);
  nspire_drive_start();
  nspire_drive_menu_tick();
  CHECK(nspire_drive_state() == DRIVE_READY);
  nspire_rom_buffer_size_choice = 2;
  init_gamepak_buffer(); init_sound();
  memcpy(bios_rom, open_gba_bios_rom, sizeof bios_rom);
  gba_screen_pixels = calloc(1, GBA_SCREEN_BUFFER_SIZE);

  CHECK(nspire_drive_path("usb:/GAMES/Test.gba"));
  CHECK(!nspire_drive_path("usb:foo.gba"));
  CHECK(nspire_drive_is_game("Long name.GBA.tns"));
  CHECK(nspire_drive_is_game("Test.zip"));
  CHECK(!nspire_drive_is_game("notes.txt"));
  CHECK(nspire_drive_fopen("usb:/GAMES/Test.gba", "wb") == NULL);
  CHECK(nspire_drive_fopen("usb:/GAMES/Test.gba", "r+") == NULL);
  CHECK(nspire_drive_fopen("usb:/GAMES/Test.gba", "ab") == NULL);
  CHECK(filestream_open("usb:/GAMES/Test.gba", RETRO_VFS_FILE_ACCESS_WRITE,
                       RETRO_VFS_FILE_ACCESS_HINT_NONE) == NULL);
  RFILE *hinted = filestream_open("usb:/GAMES/Test.gba", RETRO_VFS_FILE_ACCESS_READ,
                                  RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS);
  CHECK(hinted && filestream_get_size(hinted) == bytes);
  if (hinted) filestream_close(hinted);
  char **games, **dirs; unsigned ng, nd;
  CHECK(nspire_drive_list("/GAMES", &games, &ng, &dirs, &nd) == 0);
  CHECK(ng == 4 && nd == 1);
  for (unsigned i = 0; i < ng; i++) free(games[i]);
  for (unsigned i = 0; i < nd; i++) free(dirs[i]);
  free(games); free(dirs);

  FILE *cookie = nspire_drive_fopen("usb:/GAMES/Test.gba", "rb");
  CHECK(cookie != NULL);
  if (cookie)
  {
    CHECK(fseek(cookie, 0, SEEK_END) == 0 && ftell(cookie) == bytes);
    CHECK(fseek(cookie, LONG_MAX, SEEK_CUR) < 0);
    CHECK(fseek(cookie, -1, SEEK_SET) < 0);
    CHECK(fseek(cookie, 0, SEEK_SET) == 0);
    unsigned char b[4];
    CHECK(fread(b, 1, sizeof b, cookie) == sizeof b);
    fake_unplug();
    CHECK(fread(b, 1, sizeof b, cookie) == 0 && ferror(cookie));
    fclose(cookie);
    ready(argv[1]);
  }

  mb_budget = 1;
  CHECK(!load("usb:/GAMES/Test.gba"));
  CHECK(gamepak_buffer_count == 2 && gamepak_size == 0);
  CHECK(nspire_rom_load_need_mb == (bytes + 1048575) / 1048576);
  CHECK(nspire_rom_load_error && strstr(nspire_rom_load_error, "USB game"));
  mb_budget = -1;

  const char *names[] = { "usb:/GAMES/Test.gba", "usb:/GAMES/Test.gbz.tns", "usb:/GAMES/Test.zip",
                         "usb:/GAMES/Long name with spaces.gba" };
  for (unsigned i = 0; i < 4; i++)
  {
    if (i) ready(argv[1]);
    nspire_game_identify(names[i]);
    CHECK(load(names[i]));
    CHECK(!gamepak_must_swap());
    CHECK(memory_crc(bytes) == expected);
    nspire_game_claim();
    reset_gba();
    unsigned before = fake_commands();
    fake_unplug();
    int notes = unit_osd_count;
    nspire_drive_frame();
    CHECK(nspire_drive_state() == DRIVE_NONE && !nspire_usbms_active());
    CHECK(unit_osd_count == notes + 1 && strstr(unit_osd_message, "memory"));
    nspire_drive_frame();
    CHECK(unit_osd_count == notes + 1);
    for (unsigned p = 0; p < (bytes + 32767) / 32768; p++)
      (void)read_memory32(0x08000000 + p * 32768);
    execute_arm(execute_cycles);
    CHECK(memory_crc(bytes) == expected && fake_commands() == before);
    char base[600];
    nspire_game_base(names[i], base, sizeof base);
    CHECK(!strncmp(base, argv[3], strlen(argv[3])) && strstr(base, SAVE_FOLDER));
    states_open_game(names[i]);
    CHECK(states_save(i) == 1 && states_load(i) == 1);
    if (!i)
    {
      backup_type = BACKUP_SRAM;
      memset(gamepak_backup, 0x71, 0x8000);
      nspire_compress_saves = 0;
      update_backup_force();
      long saved; int compressed;
      CHECK(nspire_backup_info(&saved, &compressed) && saved == 0x8000 && !compressed);
      memset(gamepak_backup, 0, 0x8000);
      nspire_load_cartridge_backup();
      CHECK(gamepak_backup[0] == 0x71 && gamepak_backup[0x7fff] == 0x71);
      CHECK(!strncmp(backup_filename, argv[3], strlen(argv[3])));
      nspire_config_open_game(names[i]);
      CHECK(nspire_config_save() == 0);
      nspire_config_close_game();
    }
    CHECK(fake_commands() == before);
  }

  /* ---- 2026-11-17 (final/usbsave8): saves and states on the drive ----
   * The real states and save code over the fake drive, written to: the
   * drive's writes are kept in memory (fake_scratch), so the image file
   * stays as it is (the sha256 after this program). Here: the place's table
   * of functions as the menus call it (nspire_places.h), what gpSP does by
   * itself with Saves on: USB drive, and what a drive pulled out in the
   * middle of a write leaves, at each of the write's commands; the
   * frontend's scenarios are drive_save_tests.py's. */
  {
    const struct nspire_place_ops *ops = nspire_drive_saves;
    const char *game = names[3];
    const char *slot1 = "usb:/gpsp/states/Long name with spaces.gba0.svs";
    const char *sav = "usb:/gpsp/saves/Long name with spaces.gba.sav";
    static u16 pic[STATE_PIC_W * STATE_PIC_H];
    char path[700];
    unsigned long n_first = 0, n = 0;
    unsigned char *first, *got;
    int cut, fell = 0, others = 0, to = -1, calc_before, on_drive = 0, slot;

    fake_scratch(1);
    ready(argv[1]);
    CHECK(ops != NULL);
    nspire_saves_on = PLACE_CALCULATOR;
    nspire_game_identify(game);
    states_open_game(game);
    for (slot = 0; slot < STATE_SLOTS; slot++)        /* (an earlier run's, in the same folder) */
      if (slot != 3 && states_exists(slot))
        states_delete(slot);
    calc_before = states_count();
    CHECK(calc_before == 1 && states_exists(3));     /* (the loop above saved the calculator's slot 4) */

    /* the menus' questions: the drive is read at the first one (a tab switch), then they are answered
     * from memory: not a command more */
    CHECK(ops->ready() == 1);
    CHECK(ops->next() == 0 && !ops->exists(0) && ops->time(0) == 0);
    {
      unsigned before = fake_commands();
      CHECK(ops->next() == 0 && !ops->exists(0) && !ops->exists(7) && ops->picture(0, pic) == 0 && ops->ready() == 1);
      CHECK(fake_commands() == before);
    }
    /* Save on the drive's tab: the state there, the toast, nothing of it on the calculator */
    CHECK(ops->save(0) == 1 && ops->exists(0) && ops->next() == 1);
    CHECK(strstr(unit_osd_message, "Saved to slot 1 (USB drive)") != NULL && strlen(unit_osd_message) < 64);
    CHECK(states_count() == calc_before);
    first = nspire_drive_get(slot1, 1 << 20, &n_first);
    CHECK(first && n_first > 64 && !memcmp(first, "GPSZ", 4));
    CHECK(ops->time(0) > 0 && ops->picture(0, pic) >= 0);
    CHECK(ops->load(0) == 1 && ops->load(5) == 0);
    CHECK(ops->save(0) == 0 && ops->exists(0));     /* never over a state there */
    got = nspire_drive_get(slot1, 1 << 20, &n);
    CHECK(got && n == n_first && !memcmp(got, first, n));
    free(got);
    /* the game's name is its own on the drive from its first file there */
    got = nspire_drive_get("usb:/gpsp/saves/Long name with spaces.gba.id", 16, &n);
    CHECK(got && n == 4);
    free(got);
    {
      /* asked again once it is known: from memory (a picture that is kept, the times, the slots) */
      unsigned before = fake_commands();
      CHECK(ops->exists(0) && ops->next() == 1 && ops->time(0) > 0 && ops->picture(0, pic) >= 0);
      CHECK(fake_commands() == before);
    }

    /* c: a slot copied to the other place's next free slot, the same bytes; again: the slot that
     * has it, nothing written */
    CHECK(ops->copy(PLACE_CALCULATOR, 3, &to) == 1 && to == 1 && ops->exists(1) && ops->next() == 2);
    {
      FILE *f;
      static unsigned char calc[1 << 20];
      size_t calc_n;
      states_file_name(3, path, sizeof path);
      f = fopen(path, "rb");
      calc_n = f ? fread(calc, 1, sizeof calc, f) : 0;
      if (f) fclose(f);
      got = nspire_drive_get("usb:/gpsp/states/Long name with spaces.gba1.svs", 1 << 20, &n);
      CHECK(got && calc_n == n && !memcmp(got, calc, n));
      free(got);
    }
    CHECK(ops->copy(PLACE_CALCULATOR, 3, &to) == 1 && to == 1 && ops->next() == 2);
    CHECK(ops->copy(PLACE_DRIVE, 0, &to) == 1 && to == 4 && states_exists(4) && states_count() == calc_before + 1);
    CHECK(ops->copy(PLACE_DRIVE, 0, &to) == 1 && to == 4 && states_count() == calc_before + 1);
    CHECK(ops->copy(PLACE_DRIVE, 9, &to) == 0 && ops->copy(PLACE_CALCULATOR, 9, &to) == 0);
    CHECK(states_load(4) == 1);

    /* save17: the drive's slot 0 stored, then packed again (states_drive_set_compressed): the same state,
     * the same picture, its save time kept in its header and as the strip says it */
    {
      static unsigned char raw_a[425984], raw_b[425984];
      unsigned char *packed, *stored, *again;
      unsigned long np = 0, ns = 0, na = 0;
      unsigned u32_at = 0, flags = 0, psize = 0, pic_a = 0, pic_b = 0;
      uLongf rn = sizeof raw_a;
      unsigned first_flags = 0;
      memcpy(&first_flags, first + 20, 4);
      CHECK(states_drive_set_compressed(0, 1) == 1);      /* (packed, whichever way it was saved) */
      packed = nspire_drive_get(slot1, 1 << 20, &np);
      CHECK(packed && np > 32);
      if (packed)
      {
        memcpy(&u32_at, packed + 28, 4);
        memcpy(&psize, packed + 16, 4);
        memcpy(&pic_a, packed + 24, 4);
        CHECK(u32_at != 0 && (unsigned long)ops->time(0) == u32_at && !memcmp(first + 28, packed + 28, 4));
        CHECK(uncompress(raw_a, &rn, packed + 32, psize) == Z_OK && rn == sizeof raw_a);
      }
      CHECK(states_drive_set_compressed(0, 0) == 1);
      stored = nspire_drive_get(slot1, 1 << 20, &ns);
      CHECK(stored && ns >= 32 + sizeof raw_a);
      if (stored && packed)
      {
        unsigned at;
        memcpy(&flags, stored + 20, 4);
        memcpy(&at, stored + 28, 4);
        memcpy(&pic_b, stored + 24, 4);
        CHECK((flags & 2) && at == u32_at && pic_b == pic_a && !memcmp(stored + 32, raw_a, sizeof raw_a));
        CHECK(!memcmp(stored + 32 + sizeof raw_a, packed + 32 + psize, pic_a));
      }
      CHECK(states_drive_set_compressed(0, 0) == 1);     /* (stored already: nothing written) */
      CHECK(states_drive_set_compressed(0, 1) == 1);
      again = nspire_drive_get(slot1, 1 << 20, &na);
      rn = sizeof raw_b;
      if (again)
      {
        unsigned at;
        memcpy(&flags, again + 20, 4);
        memcpy(&at, again + 28, 4);
        memcpy(&psize, again + 16, 4);
        CHECK(!(flags & 2) && at == u32_at && uncompress(raw_b, &rn, again + 32, psize) == Z_OK &&
              !memcmp(raw_a, raw_b, sizeof raw_a));
      }
      CHECK((unsigned long)ops->time(0) == u32_at && ops->load(0) == 1);
      CHECK(states_drive_set_compressed(9, 1) == 0 && states_failure(STATES_SAVING)->why == STATES_WHY_DRIVE);
      /* as it was saved, for what follows (its size counts there) */
      CHECK(states_drive_set_compressed(0, !(first_flags & 2)) == 1);
      got = nspire_drive_get(slot1, 1 << 20, &n);
      CHECK(got && n == n_first && !memcmp(got, first, n));
      free(got);
      free(packed);
      free(stored);
      free(again);
    }

    /* Save on the drive's tab with the drive leaving at each command of the write: the menus are
     * told it did not (their message); every slot the drive lists is whole and loads, the first
     * as it was; no .tmp is listed as a slot or left beside one; with the drive back the same save
     * works. A cut in the write's last commands (the folder's: the new name) leaves the state
     * there whole, though the menus were told it was not saved: it is taken away again here, so
     * that each round starts from the same folder (with 512-byte clusters a save's commands grow
     * with the folder). */
    {
      int there = 0;
      for (cut = 1; cut < 400; cut++)
      {
        int r, s, top;
        slot = ops->next();
        if (slot != 2) { CHECK(0); break; }
        fake.gone_after = fake_commands() + cut;
        r = ops->save(slot);
        fake.gone_after = 0;
        if (r == 1)
        {
          CHECK(ops->exists(slot) && ops->load(slot) == 1);
          break;   /* (every command of the write went through) */
        }
        fell++;
        if (ops->ready() || ops->exists(0))
          others++;                                  /* (gone: no place for saves, nothing listed) */
        ready(argv[1]);
        top = ops->next();
        if (top != 2 && top != 3)
          others++;
        for (s = 0; s < top; s++)
          if (!ops->exists(s) || ops->load(s) != 1)
            others++;
        got = nspire_drive_get(slot1, 1 << 20, &n);
        if (!got || n != n_first || memcmp(got, first, n))
          others++;
        free(got);
        snprintf(path, sizeof path, "usb:/gpsp/states/Long name with spaces.gba%d.svs.tmp", slot);
        if (nspire_drive_stat(path, NULL, NULL))
          others++;
        if (top == 3)
        {
          there++;
          path[strlen(path) - 4] = 0;
          if (!nspire_drive_remove(path))
            others++;
          fake_unplug();
          nspire_drive_poll();
          ready(argv[1]);
        }
      }
      printf("  Save on the drive's tab, the drive pulled out at each of %d commands: not saved %d times (the "
             "state there whole all the same %d); a slot not whole or the first one changed %d times\n",
             cut - 1, fell, there, others);
    }
    CHECK(fell > 5 && others == 0 && cut < 400);
    CHECK(states_count() == calc_before + 1);       /* (the calculator's slots: none of it) */
    {
      /* what the first question after a drive is put in costs (the menu opening, a tab switch):
       * the folder's names, the newest state's header and picture; then nothing */
      unsigned before, first;
      fake_unplug();
      nspire_drive_poll();
      ready(argv[1]);
      before = fake_commands();
      CHECK(ops->ready() == 1 && ops->next() == 3 && ops->exists(2));
      first = fake_commands() - before;
      before = fake_commands();
      CHECK(ops->next() == 3 && ops->exists(0) && ops->time(1) > 0 && ops->picture(2, pic) >= 0);
      printf("  the drive's 3 states asked for the first time: %u commands; again: %u\n", first, fake_commands() - before);
      CHECK(first > 0 && first < 60 && fake_commands() == before);
    }

    /* ---- Saves on: USB drive: what gpSP does by itself ---- */
    nspire_saves_on = PLACE_DRIVE;
    states_open_game(game);
    nspire_saves_game_start();
    CHECK(nspire_saves_on_drive() && nspire_saves_line() == NULL);
    /* Resume last save: the newest of both places by their counts: the drive's (saved last) */
    slot = nspire_saves_resume(&on_drive);
    CHECK(on_drive == 1 && slot == ops->next() - 1 && states_drive_load(slot) == 1);
    /* a state the frames save: the calculator's slot, as always; it is the newest now; at the safe
     * point it is copied to the drive's next slot, and nothing on the drive is asked before that */
    {
      unsigned before = fake_commands();
      int next = ops->next();
      CHECK(states_save(6) == 1);
      nspire_saves_frame_saved(6);
      CHECK(fake_commands() == before && ops->next() == next);
      slot = nspire_saves_resume(&on_drive);
      CHECK(on_drive == 0 && slot == 6);
      nspire_saves_sync(1);
      CHECK(ops->next() == next + 1 && ops->exists(next) && states_exists(6));
      CHECK(nspire_saves_line() == NULL);
      before = fake_commands();
      nspire_saves_sync(1);                         /* nothing waits: not a command */
      CHECK(fake_commands() == before && ops->next() == next + 1);
      /* the same with the drive pulled out in that copy: the state is the calculator's all the same,
       * said once; with the drive back it is copied, and that is said */
      CHECK(states_save(7) == 1);
      nspire_saves_frame_saved(7);
      fake.gone_after = fake_commands() + 3;
      nspire_saves_sync(1);
      fake.gone_after = 0;
      CHECK(states_exists(7) && states_load(7) == 1 && !nspire_saves_on_drive());
      {
        const char *line = nspire_saves_line();
        CHECK(line && !strcmp(line, "No USB drive: saves on the calculator") && nspire_saves_line() == NULL);
      }
      nspire_saves_sync(1);
      CHECK(nspire_saves_line() == NULL);           /* (still out: not said again) */
      ready(argv[1]);
      next = ops->next();
      nspire_saves_sync(0);                         /* (on the way out of a game too) */
      CHECK(ops->next() == next + 1 && nspire_saves_on_drive());
      {
        const char *line = nspire_saves_line();
        CHECK(line && !strcmp(line, "Saves are on the USB drive again"));
      }
    }
    /* Save when I quit: on the drive, its newest; the next start resumes from it */
    {
      int next = ops->next();
      CHECK(nspire_saves_exit_save() == 1 && ops->next() == next + 1);
      states_open_game(game);
      nspire_saves_game_start();
      slot = nspire_saves_resume(&on_drive);
      CHECK(on_drive == 1 && slot == next);
      /* with the drive out: not saved there (the caller saves on the calculator); that state, saved
       * in a run without the drive, is the newest when the drive is back (its count is above the
       * drive's, kept beside the calculator's save) */
      fake_unplug();
      nspire_drive_poll();
      states_open_game(game);
      nspire_saves_game_start();
      CHECK(nspire_saves_exit_save() == 0 && !nspire_saves_on_drive());
      CHECK(nspire_saves_resume(&on_drive) == 7 && on_drive == 0);
      CHECK(states_save(8) == 1);
      {
        const char *line = nspire_saves_line();
        CHECK(line && !strcmp(line, "No USB drive: saves on the calculator"));
      }
      ready(argv[1]);
      states_open_game(game);
      nspire_saves_game_start();
      slot = nspire_saves_resume(&on_drive);
      CHECK(on_drive == 0 && slot == 8);
      CHECK(ops->next() == next + 1);
    }

    /* the game's save: its copy on the drive written at the safe point, when it changed; the drive
     * pulled out at each command of that write: the calculator has the new save, the drive's is the
     * old one or the new one, whole */
    backup_type = BACKUP_SRAM;
    memset(gamepak_backup, 0x33, 0x8000);
    nspire_load_cartridge_backup();          /* (the game's start: no save of it yet) */
    nspire_backup_drive_load();
    backup_type = BACKUP_SRAM;
    memset(gamepak_backup, 0x33, 0x8000);
    nspire_backup_mark_dirty();
    nspire_saves_sync(1);
    got = nspire_drive_get(sav, 1 << 20, &n);
    CHECK(got && n == 0x8000 && got[0] == 0x33 && got[0x7fff] == 0x33);
    free(got);
    {
      unsigned before = fake_commands();
      nspire_saves_sync(1);
      CHECK(fake_commands() == before);      /* nothing changed: not a command sent */
    }
    fell = others = 0;
    (void)nspire_saves_line();
    for (cut = 1; cut < 400; cut++)
    {
      unsigned char want = (unsigned char)(0x40 + cut);
      long bytes; int packed;
      const char *line;
      if (!nspire_saves_on_drive()) { CHECK(0); break; }
      memset(gamepak_backup, want, 0x8000);
      nspire_backup_mark_dirty();
      fake.gone_after = fake_commands() + cut;
      nspire_saves_sync(1);
      fake.gone_after = 0;
      if (nspire_saves_on_drive())
      {
        got = nspire_drive_get(sav, 1 << 20, &n);
        CHECK(got && n == 0x8000 && got[0] == want && got[0x7fff] == want);
        free(got);
        break;
      }
      fell++;
      line = nspire_saves_line();
      if (!nspire_backup_info(&bytes, &packed) || !line || strcmp(line, "No USB drive: saves on the calculator"))
        others++;
      ready(argv[1]);
      got = nspire_drive_get(sav, 1 << 20, &n);
      if (!got)
      {
        /* between the old file's removal and the rename: the whole new one as its .tmp (taken at the next start) */
        got = nspire_drive_get("usb:/gpsp/saves/Long name with spaces.gba.sav.tmp", 1 << 20, &n);
        if (!got || got[0] != want)
          others++;
      }
      if (!got || n != 0x8000 || memcmp(got, got + 1, 0x7fff))
        others++;
      free(got);
      /* back in: the two saves are made one again (the calculator's is the newer: copied) */
      nspire_saves_sync(1);
      got = nspire_drive_get(sav, 1 << 20, &n);
      if (!nspire_saves_on_drive() || !got || n != 0x8000 || got[0] != want)
        others++;
      free(got);
      (void)nspire_saves_line();
    }
    printf("  the save's write to the drive, the drive pulled out at each of %d commands: %d times, the drive's "
           "copy or the calculator's not whole %d times\n", cut - 1, fell, others);
    CHECK(fell > 5 && others == 0 && cut < 400);
    free(first);
    nspire_saves_on = PLACE_CALCULATOR;
    states_open_game(game);
    nspire_saves_game_start();
    CHECK(!nspire_saves_on_drive() && ops->ready() == 1);
    fake_scratch(0);
  }

  ready(argv[1]);
  fake.gone_after = fake_commands() + 8;
  CHECK(!load("usb:/GAMES/Test.gba"));
  CHECK(strstr(nspire_drive_load_error(nspire_rom_load_error), "removed"));
  fake.gone_after = 0;
  nspire_drive_stop();
  memory_term();
  free(gba_screen_pixels); gba_screen_pixels = NULL;
  printf("drive: %d checks, %d failed\n", checks, failures);
  return failures != 0;
}
