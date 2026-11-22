/* r19: a game's cheat codes.
 *
 * What it gives: each code with its own on/off switch, and the codes read from a file on the calculator or
 * on a USB flash drive.
 * Until now the engine was there (libretro-gpsp/cheats.c: CodeBreaker codes, "XXXXXXXX YYYY") and nothing
 * loaded a code into it: num_cheats was a stub 0 and the Cheats page never showed.
 *
 * THE FILE is RetroArch's .cht (libretro-database ships one a game):
 *     cheats = 74
 *     cheat0_desc = "Max Money"
 *     cheat0_code = "82000568+423F+8200056A+000F"
 *     cheat0_enable = false
 * Its name starts with the game's (the name of its file without ".gba"), in any case, and ends ".cht" or
 * ".cht.tns": "Pokemon - Emerald Version (USA, Europe) (Code Breaker).cht" is found for "Pokemon - Emerald
 * Version (USA, Europe).gba.tns". It is looked for, in this order, in: the game's own folder (the calculator's
 * or the drive's), the calculator's saves folder (zgpSP_files), and on a USB drive its top folder, "cheats"
 * and "zgpSP_files". The first folder that has one wins; in a folder, the exact name before a longer one.
 *
 * A code the engine can't run (an Action Replay or GameShark one: "XXXXXXXX YYYYYYYY", or a mistyped one) is
 * left out and counted. Which codes are on is the file's own cheatN_enable lines: they are written back when
 * the page is left and when the game is, so there is no other file.
 * At most NSPIRE_CHEATS_ON_MAX codes are on at once (the engine's slots). */
#include "common.h"
#include "cheats.h"
#include "input.h"
#include "nspire.h"
#include "nspire_cheats.h"
#include "nspire_drive.h"
#include "nspire_frontend.h"
#include "nspire_keys.h"
#include "nspire_paths.h"
#include "nspire_ui_screens.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define CODES_MAX 160             /* the codes kept (the file's first that the engine runs) */
#define FILE_CODES_MAX 400        /* the file's cheatN lines read: N under this */
#define FILE_MAX (128ul * 1024)   /* a cheat file is 10-30 KB */

struct code
{
  char name[48];
  u32 at;          /* its code's place in text */
  u32 enable_at;   /* its cheatN_enable line's value there (0: the file has none) */
  u16 length, enable_length;
  u16 number;      /* its N */
  u8 on;
};

static struct code codes[CODES_MAX];
static int count, skipped, changed;
static char *text;                /* the file, as read (not NUL-ended: text_size bytes) */
static unsigned long text_size;
static char path[600];            /* where it is (DRIVE_PREFIX...: on the drive) */
static char game[600];            /* the game it is for; "" none */
static unsigned looked_epoch;
static int looked_drive;

int nspire_cheats_count(void) { return count; }
int nspire_cheats_skipped(void) { return skipped; }
const char *nspire_cheats_name(int i) { return i >= 0 && i < count ? codes[i].name : ""; }
int nspire_cheats_is_on(int i) { return i >= 0 && i < count && codes[i].on; }

int nspire_cheats_on_count(void)
{
  int i, n = 0;
  for (i = 0; i < count; i++)
    n += codes[i].on;
  return n;
}

/* The engine's slots as the switches stand. */
static void apply(void)
{
  char line[1024];
  int i, slot = 0;
  cheat_clear();
  for (i = 0; i < count; i++)
  {
    if (!codes[i].on)
      continue;
    snprintf(line, sizeof(line), "%.*s", (int)codes[i].length, text + codes[i].at);
    if (slot < MAX_CHEATS && cheat_parse((unsigned)slot, line) == CheatNoError)
      slot++;
    else
      codes[i].on = 0;
  }
}

int nspire_cheats_set(int i, int on)
{
  if (i < 0 || i >= count || codes[i].on == (on != 0))
    return i >= 0 && i < count;
  if (on && nspire_cheats_on_count() >= NSPIRE_CHEATS_ON_MAX)
    return 0;
  codes[i].on = on != 0;
  changed = 1;
  apply();
  return 1;
}

/* ---- The file's text ---- */

/* A line "cheatN_field = value": N (-1: not one), the field's first letter after the underscore, and the value
 * (between its quotes when it has them). */
static int line_of(const char *s, const char *end, char *field, const char **value, unsigned *value_length)
{
  const char *p = s, *v, *e;
  int n = 0, digits = 0;
  while (p < end && (*p == ' ' || *p == '\t'))
    p++;
  if (end - p < 7 || strncmp(p, "cheat", 5))
    return -1;
  for (p += 5; p < end && *p >= '0' && *p <= '9' && digits < 6; p++, digits++)
    n = n * 10 + (*p - '0');
  if (!digits || p >= end || *p != '_')
    return -1;
  *field = p + 1 < end ? p[1] : 0;
  /* (desc, code, enable: by their first letter and length, so "cheat0_description" isn't the name) */
  if (!((end - p > 5 && !strncmp(p, "_desc", 5) && (p[5] == ' ' || p[5] == '=' || p[5] == '\t')) ||
        (end - p > 5 && !strncmp(p, "_code", 5) && (p[5] == ' ' || p[5] == '=' || p[5] == '\t')) ||
        (end - p > 7 && !strncmp(p, "_enable", 7) && (p[7] == ' ' || p[7] == '=' || p[7] == '\t'))))
    return -1;
  while (p < end && *p != '=')
    p++;
  if (p >= end)
    return -1;
  for (v = p + 1; v < end && (*v == ' ' || *v == '\t'); v++)
    ;
  e = end;
  while (e > v && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
    e--;
  if (e - v >= 2 && *v == '"' && e[-1] == '"')
  {
    v++;
    e--;
  }
  *value = v;
  *value_length = (unsigned)(e - v);
  return n;
}

int nspire_cheats_parse_text(char *t, unsigned long size, int *left_out)
{
  /* what the file says of each N, until all its lines are read (a code's lines come in any order) */
  static struct { u32 name, code, enable; u16 name_length, code_length, enable_length; u8 on; } *seen;
  const char *p = t, *end = t + size;
  int n, top = -1;

  count = skipped = changed = 0;
  cheat_clear();
  if (!t || !size)
    return 0;
  seen = calloc(FILE_CODES_MAX, sizeof(*seen));
  if (!seen)
    return 0;
  while (p < end)
  {
    const char *eol = memchr(p, '\n', (size_t)(end - p)), *value;
    unsigned length;
    char field;
    if (!eol)
      eol = end;
    n = line_of(p, eol, &field, &value, &length);
    if (n >= 0 && n < FILE_CODES_MAX && length < 0xFFFF)
    {
      if (field == 'd')
      {
        seen[n].name = (u32)(value - t);
        seen[n].name_length = (u16)length;
      }
      else if (field == 'c')
      {
        seen[n].code = (u32)(value - t);
        seen[n].code_length = (u16)length;
      }
      else
      {
        seen[n].enable = (u32)(value - t);
        seen[n].enable_length = (u16)length;
        seen[n].on = length == 4 && !strncmp(value, "true", 4);
      }
      if (n > top)
        top = n;
    }
    p = eol < end ? eol + 1 : end;
  }
  text = t;
  text_size = size;
  for (n = 0; n <= top; n++)
  {
    char line[1024];
    struct code *c;
    unsigned k;
    if (!seen[n].code_length)
      continue;
    /* one the engine takes (asked of it: nothing is on yet) */
    snprintf(line, sizeof(line), "%.*s", (int)seen[n].code_length, t + seen[n].code);
    if (seen[n].code_length >= sizeof(line) || cheat_parse(0, line) != CheatNoError || count >= CODES_MAX)
    {
      skipped++;
      continue;
    }
    c = &codes[count++];
    memset(c, 0, sizeof(*c));
    c->at = seen[n].code;
    c->length = seen[n].code_length;
    c->enable_at = seen[n].enable;
    c->enable_length = seen[n].enable_length;
    c->number = (u16)n;
    c->on = seen[n].on;
    if (seen[n].name_length)
      snprintf(c->name, sizeof(c->name), "%.*s", (int)seen[n].name_length, t + seen[n].name);
    else
      snprintf(c->name, sizeof(c->name), "Code %d", n + 1);
    /* (the menus' type is ASCII) */
    for (k = 0; c->name[k]; k++)
      if ((unsigned char)c->name[k] < 32 || (unsigned char)c->name[k] > 126)
        c->name[k] = '?';
  }
  free(seen);
  seen = NULL;
  /* (the file's switches: no more on than the engine has slots) */
  {
    int on = 0;
    for (n = 0; n < count; n++)
      if (codes[n].on && ++on > NSPIRE_CHEATS_ON_MAX)
        codes[n].on = 0;
  }
  apply();
  if (left_out)
    *left_out = skipped;
  return count;
}

/* The file's text with each code's switch as it stands now (malloc'd; *size bytes), or NULL. */
static char *text_now(unsigned long *size)
{
  /* each line can grow by a letter ("true" -> "false"); a code without a line gets one of 32 at the end */
  unsigned long room = text_size + (unsigned long)count * 34 + 2, used = 0, from = 0;
  char *out = malloc(room);
  int i, k;
  if (!out)
    return NULL;
  /* (the codes are in the file's order of N; their enable lines needn't be: the next one by place, each pass) */
  for (;;)
  {
    int next = -1;
    for (i = 0; i < count; i++)
      if (codes[i].enable_at && codes[i].enable_at >= from && (next < 0 || codes[i].enable_at < codes[next].enable_at))
        next = i;
    if (next < 0)
      break;
    memcpy(out + used, text + from, codes[next].enable_at - from);
    used += codes[next].enable_at - from;
    k = snprintf(out + used, room - used, "%s", codes[next].on ? "true" : "false");
    used += (unsigned long)k;
    from = codes[next].enable_at + codes[next].enable_length;
    /* (two codes can't share a line; were a file to give one N two, the later is the one kept above) */
    if (!codes[next].enable_length)
      from++;
  }
  memcpy(out + used, text + from, text_size - from);
  used += text_size - from;
  for (i = 0; i < count; i++)
    if (!codes[i].enable_at)
    {
      if (used && out[used - 1] != '\n')
        out[used++] = '\n';
      used += (unsigned long)snprintf(out + used, room - used, "cheat%u_enable = %s\n", (unsigned)codes[i].number,
                                      codes[i].on ? "true" : "false");
    }
  *size = used;
  return out;
}

/* The switches written back into the file (when one changed). */
static void keep(void)
{
  unsigned long size = 0;
  char *now;
  int done = 0;
  if (!changed || !text || !path[0])
    return;
  now = text_now(&size);
  if (!now)
    return;
  if (nspire_drive_path(path))
    done = nspire_drive_ready_now() && nspire_drive_put(path, now, size);
  else
  {
    char tmp[640];
    FILE *f;
    snprintf(tmp, sizeof(tmp), "%s.tmp.tns", path);
    f = fopen(tmp, "wb");
    if (f)
    {
      done = fwrite(now, 1, size, f) == size;
      done = fclose(f) == 0 && done;
      if (done)
        done = nspire_file_replace(tmp, path) > 0;
      else
        remove(tmp);
    }
  }
  if (done)
  {
    /* the file as it is now is the text the places are in */
    int on[CODES_MAX], i, n = count;
    for (i = 0; i < n; i++)
      on[i] = codes[i].on;
    free(text);
    text = NULL;
    nspire_cheats_parse_text(now, size, NULL);
    for (i = 0; i < n && i < count; i++)
      codes[i].on = (u8)on[i];
    apply();
    changed = 0;
  }
  else
    free(now);
}

/* ---- Finding the game's file ---- */

/* The game's name: its file's without the folder, ".tns" and its last extension. */
static void base_of(const char *rom, char *out, size_t size)
{
  const char *slash = strrchr(rom, '/');
  char *dot;
  snprintf(out, size, "%s", slash ? slash + 1 : rom);
  dot = strrchr(out, '.');
  if (dot && !strcasecmp(dot, ".tns"))
    *dot = 0;
  dot = strrchr(out, '.');
  if (dot && dot != out)
    *dot = 0;
}

/* How well a file's name fits the game: 2 its name and ".cht" (or ".cht.tns"); 1 its name, a tag in brackets
 * (" (Code Breaker)", as libretro-database names them), then that; 0 not ("Mario Kart.cht" isn't "Mario"'s). */
static int fits(const char *name, const char *base)
{
  size_t n = strlen(name), b = strlen(base);
  if (n > 4 && !strcasecmp(name + n - 4, ".tns"))
    n -= 4;
  if (n < b + 4 || strncasecmp(name + n - 4, ".cht", 4) || strncasecmp(name, base, b))
    return 0;
  if (n == b + 4)
    return 2;
  return name[b] == ' ' && name[b + 1] == '(' ? 1 : 0;
}

struct found
{
  const char *base;
  char name[280];
  int fit;
};

static int drive_each(const char *name, unsigned long size, unsigned long mtime, void *arg)
{
  struct found *f = arg;
  int fit = fits(name, f->base);
  (void)size;
  (void)mtime;
  if (fit > f->fit)
  {
    f->fit = fit;
    snprintf(f->name, sizeof(f->name), "%.279s", name);
  }
  return 1;   /* (the next one: nspire_drive_dir stops at a 0. Returning 0 found a file only when it was the
                 folder's first: the drive tests' game folder, with another file before it) */
}

/* The game's file in a folder (the calculator's, or DRIVE_PREFIX...: the drive's): its path in out, or 0. */
static int find_in(const char *dir, const char *base, char *out, size_t size)
{
  struct found f;
  memset(&f, 0, sizeof(f));
  f.base = base;
  if (nspire_drive_path(dir))
  {
    if (!nspire_drive_ready_now())
      return 0;
    nspire_drive_dir(dir, drive_each, &f);
  }
  else
  {
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL)
      drive_each(e->d_name, 0, 0, &f);
    if (d)
      closedir(d);
  }
  if (!f.fit)
    return 0;
  snprintf(out, size, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] == '/' ? "" : "/", f.name);
  return 1;
}

static int read_file(const char *from)
{
  char *t = NULL;
  unsigned long size = 0;
  if (nspire_drive_path(from))
    t = nspire_drive_get(from, FILE_MAX, &size);
  else
  {
    FILE *f = fopen(from, "rb");
    long length;
    if (f && !fseek(f, 0, SEEK_END) && (length = ftell(f)) > 0 && (unsigned long)length <= FILE_MAX &&
        !fseek(f, 0, SEEK_SET) && (t = malloc((size_t)length)) != NULL)
    {
      size = fread(t, 1, (size_t)length, f);
      if (size != (unsigned long)length)
      {
        free(t);
        t = NULL;
      }
    }
    if (f)
      fclose(f);
  }
  if (!t || !size)
  {
    free(t);
    return 0;
  }
  if (!nspire_cheats_parse_text(t, size, NULL) && !skipped)
  {
    /* not a cheat file */
    free(t);
    text = NULL;
    text_size = 0;
    return 0;
  }
  snprintf(path, sizeof(path), "%s", from);
  return 1;
}

static void forget(void)
{
  cheat_clear();
  free(text);
  text = NULL;
  text_size = 0;
  count = skipped = changed = 0;
  path[0] = 0;
}

static void look(void)
{
  char base[300], dir[600], found[900], *slash;
  int k;
  static const char *const drive_dirs[] = { DRIVE_PREFIX, DRIVE_PREFIX "cheats", DRIVE_PREFIX SAVE_FOLDER };

  looked_epoch = nspire_drive_epoch();
  looked_drive = nspire_drive_usable(0);
  if (!game[0])
    return;
  base_of(game, base, sizeof(base));
  if (!base[0])
    return;
  /* the game's own folder (a name without one: the folder gpSP runs in) */
  snprintf(dir, sizeof(dir), "%s", game);
  slash = strrchr(dir, '/');
  if (!slash)
    snprintf(dir, sizeof(dir), ".");
  else   /* ("/x.gba" and "usb:/x.gba": the folder keeps its slash) */
    slash[slash == dir || (nspire_drive_path(dir) && slash < dir + sizeof(DRIVE_PREFIX) - 1) ? 1 : 0] = 0;
  if (find_in(dir, base, found, sizeof(found)) && read_file(found))
    return;
  /* the calculator's saves folder */
  nspire_save_file(dir, sizeof(dir), "x");
  slash = strrchr(dir, '/');
  if (slash)
  {
    *slash = 0;
    if (find_in(dir, base, found, sizeof(found)) && read_file(found))
      return;
  }
  /* a drive */
  for (k = 0; k < (int)(sizeof(drive_dirs) / sizeof(drive_dirs[0])); k++)
    if (find_in(drive_dirs[k], base, found, sizeof(found)) && read_file(found))
      return;
}

void nspire_cheats_open(const char *rom_path)
{
  forget();
  snprintf(game, sizeof(game), "%s", rom_path ? rom_path : "");
  look();
}

void nspire_cheats_close(void)
{
  keep();
  forget();
  game[0] = 0;
}

void nspire_cheats_look(void)
{
  if (!game[0] || text)
    return;
  /* (asked at every draw of Settings: nothing is asked of the drive here) */
  if (looked_epoch == nspire_drive_epoch() && looked_drive == nspire_drive_usable(0))
    return;
  look();
}

/* ---- Settings > Cheats: a line a code, enter turns it on or off ---- */

void nspire_cheats_page(void)
{
  static struct ui_cheats_view v;
  int picked = 0, first = 0, i;
  u32 picked_at = nspire_ticks();
  char note[24], status[64] = "", help[80], last_shown[32] = "";

  if (!count)
    return;
  keys_wait_release();
  for (;;)
  {
    gui_action_type a;
    int on = nspire_cheats_on_count();
    memset(&v, 0, sizeof(v));
    if (picked < first)
      first = picked;
    if (picked >= first + UI_CHEAT_LINES)
      first = picked - UI_CHEAT_LINES + 1;
    v.count = count;
    v.picked = picked;
    v.first = first;
    for (i = 0; i < UI_CHEAT_LINES && first + i < count; i++)
    {
      v.names[i] = codes[first + i].name;
      v.on[i] = codes[first + i].on;
    }
    v.shown = i;
    snprintf(note, sizeof(note), "%d on", on);
    v.note = note;
    /* the band: what just happened, else where the codes are from (and how many the engine can't run) */
    if (skipped)
      snprintf(help, sizeof(help), "%s. %d not supported.", nspire_drive_path(path) ? "From the USB drive" :
               "From the calculator", skipped);
    else
      snprintf(help, sizeof(help), "%s.", nspire_drive_path(path) ? "From the USB drive" : "From the calculator");
    v.help = help;
    v.status = status;
    v.picked_ms = ui_cheats_scrolls ? (unsigned)((nspire_ticks() - picked_at) * 1000 / NSPIRE_TICK_HZ) : 0;
    ui_cheats_draw(&v);
    if (strcmp(last_shown, ui_shown))
    {
      snprintf(last_shown, sizeof(last_shown), "%s", ui_shown);
      picked_at = nspire_ticks();
    }
    update_at_vblank();
    a = get_gui_input();
    if (a != CURSOR_NONE)
      status[0] = 0;
    if (a == CURSOR_BACK || a == CURSOR_EXIT)
      break;
    if (a == CURSOR_UP)
      picked = picked > 0 ? picked - 1 : count - 1;
    else if (a == CURSOR_DOWN)
      picked = picked + 1 < count ? picked + 1 : 0;
    else if (a == CURSOR_LEFT)
      picked = picked >= UI_CHEAT_LINES ? picked - UI_CHEAT_LINES : 0;
    else if (a == CURSOR_RIGHT)
      picked = picked + UI_CHEAT_LINES < count ? picked + UI_CHEAT_LINES : count - 1;
    else if (a == CURSOR_SELECT)
    {
      if (!nspire_cheats_set(picked, !codes[picked].on))
        snprintf(status, sizeof(status), "%d codes on at most. Turn one off first.", NSPIRE_CHEATS_ON_MAX);
    }
  }
  keep();
  keys_wait_release();
}
