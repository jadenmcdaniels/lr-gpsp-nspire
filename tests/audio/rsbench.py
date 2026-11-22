"""What the USB audio path costs a frame besides the mixer, by the speed
model (tools/calc_speed/arm926.c): tests/audio/rsbench.c built with the
driver's pcm.c as gpSP builds it (-O3, ARM926), run under the model, cycles
a frame by part, at the CX II's 396 MHz and the CX's 192 / 132 (the same
memory timings as tests/calc_speed.py's calcs, times 1.05 as it prints).

  python3 -I tests/audio/rsbench.py [FRAMES]

Lines: setting, then each part's cycles and ms a frame."""
import os
import subprocess
import sys

AUDIO = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(AUDIO))
ARM = os.path.join(ROOT, 'tests', 'arm')
BUILD = os.path.join(ARM, 'build', 'rsbench')
UA = os.path.join(os.path.dirname(ROOT), 'nspire-usbaudio-gpsp')
if not os.path.exists(os.path.join(UA, 'src', 'pcm.c')):
    UA = os.path.join(os.path.dirname(ROOT), 'nspire-usbaudio')
QEMU = os.environ.get('QEMU', 'qemu-arm')
CALCS = {'cx2': (396.0, ''), 'cx192': (192.0, 'fill=90,write=5,word=5,writeback=36'),
         'cx': (132.0, 'fill=60,write=3,word=3,writeback=24')}
PARTS = [('rs_run', 'resample'), ('ring_put', 'ring_put'), ('ring_get', 'ring_get'), ('pcm_write', 'pcm_write'),
         ('pcm_silence', 'pcm_write'), ('pkt_next', 'ring_get')]


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


def main():
    frames = int(sys.argv[1]) if len(sys.argv) > 1 else 300
    os.makedirs(BUILD, exist_ok=True)
    elf = os.path.join(BUILD, 'rsbench')
    sh(['arm-none-eabi-gcc', '-O3', '-marm', '-mcpu=arm926ej-s', '-msoft-float', '-funsigned-char',
        '-fomit-frame-pointer', '-DRING_FRAMES=8192', '-I' + os.path.join(UA, 'src'), '-specs=rdimon.specs',
        os.path.join(AUDIO, 'rsbench.c'), os.path.join(UA, 'src', 'pcm.c'),
        os.path.join(UA, 'tests', 'newlib_shim.c'), '-o', elf])   # (the driver's qemu tests' allocator)
    syms = {}
    for line in subprocess.run(['arm-none-eabi-nm', elf], capture_output=True, text=True, check=True).stdout.split('\n'):
        p = line.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    regions = os.path.join(BUILD, 'regions')
    with open(regions, 'w') as f:
        for fn, name in PARTS:
            f.write('%x %s\n' % (syms[fn], name))
    so = plugin()
    print('rsbench: %d frames a run; per GBA frame: 1097 frames at 65536 Hz in, 738 out at 44100, ~16.7 packets'
          % frames)
    for label, args in (('stereo, rate 1', ['0', '100']), ('mono, rate 1', ['1', '100']),
                        ('stereo, rate 0.75 (a slow game)', ['0', '75'])):
        for calc in ('cx2', 'cx192'):
            mhz, model = CALCS[calc]
            out = os.path.join(BUILD, 'out-%s.model' % calc)
            pa = '%s,mark=%x,regions=%s,out=%s' % (so, syms['perf_frame_mark'], regions, out)
            if model:
                pa += ',' + model
            sh([QEMU, '-cpu', 'arm926', '-plugin', pa, elf, str(frames)] + args, cwd=BUILD,
               stdout=subprocess.DEVNULL)
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
            per = [s / max(n, 1) for s in sums]
            total = sum(per)
            parts = ', '.join('%s %.0f' % (names[i], per[i]) for i in range(len(names)))
            print('%-32s %-5s %6.0f cycles a frame = %.3f ms (%s)' % (label, calc, total, total * 1.05 / mhz / 1000,
                                                                         parts))


main()
