/* fast17: every remove(), rename() and rmdir() of gpSP's goes through
 * these (the Makefile's -Wl,--wrap=remove,--wrap=rename,--wrap=rmdir), so
 * the OS's file list is known stale or not at exit without reading the
 * saves folder again (nspire_paths.h nspire_file_gone). The PC's tests do
 * the same in host_hw.c's wrappers. */

#include "nspire_paths.h"

int __real_remove(const char *path);
int __real_rename(const char *from, const char *to);
int __real_rmdir(const char *path);

int __wrap_remove(const char *path)
{
  int r = __real_remove(path);
  if (r == 0)
    nspire_file_gone(path);
  return r;
}

int __wrap_rename(const char *from, const char *to)
{
  int r = __real_rename(from, to);
  if (r == 0)
    nspire_file_gone(from);
  return r;
}

int __wrap_rmdir(const char *path)
{
  int r = __real_rmdir(path);
  if (r == 0)
    nspire_file_list_touched();   /* a folder gone: the OS shows folders */
  return r;
}
