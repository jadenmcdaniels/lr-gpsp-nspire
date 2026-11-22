"""What Bluetooth costs a game frame, by the speed model (tools/calc_speed/arm926.c):
tests/bt/btbench.c with nspire-bt's library built as gpSP builds it (-Os,
the SBC encoder -O2, ARM926EJ-S, soft float, unsigned char) on nspire-bt's
fake TI OS and scripted controller, cycles a frame by part at the CX II's
396 MHz and the CX's 192 / 132 MHz (tests/calc_speed.py's memory timings,
times 1.05 as it prints).

  python3 -I tests/bt/btbench.py [FRAMES] [SCENARIO...]

Scenarios 6-8 (headphones streaming) play BTBENCH_CLIP (a WAV of the mixer's
output, tests/audio/wav.sh) when it is set, else tones; BT_DIR picks the
nspire-bt tree (another tree's numbers: the same bench on its library).

Needs the Ndless toolchain (arm-none-eabi-gcc), a qemu-arm built with
plugins ($QEMU), ../nspire-bt and its BTstack."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
ARM = os.path.join(ROOT, 'tests', 'arm')
BUILD = os.path.join(ARM, 'build', 'btbench')
NBT = os.path.realpath(os.environ.get('BT_DIR', os.path.join(os.path.dirname(ROOT), 'nspire-bt')))
BTSTACK = os.environ.get('BTSTACK', os.path.join(os.path.dirname(NBT), 'btstack'))
SHIM = os.path.realpath(os.path.join(os.path.dirname(ROOT), 'nspire-usbshim'))
PAD = os.path.realpath(os.path.join(os.path.dirname(ROOT), 'nspire-usbpad-pad'))
QEMU = os.environ.get('QEMU', 'qemu-arm')
CALCS = {'cx2': (396.0, ''), 'cx192': (192.0, 'fill=90,write=5,word=5,writeback=36'),
         'cx': (132.0, 'fill=60,write=3,word=3,writeback=24')}
PARTS = [('fos_ticks', 'os'), ('nusb_hci_service', 'usb'), ('btstack_run_loop_embedded_execute_once', 'runloop'),
         ('nbt_hid_tick', 'pads'), ('nbt_a2dp_tick', 'audio'), ('pad_frame', 'decode')]
# the streaming scenarios' (the model splits 7 regions at most): gpSP's read of the mixer, the library's write
# (resampler, ring), its pull from the ring, SBC, a packet's copies on its way down; the rest of the poll is "cpu"
AUDIO_PARTS = [('fos_ticks', 'os'), ('game_frame', 'game'), ('read_samples', 'read'),
               ('nspire_bt_audio_write', 'write'), ('nspire_bt_audio_write_mix', 'write'), ('nbt_audio_pull', 'pull'), ('nbt_sbc_encode', 'sbc'),
               ('btstack_sbc_encoder_bluedroid_encode_signed_16', 'sbc'), ('send_copies', 'send')]
NOT_COUNTED = ('os', 'game')
SCENARIOS = [(0, 'no dongle (the gate)'), (1, 'no dongle, gate open'), (2, 'dongle in, idle'),
             (3, 'dongle in, a search running'), (4, 'DualSense input, 250/s'), (5, 'Switch Pro input, 60/s'),
             (6, 'headphones, joint stereo bp35'), (7, 'headphones, mono bp35'), (8, 'headphones, a silent game')]
# the A2DP API the streaming scenarios play the sink behind (nspire-bt's test_glue does the same)
WRAP = ['a2dp_source_register_packet_handler', 'a2dp_source_establish_stream', 'a2dp_source_start_stream',
        'a2dp_source_disconnect', 'a2dp_max_media_payload_size', 'a2dp_source_stream_endpoint_request_can_send_now',
        'a2dp_source_stream_send_media_payload_rtp']
# gpSP's library (its Makefile: BT_LE=scan, BT_AVRCP=0)
OPTS = os.environ.get('BT_OPTS', 'NBT_LE_SCAN_ONLY=1 NBT_NO_AVRCP=1').split()
FLAGS = ['-std=gnu99', '-marm', '-mcpu=arm926ej-s', '-msoft-float', '-funsigned-char', '-fno-common',
         '-ffunction-sections', '-fdata-sections', '-DNUSB_OS_TEST', '-w']


def sh(cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def plugin():
    so = os.path.join(ARM, 'build', 'arm926.so')
    src = os.path.join(ROOT, 'tools', 'calc_speed', 'arm926.c')
    if not os.path.exists(so) or os.path.getmtime(src) > os.path.getmtime(so):
        inc = os.path.join(os.path.dirname(os.path.dirname(QEMU)), 'include', 'qemu')
        glib = subprocess.run(['pkg-config', '--cflags', '--libs', 'glib-2.0'], capture_output=True, text=True,
                              check=True).stdout.split()
        os.makedirs(os.path.dirname(so), exist_ok=True)
        sh(['gcc', '-O2', '-shared', '-fPIC', '-I' + inc, src, '-o', so] + glib)
    return so


def sources():
    """profiles.mk's sources and include flags (as nspire-bt's Makefile builds its library)."""
    mk = os.path.join(BUILD, 'print.mk')
    with open(mk, 'w') as f:
        f.write('NBT_ROOT := %s\nBTSTACK := %s\nUSBPAD_DIR := %s\ninclude %s/profiles.mk\n'
                'print-src:\n\t@echo $(NBT_SRC)\nprint-sbc:\n\t@echo $(or $(NBT_O2_SRC),$(NBT_SBC_ENC_SRC))\n'
                'print-cflags:\n\t@echo $(NBT_CFLAGS)\n' % (NBT, BTSTACK, PAD, NBT))
    out = lambda t: subprocess.run(['make', '-s', '-f', mk, t] + OPTS, capture_output=True, text=True,
                                   check=True).stdout.split()
    return out('print-src'), set(out('print-sbc')), out('print-cflags')


def top_functions(elf, model_out, frames, top=14):
    """Each function's own cycles (the model lists every instruction's), a frame, the largest first."""
    out = subprocess.run(['arm-none-eabi-nm', '-n', '-S', elf], capture_output=True, text=True, check=True).stdout
    syms = sorted((int(p[0], 16), int(p[1], 16), p[3]) for p in (l.split() for l in out.split('\n'))
                  if len(p) == 4 and p[2] in 'tTwW')
    starts = [a for a, _, _ in syms]
    import bisect
    acc = {}
    lines = open(model_out).read().split('\n')
    for line in lines[3:]:
        p = line.split()
        if len(p) != 7 or p[0] == 'frame':
            continue
        pc = int(p[0], 16)
        i = bisect.bisect_right(starts, pc) - 1
        name = syms[i][2] if i >= 0 and pc < syms[i][0] + syms[i][1] else '?'
        if name in ('fos_ticks', 'game_frame', 'thrash_code', 'oldest_pending', '?') or name.startswith('fos_') or \
                name.startswith('fctrl'):
            continue
        acc[name] = acc.get(name, 0) + int(p[3])
    best = sorted(acc.items(), key=lambda kv: -kv[1])[:top]
    return ', '.join('%s %.0f' % (fn, c / max(frames, 1)) for fn, c in best)


def main():
    frames = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    only = [int(a) for a in sys.argv[2:]]
    os.makedirs(BUILD, exist_ok=True)
    src, sbc, cflags = sources()
    port = [os.path.join(NBT, p) for p in ('port/bt_port.c', 'port/bt_port_log.c', 'port/bt_port_tlv.c',
                                           'port/bt_port_fw.c', 'port/bt_port_chip.c',
                                           'transport/hci_transport_nspire_usb.c', 'transport/nspire_usb_hci.c',
                                           'tests/transport/fake_os.c', 'tests/transport/fake_ctrl.c')]
    port += [os.path.join(BTSTACK, 'platform', 'embedded', 'btstack_run_loop_embedded.c'),
             os.path.join(SHIM, 'tests', 'newlib_shim.c'), os.path.join(HERE, 'btbench.c')]
    inc = cflags + ['-I' + os.path.join(NBT, d) for d in ('port', 'transport', 'tests/transport', 'profiles')] + \
        ['-I' + SHIM, '-I' + os.path.join(BTSTACK, 'platform', 'embedded'),
         '-I' + os.path.join(BTSTACK, 'chipset', 'realtek')]
    objs = []
    for s in src + port:
        o = os.path.join(BUILD, os.path.basename(s)[:-2] + '.o')
        opt = '-O2' if s in sbc else '-Os'
        sh(['arm-none-eabi-gcc', opt] + FLAGS + inc + ['-c', s, '-o', o])
        objs.append(o)
    rtk = os.path.join(BUILD, 'btstack_chipset_realtek.o')
    sh(['arm-none-eabi-gcc', '-Os'] + FLAGS + inc + ['-include', os.path.join(NBT, 'port', 'bt_port_realtek_glue.h'),
                                                       '-c', os.path.join(BTSTACK, 'chipset', 'realtek',
                                                                          'btstack_chipset_realtek.c'), '-o', rtk])
    elf = os.path.join(BUILD, 'btbench')
    sh(['arm-none-eabi-gcc', '-specs=rdimon.specs', '-Wl,--gc-sections'] + ['-Wl,--wrap=' + w for w in WRAP] + objs +
       [rtk, '-o', elf])
    clip = ''
    if os.environ.get('BTBENCH_CLIP'):
        import wave
        w = wave.open(os.environ['BTBENCH_CLIP'], 'rb')
        clip = os.path.join(BUILD, 'clip.raw')
        with open(clip, 'wb') as f:
            f.write(w.readframes(w.getnframes()))
        clip = 'clip.raw'

    syms = {}
    for line in subprocess.run(['arm-none-eabi-nm', elf], capture_output=True, text=True, check=True).stdout.split('\n'):
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    regions = os.path.join(BUILD, 'regions')
    with open(regions, 'w') as f:
        for fn, name in PARTS:
            f.write('%x %s\n' % (syms[fn], name))
    with open(regions + '-audio', 'w') as f:
        for fn, name in AUDIO_PARTS:
            if fn in syms:
                f.write('%x %s\n' % (syms[fn], name))
    so = plugin()
    os.makedirs(os.path.join(BUILD, 'btdata'), exist_ok=True)
    print('btbench: %d frames a run (17 ms each); cycles a frame, region "os" is the fake OS (not counted)' % frames)
    for sc, label in SCENARIOS:
        if only and sc not in only:
            continue
        for calc in os.environ.get('BTBENCH_CALCS', 'cx2,cx192,cx').split(','):
            mhz, model = CALCS[calc]
            out = os.path.join(BUILD, 'out-%s.model' % calc)
            pa = '%s,mark=%x,regions=%s,out=%s' % (so, syms['perf_frame_mark'], regions + ('-audio' if sc >= 6 else ''),
                                                   out)
            if model:
                pa += ',' + model
            said = subprocess.run([QEMU, '-cpu', 'arm926', '-plugin', pa, elf, str(frames), str(sc)] +
                                  ([clip] if clip and sc >= 6 else []), cwd=BUILD,
                                  capture_output=True, text=True, check=True).stdout.strip().split('\n')[-1]
            lines = open(out).read().split('\n')
            names = lines[2].split()[1:]
            sums = [0] * len(names)
            n = 0
            for line in lines:
                if line.startswith('frame'):
                    v = [int(x) for x in line.split()[1:]]
                    for i in range(len(names)):
                        sums[i] += v[i]
                    n += 1
            per = dict((names[i], sums[i] / max(n, 1)) for i in range(len(names)))
            bt = sum(v for k, v in per.items() if k not in NOT_COUNTED)   # (cpu: the rest of the poll, the glue's gate)
            print('%-30s %-5s %7.0f cycles a frame = %.4f ms (%.3f%% of a 16.7 ms frame)  [%s]' %
                  (label, calc, bt, bt * 1.05 / mhz / 1000, bt * 1.05 / mhz / 1000 / 16.7 * 100,
                   ', '.join('%s %.0f' % kv for kv in sorted(per.items()))))
            if calc == 'cx2':
                print('    (%s)' % said)
                if sc >= 6:
                    print('    by function (self, cycles a frame): %s' % top_functions(elf, out, n))


main()
