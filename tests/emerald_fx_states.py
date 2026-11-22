#!/usr/bin/env python3
"""Makes the effect scenes' Emerald states (tests/calc_speed.py SCENES rain,
storm, sand, ash, surf, dive, fog) from the reference battery save.

    python3 -I tests/emerald_fx_states.py [SAVE] [NAME ...]

SAVE: an Emerald (USA) battery save (.sav, .sav.gz or .sav.gz.tns; default
a CX II's of 2026-11-21). For each place
below, tests/emerald_warp.py makes a save that continues there (the game's
own warp: the map's weather and OnTransition script, surfing on water,
diving on an underwater map; a Repel's 250 steps and no PokeNav calls, so that no
wild Pokemon or call stops the walks); tests/arm/build/perf plays it from power-on
(Start, Start, A, Start, A: the title, Continue) under qemu-arm without the
speed model, and writes the machine's state at frame FRAMES into
$GBA_GAMES/emerald_fx_NAME.svs.tns (GPSZ, zlib), and the screen at that
frame next to it (.ppm) to look at. Build the model first
(make -C tests/arm build/perf). The RTC reads the host's clock, so a state
made another day can differ in its seconds; the scenes use the files made
once (2026-11-21), not remade ones.

Places (pokeemerald data/maps: map group, number, x, y; weatherCycleStage):
  rain   Route 119 (0, 34) at 25,32, cycle stage 3: rain (SetRoute119Weather; north of
         y 13 is the Weather Institute's sun)
  storm  Route 119 the same, stage 2: thunderstorm (lightning: whole-screen flashes)
  sand   Route 111 (0, 26) at 28,88: the desert, sandstorm (Route111_OnTransition)
  ash    Route 113 (0, 28) at 35,13: volcanic ash
  surf   Route 124 (0, 39) at 20,24: open sea, surfing
  dive   Underwater Route 124 (0, 50) at 41,5: underwater, bubbles (no seaweed on the walk)
  fog    Mt Pyre's summit (24, 22) at 23,29: horizontal fog (the map's weather)
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARM = os.path.join(ROOT, 'tests', 'arm')
QEMU = os.environ.get('QEMU', 'qemu-arm')
GAMES = os.environ.get('GBA_GAMES', '')
EMERALD = os.path.join(GAMES, 'Pokemon - Emerald Version (USA, Europe).gba')
SAVE = os.environ.get('EMERALD_SAVE', '')
CONTINUE = '240 250 START\n420 430 START\n600 610 A\n780 790 START\n960 970 A\n'
FRAMES = 1500

PLACES = {
    'rain': (0, 34, 25, 32, 3),
    'storm': (0, 34, 25, 32, 2),
    'sand': (0, 26, 28, 88, None),
    'ash': (0, 28, 35, 13, None),
    'surf': (0, 39, 20, 24, None),
    'dive': (0, 50, 41, 5, None),
    'fog': (24, 22, 23, 29, None),
}


def make(name, save, tmp):
    group, num, x, y, stage = PLACES[name]
    sav = os.path.join(tmp, name + '.sav')
    args = [sys.executable, '-I', os.path.join(ROOT, 'tests', 'emerald_warp.py'), save, sav,
            str(group), str(num), str(x), str(y), 'repel=250', 'nocall=1'] + (['stage=%d' % stage] if stage is not None else [])
    subprocess.run(args, check=True)
    keys = os.path.join(tmp, 'continue.keys')
    with open(keys, 'w') as f:
        f.write(CONTINUE)
    # semihosting passes a short command line only: links with short names
    links = {}
    for short, target in (('e.gba', EMERALD), ('b.bin', os.path.join(GAMES, 'gba_bios.bin')),
                          ('perf', os.path.join(ARM, 'build', 'perf'))):
        links[short] = os.path.join(tmp, short)
        if not os.path.exists(links[short]):
            os.symlink(target, links[short])
    ppm = os.path.join(tmp, name)
    os.makedirs(ppm, exist_ok=True)
    raw = os.path.join(tmp, name + '.raw')
    subprocess.run([QEMU, '-cpu', 'arm926', 'perf', 'e.gba', 'b.bin', '-', str(FRAMES), 'continue.keys', '1',
                    'sav=%s.sav' % name, 'state=%s.raw' % name, 'ppm=%s' % name, 'every=%d' % FRAMES],
                   cwd=tmp, check=True, stdout=subprocess.DEVNULL)
    data = open(raw, 'rb').read()
    packed = zlib.compress(data, 9)
    out = os.path.join(GAMES, 'emerald_fx_%s.svs.tns' % name)
    with open(out, 'wb') as f:
        f.write(b'GPSZ' + struct.pack('<7I', 0, 0, len(data), len(packed), 0, 0, 0) + packed)
    shutil.move(os.path.join(ppm, 's%05d.ppm' % FRAMES), out[:-len('.svs.tns')] + '.ppm')
    print('%s: %s (%d bytes)' % (name, out, 32 + len(packed)))


def main():
    args = sys.argv[1:]
    save = SAVE
    if args and os.path.exists(args[0]):
        save = args.pop(0)
    names = args or list(PLACES)
    with tempfile.TemporaryDirectory() as tmp:
        for name in names:
            make(name, save, tmp)


if __name__ == '__main__':
    main()
