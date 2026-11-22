/*
 * The rival gpSP (Vogtinator's gpSP 0.9 Nspire port) played headless for the
 * ARM926 speed model, as tests/arm/perf.c plays ours:
 *
 *   perf_rival ROM BIOS SAVE|- FRAMES KEYS|- DRAW [SETTING=VALUE...]
 *
 * From power-on (save states don't carry between the two emulators), with
 * SAVE (an in-game save, .sav or gzipped) as the cartridge's backup. DRAW: 1
 * draws every frame, 0 none, N one in N. Settings: scale=0/1/2 (its unscaled /
 * scaled 3:2 / fullscreen, default 1: scaled 3:2), every=N (a hash of every
 * Nth frame), ppm=DIR (the screen buffer at those frames). perf_frame_mark
 * marks each frame for the model (tests/calc_speed.py). Its frames end inside
 * its update_gba (synchronize, here), which never returns: the run ends there.
 */
#include "common.h"
#include <zlib.h>

/* What its nspire.c and gui.c/input.c would have defined. */
u16 extra_screen_1[320 * 240] __attribute__((aligned(32)));
u16 extra_screen_2[320 * 240] __attribute__((aligned(32)));
u16 extra_screen_3[320 * 240] __attribute__((aligned(32)));
u16 extra_screen_4[320 * 240] __attribute__((aligned(32)));
void *nspire_displayed_screen = extra_screen_1, *nspire_screen = extra_screen_2;
void *nspire_screen_2 = extra_screen_3, *nspire_screen_3 = extra_screen_4;
u16 *os_screen;

void nspire_init(void) {}
void nspire_restore(void) {}
void set_display_buffer(void *buffer) { (void)buffer; }
void update_at_vblank(void) {}
/* The CP15 cache operations (no-ops here: qemu keeps code coherent). On the
 * calculator these cost a little per translated block and per picture. */
int warm_cache_op_range(int op, void *addr, unsigned long size) { (void)op; (void)addr; (void)size; return 0; }
void clean_dcache(void) {}
void enableAlignmentExceptions(void) {}

static u32 lcd_lines;
/* The LCD's vblank flag, which its update_gba reads every line: about every
 * 263 lines at 60 Hz. */
int rival_lcd_vblank(void)
{
  return (++lcd_lines % 263) == 0;
}

/* Its gui.c and input.c, not built: what the core calls of them. */
s32 load_game_config_file(void) { return -1; }
s32 load_config_file(void) { return -1; }
s32 save_config_file(void) { return 0; }
u32 menu(u16 *original_screen) { (void)original_screen; return 0; }
void init_input(void) {}
void get_savestate_filename_noshot(u32 slot, u8 *name) { sprintf((char *)name, "slot%u.svs", (unsigned)slot); }
void input_read_savestate(file_tag_type savestate_file) { (void)savestate_file; }
void input_write_mem_savestate(file_tag_type savestate_file) { (void)savestate_file; }
s32 load_file(u8 **wildcards, u8 *result) { (void)wildcards; (void)result; return -1; }
gui_action_type get_gui_input(void) { return CURSOR_NONE; }
u32 save_backup(char *name) { (void)name; return 1; }
int chdir(const char *path) { (void)path; return 0; }

void __attribute__((noinline)) perf_frame_mark(void)
{
  __asm__ volatile ("" ::: "memory");
}

struct key_span { int from, to; u32 keys; };
static struct key_span spans[4096];
static int nspans, frames, frame, draw_every, hash_every = 50;
static const char *ppm_dir;

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
  while (fgets(line, sizeof(line), f) && nspans < 4096)
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
      if (i < 10)
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

static u32 keys_at(int n)
{
  u32 keys = 0;
  int i;
  for (i = 0; i < nspans; i++)
    if (n >= spans[i].from && n < spans[i].to)
      keys |= spans[i].keys;
  return keys;
}

/* Its update_input: the script's keys for the next frame. */
u32 update_input(void)
{
  io_registers[REG_P1] = (~keys_at(frame + 1)) & 0x3FF;
  return 0;
}

static u32 screen_hash(void)
{
  u32 h = 2166136261u, i;
  const u8 *p = (const u8 *)nspire_displayed_screen;
  for (i = 0; i < 320 * 240 * 2; i++)
    h = (h ^ p[i]) * 16777619u;
  return h;
}

/* The screen buffer last shown (landscape 5:5:5 BGR, as its LCD mode). */
static void write_ppm(int n)
{
  char path[600];
  const u16 *s = (const u16 *)nspire_displayed_screen;
  FILE *f;
  int i;
  snprintf(path, sizeof(path), "%s/r%05d.ppm", ppm_dir, n);
  f = fopen(path, "wb");
  if (!f)
    return;
  fprintf(f, "P6\n320 240\n255\n");
  for (i = 0; i < 320 * 240; i++)
  {
    u16 c = s[i];
    unsigned char rgb[3] = { (unsigned char)((c & 31) << 3), (unsigned char)(((c >> 5) & 31) << 3),
                             (unsigned char)(((c >> 10) & 31) << 3) };
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
}

/* Its frame end (update_gba, line 228): this frame counted, the next one's
 * drawing decided; the run ends after the last. */
void synchronize(void)
{
  frame++;
  if (frame % hash_every == 0 || frame == frames)
  {
    printf("frame %d %08x\n", frame, (unsigned)screen_hash());
    if (ppm_dir)
      write_ppm(frame);
  }
  if (frame >= frames)
  {
    perf_frame_mark();
    exit(0);
  }
  perf_frame_mark();
  skip_next_frame = draw_every == 0 || ((frame + 1) % draw_every) != 0;
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

static const char *text_setting(int argc, char **argv, const char *name)
{
  size_t n = strlen(name);
  int i;
  for (i = 7; i < argc; i++)
    if (!strncmp(argv[i], name, n) && argv[i][n] == '=')
      return argv[i] + n + 1;
  return NULL;
}

/* memory.c's (not in its headers). */
extern u8 gamepak_backup[1024 * 128];
enum { BACKUP_SRAM, BACKUP_FLASH, BACKUP_EEPROM, BACKUP_NONE };
enum { SRAM_SIZE_32KB, SRAM_SIZE_64KB };
enum { FLASH_SIZE_64KB, FLASH_SIZE_128KB };
enum { EEPROM_512_BYTE, EEPROM_8_KBYTE };
extern int backup_type, sram_size, flash_size, eeprom_size;
extern u8 main_path[];
void init_main(void);
void trigger_ext_event(void);

/* An in-game save (plain or gzipped) as its load_backup would take it. */
static int load_save(const char *path)
{
  gzFile f = gzopen(path, "rb");
  int size;
  if (!f)
    return 0;
  size = gzread(f, gamepak_backup, 1024 * 128);
  gzclose(f);
  switch (size)
  {
  case 0x200: backup_type = BACKUP_EEPROM; eeprom_size = EEPROM_512_BYTE; break;
  case 0x2000: backup_type = BACKUP_EEPROM; eeprom_size = EEPROM_8_KBYTE; break;
  case 0x8000: backup_type = BACKUP_SRAM; sram_size = SRAM_SIZE_32KB; break;
  case 0x10000: backup_type = BACKUP_FLASH; sram_size = FLASH_SIZE_64KB; break;
  case 0x20000: backup_type = BACKUP_FLASH; flash_size = FLASH_SIZE_128KB; break;
  default: return 0;
  }
  return 1;
}

int main(int argc, char **argv)
{
  if (argc < 7)
  {
    fprintf(stderr, "usage: perf_rival ROM BIOS SAVE|- FRAMES KEYS|- DRAW [SETTING=VALUE...]\n");
    return 2;
  }
  frames = atoi(argv[4]);
  draw_every = atoi(argv[6]);
  hash_every = setting(argc, argv, "every", 50);
  ppm_dir = text_setting(argc, argv, "ppm");
  if (!read_keys(argv[5]))
    return 2;

  /* Its calls into the OS (the save) switch to the stack its main kept. */
  {
    extern u32 not_crt0_savedsp;
    u32 sp;
    __asm__ volatile ("mov %0, sp" : "=r" (sp));
    not_crt0_savedsp = sp - 4096;
  }
  /* Its game_config.txt (idle loops, save types) from the build folder. */
  strcpy((char *)main_path, RIVAL_CONFIG_DIR);
  init_gamepak_buffer();
  {
    extern u32 gamepak_ram_buffer_size;
    fprintf(stderr, "rival: ROM buffer %u KB\n", (unsigned)(gamepak_ram_buffer_size >> 10));
  }
  init_video();
  if (load_bios(argv[2]) == -1)
  {
    fprintf(stderr, "can't read the BIOS %s\n", argv[2]);
    return 2;
  }
  init_main();
  init_sound();
  current_frameskip_type = manual_frameskip;
  frameskip_value = 0;
  screen_scale = setting(argc, argv, "scale", scaled_aspect);
  if (load_gamepak(argv[1]) == -1U)
  {
    fprintf(stderr, "can't load %s\n", argv[1]);
    return 2;
  }
  set_gba_resolution(screen_scale);
  video_resolution_small();
  init_cpu();
  init_memory();
  {
    extern u32 idle_loop_target_pc;
    fprintf(stderr, "rival: %s, flash id %x, idle loop %08x\n", gamepak_title, (unsigned)flash_device_id,
            (unsigned)idle_loop_target_pc);
  }
  if (strcmp(argv[3], "-") && !load_save(argv[3]))
  {
    fprintf(stderr, "can't read the save %s\n", argv[3]);
    return 2;
  }
  skip_next_frame = draw_every == 0 || 1 % (draw_every ? draw_every : 1) != 0;
  perf_frame_mark();
  trigger_ext_event();
  execute_arm_translate(execute_cycles);
  return 0;
}
