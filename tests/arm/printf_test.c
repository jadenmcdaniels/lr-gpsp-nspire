/* The printf family as the calculator build links it (Makefile R6b, r16:
 * --defsym _vfprintf_r and _svfprintf_r/_svfiprintf_r to newlib's integer
 * stream printf, _vfiprintf_r, so snprintf, sprintf and vsnprintf write
 * their string FILE through __sfvwrite_r) against newlib's own (run.sh
 * builds this file both ways and compares the two lines it prints): every
 * integer and string format the program uses, at buffer sizes 0 to 300
 * (snprintf's truncation and count), snprintf(NULL, 0, ...), a 4 KB
 * sprintf, vsnprintf, and fprintf to a file read back.
 *   printf_test FILE     (FILE: a scratch file for the fprintf part)
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
static unsigned long h = 5381;
static void mix(const void *p, size_t n) { const unsigned char *c = p; while (n--) h = h * 33 + *c++; }
static int vs(char *b, size_t n, const char *f, ...) { va_list ap; int r; va_start(ap, f); r = vsnprintf(b, n, f, ap); va_end(ap); return r; }
int main(int argc, char **argv) {
  const char *file = argc > 1 ? argv[1] : "printf_test.txt";
  static const char *fmts[] = { "%d", "%5d|%-5d|%05d", "%u %x %X %o", "%ld %lu %lx", "%lld %llu %llx", "%s", "%.3s|%10s|%-10s|", "%c%c%c",
    "%p", "%%", "%hhd %hd %hu", "%+d % d %#x %#o", "%*d|%-*d", "%.*s", "abc", "", "%s=%d (%s) %u%%", "%08lx", "%i", "%zu %zd" };
  static const size_t sizes[] = { 0, 1, 2, 3, 5, 8, 16, 64, 300 };
  char buf[512], big[2000];
  unsigned i, j, k;
  memset(big, 'q', sizeof big - 1); big[sizeof big - 1] = 0;
  for (i = 0; i < sizeof fmts / sizeof *fmts; i++)
    for (j = 0; j < sizeof sizes / sizeof *sizes; j++)
      for (k = 0; k < 4; k++) {
        int r;
        int iv[] = { 0, -1, 123456789, -2147483647 - 1 };
        long long llv[] = { 0, -1, 1234567890123LL, (long long)0x8000000000000000ULL };
        const char *sv[] = { "", "x", "hello world", big };
        memset(buf, 0x55, sizeof buf);
        switch (i) {
          case 0: case 18: r = snprintf(buf, sizes[j], fmts[i], iv[k]); break;
          case 1: r = snprintf(buf, sizes[j], fmts[i], iv[k], iv[k], iv[k]); break;
          case 2: r = snprintf(buf, sizes[j], fmts[i], (unsigned)iv[k], (unsigned)iv[k], (unsigned)iv[k], (unsigned)iv[k]); break;
          case 3: r = snprintf(buf, sizes[j], fmts[i], (long)iv[k], (unsigned long)iv[k], (unsigned long)iv[k]); break;
          case 4: r = snprintf(buf, sizes[j], fmts[i], llv[k], (unsigned long long)llv[k], (unsigned long long)llv[k]); break;
          case 5: r = snprintf(buf, sizes[j], fmts[i], sv[k]); break;
          case 6: r = snprintf(buf, sizes[j], fmts[i], sv[k], sv[k], sv[k]); break;
          case 7: r = snprintf(buf, sizes[j], fmts[i], 'a' + k, 0x7f, 'z'); break;
          case 8: r = snprintf(buf, sizes[j], fmts[i], (void *)(size_t)iv[k]); break;
          case 11: r = snprintf(buf, sizes[j], fmts[i], iv[k], iv[k], iv[k], iv[k]); break;
          case 10: r = snprintf(buf, sizes[j], fmts[i], iv[k], iv[k], iv[k]); break;
          case 12: r = snprintf(buf, sizes[j], fmts[i], (int)k * 3, iv[k], (int)k * 4, iv[k]); break;
          case 13: r = snprintf(buf, sizes[j], fmts[i], (int)k * 2, sv[k]); break;
          case 16: r = snprintf(buf, sizes[j], fmts[i], sv[k], iv[k], sv[3 - k], (unsigned)iv[k]); break;
          case 17: r = vs(buf, sizes[j], fmts[i], (unsigned long)iv[k]); break;
          case 19: r = snprintf(buf, sizes[j], fmts[i], (size_t)iv[k], (ssize_t)iv[k]); break;
          default: r = snprintf(buf, sizes[j], fmts[i]); break;
        }
        mix(&r, sizeof r); mix(buf, sizeof buf);
      }
  /* NULL with 0, sprintf, a long one */
  { int r = snprintf(NULL, 0, "%s %d", "abc", 42); mix(&r, sizeof r); }
  { char *b2 = malloc(5000); int r = sprintf(b2, "%s|%s|%d", big, big, 7); mix(&r, sizeof r); mix(b2, r + 1); free(b2); }
  { int r = sprintf(buf, "%d-%s", 5, "five"); mix(&r, sizeof r); mix(buf, r + 1); }
  { FILE *f = fopen(file, "w"); int r1 = fprintf(f, "%s %d %x\n", "line", -3, 255); int r2 = fprintf(f, "%s\n", big); fclose(f); mix(&r1, 4); mix(&r2, 4); }
  { FILE *f = fopen(file, "r"); int n = fread(big, 1, sizeof big - 1, f); fclose(f); mix(big, n); }
  printf("printf_test: %u cases, hash %08lx\n", (unsigned)(20 * 9 * 4 + 5), h);
  return 0;
}
