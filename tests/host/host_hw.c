/* A simulated TI-Nspire CX II for gpSP's frontend on a PC (gpsp-host):
 * the LCD controller, the 32768 Hz timer, the keypad and touchpad and the
 * backlight, driven by a script of key presses in virtual time.
 *
 *   gpsp-host [--documents DIR] [--model cx2|cx] [--rom-memory MB] [--free-storage KB]
 *             [--heap-limit KB] [--max-files N]
 *             [link options] SCRIPT OUTDIR GAMEDIR [ROM]
 *
 * gpSP runs as it does on the calculator (its main(), menus, file list and
 * game loop), as GAMEDIR/_gpSP.tns, with GAMEDIR's parent folder as the
 * calculator's documents (its ndless/ndless.cfg.tns has the file types
 * Ndless opens with each program), or DIR (GAMEDIR itself for gpSP right
 * in My Documents). Virtual time moves a little at every
 * read of the timer, the keypad or the LCD, so gpSP's waits pass time and
 * its frames come at its own pace. Work that takes time on the calculator
 * but none here (running a frame of the game, drawing a line, turning a
 * picture, reading the touchpad) costs what the script's "cost" lines say.
 * A frame's emulation costs its share as its cycles run while linked (the
 * other GBA waits on them), and all at its end otherwise.
 *
 * The LCD refreshes 60 times a second. Like the PL111, it takes a new buffer
 * address only at the start of a refresh, reads that buffer's 320 lines,
 * then waits out the porches (127 of the 447 lines' time, the CX II OS's
 * timings) before the next refresh. Writing to the buffer while its lines
 * are being read is what makes tearing and flicker, so a buffer that changed
 * between the start and the end of its lines is counted ("writes_to_shown"
 * in OUTDIR/report.txt). The picture it shows is kept, turned to landscape,
 * for the script's screenshots.
 *
 * Script lines (times in seconds):
 *   wait 1.5         let 1.5 s pass
 *   frames 30        let 30 of the game's frames run; the times of the lines
 *                    after it count from then. Holds, releases and frames
 *                    make a script that's the same whatever the game's
 *                    speed, e.g. over a slow link (tests/link)
 *   press KEY        down for 0.1 s, then up; 0.25 s pass
 *   tap KEY          down for 0.03 s, then up; 0.1 s pass
 *   hold KEY / release KEY
 *   shot NAME        OUTDIR/NAME.ppm: what the LCD shows now (320x240)
 *   note NAME        a line in the report (game, frame, backlight; see below)
 *   cost trace FILE  from now on each frame costs its own line of FILE (the
 *                    speed model's, tests/calc_speed.py --trace: game,
 *                    drawing and picture ms; from the first line again at
 *                    its end) instead of the frame, line and turn costs;
 *                    the picture's time goes by in nspire_present_frame
 *   cost KIND N      from now on: frame (ms of a game frame's emulation),
 *                    line (us to draw a line), turn (ms to turn a whole
 *                    320x240 picture; parts cost their share), touchpad (ms),
 *                    read (ms to read 1 MB of a ROM file, or unpack it),
 *                    send, receive (ms for each message the link sends or
 *                    receives)
 *   measure          the report's link lines count from here (else from
 *                    linking); "measure stop" stops them
 *   hog KB           from now on KB of the heap is taken by someone else (the
 *                    OS, open documents: save17), within --heap-limit; hog 0
 *                    gives it back. It gets what is free and no more (the
 *                    report's hog_short: what it didn't get). The report's
 *                    heap_peak and heap_failed count from the last hog line
 *   heapdump NAME    OUTDIR/heap_NAME.txt: gpSP's heap now by the code that
 *                    took it (offsets from host_heap_dump: tests/host/heap_by.py)
 *   frag KB          from now on no allocation over KB succeeds: the heap in
 *                    pieces, whatever is free in all (frag 0: whole again)
 *   peek NAME A:N    OUTDIR/NAME.bin: N bytes of the game's work RAM from
 *                    address A (hex: 2xxxxxx or 3xxxxxx; other addresses
 *                    read 0), and a note NAME
 *   backlight N      the backlight register's value at start (the OS's)
 *   pad ...          a USB gamepad plugs in or comes out... (host_usbpad.c)
 *   quit             write the report and end (keys held are let go first)
 *   crash            end at once (_exit): a power-off, nothing written
 * KEY is a gpSP key name (enter, esc, menu, del, shift, S, 8, +, ...) or up,
 * down, left, right, click, on; a USB gamepad's with _ for the space
 * (Pad_A, Pad_Start...), and Pad_Up, Pad_Down, Pad_Left, Pad_Right for its
 * D-pad (they read as host_usbpad.c's pad's buttons).
 *
 * Link options (host_link.c): --link SOCKET [--player 1|2] [--link-device
 * auto|wireless|cable|exact] [--link-delay MS] [--stand-in CODE:MODE]
 * [--link-trace FILE] [--link-peer legacy] [--link-nonce-tie N]: MS is how
 * long a message takes to come (one way), CODE:MODE plays a game's link
 * cable with a stand-in that isn't in nspire_link.c's table yet (e.g.
 * BPEE:poke; poke, aw1, aw2, fe or exact), FILE gets a line for each
 * transfer on the cable as wired. --link-peer legacy: the other gpsp-host
 * is e24876d's, whose socket carries bare messages (no send time, so no
 * delay either way). --link-nonce-tie N: the first N nonces are the same
 * number on both sides (give it to both), so N hellos tie. --link-auto:
 * the cable's watched for and linked on its own (the other connects once
 * it watches or links). --link-os: the calculators' NavNet timing
 * (host_link.c): what's sent goes at this side's next OS sleep or wait.
 *
 * With GPSP_HOST_CLOCK=SECONDS in its environment, the game's clock (the
 * cartridge's RTC, which reads time()) stays at that time, so runs at
 * different times save the same bytes (tests/link).
 *
 * The report's shot and note lines give the game's frame count (the 32-bit
 * number at 0x02000000, where the test ROM keeps it), how many different
 * pictures the LCD has shown, the backlight, the menu screen on show and
 * its picked line (nspire_ui.h's ui_shown), and the ROM being played. Its
 * load_error line is why the last game didn't load (nspire_rom_load_error).
 *
 * --frame-trace FILE: a line for each game frame that ends ("f TIME FRAME
 * DRAWN", TIME in ms) and each new picture the LCD takes at a refresh ("p
 * TIME REFRESH FRAME": the game's frame that picture was drawn at), for
 * the picture rhythm's tests (test_fast_forward_even).
 *
 * --rom-memory MB and --free-storage KB make a calculator short of memory
 * or storage (see __wrap_malloc); --heap-limit KB holds gpSP's whole heap
 * under that, --max-files N its open files and folders (save17);
 * --concurrent KB counts the moments someone else's KB wouldn't fit beside
 * gpSP's heap (concurrent_short); --storage-errno 0: a write past
 * --free-storage sets no errno (as Ndless's often doesn't).
 *
 * --os-cwd 1: the current folder as the calculator's OS gives it
 * (NU_Current_Dir: "\documents\gba\", backslashes and one at the end), and
 * the file calls taking such paths as the OS does (see os_path). */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "os.h"
#include "nspire_hw.h"
#include "nspire_keys.h"
#include "nspire_link.h"
void nspire_osd_message(const char *format, ...) __attribute__((format(printf, 1, 2)));

extern int ui_battery, ui_charging;   /* nspire_ui.c (not included: its headers redefine getcwd) */

#define TICK_HZ 32768.0
/* The LCD's refresh: 60 a second over 447 lines (320 shown, then porches: the CX II OS timings), as every test
 * has it; with --lcd-timed (2026-11-16), as the CX II's PL111 times it from its registers: 51.4 us a line of the
 * OS's 389 clocks (the logs: 447 lines 23.0 ms, 360 lines 18.5 ms), a line's time following its clocks, the
 * refresh its lines; Ndless's timing 1 (0x0720013F, 360 lines, 54 a second) at the start. */
static double refresh_ticks = TICK_HZ / 60.0, refresh_origin;
static int lcd_lines_total = 447, lcd_timed;
#define REFRESH_TICKS refresh_ticks
#define LCD_LINES_TOTAL lcd_lines_total
#define PORTRAIT_BYTES (240 * 320 * 2)
#define READ_STEP (1.0 / 64.0)         /* virtual time per hardware read, in ticks */
#define MS (TICK_HZ / 1000.0)
#define NO_PROGRESS_S 120               /* wall-clock seconds without a frame: hung (main) */

extern char *nspire_key_strings[];
extern unsigned char ewram[];          /* the GBA's 256 KB work RAM (the core's) */
extern unsigned char iwram[];          /* its 32 KB internal one, from byte 0x8000 */
extern uint16_t io_registers[];        /* its I/O registers (0x04000000) */
extern char gamepak_filename[];
extern const char *nspire_rom_load_error;
/* fwrite as it is, for the screenshots (gpSP's own writes are limited by
 * --free-storage: see __wrap_fwrite). */
size_t __real_fwrite(const void *data, size_t size, size_t count, FILE *f);
extern uint32_t skip_next_frame;
int gpsp_main(int argc, char *argv[]);
int host_link_setup(const char *path, int player, double delay_ms, int legacy_peer);
void host_link_nonce_tie(int hellos);
int host_link_trace(const char *path);
void host_link_frame(int drawn);
void host_link_measure(int on);
void host_link_report(FILE *f);
void host_os_keys_report(FILE *f);
void host_usbpad_script(const char *line);
void host_bt_script(const char *line);   /* host_bt.c: "bt ..." lines */
double host_hw_os_usb_ticks(void);

/* ---- The script ---- */

enum event_type { EV_DOWN, EV_UP, EV_SHOT, EV_NOTE, EV_COST, EV_LINK, EV_UNLINK, EV_FRAMES, EV_MEASURE,
                  EV_QUIT, EV_PEEK, EV_PAD, EV_BT, EV_CRASH, EV_POKE, EV_BATTERY, EV_TOAST, EV_HOG, EV_FRAG, EV_HEAPDUMP };
enum cost_kind { COST_FRAME, COST_LINE, COST_TURN, COST_TOUCHPAD, COST_READ, COST_SEND, COST_RECEIVE,
                 COSTS };

struct event
{
  int part;              /* the script's part: a "frames" line ends one */
  double time;           /* from the part's start */
  enum event_type type;
  unsigned key;          /* or the cost's kind; a peek's length */
  double value;          /* a cost, in ticks; frames to run; a peek's address */
  char name[128];
};

static struct event *events;
static int num_events, next_event;
static double part_start;       /* when the last "frames" wait ended */
static long frames_left = -1;   /* of a "frames" wait, or -1 */
/* A "linked N" wait: its frames end early once a link is up (the PC's two
 * gpsp-hosts link when the other's hello comes, in real time). */
static int frames_until_linked;
static char out_dir[4096];   /* realpath needs PATH_MAX */

/* ---- The simulated hardware ---- */

static double now;                  /* virtual time in timer ticks */
/* How fast the simulated timer really counts (--timer-rate), and the
 * real-time clock's seconds at the start. */
static double timer_rate = TICK_HZ;
static uint32_t rtc_start = 1700000000u;
static int rtc_frozen;
static int timer_dead;
static double costs[COSTS];         /* in ticks (a line's, a whole turn's...) */
/* "cost trace": each frame's game, drawing and picture time (ticks). */
static double (*trace_costs)[3];
static long trace_len, trace_next;
static int trace_on;
static FILE *frame_trace;
static unsigned lcd_base_frame, shown_frame;   /* the game's frame drawn into lcd_base, the shown one */
/* The keypad's four words, then a USB gamepad's buttons (host_usbpad.c). */
static uint32_t keys[5];
static int on_key;
static uint32_t timer_clock;
/* R11t: the first block's clock select (0x900C0080) is ONE word for its two
 * timers: bits 1:0 the first timer's (gpSP's), bits 3:2 the one at
 * 0x900C0020's, which a CX II's OS runs (control 0xE2, its interrupt on; a
 * CX's leaves it off). A field: bit 1 32 kHz (here --timer-rate), else bit
 * 0 ~10 MHz, else 33 MHz (Hackspire). The OS's value at start:
 * --timer-clock-os HEX (default 00000004: the first timer's field 0, the
 * OS's 1, so gpSP's write must change only its own and the old whole-word
 * write of 0xA fails every run). Any change to bits 31:2 is said on stderr
 * (a failure in every test), and the report has the word at the end. */
static uint32_t timer_clock_os = 0x4;
/* R12 (2026-11-15). The calculator showed R11t's write of 2
 * into bits 1:0 making the OS's 44-tick USB wait (its port reset after a
 * connect) last 44 s, 0x160000 counts at 32768 Hz. Modelled:
 *   --timer-sel1-hz HZ, --timer-sel0-hz HZ  a field's clock with bit 0 set,
 *        and with neither bit (Hackspire's ~10 MHz and 33 MHz by default)
 *   --os-usb-timer HEX  the OS's USB timer at 0x900C0020: off (control 42,
 *        as the CX II's logs before the adapter) until the adapter goes in
 *        (host_hw_adapter_in), then periodic with load HEX, its interrupt
 *        on (control e2); without it, the R11t model's (e2 from the start)
 *   --os-usb-wait block|timer1  how the OS times its USB ticks: "block": its
 *        USB timer's interrupts (taken at its sleeps: one pending one for
 *        all of gpSP's time between, each period's in the sleep), and bit 1
 *        of the select puts the WHOLE block on 32 kHz; "timer1": it counts
 *        0x8000 counts of the first timer (900C0000, read at its sleeps) a
 *        tick, whoever runs it (host_hw_os_usb_ticks; host_usbpad.c "pad
 *        usbtick" resets the port after N of them)
 *   --timer1-os off|free|periodic:HEX|periodic16:HEX|oneshot  the first
 *        timer as the OS has it at start (off: control 0), and
 *   --timer1-os-jump S, --timer1-os-stop S  the OS rewrites it (its load:
 *        the count jumps) or stops it S seconds in
 * The first timer is an SP804 (load, control: enable, periodic, prescale,
 * 32-bit) at its field's clock; the report has how the OS last set it and
 * how it is at the end (Run.finish: the same), gpSP's writes to it and to
 * the select (a CX II's must be none outside the legacy fallback). */
extern int host_is_cx2;
static double sel0_hz = 33000000.0, sel1_hz = 10000000.0;
static int block_slow, os_wait_timer1;
static uint32_t os_usb_load;              /* 0: the R11t model's OS timer */
static int os_usb_on;                     /* --os-usb-timer's timer runs */
static double os2_acc, os2_since;         /* the OS's timer at 0x900C0020: its count before os2_since */
#define OS2_LOAD 0x2710u                  /* its load (periodic) */
/* The fast timer's select (0x90010080): its other half (0x90010020) runs
 * nowhere, but is not gpSP's either. */
static uint32_t fast_clock_os;
static long clock_foreign_writes, clock_writes;
static double field_rate(uint32_t field)
{
  return (field & 2) ? timer_rate : (field & 1) ? sel1_hz : sel0_hz;
}
/* bit 1 of bits 1:0 slows the whole block (--os-usb-wait block), else each
 * timer at its own field's clock */
static double t1_rate(void)
{
  return block_slow && (timer_clock & 2) ? timer_rate : field_rate(timer_clock & 3);
}
static double os2_rate(void)
{
  return block_slow && (timer_clock & 2) ? timer_rate : field_rate(timer_clock >> 2 & 3);
}
static int os2_runs(void)
{
  return host_is_cx2 && (!os_usb_load || os_usb_on);
}
static uint32_t os2_load(void)
{
  return os_usb_load ? os_usb_load : OS2_LOAD;
}
static double os2_counts(void)
{
  return os2_runs() ? os2_acc + (now - os2_since) * os2_rate() / TICK_HZ : 0;
}
/* The first timer (SP804): its load, control and count from 'from' since 'since'. */
static struct { uint32_t load, control, from; double since; } t1;
static uint32_t t1_os_control, t1_os_load;   /* as the OS last set it */
/* QUIT6: its raw interrupt (+0x10), set each time its count reaches zero
 * whatever the control's bit 5, cleared only by a write to +0x0C; the
 * block's line 18 is it AND bit 5 (+0x14), and the OS's handler for that
 * line clears only its own timer's (0x900C0020): one left raised after the
 * hand-back is taken for ever (the calculator froze). --timer1-os reset is
 * every CX II's: control 20 (the reset value: interrupt enabled), value
 * ffffffff; --timer1-os-raised: raised at start (as the OS left it). */
static int t1_raised;
static double t1_zeros;   /* times its count reached zero, before t1.since */
static double t1_zeros_now(void);
static double t1_jump_at = -1, t1_stop_at = -1;
static long t1_writes;
static uint32_t t1_value(void);
/* An older CX's other timers, which nspire.c tries when the first doesn't
 * run: the fast timer (0x90010000) and the second dual timer's other half
 * (0x900D0020; the OS's tick is its first). Each counts down from its load
 * at --timer-rate, divided by its prescale, while it's on (control bit 7).
 * --timer-dead 1: no timer runs; 2: only the first doesn't (a CX
 * CAS); 3: that, and the OS runs the fast timer, so it's left alone. */
struct host_timer
{
  uint32_t base, load, control, clock, from;
  double since;   /* when it last started counting from 'from' */
};
static struct host_timer other_timers[2] = { { 0x90010000u, 0, 0x20, 0, 0, 0 }, { 0x900D0020u, 0, 0x20, 0, 0, 0 } };
static uint32_t lcd_timing[3] = { 0x1425094C, 0x03780D3F, 0x00EF3802 };   /* the OS's portrait setup */
int host_is_cx2 = 1;   /* 0: an original CX, whose OS drives a 320x240 landscape LCD */
static uint32_t lcd_control = 0x392D;
static uint32_t lcd_raw_irq;
static uint32_t backlight = 120, backlight_at_start = 120;
static long backlight_writes;
unsigned host_hw_backlight(void) { return (unsigned)backlight; }
static uint16_t os_buffer[320 * 240];   /* the OS's LCD buffer */
static uintptr_t lcd_base = (uintptr_t)os_buffer, shown_base = (uintptr_t)os_buffer;
static long refresh_index;
static int lines_done;              /* the visible lines of this refresh were read */
static uint16_t shown_at_start[240 * 320];   /* the shown buffer as its refresh began (writes_to_shown) */
static uint16_t shown_image[240 * 320];   /* portrait, as the LCD read it */

/* What the report counts. */
static long refreshes, shown_changes, base_writes, distinct_frames;
static long writes_to_shown;
/* The game's picture drawn over (__wrap_nspire_present_frame). */
static long picture_frames_checked, picture_overwritten, picture_stale;
static char picture_first[128], picture_stale_first[128];
static FILE *pictures_file, *pictures_raw;
static double first_writes[5];
static int first_writes_count;
static char registered[256];
static long osscr_refreshes;
static int osscr_refreshed_lcd_os;   /* the OS's screen was back when it came */
static char notes[400][300];   /* shot and note lines, in order */
static int num_notes;

static const char *game_name(void)
{
  const char *slash = strrchr(gamepak_filename, '/');
  return gamepak_filename[0] ? (slash ? slash + 1 : gamepak_filename) : "-";
}

/* The menus' screen on show (nspire_ui.h), and the FPS counter's text. */
extern char ui_shown[32];
extern char nspire_fps_text[64];
extern uint32_t nspire_fps_full;

/* The test ROM's frame count (the 32-bit number at 0x02000000). */
static unsigned game_frame(void)
{
  return ewram[0] | (ewram[1] << 8) | (ewram[2] << 16) | ((unsigned)ewram[3] << 24);
}

static long touchpad_scans;   /* touchpad_scan calls so far (OUT-04) */

long long host_heap_live(void);
int host_files_open(void);
static void host_heap_report(FILE *f);
static void host_heap_hog(long long bytes);
static void host_heap_frag(long long bytes);
static void host_heap_dump(const char *name);

static void add_note(const char *kind, const char *name)
{
  extern uint32_t gamepak_buffer_count;
  unsigned frame = game_frame();
  char fps[64], *c;
  /* The report is split on spaces: the readout's ("fps: 60") are '_' there. */
  snprintf(fps, sizeof(fps), "%s", nspire_fps_text[0] ? nspire_fps_text : "-");
  for (c = fps; *c; c++)
    if (*c == ' ')
      *c = '_';
  if (num_notes < (int)(sizeof(notes) / sizeof(notes[0])))
    snprintf(notes[num_notes++], sizeof(notes[0]),
             "%s %s frame=%u time=%.3f shown=%ld backlight=%u ui=%s fps=%s full=%u scans=%ld heap=%lld files=%d rom_mb=%u game=%s",
             kind, name, frame, now / TICK_HZ, distinct_frames, (unsigned)backlight, ui_shown,
             fps, (unsigned)nspire_fps_full, touchpad_scans, host_heap_live(), host_files_open(),
             (unsigned)gamepak_buffer_count, game_name());
}

static uint64_t hash_bytes(const void *data, size_t size)
{
  const unsigned char *p = data;
  uint64_t h = 1469598103934665603ull;
  while (size--)
    h = (h ^ *p++) * 1099511628211ull;
  return h;
}

/* A script's "peek NAME ADDR:LEN": the game's work RAM from ADDR (hex,
 * 0x02... or 0x03...), LEN bytes, to out/NAME.bin, for a test to read the
 * game's state (the link tests' battles: tests/link/scenarios/pokemon.py). */
static void write_peek(const char *name, uint32_t address, unsigned len)
{
  char path[4300];
  FILE *f;
  unsigned i;

  snprintf(path, sizeof(path), "%s/%s.bin", out_dir, name);
  f = fopen(path, "wb");
  if (!f)
  {
    fprintf(stderr, "gpsp-host: can't write %s\n", path);
    return;
  }
  for (i = 0; i < len; i++)
  {
    uint32_t a = address + i;
    fputc(a >> 24 == 2 ? ewram[a & 0x3FFFF] : a >> 24 == 3 ? iwram[(a & 0x7FFF) + 0x8000] :
          a >> 24 == 4 && (a & 0xFFFFFF) < 0x400 ? ((unsigned char *)io_registers)[a & 0x3FF] : 0, f);
  }
  fclose(f);
}

static void write_ppm(const char *name)
{
  char path[4300];
  FILE *f;
  int x, y;

  snprintf(path, sizeof(path), "%s/%s.ppm", out_dir, name);
  f = fopen(path, "wb");
  if (!f)
  {
    fprintf(stderr, "gpsp-host: can't write %s\n", path);
    return;
  }
  __real_fwrite("P6\n320 240\n255\n", 1, 15, f);
  /* Landscape pixel (x, y) is position 239 - y of LCD line x (a CX's LCD
   * is landscape already). */
  for (y = 0; y < 240; y++)
    for (x = 0; x < 320; x++)
    {
      uint16_t p = host_is_cx2 ? shown_image[x * 240 + 239 - y] : shown_image[y * 320 + x];
      unsigned char rgb[3] = { (unsigned char)((p >> 11) << 3), (unsigned char)(((p >> 5) & 63) << 2),
                               (unsigned char)((p & 31) << 3) };
      if (!host_is_cx2)
      {
        /* nspire.c sets a CX's LCD to 1:5:5:5, blue in the top bits */
        rgb[0] = (unsigned char)((p & 31) << 3);
        rgb[1] = (unsigned char)(((p >> 5) & 31) << 3);
        rgb[2] = (unsigned char)(((p >> 10) & 31) << 3);
      }
      __real_fwrite(rgb, 1, 3, f);   /* not gpSP's writes: never short */
    }
  fclose(f);
}

unsigned nspire_link_log_live_writes(void);
static long log_file_writes, log_file_writes_at_quit = -1;   /* __wrap_fopen */
/* (r19-usb-exit) The logs written a line at a time: the buffer each was
 * written with last (setvbuf's size, 0 the C library's own: Ndless's newlib
 * gives 1 KB, one OS call each; -1 not written), the report's
 * "log_buffer:NAME SIZE". */
static const char *const buffered_logs[] = { "gpsp_pad.txt.tns", "gpsp_perf.txt.tns", "gpsp_states_time.txt.tns" };
#define BUFFERED_LOGS (sizeof(buffered_logs) / sizeof(buffered_logs[0]))
static long log_buffer_last[BUFFERED_LOGS] = { -1, -1, -1 };
static struct { FILE *f; int log; long size; } log_open[8];
static int host_restarted;
static double host_restarted_at;
static void write_report(void);

/* R11w part 2: GPSP_HOST_AUDIO=1: a USB sound adapter counts as in (the
 * volume keys, nspire_input.c) */
int host_audio_adapter(void)
{
  const char *e = getenv("GPSP_HOST_AUDIO");
  return e && e[0] == '1';
}

/* R11w: nspire_os_restart (host_platform.c): the calculator restarts: the
 * report now, then the process ends as a reset ends it (no atexit, no
 * flush of anything gpSP still holds). */
void host_hw_restart(void)
{
  host_restarted = 1;
  host_restarted_at = now;
  write_report();
  fflush(NULL);
  _exit(0);
}

static void write_report(void)
{
  char path[4300];
  FILE *f;
  int i;

  snprintf(path, sizeof(path), "%s/report.txt", out_dir);
  f = fopen(path, "w");
  if (!f)
    return;
  fprintf(f, "time %.2f\n", now / TICK_HZ);
  fprintf(f, "refreshes %ld\n", refreshes);
  fprintf(f, "frames_shown %ld\n", distinct_frames);
  fprintf(f, "lcd_base_writes %ld\n", base_writes);
  fprintf(f, "writes_to_shown %ld\n", writes_to_shown);
  fprintf(f, "picture_frames_checked %ld\n", picture_frames_checked);
  fprintf(f, "picture_overwritten %ld\n", picture_overwritten);
  fprintf(f, "picture_overwritten_first %s\n", picture_first[0] ? picture_first : "-");
  fprintf(f, "picture_stale %ld\n", picture_stale);
  fprintf(f, "picture_stale_first %s\n", picture_stale_first[0] ? picture_stale_first : "-");
  for (i = 0; i < first_writes_count; i++)
    fprintf(f, "write_to_shown_at %.3f\n", first_writes[i] / TICK_HZ);
  fprintf(f, "registered %s\n", registered[0] ? registered : "-");
  fprintf(f, "osscr_refreshes %ld\n", osscr_refreshes);
  fprintf(f, "osscr_refresh_lcd %s\n", osscr_refreshed_lcd_os ? "os" : "gpsp");
  fprintf(f, "backlight_start %u\n", (unsigned)backlight_at_start);
  fprintf(f, "backlight_end %u\n", (unsigned)backlight);
  fprintf(f, "backlight_writes %ld\n", backlight_writes);
  fprintf(f, "log_live_writes %u\n", nspire_link_log_live_writes());
  fprintf(f, "log_file_writes %ld\n", log_file_writes);
  fprintf(f, "log_file_writes_at_exit %ld\n", log_file_writes_at_quit < 0 ? -1 : log_file_writes - log_file_writes_at_quit);
  {
    unsigned i;
    for (i = 0; i < BUFFERED_LOGS; i++)
      fprintf(f, "log_buffer:%s %ld\n", buffered_logs[i], log_buffer_last[i]);
  }
  {
    extern int states_save_serve_seen;   /* (nspire_states.c: the last states_save's states_serve_os, -1 none) */
    fprintf(f, "states_save_serve %d\n", states_save_serve_seen);
  }
  host_link_report(f);
  host_os_keys_report(f);
  if (nspire_rom_load_error)
  {
    /* why the last game didn't load, on one line; its non-breaking spaces
     * (Latin-1's 0xa0) in UTF-8, as the report is read */
    const char *c;
    fprintf(f, "load_error ");
    for (c = nspire_rom_load_error; *c; c++)
      if ((unsigned char)*c == 0xa0)
        fputs("\xc2\xa0", f);
      else
        fputc(*c == '\n' ? ' ' : *c, f);
    fputc('\n', f);
  }
  fprintf(f, "lcd_base_end %s\n", lcd_base == (uintptr_t)os_buffer ? "os" : "gpsp");
  {
    /* R11w: the exit's watchdog backstop, the restart */
    extern int host_os_backstop_arms, host_os_backstop_on;
    fprintf(f, "exit_backstop_arms %d\n", host_os_backstop_arms);
    fprintf(f, "exit_backstop_end %s\n", host_os_backstop_on ? "on" : "off");
    fprintf(f, "restarted %d\n", host_restarted);
    fprintf(f, "restarted_at %.2f\n", host_restarted ? host_restarted_at / TICK_HZ : -1.0);
  }
  /* R11t: the shared clock selects at the end (gpSP's field put back) */
  fprintf(f, "timer_clock_os %08x\n", (unsigned)timer_clock_os);
  fprintf(f, "timer_clock_end %08x\n", (unsigned)timer_clock);
  fprintf(f, "fast_clock_os %08x\n", (unsigned)fast_clock_os);
  fprintf(f, "fast_clock_end %08x\n", (unsigned)other_timers[0].clock);
  fprintf(f, "clock_foreign_writes %ld\n", clock_foreign_writes);
  /* R12: every write to the select, the first timer as the OS last set it
   * and at the end (Run.finish: the same), gpSP's writes to it */
  fprintf(f, "timer_clock_writes %ld\n", clock_writes);
  fprintf(f, "timer1_os %02x %08x\n", (unsigned)t1_os_control, (unsigned)t1_os_load);
  fprintf(f, "timer1_end %02x %08x\n", (unsigned)t1.control, (unsigned)t1.load);
  fprintf(f, "timer1_writes %ld\n", t1_writes);
  /* QUIT6: its interrupt at the end (raw, and raw AND enable: line 18
   * raised for the OS, which never clears it), the times its count passed zero */
  {
    double zeros = t1_zeros_now();
    fprintf(f, "timer1_irq_end %d %d\n", t1_raised, t1_raised & (int)((t1.control >> 5) & 1));
    fprintf(f, "timer1_zeros %.0f\n", zeros);
  }
  fprintf(f, "os_usb_ticks %.0f\n", host_hw_os_usb_ticks());
  host_heap_report(f);
  for (i = 0; i < num_notes; i++)
    fprintf(f, "%s\n", notes[i]);
  fclose(f);
}

/* A refresh starts at every multiple of REFRESH_TICKS: the LCD takes the
 * buffer address and starts reading its lines. */
static void refresh_start(void)
{
  if (lcd_base != shown_base)
  {
    shown_changes++;
    shown_frame = lcd_base_frame;
    if (frame_trace)
      fprintf(frame_trace, "p %.3f %ld %u\n", (refresh_origin + refresh_index * REFRESH_TICKS) / MS, refresh_index, shown_frame);
  }
  shown_base = lcd_base;
  memcpy(shown_at_start, (const void *)shown_base, PORTRAIT_BYTES);
  if (memcmp(shown_image, (const void *)shown_base, PORTRAIT_BYTES))
    distinct_frames++;
  memcpy(shown_image, (const void *)shown_base, PORTRAIT_BYTES);
  refreshes++;
}

/* Its 320 lines are read: the buffer mustn't have changed meanwhile. The
 * vertical compare interrupt comes here (the OS sets it to the porch's start). */
static void refresh_lines_done(void)
{
  if (memcmp((const void *)shown_base, shown_at_start, PORTRAIT_BYTES))
  {
    writes_to_shown++;
    if (first_writes_count < 5)
      first_writes[first_writes_count++] = now;
  }
  lcd_raw_irq |= 4;
}

static void run_events(void)
{
  while (next_event < num_events)
  {
    struct event *e = &events[next_event];
    if (frames_left > 0)
      break;
    if (frames_left == 0)
    {
      /* The frames ran: the next part's times count from now. */
      frames_left = -1;
      part_start = now;
    }
    if (part_start + e->time > now)
      break;
    next_event++;
    switch (e->type)
    {
    case EV_DOWN:
      if (e->key == 0)
        on_key = 1;
      else
        keys[(e->key - 1) >> 5] |= 1u << ((e->key - 1) & 31);
      break;
    case EV_UP:
      if (e->key == 0)
        on_key = 0;
      else
        keys[(e->key - 1) >> 5] &= ~(1u << ((e->key - 1) & 31));
      break;
    case EV_SHOT:
      write_ppm(e->name);
      add_note("shot", e->name);
      break;
    case EV_NOTE:
      add_note("note", e->name);
      break;
    case EV_HOG:
      host_heap_hog((long long)e->value * 1024);
      break;
    case EV_FRAG:
      host_heap_frag((long long)e->value * 1024);
      break;
    case EV_HEAPDUMP:
      host_heap_dump(e->name);
      break;
    case EV_COST:
      if (e->key == COSTS)
      {
        /* cost trace: the frames' own costs from now on */
        trace_on = 1;
        costs[COST_FRAME] = costs[COST_LINE] = costs[COST_TURN] = 0;
      }
      else
      {
        costs[e->key] = e->value;
        if (e->key == COST_FRAME || e->key == COST_LINE || e->key == COST_TURN)
          trace_on = 0;
      }
      break;
    case EV_LINK:
      nspire_link_connect();
      add_note("note", "link");
      break;
    case EV_BATTERY:
      /* the header's battery for the pictures (the calculator never sets it until it is calibrated) */
      ui_battery = (int)e->key;
      ui_charging = (int)e->value;
      break;
    case EV_TOAST:
      /* a message over the game, as the frontend posts one (tests/host/ui_wired.py: long toasts' layout) */
      nspire_osd_message("%s", e->name);
      break;
    case EV_UNLINK:
      nspire_link_disconnect();
      add_note("note", "unlink");
      break;
    case EV_FRAMES:
      frames_left = (long)e->value;
      frames_until_linked = e->key;
      break;
    case EV_MEASURE:
      host_link_measure(e->key);
      break;
    case EV_PEEK:
      write_peek(e->name, (uint32_t)e->value, e->key);
      add_note("note", e->name);
      break;
    case EV_POKE:
      /* R4: a byte of the GBA's RAM set (a state as a crashed link left it). */
    {
      uint32_t a = (uint32_t)e->value;
      if (a >> 24 == 2)
        ewram[a & 0x3FFFF] = (unsigned char)e->key;
      else if (a >> 24 == 3)
        iwram[(a & 0x7FFF) + 0x8000] = (unsigned char)e->key;
      else if (a >> 24 == 0xE)
      {
        /* 2026-11-17: the cartridge's save memory, written as the game writes it (the core's own
         * write: the save is marked changed and its file follows): the pattern ROM saves nothing. */
        extern int write_memory8(uint32_t address, uint8_t value);
        write_memory8(a, (uint8_t)e->key);
      }
      break;
    }
    case EV_PAD:
      host_usbpad_script(e->name);
      break;
    case EV_BT:
      host_bt_script(e->name);
      break;
    case EV_CRASH:
      _exit(9);   /* a power-off: no flush, no report, no atexit */
    case EV_QUIT:
      add_note("note", "quit");
      /* R6: the script's end lets its keys go (gpSP's own exit, quit(),
       * waits for that; this one ends it through exit's handlers only). */
      memset(keys, 0, 4 * sizeof(keys[0]));
      on_key = 0;
      nspire_link_log_close();   /* gpsp_link.txt as the calculator's at its end */
      exit(0);   /* write_report runs at exit */
    }
  }
}

/* --lcd-timed: the refresh from timings 0 and 1 (see refresh_ticks); the refresh going on keeps its start. */
static void lcd_retime(void)
{
  uint32_t t0 = lcd_timing[0], t1 = lcd_timing[1];
  double clocks = 16.0 * (((t0 >> 2) & 0x3F) + 1) + ((t0 >> 8) & 0xFF) + 1 + ((t0 >> 16) & 0xFF) + 1 + (t0 >> 24) + 1;
  double start = refresh_origin + refresh_index * refresh_ticks;
  lcd_lines_total = (int)((t1 & 0x3FF) + 1 + ((t1 >> 10) & 0x3F) + 1 + ((t1 >> 16) & 0xFF) + (t1 >> 24));
  refresh_ticks = lcd_lines_total * 51.43e-6 * clocks / 389.0 * TICK_HZ;
  refresh_origin = start - refresh_index * refresh_ticks;
}

/* Time passes, and the LCD refreshes meanwhile. Nothing runs during it, so
 * once a refresh has gone by the buffers are as it saw them: the ones after
 * it (a long wait for the link) only count. */
static void pass_time(double ticks)
{
  int seen = 0;

  now += ticks;
  for (;;)
  {
    /* The CX II's LCD reads 320 lines, an older CX's 240 (as host_hw_lcd_current says). */
    double lines_end = refresh_origin + refresh_index * REFRESH_TICKS + REFRESH_TICKS * (host_is_cx2 ? 320 : 240) / LCD_LINES_TOTAL;
    if (!lines_done && now >= lines_end)
    {
      lines_done = 1;
      if (seen)
        lcd_raw_irq |= 4;
      else
        refresh_lines_done();
    }
    else if (now >= refresh_origin + (refresh_index + 1) * REFRESH_TICKS)
    {
      if (!lines_done)
      {
        if (seen)
          lcd_raw_irq |= 4;
        else
          refresh_lines_done();
      }
      refresh_index++;
      lines_done = 0;
      if (seen)
        refreshes++;
      else
        refresh_start();
      seen = 1;
    }
    else
      break;
  }
}

static void advance(double ticks)
{
  pass_time(ticks);
  run_events();
}

/* The link's (host_link.c): the virtual time in seconds, and time going by
 * while gpSP waits on the link or handles a message. The script's lines
 * wait for the next read of the hardware: the link's code mustn't run
 * from inside itself. */
double host_hw_now(void)
{
  return now / TICK_HZ;
}

const char *host_hw_out_dir(void)
{
  return out_dir;
}

uint32_t host_hw_pad_keys(void)
{
  return keys[4];
}

/* The keypad's words and the on key now (host_platform.c's model of the OS). */
void host_hw_keypad(uint32_t words[4], int *on)
{
  memcpy(words, keys, 4 * sizeof(keys[0]));
  *on = on_key;
}

void host_hw_spend(double seconds)
{
  if (seconds > 0)
    pass_time(seconds * TICK_HZ);
}

/* A message's cost (the script's cost send / receive lines), in seconds. */
double host_hw_message_cost(int received)
{
  return costs[received ? COST_RECEIVE : COST_SEND] / TICK_HZ;
}

static struct host_timer *other_timer(uint32_t reg)
{
  unsigned i;
  for (i = 0; i < 2; i++)
    if (reg >= other_timers[i].base && reg < other_timers[i].base + 12)
      return &other_timers[i];
  return NULL;
}

static uint32_t other_timer_value(const struct host_timer *t)
{
  static const unsigned divide[4] = { 1, 16, 256, 256 };
  if (timer_dead == 1 || !(t->control & 0x80))
    return t->from;
  return t->from - (uint32_t)(uint64_t)((now - t->since) * timer_rate / TICK_HZ / divide[(t->control >> 2) & 3]);
}

/* ---- R12: the first timer as an SP804, the OS's USB timer and its ticks ---- */

static void t1_os_events(void);

static uint32_t t1_value(void)
{
  static const unsigned divide[4] = { 1, 16, 256, 256 };
  uint32_t mask = (t1.control & 2) ? 0xFFFFFFFFu : 0xFFFFu, from = t1.from & mask, top;
  uint64_t n;

  if (timer_dead)
    return 0x12345678u;   /* --timer-dead: a first timer that never moves */
  if (!(t1.control & 0x80))
    return t1.from;
  n = (uint64_t)((now - t1.since) * t1_rate() / TICK_HZ / divide[(t1.control >> 2) & 3]);
  if (n <= from)
    return from - (uint32_t)n;
  if (t1.control & 1)
    return 0;   /* one-shot: stops at 0 */
  n -= (uint64_t)from + 1;
  top = (t1.control & 0x40) ? t1.load & mask : mask;
  return top - (uint32_t)(n % ((uint64_t)top + 1));
}

/* QUIT6: the times its count reached zero since t1.since (an SP804: on
 * reaching zero, then each time it does again; a one-shot stops there). */
static double t1_zeros_since(void)
{
  static const unsigned divide[4] = { 1, 16, 256, 256 };
  uint32_t mask = (t1.control & 2) ? 0xFFFFFFFFu : 0xFFFFu, from = t1.from & mask, top;
  double n;

  if (timer_dead || !(t1.control & 0x80))
    return 0;
  n = floor((now - t1.since) * t1_rate() / TICK_HZ / divide[(t1.control >> 2) & 3]);
  top = (t1.control & 0x40) ? t1.load & mask : mask;
  if (from == 0)   /* at zero already (counted then): the next zero a whole turn on */
    return (t1.control & 1) ? 0 : floor(n / ((double)top + 1));
  if (n < (double)from)
    return 0;
  if (t1.control & 1)
    return 1;
  return 1 + floor((n - from) / ((double)top + 1));
}

static double t1_zeros_now(void)
{
  double z = t1_zeros_since();
  if (z > 0)
    t1_raised = 1;
  return t1_zeros + z;
}

/* Its count goes on from where it is now (before a setting or a clock changes). */
static void t1_anchor(void)
{
  t1_zeros = t1_zeros_now();
  t1.from = t1_value();
  t1.since = now;
}

/* The OS's own doings with it (--timer1-os-jump, --timer1-os-stop). */
static void t1_os_events(void)
{
  if (t1_jump_at >= 0 && now >= t1_jump_at * TICK_HZ)
  {
    t1_jump_at = -1;
    t1_anchor();
    t1.from = (t1.control & 0x40) ? t1.load : 0xFFFFFFFFu;   /* its load written again: the count from there */
    t1_os_control = t1.control;
    t1_os_load = t1.load;
  }
  if (t1_stop_at >= 0 && now >= t1_stop_at * TICK_HZ)
  {
    t1_stop_at = -1;
    t1_anchor();
    t1.control &= ~0x80u;
    t1_os_control = t1.control;
    t1_os_load = t1.load;
  }
}

/* The adapter went in: the OS's USB timer starts (--os-usb-timer). */
void host_hw_adapter_in(void)
{
  if (!os_usb_load || os_usb_on)
    return;
  os_usb_on = 1;
  os2_acc = 0;
  os2_since = now;
}

/* The OS's USB ticks as it counts them (--os-usb-wait): its timer's
 * interrupts, or the first timer's counts by 0x8000, taken when the OS runs
 * (each OS sleep's start and end: host_platform.c). */
static double usb_periods_seen;
static long usb_irqs;
static uint32_t os_t1_prev;
static int os_t1_seen;
static double os_t1_counts;

static void os_looks(int sleep_end)
{
  double p = os2_runs() ? floor(os2_counts() / ((double)os2_load() + 1)) : 0;

  /* at the sleep's start one pending interrupt for gpSP's time (the CPU's
   * interrupts are masked while gpSP runs), at its end each one in the sleep */
  if (p > usb_periods_seen)
    usb_irqs += sleep_end ? (long)(p - usb_periods_seen) : 1;
  usb_periods_seen = p;
  t1_os_events();
  {
    uint32_t v = t1_value(), mask = (t1.control & 2) ? 0xFFFFFFFFu : 0xFFFFu;
    uint32_t top = (t1.control & 0x40) ? t1.load & mask : mask;
    double d;
    if (os_t1_seen)
    {
      d = v <= os_t1_prev ? (double)(os_t1_prev - v) : (double)os_t1_prev + 1 + (double)(top >= v ? top - v : 0);
      /* (a jump, a rewrite by whoever: not time) */
      if (d <= t1_rate() * 0.5)
        os_t1_counts += d;
    }
    os_t1_prev = v;
    os_t1_seen = 1;
  }
}

void host_hw_os_sleep_begin(void)
{
  os_looks(0);
}

void host_hw_os_sleep_end(void)
{
  os_looks(1);
}

double host_hw_os_usb_ticks(void)
{
  return os_wait_timer1 ? floor(os_t1_counts / 0x8000) : (double)usb_irqs;
}

/* nspire_cx2_timer_hz on the PC (host_platform.c): the first timer's
 * speed as the model has it, at once (the calculator's 20 ms loop isn't
 * timed here), off by GPSP_HOST_CPU_TIMING_PPM (the CPU loop's error). */
uint32_t host_hw_timer_hz(uint32_t value_reg)
{
  static const unsigned divide[4] = { 1, 16, 256, 256 };
  const char *ppm = getenv("GPSP_HOST_CPU_TIMING_PPM");
  double hz;

  advance(READ_STEP);
  t1_os_events();
  if (value_reg != HW_TIMER_VALUE || timer_dead || !(t1.control & 0x80))
    return 0;
  hz = t1_rate() / divide[(t1.control >> 2) & 3];
  if (ppm)
    hz *= 1.0 + atof(ppm) / 1e6;
  return (uint32_t)(hz + 0.5);
}

uint32_t host_hw_read(uint32_t reg)
{
  struct host_timer *t;

  advance(READ_STEP);
  t1_os_events();
  /* R12: a CX II's second block's other half (read for the logs only) */
  if (host_is_cx2 && reg >= 0x900D0020u && reg < 0x900D0040u)
    return reg == 0x900D0024u ? 0xFFFFFFFFu : 0;
  if ((t = other_timer(reg)) != NULL)
  {
    if (host_is_cx2)
      fprintf(stderr, "gpsp-host: an older CX's other timer read on a CX II\n");
    switch (reg - t->base)
    {
    case 0:
      return t->load;
    case 4:
      return other_timer_value(t);
    default:
      return t->control;
    }
  }
  switch (reg)
  {
  /* R11t: the first block's other timer, the OS's on a CX II (periodic,
   * its interrupt on), at its own field's clock */
  case 0x900C0020u:
  case 0x900C0038u:
    return os2_load();
  case 0x900C0024u:
  {
    uint64_t c = (uint64_t)os2_counts();
    return os2_load() - (uint32_t)(c % ((uint64_t)os2_load() + 1));
  }
  case 0x900C0028u:
    return !host_is_cx2 ? 0 : os2_runs() ? 0xE2u : 0x42u;
  case 0x900C0018u:   /* the first timer's background load: its load */
    return t1.load;
  case 0x900C0010u:   /* QUIT6: its raw interrupt, and that AND its enable (line 18) */
  case 0x900C0014u:
    t1_zeros_now();
    return (uint32_t)(reg == 0x900C0010u ? t1_raised : t1_raised & (t1.control >> 5) & 1);
  case 0x900C0030u:   /* the OS's timers' interrupts (read for the logs), an older */
  case 0x900C0034u:   /* CX's other timers': not modelled */
  case 0x900D0010u:
  case 0x900D0014u:
  case 0x90010010u:
  case 0x90010014u:
  case 0x900D0030u:
  case 0x900D0034u:
    return 0;
  case 0x900D0004u:   /* the OS's tick (periodic from 0x148 at 32768 Hz: ~10 ms) */
    return 0x148u - (uint32_t)((uint64_t)now % 0x149u);
  case 0x900D0018u:
    return 0x148u;
  /* the fast timer's other half (runs nowhere) and the OS's tick (the
   * second block's first: control 0xE6), read only for the logs */
  case 0x90010020u:
  case 0x90010028u:
    return 0;
  case 0x90010024u:
    return 0xFFFFFFFFu;
  case 0x900D0000u:
    return 0x148u;
  case 0x900D0008u:
    return 0xE6u;
  case 0x90010080u:
    return other_timers[0].clock;
  case 0x900D0080u:
    return other_timers[1].clock;
  case HW_CX_CLOCK_GATES:
  case HW_CX_CLOCK_GATES2:
    if (host_is_cx2)
      fprintf(stderr, "gpsp-host: an older CX's power controller read on a CX II\n");
    return 0;
  case HW_TIMER_VALUE:
    /* Counts down; at --timer-rate's speed (an older CX's is slower);
     * --timer-dead: a timer that never moves (the others: other_timers).
     * R11t: at its own field's clock (bits 1:0 of the shared select); R12:
     * an SP804 (t1_value) */
    return t1_value();
  case HW_RTC_SECONDS:
    /* --rtc-frozen: a real-time clock that doesn't move while gpSP runs. */
    return rtc_start + (rtc_frozen ? 0 : (uint32_t)(now / TICK_HZ));
  case HW_TIMER_LOAD:
    return t1.load;
  case HW_TIMER_CONTROL:
    return t1.control;
  case HW_TIMER_CLOCK:
    return timer_clock;
  case HW_KEYPAD:
  case HW_KEYPAD + 4:
  case HW_KEYPAD + 8:
  case HW_KEYPAD + 12:
    return keys[(reg - HW_KEYPAD) / 4];
  case HW_LCD_TIMING0:
  case HW_LCD_TIMING1:
  case HW_LCD_TIMING2:
    return lcd_timing[(reg - HW_LCD_TIMING0) / 4];
  case HW_LCD_CONTROL:
    return lcd_control;
  case HW_LCD_RAW_IRQ:
    return lcd_raw_irq;
  case HW_LCD_IRQ_MASK:
    return 0;
  case HW_BACKLIGHT:
    if (!host_is_cx2)
      fprintf(stderr, "gpsp-host: the CX II's backlight register read on an older CX\n");
    return backlight;
  case HW_CX_BACKLIGHT:
    if (host_is_cx2)
      fprintf(stderr, "gpsp-host: the older CX's backlight register read on a CX II\n");
    return backlight;
  default:
    fprintf(stderr, "gpsp-host: read of unknown register %08x\n", (unsigned)reg);
    return 0;
  }
}

void host_hw_write(uint32_t reg, uint32_t value)
{
  struct host_timer *t;

  if ((t = other_timer(reg)) != NULL)
  {
    if (host_is_cx2)
      fprintf(stderr, "gpsp-host: an older CX's other timer written on a CX II\n");
    if (t == &other_timers[0] && timer_dead == 3)
      fprintf(stderr, "gpsp-host: the OS's fast timer written\n");
    /* As an SP804: a load starts the count from it; the control's new
     * setting counts on from where it is. */
    t->from = reg == t->base ? value : other_timer_value(t);
    t->since = now;
    if (reg == t->base)
      t->load = value;
    else if (reg == t->base + 8)
    {
      t->control = value;
    }
    return;
  }
  switch (reg)
  {
  case 0x900C0020u:
  case 0x900C0024u:
  case 0x900C0028u:
  case 0x900C002Cu:
    fprintf(stderr, "gpsp-host: the first block's other timer (the OS's on a CX II) written: %08x = %08x\n",
            (unsigned)reg, (unsigned)value);
    return;
  case 0x90010080u:
    /* R11t: its bits 31:2 are not the fast timer's */
    if ((value ^ fast_clock_os) & ~3u)
    {
      clock_foreign_writes++;
      fprintf(stderr, "gpsp-host: the fast timer's clock select changed in bits not its own: %08x -> %08x\n",
              (unsigned)other_timers[0].clock, (unsigned)value);
    }
    other_timers[0].clock = value;
    return;
  case 0x900D0080u:
    fprintf(stderr, "gpsp-host: the OS's tick's clock select written\n");
    return;
  case HW_TIMER_LOAD:
    /* an SP804: a load starts the count from it */
    t1_os_events();
    t1_anchor();
    t1.load = t1.from = value;
    t1_writes++;
    break;
  case HW_TIMER_CONTROL:
    /* the count goes on from where it is */
    t1_os_events();
    t1_anchor();
    t1.control = value;
    t1_writes++;
    break;
  case 0x900C000Cu:
    /* QUIT6: its interrupt clear */
    t1_os_events();
    t1_anchor();
    t1_raised = 0;
    t1_writes++;
    break;
  case 0x9001000Cu:
  case 0x900D002Cu:
    return;   /* an older CX's other timers' interrupt clears (not modelled) */
  case HW_TIMER_CLOCK:
    /* R11t: one word for both timers: each counts on at its new field's clock */
    t1_os_events();
    t1_anchor();
    os2_acc = os2_counts();
    os2_since = now;
    clock_writes++;
    if ((value ^ timer_clock_os) & ~3u)
    {
      clock_foreign_writes++;
      fprintf(stderr, "gpsp-host: the clock select 900C0080 changed in bits not the first timer's (the timer at "
              "900C0020's, the OS's on a CX II, or unknown ones): %08x -> %08x (the OS had %08x)\n",
              (unsigned)timer_clock, (unsigned)value, (unsigned)timer_clock_os);
    }
    timer_clock = value;
    break;
  case HW_LCD_TIMING0:
  case HW_LCD_TIMING1:
  case HW_LCD_TIMING2:
    lcd_timing[(reg - HW_LCD_TIMING0) / 4] = value;
    if (lcd_timed)
      lcd_retime();
    break;
  case HW_LCD_CONTROL:
    lcd_control = value;
    break;
  case HW_LCD_IRQ_CLEAR:
    lcd_raw_irq &= ~value;
    break;
  case HW_BACKLIGHT:
    if (value > 225 || !host_is_cx2)
    {
      fprintf(stderr, "gpsp-host: backlight set to %u, past the OS's darkest (225), or on an older CX\n",
              (unsigned)value);
      exit(4);
    }
    backlight = value;
    backlight_writes++;
    break;
  case HW_CX_BACKLIGHT:
    /* Hackspire: 0x100 is off, about 0x1D0 the brightest. */
    if (value <= 0x100 || value > 0x1D0 || host_is_cx2)
    {
      fprintf(stderr, "gpsp-host: CX backlight set to %03x (off or past 0x1d0), or on a CX II\n", (unsigned)value);
      exit(4);
    }
    backlight = value;
    backlight_writes++;
    break;
  default:
    fprintf(stderr, "gpsp-host: write of unknown register %08x\n", (unsigned)reg);
  }
}

uintptr_t host_hw_lcd_base(void)
{
  return lcd_base;
}

void host_hw_set_lcd_base(uintptr_t base)
{
  lcd_base = base;
  lcd_base_frame = game_frame();
  base_writes++;
}

/* The line of the shown buffer the LCD reads now (past its end during the
 * porches). */
uintptr_t host_hw_lcd_current(void)
{
  double line;
  advance(READ_STEP);
  line = fmod(now - refresh_origin, REFRESH_TICKS) / REFRESH_TICKS * LCD_LINES_TOTAL;
  if (!host_is_cx2)
    return line >= 240 ? shown_base + PORTRAIT_BYTES : shown_base + (uintptr_t)line * 640;
  if (line >= 320)
    return shown_base + PORTRAIT_BYTES;
  return shown_base + (uintptr_t)line * 480;
}

/* ---- What takes time on the calculator (the linker's --wrap) ---- */

void __real_execute_arm(uint32_t cycles);
bool __real_update_serial(unsigned cycles);
void __real_update_scanline(void);
void __real_nspire_turn_raw(uint16_t *dst, const uint16_t *src, uint32_t pitch);
void __real_nspire_turn_1x(uint16_t *dst, const uint16_t *src, uint32_t pitch);
void __real_nspire_turn_rect_555(uint16_t *dst, const uint16_t *src, uint32_t pitch, uint32_t x0,
                                 uint32_t y0, uint32_t w, uint32_t h);
void __real_nspire_turn_rect_565(uint16_t *dst, const uint16_t *src, uint32_t pitch, uint32_t x0,
                                 uint32_t y0, uint32_t w, uint32_t h);

/* Of this frame's emulation cost, what its cycles have spent so far. */
static double frame_spent;
/* "cost trace": the drawn frame's picture time, until nspire_present_frame
 * (or the next frame, if it showed nothing) spends it. */
static double trace_picture;

void __wrap_execute_arm(uint32_t cycles)
{
  __real_execute_arm(cycles);
  if (frames_left > 0)
  {
    frames_left--;
    if (frames_until_linked && nspire_link_state() == LINK_LINKED)
      frames_left = 0;
  }
  if (trace_on && trace_len)
  {
    /* The model's frame: its game, and its drawing and picture if drawn
     * (all here: the line and turn costs are off). */
    double *c = trace_costs[trace_next];
    trace_next = (trace_next + 1) % trace_len;
    advance((c[0] > frame_spent ? c[0] - frame_spent : 0) + (skip_next_frame ? 0 : c[1]) + trace_picture);
    /* The picture's time goes by in nspire_present_frame (its wrapper),
     * which the frontend times as the picture's (output_ticks). */
    trace_picture = skip_next_frame ? 0 : c[2];
  }
  else
    advance(costs[COST_FRAME] > frame_spent ? costs[COST_FRAME] - frame_spent : 0);
  frame_spent = 0;
  if (frame_trace)
    fprintf(frame_trace, "f %.3f %u %d\n", now / MS, game_frame(), !skip_next_frame);
  host_link_frame(!skip_next_frame);
  alarm(NO_PROGRESS_S);   /* a frame ran: the watchdog starts again (main) */
}

/* The core counts the cycles it has run here (update_gba). While linked,
 * they cost their share of the frame as they go: the other GBA waits on
 * this one's time (nspire_link.c), so the time a transfer comes at is as
 * on the calculator. */
bool __wrap_update_serial(unsigned cycles)
{
  if (nspire_link_state() == LINK_LINKED && costs[COST_FRAME] > 0)
  {
    double share = costs[COST_FRAME] * cycles / 280896.0;
    pass_time(share);
    frame_spent += share;
  }
  return __real_update_serial(cycles);
}

void __wrap_update_scanline(void)
{
  __real_update_scanline();
  if (!skip_next_frame)
    advance(costs[COST_LINE]);
}

static void turn_cost(uint32_t pixels)
{
  advance(costs[COST_TURN] * pixels / (320.0 * 240.0));
}

/* ---- The game's picture, checked as it is shown ----
 *
 * Every picture function nspire_present_frame calls is run again into two
 * scratch buffers, one filled with 0x0000 and one with 0xFFFF first: a pixel
 * it writes differs from the fill in one of them, so that gives exactly the
 * pixels it writes and their values. When nspire_present_frame has flipped
 * the buffer, each of those pixels must still hold that value: anything
 * drawn after the picture (the text bands, a message) went over the game
 * (the CX II's top row flashed black, 2026-11-11). The 1:1 boxes over a
 * picture that fills the screen are by design: fullscreen isn't checked.
 * The report: picture_frames_checked, picture_overwritten (frames with a
 * changed pixel) and where the first one was. With GPSP_HOST_PICTURES=1,
 * OUTDIR/pictures.txt gets a line for each picture shown: the core's frame
 * count, a hash of the whole buffer (tests compare runs frame by frame) and
 * how many game frames before it the frame blended with was (0: none). */
enum pic_kind { PIC_TURN_RAW, PIC_TURN_1X, PIC_ASPECT, PIC_FULL, PIC_L_ASPECT, PIC_UP_ASPECT, PIC_UP_FAST,
                PIC_UP_RAW, PIC_UP_RAW_GBA };
struct pic_call
{
  enum pic_kind kind;
  const uint16_t *src, *prev;
  uint32_t pitch;
};
#define PIC_PIXELS (320 * 240)
static uint16_t pic_expect[PIC_PIXELS], pic_fill_0[PIC_PIXELS], pic_fill_1[PIC_PIXELS];
static uint8_t pic_mask[PIC_PIXELS];
static int in_present, pic_drawn;
extern void *nspire_displayed_screen;
extern void *nspire_screen;
extern uint32_t screen_scale;
extern uint32_t frame_counter;
extern int nspire_lcd_portrait;
extern uint32_t nspire_blend_gap;   /* this picture blended with the frame this many before (0: not blended) */

void __real_nspire_blend_turn_aspect(uint16_t *dst, const uint16_t *src, const uint16_t *prev);
void __real_nspire_blend_turn_full(uint16_t *dst, const uint16_t *src, const uint16_t *prev);
void __real_nspire_blend_aspect_landscape(uint16_t *dst, const uint16_t *src, const uint16_t *prev);
void __real_upscale_aspect(uint16_t *dst, uint16_t *src);
void __real_upscale_aspect_fast(uint16_t *dst, uint16_t *src);
void __real_upscale_aspect_raw(uint16_t *dst, uint16_t *src);
void __real_upscale_aspect_raw_from_gba(uint16_t *dst, const uint16_t *gba);
void __real_nspire_present_frame(void);

static void pic_draw(const struct pic_call *c, uint16_t *dst)
{
  switch (c->kind)
  {
  case PIC_TURN_RAW: __real_nspire_turn_raw(dst, c->src, c->pitch); break;
  case PIC_TURN_1X: __real_nspire_turn_1x(dst, c->src, c->pitch); break;
  case PIC_ASPECT: __real_nspire_blend_turn_aspect(dst, c->src, c->prev); break;
  case PIC_FULL: __real_nspire_blend_turn_full(dst, c->src, c->prev); break;
  case PIC_L_ASPECT: __real_nspire_blend_aspect_landscape(dst, c->src, c->prev); break;
  case PIC_UP_ASPECT: __real_upscale_aspect(dst, (uint16_t *)c->src); break;
  case PIC_UP_FAST: __real_upscale_aspect_fast(dst, (uint16_t *)c->src); break;
  case PIC_UP_RAW: __real_upscale_aspect_raw(dst, (uint16_t *)c->src); break;
  case PIC_UP_RAW_GBA: __real_upscale_aspect_raw_from_gba(dst, c->src); break;
  }
}

/* The picture function's call (already made into the screen buffer), done
 * again into the scratch buffers: the pixels it writes and their values. */
static void picture_drawn(enum pic_kind kind, const uint16_t *src, const uint16_t *prev, uint32_t pitch)
{
  struct pic_call c = { kind, src, prev, pitch };
  int i;

  if (!in_present)
    return;
  memset(pic_fill_0, 0, sizeof(pic_fill_0));
  memset(pic_fill_1, 0xFF, sizeof(pic_fill_1));
  pic_draw(&c, pic_fill_0);
  pic_draw(&c, pic_fill_1);
  for (i = 0; i < PIC_PIXELS; i++)
    if (pic_fill_0[i] != 0 || pic_fill_1[i] != 0xFFFF)
    {
      pic_mask[i] = 1;
      pic_expect[i] = pic_fill_1[i];
    }
  pic_drawn = 1;
}

void __wrap_nspire_present_frame(void)
{
  const uint16_t *shown;
  int i;

  advance(trace_picture);
  trace_picture = 0;
  memset(pic_mask, 0, sizeof(pic_mask));
  pic_drawn = 0;
  in_present = 1;
  {
    const void *was = nspire_displayed_screen;
    __real_nspire_present_frame();
    in_present = 0;
    if (!pic_drawn)
      return;
    /* (final/r19-pace) not flipped yet: fast forward flips it once the LCD has
     * the last one (nspire_frameskip.c ff_flush); the picture is in the buffer
     * drawn into */
    shown = (const uint16_t *)(nspire_displayed_screen == was ? nspire_screen : nspire_displayed_screen);
  }
  if (screen_scale != 2)   /* fullscreen */
  {
    picture_frames_checked++;
    for (i = 0; i < PIC_PIXELS; i++)
      if (pic_mask[i] && shown[i] != pic_expect[i])
      {
        if (!picture_overwritten++)
        {
          /* landscape (x, y): a portrait buffer holds (x, y) at x * 240 + 239 - y */
          int x = nspire_lcd_portrait ? i / 240 : i % 320, y = nspire_lcd_portrait ? 239 - i % 240 : i / 320;
          snprintf(picture_first, sizeof(picture_first), "frame=%u x=%d y=%d scale=%u was=%04x now=%04x",
                   (unsigned)frame_counter, x, y, (unsigned)screen_scale, (unsigned)pic_expect[i],
                   (unsigned)shown[i]);
        }
        break;
      }
    /* And everything else but the text bands is
     * black, in every buffer: nothing left of a screen drawn before (the
     * link's wait screen stayed in line 226 of the CX's 3:2, in the CX
     * II's unscaled border). */
    for (i = 0; i < PIC_PIXELS; i++)
    {
      int x = nspire_lcd_portrait ? i / 240 : i % 320, y = nspire_lcd_portrait ? 239 - i % 240 : i / 320;
      if (!pic_mask[i] && shown[i] && y >= 13 && y < 240 - 13)   /* the bands: OSD_BAND_H */
      {
        if (!picture_stale++)
          snprintf(picture_stale_first, sizeof(picture_stale_first), "frame=%u x=%d y=%d scale=%u pixel=%04x",
                   (unsigned)frame_counter, x, y, (unsigned)screen_scale, (unsigned)shown[i]);
        break;
      }
    }
  }
  if (pictures_file)
    fprintf(pictures_file, "%u %016llx %u %.3f\n", (unsigned)frame_counter,
            (unsigned long long)hash_bytes(shown, PIC_PIXELS * 2), (unsigned)nspire_blend_gap, now * 1000 / TICK_HZ);
  if (pictures_raw)
    __real_fwrite(shown, 2, PIC_PIXELS, pictures_raw);
}

void __wrap_nspire_turn_raw(uint16_t *dst, const uint16_t *src, uint32_t pitch)
{
  __real_nspire_turn_raw(dst, src, pitch);
  picture_drawn(PIC_TURN_RAW, src, NULL, pitch);
  turn_cost(320 * 216);
}

void __wrap_nspire_turn_1x(uint16_t *dst, const uint16_t *src, uint32_t pitch)
{
  __real_nspire_turn_1x(dst, src, pitch);
  picture_drawn(PIC_TURN_1X, src, NULL, pitch);
  turn_cost(240 * 160);
}

/* An older CX's pictures (no turning: no cost here, as before). */
void __wrap_nspire_blend_aspect_landscape(uint16_t *dst, const uint16_t *src, const uint16_t *prev)
{
  __real_nspire_blend_aspect_landscape(dst, src, prev);
  picture_drawn(PIC_L_ASPECT, src, prev, 0);
}

void __wrap_upscale_aspect(uint16_t *dst, uint16_t *src)
{
  __real_upscale_aspect(dst, src);
  picture_drawn(PIC_UP_ASPECT, src, NULL, 0);
}

void __wrap_upscale_aspect_fast(uint16_t *dst, uint16_t *src)
{
  __real_upscale_aspect_fast(dst, src);
  picture_drawn(PIC_UP_FAST, src, NULL, 0);
}

void __wrap_upscale_aspect_raw(uint16_t *dst, uint16_t *src)
{
  __real_upscale_aspect_raw(dst, src);
  picture_drawn(PIC_UP_RAW, src, NULL, 0);
}

void __wrap_upscale_aspect_raw_from_gba(uint16_t *dst, const uint16_t *gba)
{
  __real_upscale_aspect_raw_from_gba(dst, gba);
  picture_drawn(PIC_UP_RAW_GBA, gba, NULL, 0);
}

void __wrap_nspire_turn_rect_555(uint16_t *dst, const uint16_t *src, uint32_t pitch, uint32_t x0,
                                 uint32_t y0, uint32_t w, uint32_t h)
{
  __real_nspire_turn_rect_555(dst, src, pitch, x0, y0, w, h);
  turn_cost(w * h);
}

void __wrap_nspire_turn_rect_565(uint16_t *dst, const uint16_t *src, uint32_t pitch, uint32_t x0,
                                 uint32_t y0, uint32_t w, uint32_t h)
{
  __real_nspire_turn_rect_565(dst, src, pitch, x0, y0, w, h);
  turn_cost(w * h);
}

/* Reading ROM files (and unpacking zips) costs what the script says a MB. */
struct RFILE;
struct zip_rom;
int64_t __real_filestream_read(struct RFILE *stream, void *data, int64_t len);
int __real_zip_read(struct zip_rom *zip, uint8_t *dst, uint32_t n);

int64_t __wrap_filestream_read(struct RFILE *stream, void *data, int64_t len)
{
  int64_t got = __real_filestream_read(stream, data, len);
  if (got > 0)
    advance(costs[COST_READ] * (double)got / (1024.0 * 1024.0));
  return got;
}

int __wrap_zip_read(struct zip_rom *zip, uint8_t *dst, uint32_t n)
{
  int result = __real_zip_read(zip, dst, n);
  advance(costs[COST_READ] * (double)n / (1024.0 * 1024.0));
  return result;
}

/* ---- A calculator short of memory or storage (the linker's --wrap) ----
 *
 * --rom-memory MB: the most 1 MB blocks gpSP can have at once, as on a
 * calculator whose free memory is short (open documents, fragments): the
 * ROM buffer (init_gamepak_buffer) and a zip's (gba_memory.c) are those
 * blocks; a malloc of one more fails. Other allocations aren't limited.
 * --free-storage KB: the storage free for what gpSP writes. A write past it
 * comes back short, as on a full calculator; a file removed gives its size
 * back. */
void *__real_malloc(size_t size);
void __real_free(void *p);
int __real_remove(const char *path);

#define ROM_BLOCK (1024 * 1024)
static int rom_memory_mb = -1;              /* -1: as much as there is */
static void *rom_blocks[64];
static int rom_blocks_held;
static long long free_storage = -1;         /* bytes; -1: no limit */
static int storage_errno = 1;               /* a write past it sets ENOSPC */

/* save17: the heap gpSP holds, as the calculator's would be (its OS's malloc
 * is the only one): every malloc, calloc and realloc of gpSP's (not libc's
 * own inside fopen and the like) counted while it is held. --heap-limit KB:
 * one that would take more than that fails, as on a calculator whose memory
 * is that short (the ROM's blocks included). The report's heap_live and
 * heap_peak (bytes), heap_failed (allocations refused) and each note's
 * heap= say how much was held. */
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *p, size_t size);
#define HEAP_SLOTS (1u << 18)
static struct heap_slot { void *p; size_t size; void *caller; } heap_slots[HEAP_SLOTS];
static long long heap_live, heap_peak, heap_limit = -1;
static long heap_failed;
static unsigned heap_held;
static long long heap_hogged;   /* (the script's hog: counted as held, nothing allocated; not gpSP's) */
static long long hog_short;     /* what the last hog asked for and didn't get */
static long long frag_largest = -1;   /* the script's frag: the largest block there is (-1: any) */
/* --concurrent KB (save17): at each of gpSP's allocations, would someone else's KB (an OS, USB,
 * Bluetooth or link allocation in that moment) still fit? The report's concurrent_short counts the times not */
static long long concurrent_bytes;
static long concurrent_short;

static unsigned heap_hash(const void *p)
{
  return (unsigned)(((uintptr_t)p >> 4) * 2654435761u) & (HEAP_SLOTS - 1);
}

static int heap_room(size_t more)
{
  if ((heap_limit >= 0 && heap_live + (long long)more > heap_limit) ||
      (frag_largest >= 0 && (long long)more > frag_largest))
  {
    heap_failed++;
    return 0;
  }
  return 1;
}

static void *heap_caller;   /* (the wrapper's caller, for heapdump) */

static void heap_add(void *p, size_t size)
{
  unsigned i = heap_hash(p);
  if (!p || heap_held >= HEAP_SLOTS - 1)
    return;
  while (heap_slots[i].p && heap_slots[i].p != (void *)1)
    i = (i + 1) & (HEAP_SLOTS - 1);
  heap_slots[i].p = p;
  heap_slots[i].size = size;
  heap_slots[i].caller = heap_caller;
  heap_held++;
  heap_live += (long long)size;
  if (heap_live - heap_hogged > heap_peak)
    heap_peak = heap_live - heap_hogged;
  if (concurrent_bytes && heap_limit >= 0 && heap_live + concurrent_bytes > heap_limit)
    concurrent_short++;
}

/* p's size, taken out of the count (0: not one of gpSP's) */
static size_t heap_take(void *p)
{
  unsigned i = heap_hash(p), n;
  for (n = 0; p && heap_slots[i].p && n < HEAP_SLOTS; n++, i = (i + 1) & (HEAP_SLOTS - 1))
    if (heap_slots[i].p == p)
    {
      size_t size = heap_slots[i].size;
      heap_slots[i].p = (void *)1;   /* (a gap the search goes past) */
      heap_held--;
      heap_live -= (long long)size;
      return size;
    }
  return 0;
}

static size_t heap_size_of(void *p)
{
  unsigned i = heap_hash(p), n;
  for (n = 0; p && heap_slots[i].p && n < HEAP_SLOTS; n++, i = (i + 1) & (HEAP_SLOTS - 1))
    if (heap_slots[i].p == p)
      return heap_slots[i].size;
  return 0;
}

void *__wrap_malloc(size_t size)
{
  void *p;
  heap_caller = __builtin_return_address(0);
  if (!heap_room(size))
    return NULL;
  if (size != ROM_BLOCK || rom_memory_mb < 0)
  {
    p = __real_malloc(size);
    heap_add(p, size);
    return p;
  }
  if (rom_blocks_held >= rom_memory_mb || rom_blocks_held >= 64)
    return NULL;
  p = __real_malloc(size);
  if (p)
    rom_blocks[rom_blocks_held++] = p;
  heap_add(p, size);
  return p;
}

void *__wrap_calloc(size_t n, size_t size)
{
  void *p;
  heap_caller = __builtin_return_address(0);
  if (size && n > (size_t)-1 / size)
    return NULL;
  if (!heap_room(n * size))
    return NULL;
  p = __real_calloc(n, size);
  heap_add(p, n * size);
  return p;
}

void *__wrap_realloc(void *p, size_t size)
{
  size_t had = heap_size_of(p);
  void *q;
  heap_caller = __builtin_return_address(0);
  if (size > had && !heap_room(size - had))
    return NULL;
  q = __real_realloc(p, size);
  if (q || !size)
  {
    heap_take(p);
    heap_add(q, size);
  }
  return q;
}

void __wrap_free(void *p)
{
  int i;
  for (i = 0; p && i < rom_blocks_held; i++)
    if (rom_blocks[i] == p)
    {
      rom_blocks[i] = rom_blocks[--rom_blocks_held];
      break;
    }
  heap_take(p);
  __real_free(p);
}


/* save17: the script's "heapdump NAME": OUTDIR/heap_NAME.txt, gpSP's live allocations summed by the code that
 * made them (an address: addr2line -f -e build/gpsp-host), the biggest first */
static void host_heap_dump(const char *name)
{
  static struct { void *caller; long long bytes; long n; } by[4096];
  int count = 0, i, j;
  unsigned k;
  char path[4400];
  FILE *f;
  for (k = 0; k < HEAP_SLOTS; k++)
  {
    if (!heap_slots[k].p || heap_slots[k].p == (void *)1)
      continue;
    for (i = 0; i < count && by[i].caller != heap_slots[k].caller; i++)
      ;
    if (i == count)
    {
      if (count == 4096)
        continue;
      by[count].caller = heap_slots[k].caller;
      by[count].bytes = 0;
      by[count].n = 0;
      count++;
    }
    by[i].bytes += (long long)heap_slots[k].size;
    by[i].n++;
  }
  for (i = 1; i < count; i++)
    for (j = i; j > 0 && by[j].bytes > by[j - 1].bytes; j--)
    {
      __typeof__(by[0]) t = by[j];
      by[j] = by[j - 1];
      by[j - 1] = t;
    }
  snprintf(path, sizeof(path), "%s/heap_%s.txt", out_dir, name);
  f = fopen(path, "w");   /* (OUTDIR: not counted) */
  if (!f)
    return;
  for (i = 0; i < count; i++)
    fprintf(f, "%p %lld %ld\n", (void *)((char *)by[i].caller - (char *)&host_heap_dump), by[i].bytes, by[i].n);
  fclose(f);
}

/* the script's frag: no block over bytes from now on (the heap in pieces), whatever the total free */
static void host_heap_frag(long long bytes)
{
  frag_largest = bytes > 0 ? bytes : -1;
  heap_failed = 0;
}

/* gpSP's (the hog's not counted) */
long long host_heap_live(void)
{
  return heap_live - heap_hogged;
}

static void host_heap_hog(long long bytes)
{
  /* (save17: as someone else's mallocs, it gets what is free and no more: the report's hog_short is
   * what it asked for and didn't get) */
  heap_live -= heap_hogged;
  heap_hogged = 0;
  if (heap_limit >= 0 && heap_live + bytes > heap_limit)
  {
    hog_short = heap_live + bytes - heap_limit;
    bytes = heap_limit - heap_live > 0 ? heap_limit - heap_live : 0;
  }
  else
    hog_short = 0;
  heap_live += bytes;
  heap_hogged = bytes;
  /* (the report's peak and refusals count from here) */
  heap_peak = heap_live - heap_hogged;
  heap_failed = 0;
}

static int files_open, files_open_peak;
static long files_refused;

int host_files_open(void)
{
  return files_open;
}

static void host_heap_report(FILE *f)
{
  fprintf(f, "heap_live %lld\n", heap_live - heap_hogged);
  fprintf(f, "heap_peak %lld\n", heap_peak);
  fprintf(f, "heap_failed %ld\n", heap_failed);
  fprintf(f, "hog_short %lld\n", hog_short);
  fprintf(f, "concurrent_short %ld\n", concurrent_short);
  fprintf(f, "files_open %d\n", files_open);
  fprintf(f, "files_open_peak %d\n", files_open_peak);
  fprintf(f, "files_refused %ld\n", files_refused);
}

/* save17: the files gpSP has open (fopen less fclose; opendir less
 * closedir). --max-files N: an open past N fails (EMFILE), as the OS's
 * table of open files would. */
int __real_fclose(FILE *f);
int __real_closedir(DIR *d);
static int max_files = -1;
static const void *open_handles[512];

static int file_slot_free(void)
{
  if (max_files >= 0 && files_open >= max_files)
  {
    files_refused++;
    errno = EMFILE;
    return 0;
  }
  return 1;
}

static void file_opened(const void *handle)
{
  if (!handle || files_open >= (int)(sizeof(open_handles) / sizeof(open_handles[0])))
    return;
  open_handles[files_open++] = handle;
  if (files_open > files_open_peak)
    files_open_peak = files_open;
}

static void file_closed(const void *handle)
{
  int i;
  for (i = 0; handle && i < files_open; i++)
    if (open_handles[i] == handle)
    {
      open_handles[i] = open_handles[--files_open];
      return;
    }
}

int __wrap_fclose(FILE *f)
{
  unsigned i;
  for (i = 0; i < sizeof(log_open) / sizeof(log_open[0]); i++)
    if (log_open[i].f == f)
    {
      log_buffer_last[log_open[i].log] = log_open[i].size;
      log_open[i].f = NULL;
    }
  file_closed(f);
  return __real_fclose(f);
}

int __real_setvbuf(FILE *f, char *buf, int mode, size_t size);
int __wrap_setvbuf(FILE *f, char *buf, int mode, size_t size)
{
  unsigned i;
  for (i = 0; i < sizeof(log_open) / sizeof(log_open[0]); i++)
    if (log_open[i].f == f)
      log_open[i].size = mode == _IOFBF ? (long)size : 0;
  return __real_setvbuf(f, buf, mode, size);
}

int __wrap_closedir(DIR *d)
{
  file_closed(d);
  return __real_closedir(d);
}

/* EXIT-R2: the link log's writes (gpsp_link.txt.tns opened to write): in
 * all, and as gpSP's quit began (host_link.c's link_cable_shutdown, quit()'s
 * first call): a quit writes it once (EXIT-3), and each write is one of the
 * CX CAS's slow ones. */
FILE *__real_fopen(const char *path, const char *mode);

static const char *os_path(const char *path, char *buf, size_t size);

FILE *__wrap_fopen(const char *path, const char *mode)
{
  char b[1024];
  size_t n = strlen(path);
  FILE *f;
  if (strchr(mode, 'w') && n >= 17 && !strcmp(path + n - 17, "gpsp_link.txt.tns"))
    log_file_writes++;
  if (!strncmp(path, out_dir, strlen(out_dir)))
    return __real_fopen(path, mode);   /* (gpsp-host's own: the report, the pictures) */
  if (!file_slot_free())
    return NULL;
  f = __real_fopen(os_path(path, b, sizeof(b)), mode);
  file_opened(f);
  if (f && strchr(mode, 'w'))
  {
    unsigned i, k;
    for (i = 0; i < BUFFERED_LOGS; i++)
    {
      size_t m = strlen(buffered_logs[i]);
      if (n >= m && !strcmp(path + n - m, buffered_logs[i]) && (n == m || path[n - m - 1] == '/'))
        for (k = 0; k < sizeof(log_open) / sizeof(log_open[0]); k++)
          if (!log_open[k].f)
          {
            log_open[k].f = f;
            log_open[k].log = (int)i;
            log_open[k].size = 0;
            break;
          }
    }
  }
  return f;
}

void host_hw_quit_began(void)
{
  if (log_file_writes_at_quit < 0)
    log_file_writes_at_quit = log_file_writes;
}

size_t __wrap_fwrite(const void *data, size_t size, size_t count, FILE *f)
{
  size_t fit = count;
  if (free_storage >= 0 && size)
  {
    if ((long long)(size * count) > free_storage)
    {
      fit = (size_t)(free_storage / (long long)size);
      if (storage_errno)
        errno = ENOSPC;   /* (--storage-errno 0: the OS says nothing, as Ndless's often doesn't) */
    }
    free_storage -= (long long)(size * fit);
  }
  return fit ? __real_fwrite(data, size, fit, f) : 0;
}

void nspire_file_gone(const char *path);
void nspire_file_list_touched(void);

int __wrap_remove(const char *path)
{
  const char *given = path;
  char b[1024];
  FILE *f = free_storage >= 0 ? fopen(path, "rb") : NULL;
  long size = 0;
  int result;
  path = os_path(path, b, sizeof(b));
  if (f)
  {
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fclose(f);
  }
  result = __real_remove(path);
  if (result == 0 && size > 0)
    free_storage += size;
  if (result == 0)
    nspire_file_gone(given);   /* fast17: as the calculator's nspire_fswrap.c */
  return result;
}

/* The blended pictures cost what turning as many pixels does (roughly so
 * on the calculator, tests/arm and nspire_blend.c). */
void __wrap_nspire_blend_turn_aspect(uint16_t *dst, const uint16_t *src, const uint16_t *prev)
{
  __real_nspire_blend_turn_aspect(dst, src, prev);
  picture_drawn(PIC_ASPECT, src, prev, 0);
  turn_cost(320 * 216);
}

void __wrap_nspire_blend_turn_full(uint16_t *dst, const uint16_t *src, const uint16_t *prev)
{
  __real_nspire_blend_turn_full(dst, src, prev);
  picture_drawn(PIC_FULL, src, prev, 0);
  turn_cost(320 * 240);
}

/* --link-device: what's plugged in, whatever the settings say. */
static int link_device = -1;

void __real_nspire_link_apply_device(void);

void __wrap_nspire_link_apply_device(void)
{
  if (link_device >= 0)
    nspire_link_device = (uint32_t)link_device;
  __real_nspire_link_apply_device();
}

/* The C library's time(), or GPSP_HOST_CLOCK's. */
time_t __real_time(time_t *t);

time_t __wrap_time(time_t *t)
{
  const char *clock = getenv("GPSP_HOST_CLOCK");
  time_t now = clock ? (time_t)strtoll(clock, NULL, 10) : __real_time(NULL);

  if (t)
    *t = now;
  return now;
}

/* ---- Ndless's functions ---- */

static int held(unsigned key)
{
  return (keys[(key - 1) >> 5] >> ((key - 1) & 31)) & 1;
}

/* The arrows and the click are the touchpad's: gpSP masks them out of the
 * keypad registers and reads them here, as on a CX II. */
int touchpad_scan(touchpad_report_t *report)
{
  int up = held(113), right = held(115), down = held(117), left = held(119);
  touchpad_scans++;
  advance(costs[COST_TOUCHPAD]);
  memset(report, 0, sizeof(*report));
  if (held(98))
    report->arrow = TPAD_ARROW_CLICK;
  else if (up && right)
    report->arrow = TPAD_ARROW_UPRIGHT;
  else if (right && down)
    report->arrow = TPAD_ARROW_RIGHTDOWN;
  else if (down && left)
    report->arrow = TPAD_ARROW_DOWNLEFT;
  else if (left && up)
    report->arrow = TPAD_ARROW_LEFTUP;
  else if (up)
    report->arrow = TPAD_ARROW_UP;
  else if (right)
    report->arrow = TPAD_ARROW_RIGHT;
  else if (down)
    report->arrow = TPAD_ARROW_DOWN;
  else if (left)
    report->arrow = TPAD_ARROW_LEFT;
  report->pressed = report->arrow != TPAD_ARROW_NONE;
  return 0;
}

int on_key_pressed(void)
{
  advance(READ_STEP);
  return on_key;
}

void refresh_osscr(void)
{
  osscr_refreshes++;
  osscr_refreshed_lcd_os = lcd_base == (uintptr_t)os_buffer;
}

void cfg_register_fileext(const char *ext, const char *prgm)
{
  size_t len = strlen(registered);
  snprintf(registered + len, sizeof(registered) - len, "%s%s=%s", len ? "," : "", ext, prgm);
}

static char documents_dir[600];

const char *get_documents_dir(void)
{
  return documents_dir;
}

/* --os-cwd: the calculator's own form of paths. Its OS gives the current
 * folder as "\documents\gba\" and takes "\documents\gba\/Game.gbz.tns" as
 * the file in it (the calculators' logs: gpSP joins a name to the folder
 * with a '/'); a path that starts with a backslash is whole. Here the file
 * calls (the linker's --wrap: fopen, stat, opendir, chdir, rename, remove,
 * mkdir, rmdir, open) take a backslash as a '/'. 2026-11-17: the paused
 * menu's other games are such paths, and the start took them for names in
 * the folder ("\documents\gba\/\documents\gba\/Pokemon - Emerald ...gbz.tns"
 * in the CX II's gpsp_states_time.txt.tns: the game didn't start). */
static int os_cwd;

static const char *os_path(const char *path, char *buf, size_t size)
{
  size_t i;
  if (!os_cwd || !path || !strchr(path, '\\'))
    return path;
  for (i = 0; path[i] && i + 1 < size; i++)
    buf[i] = path[i] == '\\' ? '/' : path[i];
  buf[i] = 0;
  return buf;
}

int NU_Current_Dir(const char *drive, char *path)
{
  char here[512];
  size_t i, n;
  (void)drive;
  if (!getcwd(here, sizeof(here) - 2))
    return -1;
  if (!os_cwd)
  {
    snprintf(path, 512, "%s", here);
    return 0;
  }
  n = strlen(here);
  for (i = 0; i < n; i++)
    path[i] = here[i] == '/' ? '\\' : here[i];
  if (!n || path[n - 1] != '\\')
    path[n++] = '\\';
  path[n] = 0;
  return 0;
}

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
int __real_stat(const char *path, struct stat *st);
DIR *__real_opendir(const char *path);
int __real_chdir(const char *path);
int __real_rename(const char *from, const char *to);
int __real_mkdir(const char *path, mode_t mode);
int __real_rmdir(const char *path);
int __real_open(const char *path, int flags, ...);

int __wrap_stat(const char *path, struct stat *st)
{
  char b[1024];
  return __real_stat(os_path(path, b, sizeof(b)), st);
}

DIR *__wrap_opendir(const char *path)
{
  char b[1024];
  DIR *d;
  if (!file_slot_free())
    return NULL;
  d = __real_opendir(os_path(path, b, sizeof(b)));
  file_opened(d);
  return d;
}

int __wrap_chdir(const char *path)
{
  char b[1024];
  return __real_chdir(os_path(path, b, sizeof(b)));
}

int __wrap_rename(const char *from, const char *to)
{
  char b[1024], c[1024];
  int r = __real_rename(os_path(from, b, sizeof(b)), os_path(to, c, sizeof(c)));
  if (r == 0)
    nspire_file_gone(from);   /* fast17: as the calculator's nspire_fswrap.c */
  return r;
}

int __wrap_mkdir(const char *path, mode_t mode)
{
  char b[1024];
  return __real_mkdir(os_path(path, b, sizeof(b)), mode);
}

int __wrap_rmdir(const char *path)
{
  char b[1024];
  int r = __real_rmdir(os_path(path, b, sizeof(b)));
  if (r == 0)
    nspire_file_list_touched();
  return r;
}

int __wrap_open(const char *path, int flags, ...)
{
  char b[1024];
  mode_t mode = 0;
  if (flags & O_CREAT)
  {
    va_list ap;
    va_start(ap, flags);
    mode = (mode_t)va_arg(ap, int);
    va_end(ap);
  }
  return __real_open(os_path(path, b, sizeof(b)), flags, mode);
}

/* ---- Setting up ---- */

/* A key's name in a script: _ for a space ("Pad_A"). */
static int key_name_is(const char *script, const char *name)
{
  for (; *script && *name; script++, name++)
    if (!(*script == '_' && *name == ' ') && tolower((unsigned char)*script) != tolower((unsigned char)*name))
      return 0;
  return !*script && !*name;
}

static unsigned key_number(const char *name)
{
  static const struct { const char *name; unsigned key; } extra[] =
  {
    { "up", 113 }, { "right", 115 }, { "down", 117 }, { "left", 119 }, { "click", 98 }, { "on", 0 },
    { "Pad_Up", 129 }, { "Pad_Down", 130 }, { "Pad_Left", 131 }, { "Pad_Right", 132 },
  };
  unsigned i;
  for (i = 0; i < sizeof(extra) / sizeof(extra[0]); i++)
    if (!strcasecmp(name, extra[i].name))
      return extra[i].key;
  for (i = 1; i <= NUM_KEYS; i++)
    if (nspire_key_strings[i] && key_name_is(name, nspire_key_strings[i]))
      return i;
  fprintf(stderr, "gpsp-host: unknown key %s\n", name);
  exit(2);
}

/* The script's part being read: each "frames" line ends one. */
static int script_part;

static void add_event(double time, enum event_type type, unsigned key, double value, const char *name)
{
  events = realloc(events, sizeof(*events) * (num_events + 1));
  events[num_events].part = script_part;
  events[num_events].time = time;
  events[num_events].type = type;
  events[num_events].key = key;
  events[num_events].value = value;
  snprintf(events[num_events].name, sizeof(events[num_events].name), "%s", name ? name : "");
  num_events++;
}

static int by_time(const void *a, const void *b)
{
  const struct event *x = a, *y = b;
  if (x->part != y->part)
    return x->part < y->part ? -1 : 1;
  if (x->time != y->time)
    return x->time < y->time ? -1 : 1;
  return x < y ? -1 : 1;
}

static void read_script(const char *path)
{
  char line[600], word[64], arg[200], arg2[400];
  double t = 0;
  FILE *f = fopen(path, "r");

  if (!f)
  {
    fprintf(stderr, "gpsp-host: can't read %s\n", path);
    exit(2);
  }
  while (fgets(line, sizeof(line), f))
  {
    int n;
    char *hash = strchr(line, '#');
    if (hash)
      *hash = 0;
    arg[0] = arg2[0] = 0;
    n = sscanf(line, "%63s %199s %399s", word, arg, arg2);
    if (n < 1)
      continue;
    if (!strcmp(word, "wait"))
      t += atof(arg) * TICK_HZ;
    else if (!strcmp(word, "press") || !strcmp(word, "tap"))
    {
      int tap = !strcmp(word, "tap");
      unsigned key = key_number(arg);
      add_event(t, EV_DOWN, key, 0, NULL);
      add_event(t + (tap ? 0.03 : 0.1) * TICK_HZ, EV_UP, key, 0, NULL);
      t += (tap ? 0.1 : 0.25) * TICK_HZ;
    }
    else if (!strcmp(word, "hold"))
      add_event(t, EV_DOWN, key_number(arg), 0, NULL);
    else if (!strcmp(word, "release"))
      add_event(t, EV_UP, key_number(arg), 0, NULL);
    else if (!strcmp(word, "shot"))
      add_event(t, EV_SHOT, 0, 0, arg);
    else if (!strcmp(word, "note"))
      add_event(t, EV_NOTE, 0, 0, arg);
    else if (!strcmp(word, "hog"))
      add_event(t, EV_HOG, 0, atof(arg), NULL);
    else if (!strcmp(word, "frag"))
      add_event(t, EV_FRAG, 0, atof(arg), NULL);
    else if (!strcmp(word, "heapdump"))
      add_event(t, EV_HEAPDUMP, 0, 0, arg);
    else if (!strcmp(word, "frames"))
    {
      /* From this part's time t, the frames; the lines after it are the
       * next part, timed from when they've run. */
      add_event(t, EV_FRAMES, 0, atoi(arg), NULL);
      script_part++;
      t = 0;
    }
    else if (!strcmp(word, "linked"))
    {
      /* "frames N", over as soon as a link is up. */
      add_event(t, EV_FRAMES, 1, atoi(arg), NULL);
      script_part++;
      t = 0;
    }
    else if (!strcmp(word, "measure"))
      add_event(t, EV_MEASURE, strcmp(arg, "stop") != 0, 0, NULL);
    else if (!strcmp(word, "peek"))
    {
      unsigned address, len;
      if (n < 3 || sscanf(arg2, "%x:%u", &address, &len) != 2)
      {
        fprintf(stderr, "gpsp-host: peek NAME ADDR:LEN: %s", line);
        exit(2);
      }
      add_event(t, EV_PEEK, len, address, arg);
    }
    else if (!strcmp(word, "poke"))
    {
      unsigned address, value;
      if (n < 2 || sscanf(arg, "%x:%x", &address, &value) != 2)
      {
        fprintf(stderr, "gpsp-host: poke ADDR:BYTE (hex): %s", line);
        exit(2);
      }
      add_event(t, EV_POKE, value, address, NULL);
    }
    else if (!strcmp(word, "cost") && n >= 3 && !strcmp(arg, "trace"))
    {
      /* Read now (the script's events only switch it on). */
      FILE *tf = fopen(arg2, "r");
      char tl[256];
      double g, d, p;
      if (!tf)
      {
        fprintf(stderr, "gpsp-host: can't read %s\n", arg2);
        exit(2);
      }
      while (fgets(tl, sizeof(tl), tf))
        if (sscanf(tl, "%lf %lf %lf", &g, &d, &p) == 3)
        {
          trace_costs = realloc(trace_costs, (trace_len + 1) * sizeof(*trace_costs));
          trace_costs[trace_len][0] = g * MS;
          trace_costs[trace_len][1] = d * MS;
          trace_costs[trace_len][2] = p * MS;
          trace_len++;
        }
      fclose(tf);
      add_event(t, EV_COST, COSTS, 0, NULL);
    }
    else if (!strcmp(word, "cost"))
    {
      static const char *const kinds[COSTS] = { "frame", "line", "turn", "touchpad", "read", "send",
                                                "receive" };
      unsigned kind;
      for (kind = 0; kind < COSTS && strcmp(arg, kinds[kind]); kind++)
        ;
      if (kind == COSTS || n < 3)
      {
        fprintf(stderr, "gpsp-host: unknown cost: %s", line);
        exit(2);
      }
      /* Lines cost microseconds, the rest milliseconds. */
      add_event(t, EV_COST, kind, atof(arg2) * MS / (kind == COST_LINE ? 1000.0 : 1.0), NULL);
    }
    else if (!strcmp(word, "backlight"))
      backlight = backlight_at_start = (uint32_t)atoi(arg);
    else if (!strcmp(word, "link"))
      add_event(t, EV_LINK, 0, 0, NULL);
    else if (!strcmp(word, "unlink"))
      add_event(t, EV_UNLINK, 0, 0, NULL);
    else if (!strcmp(word, "battery"))
      add_event(t, EV_BATTERY, atoi(arg), strcmp(arg2, "charging") == 0, NULL);   /* battery PERCENT [charging]; -1: not known */
    else if (!strcmp(word, "toast"))
    {
      /* toast TEXT...: the rest of the line, posted as a message (nspire_osd_message) */
      char *rest = strstr(line, "toast") + 5, *end;
      rest += strspn(rest, " \t");
      if ((end = strpbrk(rest, "\r\n")) != NULL)
        *end = 0;
      add_event(t, EV_TOAST, 0, 0, rest);
    }
    else if (!strcmp(word, "quit"))
      add_event(t, EV_QUIT, 0, 0, NULL);
    else if (!strcmp(word, "crash"))
      add_event(t, EV_CRASH, 0, 0, NULL);
    else if (!strcmp(word, "pad"))
    {
      char *rest = strstr(line, "pad") + 3, *end;
      rest += strspn(rest, " \t");
      if ((end = strpbrk(rest, "\r\n")) != NULL)
        *end = 0;
      add_event(t, EV_PAD, 0, 0, rest);
    }
    else if (!strcmp(word, "bt"))
    {
      char *rest = strstr(line, "bt") + 2, *end;
      rest += strspn(rest, " \t");
      if ((end = strpbrk(rest, "\r\n")) != NULL)
        *end = 0;
      add_event(t, EV_BT, 0, 0, rest);
    }
    else
    {
      fprintf(stderr, "gpsp-host: unknown script line: %s", line);
      exit(2);
    }
  }
  fclose(f);
  /* Part by part; stable: events at the same time keep the script's order. */
  qsort(events, num_events, sizeof(*events), by_time);
}

int nspire_os_sleep(void);
int nspire_os_keypad_irq(int on);

static void late_os_user(void)
{
  nspire_os_sleep();
  nspire_os_keypad_irq(0);
}

static void timed_out(int sig)
{
  (void)sig;
  fprintf(stderr, "gpsp-host: no progress (no frame for %d s; stuck at %.2f s of virtual time)\n",
          NO_PROGRESS_S, now / TICK_HZ);
  write_report();
  _exit(3);
}

#ifdef __SANITIZE_ADDRESS__
/* make SAN=1: gui.c's menus are nested functions whose trampolines live on
 * the stack, which ASan's fake stacks (for use-after-return) can't run. */
const char *__asan_default_options(void);
const char *__asan_default_options(void)
{
  return "detect_stack_use_after_return=0";
}
#endif

int main(int argc, char *argv[])
{
  static char exe[600];
  const char *documents = NULL, *link = NULL;
  int player = 0;
  double link_delay = 0;
  int legacy_peer = 0;
  char *args[3];

  for (;;)
  {
    if (argc > 2 && !strcmp(argv[1], "--documents"))
      documents = argv[2];
    else if (argc > 2 && !strcmp(argv[1], "--link"))
      link = argv[2];
    else if (argc > 2 && !strcmp(argv[1], "--player"))
      player = atoi(argv[2]);
    else if (argc > 2 && !strcmp(argv[1], "--model"))
    {
      /* An original CX: its OS leaves the LCD landscape, 320 pixels a line. */
      host_is_cx2 = strcmp(argv[2], "cx") != 0;
      if (!host_is_cx2)
      {
        backlight = backlight_at_start = 0x174;   /* the CX OS's normal contrast */
        lcd_timing[1] = (lcd_timing[1] & ~0x3FFu) | 239;
        lcd_timing[2] = (lcd_timing[2] & ~(0x3FFu << 16)) | (319u << 16);
      }
    }
    else if (argc > 2 && !strcmp(argv[1], "--frame-trace"))
    {
      frame_trace = fopen(argv[2], "w");
      if (!frame_trace)
      {
        fprintf(stderr, "gpsp-host: can't write %s\n", argv[2]);
        return 2;
      }
    }
    else if (argc > 2 && !strcmp(argv[1], "--rom-memory"))
      rom_memory_mb = atoi(argv[2]);
    else if (argc > 2 && !strcmp(argv[1], "--heap-limit"))
      heap_limit = atoll(argv[2]) * 1024;
    else if (argc > 2 && !strcmp(argv[1], "--max-files"))
      max_files = atoi(argv[2]);
    else if (argc > 2 && !strcmp(argv[1], "--concurrent"))
      concurrent_bytes = atoll(argv[2]) * 1024;
    else if (argc > 2 && !strcmp(argv[1], "--storage-errno"))
      storage_errno = atoi(argv[2]);
    else if (argc > 2 && !strcmp(argv[1], "--lcd-timed"))
    {
      lcd_timed = atoi(argv[2]);
      if (lcd_timed)
      {
        lcd_timing[1] = 0x0720013Fu;   /* Ndless's (install.c) */
        lcd_retime();
      }
    }
    else if (argc > 2 && !strcmp(argv[1], "--free-storage"))
      free_storage = atoll(argv[2]) * 1024;
    else if (argc > 2 && !strcmp(argv[1], "--os-cwd"))
      os_cwd = atoi(argv[2]);
    else if (argc > 2 && !strcmp(argv[1], "--timer-rate"))
      timer_rate = atof(argv[2]);
    else if (argc > 2 && !strcmp(argv[1], "--rtc-frozen"))
      rtc_frozen = atoi(argv[2]);
    else if (argc > 2 && !strcmp(argv[1], "--timer-clock-os"))
      timer_clock_os = (uint32_t)strtoul(argv[2], NULL, 16);
    /* R12: the clocks, the OS's USB timer and wait, the OS's first timer */
    else if (argc > 2 && !strcmp(argv[1], "--timer-sel1-hz"))
      sel1_hz = atof(argv[2]);
    else if (argc > 2 && !strcmp(argv[1], "--timer-sel0-hz"))
      sel0_hz = atof(argv[2]);
    else if (argc > 2 && !strcmp(argv[1], "--os-usb-timer"))
      os_usb_load = (uint32_t)strtoul(argv[2], NULL, 16);
    else if (argc > 2 && !strcmp(argv[1], "--os-usb-wait"))
    {
      block_slow = !strcmp(argv[2], "block");
      os_wait_timer1 = !strcmp(argv[2], "timer1");
    }
    else if (argc > 2 && !strcmp(argv[1], "--timer1-os"))
    {
      const char *m = argv[2], *colon = strchr(m, ':');
      uint32_t load = colon ? (uint32_t)strtoul(colon + 1, NULL, 16) : 0;
      if (!strcmp(m, "free"))
        t1.control = 0x82, t1.load = t1.from = 0xFFFFFFFFu;
      else if (!strncmp(m, "periodic16:", 11))
        t1.control = 0xC0, t1.load = t1.from = load & 0xFFFF;
      else if (!strncmp(m, "periodic:", 9))
        t1.control = 0xC2, t1.load = t1.from = load;
      else if (!strcmp(m, "oneshot"))
        t1.control = 0x83, t1.load = t1.from = 0x10000000u;
      else if (!strcmp(m, "reset"))   /* QUIT6: every CX II's (the reset value) */
        t1.control = 0x20, t1.load = 0, t1.from = 0xFFFFFFFFu;
    }
    else if (argc > 1 && !strcmp(argv[1], "--timer1-os-raised"))
    {
      t1_raised = 1;   /* QUIT6: its raw interrupt set as gpSP starts */
      argv--;
      argc++;
    }
    else if (argc > 2 && !strcmp(argv[1], "--timer1-os-jump"))
      t1_jump_at = atof(argv[2]);
    else if (argc > 2 && !strcmp(argv[1], "--timer1-os-stop"))
      t1_stop_at = atof(argv[2]);
    else if (argc > 2 && !strcmp(argv[1], "--timer-dead"))
    {
      timer_dead = atoi(argv[2]);
      if (timer_dead == 3)
        other_timers[0].control = 0xE6;   /* the OS's, running with its interrupt */
    }
    else if (argc > 2 && !strcmp(argv[1], "--link-device"))
      link_device = !strcmp(argv[2], "wireless") ? LINK_DEVICE_WIRELESS :
                    !strcmp(argv[2], "cable") ? LINK_DEVICE_CABLE :
                    !strcmp(argv[2], "exact") ? LINK_DEVICE_EXACT : LINK_DEVICE_AUTO;
    else if (argc > 2 && !strcmp(argv[1], "--link-delay"))
      link_delay = atof(argv[2]);
    else if (argc > 1 && !strcmp(argv[1], "--link-auto"))
    {
      extern int host_link_auto;
      host_link_auto = 1;
      argv--;   /* no value: one argument, not two */
      argc++;
    }
    else if (argc > 2 && !strcmp(argv[1], "--link-write"))
    {
      extern double host_link_write_ms;
      host_link_write_ms = atof(argv[2]);
    }
    else if (argc > 3 && !strcmp(argv[1], "--link-slow"))
    {
      extern double host_link_slow_ms;
      extern int host_link_slow_every;
      host_link_slow_ms = atof(argv[2]);
      host_link_slow_every = atoi(argv[3]);
      argv++;   /* two values: three arguments */
      argc--;
    }
    else if (argc > 1 && !strcmp(argv[1], "--link-hold-time"))
    {
      extern int host_link_hold_time;
      host_link_hold_time = 1;
      argv--;   /* no value */
      argc++;
    }
    else if (argc > 1 && !strcmp(argv[1], "--link-slices"))
    {
      /* R4: a wait ends after its wait_ms with nothing, as the calculator's
       * (the waiting screen's test); without it a wait runs to the message. */
      extern int host_link_slices;
      host_link_slices = 1;
      argv--;   /* no value */
      argc++;
    }
    else if (argc > 1 && !strcmp(argv[1], "--link-nohold"))
    {
      extern int host_link_nohold;
      host_link_nohold = 1;
      argv--;   /* no value */
      argc++;
    }
    else if (argc > 2 && !strcmp(argv[1], "--link-fast"))
    {
      extern int host_link_fast;
      host_link_fast = !strcmp(argv[2], "host") ? 1 : !strcmp(argv[2], "device") ? 2 : !strcmp(argv[2], "noanswer") ? 3 : 0;
    }
    else if (argc > 2 && !strcmp(argv[1], "--link-speed"))
      nspire_link_speed = !strcmp(argv[2], "compatible") ? LINK_SPEED_COMPATIBLE : LINK_SPEED_AUTO;
    else if (argc > 2 && !strcmp(argv[1], "--link-fast-fall"))
    {
      extern int host_link_fast_fall;
      host_link_fast_fall = atoi(argv[2]);
    }
    else if (argc > 2 && !strcmp(argv[1], "--os-tick"))
    {
      extern double host_os_tick;
      host_os_tick = atof(argv[2]) / 1000.0;
    }
    else if (argc > 2 && !strcmp(argv[1], "--os-timer"))
    {
      extern int host_os_timer;
      host_os_timer = atoi(argv[2]);
    }
    else if (argc > 2 && !strcmp(argv[1], "--os-wake"))
    {
      extern double host_os_wake;
      host_os_wake = atof(argv[2]) / 1000.0;
    }
    else if (argc > 2 && !strcmp(argv[1], "--os-jitter"))
    {
      extern double host_os_jitter;
      host_os_jitter = atof(argv[2]) / 1000.0;
    }
    else if (argc > 2 && !strcmp(argv[1], "--os-busy-fast"))
    {
      extern double host_os_busy_fast;
      host_os_busy_fast = atof(argv[2]) / 1000.0;
    }
    else if (argc > 1 && !strcmp(argv[1], "--link-fast-os"))
    {
      extern int host_link_fast_os;
      host_link_fast_os = 1;
      argv--;   /* no value */
      argc++;
    }
    else if (argc > 1 && !strcmp(argv[1], "--link-os"))
    {
      extern int host_link_os;
      host_link_os = 1;
      argv--;   /* no value */
      argc++;
    }
    else if (argc > 2 && !strcmp(argv[1], "--link-peer"))
      legacy_peer = !strcmp(argv[2], "legacy");
    else if (argc > 2 && !strcmp(argv[1], "--link-nonce-tie"))
      host_link_nonce_tie(atoi(argv[2]));
    else if (argc > 2 && !strcmp(argv[1], "--link-trace"))
    {
      if (host_link_trace(argv[2]) != 0)
      {
        fprintf(stderr, "gpsp-host: can't write %s\n", argv[2]);
        return 2;
      }
    }
    else if (argc > 2 && !strcmp(argv[1], "--stand-in"))
    {
      if (nspire_link_try_stand_in(argv[2]) != 0)
      {
        fprintf(stderr, "gpsp-host: --stand-in CODE:MODE, MODE poke, aw1, aw2, fe or exact: %s\n", argv[2]);
        return 2;
      }
    }
    else
      break;
    argv += 2;
    argc -= 2;
  }
  if (argc < 4)
  {
    fprintf(stderr, "usage: gpsp-host [--documents DIR] [--model cx2|cx] [--rom-memory MB] [--free-storage KB] [--heap-limit KB] [--max-files N] [--concurrent KB] [--storage-errno 0] [--os-cwd 1] [--frame-trace FILE] [--timer-rate HZ] [--timer-dead 1|2|3] [--timer-clock-os HEX] [--link SOCKET [--player 1|2] "
            "[--link-device auto|wireless|cable|exact] [--link-delay MS] [--stand-in CODE:MODE] "
            "[--link-trace FILE] [--link-peer legacy] [--link-nonce-tie N] [--link-auto] [--link-os] [--link-fast host|device|noanswer] [--link-fast-fall N] [--link-fast-os] [--os-tick MS] [--os-busy-fast MS] [--link-speed auto|compatible] [--link-write MS] [--link-nohold] [--link-hold-time]] SCRIPT OUTDIR GAMEDIR [ROM]\n");
    return 2;
  }
  /* R11t: the clock selects as the OS has them */
  timer_clock = timer_clock_os;
  other_timers[0].clock = fast_clock_os;
  /* R12: the first timer as the OS has it (counting from now) */
  t1.since = now;
  t1_os_control = t1.control;
  t1_os_load = t1.load;
  /* Before the watchdog: the other gpsp-host may take a while to start. */
  if (link && host_link_setup(link, player, link_delay, legacy_peer) != 0)
  {
    fprintf(stderr, "gpsp-host: no other gpsp-host at %s\n", link);
    return 2;
  }
  read_script(argv[1]);
  if (!realpath(argv[2], out_dir))
  {
    fprintf(stderr, "gpsp-host: no folder %s\n", argv[2]);
    return 2;
  }
  if (chdir(argv[3]) != 0)
  {
    fprintf(stderr, "gpsp-host: no folder %s\n", argv[3]);
    return 2;
  }
  if (!getcwd(exe, sizeof(exe) - 20))
    return 2;
  /* The documents: the game folder's parent (or DIR), with a slash, as
   * Ndless gives it. */
  if (documents)
  {
    static char full[4096];   /* realpath needs PATH_MAX */
    if (!realpath(documents, full) || strlen(full) + 2 > sizeof(documents_dir))
    {
      fprintf(stderr, "gpsp-host: no folder %s\n", documents);
      return 2;
    }
    strcpy(documents_dir, full);
    strcat(documents_dir, "/");
  }
  else
  {
    snprintf(documents_dir, sizeof(documents_dir), "%s", exe);
    if (strrchr(documents_dir, '/'))
      strrchr(documents_dir, '/')[1] = 0;
  }
  strcat(exe, "/_gpSP.tns");
  args[0] = exe;
  args[1] = argc > 4 ? argv[4] : NULL;
  args[2] = NULL;

  memcpy(shown_at_start, (const void *)shown_base, PORTRAIT_BYTES);
  /* GPSP_HOST_OS_CWD=1: --os-cwd 1 for every run (a whole suite on the OS's paths) */
  if (getenv("GPSP_HOST_OS_CWD"))
    os_cwd = atoi(getenv("GPSP_HOST_OS_CWD"));
  if (getenv("GPSP_HOST_PICTURES"))
  {
    char path[4300];
    snprintf(path, sizeof(path), "%s/pictures.txt", out_dir);
    pictures_file = fopen(path, "w");
    /* GPSP_HOST_PICTURES=raw: each one's buffer too, in pictures.bin (to look at a difference) */
    snprintf(path, sizeof(path), "%s/pictures.bin", out_dir);
    if (!strcmp(getenv("GPSP_HOST_PICTURES"), "raw"))
      pictures_raw = fopen(path, "wb");
  }
  /* The LCD's DMA ran dry before gpSP (the OS's time): the first
   * picture line counts from the game's start, so it must say "no". */
  if (getenv("GPSP_HOST_LCD_UNDERFLOW"))
    lcd_raw_irq |= 2;
  atexit(write_report);
  /* EXIT-1's check of the check: something that lets the OS run and masks
   * the keypad line again after gpSP's hand-back (runs after gpSP's own
   * exit handlers): the report must say so (os_after_hand_back). */
  if (getenv("GPSP_HOST_LATE_SLEEP"))
    atexit(late_os_user);
  signal(SIGALRM, timed_out);
  /* A script that hangs fails: NO_PROGRESS_S wall-clock seconds without a
   * game frame (each frame starts it again). Not a limit on the whole run:
   * linked runs trade tens of thousands of messages, each a wakeup of the
   * other gpsp-host, and on a busy PC they can take minutes and still be
   * fine; a link that really hangs stops the frames. */
  alarm(NO_PROGRESS_S);
  return gpsp_main(argc > 4 ? 2 : 1, args);
}
