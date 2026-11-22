/* A zlib stream packed from one buffer into another, fast: states and their
 * pictures (nspire_states.c).
 *
 * The output is an ordinary zlib stream (RFC 1950/1951: 78 01, deflate
 * blocks, Adler-32), so zlib's uncompress, older builds and PCs read it. It
 * is made in one pass over the input as it is in memory (no window copy):
 * greedy matches from a small table of positions, then each block of up to
 * FASTDEFLATE_BLOCK_SYMBOLS symbols goes out as stored, fixed or dynamic
 * Huffman codes, whichever is smallest. */
#ifndef NSPIRE_FASTDEFLATE_H
#define NSPIRE_FASTDEFLATE_H

#include <stddef.h>

/* The most it writes for n bytes: the zlib header, n bytes stored in
 * blocks of 65535 with 5 bytes each, and the Adler-32. Below zlib's
 * compressBound(n). */
#define FASTDEFLATE_BOUND(n) ((n) + 5 * ((n) / 65535 + 1) + 6)

/* Packs src (n bytes) into dst (cap bytes); returns the size of the zlib
 * stream, 0 if it didn't fit in cap or there was no memory for the
 * work (never the input stored for that). */
size_t fastdeflate(unsigned char *dst, size_t cap, const unsigned char *src, size_t n);

/* The same for a picture of 16-bit pixels in rows of row bytes (1 to
 * 32768; any other row: no row is assumed): the pixels one row up and one
 * pixel back are tried first, and the search goes deeper than fastdeflate's
 * (a state's picture packs smaller than at zlib level 1, unpacks faster,
 * and still packs faster than zlib 1). */
size_t fastdeflate_picture(unsigned char *dst, size_t cap, const unsigned char *src, size_t n, unsigned row);

/* fastdeflate a part at a time (2026-11-17: a state packed over the game's
 * frames, nspire_states.c states_save_begin): begin (0: no memory for the
 * work), then step until it says 0 (each the next bytes of input, a 4 KB
 * piece past it at most), then end: fastdeflate's size and the same bytes
 * (the steps stop between the 4 KB pieces it works in anyway). src and dst
 * stay as they are meanwhile. end before the last step gives up (0); end
 * frees the work either way. */
struct fastdeflate_job
{
  void *work;
  unsigned char *dst;
  const unsigned char *src;
  size_t cap, n, pos, size;
  int more;
};
int fastdeflate_begin(struct fastdeflate_job *j, unsigned char *dst, size_t cap, const unsigned char *src, size_t n);
int fastdeflate_step(struct fastdeflate_job *j, size_t bytes);
size_t fastdeflate_end(struct fastdeflate_job *j);

/* save17: a work area for the packings (a malloc'd one of at least
 * fastdeflate_work_size() bytes, the most any of them needs), so that a
 * state saved late in a long session needs no fresh memory for it: one
 * packing at a time uses it instead of a malloc of its own (another one
 * meanwhile mallocs as before). NULL: none (the caller frees it after). */
size_t fastdeflate_work_size(void);
void fastdeflate_give_work(void *area, size_t size);

#endif
