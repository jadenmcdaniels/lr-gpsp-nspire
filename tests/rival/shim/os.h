/* Stand-in for Ndless's os.h, to build the rival gpSP's core (Vogtinator's
 * gpSP 0.9 Nspire port) under qemu-arm for the speed model (harness.c). */
#ifndef RIVAL_SHIM_OS_H
#define RIVAL_SHIM_OS_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#define is_classic 0
#define is_cx 1
#define BOOL int
#define TRUE 1
#define FALSE 0
void clean_dcache(void);
int rival_lcd_vblank(void);
unsigned long save_backup(char *name);   /* harness.c */
#define NU_Current_Dir(drive, path) (strcpy((path), "."), (char *)(path))
#define msleep(ms) ((void)(ms))
struct tm *localtime(const time_t *);
void enableAlignmentExceptions(void);
#endif
