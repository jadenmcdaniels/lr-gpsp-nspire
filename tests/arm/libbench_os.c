/*
 * The calculator's side of newlib, for tests/arm/libbench.c run under
 * qemu-arm: the Ndless SDK's libsyscalls (libsyscalls/stdlib.cpp, which
 * turns newlib's _open/_read/... into the OS's own file calls) as it is,
 * over a stand-in for the OS's file system and heap.
 *
 * What runs on the calculator is kept: newlib's stdio with libsyscalls
 * under it, so a FILE gets the same 1024-byte buffer (libsyscalls' _fstat
 * fails, so newlib takes BUFSIZ and never optimizes fseek), fopen "wb"
 * makes the OS open the file twice (O_CREAT: fopen "a" and fclose first),
 * an lseek is the OS's fseek and ftell, malloc is the OS's (8 more bytes,
 * aligned by hand), calloc the OS's malloc and memset.
 *
 * What the OS does isn't known here and isn't modeled: its file calls and
 * heap are the os_* functions below (files through semihosting, a
 * first-fit heap), which tests/libbench.py puts in a region of their own
 * and leaves out of the times. They count each call and its bytes instead
 * (os_count), and the stdio calls the program makes (--wrap, the
 * __wrap_* functions) are counted the same way. Directories: semihosting
 * can't list them, so a folder's names are read from its ".list" file,
 * which tests/libbench.py writes (one name per line, a trailing / for a
 * folder); stat says a path with a ".list" in it is a folder.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <reent.h>
#include <dirent.h>

#undef errno
extern int errno;

#define NOINLINE __attribute__((noinline, noclone))

/* ---- Semihosting ---- */

enum
{
  SYS_OPEN = 0x01, SYS_CLOSE = 0x02, SYS_WRITE = 0x05, SYS_READ = 0x06, SYS_SEEK = 0x0A,
  SYS_FLEN = 0x0C, SYS_REMOVE = 0x0E, SYS_RENAME = 0x0F, SYS_EXIT_EXTENDED = 0x20
};

static NOINLINE int os_semi(int op, void *arg)
{
  register int r0 __asm__("r0") = op;
  register void *r1 __asm__("r1") = arg;
  __asm__ volatile("svc 0x123456" : "+r"(r0) : "r"(r1) : "memory");
  return r0;
}

static NOINLINE int os_semi_open(const char *name, int mode)
{
  int args[3] = { (int)(uintptr_t)name, mode, (int)strlen(name) };
  return os_semi(SYS_OPEN, args);
}

static int stdio_handles[3];   /* stdin, stdout, stderr (":tt") */

/* ---- Counting ---- */

/* What a call was: the stdio functions (the program's calls) and the OS's
 * (what the file system sees). */
static const char *const op_names[] =
{
  "fopen", "fclose", "fread", "fwrite", "fseek", "ftell", "fgets", "fputs", "fprintf", "fflush",
  "rename", "remove", "stat",
  "OS fopen", "OS fclose", "OS fread", "OS fwrite", "OS fseek", "OS ftell", "OS fgetc", "OS stat",
  "OS unlink", "OS rename", "OS mkdir", "OS rmdir", "OS opendir", "OS readdir", "OS closedir",
  "OS malloc", "OS free", "OS realloc", "OS memset"
};
enum
{
  C_FOPEN, C_FCLOSE, C_FREAD, C_FWRITE, C_FSEEK, C_FTELL, C_FGETS, C_FPUTS, C_FPRINTF, C_FFLUSH,
  C_RENAME, C_REMOVE, C_STAT,
  OS_FOPEN, OS_FCLOSE, OS_FREAD, OS_FWRITE, OS_FSEEK, OS_FTELL, OS_FGETC, OS_STAT, OS_UNLINK,
  OS_RENAME, OS_MKDIR, OS_RMDIR, OS_OPENDIR, OS_READDIR, OS_CLOSEDIR, OS_MALLOC, OS_FREE, OS_REALLOC, OS_MEMSET,
  OPS
};

/* Each (call, size) pair seen: how often, in the step being measured. */
#define SIZES 512
static struct
{
  unsigned short op;
  unsigned short step;
  uint32_t size, count;
} sizes[SIZES];
static int n_sizes, sizes_lost;
/* Set by the bench (libbench.c): 0 before the first step (not counted). */
int os_count_step;

NOINLINE void os_count(int op, uint32_t size)
{
  int i;

  if (!os_count_step)
    return;
  for (i = n_sizes - 1; i >= 0; i--)
    if (sizes[i].op == op && sizes[i].size == size && sizes[i].step == os_count_step)
    {
      sizes[i].count++;
      return;
    }
  if (n_sizes < SIZES)
  {
    sizes[n_sizes].op = (unsigned short)op;
    sizes[n_sizes].step = (unsigned short)os_count_step;
    sizes[n_sizes].size = size;
    sizes[n_sizes++].count = 1;
  }
  else
    sizes_lost++;
}

/* Output, in the OS stand-in's region so the model leaves it out. */
NOINLINE int os_printf(const char *format, ...)
{
  char buf[1024];
  va_list ap;
  int n, args[3];

  va_start(ap, format);
  n = vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  if (n > (int)sizeof(buf) - 1)
    n = sizeof(buf) - 1;
  args[0] = stdio_handles[1];
  args[1] = (int)(uintptr_t)buf;
  args[2] = n;
  os_semi(SYS_WRITE, args);
  return n;
}

/* The counts, after the last step: "calls STEP OP|SIZE COUNT" lines. */
NOINLINE void os_count_print(void)
{
  int i;
  for (i = 0; i < n_sizes; i++)
    os_printf("calls %u %s|%lu %lu\n", sizes[i].step, op_names[sizes[i].op], (unsigned long)sizes[i].size,
              (unsigned long)sizes[i].count);
  if (sizes_lost)
    os_printf("calls 0 lost|0 %d\n", sizes_lost);
}

/* Ends the program at once: what exit() does (stdio's flushing) would
 * come after the last step and be listed with it. */
NOINLINE __attribute__((noreturn)) void os_exit(int code)
{
  int args[2] = { 0x20026, code };   /* ADP_Stopped_ApplicationExit */
  os_semi(SYS_EXIT_EXTENDED, args);
  for (;;)
    ;
}

/* ---- The OS's files (Nucleus's NUC_FILE, through semihosting) ---- */

typedef struct
{
  int handle, in_use, append, eof;
  long pos;
} os_file;

#define OS_FILES 24
static os_file os_files[OS_FILES];

static NOINLINE os_file *os_fopen(const char *path, const char *mode)
{
  int m, i, h;
  int plus = strchr(mode, '+') != NULL, bin = strchr(mode, 'b') != NULL;

  os_count(OS_FOPEN, 0);
  switch (mode[0])
  {
    case 'r': m = 0; break;
    case 'w': m = 4; break;
    case 'a': m = 8; break;
    default: return NULL;
  }
  m += (plus ? 2 : 0) + (bin ? 1 : 0);
  /* a folder isn't a file */
  if (strstr(path, ".list"))
    return NULL;
  h = os_semi_open(path, m);
  if (h < 0)
    return NULL;
  for (i = 0; i < OS_FILES && os_files[i].in_use; i++)
    ;
  if (i == OS_FILES)
    return NULL;
  os_files[i].handle = h;
  os_files[i].in_use = 1;
  os_files[i].append = mode[0] == 'a';
  os_files[i].eof = 0;
  os_files[i].pos = 0;
  if (os_files[i].append)
    os_files[i].pos = os_semi(SYS_FLEN, &h);
  return &os_files[i];
}

static NOINLINE int os_fclose(os_file *f)
{
  os_count(OS_FCLOSE, 0);
  os_semi(SYS_CLOSE, &f->handle);
  f->in_use = 0;
  return 0;
}

static NOINLINE int os_fread(void *ptr, int len, os_file *f)
{
  int args[3] = { f->handle, (int)(uintptr_t)ptr, len };
  int n;

  os_count(OS_FREAD, (uint32_t)len);
  {
    int seek[2] = { f->handle, (int)f->pos };
    os_semi(SYS_SEEK, seek);
  }
  n = len - os_semi(SYS_READ, args);
  f->pos += n;
  f->eof = n < len;
  return n;
}

static NOINLINE int os_fwrite(const void *ptr, int len, os_file *f)
{
  int args[3] = { f->handle, (int)(uintptr_t)ptr, len };
  int n;

  os_count(OS_FWRITE, (uint32_t)len);
  if (!f->append)
  {
    int seek[2] = { f->handle, (int)f->pos };
    os_semi(SYS_SEEK, seek);
  }
  n = len - os_semi(SYS_WRITE, args);
  f->pos += n;
  return n;
}

static NOINLINE int os_fseek(os_file *f, long off, int whence)
{
  long size;

  os_count(OS_FSEEK, 0);
  if (whence == SEEK_SET)
    f->pos = off;
  else if (whence == SEEK_CUR)
    f->pos += off;
  else
  {
    size = os_semi(SYS_FLEN, &f->handle);
    f->pos = size + off;
  }
  f->eof = 0;
  return f->pos < 0 ? -1 : 0;
}

static NOINLINE long os_ftell(os_file *f)
{
  os_count(OS_FTELL, 0);
  return f->pos;
}

static NOINLINE int os_fgetc(os_file *f)
{
  os_count(OS_FGETC, 0);
  return f->eof ? EOF : 0;
}

/* A folder is a path whose ".list" opens. */
static NOINLINE int os_is_dir(const char *path)
{
  char list[700];
  size_t len = strlen(path);
  int h;

  if (len + 7 > sizeof(list))
    return 0;
  memcpy(list, path, len);
  if (len && path[len - 1] == '/')
    len--;
  if (len == 1 && path[0] == '.')
    len = 0;
  memcpy(list + len, len ? "/.list" : ".list", len ? 7 : 6);
  h = os_semi_open(list, 1);
  if (h < 0)
    return 0;
  os_semi(SYS_CLOSE, &h);
  return 1;
}

static NOINLINE int os_stat_quiet(const char *path, struct stat *st)
{
  int h;

  memset(st, 0, sizeof(*st));
  if (!strstr(path, ".list") && (h = os_semi_open(path, 1)) >= 0)
  {
    st->st_mode = S_IFREG | 0644;
    st->st_size = os_semi(SYS_FLEN, &h);
    os_semi(SYS_CLOSE, &h);
    return 0;
  }
  if (os_is_dir(path))
  {
    st->st_mode = S_IFDIR | 0755;
    return 0;
  }
  return -1;
}

static NOINLINE int os_stat(const char *path, struct stat *st)
{
  os_count(OS_STAT, 0);
  return os_stat_quiet(path, st);
}

static NOINLINE int os_unlink(const char *path)
{
  int args[2] = { (int)(uintptr_t)path, (int)strlen(path) };
  os_count(OS_UNLINK, 0);
  return os_semi(SYS_REMOVE, args) == 0 ? 0 : -1;
}

static NOINLINE int os_rename(const char *from, const char *to)
{
  int args[4] = { (int)(uintptr_t)from, (int)strlen(from), (int)(uintptr_t)to, (int)strlen(to) };
  os_count(OS_RENAME, 0);
  return os_semi(SYS_RENAME, args) == 0 ? 0 : -1;
}

/* ---- The OS's heap: first fit over _sbrk (tests/arm/newlib_shim.c) ---- */

typedef struct Block { size_t size; struct Block *next; } Block;
#define HDR ((sizeof(Block) + 7) & ~(size_t)7)
static Block *free_list;
static size_t round_size(size_t n) { return (n + HDR + 7) & ~(size_t)7; }
void *_sbrk(ptrdiff_t incr);

static NOINLINE void os_free(void *ptr)
{
  Block *b, **pp = &free_list, *prev = NULL;

  if (!ptr)
    return;
  b = (Block *)((char *)ptr - HDR);
  while (*pp && *pp < b)
  {
    prev = *pp;
    pp = &(*pp)->next;
  }
  b->next = *pp;
  *pp = b;
  if (b->next && (char *)b + b->size == (char *)b->next)
  {
    b->size += b->next->size;
    b->next = b->next->next;
  }
  if (prev && (char *)prev + prev->size == (char *)b)
  {
    prev->size += b->size;
    prev->next = b->next;
  }
}

static NOINLINE void *os_malloc(size_t n)
{
  size_t need = round_size(n ? n : 1), grow;
  Block **pp = &free_list, *b;
  char *p;

  for (b = free_list; b; pp = &b->next, b = b->next)
    if (b->size >= need)
    {
      if (b->size - need >= HDR + 16)
      {
        Block *rest = (Block *)((char *)b + need);
        rest->size = b->size - need;
        rest->next = b->next;
        *pp = rest;
        b->size = need;
      }
      else
        *pp = b->next;
      return (char *)b + HDR;
    }
  grow = need < (1 << 20) ? (1 << 20) : need;
  p = (char *)_sbrk((ptrdiff_t)grow);
  if (p == (char *)-1)
    return NULL;
  b = (Block *)p;
  b->size = grow;
  if (grow - need >= HDR + 16)
  {
    Block *rest = (Block *)(p + need);
    rest->size = grow - need;
    b->size = need;
    os_free((char *)rest + HDR);
  }
  return p + HDR;
}

static NOINLINE void *os_realloc(void *ptr, size_t n)
{
  Block *b;
  size_t have;
  void *q;

  if (!ptr)
    return os_malloc(n);
  b = (Block *)((char *)ptr - HDR);
  have = b->size - HDR;
  if (have >= n)
    return ptr;
  q = os_malloc(n);
  if (q)
  {
    memcpy(q, ptr, have);
    os_free(ptr);
  }
  return q;
}

static NOINLINE void *os_malloc_counted(size_t n)
{
  os_count(OS_MALLOC, (uint32_t)n);
  return os_malloc(n);
}

static NOINLINE void os_free_counted(void *p)
{
  os_count(OS_FREE, 0);
  os_free(p);
}

static NOINLINE void *os_realloc_counted(void *p, size_t n)
{
  os_count(OS_REALLOC, (uint32_t)n);
  return os_realloc(p, n);
}

static NOINLINE void os_memset_counted(uint32_t n)
{
  os_count(OS_MEMSET, n);
}

/* ---- The OS's folders (Ndless's dirent.h: nuc_opendir, nuc_readdir) ---- */

struct shim_dir
{
  char *names;      /* the .list file's bytes, one name per line */
  size_t size, at;
  struct dirent entry;
};

static NOINLINE DIR *os_opendir(const char *path)
{
  char list[700];
  size_t len = strlen(path);
  struct shim_dir *d;
  int h;

  os_count(OS_OPENDIR, 0);
  if (len + 7 > sizeof(list))
    return NULL;
  memcpy(list, path, len);
  if (len && path[len - 1] == '/')
    len--;
  memcpy(list + len, len ? "/.list" : ".list", len ? 7 : 6);
  h = os_semi_open(list, 1);
  if (h < 0)
    return NULL;
  d = (struct shim_dir *)os_malloc(sizeof(*d));
  memset(d, 0, sizeof(*d));
  d->size = (size_t)os_semi(SYS_FLEN, &h);
  d->names = (char *)os_malloc(d->size + 1);
  {
    int args[3] = { h, (int)(uintptr_t)d->names, (int)d->size };
    os_semi(SYS_READ, args);
  }
  os_semi(SYS_CLOSE, &h);
  return (DIR *)d;
}

static NOINLINE struct dirent *os_readdir(DIR *dir)
{
  struct shim_dir *d = (struct shim_dir *)dir;
  size_t n = 0;

  os_count(OS_READDIR, 0);
  if (!d || d->at >= d->size)
    return NULL;
  while (d->at < d->size && d->names[d->at] != '\n')
  {
    if (n < sizeof(d->entry.d_name) - 1)
      d->entry.d_name[n++] = d->names[d->at];
    d->at++;
  }
  d->at++;
  d->entry.d_type = DT_REG;
  if (n && d->entry.d_name[n - 1] == '/')
  {
    n--;
    d->entry.d_type = DT_DIR;
  }
  d->entry.d_name[n] = 0;
  return &d->entry;
}

static NOINLINE int os_closedir(DIR *dir)
{
  struct shim_dir *d = (struct shim_dir *)dir;
  os_count(OS_CLOSEDIR, 0);
  if (d)
  {
    os_free(d->names);
    os_free(d);
  }
  return 0;
}

/* ======== libsyscalls (stdlib.cpp), as it is ======== */

#define MAX_OPEN_FILES 20
static os_file *openfiles[MAX_OPEN_FILES];

static int errno_set(int error)
{
  errno = error;
  return -1;
}

#define GETFD(f) \
  do { if (file >= MAX_OPEN_FILES) return errno_set(EBADF); \
       f = openfiles[file]; if (f == 0) return errno_set(EBADF); } while (0)

/* stdin, stdout and stderr: the host's terminal (":tt") */
void initialise_monitor_handles(void)
{
  stdio_handles[0] = os_semi_open(":tt", 0);
  stdio_handles[1] = os_semi_open(":tt", 4);
  stdio_handles[2] = os_semi_open(":tt", 8);
}
int __heap_limit = 0xcafedead;

static int newslot(void)
{
  int i;
  for (i = 3; i < MAX_OPEN_FILES; i++)
    if (openfiles[i] == 0)
      return i;
  return -1;
}

void *malloc(size_t size)
{
  uint8_t *alloc_ptr, *aligned_ptr;

  if (!size)
    return NULL;
  alloc_ptr = (uint8_t *)os_malloc_counted(size + 8);
  if (!alloc_ptr)
  {
    errno = ENOMEM;
    return NULL;
  }
  aligned_ptr = (uint8_t *)(((uint32_t)(uintptr_t)alloc_ptr + 8) & ~7u);
  *(aligned_ptr - 1) = (uint8_t)(aligned_ptr - alloc_ptr);
  return aligned_ptr;
}

void free(void *mem)
{
  if (mem)
    os_free_counted((uint8_t *)mem - *((uint8_t *)mem - 1));
}

void *realloc(void *mem, size_t size)
{
  uint8_t old_alignment, new_alignment, *old_ptr, *new_ptr, *aligned_ptr;

  if (!mem)
    return malloc(size);
  if (!size)
  {
    free(mem);
    return NULL;
  }
  old_alignment = *((uint8_t *)mem - 1);
  old_ptr = (uint8_t *)mem - old_alignment;
  new_ptr = (uint8_t *)os_realloc_counted(old_ptr, size + 8);
  if (!new_ptr)
    return NULL;
  aligned_ptr = (uint8_t *)(((uint32_t)(uintptr_t)new_ptr + 8) & ~7u);
  new_alignment = (uint8_t)(aligned_ptr - new_ptr);
  if (old_alignment != new_alignment)
    memmove(new_ptr + new_alignment, new_ptr + old_alignment, size);
  *(aligned_ptr - 1) = new_alignment;
  return aligned_ptr;
}

/* The OS's memset (a syscall on the calculator) runs on the CPU: newlib's
 * here, counted as the program's time. */
void *calloc(size_t nmemb, size_t size)
{
  uint64_t of_size;
  void *ptr;

  if (nmemb == 0 || size == 0)
    return NULL;
  of_size = (uint64_t)nmemb * size;
  if (of_size != (uint32_t)of_size)
    return NULL;
  ptr = malloc((size_t)of_size);
  if (!ptr)
    return NULL;
  os_memset_counted((uint32_t)of_size);
  return memset(ptr, 0, (size_t)of_size);
}

void *_malloc_r(struct _reent *r, size_t size) { (void)r; return malloc(size); }
void _free_r(struct _reent *r, void *mem) { (void)r; free(mem); }
void *_realloc_r(struct _reent *r, void *mem, size_t size) { (void)r; return realloc(mem, size); }
void *_calloc_r(struct _reent *r, size_t nmemb, size_t size) { (void)r; return calloc(nmemb, size); }

void _exit(int ret)
{
  os_exit(ret);
}

void abort(void)
{
  os_exit(134);
}

int _kill(int pid, int sig)
{
  (void)pid;
  (void)sig;
  return errno_set(ENOSYS);
}

int _getpid(void)
{
  return 1;
}

void *_sbrk(ptrdiff_t incr)
{
  extern char end __asm__("end");
  static char *heap_end;
  char *prev;

  if (!heap_end)
    heap_end = &end;
  prev = heap_end;
  if (__heap_limit != (int)0xcafedead && heap_end + incr > (char *)(uintptr_t)__heap_limit)
  {
    errno = ENOMEM;
    return (void *)-1;
  }
  heap_end += incr;
  return prev;
}

int _stat(const char *path, struct stat *st)
{
  /* the OS's nuc_stat copied field by field: the same result */
  return os_stat(path, st) < 0 ? errno_set(ENOENT) : 0;
}

int _fstat(int file, struct stat *st)
{
  (void)file;
  (void)st;
  return errno_set(ENOSYS);
}

int _read(int file, char *ptr, int len)
{
  os_file *f;
  int ret;

  if (file < 3)
  {
    int args[3] = { stdio_handles[file], (int)(uintptr_t)ptr, len };
    return len - os_semi(SYS_READ, args);
  }
  GETFD(f);
  ret = os_fread(ptr, len, f);
  if (ret == 0)
  {
    if (os_fgetc(f) == EOF)
      return 0;
    return errno_set(EIO);
  }
  return ret;
}

int _write(int file, char *ptr, int len)
{
  os_file *f;
  int ret;

  if (file < 3)
  {
    int args[3] = { stdio_handles[file], (int)(uintptr_t)ptr, len };
    return len - os_semi(SYS_WRITE, args);
  }
  GETFD(f);
  ret = os_fwrite(ptr, len, f);
  if (ret == 0)
    return errno_set(EIO);
  return ret;
}

int _lseek(int file, int ptr, int dir)
{
  os_file *f;
  GETFD(f);
  if (os_fseek(f, ptr, dir) == -1)
    return errno_set(EINVAL);
  return (int)os_ftell(f);
}

int _open(const char *path, int flags)
{
  char mode[4];
  int curmode = 0;
  int file = newslot();

  if (file == -1)
    return errno_set(EMFILE);
  if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL))
  {
    struct stat st;
    if (_stat(path, &st))
      return errno_set(EEXIST);
  }
  if (flags & O_CREAT)
  {
    os_file *f = os_fopen(path, "a");
    if (f == 0)
      return errno_set(ENOENT);
    os_fclose(f);
  }
  if (flags & O_APPEND)
  {
    mode[curmode++] = 'a';
    if (flags & O_RDWR)
      mode[curmode++] = '+';
  }
  else if (flags & O_WRONLY)
    mode[curmode++] = 'w';
  else if (flags & O_RDWR)
  {
    mode[curmode++] = 'r';
    mode[curmode++] = '+';
  }
  else
    mode[curmode++] = 'r';
  mode[curmode++] = 'b';
  mode[curmode] = 0;
  openfiles[file] = os_fopen(path, mode);
  if (openfiles[file] == 0)
    return errno_set(ENOENT);
  return file;
}

int _close(int file)
{
  os_file *f;
  if (file < 3)
    return errno_set(EIO);
  GETFD(f);
  openfiles[file] = 0;
  return os_fclose(f);
}

int _isatty(int file)
{
  return file < 3;
}

int isatty(int file)
{
  return file < 3;
}

int _unlink(const char *path)
{
  return os_unlink(path) ? errno_set(ENOENT) : 0;
}

int _link(const char *oldpath, const char *newpath)
{
  (void)oldpath;
  (void)newpath;
  return errno_set(ENOSYS);
}

int _rename(const char *oldpath, const char *newpath)
{
  return os_rename(oldpath, newpath) ? errno_set(ENOENT) : 0;
}

int mkdir(const char *path, mode_t mode)
{
  struct stat st;
  (void)mode;
  os_count(OS_MKDIR, 0);
  /* folders are made by tests/libbench.py: one that's there is "made" */
  return os_stat_quiet(path, &st) == 0 ? errno_set(EEXIST) : errno_set(ENOENT);
}

int rmdir(const char *path)
{
  (void)path;
  os_count(OS_RMDIR, 0);
  return errno_set(ENOTEMPTY);
}

static char cwd[512] = "/documents/";

char *getcwd(char *buf, size_t size)
{
  snprintf(buf, size, "%s", cwd);
  return buf;
}

/* nspire.h's getcwd: the OS's NU_Current_Dir, 0 when it works */
int NU_Current_Dir(const char *drive, char *path)
{
  (void)drive;
  strcpy(path, cwd);
  return 0;
}

int chdir(const char *path)
{
  snprintf(cwd, sizeof(cwd), "%s", path);
  return 0;
}

int _gettimeofday(struct timeval *tv, void *tz)
{
  (void)tz;
  if (tv)
  {
    tv->tv_sec = 0;
    tv->tv_usec = 0;
  }
  return 0;
}

clock_t _times(struct tms *ptms)
{
  (void)ptms;
  return 0;
}

int _system(const char *s)
{
  (void)s;
  return -1;
}

/* Ndless's dirent.h: the OS's own calls */
DIR *opendir(const char *dir)
{
  return os_opendir(dir);
}

struct dirent *readdir(DIR *dir)
{
  return os_readdir(dir);
}

int closedir(DIR *dir)
{
  return os_closedir(dir);
}

/* ======== The program's stdio calls, counted (ld --wrap) ======== */

FILE *__real_fopen(const char *path, const char *mode);
int __real_fclose(FILE *f);
size_t __real_fread(void *p, size_t s, size_t n, FILE *f);
size_t __real_fwrite(const void *p, size_t s, size_t n, FILE *f);
int __real_fseek(FILE *f, long off, int whence);
long __real_ftell(FILE *f);
int __real_fseeko(FILE *f, off_t off, int whence);
off_t __real_ftello(FILE *f);
char *__real_fgets(char *s, int n, FILE *f);
int __real_fputs(const char *s, FILE *f);
int __real_fflush(FILE *f);
int __real_rename(const char *a, const char *b);
int __real_remove(const char *a);
int __real_stat(const char *a, struct stat *st);

FILE *__wrap_fopen(const char *path, const char *mode)
{
  os_count(C_FOPEN, mode[0]);
  return __real_fopen(path, mode);
}

int __wrap_fclose(FILE *f)
{
  os_count(C_FCLOSE, 0);
  return __real_fclose(f);
}

size_t __wrap_fread(void *p, size_t s, size_t n, FILE *f)
{
  os_count(C_FREAD, (uint32_t)(s * n));
  return __real_fread(p, s, n, f);
}

size_t __wrap_fwrite(const void *p, size_t s, size_t n, FILE *f)
{
  os_count(C_FWRITE, (uint32_t)(s * n));
  return __real_fwrite(p, s, n, f);
}

int __wrap_fseek(FILE *f, long off, int whence)
{
  os_count(C_FSEEK, (uint32_t)whence);
  return __real_fseek(f, off, whence);
}

long __wrap_ftell(FILE *f)
{
  os_count(C_FTELL, 0);
  return __real_ftell(f);
}

int __wrap_fseeko(FILE *f, off_t off, int whence)
{
  os_count(C_FSEEK, (uint32_t)whence);
  return __real_fseeko(f, off, whence);
}

off_t __wrap_ftello(FILE *f)
{
  os_count(C_FTELL, 0);
  return __real_ftello(f);
}

char *__wrap_fgets(char *s, int n, FILE *f)
{
  os_count(C_FGETS, 0);
  return __real_fgets(s, n, f);
}

int __wrap_fputs(const char *s, FILE *f)
{
  os_count(C_FPUTS, 0);
  return __real_fputs(s, f);
}

int __wrap_fprintf(FILE *f, const char *format, ...)
{
  va_list ap;
  int n;
  os_count(C_FPRINTF, 0);
  va_start(ap, format);
  n = vfprintf(f, format, ap);
  va_end(ap);
  return n;
}

int __wrap_fflush(FILE *f)
{
  os_count(C_FFLUSH, 0);
  return __real_fflush(f);
}

int __wrap_rename(const char *a, const char *b)
{
  os_count(C_RENAME, 0);
  return __real_rename(a, b);
}

int __wrap_remove(const char *a)
{
  os_count(C_REMOVE, 0);
  return __real_remove(a);
}

int __wrap_stat(const char *a, struct stat *st)
{
  os_count(C_STAT, 0);
  return __real_stat(a, st);
}
