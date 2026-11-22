/* The optional desktop JIT allocator: mmap failure is not a mapped block.
 * Include the implementation with a fake allocator, so this runs without
 * executable mappings or address-space exhaustion. */
#include <stdio.h>
#include <stdint.h>
#define MMAP_JIT_CACHE
#define mmap test_mmap
#define munmap test_munmap
#include "../../libretro-gpsp/memmap.c"
#undef mmap
#undef munmap

static unsigned attempts, releases, fail = 1;
void *test_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
  (void)addr; (void)length; (void)prot; (void)flags; (void)fd; (void)offset;
  attempts++;
  return fail ? MAP_FAILED : (void *)(uintptr_t)map_jit_block;
}
int test_munmap(void *addr, size_t length)
{
  (void)addr; (void)length;
  releases++;
  return 0;
}

int main(void)
{
  if (map_jit_block(4096) || !attempts || releases)
    return fprintf(stderr, "failed mmap was treated as a mapping\n"), 1;
  fail = 0;
  void *p = map_jit_block(4096);
  if (p != (void *)(uintptr_t)map_jit_block)
    return fprintf(stderr, "valid nearby mapping rejected\n"), 1;
  unmap_jit_block(p, 4096);
  if (releases != 1 || validate_addr_offset((void *)(UINTPTR_MAX - 3), 4096, 2048))
    return fprintf(stderr, "mapping release or wraparound check failed\n"), 1;
  puts("memmap: allocation failure, nearby mapping, release and wraparound passed");
  return 0;
}
