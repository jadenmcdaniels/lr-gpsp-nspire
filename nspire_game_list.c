/* The file list's order: see nspire_game_list.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "nspire_game_list.h"
#include "nspire_paths.h"
#include "nspire_states.h"

static int compare_names(const void *a, const void *b)
{
  const char *x = *(char *const *)a, *y = *(char *const *)b;
  int x_up = !strcmp(x, ".."), y_up = !strcmp(y, "..");

  if (x_up || y_up)
    return y_up - x_up;
  return strcasecmp(x, y);
}

void game_list_sort(char **names, unsigned count)
{
  if (count > 1)
    qsort(names, count, sizeof(char *), compare_names);
}

void game_list_move(char **names, unsigned count, unsigned from, unsigned to)
{
  char *name;
  unsigned i;

  /* (The bound on count lets GCC see the copies stay small.) */
  if (count > 65536 || from >= count || to >= count || from == to)
    return;
  name = names[from];
  for (i = from; i < to; i++)
    names[i] = names[i + 1];
  for (i = from; i > to; i--)
    names[i] = names[i - 1];
  names[to] = name;
}

static void order_path(char *path, size_t size, const char *dir)
{
  nspire_order_file(path, size, dir);
}

/* UI-3: the order is written to "<file>.tmp" and takes its name when whole
 * (nspire_tmp_take_place, as the saves do). A leftover .tmp alone is whole
 * when it ends in a newline: every line is written with one. */
static int order_tmp_whole(const char *tmp)
{
  FILE *f = fopen(tmp, "rb");
  int last = -1;

  if (!f)
    return 0;
  if (fseek(f, -1, SEEK_END) == 0)
    last = fgetc(f);
  fclose(f);
  return last == '\n';
}

/* Writes text as the order file, whole or not at all. */
static int write_order_file(const char *path, const char *text, size_t len)
{
  char tmp[1110];
  FILE *f;
  int ok;

  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  f = fopen(tmp, "wb");
  if (!f)
    return 0;
  ok = fwrite(text, 1, len, f) == len;
  if (fclose(f) != 0)
    ok = 0;
  return nspire_tmp_take_place(tmp, path, ok);
}

int game_list_apply_order(char **names, unsigned count, const char *dir)
{
  char path[1100], line[512];
  unsigned placed = 0, i;
  FILE *f;

  order_path(path, sizeof(path), dir);
  nspire_tmp_recover(path, order_tmp_whole);
  f = fopen(path, "r");
  if (!f)
    return 0;
  while (placed < count && fgets(line, sizeof(line), f))
  {
    line[strcspn(line, "\r\n")] = 0;
    for (i = placed; i < count; i++)
      if (!strcmp(names[i], line))
      {
        game_list_move(names, count, i, placed++);
        break;
      }
  }
  fclose(f);
  return 1;
}

int game_list_save_order(char **names, unsigned count, const char *dir)
{
  char path[1100];
  char *text, *p;
  size_t len = 0;
  unsigned i;
  int ok;

  order_path(path, sizeof(path), dir);
  nspire_save_dir_make();
  for (i = 0; i < count; i++)
    len += strlen(names[i]) + 1;
  text = (char *)malloc(len + 1);
  if (!text)
    return 0;
  for (p = text, i = 0; i < count; i++)
    p += sprintf(p, "%s\n", names[i]);
  ok = write_order_file(path, text, len);
  free(text);
  return ok;
}

void game_list_forget_order(const char *dir)
{
  char path[1100];
  order_path(path, sizeof(path), dir);
  remove(path);
  strcat(path, ".tmp");
  remove(path);
}

int game_list_rename(const char *dir, const char *old_name, const char *new_name)
{
  char path[1100], line[512];
  char *text = NULL, *grown;
  size_t len = 0, cap = 0, n;
  int changed = 0, ok = 1;
  FILE *f;

  order_path(path, sizeof(path), dir);
  nspire_tmp_recover(path, order_tmp_whole);
  f = fopen(path, "r");
  if (!f)
    return 1;
  while (fgets(line, sizeof(line), f))
  {
    line[strcspn(line, "\r\n")] = 0;
    if (!strcmp(line, old_name))
    {
      snprintf(line, sizeof(line), "%s", new_name);
      changed = 1;
    }
    n = strlen(line) + 1;
    if (len + n + 1 > cap)
    {
      cap = (len + n + 1) * 2;
      grown = (char *)realloc(text, cap);
      if (!grown)
      {
        free(text);
        fclose(f);
        return 0;
      }
      text = grown;
    }
    memcpy(text + len, line, n - 1);
    text[len + n - 1] = '\n';
    len += n;
  }
  fclose(f);
  if (changed)
    ok = write_order_file(path, text, len);
  free(text);
  return ok;
}
