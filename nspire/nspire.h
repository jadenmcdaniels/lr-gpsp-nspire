/* Gamepak ROM page-cache allocation (total megabytes). Override before including
 * this header or pass -DROM_BUFFER_SIZE=N on the compiler command line. */
#ifndef ROM_BUFFER_SIZE
#define ROM_BUFFER_SIZE 8
#endif

#define getcwd(path, size) \
	NU_Current_Dir("A:", (char*)(path))
	
#define GetRTC() (*(volatile unsigned*)0x90090000)

#define WOP_D_CLEAN 0
#define WOP_I_INVALIDATE 1

/* RGB565 full LCD back buffer (matches extra_screen_*); D-clean range before display swap. */
#define NSPIRE_LCD_FB_SIZE_BYTES ((unsigned long)(320 * 240 * 2))

/* nspire_screen is the buffer the next frame is drawn into, and
 * nspire_flip() shows it; the buffers take turns (nspire.c). */
extern void* nspire_screen;
extern void* nspire_displayed_screen;
extern u16* os_screen;
/* A landscape 320x240 scratch frame: menus, and the blended scalers on a
 * portrait LCD, draw here before it is turned into nspire_screen. */
extern u16 nspire_landscape[320 * 240];
/* Nonzero on a portrait LCD (CX II): frames go into the display buffers
 * turned, 240 pixels per line, 320 lines, 5:6:5 (see nspire_turn.c). */
extern int nspire_lcd_portrait;
void nspire_init();
void nspire_restore();
void set_display_buffer(void* buffer);
void nspire_flip(void);
/* (r19-fix) Every nspire_flip counted, never reset: a picture held back to
 * flip later (nspire_frameskip.c's fast forward) is the one in nspire_screen
 * only while this hasn't moved since (the buffers' pointer comes round
 * again after three flips). */
extern u32 nspire_flip_serial;
/* Which of the screen buffers nspire_screen is (0, 1 or 2). */
int nspire_screen_index(void);
/* Whether the LCD has started showing the frame nspire_flip gave it last. */
int nspire_flip_taken(void);
/* About how long (timer ticks) until the LCD's next refresh starts. */
u32 nspire_lcd_ticks_to_refresh(void);
/* An LCD refresh in 16.16 timer ticks: as measured at the start (nspire.c lcd_tune, the CX II: a GBA frame), else
 * a 60th of a second. */
u32 nspire_lcd_period16(void);
/* Whether gpSP moved the LCD's refresh to a GBA frame (the CX II: nspire.c lcd_tune). */
int nspire_lcd_at_gba_rate(void);
/* One log line: the LCD's refresh (timed now when timed: up to 0.1 s of
 * busy wait), registers, DMA underflow and the flips since the last line
 * (nspire.c). */
void nspire_lcd_report(char *text, u32 size, int timed);
/* Clears the DMA underflow bit and the flip counts: the next line counts
 * from here (a game's start). */
void nspire_lcd_report_restart(void);
/* Waits for the LCD's next refresh (at most 1/20 s); menus run at its pace. */
void update_at_vblank();

/* A free-running 32768 Hz tick counter (an SP804 timer: nspire.c). */
#define NSPIRE_TICK_HZ 32768u
u32 nspire_ticks(void);
/* Whether a timer runs: if none does, nspire_ticks only moves as frames
 * are paced (nspire_wait_until), so waits must be timed some other way. */
int nspire_timer_runs(void);
/* A line about the timer's speed (nspire_tickrate.h), for the speed test
 * results and the perf log. */
void nspire_timer_report(char *text, u32 size);
/* EXIT-3: the timer lines for the clean-exit marker (R11t: the clock
 * select's line on both models, an older CX's timing lines after it);
 * EXIT-1: the OS's timer settings back (the exit's last steps). */
void nspire_timer_log_text(char *text, u32 size);
void nspire_timer_hand_back(void);
/* R11t: one line on the timer block's shared clock select: the OS's value,
 * gpSP's write (its own timer's field only), the block's other timer's count
 * before and after it, and after the exit's put-back that too. Register
 * reads only. Returns 0 while no timer was tried yet (nspire_init). */
int nspire_timer_clock_text(char *text, u32 size);
/* R12: the timer's log lines (the pad log's and the link log's headers, the
 * exit marker): line 0 is nspire_timer_clock_text's; on a CX II, 1 the time
 * base (its mode and why, the speeds measured at start and in use, each
 * refinement, jumps), 2 and 3 the timer blocks 900C0000 and 900D0000 as
 * found before any write and their timers' speeds. Returns 0 past the last. */
int nspire_timer_header_text(int line, char *text, u32 size);
/* R12: the timer block at a USB come-up's step, for the pad log (six words:
 * nspire_padlog.h PL_TIMER); counts 900C0020 for ~5 ms when it runs. */
void nspire_timer_usb_probe(uint32_t w[6]);
/* R12: a CX II's fallback (R11t's 32 kHz clock select, when the OS runs the
 * first timer in a way that can't be followed): the timer block as the OS
 * had it for a come-up's burst, gpSP's time meanwhile by OS sleeps and the
 * real-time clock; then taken again, the time going on. Nothing otherwise. */
void nspire_timer_lend(void);
void nspire_timer_retake(void);
/* Times the timer against the real-time clock again (after a clock switch
 * on an older CX, which may change its speed; R12: on a CX II too). */
void nspire_timer_recheck(void);
/* The real-time clock's seconds (the FPS counter counts real seconds). */
u32 nspire_rtc_seconds(void);
/* The timer's own count register (it counts down), or NULL without a timer:
 * read with no call, so the link's NavNet callback (an OS task) can time
 * its writes by it. And a count's time in us. */
extern volatile const u32 *nspire_raw_timer;
u32 nspire_raw_timer_us(u32 counts);
void nspire_wait_until(u32 tick);

/* Coherent LCD framebuffer before triple-buffer swap (range vs full D-cache clean). */
void nspire_lcd_fb_cache_clean_for_display(void);

int warm_cache_op_range(int op, void *addr, unsigned long size);
void clean_dcache(void);
void nspire_jit_cache_sync(void *base, void *end);
/* Makes code written as data anywhere runnable (the menu's trampolines). */
void nspire_sync_written_code(void);

struct tm* localtime(const time_t*);

void upscale_aspect(u16 *dst, u16 *src);
void upscale_aspect_fast(u16 *dst, u16 *src);
void upscale_aspect_raw(u16 *dst, u16 *src);
/* Fused libretro RGB565 repack + raw upscale (no nspire_upscale_src pass). */
void upscale_aspect_raw_from_gba(u16 *dst, const u16 *gba);

/* nspire_turn.c. dst is a 240x320 portrait display buffer; landscape pixel
 * (x, y) goes to position 239 - y of line x. */
void nspire_turn_raw(u16 *dst, const u16 *src, u32 src_pitch);
void nspire_turn_rect_555(u16 *dst, const u16 *src, u32 src_pitch,
                          u32 x0, u32 y0, u32 w, u32 h);

void enableAlignmentExceptions();
u32 menu_wrapper(u16*);
u32 save_backup_wrapper(char*);
void save_config_file_wrapper();
void load_state_wrapper(u8*);
void save_state_wrapper(u8*, u16*);
void quit_wrapper();

u32 key_scan();
