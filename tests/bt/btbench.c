/* What Bluetooth costs a game frame on the calculator's CPU, by the speed
 * model (tools/calc_speed/arm926.c; tests/bt/btbench.py runs it): nspire-bt's
 * library (the profiles, BTstack, the calculator port and its USB
 * transport) built as for gpSP (-Os, the SBC encoder -O2, ARM926), on
 * nspire-bt's fake TI OS and scripted controller (tests/transport: the
 * dongle's USB side and its HCI answers), with each frame doing what gpSP's
 * glue does (nspire_bt_glue.c):
 *
 *   btbench FRAMES SCENARIO
 *   0  Bluetooth on, no dongle: the per-frame sites' gate (NSPIRE_BT_IDLE:
 *      the port's idle word and the glue's, read and compared), 3 sites
 *   1  the same with the gate open (what a poll would cost: for comparison)
 *   2  a dongle in (a CSR one, no firmware), Bluetooth on and idle: each
 *      frame the bus runs a frame's time (the fake OS: not counted, region
 *      "os"), the interrupt window, the glue's poll (nspire_bt_poll, region
 *      "bt"), the pads' keys
 *   3  as 2 with an inquiry running (events coming in: a search)
 *   4  a DualSense's input path, the profiles' part: 4 Bluetooth 0x31 reports
 *      a frame (250 a second at 60 frames), each CRC-checked, decoded and
 *      published into the slot's state (nbt_pad_input + nbt_publish)
 *   5  a Switch Pro Controller's: one 0x30 report a frame (~60 a second)
 *   6  headphones streaming, joint stereo bitpool 35 (the CX II's): each
 *      frame what gpSP does with the game's sound (nspire_audio.c's
 *      frame_end with the headphones taking it: the mixer's 1097 frames read,
 *      cleared and clamped as sound_read_samples does, in chunks of 512, into
 *      nspire_bt_audio_write) and its OS-time slot (the window, nspire_bt_poll:
 *      the media clock's blocks pulled from the ring, SBC, packets). The A2DP
 *      API is wrapped (btbench.py links with --wrap, as nspire-bt's test_glue):
 *      a sink that takes every packet at once; a packet's way down (BTstack's
 *      L2CAP buffer, the transport's OUT ring, its transfer buffer) is three
 *      copies of it here (region "send"); the OS's own USB time is not in it.
 *      Both caches are thrown out between frames (the emulator's frame: region
 *      "game", not counted).
 *   7  the same, mono bitpool 35 (the CX's)
 *   8  as 6 with a silent game (the cached silent frame)
 *   (6-8: argv[3], a raw file of 16-bit stereo at 65536 Hz, the mixer's
 *   output: tests/audio/wav.sh's clip without its header; else tones)
 * perf_frame_mark() at each frame's start; the regions file names
 * nspire_bt_poll "bt" and fos_ticks "os". */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nspire_bt.h"
#include "bt_port.h"
#include "fake_os.h"
#include "fake_ctrl.h"
#include "nbt_internal.h"

#include <sys/types.h>
/* (newlib's semihosting has no mkdir: the port makes its folder) */
int mkdir(const char *path, mode_t mode);
int mkdir(const char *path, mode_t mode)
{
    (void) path;
    (void) mode;
    return 0;
}

static uint32_t now = 1000;
static uint32_t clock_ms(void) { return now; }
static void sleep_tick(void) { now += 10; fos_ticks(10); }

void __attribute__((noinline)) perf_frame_mark(void)
{
    __asm__ volatile("" ::: "memory");
}

/* ---- scenarios 6-8: headphones streaming ---- */
#include "btstack.h"

static btstack_packet_handler_t a2dp_cb;
static uint16_t a2dp_cid = 0x41;
static uint8_t l2cap_buf[1100], out_ring[1100], out_buf[1100];
static uint32_t n_packets, n_frames_sent, ts_gaps, last_ts;

void __wrap_a2dp_source_register_packet_handler(btstack_packet_handler_t cb);
uint8_t __wrap_a2dp_source_establish_stream(bd_addr_t a, uint16_t *cid);
uint8_t __wrap_a2dp_source_start_stream(uint16_t cid, uint8_t seid);
uint8_t __wrap_a2dp_source_disconnect(uint16_t cid);
int __wrap_a2dp_max_media_payload_size(uint16_t cid, uint8_t seid);
void __wrap_a2dp_source_stream_endpoint_request_can_send_now(uint16_t cid, uint8_t seid);
uint8_t __wrap_a2dp_source_stream_send_media_payload_rtp(uint16_t cid, uint8_t seid, uint8_t marker, uint32_t ts,
                                                         uint8_t *payload, uint16_t size);

void __wrap_a2dp_source_register_packet_handler(btstack_packet_handler_t cb) { a2dp_cb = cb; }
uint8_t __wrap_a2dp_source_establish_stream(bd_addr_t a, uint16_t *cid) { (void) a; *cid = a2dp_cid; return 0; }
uint8_t __wrap_a2dp_source_start_stream(uint16_t cid, uint8_t seid) { (void) cid; (void) seid; return 0; }
uint8_t __wrap_a2dp_source_disconnect(uint16_t cid) { (void) cid; return 0; }
int __wrap_a2dp_max_media_payload_size(uint16_t cid, uint8_t seid) { (void) cid; (void) seid; return 679 - 12; }

static void a2dp_ev(uint8_t sub, uint8_t *e, unsigned n)
{
    e[0] = HCI_EVENT_A2DP_META;
    e[1] = (uint8_t) (n - 2);
    e[2] = sub;
    little_endian_store_16(e, 3, a2dp_cid);
    a2dp_cb(HCI_EVENT_PACKET, 0, e, (uint16_t) n);
}

void __wrap_a2dp_source_stream_endpoint_request_can_send_now(uint16_t cid, uint8_t seid)
{
    uint8_t e[7];
    (void) cid; (void) seid;
    memset(e, 0, sizeof e);
    a2dp_ev(A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW, e, sizeof e);   /* (the link has room) */
}

/* the packet's way down: three copies (BTstack's L2CAP buffer with the
 * headers, the transport's OUT ring, the transfer's buffer) */
static void __attribute__((noinline, noclone)) send_copies(const uint8_t *payload, unsigned size)
{
    memcpy(l2cap_buf + 4 + 4 + 12, payload, size);
    memcpy(out_ring, l2cap_buf, size + 20);
    memcpy(out_buf, out_ring, size + 20);
}

uint8_t __wrap_a2dp_source_stream_send_media_payload_rtp(uint16_t cid, uint8_t seid, uint8_t marker, uint32_t ts,
                                                         uint8_t *payload, uint16_t size)
{
    (void) cid; (void) seid; (void) marker;
    if (n_packets && ts != last_ts + 128u * (out_buf[20] & 0x0F))
        ts_gaps++;
    send_copies(payload, size);
    last_ts = ts;
    n_packets++;
    n_frames_sent += payload[0] & 0x0F;
    return 0;
}

static void headphones_in(int mono)
{
    uint8_t e[18];
    static const uint8_t addr[6] = { 0xAA, 0xBB, 0xCC, 0x00, 0x00, 0x01 };
    memset(e, 0, sizeof e);
    reverse_bd_addr(addr, e + 5);
    e[13] = 0;
    a2dp_ev(A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED, e, 14);
    memset(e, 0, sizeof e);
    e[5] = 1;
    e[6] = AVDTP_AUDIO;
    e[7] = 0x0F; e[8] = 0x0F; e[9] = 0x0F; e[10] = 0x03; e[11] = 0x03; e[12] = 2; e[13] = 53;
    a2dp_ev(A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CAPABILITY, e, 14);
    memset(e, 0, sizeof e);
    e[5] = 1;
    e[6] = 1;
    e[8] = AVDTP_AUDIO;
    little_endian_store_16(e, 9, 44100);
    e[11] = mono ? AVDTP_CHANNEL_MODE_MONO : AVDTP_CHANNEL_MODE_JOINT_STEREO;
    e[12] = mono ? 1 : 2;
    e[13] = 16; e[14] = 8; e[15] = 1 /* loudness (AVDTP 1-based) */; e[16] = 2; e[17] = 53;
    a2dp_ev(A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION, e, 18);
    memset(e, 0, sizeof e);
    reverse_bd_addr(addr, e + 5);
    e[11] = 1;
    e[12] = 1;
    a2dp_ev(A2DP_SUBEVENT_STREAM_ESTABLISHED, e, 14);
    memset(e, 0, sizeof e);
    e[5] = 1;
    a2dp_ev(A2DP_SUBEVENT_STREAM_STARTED, e, 6);
}

/* the mixer's buffer (gpSP's sound_buffer: 12-bit samples summed by the
 * channels) and sound_read_samples's loop: read, cleared, clamped, times 16 */
#define MIX_SIZE 65536
static int16_t mixbuf[MIX_SIZE] __attribute__((aligned(4)));
static uint32_t mix_base, mix_end;
static int16_t *clip;
static unsigned clip_frames, clip_at;

static uint32_t __attribute__((noinline, noclone, unused)) read_samples(int16_t *out, uint32_t frames)
{
    uint32_t i, avail = ((mix_end - mix_base) & (MIX_SIZE - 1)) & ~1u, n = frames * 2 < avail ? frames * 2 : avail;
    for (i = 0; i < n; i++) {
        uint32_t at = (mix_base + i) & (MIX_SIZE - 1);
        int32_t v = mixbuf[at];
        mixbuf[at] = 0;
        if (v > 2047)
            v = 2047;
        if (v < -2048)
            v = -2048;
        out[i] = (int16_t) (v * 16);
    }
    mix_base = (mix_base + n) & (MIX_SIZE - 1);
    return n / 2;
}

/* a game frame's sound into the mixer's buffer (not counted: the core's) */
static void mix_frame(unsigned frames, int silent)
{
    unsigned i;
    for (i = 0; i < frames; i++) {
        int16_t l = 0, r = 0;
        if (!silent && clip_frames) {
            l = (int16_t) (clip[2 * clip_at] / 16);
            r = (int16_t) (clip[2 * clip_at + 1] / 16);
            clip_at = clip_at + 1 < clip_frames ? clip_at + 1 : 0;
        } else if (!silent) {
            static uint32_t ph[4], seed = 7;
            ph[0] += 440u * 65536u; ph[1] += 1250u * 65536u; ph[2] += 220u * 65536u; ph[3] += 3300u * 65536u;
            seed = seed * 1664525u + 1013904223u;
            l = (int16_t) (((int32_t) ph[0] >> 22) + ((int32_t) ph[1] >> 23) + ((ph[2] >> 31) ? 300 : -300) +
                           ((int32_t) seed >> 27));
            r = (int16_t) (((int32_t) ph[3] >> 23) + ((int32_t) ph[1] >> 22) + ((ph[2] >> 31) ? -250 : 250) +
                           ((int32_t) seed >> 27));
        }
        mixbuf[mix_end & (MIX_SIZE - 1)] = l;
        mixbuf[(mix_end + 1) & (MIX_SIZE - 1)] = r;
        mix_end = (mix_end + 2) & (MIX_SIZE - 1);
    }
}

/* the emulator's frame between two of ours: its sound into the mixer's
 * buffer, both caches thrown out (region "game", not counted) */
static uint32_t thrash_data[16384 / 4];
void thrash_code(void);
__asm__(".text\n .align 5\n .global thrash_code\n .type thrash_code, %function\nthrash_code:\n"
        " .rept 4096\n nop\n .endr\n bx lr\n");             /* 16 KB of code run: the I-cache's lines */
static void __attribute__((noinline, noclone)) game_frame(unsigned frames, int silent)
{
    unsigned i;
    mix_frame(frames, silent);
    thrash_code();
    /* 16 KB read and written a line at a time: the data cache (8 KB) holds
     * none of ours after it, all its lines dirty */
    for (i = 0; i < sizeof thrash_data / 4; i += 8)
        thrash_data[i]++;
}

/* nspire_audio.c frame_end with the headphones taking the sound: since
 * bt10 the mixer read in place, in one pass, by the library
 * (nspire_bt_audio_write_mix); before it (a library without that call,
 * BT_DIR's older trees) sound_read_samples's copy in chunks of 512, each
 * into nspire_bt_audio_write */
#ifdef NSPIRE_BT_MIX_CLEAR
static void __attribute__((noinline)) frame_end(void)
{
    uint32_t n = ((mix_end - mix_base) & (MIX_SIZE - 1)) >> 1, first = (MIX_SIZE - mix_base) >> 1;
    uint32_t *w = (uint32_t *) (void *) mixbuf;
    if (n > 8 * 512)
        n = 8 * 512;
    if (n <= first)
        nspire_bt_audio_write_mix(w + (mix_base >> 1), n, NULL, 0, NSPIRE_BT_MIX_CLEAR);
    else
        nspire_bt_audio_write_mix(w + (mix_base >> 1), first, w, n - first, NSPIRE_BT_MIX_CLEAR);
    mix_base = (mix_base + 2 * n) & (MIX_SIZE - 1);
}
#else
static void __attribute__((noinline)) frame_end(void)
{
    static int16_t in[512 * 2];
    unsigned chunk;
    for (chunk = 0; chunk < 8; chunk++) {
        unsigned n = read_samples(in, 512);
        if (!n)
            break;
        nspire_bt_audio_write(in, n);
    }
}
#endif

/* gpSP's gate (nspire_bt_glue.h NSPIRE_BT_IDLE): two words, one compare */
static const volatile uint32_t zero_word;
static const volatile uint32_t *wake = &zero_word;
static volatile unsigned live;
static volatile uint32_t sink;

static struct nbt_pad pad;
static struct usbpad_config pcfg = { 1, 50, 25 };
static uint8_t report[80];
static unsigned report_len;

static uint8_t reports[4][80];     /* (made before the frames: their CRCs are the pad's work, not ours) */

static void make_reports(void)
{
    int k;
    for (k = 0; k < 4; k++) {
        memcpy(reports[k], report, sizeof report);
        reports[k][10] = (uint8_t) k;                       /* (a button moving: not the same bytes each time) */
        if (pad.proto == NBT_PROTO_DS)
            nbt_put_le32(reports[k] + 74, nbt_ps_crc(0xA1, reports[k], 74));
    }
}

static void __attribute__((noinline)) pad_frame(int n)
{
    struct usbpad_state st;
    struct nbt_publish_held h = { 0, 0 };
    int i;
    for (i = 0; i < n; i++) {
        nbt_pad_input(&pad, reports[i & 3], report_len, now);
        nbt_publish(&pad.in, &pcfg, &h, &st);
        sink += st.buttons;
    }
}

static void __attribute__((noinline)) glue_frame(int open)
{
    unsigned flags = 0;
    int i;
    uint32_t keys = 0;
    /* keys_poll's site */
    if (open || (*wake | live)) {
        flags = (unsigned) nspire_bt_poll();
        if (nspire_bt_pad_count())
            for (i = 0; i < NSPIRE_BT_PADS; i++)
                if (nspire_bt_pad(i)->connected)
                    keys |= nspire_bt_pad(i)->buttons;
        if (flags & NSPIRE_BT_EVENTS) {
            struct nspire_bt_event ev;
            while (nspire_bt_next_event(&ev))
                ;
        }
    }
    /* the pacing's and the sound's sites */
    if (live || open)
        sink += flags;
    if (live || open)
        sink += keys;
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 300, scenario = argc > 2 ? atoi(argv[2]) : 0, i;
    bt_port_config_t cfg;

    fos_reset();
    fctrl_reset();
    fctrl_attach();
    fos.vid = 0x0A12;
    fos.pid = 0x0001;
    fc.realtek = 0;
    fc.manufacturer = 10;
    fc.inquiry_results = 40;
    memset(&cfg, 0, sizeof cfg);
    cfg.now_ms = clock_ms;
    cfg.sleep_tick = sleep_tick;
    cfg.data_dir = "btdata";
    cfg.log_level = 1;
    if (scenario == 4 || scenario == 5) {
        /* the profiles' input path alone (no dongle, no BTstack around it) */
        memset(report, 0, sizeof report);
        if (scenario == 4) {
            nbt_pad_init(&pad, NBT_PROTO_DS, 0, 0, 0, NULL, 0, "DualSense Wireless Controller");
            report[0] = 0x31;
            report[1] = 0x10;
            report[2] = report[3] = report[4] = report[5] = 0x80;   /* sticks centred */
            report[9] = 0x08;                                       /* hat released */
            report_len = 78;
        } else {
            nbt_pad_init(&pad, NBT_PROTO_SWITCH, 0, 0, 0, NULL, 0, "Pro Controller");
            report[0] = 0x30;
            report[6] = 0x00; report[7] = 0x08; report[8] = 0x80;   /* left stick centred (12-bit) */
            report[9] = 0x00; report[10] = 0x08; report[11] = 0x80;
            report_len = 49;
        }
        make_reports();
        for (i = 0; i < 10; i++)
            pad_frame(1);
        for (i = 0; i < frames; i++) {
            perf_frame_mark();
            pad_frame(scenario == 4 ? 4 : 1);
            now += 17;
        }
        perf_frame_mark();
        printf("btbench: scenario %d, %d frames, %lu reports decoded\n", scenario, frames, (unsigned long) pad.reports);
        return 0;
    }
    if (scenario >= 6) {
        /* headphones streaming */
        uint32_t now_us = now * 1000u;
        int silent = scenario == 8;
        if (argc > 3) {
            FILE *f = fopen(argv[3], "rb");
            if (f) {
                static int16_t data[65536 * 2 * 11];
                clip_frames = (unsigned) fread(data, 4, sizeof data / 4, f);
                fclose(f);
                clip = data;
            }
        }
        if (nspire_bt_start(&cfg) != 0) {
            printf("btbench: start failed\n");
            return 1;
        }
        wake = bt_port_idle_word();
        fos_plug();
        for (i = 0; i < 4000 && nspire_bt_state() != NSPIRE_BT_ON; i++) {
            fos_ticks(2);
            now += 2;
            nspire_bt_poll();
        }
        if (nspire_bt_state() != NSPIRE_BT_ON) {
            printf("btbench: the dongle didn't come up (state %d)\n", nspire_bt_state());
            return 1;
        }
        nspire_bt_audio_set_input_rate(65536);
        nspire_bt_audio_set_codec(scenario == 7, 0);
        headphones_in(scenario == 7);
        if (!nspire_bt_audio_active()) {
            printf("btbench: the headphones aren't streaming\n");
            return 1;
        }
        now_us = now * 1000u;
        for (i = -120; i < frames; i++) {       /* (two seconds to settle: the ring primed, the rate held) */
            unsigned in_frames;
            if (i >= 0)
                perf_frame_mark();
            fos_ticks(17);
            /* frame_begin; the game's frame (its sound into the mixer's buffer); frame_end; the next frame's
             * OS-time slot: the window and the poll */
            nspire_bt_audio_frame_begin(1);
            in_frames = (unsigned) ((uint64_t) (i + 121) * 65536u * 10000u / 597275u -
                                    (uint64_t) (i + 120) * 65536u * 10000u / 597275u);
            game_frame(in_frames, silent);
            frame_end();
            now_us += 16743;
            now = now_us / 1000u;
            nspire_bt_poll();
        }
        perf_frame_mark();
        {
            struct nspire_bt_audio_stats st;
            nspire_bt_audio_stats(&st);
            printf("btbench: scenario %d, %d frames, %lu packets (%.2f a frame, %.1f SBC frames each), "
                   "ts gaps %lu, ring %lu ms, under %lu, late %lu, dropped %lu, speed %+.3f%%%s\n", scenario, frames,
                   (unsigned long) n_packets, (double) n_packets / (frames + 120),
                   n_packets ? (double) n_frames_sent / n_packets : 0.0, (unsigned long) ts_gaps,
                   (unsigned long) st.ring_ms, (unsigned long) st.underruns, (unsigned long) st.late,
                   (unsigned long) st.dropped, (st.speed - 65536.0) * 100.0 / 65536.0,
                   clip_frames ? ", the clip's sound" : ", tones");
        }
        return 0;
    }
    if (nspire_bt_start(&cfg) != 0) {
        printf("btbench: start failed\n");
        return 1;
    }
    wake = bt_port_idle_word();
    if (scenario >= 2) {
        fos_plug();
        for (i = 0; i < 4000 && nspire_bt_state() != NSPIRE_BT_ON; i++) {
            fos_ticks(2);
            now += 2;
            nspire_bt_poll();
        }
        if (nspire_bt_state() != NSPIRE_BT_ON) {
            printf("btbench: the dongle didn't come up (state %d)\n", nspire_bt_state());
            return 1;
        }
        live = 1;
        if (scenario == 3)
            nspire_bt_scan_start(30);
        /* (settle) */
        for (i = 0; i < 60; i++) {
            fos_ticks(17);
            now += 17;
            nspire_bt_poll();
        }
    }
    {
        bt_port_info_t in0, in1;
        bt_port_info(&in0);
        for (i = 0; i < frames; i++) {
            perf_frame_mark();
            if (scenario >= 2)
                fos_ticks(17);      /* the bus for a frame (its completions: the window) */
            now += 17;
            glue_frame(scenario == 1);
        }
        perf_frame_mark();
        bt_port_info(&in1);
        printf("btbench: scenario %d, %d frames, state %d, %d found, %lu events (%.2f a frame), idle word %lu\n",
               scenario, frames, nspire_bt_state(), nspire_bt_scan_count(), (unsigned long) (in1.events_in - in0.events_in),
               (double) (in1.events_in - in0.events_in) / frames, (unsigned long) *wake);
    }
    return 0;
}
