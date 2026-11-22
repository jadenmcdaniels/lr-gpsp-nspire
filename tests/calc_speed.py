#!/usr/bin/env python3
"""Estimates how fast gpSP runs a game on the calculator (TI-Nspire CX II,
ARM926EJ-S at 396 MHz), from a PC. Run it through tests/calc_speed.sh, which
puts the Ndless toolchain on PATH.

It builds tests/arm/perf (the core with its ARM dynarec, the renderer and the
picture output, compiled with the calculator's flags), plays a game from a
save state under qemu-arm with the ARM926 model (tools/calc_speed/arm926.c:
pipeline, 16 KB / 8 KB caches, write buffer, the memory timings measured on
the calculator), and reports milliseconds per frame by part of the emulator
and the frame rates that gives:

  game     running the game: the dynarec's code and what it calls (memory,
           I/O), translating code, DMA, the timers and other events
  drawing  the GBA picture, line by line (the renderer)
  picture  scaling, blending and turning it into the LCD's buffer

Checked against PocketSNES's speed tests on the calculator (Super Mario
World, 2026-10-10): the model's milliseconds times 1.05 came within 6% of
all three (3.6 vs 3.70 ms a frame running the game on the ARM core, 4.8 vs
4.74 on the C core, 10.0 vs 10.6 drawing every frame). gpSP's own speed test
on the calculator (Menu > speed test) is the real thing.

The model runs the calculator build's layout: tests/arm/build/perf puts
every function where gpsp_libretro.elf has it modulo 4 KB (the caches'
ways; tests/arm/mirror_layout.py), so build the calculator first (make in
the top folder) and again after a change, or the model runs the old layout
(said on the "layout:" line).

For a speed hunt (speed hunt tooling): --scene NAME runs a
named scene (--scene list), --layouts N runs it at N placements of the
dynarec's blocks and the stack and gives the mean and range of the model's
times (one placement alone can be 10% off: the instruction cache), and the
exact instructions per frame, which no placement changes: the number to
judge a small change by. --regions splits the time by what owns the code
(emitted code, stubs, memory handlers, update_gba, renderer, picture
output, frontend), --listing FILE keeps every function's line, --elf runs
another build of tests/arm/build/perf (to compare two builds).
"""

import argparse
import bisect
import collections
import concurrent.futures
import glob
import os
import random
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ARM = os.path.join(ROOT, 'tests', 'arm')
BUILD = os.path.join(ARM, 'build')
RUN = os.path.join(BUILD, 'speed')
# PERF_AUDIO=1 (tests/audio): the harness with the mixer on, build/perf_audio
PERF = 'perf_audio' if os.environ.get('PERF_AUDIO') else 'perf'
QEMU = os.environ.get('QEMU', 'qemu-arm')
GAMES = os.environ.get('GBA_GAMES', '')
SCALE = 1.05      # model -> calculator (fitted on PocketSNES, see above)
MHZ = 396.0
# The calculators (--calc): CPU MHz, the ARM926 model's memory timings (CPU
# cycles) and the LCD. The CX II's are measured (above); the older CX's
# (132 MHz, a 66 MHz bus) are a guess: the same memory in nanoseconds would
# be 40 cycles a line fill, a slower bus more, so 60 (about 450 ns).
# cx192: the older CX at 192 MHz (bus 64 MHz, CPU speed auto): the same
# memory in bus cycles (fill 30, writes 1.5, writeback 12) at 3 CPU cycles
# each.
CALCS = {'cx2': (396.0, '', []),
         'cx': (132.0, 'fill=60,write=3,word=3,writeback=24', ['portrait=0']),
         'cx192': (192.0, 'fill=90,write=5,word=5,writeback=36', ['portrait=0'])}

# Where each function's cycles go: the call and everything it calls.
REGIONS = [('update_scanline', 'drawing'), ('nspire_present_frame', 'picture'),
           ('translate_block_arm', 'translate'), ('translate_block_thumb', 'translate'),
           ('update_gba', 'events'), ('dma_transfer', 'dma'), ('sound_timer', 'audio'),
           ('render_gbc_sound', 'audio'), ('sound_read_samples', 'audio'),
           ('perf_audio_resample', 'audio'), ('frame_hash', 'harness'), ('perf_audio_frame', 'harness')]
# The harness's own work (perf.c's frame hashes): not counted at all.
GAME_PARTS = ('cpu', 'translate', 'events', 'dma', 'audio')

# Named scenes (--scene): ROM, state, keys, frames, frames left out, one
# frame drawn in N, what it is. The reference Emerald states are read where
# they are ($EMERALD_STATES, default the newest *-emerald-states backup):
# cx2/ (the CX II's slots) and cx/ (the CX CAS's: Dewford).
EMERALD = 'Pokemon - Emerald Version (USA, Europe).gba'
STATES = os.environ.get('EMERALD_STATES', '')
REPLAY = os.path.join(ARM, 'replay')


def slot(calc, n):
    return os.path.join(STATES, calc, EMERALD + '%d.svs.tns' % n)


GRASS = os.path.join(GAMES, 'emerald_route_grass.svs.tns')
GRASS_KEYS = os.path.join(REPLAY, 'emerald_grass.keys')
BOOT_KEYS = os.path.join(REPLAY, 'boot.keys')
FX = os.path.join(GAMES, 'emerald_fx_%s.svs.tns')
FX_KEYS = os.path.join(REPLAY, 'fx_%s.keys')
SCENES = {
    'dewford': (EMERALD, slot('cx', 26), os.path.join(REPLAY, 'dewford_pc.keys'), 600, 300, 1,
                "Emerald, Dewford Town's Pokemon Center (map group 3 number 2), walking round"
                " (the CX CAS's state 26)"),
    'walk': (EMERALD, GRASS, GRASS_KEYS, 290, 30, 1, 'Emerald, a route, walking to the tall grass'),
    'wildwipe': (EMERALD, GRASS, GRASS_KEYS, 345, 298, 1, 'Emerald, the wild-battle wipe'),
    'town': (EMERALD, slot('cx2', 24), '-', 300, 30, 1, 'Emerald, Mauville (the CX II\'s state 24)'),
    'trainwipe': (EMERALD, slot('cx2', 28), '-', 200, 0, 1, 'Emerald, a trainer battle wipe (state 28)'),
    'battle': (EMERALD, slot('cx2', 30), '-', 300, 30, 1, 'Emerald, a battle (state 30)'),
    'menu': (EMERALD, slot('cx2', 23), '-', 300, 30, 1, 'Emerald, the summary screen (state 23)'),
    # Fast forward draws as many pictures as normal play (60 a second on a CX II,
    # evenly spaced): at 180 game frames a second
    # that is one frame in 3 drawn. "all" is then fast forward's own frame.
    'ff': (EMERALD, slot('cx2', 31), os.path.join(REPLAY, 'fallarbor_pc.keys'), 600, 300, 3,
           "fast forward, 60 pictures at 180: Fallarbor's Pokemon Center (the CX II's state 31),"
           " walking round, one frame in 3 drawn"),
    'fftown': (EMERALD, slot('cx2', 24), '-', 390, 30, 3,
               'fast forward, 60 pictures at 180: Mauville (state 24), one frame in 3 drawn'),
    'ffwalk': (EMERALD, GRASS, GRASS_KEYS, 290, 50, 3,
               'fast forward, 60 pictures at 180: the route walk, one frame in 3 drawn'),
    'fallarbor': (EMERALD, slot('cx2', 31), os.path.join(REPLAY, 'fallarbor_pc.keys'), 600, 300, 1,
                  "Emerald, Fallarbor's Pokemon Center, walking round (the CX II's state 31)"),
    'grass': (EMERALD, GRASS, GRASS_KEYS, 1900, 30, 1,
              'Emerald, route to grass and the battle (tests/speed_scenes.sh "emerald")'),
    # The effect scenes (2026-11-21, final/r19-fx): the reference save
    # continued where the effect is (tests/emerald_fx_states.py: the game's
    # own warp, its weather), walking (tests/arm/replay/fx_*.keys).
    'rain': (EMERALD, FX % 'rain', FX_KEYS % 'rain', 330, 30, 1,
             'Emerald, Route 119 in the rain, walking 8 tiles down and back'),
    'storm': (EMERALD, FX % 'storm', FX_KEYS % 'rain', 330, 30, 1,
              'Emerald, Route 119 in the thunderstorm (lightning at frames 105 and 183), the rain\'s walk'),
    'sand': (EMERALD, FX % 'sand', FX_KEYS % 'sand', 330, 30, 1,
             'Emerald, Route 111\'s desert in the sandstorm, walking 4 tiles east and back'),
    'ash': (EMERALD, FX % 'ash', FX_KEYS % 'ash', 330, 30, 1,
            'Emerald, Route 113 in the falling ash, walking 6 tiles east and back'),
    'surf': (EMERALD, FX % 'surf', FX_KEYS % 'surf', 330, 30, 1,
             'Emerald, surfing on Route 124, 12 tiles east and back'),
    'dive': (EMERALD, FX % 'dive', FX_KEYS % 'dive', 330, 30, 1,
             'Emerald, diving on Underwater Route 124, 7 tiles east and back'),
    'fog': (EMERALD, FX % 'fog', FX_KEYS % 'fog', 330, 30, 1,
            'Emerald, Mt Pyre\'s summit in the fog, walking 10 tiles up and back'),
    'aw': ('Advance Wars (USA) (Rev 1).gba', os.path.join(GAMES, 'aw_latest.svs.tns'), '-', 600, 30, 1,
           'Advance Wars, the name entry (an interrupt every line)'),
    'fe': ('Fire Emblem - The Sacred Stones (USA, Australia).gba', '-', BOOT_KEYS, 2400, 1700, 1,
           'Fire Emblem, the intro (frames 1700-2400 from power-on)'),
    'gs': ('Golden Sun (USA, Europe).gba', '-', BOOT_KEYS, 2400, 1700, 1, 'Golden Sun, the intro'),
    'mc': ('Legend of Zelda, The - The Minish Cap (USA).gba', '-', BOOT_KEYS, 2400, 1700, 1,
           'Minish Cap, the intro'),
}

# What owns the code (--regions), by the object file a function comes from
# (tests/arm/build/*.o; LTO's renamed copies such as foo.lto_priv.0 count as
# foo). OS time is not in the model: the calculator's log has it.
HOLDERS = [('emitted code', ['rom_translation_cache', 'ram_translation_cache']),
          ('stubs', ['arm_stub.o']),
          ('memory handlers', ['core_gba_memory.o']),
          ('update_gba', ['core_main.o']),
          ('dynarec C', ['core_cpu_threaded.o', 'core_cpu.o']),
          ('sound, serial', ['core_sound.o', 'core_serial.o', 'core_serial_proto.o', 'core_serial_fe.o',
                             'core_serial_msio.o', 'core_rfu.o', 'core_gbp.o', 'core_cheats.o']),
          ('renderer', ['video_cc_renderer.o', 'front_nspire_lines.o', 'scanline_fill16.o']),
          ('picture output', ['front_nspire_video_present.o', 'front_nspire_blend.o', 'blend_landscape.o', 'blend_portrait.o',
                              'front_nspire_turn.o', 'upscale_aspect.o', 'front_nspire_upscale_from_gba.o',
                              'front_nspire_osd.o', 'front_nspire_video_ui.o', 'front_nspire_ui.o']),
          ('frontend', ['perf.o', 'perf_stubs.o', 'link_stubs.o', 'front_nspire_stubs.o',
                        'front_nspire_render_time.o', 'newlib_shim.o'])]


# What importers (tests/arm/ui_bench.sh) get: the CX II, this gpSP.
ARGS = argparse.Namespace(calc='cx2', emu='ours')


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


RIVAL = os.path.join(ROOT, 'tests', 'rival')


CALC_ELF = os.path.join(ROOT, 'gpsp_libretro.elf')


def layout_note():
    """Which layout the model runs (tests/arm/mirror_layout.py copies the
    calculator build's, gpsp_libretro.elf, modulo 4 KB), and whether that
    build is older than the sources (then the model runs the old layout)."""
    if ARGS.elf or ARGS.emu != 'ours':
        return
    if not os.path.exists(CALC_ELF):
        print('layout: the model\'s own (no gpsp_libretro.elf: make in the top folder for the calculator\'s)')
        return
    t = os.path.getmtime(CALC_ELF)
    newer = []
    for d in (ROOT, os.path.join(ROOT, 'nspire'), os.path.join(ROOT, 'libretro-gpsp'),
              os.path.join(ROOT, 'libretro-gpsp', 'arm')):
        for f in glob.glob(os.path.join(d, '*')):
            if f.endswith(('.c', '.cc', '.h', '.S', '.s')) and os.path.getmtime(f) > t:
                newer.append(os.path.relpath(f, ROOT))
    print('layout: the calculator build\'s (gpsp_libretro.elf)%s' % (
        '' if not newer else '; OLDER than %s%s: make in the top folder, or the model runs the old layout'
        % (', '.join(newer[:3]), ' ...' if len(newer) > 3 else '')))


def build():
    if ARGS.emu == 'rival':
        sh(['make', '-s', '-C', RIVAL, '-j4'], stdout=subprocess.DEVNULL)
    else:
        sh(['make', '-s', '-C', ARM, '-j%d' % min(4, os.cpu_count()), 'build/' + PERF] + (['AUDIO=1'] if PERF == 'perf_audio' else []), stdout=subprocess.DEVNULL)
    return build_plugin()


def build_plugin():
    plugin = os.path.join(BUILD, 'arm926.so')
    src = os.path.join(ROOT, 'tools', 'calc_speed', 'arm926.c')
    if not os.path.exists(plugin) or os.path.getmtime(src) > os.path.getmtime(plugin):
        inc = os.path.join(os.path.dirname(os.path.dirname(QEMU)), 'include', 'qemu')
        glib = subprocess.run(['pkg-config', '--cflags', '--libs', 'glib-2.0'], capture_output=True,
                              text=True, check=True).stdout.split()
        # (runs side by side may build it at once: each its own file, then moved)
        tmp = '%s.%d' % (plugin, os.getpid())
        sh(['gcc', '-O2', '-shared', '-fPIC', '-I' + inc, src, '-o', tmp] + glib)
        os.replace(tmp, plugin)
    return plugin


def symbols(elf):
    out = subprocess.run(['arm-none-eabi-nm', '-n', '-S', elf], capture_output=True, text=True,
                         check=True).stdout
    syms = []
    for line in out.split('\n'):
        p = line.split()
        if len(p) == 4:
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
        elif len(p) == 3:
            syms.append((int(p[0], 16), 0, p[2]))
    syms.sort()
    return syms


def link_name(name):
    tag, ext = os.path.splitext(name)
    if len(tag) <= 12:
        return name
    import hashlib
    return 'h' + hashlib.md5(tag.encode()).hexdigest()[:10] + ext


def link(name, target):
    """Semihosting splits the command line at spaces and cuts it short: plain,
    short names (one set per --tag, so runs can go side by side; a long tag
    such as tests/layout_moves.sh's goes by a hash of it)."""
    path = os.path.join(RUN, link_name(name))
    tmp = path + '.new'
    if os.path.lexists(tmp):
        os.remove(tmp)
    os.symlink(os.path.abspath(target), tmp)
    os.replace(tmp, path)
    return os.path.relpath(path, ARM)


def elf_path(args):
    if args.elf:
        return os.path.abspath(args.elf)
    return os.path.join(BUILD, PERF) if args.emu == 'ours' else os.path.join(RIVAL, 'build', 'perf_rival')


def sram_ranges(spec, syms, calc_elf):
    """--sram: the model's lines the calculator has in its on-chip SRAM
    (nspire_sram.c), as (lo, hi) of the model's addresses. spec, comma
    separated: "hot" (the moved stretch, update_gba up to nspire_hot_end),
    "calc:LO-HI" (what gpsp_libretro.elf has there, hex), "sym:NAME" (a
    symbol of the model, its size), "ram:KB" / "rom:KB" (the first KB of the
    RAM / ROM translation cache). Code of the calculator's build is found in
    the model by its symbols (LTO's suffixes dropped); a symbol without a size
    (arm_stub.S's) runs to the next one."""
    def base(n):
        return re.sub(r'\.(lto_priv|constprop|isra|part|cold)\.\d+', '', n)

    def sized(sl):
        out = []
        for i, (a, size, n) in enumerate(sl):
            if not size:
                nxt = [b for b, _, _ in sl[i + 1:i + 8] if b > a]
                size = nxt[0] - a if nxt else 4
            out.append((a, size, n))
        return out

    model = {}
    for a, size, n in sized(syms):
        model.setdefault(base(n), (a, size))
    calc = sized(symbols(calc_elf)) if os.path.exists(calc_elf) else []
    by_name = {n: a for a, _, n in calc}
    ranges, missing = [], []

    def from_calc(lo, hi):
        for a, size, n in calc:
            if lo <= a < hi and size and not n.startswith('$'):
                m = model.get(base(n))
                if m:
                    ranges.append((m[0], m[0] + min(m[1], size, hi - a)))
                elif size > 64:
                    missing.append(n)
    for item in spec.split(','):
        kind, _, arg = item.partition(':')
        if kind == 'hot':
            from_calc(by_name['update_gba'], by_name['nspire_hot_end'])
        elif kind == 'calc':
            lo, hi = arg.split('-')
            from_calc(int(lo, 16), int(hi, 16))
        elif kind == 'sym':
            a, size = model[base(arg)]
            ranges.append((a, a + size))
        elif kind in ('ram', 'rom'):
            a, _ = model[kind + '_translation_cache']
            ranges.append((a, a + int(arg) * 1024))
        else:
            sys.exit('--sram: what is %s?' % item)
    if missing:
        print('sram: not in the model: %s' % ', '.join(missing[:6]))
    return ranges


def run(args, plugin, tag, more=()):
    os.makedirs(RUN, exist_ok=True)
    elf = elf_path(args)
    regions_list = REGIONS + ([('flip_screen', 'picture')] if args.emu == 'rival' else [])
    syms = symbols(elf)
    by_name = {n: a for a, _, n in syms}
    regions = os.path.join(RUN, tag + '.regions')
    with open(regions, 'w') as f:
        for fn, name in regions_list:
            if fn in by_name:
                f.write('%x %s\n' % (by_name[fn], name))
    out = os.path.join(RUN, tag + '.model')
    plugin_args = '%s,mark=%x,regions=%s,out=%s' % (plugin, by_name['perf_frame_mark'], regions, out)
    if args.functions_of:
        first, last = args.functions_of.split('-')
        plugin_args += ',first=%d,last=%d' % (int(first) - 1, int(last))
    model = ','.join(m for m in (CALCS[args.calc][1], args.model) if m)
    if args.sram:
        fast = os.path.join(RUN, tag + '.sram')
        with open(fast, 'w') as f:
            for lo, hi in sram_ranges(args.sram, syms, CALC_ELF):
                f.write('%x %x\n' % (lo, hi))
        model = ','.join(m for m in (model, 'fastfile=' + fast) if m)
    if model:
        plugin_args += ',' + model
    # The program's command line reaches it whole only if short (semihosting):
    # another build goes through a link next to the others.
    prog = os.path.relpath(elf, ARM) if not args.elf else link(tag + '.elf', elf)
    cmd = [QEMU, '-cpu', 'arm926', '-plugin', plugin_args,
           prog, link(tag + '.rom', args.rom), link(tag + '.bios', args.bios),
           link(tag + '.state', args.state) if args.state != '-' else '-', str(args.frames),
           link(tag + '.keys', args.keys) if args.keys != '-' else '-', str(args.draw)] + \
        (CALCS[args.calc][2] if args.emu == 'ours' else []) + args.set + list(more)
    res = subprocess.run(cmd, cwd=ARM, capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit('perf failed (exit %d):\n%s%s' % (res.returncode, res.stdout, res.stderr))
    return out, syms, res.stdout


def analyse(out, syms):
    """The frames (each region's cycles, and its instructions under
    'insns') and the functions' sums."""
    lines = open(out).read().split('\n')
    names = lines[2].split()[1:]
    n = len(names)
    frames = []
    for line in lines:
        if line.startswith('frame'):
            v = [int(x) for x in line.split()[1:]]
            fr = dict(zip(names, v[:n]))
            frames.append((fr, dict(zip(names, v[n:2 * n]))))
    addrs = [a for a, _, _ in syms]
    funcs = collections.defaultdict(lambda: [0, 0, 0, 0, 0])
    for line in lines[3:]:
        p = line.split()
        if len(p) != 7:
            continue
        pc = int(p[0], 16)
        i = bisect.bisect_right(addrs, pc) - 1
        fn = '?'
        if i >= 0:
            a, size, sym = syms[i]
            fn = sym if not size or pc < a + size else '?'
        acc = funcs[(names[int(p[1])], fn)]
        for j in range(5):
            acc[j] += int(p[2 + j])
    return frames, funcs


def ms(cycles):
    return cycles * SCALE / (CALCS[ARGS.calc][0] * 1000.0)


def demangle(names):
    res = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True)
    return dict(zip(names, res.stdout.split('\n')))


def costs(frames, skip):
    """One run's costs over its timed frames: cycles (and instructions)
    a frame of game, a drawn frame's drawing and picture, all."""
    timed = frames[skip:]
    nf = len(timed)
    if not nf:
        sys.exit('no frames timed')
    c = {'frames': nf}
    for i, key in ((0, ''), (1, 'i_')):
        timed_i = [{k: v for k, v in f[i].items() if k != 'harness'} for f in timed]
        parts = collections.Counter()
        for fr in timed_i:
            parts.update(fr)
        drawn = [fr for fr, f in zip(timed_i, timed) if f[0].get('picture', 0)]
        undrawn = [fr for fr, f in zip(timed_i, timed) if not f[0].get('picture', 0)]
        nd = max(1, len(drawn))
        c[key + 'game'] = sum(parts[k] for k in GAME_PARTS) / nf
        for k in GAME_PARTS:
            c[key + k] = parts[k] / nf
        c[key + 'drawing'] = sum(fr.get('drawing', 0) for fr in drawn) / nd
        c[key + 'picture'] = sum(fr.get('picture', 0) for fr in drawn) / nd
        c[key + 'all'] = sum(sum(fr.values()) for fr in timed_i) / nf
        c[key + 'skipped'] = sum(sum(fr.values()) for fr in undrawn) / len(undrawn) if undrawn else 0
        c[key + 'totals'] = [sum(fr.values()) for fr in timed_i]
    c['drawn'] = sum(1 for fr in timed if fr[0].get('picture', 0))
    return c


def spread(values, fmt='%.2f'):
    """Mean, and the range when there are several."""
    mean = sum(values) / len(values)
    if len(values) == 1:
        return fmt % mean
    return (fmt + ' (%s-%s)') % (mean, fmt % min(values), fmt % max(values))


def report(args, runs, funcs, frames_run):
    """runs: one costs() per layout (the first: the build's own)."""
    c0 = runs[0]
    nf, nd = c0['frames'], c0['drawn']
    msv = lambda key: [ms(c[key]) for c in runs]
    mean = lambda key: sum(c[key] for c in runs) / len(runs)
    lay = '' if len(runs) == 1 else '; mean (range) of %d layouts' % len(runs)
    print('%d frames timed (%d drawn); estimated ms per frame on the %s at %.0f MHz%s:'
          % (nf, nd, 'CX II' if args.calc == 'cx2' else 'CX', CALCS[args.calc][0], lay))
    print('  game     %s   (dynarec code %.2f, translating %.2f, events %.2f, DMA %.2f, audio %.2f)'
          % (spread(msv('game')), ms(mean('cpu')), ms(mean('translate')), ms(mean('events')),
             ms(mean('dma')), ms(mean('audio'))))
    if nd and nd < nf:
        print('  skipped  %s   per frame not drawn' % spread(msv('skipped')))
    if nd:
        print('  drawing  %s   per drawn frame' % spread(msv('drawing')))
        print('  picture  %s   per drawn frame' % spread(msv('picture')))
    avg = mean('all')
    print('  all      %s   per frame on average: %.1f game frames a second with no speed limit'
          % (spread(msv('all')), 1000.0 / ms(avg)))
    if len(runs) > 1:
        # Each placement's (the same seeds every run): two builds compared
        # placement by placement.
        print('  per placement, game / all ms: %s' % ', '.join(
            '%.2f/%.2f' % (g, a) for g, a in zip(msv('game'), msv('all'))))
    # The instructions: the same at any placement (checked here).
    keys = ('i_game', 'i_drawing', 'i_picture', 'i_all')
    same = all(abs(c[k] - c0[k]) < 0.5 for c in runs for k in keys)
    print('  instructions a frame (exact, layout-free): game %s, drawing %s and picture %s per drawn frame,'
          ' all %s%s' % ('{:,.0f}'.format(c0['i_game']), '{:,.0f}'.format(c0['i_drawing']),
                         '{:,.0f}'.format(c0['i_picture']), '{:,.0f}'.format(c0['i_all']),
                         '' if same else '  (DIFFER between layouts: %s)'
                         % ', '.join('{:,.0f}'.format(c['i_all']) for c in runs)))
    # The slowest stretches: a second's worth of frames (the FPS counter's
    # window) and the slowest single frames (the build's own layout).
    totals = c0['totals']
    win = min(nf, args.window)
    worst = max(sum(totals[i:i + win]) for i in range(nf - win + 1))
    print('  slowest %d frames in a row: %.1f game frames a second; slowest frame %.2f ms'
          % (win, win * 1000.0 / ms(worst), ms(max(totals))))
    game, draw, pic = mean('game'), mean('drawing'), mean('picture')
    if nd:
        # Fast forward drawing at the LCD's pace: 60 pictures a second, the
        # other frames only run (from this run's averages): N frames of game
        # and 60 of drawing and picture fit in a second.
        budget = 1000.0 - 60 * ms(draw + pic)
        print('  fast forward drawing 60 pictures a second (from these averages): %.0f game frames a second'
              % (budget / ms(game) if budget > 0 else 0))
        summary(ms(game), ms(draw + pic) + ms(game), args)
    if args.regions:
        holders(args, funcs, frames_run, c0)
    if args.top or args.listing:
        listing(args, funcs, frames_run)


def per_frame_count(args, frames_run):
    if args.functions_of:
        first, last = args.functions_of.split('-')
        return min(frames_run, int(last)) - int(first) + 1
    return frames_run


def owner_map():
    """Function name -> holder (HOLDERS), from the objects' symbols."""
    found = {}
    for holder, objs in HOLDERS:
        for obj in objs:
            if not obj.endswith('.o'):
                found[obj] = holder
                continue
            path = os.path.join(BUILD, obj)
            if not os.path.exists(path):
                continue
            out = subprocess.run(['arm-none-eabi-nm', '--defined-only', path], capture_output=True,
                                 text=True).stdout
            for line in out.split('\n'):
                p = line.split()
                if len(p) == 3 and p[1] in 'tTW':
                    found.setdefault(p[2], holder)
    return found


def owner_of(fn, found):
    if fn in found:
        return found[fn]
    base = fn.split('.')[0]
    if base in found:
        return found[base]
    return 'library (memcpy...)' if fn != '?' else 'unknown'


def holders(args, funcs, frames_run, c0):
    found = owner_map()
    per = per_frame_count(args, frames_run)
    acc = collections.defaultdict(lambda: [0, 0])
    for (region, fn), v in funcs.items():
        if region == 'harness':
            continue
        o = owner_of(fn, found)
        acc[o][0] += v[0]
        acc[o][1] += v[1]
    total = sum(v[1] for v in acc.values()) or 1
    print('\nby holder of the code (per frame%s): ms, share, instructions'
          % (', frames %s' % args.functions_of if args.functions_of else ''))
    for o, v in sorted(acc.items(), key=lambda kv: -kv[1][1]):
        print('  %-20s %6.3f ms %5.1f%% %10s' % (o, ms(v[1] / per), 100.0 * v[1] / total,
                                                 '{:,.0f}'.format(v[0] / per)))
    print('  %-20s  (not in the model; gpsp_perf.txt "os" on the calculator)' % 'OS time')


def listing(args, funcs, frames_run):
    dem = demangle([k[1] for k in funcs])
    found = owner_map() if args.listing else {}
    per = per_frame_count(args, frames_run)
    rows = sorted(funcs.items(), key=lambda kv: -kv[1][1])
    head = '%-8s %-46s %8s %8s %5s %7s %7s %7s %6s' % ('part', 'function', 'insns', 'cycles', 'CPI',
                                                     'ifetch', 'data', 'wbuf', 'ms')

    def line(region, fn, v, width=46):
        return '%-8s %-*s %8.0f %8.0f %5.2f %7.0f %7.0f %7.0f %6.3f' % (
            {'cpu': 'game'}.get(region, region), width, dem.get(fn, fn)[:width], v[0] / per, v[1] / per,
            v[1] / max(1, v[0]), v[2] / per, v[3] / per, v[4] / per, ms(v[1] / per))
    note = ('(per frame, over %s; ifetch/data/wbuf: cycles waiting for instruction fetches, data reads'
            ' and the write buffer)' % ('frames ' + args.functions_of if args.functions_of
                                        else 'all frames run'))
    if args.top:
        print('\n' + head)
        for (region, fn), v in rows[:args.top]:
            print(line(region, fn, v))
        print(note)
    if args.listing:
        with open(args.listing, 'w') as f:
            f.write(' '.join(sys.argv) + '\n' + head + ' holder\n')
            for (region, fn), v in rows:
                f.write(line(region, fn, v, 80) + ' ' + owner_of(fn, found) + '\n')
            f.write(note + '\n')
        print('every function: %s' % args.listing)


FRAME_MS = 1000.0 / 59.7275   # a GBA frame


def summary(skipped, drawn, args):
    """What the frontend makes of these costs (nspire_frameskip.c's pace
    rule, 2026-11-15, final/pace; automatic frameskip, the default, on every
    calculator): normal play draws every frame while they fit in a GBA
    frame, else leaves out only as many pictures as hold full speed, at most
    Frameskip value in a row (one picture in 5 at the default 4); fast
    forward at its top speed "max" draws 60 pictures a second and runs the
    game with the rest of the time, or where that leaves under 60 game
    frames, as normal play. Also the time a second the pictures take in it,
    and fast forward as it was 2026-11-10 to 11-12 (pictures at most 15 a
    second and a fifth of the time). An upper bound: whole frames between
    refreshes cost the PC's gpsp-host 0-3% more. (Until final/pace:
    automatic's levels, d drawn in n, and the CX II with frameskip off.)"""
    floor_n = 5
    if drawn <= FRAME_MS:
        share = 1.0                                            # every picture
    else:
        share = max(1.0 / floor_n, 1.0 - (drawn - FRAME_MS) / (drawn - skipped))
    per = share * drawn + (1.0 - share) * skipped
    speed = min(100.0, 100.0 * FRAME_MS / per)
    game = 1000.0 / per if speed < 100.0 else 59.7275
    pictures = game * share
    # Fast forward: 60 pictures, the game with the rest; under 60, as normal play.
    ff_game = 60.0 + (1000.0 - 60.0 * drawn) / skipped
    if ff_game >= 59.7275:
        ff_pictures = 60.0
    else:
        ff_game, ff_pictures = (1000.0 / per if speed < 100.0 else 59.7275), pictures
    picture_ms = ff_pictures * (drawn - skipped)
    # 2026-11-10 to 11-12: pictures at most 15 a second and a fifth of the time.
    capped_pictures = min(15.0, 200.0 / drawn)
    capped = capped_pictures + max(0.0, (1000.0 - capped_pictures * drawn) / skipped)
    # 2026-11-17 (final/speed8; the reference bar: 120 game frames a second
    # and 60 pictures with a hub, a pad, the sound adapter and a drive in):
    # fast forward with the OS's time a second taken out (--os-ms, the perf
    # log's "os ... ms" outside the waits and the sound's "win ... ms").
    os_part = ''
    for os_ms in args.os_ms:
        g = 60.0 + (1000.0 - os_ms - 60.0 * drawn) / skipped
        os_part += '; with the OS %.0f ms a second %.0f' % (os_ms, g)
    print('  summary: normal %.0f%% (%.0f game frames, %.0f pictures a second), fast forward max %.0f game frames'
          ' a second with %.0f pictures (%.0f ms of each second, the game %.0f), with pictures capped at 15 a'
          ' second %.0f%s' % (speed, game, pictures, ff_game, ff_pictures, picture_ms, 1000.0 - picture_ms, capped,
                              os_part))


LAYOUT_SEED = 926


def layouts(n):
    """Placements beyond the build's own: the ROM blocks' and RAM blocks'
    start in their caches and the stack, spread over the caches' ways
    (4 KB instruction, 2 KB data), the same every time."""
    rnd = random.Random(LAYOUT_SEED)
    out = [[]]
    for _ in range(n - 1):
        out.append(['jit=%d' % (rnd.randrange(1024) * 4), 'ramjit=%d' % (rnd.randrange(1024) * 4),
                    'stack=%d' % (rnd.randrange(256) * 8)])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('state', nargs='?', default=None,
                    help='a gpSP save state to start from, or - (default: Emerald by the tall grass)')
    ap.add_argument('--scene', help='a named scene (ROM, state, keys, frames; "list" lists them);'
                                    ' its frames are the ones listed by --top')
    ap.add_argument('--rom', default=None)
    ap.add_argument('--bios', default=os.path.join(GAMES, 'gba_bios.bin'))
    ap.add_argument('--frames', type=int, default=None, help='frames to run (default 600)')
    ap.add_argument('--skip', type=int, default=None, help='first frames left out (default 30)')
    ap.add_argument('--keys', default=None, help='keys to press (replay.c format)')
    ap.add_argument('--draw', type=int, default=None,
                    help='draw every frame (1, default), none (0) or one in N')
    ap.add_argument('--window', type=int, default=120,
                    help='frames in the slowest-stretch figure (default 120: one second at 2x)')
    ap.add_argument('--top', type=int, default=0, help='list the N costliest functions')
    ap.add_argument('--functions-of', metavar='A-B',
                    help='list the functions of frames A to B only (with --top)')
    ap.add_argument('--regions', action='store_true',
                    help='split the time by what owns the code (emitted code, stubs, ...)')
    ap.add_argument('--listing', metavar='FILE', help='write every function\'s line to FILE')
    ap.add_argument('--layouts', type=int, default=1,
                    help='run at N placements (the build\'s own and N-1 others) and give the mean'
                         ' and range (default 1)')
    ap.add_argument('--jobs', type=int, default=4, help='runs at once with --layouts (default 4)')
    ap.add_argument('--elf', help='run this build of perf instead of building tests/arm/build/perf')
    ap.add_argument('--model', metavar='ARGS',
                    help='more arguments for the ARM926 model, e.g. repl=random or icache=32768'
                         ' (tools/calc_speed/arm926.c)')
    ap.add_argument('--calc', default='cx2', choices=sorted(CALCS),
                    help='the calculator: cx2 (CX II, 396 MHz, default), cx (an older CX, 132 MHz:'
                         ' memory timings guessed, its landscape LCD) or cx192 (that CX at 192 MHz,'
                         ' CPU speed auto)')
    ap.add_argument('--emu', default='ours', choices=('ours', 'rival'),
                    help='ours, or the rival gpSP (tests/rival: STATE is then an in-game save or -)')
    ap.add_argument('--tag', default='run', help='name of the output files in tests/arm/build/speed')
    ap.add_argument('--trace', metavar='FILE',
                    help='write each timed frame\'s game, drawing and picture ms (one line a frame: '
                         'gpsp-host\'s "cost trace FILE" plays them back)')
    ap.add_argument('--sram', metavar='SPEC',
                    help='what the calculator has in its on-chip SRAM, filled at a quarter of the cost (--model '
                    'fastfill=N for another): hot, calc:LO-HI, sym:NAME, ram:KB, rom:KB (comma separated)')
    ap.add_argument('--os-ms', type=float, action='append', default=None, metavar='MS',
                    help='the summary\'s fast forward also with this much of each second the OS\'s (USB, sound;'
                         ' repeatable; default 70 and 160: a hub, a pad and the sound with final/usb8, and r15\'s'
                         ' with the pad\'s own ticks, the reference CX II log)')
    ap.add_argument('--set', action='append', default=[], metavar='NAME=VALUE',
                    help='a setting other than the reference (rombuf=16 policy=0 scale=1 mix=1'
                         ' sprlim=1)')
    global ARGS
    args = ARGS = ap.parse_args()
    if args.os_ms is None:
        args.os_ms = [70.0, 160.0]
    if args.scene == 'list':
        for name, sc in SCENES.items():
            print('%-10s %s' % (name, sc[6]))
        return
    defaults = dict(state=GRASS, rom=os.path.join(GAMES, EMERALD), keys='-', frames=600, skip=30, draw=1)
    if args.scene:
        if args.scene not in SCENES:
            sys.exit('no scene %s (--scene list)' % args.scene)
        rom, state, keys, frames, skip, draw, _ = SCENES[args.scene]
        defaults = dict(state=state, rom=os.path.join(GAMES, rom), keys=keys, frames=frames, skip=skip,
                        draw=draw)
    for k, v in defaults.items():
        if getattr(args, k) is None:
            setattr(args, k, v)
    if args.scene and not args.functions_of:
        args.functions_of = '%d-%d' % (args.skip + 1, args.frames)
    for path in (args.rom, args.state):
        if path != '-' and not os.path.exists(path):
            sys.exit('missing %s' % path)
    plugin = build() if not args.elf else build_plugin()
    skip = args.skip
    sets = layouts(max(1, args.layouts))

    def one(i):
        return run(args, plugin, args.tag if i == 0 else '%s-l%d' % (args.tag, i), sets[i])
    with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
        results = list(pool.map(one, range(len(sets))))
    runs, hashes = [], set()
    for i, (out, syms, stdout) in enumerate(results):
        frames, funcs = analyse(out, syms)
        if i == 0:
            frames0, funcs0, stdout0 = frames, funcs, stdout
        runs.append(costs(frames, min(skip, max(0, len(frames) - 1))))
        hashes.add(stdout.strip().split('\n')[-1])
    print(stdout0.strip().split('\n')[-1] + ('' if len(hashes) == 1 else '  (OTHER PICTURES at other layouts)'))
    if args.scene:
        print('scene %s: %s' % (args.scene, SCENES[args.scene][6]))
    layout_note()
    report(args, runs, funcs0, len(frames0))
    for i in range(1, len(sets)):
        for ext in ('.model', '.regions', '.rom', '.bios', '.state', '.keys', '.elf'):
            name = '%s-l%d%s' % (args.tag, i, ext)
            path = os.path.join(RUN, name if ext in ('.model', '.regions') else link_name(name))
            if os.path.lexists(path):
                os.remove(path)
    if args.trace:
        tskip = min(skip, max(0, len(frames0) - 1))
        with open(args.trace, 'w') as f:
            f.write('# %s %s: game drawing picture (ms) a frame\n' % (args.calc, os.path.basename(args.state)))
            for fr in frames0[tskip:]:
                f.write('%.3f %.3f %.3f\n' % (ms(sum(fr.get(k, 0) for k in GAME_PARTS)), ms(fr.get('drawing', 0)),
                                              ms(fr.get('picture', 0))))


if __name__ == '__main__':
    main()
