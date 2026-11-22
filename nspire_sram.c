/* gpSP's hot code and its newest translated blocks run from the CX II's
 * on-chip SRAM (Advanced > On-chip memory; on by default on a CX II since
 * final/speed5, never on a CX). Stage S1 of
 * the on-chip SRAM plan, and the start of S2, with
 * PocketSNES's mover (PocketSNES's frontend/platform_nspire.cpp sram_code_*).
 *
 * The hot stretch (.text.hot.gpsp: update_gba up to arm_stub.S's
 * nspire_hot_end, about 14.5 KB: the stubs, memory handlers, block lookups
 * and update_gba) misses the instruction cache at every interrupt; a line
 * from normal memory costs about 120 cycles, from the SRAM about a quarter
 * (measured for data reads only, never for code). Its 4 KB pages, then the
 * first two whole pages of the ROM and of the RAM translation cache (the
 * blocks the dynarec makes first after a start or a flush: written while
 * they run, "dynamic"), are copied into SRAM pages nothing has written since
 * power-on (the two CX II dumps: 0xA4030000-33FFF, 0xA4024000-26FFF,
 * 0xA403D000-3EFFF), and each megabyte they are in (up to 3) gets a
 * second-level table (at 0xA402B000, a page of zeros in both dumps) that
 * maps everything else to the same physical memory as before (64 KB pages;
 * 4 KB pages in the 64 KB around the moved ones) and the moved pages to their
 * SRAM copies, with the section's access and cache bits. Everything keeps its
 * address: nothing else in gpSP changes. Turned off during a game, the
 * dynamic pages go back to normal memory with what the dynarec wrote there.
 *
 * Never written: the SRAM's first 32 KB (the OS's vectors, clock code and
 * page table: both of PocketSNES's freezes were writes there), the clock
 * switch's page (0xA4035000) and the top page (0xA403F000). Every byte used
 * is saved first and put back.
 *
 * A marker (gpsp_sram.tns, next to the settings) is written and closed
 * before the first SRAM write and removed once all is back. If it is there
 * at start, the last run with it on did not end (or found its SRAM code
 * changed at the end): the setting is turned off and saved, and the menu
 * says so. A record next to the tables lets the next start undo a mapping
 * that is still there (gpSP died but the OS did not), an older build's
 * record too. CX II only: the CX's SRAM is 128 KB and its use is not known
 * (RESEARCH.txt 3). */

#include "common.h"
#include "nspire.h"
#include "nspire_paths.h"
#include "nspire_sram.h"
#include "nspire_frontend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned nspire_sram_code = 1;   /* on by default (CX II only: nspire_sram_start) */

/* fast17: not a .tns, so the OS's file list never shows it and its removal
 * never makes that list stale (a marker left by a run that didn't end and
 * removed at the next start made the exit read the OS's list again: 6.6 s
 * on the CX II). Builds before fast17 named it gpsp_sram.tns: still found. */
u32 nspire_rtc_seconds(void);   /* nspire.c */
#define SRAM_MARKER "gpsp_sram.run"
#define SRAM_MARKER_OLD "gpsp_sram.tns"

static char sram_state[160] = "sram: off";
static int sram_crashed;   /* the marker was there at start */
static int sram_on;        /* the code runs from the SRAM now */

int nspire_sram_active(void)
{
  return sram_on;
}

void nspire_sram_report(char *text, size_t size)
{
  snprintf(text, size, "%s", sram_state);
}

static void marker_path(char *path, size_t size)
{
  nspire_save_file(path, size, SRAM_MARKER);
}

static void marker_remove(void)
{
  char path[600];
  marker_path(path, sizeof(path));
  remove(path);
}

static int file_found(const char *path)
{
  FILE *f = fopen(path, "r");
  if (!f)
    return 0;
  fclose(f);
  return 1;
}

/* fast17: the marker says when its run started ("started at clock N
 * s", the real-time clock); the exit marker (gpsp_link_run.txt.tns) says
 * "clean at clock M s" for the last run that ended, whichever build ran it.
 * M >= N: a later run ended cleanly (an older build that doesn't know this
 * marker ran between): stale, not a crash. A marker without its clock (or
 * no clean exit after it) is a crash, as before. */
static unsigned long text_clock(const char *path, const char *key, size_t max)
{
  char text[400];
  const char *at;
  FILE *f = fopen(path, "rb");
  size_t n;

  if (!f)
    return 0;
  n = fread(text, 1, max < sizeof(text) - 1 ? max : sizeof(text) - 1, f);
  fclose(f);
  text[n] = 0;
  at = strstr(text, key);
  return at ? strtoul(at + strlen(key), NULL, 10) : 0;
}

static int marker_stale(const char *path)
{
  char exit_path[600];
  unsigned long started = text_clock(path, "started at clock ", 400), clean;

  if (!started)
    return 0;
  nspire_save_file(exit_path, sizeof(exit_path), "gpsp_link_run.txt.tns");
  clean = text_clock(exit_path, "clean at clock ", 40);
  return clean && clean >= started;
}

/* The marker, or an older build's (removed here: it was a .tns; looked for
 * only when the saves folder had it at start: no file call more) */
static int marker_found(void)
{
  char path[600];

  nspire_save_file(path, sizeof(path), SRAM_MARKER_OLD);
  if (nspire_save_folder_had(SRAM_MARKER_OLD) && file_found(path))
  {
    remove(path);
    return 1;
  }
  marker_path(path, sizeof(path));
  if (!file_found(path))
    return 0;
  if (marker_stale(path))
  {
    remove(path);   /* (not a .tns: nothing for the OS's list) */
    return 0;
  }
  return 1;
}

#if defined(_TINSPIRE)

static int marker_write(void)
{
  char path[600];
  FILE *f;
  int ok;

  marker_path(path, sizeof(path));
  f = fopen(path, "w");
  if (!f)
    return 0;
  ok = fprintf(f, "gpSP runs its hot code from the on-chip SRAM. If this file is here when gpSP starts, "
               "that run didn't end: On-chip memory is turned off.\nstarted at clock %lu s\n",
               (unsigned long)nspire_rtc_seconds()) >= 0;
  if (fclose(f) != 0)
    ok = 0;
  return ok;
}

#define SRAM_START   0xA4000000u
#define SRAM_TTB     0xA4004000u   /* the OS's first-level table (CX II, OS 6.4) */
#define SRAM_TABLES  0xA402B000u   /* coarse tables, 1 KB each: up to SRAM_SECTIONS */
#define SRAM_SECTIONS 3
#define SRAM_RECORD  (SRAM_TABLES + 0xC00)
#define SRAM_MAGIC   0x32535047u   /* "GPS2" */
#define SRAM_PAGES   9
#define SRAM_DYN_PAGES 2           /* of each translation cache: its first 8 KB */

/* The pages nothing had written in either CX II dump, first choice first
 * (RESEARCH.txt 2.3, 7.1: "whole pages nothing has written: 24 25 26 2B 30
 * 31 32 33 3D 3E"; 2B holds the tables). Not 0xA4035000 (the clock
 * switch's), not 0xA403F000 (the top could be a stack's: a guess). */
static const u32 sram_page[SRAM_PAGES] =
{
  0xA4030000u, 0xA4031000u, 0xA4032000u, 0xA4033000u,
  0xA4024000u, 0xA4025000u, 0xA4026000u, 0xA403D000u, 0xA403E000u
};

/* What a run put where (at SRAM_RECORD), for the next start if this one
 * dies with the OS alive. dynamic: pages written while they run (the
 * translation caches': the dynarec writes its blocks there), so their SRAM
 * copy changes and goes back to normal memory when it is turned off. */
struct sram_record
{
  u32 magic;
  u32 sections;                /* megabytes with a coarse table: 1 to SRAM_SECTIONS */
  u32 mb[SRAM_SECTIONS];       /* their first-level entries */
  u32 original[SRAM_SECTIONS]; /* the section descriptors they had */
  u32 pages, dynamic;          /* the moved pages; a bit each: written while it runs */
  u32 va[SRAM_PAGES];          /* each one's address */
  u32 sram[SRAM_PAGES];        /* each one's SRAM copy */
  u32 sum[SRAM_PAGES];         /* each page's sum when copied */
};
#define RECORD ((volatile struct sram_record *)SRAM_RECORD)

/* The record of builds before GPS2 (2 tables at +0 and +0x400, the record
 * at +0x800, 7 pages in a row from va): undone the same way at start. */
#define SRAM_MAGIC1  0x31535047u   /* "GPS1" */
struct sram_record1
{
  u32 magic, sections, mb[2], original[2], va, pages, sram[7], sum[7];
};
#define RECORD1 ((volatile struct sram_record1 *)(SRAM_TABLES + 0x800))

extern const u8 nspire_hot_end[];

static u8 *saved;         /* the SRAM's bytes: the table page, then each moved page */
static int exit_set;

static u32 *first_level(void)
{
  u32 ttb;
  __asm__ volatile ("mrc p15, 0, %0, c2, c0, 0" : "=r" (ttb));
  return (u32 *)(ttb & 0xFFFFC000u);
}

static u32 irq_off(void)
{
  u32 old, off;
  __asm__ volatile ("mrs %0, cpsr\n\torr %1, %0, #0xC0\n\tmsr cpsr_c, %1"
                    : "=r" (old), "=r" (off) : : "memory");
  return old;
}

static void irq_restore(u32 old)
{
  __asm__ volatile ("msr cpsr_c, %0" : : "r" (old) : "memory");
}

/* The whole data cache written back and dropped, the write buffer drained. */
static void dcache_clean_flush(void)
{
  __asm__ volatile ("1: mrc p15, 0, r15, c7, c14, 3\n\t"
                    "bne 1b\n\t"
                    "mcr p15, 0, %0, c7, c10, 4" : : "r" (0) : "cc", "memory");
}

/* After a table changed: the TLBs and the instruction cache start over. */
static void mmu_changed(void)
{
  __asm__ volatile ("mcr p15, 0, %0, c7, c10, 4\n\t"   /* drain the write buffer */
                    "mcr p15, 0, %0, c8, c7, 0\n\t"    /* invalidate both TLBs */
                    "mcr p15, 0, %0, c7, c5, 0"        /* invalidate the instruction cache */
                    : : "r" (0) : "memory");
}

static u32 page_sum(const volatile u32 *p)
{
  u32 sum = 0, i;
  for (i = 0; i < 1024; i++)
    sum = (sum << 5 | sum >> 27) ^ p[i];
  return sum;
}

/* A second-level entry: a 4 KB page (type 2) or one of the 16 of a 64 KB
 * page (type 1), with the section's access (all four subpages) and C/B. */
static u32 page_entry(u32 pa, u32 section, int small)
{
  u32 ap = (section >> 10) & 3;
  return pa | (ap * 0x55u) << 4 | (section & 0xC) | (small ? 2u : 1u);
}

/* The physical address the MMU gives va, or ~0 if it is not a section, a
 * 64 KB or a 4 KB page (tables are read at VA = PA, as Ndless does). */
static u32 lookup(u32 va)
{
  u32 e = first_level()[va >> 20];
  if ((e & 3) == 2)
    return (e & 0xFFF00000u) | (va & 0xFFFFFu);
  if ((e & 3) == 1)
  {
    u32 s = ((const volatile u32 *)(e & 0xFFFFFC00u))[(va >> 12) & 0xFF];
    if ((s & 3) == 1)
      return (s & 0xFFFF0000u) | (va & 0xFFFFu);
    if ((s & 3) == 2)
      return (s & 0xFFFFF000u) | (va & 0xFFFu);
  }
  return ~0u;
}

static int tables_here(u32 sections, const volatile u32 *mb)
{
  u32 *table = first_level();
  u32 i;
  for (i = 0; i < sections; i++)
    if (mb[i] >= 4096 || (table[mb[i]] & 3) != 1 || (table[mb[i]] & 0xFFFFFC00u) != SRAM_TABLES + i * 0x400)
      return 0;
  return 1;
}

static int mapped_here(void)
{
  volatile struct sram_record *r = RECORD;
  return r->magic == SRAM_MAGIC && r->sections >= 1 && r->sections <= SRAM_SECTIONS &&
         r->pages <= SRAM_PAGES && tables_here(r->sections, r->mb);
}

/* A GPS1 build's mapping (gpSP died, the OS lived, then this build started):
 * its descriptors back and its pages copied back, as that build would have. */
static int undo_gps1(void)
{
  volatile struct sram_record1 *r = RECORD1;
  u32 *table = first_level();
  u32 cpsr, i, n;

  if (r->magic != SRAM_MAGIC1 || r->sections < 1 || r->sections > 2 || r->pages > 7 ||
      !tables_here(r->sections, r->mb))
    return 0;
  cpsr = irq_off();
  dcache_clean_flush();
  for (i = 0; i < r->sections; i++)
    table[r->mb[i]] = r->original[i];
  mmu_changed();
  for (n = 0; n < r->pages; n++)
    memcpy((void *)(r->va + n * 4096), (const void *)r->sram[n], 4096);
  r->magic = 0;
  dcache_clean_flush();
  mmu_changed();
  irq_restore(cpsr);
  return 1;
}

/* The section descriptors back. With copy_back, the moved pages' contents
 * (whatever is there now) go back to normal memory: all of them after a run
 * that didn't end (a new program may live there and have been written into
 * the SRAM), the dynamic ones when it is turned off during a game (the
 * translation caches go on with what the SRAM holds). */
static void undo(int copy_back, u32 which)
{
  volatile struct sram_record *r = RECORD;
  u32 *table = first_level();
  u32 cpsr = irq_off(), i, n;

  dcache_clean_flush();
  for (i = 0; i < r->sections && i < SRAM_SECTIONS; i++)
    table[r->mb[i]] = r->original[i];
  mmu_changed();
  if (copy_back)
  {
    for (n = 0; n < r->pages && n < SRAM_PAGES; n++)
      if (which >> n & 1)
        memcpy((void *)r->va[n], (const void *)r->sram[n], 4096);
    dcache_clean_flush();
    mmu_changed();
  }
  r->magic = 0;
  dcache_clean_flush();
  irq_restore(cpsr);
}

/* The SRAM's saved bytes back (the table page and the moved pages). */
static void put_back(u32 pages)
{
  u32 cpsr = irq_off(), n;

  if (pages > SRAM_PAGES)
    pages = SRAM_PAGES;
  dcache_clean_flush();
  memcpy((void *)SRAM_TABLES, saved, 4096);
  for (n = 0; n < pages; n++)
    memcpy((void *)sram_page[n], saved + (n + 1) * 4096, 4096);
  dcache_clean_flush();
  irq_restore(cpsr);
  free(saved);
  saved = NULL;
}

/* exit() by any other way than quit(): the mapping and the bytes back, no
 * file calls (the marker stays: the next start turns the setting off). */
static void at_exit(void)
{
  if (!sram_on)
    return;
  undo(0, 0);
  put_back(RECORD->pages);
  sram_on = 0;
}

/* The distinct megabytes of va[0..pages-1] into mb (at most max of them);
 * returns how many, or max + 1 if there are more. */
static u32 megabytes(const u32 *va, u32 pages, u32 *mb, u32 max)
{
  u32 count = 0, n, i;
  for (n = 0; n < pages; n++)
  {
    for (i = 0; i < count && mb[i] != va[n] >> 20; i++)
      ;
    if (i < count)
      continue;
    if (count == max)
      return max + 1;
    mb[count++] = va[n] >> 20;
  }
  return count;
}

/* What goes to the SRAM, at most SRAM_PAGES pages: the hot stretch
 * (update_gba up to arm_stub.S's nspire_hot_end: 4 or 5 pages, as the
 * program lands), then the first SRAM_DYN_PAGES whole pages of the ROM and
 * of the RAM translation cache, where the dynarec puts the blocks it makes
 * first after a start or a flush. At most SRAM_SECTIONS megabytes get a
 * table: a cache's pages are left out (the RAM's, then the ROM's) when its
 * megabyte would need one more. Returns the pages (0: the hot stretch
 * doesn't fit), their addresses in va[], the dynamic ones in *dyn. */
static u32 sram_pick(u32 *va, u32 *dyn, u32 *mb, u32 *sections)
{
  u32 start = (u32)(uintptr_t)update_gba & ~0xFFFu;
  u32 end = ((u32)(uintptr_t)nspire_hot_end + 0xFFF) & ~0xFFFu;
  u32 caches[2], c, k, pages = 0;

  *dyn = 0;
  if (end <= start || (end - start) / 4096 > SRAM_PAGES)
    return 0;
  for (; start < end; start += 4096)
    va[pages++] = start;
  if (megabytes(va, pages, mb, SRAM_SECTIONS) > SRAM_SECTIONS)
    return 0;
  /* Whole pages of the caches only: the program's base is not on a page,
   * and the bytes before the ROM cache are newlib's (stdio, atexit, errno),
   * which exit() reads after at_exit has put the pages back. */
  caches[0] = ((u32)(uintptr_t)rom_translation_cache + 0xFFF) & ~0xFFFu;
  caches[1] = ((u32)(uintptr_t)ram_translation_cache + 0xFFF) & ~0xFFFu;
  for (c = 0; c < 2; c++)
  {
    u32 first = pages, bits = *dyn;
    for (k = 0; k < SRAM_DYN_PAGES && pages < SRAM_PAGES; k++)
    {
      va[pages] = caches[c] + k * 4096;
      bits |= 1u << pages++;
    }
    if (megabytes(va, pages, mb, SRAM_SECTIONS) > SRAM_SECTIONS)
      pages = first;   /* no table left for its megabyte: not this cache */
    else
      *dyn = bits;
  }
  *sections = megabytes(va, pages, mb, SRAM_SECTIONS);
  return pages;
}

static void sram_enable(void)
{
  u32 *table = first_level();
  u32 ttb = (u32)table, se = table[SRAM_START >> 20];
  u32 va[SRAM_PAGES], dyn, mb[SRAM_SECTIONS], sections, pages, i, n, cpsr;
  u32 sum[SRAM_PAGES];
  volatile struct sram_record *r = RECORD;

  if (sram_on)
    return;
  if (ttb != SRAM_TTB)
  {
    snprintf(sram_state, sizeof(sram_state), "sram: off: the page table is at %08lx, not %08lx",
             (unsigned long)ttb, (unsigned long)SRAM_TTB);
    return;
  }
  if ((se & 3) != 2 || (se & 0xFFF00000u) != SRAM_START || !((se >> 10) & 3))
  {
    snprintf(sram_state, sizeof(sram_state), "sram: off: the SRAM is mapped %08lx", (unsigned long)se);
    return;
  }
  pages = sram_pick(va, &dyn, mb, &sections);
  if (!pages)
  {
    snprintf(sram_state, sizeof(sram_state), "sram: off: the hot code is %08lx-%08lx",
             (unsigned long)(uintptr_t)update_gba, (unsigned long)(uintptr_t)nspire_hot_end);
    return;
  }
  for (i = 0; i < sections; i++)
    if ((table[mb[i]] & 3) != 2 || (table[mb[i]] & 0xFFF00000u) != mb[i] << 20)
    {
      snprintf(sram_state, sizeof(sram_state), "sram: off: the megabyte %03lx is mapped %08lx",
               (unsigned long)mb[i], (unsigned long)table[mb[i]]);
      return;
    }
  /* No table of the OS's in the pages used. */
  for (i = 0; i < 4096; i++)
  {
    u32 e = table[i], base;
    if ((e & 3) != 1 && (e & 3) != 3)
      continue;
    base = e & ((e & 3) == 1 ? 0xFFFFFC00u : 0xFFFFF000u);
    if ((base >= 0xA4024000u && base < 0xA4034000u) || (base >= 0xA403D000u && base < 0xA403F000u))
    {
      snprintf(sram_state, sizeof(sram_state), "sram: off: entry %03lx has a table at %08lx",
               (unsigned long)i, (unsigned long)base);
      return;
    }
  }
  saved = malloc((pages + 1) * 4096);
  if (!saved)
  {
    snprintf(sram_state, sizeof(sram_state), "sram: off: no %lu KB free", (unsigned long)(pages + 1) * 4);
    return;
  }
  if (!marker_write())
  {
    free(saved);
    saved = NULL;
    marker_remove();
    snprintf(sram_state, sizeof(sram_state), "sram: off: the marker %s couldn't be written", SRAM_MARKER);
    return;
  }
  if (!exit_set)
    exit_set = atexit(at_exit) == 0;

  cpsr = irq_off();
  dcache_clean_flush();
  memcpy(saved, (const void *)SRAM_TABLES, 4096);
  for (n = 0; n < pages; n++)
  {
    memcpy(saved + (n + 1) * 4096, (const void *)sram_page[n], 4096);
    sum[n] = page_sum((const volatile u32 *)va[n]);
    memcpy((void *)sram_page[n], (const void *)va[n], 4096);
  }
  r->magic = 0;
  RECORD1->magic = 0;
  for (i = 0; i < sections; i++)
  {
    u32 section = table[mb[i]];
    u32 base = section & 0xFFF00000u;
    volatile u32 *pt = (volatile u32 *)(SRAM_TABLES + i * 0x400);
    u32 chunk, k;
    for (chunk = 0; chunk < 16; chunk++)
    {
      u32 chunk_va = (mb[i] << 20) + chunk * 0x10000;
      int small = 0;
      for (n = 0; n < pages; n++)
        small |= (va[n] & ~0xFFFFu) == chunk_va;
      for (k = 0; k < 16; k++)
      {
        u32 page_va = chunk_va + k * 4096;
        u32 pa = small ? base + (page_va & 0xFF000u) : base + (chunk_va & 0xF0000u);
        for (n = 0; n < pages; n++)
          if (va[n] == page_va)
            pa = sram_page[n];
        pt[chunk * 16 + k] = page_entry(pa, section, small);
      }
    }
    r->mb[i] = mb[i];
    r->original[i] = section;
  }
  for (n = 0; n < pages; n++)
  {
    r->va[n] = va[n];
    r->sram[n] = sram_page[n];
    r->sum[n] = sum[n];
  }
  r->sections = sections;
  r->pages = pages;
  r->dynamic = dyn;
  r->magic = SRAM_MAGIC;
  dcache_clean_flush();
  /* The domain from the section (bits 8-5); bit 4 set, as the ARM926 wants. */
  for (i = 0; i < sections; i++)
    table[mb[i]] = (SRAM_TABLES + i * 0x400) | (r->original[i] & 0x1E0u) | 0x10 | 1;
  mmu_changed();
  irq_restore(cpsr);

  /* Through the moved pages' own addresses now: the SRAM, with their bytes
   * (the caches' pages too: nothing ran since the copy). */
  for (n = 0; n < pages; n++)
    if (lookup(va[n]) != sram_page[n] || page_sum((const volatile u32 *)va[n]) != sum[n])
      break;
  if (n < pages)
  {
    u32 pa = lookup(va[n]);
    undo(0, 0);
    put_back(pages);
    marker_remove();
    snprintf(sram_state, sizeof(sram_state), "sram: off: the mapping didn't take (page %lu at %08lx: %08lx)",
             (unsigned long)n, (unsigned long)va[n], (unsigned long)pa);
    return;
  }
  sram_on = 1;
  snprintf(sram_state, sizeof(sram_state),
           "sram: on: %lu KB (code %08lx, %lu KB of the translation caches), %lu MB with a table at %08lx",
           (unsigned long)pages * 4, (unsigned long)va[0], (unsigned long)__builtin_popcount(dyn) * 4,
           (unsigned long)sections, (unsigned long)SRAM_TABLES);
}

void nspire_sram_stop(void)
{
  volatile struct sram_record *r = RECORD;
  u32 pages, dyn, n, changed = 0, code = 0;
  u32 va[SRAM_PAGES];

  if (!sram_on)
    return;
  pages = r->pages;
  dyn = r->dynamic;
  for (n = 0; n < pages; n++)
    va[n] = r->va[n];
  /* The caches' pages go back with what the dynarec wrote there. */
  undo(1, dyn);
  /* Normal memory has the code again: the SRAM copy should hold the same
   * bytes still (nothing writes the code); anything else wrote there. */
  for (n = 0; n < pages; n++)
    if (!(dyn >> n & 1))
    {
      code++;
      if (page_sum((const volatile u32 *)sram_page[n]) != page_sum((const volatile u32 *)va[n]))
        changed++;
    }
  put_back(pages);
  sram_on = 0;
  if (changed)
    /* The marker stays: the next start turns the setting off. */
    snprintf(sram_state, sizeof(sram_state), "sram: was on; %lu of %lu SRAM code pages CHANGED by someone else: off at the next start",
             (unsigned long)changed, (unsigned long)code);
  else
  {
    marker_remove();
    snprintf(sram_state, sizeof(sram_state), "sram: was on (%lu KB from %08lx), put back, the SRAM's code unchanged",
             (unsigned long)pages * 4, (unsigned long)va[0]);
  }
}

int nspire_sram_start(void)
{
  if (!is_cx2)
  {
    nspire_sram_code = 0;
    snprintf(sram_state, sizeof(sram_state), "sram: off (CX II only)");
    return 0;
  }
  /* gpSP died and the OS lived on: its mapping is still there. */
  if (mapped_here())
    undo(1, ~0u);
  else
    undo_gps1();
  if (marker_found())
  {
    marker_remove();
    sram_crashed = 1;
    if (nspire_sram_code)
    {
      nspire_sram_code = 0;
      snprintf(sram_state, sizeof(sram_state), "sram: off: the last run with it didn't end (%s was there)", SRAM_MARKER);
      return 1;
    }
  }
  if (nspire_sram_code)
    sram_enable();
  return 0;
}

#else /* the PC's builds: nothing to map */

static void sram_enable(void)
{
  snprintf(sram_state, sizeof(sram_state), "sram: off (not a calculator)");
}

void nspire_sram_stop(void)
{
  sram_on = 0;
}

int nspire_sram_start(void)
{
  if (!is_cx2)
  {
    nspire_sram_code = 0;
    snprintf(sram_state, sizeof(sram_state), "sram: off (CX II only)");
    return 0;
  }
  if (marker_found())
  {
    marker_remove();
    sram_crashed = 1;
    if (nspire_sram_code)
    {
      nspire_sram_code = 0;
      return 1;
    }
  }
  if (nspire_sram_code)
    sram_enable();
  return 0;
}

#endif

void nspire_sram_changed(void)
{
  nspire_sram_code %= 2;
  if (nspire_sram_code && !sram_on)
  {
#if defined(_TINSPIRE)
    if (!is_cx2)
    {
      nspire_sram_code = 0;
      return;
    }
#endif
    sram_crashed = 0;
    sram_enable();
    if (!sram_on)
    {
      nspire_sram_code = 0;
      nspire_osd_message("Fast memory: %s", sram_state + 6);
    }
    else
      nspire_osd_message("Fast memory on");
  }
  else if (!nspire_sram_code && sram_on)
  {
    nspire_sram_stop();
    nspire_osd_message("Fast memory off");
  }
}

int nspire_sram_crashed(void)
{
  return sram_crashed;
}
