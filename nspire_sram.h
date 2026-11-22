#ifndef NSPIRE_SRAM_H
#define NSPIRE_SRAM_H

/* gpSP's hot code and newest translated blocks run from the CX II's
 * on-chip SRAM (nspire_sram.c). */

#include <stddef.h>

/* The setting (Advanced > On-chip memory): 0 off, 1 on (the default; 0 on
 * a CX). Saved in the settings file (nspire_config.c word 69). */
extern unsigned nspire_sram_code;

/* At start, after the settings are read: a run with it on that didn't end
 * turns it off (returns 1: the settings should be saved), else it is put on
 * if the setting says so. */
int nspire_sram_start(void);
/* The menu's hook: on or off now, as the setting says. */
void nspire_sram_changed(void);
/* At quit, with the files still open to us: the code back in normal
 * memory, the SRAM's bytes back, the marker gone. Safe to call twice. */
void nspire_sram_stop(void);
/* One line for gpsp_perf.txt.tns ("sram: ..."). */
void nspire_sram_report(char *text, size_t size);
/* 1 while the code runs from the SRAM. */
int nspire_sram_active(void);
/* 1 if a run with it on didn't end (the menu says so until it is set). */
int nspire_sram_crashed(void);

#endif
