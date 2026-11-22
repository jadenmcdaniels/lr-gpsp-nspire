/*
 * The cartridge clock (RTC) as Pokemon Emerald sees it, on gpSP's core as
 * the calculator runs it (the ARM dynarec, the ROM page cache) under
 * qemu-arm: the game is started (from power-on, or from a save state and
 * then started over as gui.c's "Start over" does) and run to its menus,
 * then what it made of the clock is printed.
 *
 *   rtc_test ROM BIOS STATE|- RTC ROMBUF_MB RESET CLOCK FRAMES KEYS [PPMDIR]
 *
 * RTC: auto, on or off (the Cartridge clock setting, as
 * nspire_rtc_force_value gives it to load_gamepak). RESET: N starts the
 * game over (reset_gba, as gui.c's Start over) before frame N (1: right
 * after STATE is loaded; 0: never). CLOCK: the calculator's clock
 * (time(), Unix seconds; the CX II's reads the date, the CX's was reset to
 * 1970). KEYS as replay.c's ("-": none). PPMDIR: every 100th frame and
 * the last as pictures.
 *
 * It prints, at the end:
 *   rtc status SS error EEEE probe PP time YY MM DD WD HH MM SS ST
 * SS: the clock chip's status register (0x40: 24-hour mode); EEEE: the
 * game's RTC error flags (pokeemerald's rtc.c sErrorStatus, IWRAM
 * 0x03000DB8 in Emerald (USA, Europe): 0 when the clock works, 0x10 its
 * 12-hour flag, any of 0x0FF0: "The internal battery has run dry"); PP:
 * what the game's probe of the chip found at its start (sProbeResult,
 * 0x03000DCC: 0x01 good, 0x11 it had to reset the chip); then the game's
 * copy of the clock (sRtc, 0x03000DC0: BCD year, month, day, weekday,
 * hour, minute, second, and its status: 0x40 is 24-hour). The state's line
 * ("state rtc status SS error EEEE") gives the same as the state has them.
 * tests/arm/run.sh checks them.
 */

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

extern u32 nspire_rom_buffer_size_choice, nspire_dynarec_ram_policy;
extern u16 *gba_screen_pixels;
extern u32 execute_cycles;
extern u32 rtc_status;
void init_gamepak_buffer(void);

/* The calculator's clock: time() as Ndless gives it (its RTC's seconds). */
static time_t clock_now;
time_t time(time_t *t)
{
  if (t)
    *t = clock_now;
  return clock_now;
}

struct key_span { int from, to; u32 keys; };
static struct key_span spans[256];
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
  while (fgets(line, sizeof(line), f) && nspans < 256)
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

/* A state as gpSP writes it (replay.c's). */
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

/* Emerald's sErrorStatus (rtc.c), IWRAM 0x03000DB8: RtcGetErrorStatus at
 * 0x0802F27C reads it */
static unsigned game_rtc_error(void)
{
  return iwram[0x8000 + 0xDB8] | (iwram[0x8000 + 0xDB9] << 8);
}

static void write_ppm(const char *dir, int frame)
{
  char path[600];
  FILE *f;
  int x, y;

  snprintf(path, sizeof(path), "%s/r%05d.ppm", dir, frame);
  f = fopen(path, "wb");
  if (!f)
    return;
  fprintf(f, "P6\n240 160\n255\n");
  for (y = 0; y < 160; y++)
    for (x = 0; x < 240; x++)
    {
      u16 c = gba_screen_pixels[y * GBA_SCREEN_PITCH + x];
      unsigned char rgb[3] = { (unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2),
                               (unsigned char)((c & 31) << 3) };
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
}

int main(int argc, char **argv)
{
  const char *ppm_dir;
  int frames, frame, force_rtc, reset_at;
  u32 last_status;
  const u8 *game_rtc;

  if (argc < 10)
  {
    fprintf(stderr, "usage: rtc_test ROM BIOS STATE|- RTC ROMBUF_MB RESET CLOCK FRAMES KEYS [PPMDIR]\n");
    return 2;
  }
  if (!strcmp(argv[4], "auto"))
    force_rtc = FEAT_AUTODETECT;
  else if (!strcmp(argv[4], "on"))
    force_rtc = FEAT_ENABLE;
  else if (!strcmp(argv[4], "off"))
    force_rtc = FEAT_DISABLE;
  else
  {
    fprintf(stderr, "RTC is auto, on or off\n");
    return 2;
  }
  nspire_rom_buffer_size_choice = atoi(argv[5]);
  nspire_dynarec_ram_policy = 0;
  clock_now = (time_t)strtoul(argv[7], NULL, 10);
  frames = atoi(argv[8]);
  ppm_dir = argc > 10 && strcmp(argv[10], "-") ? argv[10] : NULL;
  if (!read_keys(argv[9]))
  {
    fprintf(stderr, "can't read keys %s\n", argv[9]);
    return 2;
  }

  init_gamepak_buffer();
  init_sound();
  gba_screen_pixels = calloc(1, GBA_SCREEN_BUFFER_SIZE);
  if (load_bios((char *)argv[2]) != 0)
  {
    fprintf(stderr, "can't read the BIOS %s\n", argv[2]);
    return 2;
  }
  if (load_gamepak(NULL, argv[1], force_rtc, FEAT_AUTODETECT, SERIAL_MODE_AUTO) != 0)
  {
    fprintf(stderr, "can't load %s\n", argv[1]);
    return 2;
  }
  reset_gba();
  /* As the calculator does without a save file (nspire_backup.c). */
  memset(gamepak_backup, 0xFF, sizeof(gamepak_backup));
  if (strcmp(argv[3], "-"))
  {
    if (!load_state_file(argv[3]))
    {
      fprintf(stderr, "can't load the state %s\n", argv[3]);
      return 2;
    }
    printf("state rtc status %02x error %04x\n", (unsigned)rtc_status, game_rtc_error());
  }
  reset_at = atoi(argv[6]);
  last_status = rtc_status;
  for (frame = 1; frame <= frames; frame++)
  {
    if (frame == reset_at)
    {
      /* gui.c's Start over */
      printf("frame %d: start over\n", frame);
      reset_gba();
      instruction_count = 0;
      reg[OAM_UPDATED] = 1;
    }
    write_ioreg(REG_P1, (~keys_at(frame)) & 0x3FF);
    execute_arm_translate(execute_cycles);
    if (nspire_bad_jump)
    {
      printf("frame %d stopped at %08X\n", frame, (unsigned)nspire_bad_jump_pc);
      return 3;
    }
    if (rtc_status != last_status)
    {
      /* the game wrote the chip's status (its reset of the chip: a new
       * game, or a chip found in 12-hour mode at its start) */
      printf("frame %d: rtc status %02x\n", frame, (unsigned)rtc_status);
      last_status = rtc_status;
    }
    if (ppm_dir && (frame % 100 == 0 || frame == frames))
      write_ppm(ppm_dir, frame);
  }
  if (ppm_dir)
  {
    char path[600];
    FILE *f;
    snprintf(path, sizeof(path), "%s/iwram.bin", ppm_dir);
    f = fopen(path, "wb");
    if (f)
    {
      fwrite(&iwram[0x8000], 1, 0x8000, f);
      fclose(f);
    }
  }
  game_rtc = &iwram[0x8000 + 0xDC0];
  printf("rtc status %02x error %04x probe %02x time %02x %02x %02x %02x %02x %02x %02x %02x\n",
         (unsigned)rtc_status, game_rtc_error(), iwram[0x8000 + 0xDCC], game_rtc[0], game_rtc[1], game_rtc[2],
         game_rtc[3], game_rtc[4], game_rtc[5], game_rtc[6], game_rtc[7]);
  return 0;
}
