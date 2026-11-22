/* Fault injection at rename, including the calculator's non-replacing
 * rename semantics. The old save must survive a failed promotion. */
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
static int fake_rename(const char *from, const char *to);
static int fake_remove(const char *path);
#define rename fake_rename
#define remove fake_remove
#include "../../nspire_paths.c"
#undef rename
#undef remove

static int no_replace, fail_promote, fail_restore, fail_discard;
static int failures, checks;
static char final_path[600], temp_path[620], rollback[620];
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static int fake_rename(const char *from, const char *to)
{
  struct stat st;
  if ((no_replace && stat(to, &st) == 0) ||
      (fail_promote && !strcmp(from, temp_path) && !strcmp(to, final_path)) ||
      (fail_restore && !strcmp(from, rollback) && !strcmp(to, final_path)))
  { errno = EACCES; return -1; }
  return rename(from, to);
}
static int fake_remove(const char *path)
{
  if (fail_discard && !strcmp(path, rollback)) { errno = EACCES; return -1; }
  return remove(path);
}
static void put(const char *path, const char *text)
{
  FILE *f = fopen(path, "wb");
  if (!f || fwrite(text, 1, strlen(text), f) != strlen(text) || fclose(f)) exit(2);
}
static int has(const char *path, const char *text)
{
  char buf[64] = {0};
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  size_t n = fread(buf, 1, sizeof(buf), f);
  fclose(f);
  return n == strlen(text) && !memcmp(buf, text, n);
}
int main(void)
{
  char dir[] = "/tmp/gpsp-replace-XXXXXX";
  if (!mkdtemp(dir)) return 2;
  snprintf(final_path, sizeof(final_path), "%s/save", dir);
  snprintf(temp_path, sizeof(temp_path), "%s.tmp", final_path);
  snprintf(rollback, sizeof(rollback), "%s.rollback", final_path);
  put(final_path, "old"); put(temp_path, "new");
  CHECK(nspire_file_replace(temp_path, final_path) && has(final_path, "new"));
  no_replace = 1;
  put(temp_path, "newer");
  CHECK(nspire_file_replace(temp_path, final_path) && has(final_path, "newer"));
  CHECK(access(rollback, F_OK) != 0);
  fail_promote = 1; put(temp_path, "failed");
  CHECK(!nspire_file_replace(temp_path, final_path));
  CHECK(has(final_path, "newer") && has(temp_path, "failed"));
  fail_restore = 1;
  CHECK(!nspire_file_replace(temp_path, final_path));
  CHECK(access(final_path, F_OK) != 0 && has(rollback, "newer"));
  fail_restore = fail_promote = 0;
  CHECK(nspire_file_recover(final_path) && has(final_path, "newer"));
  /* A restart after moving the old name, before promoting the new one. */
  CHECK(rename(final_path, rollback) == 0);
  CHECK(nspire_file_recover(final_path) && has(final_path, "newer"));
  /* A stale rollback from a successful replacement must not block saves. */
  put(rollback, "older"); put(temp_path, "latest");
  CHECK(nspire_file_replace(temp_path, final_path) && has(final_path, "latest"));
  CHECK(access(rollback, F_OK) != 0);
  CHECK(!nspire_file_replace(final_path, final_path) && has(final_path, "latest"));
  put(rollback, "previous"); put(temp_path, "pending");
  fail_discard = 1;
  CHECK(!nspire_file_delete(final_path) && has(final_path, "latest") && has(rollback, "previous"));
  fail_discard = 0;
  CHECK(nspire_file_delete(final_path));
  CHECK(access(final_path, F_OK) != 0 && access(rollback, F_OK) != 0 && access(temp_path, F_OK) != 0);
  CHECK(nspire_file_recover(final_path) && access(final_path, F_OK) != 0);
  put(rollback, "orphan");
  CHECK(nspire_file_delete(final_path) && access(rollback, F_OK) != 0);
  put(final_path, "safe"); CHECK(mkdir(rollback, 0700) == 0);
  CHECK(!nspire_file_delete(final_path) && has(final_path, "safe"));
  no_replace = 0; put(temp_path, "safe update");
  CHECK(nspire_file_replace(temp_path, final_path) && has(final_path, "safe update"));
  { struct stat st; CHECK(stat(rollback, &st) == 0 && S_ISDIR(st.st_mode)); }
  rmdir(rollback);
  remove(final_path); CHECK(mkdir(final_path, 0700) == 0); put(temp_path, "safe");
  CHECK(!nspire_file_replace(temp_path, final_path) && has(temp_path, "safe"));
  remove(temp_path); rmdir(final_path);
  /* r19-snd: nspire_file_replace_step, a save's replace a step a frame: the
   * calculator's rename (no_replace) over an old file in 6 steps, a new name
   * in 3; stopped after any step (the battery out, a reset) the save is the
   * old one or the new one once recovered, never neither; finished, the new. */
  for (int over = 0; over < 2; over++)
    for (int stop = 0; stop <= 6; stop++)
    {
      struct nspire_replace r;
      int done = NSPIRE_REPLACE_BUSY, steps = 0;
      no_replace = 1;
      remove(final_path); remove(rollback); remove(temp_path);
      if (over) put(final_path, "old");
      put(temp_path, "new");
      CHECK(nspire_file_replace_begin(&r, temp_path, final_path));
      while (steps < stop && done == NSPIRE_REPLACE_BUSY)
      {
        done = nspire_file_replace_step(&r);
        steps++;
      }
      if (done == NSPIRE_REPLACE_BUSY)
      {
        /* (gpSP starts again: the save's file is recovered before it's read) */
        CHECK(nspire_file_recover(final_path));
        CHECK(has(final_path, "new") || (over ? has(final_path, "old") : access(final_path, F_OK) != 0));
        CHECK(has(final_path, "new") || has(temp_path, "new"));
      }
      else
      {
        CHECK(done == 1 && steps == (over ? 6 : 3));
        CHECK(has(final_path, "new") && access(rollback, F_OK) != 0 && access(temp_path, F_OK) != 0);
      }
    }
  /* and the steps all at once are the one-call version's result */
  {
    struct nspire_replace r;
    int done;
    remove(final_path); remove(temp_path);
    put(final_path, "old"); put(temp_path, "new");
    CHECK(nspire_file_replace_begin(&r, temp_path, final_path));
    while ((done = nspire_file_replace_step(&r)) == NSPIRE_REPLACE_BUSY)
      ;
    CHECK(done == 1 && has(final_path, "new") && access(rollback, F_OK) != 0);
    CHECK(!nspire_file_replace_begin(&r, final_path, final_path));
  }
  no_replace = 0;
  remove(final_path); remove(temp_path); remove(rollback); rmdir(dir);
  printf("file replacement: %d checks, %d failed\n", checks, failures);
  return failures != 0;
}
