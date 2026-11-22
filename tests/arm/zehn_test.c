/* The packed program file (tools/zehn_pack.py, nspire/zehn_entry.S)
 * loads into the same memory as
 * genzehn's own file:
 *
 *   zehn_test PLAIN.zehn PACKED.tns
 *
 * Each file is loaded as Ndless's zehn_load does (Ndless's
 * zehn_loader.cpp: unpack, zeros to the end of the program's memory, the
 * relocations in the file) at the same address; for the packed one, the
 * stub's relocation routine is then run from the loaded image itself (the
 * bl after the entry's push), as the calculator runs it. Every byte of
 * the program's memory must then be the plain file's (its branches and
 * literal loads put back, its relocations made), but for the stub's four
 * parameter words.
 * The plain image's routine must do nothing (its parameters are 0). Not
 * run here: the entry's push, cache flush and branch to crt0 (cp15 needs a
 * privileged mode).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* Where both are loaded: executable, as Ndless's execmem_alloc. */
__asm__(".section .zehn_image,\"awx\",%nobits\n"
        ".balign 4096\n"
        ".global zehn_image\n"
        "zehn_image: .space 16 << 20\n"
        ".previous\n");
extern uint8_t zehn_image[16 << 20];

static uint8_t *read_file(const char *path, long *len) {
  FILE *f = fopen(path, "rb");
  uint8_t *d;
  if (!f) { printf("FAIL: can't open %s\n", path); exit(1); }
  fseek(f, 0, SEEK_END);
  *len = ftell(f);
  fseek(f, 0, SEEK_SET);
  d = malloc(*len);
  if (!d || fread(d, 1, *len, f) != (size_t)*len) { printf("FAIL: can't read %s\n", path); exit(1); }
  fclose(f);
  return d;
}

static uint32_t rd(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* zehn_load's work on the image; returns the entry's offset, or -1. */
static long load(const char *path, uint32_t *mem_out, int *relocs_out) {
  long len, i;
  uint8_t *d = read_file(path, &len);
  uint32_t sig = rd(d), ver = rd(d + 4), file_size = rd(d + 8), rc = rd(d + 12), fc = rd(d + 16);
  uint32_t es = rd(d + 20), alloc_size = rd(d + 24), entry = rd(d + 28);
  uint32_t head = 32 + 4 * rc + 4 * fc + es, mem = alloc_size - head, body = file_size - head;
  const uint8_t *rel = d + 32;
  if (sig != 0x6e68655a || ver != 1 || file_size > alloc_size || file_size > (uint32_t)len
      || mem > sizeof zehn_image) {
    printf("FAIL: %s is not a Zehn file this test can load\n", path);
    return -1;
  }
  memset(zehn_image, 0x5a, mem);   /* the loader's malloc gives no zeros */
  if (rc && (rd(rel) & 0xff) == 3) {
    uLongf out = mem;
    if (rd(rel) >> 8 || uncompress(zehn_image, &out, d + head, body) != Z_OK) {
      printf("FAIL: %s doesn't unpack\n", path);
      return -1;
    }
    memset(zehn_image + out, 0, mem - out);
  } else {
    memcpy(zehn_image, d + head, body);
    memset(zehn_image + body, 0, mem - body);
  }
  for (i = 0; i < (long)rc; i++) {
    uint32_t r = rd(rel + 4 * i), t = r & 0xff, off = r >> 8;
    uint8_t *p = zehn_image + off;
    if (off >= mem) { printf("FAIL: %s: a relocation outside the program\n", path); return -1; }
    if (t == 0)
      wr(p, rd(p) + (uint32_t)zehn_image);
    else if (t == 1)
      for (; rd(p) != 0xFFFFFFFF; p += 4)
        wr(p, rd(p) + (uint32_t)zehn_image);
    else if (t == 2)
      wr(p, 0);
    else if (t != 3 && t != 4) { printf("FAIL: %s: relocation type %u\n", path, (unsigned)t); return -1; }
  }
  free(d);
  *mem_out = mem;
  *relocs_out = rc;
  return entry;
}

/* The stub's routine: the bl right after the entry's push. */
static int (*relocate_at(long entry))(void) {
  uint32_t push = rd(zehn_image + entry), bl = rd(zehn_image + entry + 4);
  int32_t off = (int32_t)(bl << 8) >> 6;
  if (push != 0xe92d5fff || (bl >> 24) != 0xeb)
    return NULL;
  return (int (*)(void))(zehn_image + entry + 4 + 8 + off);
}

int main(int argc, char **argv) {
  uint32_t mem_plain, mem, i, params = 0, diff = 0;
  int rc_plain, rc, done;
  long entry;
  uint8_t *want;
  int (*fn)(void);
  static const uint8_t magic[4] = {'G', 'Z', 'R', 'L'};
  if (argc != 3) { printf("usage: zehn_test PLAIN.zehn PACKED.tns\n"); return 2; }

  if (load(argv[1], &mem_plain, &rc_plain) < 0) return 1;
  want = malloc(mem_plain);
  if (!want) { printf("FAIL: no memory\n"); return 1; }
  memcpy(want, zehn_image, mem_plain);

  entry = load(argv[2], &mem, &rc);
  if (entry < 0) return 1;
  if (mem != mem_plain) {
    printf("FAIL: the packed program asks for %u bytes, the plain one %u\n", (unsigned)mem, (unsigned)mem_plain);
    return 1;
  }
  fn = relocate_at(entry);
  if (!fn) { printf("FAIL: the packed program's entry (0x%lx) isn't zehn_entry.S's\n", entry); return 1; }
  done = fn();
  for (i = entry; i + 20 <= (uint32_t)entry + 0x200; i += 4)
    if (!memcmp(zehn_image + i, magic, 4)) { params = i + 4; break; }
  if (!params) { printf("FAIL: no stub parameters after the entry\n"); return 1; }
  for (i = 0; i < mem; i++)
    if (zehn_image[i] != want[i] && (i < params || i >= params + 16))
      if (diff++ < 5)
        printf("  differs at 0x%x: 0x%02x, plain 0x%02x\n", (unsigned)i, zehn_image[i], want[i]);
  if (done != 1 || diff) {
    printf("FAIL: the packed program (%d relocations in the file, stub returned %d) differs from the plain one "
           "in %u bytes\n", rc, done, (unsigned)diff);
    return 1;
  }

  /* The plain image's routine: parameters 0, so it must leave all alone. */
  if (load(argv[1], &mem_plain, &rc_plain) < 0) return 1;
  fn = relocate_at(entry);
  if (!fn || fn() != 0 || memcmp(zehn_image, want, mem_plain)) {
    printf("FAIL: the stub changed the plain (unpacked) program\n");
    return 1;
  }
  printf("PASS: the packed program file loads as genzehn's own (%u bytes of memory; %d relocations in the file "
         "against %d, the rest fixed by its entry)\n", (unsigned)mem, rc, rc_plain);
  free(want);
  return 0;
}
