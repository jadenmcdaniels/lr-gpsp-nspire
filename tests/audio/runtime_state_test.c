/* Reuse the established register sequence without changing its reference.
 * Exact constant-mode comparisons include every serialized byte. Toggle
 * comparisons normalize only buf-base, the frontend's PCM read cursor. */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int mode, canonical;
static unsigned frame;

static void runtime_init(void)
{
  init_sound();
#ifdef NSPIRE_RUNTIME_AUDIO
  sound_set_output_enabled(mode == 1);
#endif
}

static void runtime_render(void)
{
#ifdef NSPIRE_RUNTIME_AUDIO
  if (mode == 2)
    sound_set_output_enabled((frame % 11) < 5);
#endif
  render_gbc_sound();
  frame++;
}

static unsigned runtime_write(u8 *out)
{
  static const char digits[] = "0123456789abcdef";
  unsigned size, i;
  u8 doc[4096], *header = doc;
  char hex[8193];
  if (!canonical) {
    size = sound_write_savestate(out);
  } else {
    size = sound_write_savestate(doc + 4);
    doc[4 + size] = 0;
    bson_write_u32(header, size + 5);
    u8 *cursor = (u8 *)bson_find_key(bson_find_document(doc, "sound"), "buf-base");
    if (!cursor) {
      fputs("FAIL runtime state: missing PCM read cursor\n", stderr);
      exit(1);
    }
    memset(cursor, 0, 4);
    memcpy(out, doc + 4, size);
  }
  if (size > sizeof(hex) / 2) {
    fputs("FAIL runtime state: serialized state exceeds fixture buffer\n", stderr);
    exit(1);
  }
  /* Compare the actual bytes, in addition to the established sequence's
   * hashes/status lines. One output call keeps semihosted tests inexpensive. */
  for (i = 0; i < size; i++) {
    hex[i * 2] = digits[out[i] >> 4];
    hex[i * 2 + 1] = digits[out[i] & 15];
  }
  hex[size * 2] = 0;
  puts(hex);
  return size;
}

#define init_sound runtime_init
#define render_gbc_sound runtime_render
#define sound_write_savestate runtime_write
#define main reference_sequence
#include "state_test.c"
#undef main
#undef sound_write_savestate
#undef render_gbc_sound
#undef init_sound

int main(int argc, char **argv)
{
  mode = argc > 1 && !strcmp(argv[1], "on") ? 1 :
         argc > 1 && !strcmp(argv[1], "toggle") ? 2 : 0;
  canonical = mode == 2;
  return reference_sequence();
}
