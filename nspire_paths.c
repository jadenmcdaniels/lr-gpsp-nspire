/* Where gpSP's files go: see nspire_paths.h. */

#include "nspire_paths.h"

#include <os.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

char nspire_program_path[512];

/* gpSP's own files that earlier builds kept next to the program, and the
 * BIOS. gpSP's settings files move in too (is_settings_file), so the
 * program's folder has only the program and the games. */
static const char *const program_files[] =
{
  "gpsp_romdir.tns", "gpsp_clock.tns", "gpsp_perf.txt.tns", "gpsp_results.txt.tns", BIOS_FILE
};

/* A game's files are its base name and one of these, or its base name, a
 * slot number and ".svs.tns" (a state). */
static const char *const game_suffixes[] = { ".sav.tns", ".sav.gz.tns", ".settings.tns" };
#define NUM_GAME_SUFFIXES (sizeof(game_suffixes) / sizeof(game_suffixes[0]))
#define OLD_ORDER_FILE "gpsp_order.txt.tns"

void nspire_save_file(char *out, size_t size, const char *name)
{
  const char *docs = get_documents_dir();
  size_t len = strlen(docs);

  snprintf(out, size, "%s%s%s%s%s", docs, len && docs[len - 1] == '/' ? "" : "/", SAVE_FOLDER,
           name[0] ? "/" : "", name);
}

void nspire_log_buffered(void *f)
{
  if (f)
    (void)setvbuf((FILE *)f, NULL, _IOFBF, NSPIRE_LOG_BUFFER);
}

void nspire_save_dir_make(void)
{
  char dir[600];
  struct stat st;

  nspire_save_file(dir, sizeof(dir), "");
  if (stat(dir, &st) != 0 && mkdir(dir, 0755) == 0)
    nspire_file_list_touched();
}

void nspire_normalize_path(char *out, size_t size, const char *path)
{
  size_t i, len = 0;

  if (!size)
    return;
  for (i = 0; path[i] && len + 1 < size; i++)
  {
    char c = path[i] == '\\' ? '/' : path[i];
    if (c == '/' && len && out[len - 1] == '/')
      continue;
    out[len++] = c;
  }
  while (len > 1 && out[len - 1] == '/')
    len--;
  out[len] = 0;
}

void nspire_order_file(char *out, size_t size, const char *dir)
{
  char path[512], docs[512], name[600];
  const char *rel = NULL;
  size_t docs_len, i;

  nspire_normalize_path(path, sizeof(path), dir);
  nspire_normalize_path(docs, sizeof(docs), get_documents_dir());
  docs_len = strlen(docs);
  if (!strncasecmp(path, docs, docs_len) && (path[docs_len] == 0 || path[docs_len] == '/'))
    rel = path + docs_len;
  else
  {
    /* A path the OS gave in another form ("A:/documents/gba"). */
    const char *p = strstr(path, "/documents");
    if (p && (p[10] == 0 || p[10] == '/'))
      rel = p + 10;
    else
      rel = path;
  }
  while (*rel == '/')
    rel++;
  if (!*rel)
  {
    nspire_save_file(out, size, OLD_ORDER_FILE);
    return;
  }
  snprintf(name, sizeof(name), "gpsp_order_%s.txt.tns", rel);
  for (i = 0; name[i]; i++)
    if (name[i] == '/')
      name[i] = '_';
  nspire_save_file(out, size, name);
}

/* Moves a file unless the new name is taken. */
static void move_file(const char *from, const char *to)
{
  struct stat st;

  if (stat(from, &st) != 0 || stat(to, &st) == 0)
    return;
  if (rename(from, to) == 0)
    nspire_file_list_touched();
}

/* The slot part of a state's name after its base ("12.svs.tns"): its
 * length, or 0 if it isn't one. */
static size_t state_tail(const char *tail)
{
  size_t digits = 0;

  while (isdigit((unsigned char)tail[digits]) && digits < 4)
    digits++;
  return digits && !strcasecmp(tail + digits, ".svs.tns") ? digits + 8 : 0;
}

/* The names in a folder that pass the test, each malloc'd, in a malloc'd
 * array; *count gets how many. Gathered before anything moves, so the
 * folder isn't changed while it's read. */
static int folder_unread;   /* the last folder_names couldn't open its folder, or ran out of memory */

static char **folder_names(const char *dir, int (*wanted)(const char *name, const void *arg),
                           const void *arg, unsigned *count)
{
  char **names = NULL, **grown;
  unsigned capacity = 0;
  struct dirent *entry;
  DIR *d = opendir(dir);

  *count = 0;
  folder_unread = d == NULL;
  if (!d)
    return NULL;
  while ((entry = readdir(d)) != NULL)
  {
    if (!wanted(entry->d_name, arg))
      continue;
    if (*count == capacity)
    {
      capacity = capacity ? capacity * 2 : 16;
      grown = (char **)realloc(names, capacity * sizeof(*names));
      if (!grown)
      {
        folder_unread = 1;
        break;
      }
      names = grown;
    }
    names[*count] = strdup(entry->d_name);
    if (names[*count])
      (*count)++;
    else
      folder_unread = 1;
  }
  closedir(d);
  return names;
}

static void free_names(char **names, unsigned count)
{
  unsigned i;
  for (i = 0; i < count; i++)
    free(names[i]);
  free(names);
}

/* Splits "dir/name" into its folder ("." for none) and name. */
static const char *split_path(const char *path, char *dir, size_t size)
{
  const char *slash = strrchr(path, '/');

  if (!slash)
  {
    snprintf(dir, size, ".");
    return path;
  }
  snprintf(dir, size, "%.*s", slash == path ? 1 : (int)(slash - path), path);
  return slash + 1;
}

static int is_state_of(const char *name, const void *arg)
{
  const char *base = (const char *)arg;
  size_t len = strlen(base);
  return !strncasecmp(name, base, len) && state_tail(name + len);
}

void nspire_move_game_files(const char *from_base, const char *to_base)
{
  char from[700], to[700], dir[600], clean_from[600], clean_to[600];
  const char *name;
  char **states;
  unsigned count, i;

  nspire_normalize_path(clean_from, sizeof(clean_from), from_base);
  nspire_normalize_path(clean_to, sizeof(clean_to), to_base);
  from_base = clean_from;
  to_base = clean_to;
  if (!strcmp(from_base, to_base))
    return;
  for (i = 0; i < NUM_GAME_SUFFIXES; i++)
  {
    snprintf(from, sizeof(from), "%s%s", from_base, game_suffixes[i]);
    snprintf(to, sizeof(to), "%s%s", to_base, game_suffixes[i]);
    move_file(from, to);
  }
  name = split_path(from_base, dir, sizeof(dir));
  states = folder_names(dir, is_state_of, name, &count);
  for (i = 0; i < count; i++)
  {
    snprintf(from, sizeof(from), "%s/%s", dir, states[i]);
    snprintf(to, sizeof(to), "%s%s", to_base, states[i] + strlen(name));
    move_file(from, to);
  }
  free_names(states, count);
}

/* A file name an earlier build gave a GBA game's save, state or settings:
 * its base name (the ROM's name) ends in .gba, .agb or .bin, so other
 * emulators' saves (Game.gbc.sav.tns) stay. */
static int is_old_game_file(const char *name)
{
  size_t len = strlen(name), base_len = 0, i;

  for (i = 0; i < NUM_GAME_SUFFIXES && !base_len; i++)
  {
    size_t n = strlen(game_suffixes[i]);
    if (len > n && !strcasecmp(name + len - n, game_suffixes[i]))
      base_len = len - n;
  }
  if (!base_len && len > 8 && !strcasecmp(name + len - 8, ".svs.tns"))
  {
    base_len = len - 8;
    while (base_len && isdigit((unsigned char)name[base_len - 1]) && len - 8 - base_len < 4)
      base_len--;
    if (base_len == len - 8)
      base_len = 0;
  }
  return base_len > 4 && (!strncasecmp(name + base_len - 4, ".gba", 4) ||
                          !strncasecmp(name + base_len - 4, ".agb", 4) ||
                          !strncasecmp(name + base_len - 4, ".bin", 4));
}

/* A settings file of gpSP's in its folder: this build's before
 * (gpsp_qol.cfg.tns), older builds' (gpsp.cfg.tns, "..cfg.tns" with no game
 * open, and a game's: "Game.gba.cfg.tns", or "Game.cfg.tns" for a game in
 * the folder). Another program's (ndless.cfg.tns) isn't. */
static int is_settings_file(const char *dir, const char *name)
{
  static const char *const rom_exts[] = { ".gba.tns", ".zip.tns", ".gbz.tns" };
  size_t len = strlen(name), base_len;
  char path[700];
  struct stat st;
  unsigned i;

  if (len <= 8 || strcasecmp(name + len - 8, ".cfg.tns"))
    return 0;
  base_len = len - 8;
  if (!strcmp(name, "..cfg.tns") || !strncasecmp(name, "gpsp", 4))
    return 1;
  if (base_len > 4 && (!strncasecmp(name + base_len - 4, ".gba", 4) || !strncasecmp(name + base_len - 4, ".agb", 4)))
    return 1;
  for (i = 0; i < sizeof(rom_exts) / sizeof(rom_exts[0]); i++)
  {
    snprintf(path, sizeof(path), "%s/%.*s%s", dir, (int)base_len, name, rom_exts[i]);
    if (stat(path, &st) == 0)
      return 1;
  }
  return 0;
}

static int is_program_file(const char *dir, const char *name)
{
  unsigned i;
  for (i = 0; i < sizeof(program_files) / sizeof(program_files[0]); i++)
    if (!strcasecmp(name, program_files[i]))
      return 1;
  return is_settings_file(dir, name);
}

struct sweep
{
  const char *dir;
  int with_program_files;
};

static int is_old_file(const char *name, const void *arg)
{
  const struct sweep *sweep = (const struct sweep *)arg;
  return is_old_game_file(name) || !strcmp(name, OLD_ORDER_FILE) ||
         (sweep->with_program_files && is_program_file(sweep->dir, name));
}

void nspire_move_old_files(const char *folder, int with_program_files)
{
  char from[1100], to[1100], dir[600];
  struct sweep sweep;
  char **names;
  unsigned count, i;

  nspire_normalize_path(dir, sizeof(dir), folder);
  sweep.dir = dir;
  sweep.with_program_files = with_program_files;
  names = folder_names(dir, is_old_file, &sweep, &count);
  if (count)
    nspire_save_dir_make();
  for (i = 0; i < count; i++)
  {
    snprintf(from, sizeof(from), "%s%s%s", dir, !strcmp(dir, "/") ? "" : "/", names[i]);
    if (!strcmp(names[i], OLD_ORDER_FILE))
      nspire_order_file(to, sizeof(to), dir);
    else
      nspire_save_file(to, sizeof(to), names[i]);
    move_file(from, to);
  }
  free_names(names, count);
}

static int is_any_file(const char *name, const void *arg)
{
  (void)arg;
  return strcmp(name, ".") && strcmp(name, "..");
}

static int is_documents_settings_file(const char *name, const void *arg)
{
  return is_settings_file((const char *)arg, name);
}

/* Each name in from_dir moved to the same name in the saves folder. */
static void move_names_in(const char *from_dir, char **names, unsigned count)
{
  char from[1100], to[1100];
  unsigned i;

  if (count)
    nspire_save_dir_make();
  for (i = 0; i < count; i++)
  {
    snprintf(from, sizeof(from), "%s%s%s", from_dir, !strcmp(from_dir, "/") ? "" : "/", names[i]);
    nspire_save_file(to, sizeof(to), names[i]);
    move_file(from, to);
  }
  free_names(names, count);
}

void nspire_move_old_documents_files(void)
{
  static const char *const old_folders[] = OLD_SAVE_FOLDERS;
  char docs[600], old_dir[700];
  char **names;
  unsigned count, i;
  struct stat st;

  nspire_normalize_path(docs, sizeof(docs), get_documents_dir());
  for (i = 0; i < sizeof(old_folders) / sizeof(old_folders[0]); i++)
  {
    snprintf(old_dir, sizeof(old_dir), "%s%s%s", docs, !strcmp(docs, "/") ? "" : "/", old_folders[i]);
    if (stat(old_dir, &st) != 0)
      continue;
    names = folder_names(old_dir, is_any_file, NULL, &count);
    move_names_in(old_dir, names, count);
    if (rmdir(old_dir) == 0)   /* only goes if everything moved */
      nspire_file_list_touched();
  }
  names = folder_names(docs, is_documents_settings_file, docs, &count);
  move_names_in(docs, names, count);
}

/* "dir/name" (dir "/" gives "/name"). */
static void join(char *out, size_t size, const char *dir, const char *name)
{
  snprintf(out, size, "%s%s%s", dir, !strcmp(dir, "/") ? "" : "/", name);
}

static int is_folder(const char *name, const void *arg)
{
  char path[1100];
  struct stat st;

  if (!strcmp(name, ".") || !strcmp(name, "..") || !strcasecmp(name, SAVE_FOLDER))
    return 0;
  join(path, sizeof(path), (const char *)arg, name);
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_bios_name(const char *name, const void *arg)
{
  (void)arg;
  return !strcasecmp(name, BIOS_FILE);
}

/* The BIOS in dir (any case) moved into the saves folder: 1 if it moved. */
static int move_bios_from(const char *dir)
{
  char from[1100], to[700];
  char **names;
  unsigned count;
  int moved = 0;
  struct stat st;

  names = folder_names(dir, is_bios_name, NULL, &count);
  if (count)
  {
    join(from, sizeof(from), dir, names[0]);
    nspire_save_file(to, sizeof(to), BIOS_FILE);
    move_file(from, to);
    moved = stat(to, &st) == 0;
  }
  free_names(names, count);
  return moved;
}

void nspire_tidy_save_folder(void)
{
  char path[700], old_path[700], docs[600], dir[1100];
  char **folders;
  unsigned count, i;
  struct stat st;

  nspire_save_file(old_path, sizeof(old_path), OLD_CONFIG_FILE);
  if (stat(old_path, &st) == 0)
  {
    nspire_save_file(path, sizeof(path), CONFIG_FILE);
    /* SAV-3: a gpsp.cfg.tns of this build's size (more than the 152 bytes
     * of gpSP's older formats) is the current copy: it stays, the old-named
     * file goes. An older format is set aside, not removed, until the
     * rename has worked. */
    if (stat(path, &st) == 0 && st.st_size > 152)
    {
      remove(old_path);
      nspire_file_list_touched();
    }
    else
    {
      char prev[720];
      int had = stat(path, &st) == 0;

      snprintf(prev, sizeof(prev), "%s.prev", path);
      if (had && rename(path, prev) != 0)
        return;
      if (rename(old_path, path) == 0)
      {
        remove(prev);
        nspire_file_list_touched();
      }
      else if (had)
        rename(prev, path);
    }
  }

  nspire_save_file(path, sizeof(path), BIOS_FILE);
  if (stat(path, &st) == 0)
    return;
  nspire_normalize_path(docs, sizeof(docs), get_documents_dir());
  if (move_bios_from(docs))
    return;
  folders = folder_names(docs, is_folder, docs, &count);
  for (i = 0; i < count; i++)
  {
    join(dir, sizeof(dir), docs, folders[i]);
    if (move_bios_from(dir))
      break;
  }
  free_names(folders, count);
}

/* ---- The OS's file list (nspire_paths.h) ---- */

static int file_list_touched;
static uint32_t *start_names;
static unsigned start_count;

static uint32_t name_hash(const char *name)
{
  uint32_t hash = 2166136261u;   /* FNV-1a, ignoring case like the OS */
  for (; *name; name++)
    hash = (hash ^ (uint32_t)tolower((unsigned char)*name)) * 16777619u;
  return hash;
}

static int compare_u32(const void *a, const void *b)
{
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return x < y ? -1 : x > y;
}

/* The saves folder's names as sorted hashes, malloc'd; NULL if none.
 * *ok: 0 if the folder couldn't be read or the memory ran out. */
static uint32_t *save_folder_hashes(unsigned *count, int *ok)
{
  char dir[600];
  char **names;
  uint32_t *hashes;
  unsigned i, n;

  nspire_save_file(dir, sizeof(dir), "");
  names = folder_names(dir, is_any_file, NULL, &n);
  *ok = !folder_unread;
  hashes = n ? (uint32_t *)malloc(n * sizeof(*hashes)) : NULL;
  if (n && !hashes)
    *ok = 0;
  *count = hashes ? n : 0;
  for (i = 0; i < *count; i++)
    hashes[i] = name_hash(names[i]);
  free_names(names, n);
  if (hashes)
    qsort(hashes, *count, sizeof(*hashes), compare_u32);
  return hashes;
}

static int start_listed;          /* nspire_file_list_start read the folder (whole) */
static char start_dir[600];       /* the saves folder, normalized */

void nspire_file_list_start(void)
{
  char dir[600];

  free(start_names);
  start_names = save_folder_hashes(&start_count, &start_listed);
  nspire_save_file(dir, sizeof(dir), "");
  nspire_normalize_path(start_dir, sizeof(start_dir), dir);
}

unsigned nspire_save_folder_moves;

void nspire_file_list_touched(void)
{
  file_list_touched = 1;
  nspire_save_folder_moves++;
}

/* fast17: the names in the saves folder that were there at start and that
 * gpSP removed or renamed away since (nspire_file_gone): the only ones
 * that can be missing at exit. Exit looks at these alone (a stat each, and
 * none at all when nothing went), not at the whole folder again (2-3 s on
 * the CX II at exit). */
#define GONE_MAX 32
static char *gone[GONE_MAX];
static unsigned gone_count;
static char list_why[120];
static char list_step[160];

static int ends_tns(const char *name)
{
  size_t n = strlen(name);
  return n > 4 && !strcasecmp(name + n - 4, ".tns");
}

void nspire_file_gone(const char *path)
{
  char clean[600], dir[600];
  const char *slash, *name;
  uint32_t hash;
  unsigned i;

  if (!path || !strncmp(path, "usb:", 4))
    return;   /* a USB drive's file: not in the OS's list */
  nspire_normalize_path(clean, sizeof(clean), path);
  slash = strrchr(clean, '/');
  name = slash ? slash + 1 : clean;
  /* The OS lists only .tns files (and folders): a .tmp, a .rollback or a
   * marker without .tns going is nothing it showed. */
  if (!ends_tns(name))
    return;
  snprintf(dir, sizeof(dir), "%.*s", slash ? (int)(slash - clean) : 0, clean);
  if (!start_listed || !slash || strcasecmp(dir, start_dir))
  {
    /* a .tns elsewhere (a game's folder, My Documents): not one the start
     * listed, so the list is read again, as for a move */
    if (!file_list_touched)
      snprintf(list_why, sizeof(list_why), "%.100s went", name);
    file_list_touched = 1;
    return;
  }
  hash = name_hash(name);
  if (!start_names || !bsearch(&hash, start_names, start_count, sizeof(hash), compare_u32))
    return;   /* made in this run (a state, a log, a marker): the list never had it */
  for (i = 0; i < gone_count; i++)
    if (!strcasecmp(gone[i], clean))
      return;
  if (gone_count == GONE_MAX || !(gone[gone_count] = strdup(clean)))
  {
    snprintf(list_why, sizeof(list_why), "more than %d names went", GONE_MAX);
    file_list_touched = 1;
    return;
  }
  gone_count++;
}

int nspire_save_folder_had(const char *name)
{
  uint32_t hash = name_hash(name);
  if (!start_listed)
    return 1;   /* not known: maybe */
  return start_names && bsearch(&hash, start_names, start_count, sizeof(hash), compare_u32) != NULL;
}

const char *nspire_file_list_why(void)
{
  return list_step[0] ? list_step : "file list looked at";
}

#ifdef NSPIRE_HOST
/* The tests start over (on the calculator a start is a new program). */
void nspire_file_list_untouch(void)
{
  file_list_touched = 0;
  while (gone_count)
    free(gone[--gone_count]);
  list_why[0] = 0;
}
#endif

static int list_result(int changed, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int list_result(int changed, const char *fmt, ...)
{
  va_list ap;
  char why[120];

  va_start(ap, fmt);
  vsnprintf(why, sizeof(why), fmt, ap);
  va_end(ap);
  snprintf(list_step, sizeof(list_step), "file list looked at (%s: %s)", changed ? "changed" : "unchanged", why);
  return changed;
}

int nspire_file_list_changed(void)
{
  struct stat st;
  unsigned i;

  if (file_list_touched)
    return list_result(1, "%s", list_why[0] ? list_why : "a move, a new folder or the ROM manager");
  for (i = 0; i < gone_count; i++)
    if (stat(gone[i], &st) != 0)
    {
      const char *slash = strrchr(gone[i], '/');
      return list_result(1, "%s gone, %u looked at", slash ? slash + 1 : gone[i], gone_count);
    }
  return list_result(0, "%u name(s) looked at", gone_count);
}

/* On the calculator rename does not replace an existing file. Keep the
 * previous complete file until the new one has its final name, and leave
 * a recoverable rollback file if restoring the name fails too. */
static int rollback_path(const char *path, char *out, size_t size)
{
  int n = snprintf(out, size, "%s.rollback", path);
  return n >= 0 && (size_t)n < size;
}

int nspire_file_recover(const char *path)
{
  char old[640];
  struct stat st;
  if (!rollback_path(path, old, sizeof(old)))
    return 0;
  if (stat(path, &st) == 0)
    return S_ISREG(st.st_mode);
  if (errno != ENOENT)
    return 0;
  if (stat(old, &st) != 0)
    return errno == ENOENT;
  return S_ISREG(st.st_mode) && rename(old, path) == 0;
}

/* (r19-snd) The same, one OS call or two a step (a save's parts at the
 * game's frames' ends: nspire_states.c, nspire_backup.c), the same calls in
 * the same order; between two steps the files are as the one-call version
 * leaves them between its calls (the old file, or its rollback copy, kept
 * until the new one has its name), so a reset or a quit there loses
 * nothing nspire_file_recover doesn't bring back. */
enum { R_CHECK, R_RENAME, R_TIDY, R_STAT, R_ASIDE, R_PUT, R_DROP };

int nspire_file_replace_begin(struct nspire_replace *r, const char *tmp, const char *path)
{
  r->tmp = tmp;
  r->path = path;
  r->step = R_CHECK;
  return !strcmp(tmp, path) || !rollback_path(path, r->old, sizeof(r->old)) || !strcmp(tmp, r->old) ? 0 : 1;
}

int nspire_file_replace_step(struct nspire_replace *r)
{
  struct stat st;
  switch (r->step)
  {
  case R_CHECK:
    if (stat(r->tmp, &st) != 0 || !S_ISREG(st.st_mode) || !nspire_file_recover(r->path))
      return 0;
    r->step = R_RENAME;
    return NSPIRE_REPLACE_BUSY;
  case R_RENAME:
    /* Atomic replacement on hosts that support it; a plain move when the
     * target does not yet exist on the calculator. */
    r->step = rename(r->tmp, r->path) == 0 ? R_TIDY : R_STAT;
    return NSPIRE_REPLACE_BUSY;
  case R_TIDY:
    if (stat(r->old, &st) == 0 && S_ISREG(st.st_mode))
      remove(r->old); /* leftover from an earlier committed replacement */
    return 1;
  case R_STAT:
    if (stat(r->path, &st) != 0 || !S_ISREG(st.st_mode))
      return 0;
    /* An earlier successful replacement may have left its rollback copy.
     * The current path is still complete while that obsolete copy goes. */
    if (stat(r->old, &st) == 0)
    {
      if (!S_ISREG(st.st_mode) || remove(r->old) != 0)
        return 0;
    }
    else if (errno != ENOENT)
      return 0;
    r->step = R_ASIDE;
    return NSPIRE_REPLACE_BUSY;
  case R_ASIDE:
    if (rename(r->path, r->old) != 0)
      return 0;
    r->step = R_PUT;
    return NSPIRE_REPLACE_BUSY;
  case R_PUT:
    if (rename(r->tmp, r->path) != 0)
    {
      (void)rename(r->old, r->path); /* otherwise recover it when next opened */
      return 0;
    }
    r->step = R_DROP;
    return NSPIRE_REPLACE_BUSY;
  case R_DROP:
    remove(r->old);
    return 1;
  default:
    return 0;
  }
}

int nspire_file_replace(const char *tmp, const char *path)
{
  struct nspire_replace r;
  int done;
  if (!nspire_file_replace_begin(&r, tmp, path))
    return 0;
  while ((done = nspire_file_replace_step(&r)) == NSPIRE_REPLACE_BUSY)
    ;
  return done;
}

/* Explicit deletion must also discard recovery versions, or the next
 * startup would restore a save the user just removed. Validate all names
 * first and remove recovery versions while the current file still exists. */
int nspire_file_delete(const char *path)
{
  char old[640], tmp[640];
  const char *names[3] = { old, tmp, path };
  struct stat st;
  unsigned present = 0;
  int i, n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  if (!rollback_path(path, old, sizeof(old)) || n < 0 || (size_t)n >= sizeof(tmp))
    return 0;
  for (i = 0; i < 3; i++)
  {
    if (stat(names[i], &st) == 0)
    {
      if (!S_ISREG(st.st_mode))
        return 0;
      present |= 1u << i;
    }
    else if (errno != ENOENT)
      return 0;
  }
  for (i = 0; i < 3; i++)
    if ((present & (1u << i)) && remove(names[i]) != 0)
      return 0;
  return present != 0;
}
