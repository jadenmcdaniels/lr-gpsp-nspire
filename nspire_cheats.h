/* r19: a game's cheat codes (nspire_cheats.c): read from its cheat file, on the calculator or a USB drive, each
 * code a line with a switch on Settings > Cheats. */
#ifndef NSPIRE_CHEATS_H
#define NSPIRE_CHEATS_H

/* A game started / is being left (nspire_config.c's open and close of a game). */
void nspire_cheats_open(const char *rom_path);
void nspire_cheats_close(void);
/* Looks again for the game's file when none is loaded and something changed (a drive came in): the menu. */
void nspire_cheats_look(void);
/* The codes the engine can run (the file's others are counted in skipped). */
int nspire_cheats_count(void);
int nspire_cheats_skipped(void);
int nspire_cheats_on_count(void);
/* Settings > Cheats. */
void nspire_cheats_page(void);

/* For the PC's tests: what the file's text gives (count; *skipped the ones the engine can't run). */
int nspire_cheats_parse_text(char *text, unsigned long size, int *skipped);
const char *nspire_cheats_name(int i);
int nspire_cheats_is_on(int i);
/* Turns code i on or off: 1 done, 0 not (too many on: NSPIRE_CHEATS_ON_MAX). */
int nspire_cheats_set(int i, int on);
#define NSPIRE_CHEATS_ON_MAX 20   /* cheats.h MAX_CHEATS: the engine's slots */

#endif
