/* Link speed (nspire_link.h): Auto tries the fast transport
 * (nspire_link_fast.c), Compatible links over NavNet only; and the fast
 * transport turned off by itself after a run that died while it was in
 * use, until the setting is put to Auto again.
 *
 * The marker (gpsp_fast.txt.tns in the saves folder, a line): "in use at
 * clock N" written just before a fast bring-up starts and removed when the
 * link ends or gpSP quits; a run that finds it there died with the fast
 * transport coming up or in use, and writes "off after a crash ..." in its
 * place, which keeps it off in the runs after (nspire_link_fast_allowed)
 * until the setting is changed to Auto (nspire_link_speed_changed). A
 * link that ended cleanly but left the USB controller not as its OS had it
 * (link_cable_fast_damage) writes "off: the controller was not given back
 * as found ..." the same way. The writes come at a link's start and end,
 * never in a linked frame. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "nspire_link.h"
#include "nspire_frontend.h"
#include "nspire_paths.h"


uint32_t nspire_link_speed = LINK_SPEED_AUTO;

/* fast17: not a .tns (the OS's file list never shows it, so its removal
 * never makes the exit read that list again: 6.6 s on the CX II). r16 and
 * older named it gpsp_fast.txt.tns: renamed at the first read. */
#define MARKER "gpsp_fast.run"
#define MARKER_OLD "gpsp_fast.txt.tns"

static int crash_off = -1;   /* -1: not read yet */
static int marker_on;
static char crash_text[200];

static void marker_path(char *path, unsigned size)
{
  nspire_save_file(path, size, MARKER);
}

/* 1 if the marker now says text (F3: a bring-up without its marker would
 * have no crash protection, so it isn't made). */
static int marker_put(const char *text)
{
  char path[600];
  FILE *f;
  size_t n = strlen(text);
  int ok;
  marker_path(path, sizeof(path));
  f = fopen(path, "w");
  if (!f)
    return 0;
  ok = fwrite(text, 1, n, f) == n;
  ok = fclose(f) == 0 && ok;
  return ok;
}

static void marker_remove(void)
{
  char path[600];
  marker_path(path, sizeof(path));
  remove(path);
}

/* The marker as the last run left it: once a run. */
static void read_marker(void)
{
  char path[600], text[200];
  FILE *f;
  size_t n;

  crash_off = 0;
  marker_path(path, sizeof(path));
  static int old_looked;
  if (!old_looked && (old_looked = 1, nspire_save_folder_had(MARKER_OLD)))
  {
    /* an older build's: takes the new name (or goes, if both are there):
     * the OS's list is read again at this exit, once */
    char old[600];
    nspire_save_file(old, sizeof(old), MARKER_OLD);
    if (rename(old, path) != 0)
      remove(old);
  }
  f = fopen(path, "rb");
  if (!f)
    return;
  n = fread(text, 1, sizeof(text) - 1, f);
  fclose(f);
  text[n] = 0;
  while (n && (text[n - 1] == '\n' || text[n - 1] == '\r'))
    text[--n] = 0;
  if (!strncmp(text, "in use", 6))
  {
    /* The last run died with it coming up or in use. */
    char line[300];
    crash_off = 1;
    snprintf(crash_text, sizeof(crash_text), "%s", text);
    snprintf(line, sizeof(line), "off after a crash (the last run's marker: %s); Link speed: Auto turns it on\n",
             crash_text);
    marker_put(line);
    nspire_link_log("fast transport: turned off: the last run died with it %s; links go over NavNet until "
                    "Link speed is set to Auto again", crash_text);
    nspire_osd_message("Fast link turned off after a crash");
  }
  else if (!strncmp(text, "off after a crash", 17) || !strncmp(text, "off: ", 5))
  {
    crash_off = 1;
    snprintf(crash_text, sizeof(crash_text), "%s", text);
  }
}

void nspire_link_speed_reread(void)
{
  crash_off = -1;
  marker_on = 0;
}

void nspire_link_speed_check(void)
{
  if (crash_off < 0)
    read_marker();
}

int nspire_link_fast_crashed(void)
{
  nspire_link_speed_check();
  return crash_off > 0;
}

const char *nspire_link_fast_off_why(void)
{
  nspire_link_speed_check();
  if (nspire_link_speed % LINK_SPEEDS == LINK_SPEED_COMPATIBLE)
    return "Link speed is Compatible (NavNet only)";
  if (crash_off > 0)
    return "turned off after a crash (Link speed: Auto turns it on)";
  return NULL;
}

int nspire_link_fast_allowed(void)
{
  return !nspire_link_fast_off_why();
}

int nspire_link_fast_begin(void)
{
  char text[80];
  nspire_link_speed_check();
  if (crash_off > 0)
    return 0;   /* never tried then; the "off" marker stays */
  snprintf(text, sizeof(text), "in use at clock %u s\n", (unsigned)nspire_rtc_seconds());
  if (!marker_put(text))
  {
    marker_remove();   /* a part written: not taken for a crash */
    return 0;
  }
  marker_on = 1;
  return 1;
}

void nspire_link_fast_end(void)
{
  const char *damage;
  if (!marker_on)
    return;
  marker_on = 0;
  if (crash_off > 0)
    return;
  /* A clean end that left the controller not as the OS had it (F3): off,
   * as after a crash, for this run and the ones after. */
  damage = link_cable_fast_damage();
  if (damage)
  {
    char line[300];
    crash_off = 1;
    snprintf(crash_text, sizeof(crash_text), "off: the controller was not given back as found: %s", damage);
    snprintf(line, sizeof(line), "%s; Link speed: Auto turns it on\n", crash_text);
    if (!marker_put(line))
      nspire_link_log("fast transport: the marker couldn't be written: off for this run only");
    nspire_link_log("fast transport: turned off: the controller was not given back as found (%s); links go over "
                    "NavNet until Link speed is set to Auto again", damage);
    nspire_osd_message("Fast link turned off: USB not given back");
    return;
  }
  marker_remove();
}

void nspire_link_speed_changed(void)
{
  nspire_link_speed %= LINK_SPEEDS;
  nspire_link_speed_check();
  if (nspire_link_speed == LINK_SPEED_AUTO && crash_off > 0)
  {
    crash_off = 0;
    marker_remove();
    nspire_link_log("fast transport: on again (Link speed set to Auto)");
    nspire_osd_message("Fast link on again");
  }
}
