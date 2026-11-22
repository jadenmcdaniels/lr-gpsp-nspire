#!/usr/bin/env python3
"""What gpSP's load and save paths cost on the calculator, library code
included, estimated on a PC. Run it through tests/libbench.sh:

  tests/libbench.sh LABEL [BASELINE.txt]

writes build/perf/libbench-LABEL.txt (and prints it); with a baseline (an
earlier libbench-*.txt) every time gets a delta column.

It builds tests/arm/libbench (gpSP's own load and save code: the core's
load_gamepak, nspire_states.c, nspire_backup.c, nspire_rom_convert.c,
nspire_config.c, nspire_paths.c, nspire_zip.c, nspire_gbz.c, compiled with
the calculator's flags and linked with zlib built as the calculator build
builds it (the SDK's sources, nspire_cold.mk) and the toolchain's newlib,
over tests/arm/libbench_os.c: the SDK's libsyscalls and a stand-in for the
OS), makes a calculator folder for each path in build/libbench/
(the games and states of $GBA_GAMES; a .gbz made by tools/gbz.py), and runs
each path under qemu-arm with the ARM926 model (tools/calc_speed/arm926.c):
  CX II  396 MHz, the model's memory timings (measured on the CX II)
  CX     132 MHz, the same memory in nanoseconds: a line fill 40 cycles,
         a word written 2 (not measured: the CX CAS's timer is dead)
Milliseconds are the model's cycles times 1.05 (tests/calc_speed.py) over
the clock.

Not modeled: what the OS does. Its file calls and heap (the OS's malloc)
are the stand-in's os_* functions, left out of every time; the stdio calls
gpSP makes and the OS calls they turn into are counted, with their bytes,
instead. So "write" and "read" here are only the CPU's part.
"""

import bisect
import collections
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import calc_speed  # noqa: E402  (symbols, QEMU, SCALE, GAMES)

ROOT = calc_speed.ROOT
ARM = calc_speed.ARM
ELF = os.path.join(ARM, 'build', 'libbench')
WORK = os.path.join(ROOT, 'build', 'libbench')
PERF = os.path.join(ROOT, 'build', 'perf')
GAMES = calc_speed.GAMES
# runs at once: $GPSP_TEST_JOBS, or half the CPUs
JOBS = int(os.environ.get('GPSP_TEST_JOBS') or max(1, len(os.sched_getaffinity(0)) // 2))

MODELS = [('CX II', 396.0, ''), ('CX', 132.0, 'fill=40,write=2,word=2,writeback=16')]

EMERALD = 'Pokemon - Emerald Version (USA, Europe).gba'
EMERALD_ZIP = 'Pokemon - Emerald Version (USA, Europe).zip'
AW = 'Advance Wars (USA) (Rev 1).gba'
LIST_GAMES = [('aw', AW), ('emerald', EMERALD), ('fe8', 'Fire Emblem - The Sacred Stones (USA, Australia).gba'),
              ('gs', 'Golden Sun (USA, Europe).gba'), ('minish', 'Legend of Zelda, The - The Minish Cap (USA).gba')]
STATES = {'emerald': 'emerald_route_grass.svs.tns', 'aw': 'aw_latest.svs.tns'}
# The games' in-game saves: real ones from the reference calculator (a backup of
# its saves folder; the states hold no save memory), copied once into
# build/libbench/inputs. Without them the saves are blank (all 0xFF).
SAVES_DIR = os.environ.get('LIBBENCH_SAVES', '')
SAVES = {'emerald': EMERALD + '.sav.gz.tns', 'aw': AW + '.sav.gz.tns'}
BIOS = 'gba_bios.bin'
PROGRAM_SIZE = 589889   # _gpSP.tns at 9fc0aaa: the settings area goes after it

# The calculator's own timings (CX II, gpsp_states_time.txt.tns, the reference
# game), for the comparison.
REAL = {('state-save-emerald', 'copy'): 12.7, ('state-save-emerald', 'pack'): 111.9,
        ('state-save-emerald', 'picture'): 16.2, ('state-save-emerald', 'write'): 28.5,
        ('state-save-emerald', 'replace'): 17.2,
        ('state-load-emerald', 'read'): 21.6, ('state-load-emerald', 'unpack'): 24.4,
        ('state-load-emerald', 'apply'): 14.0}


# The reference saves folder, for the menu paths (CX II, 2026-11-17 listing):
# Emerald's states in slots 19-36 (the real files of 19-31 from a backup of
# the calculator, 19-22 from a build before pictures; 32-36 this build's,
# the newest), Advance Wars 0-2, Minish Cap 0-4, Fire Emblem 0, Golden Sun
# none, and the folder's other files (settings, saves, logs): 66 names.
OWNER_STATES = os.environ.get('LIBBENCH_OWNER_STATES', '')
OWNER_OTHER = ['gba_bios.bin.tns', 'gpsp.cfg.tns', 'gpsp_romdir.tns', 'gpsp_states_time.txt.tns',
               'gpsp_perf.txt.tns', 'gpsp_link.txt.tns', 'gpsp_link_last.txt.tns', 'gpsp_link_linked.txt.tns',
               'gpsp_link_prev.txt.tns', 'gpsp_link_prev2.txt.tns', 'gpsp_link_prev3.txt.tns',
               'gpsp_link_run.txt.tns', 'gpsp_pad.txt.tns', 'gpsp_pad_prev.txt.tns', 'gpsp_usbpad.cfg.tns',
               'gpsp_hints.txt.tns', 'gpsp_exit_after.txt.tns', 'sramprobe_log.txt.tns', '..cfg.tns']
for _g in ('emerald', 'aw', 'fe8', 'gs', 'minish'):
    OWNER_OTHER += ['%s.cfg.tns' % _g, '%s.gba.cfg.tns' % _g, '%s.gba.id.tns' % _g, '%s.gba.sav.gz.tns' % _g]

# What the calculator's file system takes, per OS call (CX II ms: a fixed
# part, and per KB read or written). Fitted on the reference
# gpsp_states_time.txt.tns (2b8d4d6, CX II, 2026-11-17): a state's read
# (stat, fopen, 2 fseek, 2 ftell, fread, fclose) 11.0-18.5 ms for 13-83 KB
# = 11.4 ms + 0.07 ms/KB; its write (2 fopen, 2 fwrite, 2 fclose) 27.4-35.7
# ms for 43-83 KB = 18.9 ms + 0.2 ms/KB; replacing a new slot's name (4 stat,
# a rename) 36-40 ms, an old slot's (+ a failed rename, a rename, a remove)
# 66-71 ms; the old 1 KB reads (r6/r7 builds, ~75 OS freads for a 70 KB state:
# 31-32.5 ms) put a small call at ~0.2 ms. The split of the fixed 11 ms
# between stat and fopen (5 each) is assumed, and nothing measured readdir:
# 0.2 ms a name is a guess. The CX: 3.4 times the CX II (its state read in
# the r6 build, 105.6 ms against 31.0 for the same calls).
OS_MS = {'OS stat': (5.0, 0.0), 'OS fopen': (5.0, 0.0), 'OS fclose': (0.5, 0.0),
         'OS fread': (0.2, 0.07), 'OS fgetc': (0.2, 0.0), 'OS fwrite': (3.9, 0.2), 'OS fseek': (0.2, 0.0), 'OS ftell': (0.2, 0.0),
         'OS opendir': (5.0, 0.0), 'OS readdir': (0.2, 0.0), 'OS closedir': (0.5, 0.0),
         'OS rename': (17.0, 0.0), 'OS unlink': (9.0, 0.0), 'OS mkdir': (17.0, 0.0), 'OS rmdir': (17.0, 0.0)}
OS_CX = 3.4


def os_ms(entries):
    """The file system's estimated ms (CX II) for a step's calls."""
    t = 0.0
    for op, size, count in entries:
        if op in OS_MS:
            fixed, per_kb = OS_MS[op]
            t += count * (fixed + per_kb * size / 1024.0)
    return t


def rom(name):
    return 'documents/gba/%s.tns' % name


R_EM, R_AW = rom('emerald.gba'), rom('aw.gba')
R_BIOS = 'documents/zgpSP_files/gba_bios.bin.tns'


def path_list():
    """name, title, bench arguments, step names, what goes in its folder"""
    p = []
    # (names are unique: each run has its own folder)
    for kind, f in (('gba', 'emerald.gba'), ('zip', 'emerald.zip'), ('gbz', 'emerald.gbz')):
        p.append(('load-' + kind, 'ROM loaded at a game\'s start: Emerald .%s (16 MB ROM, ROM buffer 16 MB)' % kind,
                  ['rom', rom(f), '16'], ['load_gamepak'], dict(roms=[f])))
    for game, title in (('emerald', 'Emerald (route, tall grass)'), ('aw', 'Advance Wars')):
        r = rom(game + '.gba')
        p.append(('state-save-' + game, 'State saved over a slot: %s' % title,
                  ['state-save', r, R_BIOS, 'documents/zgpSP_files/%s.gba0.svs.tns' % game],
                  ['copy', 'pack', 'picture', 'write', 'replace'], dict(roms=[game + '.gba'], states={game: [0]})))
        p.append(('state-load-' + game, 'State loaded: %s' % title, ['state-load', r, R_BIOS],
                  ['read', 'unpack', 'apply'], dict(roms=[game + '.gba'], states={game: [0]})))
    for form, what in (('gz', 'gzipped (Compress saves on)'), ('plain', 'plain .sav')):
        p.append(('sram-save-' + form,
                  'In-game save changed, written (update_backup_force), Emerald 128 KB flash, ' + what,
                  ['sram-save', R_EM, R_BIOS, 'documents/zgpSP_files/emerald.gba0.svs.tns', form],
                  ['update_backup_force'], dict(roms=['emerald.gba'], states={'emerald': [0]}, saves=['emerald'])))
        p.append(('sram-load-' + form, 'In-game save read at a game\'s start, Emerald, ' + what,
                  ['sram-load', R_EM], ['nspire_load_cartridge_backup'],
                  dict(roms=['emerald.gba'], sram_from='sram-save-' + form)))
    p.append(('sram-leave-twice', 'In-game save changed, written when leaving, then left again with no save '
              'in between (quit() after leaving), Emerald, gzipped',
              ['sram-save', R_EM, R_BIOS, 'documents/zgpSP_files/emerald.gba0.svs.tns', 'gz', 'twice'],
              ['update_backup_force', 'update_backup_force again'],
              dict(roms=['emerald.gba'], states={'emerald': [0]}, saves=['emerald'])))
    p.append(('convert-zip', 'ROM manager C: Emerald .gba zipped (level 6), read back, renamed', ['rom-zip', R_EM],
              ['rom_compress'], dict(roms=['emerald.gba'])))
    p.append(('convert-unzip', 'ROM manager U: Emerald .zip to .gba, read back, renamed',
              ['rom-unzip', rom('emerald.zip')], ['rom_uncompress'], dict(roms=['emerald.zip'])))
    p.append(('launch', 'gpSP starting, up to the file list', ['launch'],
              ['saves folder, old Documents files', 'old files next to gpSP', 'saves folder tidied',
               'file list start', 'settings read'],
              dict(roms=['emerald.gba', 'aw.gba'], states_from={'emerald': [0, 1, 2], 'aw': [0]},
                   saves=['emerald', 'aw'], config=True)))
    p.append(('config', 'Settings (all games: program file + copy; a game\'s own settings and keys)',
              ['config', R_EM], ['read (nspire_config_load)', 'written (nspire_config_save)',
                                 'game\'s own read', 'game\'s own written'], dict(roms=['emerald.gba'])))
    p.append(('start', 'A game\'s start, besides its ROM, save and state (Emerald, 3 states)',
              ['start', R_EM, R_BIOS], ['old files moved', 'game settings opened', 'BIOS read',
                                        'states looked up'],
              dict(roms=['emerald.gba'], states_from={'emerald': [0, 1, 2]}, saves=['emerald'], config=True)))
    p.append(('list', 'File list: the gba folder (5 games), then each game\'s details as the cursor passes',
              ['list', 'documents/gba'], ['folder read, sorted, ordered'] +
              ['details: %s' % g for g, _ in LIST_GAMES],
              dict(roms=[g + '.gba' for g, _ in LIST_GAMES], states_from={'emerald': [0, 1, 2], 'aw': [0]},
                   saves=['emerald', 'aw'], config=True)))
    ref = dict(roms=[g + '.gba' for g, _ in LIST_GAMES], ref=True, config=True)
    others = [rom('aw.gba'), rom('fe8.gba'), rom('gs.gba'), rom('minish.gba')]
    p.append(('menu-list', 'Game list (the reference saves folder: Emerald 18 states, AW 3, Minish 5, FE8 1, GS 0, '
              '66 files): opened, the picked game\'s details, the 4 others\' pictures; left and opened again',
              ['menu-list', 'documents/gba', rom('emerald.gba')] + others,
              ['folder read', 'picked: emerald (18 states)', 'others: aw, fe8, gs, minish',
               'again: folder read', 'again: picked emerald', 'again: the 4 others'], ref))
    p.append(('menu-paused', 'Paused menu in Emerald (the reference saves folder): Load, Save, Resume\'s other games, '
              'the save manager',
              ['menu-paused', rom('emerald.gba'), rom('aw.gba'), rom('minish.gba')],
              ['Load: newest and neighbours', 'Load: a slot further', 'menu again: Load', 'Save: its strip',
               'Resume up: AW', 'Resume up: Minish', 'Resume down: AW again', 'save manager opened',
               'manager: first state\'s pic', 'manager: next state\'s pic'], ref))
    p.append(('menu-start', 'Starting Emerald (the reference folder): the loading screen\'s picture, the states looked up',
              ['menu-start', rom('emerald.gba')], ['loading screen picture', 'states looked up'], ref))
    p.append(('quit', 'Exit gpSP after Advance Wars (save on exit\'s state save not included: state-save)',
              ['quit', R_AW, R_BIOS, 'documents/zgpSP_files/aw.gba0.svs.tns'],
              ['game save written (leaving)', 'game settings closed', 'settings saved', 'timings file',
               'last folder', 'game save written (quit)'],
              dict(roms=['aw.gba'], states={'aw': [0]}, saves=['aw'], config=True)))
    return p


ROM_SOURCES = {'emerald.gba': EMERALD, 'emerald.zip': EMERALD_ZIP, 'aw.gba': AW}
ROM_SOURCES.update({g + '.gba': f for g, f in LIST_GAMES})


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


def build():
    sh(['make', '-s', '-C', ARM, '-j%d' % JOBS, 'build/libbench'], stdout=subprocess.DEVNULL)
    plugin = os.path.join(ARM, 'build', 'arm926.so')
    src = os.path.join(ROOT, 'tools', 'calc_speed', 'arm926.c')
    if not os.path.exists(plugin) or os.path.getmtime(src) > os.path.getmtime(plugin):
        inc = os.path.join(os.path.dirname(os.path.dirname(calc_speed.QEMU)), 'include', 'qemu')
        glib = subprocess.run(['pkg-config', '--cflags', '--libs', 'glib-2.0'], capture_output=True,
                              text=True, check=True).stdout.split()
        sh(['gcc', '-O2', '-shared', '-fPIC', '-I' + inc, src, '-o', plugin] + glib)
    return plugin


def gbz_input():
    """Emerald as a .gbz, made by the project's own tool (tools/gbz.py)."""
    src = os.path.join(GAMES, EMERALD)
    out_dir = os.path.join(WORK, 'inputs')
    out = os.path.join(out_dir, 'Pokemon - Emerald Version (USA, Europe).gbz.tns')
    if not os.path.exists(out) or os.path.getmtime(out) < os.path.getmtime(src):
        os.makedirs(out_dir, exist_ok=True)
        sh([sys.executable, '-I', os.path.join(ROOT, 'tools', 'gbz.py'), 'pack', '-o', out_dir, src],
           stdout=subprocess.DEVNULL)
    return out


def saves_inputs():
    """The real in-game saves, copied into build/libbench/inputs once:
    {game: path}, and what the table says about them."""
    out_dir = os.path.join(WORK, 'inputs')
    got, notes = {}, []
    for game, name in SAVES.items():
        dst = os.path.join(out_dir, '%s.gba.sav.gz.tns' % game)
        if not os.path.exists(dst) and os.path.exists(os.path.join(SAVES_DIR, name)):
            os.makedirs(out_dir, exist_ok=True)
            shutil.copyfile(os.path.join(SAVES_DIR, name), dst)
        if os.path.exists(dst):
            got[game] = dst
            notes.append('%s %d B' % (name, os.path.getsize(dst)))
        else:
            notes.append('%s: none (blank save)' % name)
    return got, notes


def deps(spec):
    d = set(spec.get('states_from', {}) and ['state-save-' + g for g in spec['states_from']])
    if spec.get('config'):
        d.add('config')
    if spec.get('sram_from'):
        d.add(spec['sram_from'])
    if spec.get('ref'):
        d |= {'state-save-emerald', 'state-save-aw'}
    return d


def write_lists(top):
    """Each folder's names (libbench_os.c reads them: semihosting can't)."""
    for d, dirs, files in os.walk(top):
        names = sorted(n + '/' for n in dirs) + sorted(n for n in files if n != '.list')
        with open(os.path.join(d, '.list'), 'w') as f:
            f.write(''.join(n + '\n' for n in names))


def make_folder(name, model, spec, gbz, saves_in):
    run = os.path.join(WORK, 'run', '%s.%s' % (name, model.replace(' ', '')))
    shutil.rmtree(run, ignore_errors=True)
    gba, saves = os.path.join(run, 'documents', 'gba'), os.path.join(run, 'documents', 'zgpSP_files')
    os.makedirs(gba)
    os.makedirs(saves)
    for r in spec.get('roms', []):
        src = gbz if r == 'emerald.gbz' else os.path.join(GAMES, ROM_SOURCES[r])
        os.symlink(src, os.path.join(gba, r + '.tns'))
    os.symlink(os.path.join(GAMES, BIOS), os.path.join(saves, 'gba_bios.bin.tns'))
    for game, slots in spec.get('states', {}).items():
        for slot in slots:
            shutil.copyfile(os.path.join(GAMES, STATES[game]), os.path.join(saves, '%s.gba%d.svs.tns' % (game, slot)))
    # This build's states (with their pictures), as state-save wrote them.
    for game, slots in spec.get('states_from', {}).items():
        src = os.path.join(WORK, 'run', 'state-save-%s.CXII' % game, 'documents', 'zgpSP_files', '%s.gba0.svs.tns' % game)
        for slot in slots:
            shutil.copyfile(src, os.path.join(saves, '%s.gba%d.svs.tns' % (game, slot)))
    if spec.get('ref'):
        owner_folder(saves)
    for game in spec.get('saves', []):
        if game in saves_in:
            shutil.copyfile(saves_in[game], os.path.join(saves, '%s.gba.sav.gz.tns' % game))
    # The program, with room after it for the settings (nspire_selfstore.c).
    with open(os.path.join(gba, '_gpSP.tns'), 'wb') as f:
        f.write(bytes(PROGRAM_SIZE))
    if spec.get('config'):
        # gpsp.cfg.tns and the program's settings: made by the config path.
        src = os.path.join(WORK, 'run', 'config.CXII', 'documents')
        shutil.copyfile(os.path.join(src, 'zgpSP_files', 'gpsp.cfg.tns'), os.path.join(saves, 'gpsp.cfg.tns'))
        shutil.copyfile(os.path.join(src, 'gba', '_gpSP.tns'), os.path.join(gba, '_gpSP.tns'))
    if spec.get('sram_from'):
        src = os.path.join(WORK, 'run', spec['sram_from'] + '.CXII', 'documents', 'zgpSP_files')
        for n in os.listdir(src):
            if '.sav' in n:
                shutil.copyfile(os.path.join(src, n), os.path.join(saves, n))
    write_lists(os.path.join(run, 'documents'))
    return run


def owner_folder(saves):
    """The reference saves folder (OWNER_STATES, OWNER_OTHER) in saves."""
    import struct

    def state(src, dst, counter=None):
        data = bytearray(open(src, 'rb').read())
        if counter is not None:
            struct.pack_into('<I', data, 8, counter)
        with open(os.path.join(saves, dst), 'wb') as f:
            f.write(data)

    em = os.path.join(WORK, 'run', 'state-save-emerald.CXII', 'documents', 'zgpSP_files', 'emerald.gba0.svs.tns')
    aw = os.path.join(WORK, 'run', 'state-save-aw.CXII', 'documents', 'zgpSP_files', 'aw.gba0.svs.tns')
    for slot in range(19, 32):
        state(os.path.join(OWNER_STATES, 'Pokemon - Emerald Version (USA, Europe).gba%d.svs.tns' % slot),
              'emerald.gba%d.svs.tns' % slot)
    for slot in range(32, 37):
        state(em, 'emerald.gba%d.svs.tns' % slot, 323 + slot)
    for slot in range(3):
        state(aw, 'aw.gba%d.svs.tns' % slot, slot + 1)
    for slot in range(5):
        state(aw, 'minish.gba%d.svs.tns' % slot, slot + 1)
    state(em, 'fe8.gba0.svs.tns', 1)
    for name in OWNER_OTHER:
        path = os.path.join(saves, name)
        if not os.path.exists(path):
            with open(path, 'wb') as f:
                f.write(b'x' * 64)


def run_path(item, plugin, syms, gbz, saves_in):
    (name, title, args, steps, spec), (model, mhz, model_args) = item
    run = make_folder(name, model, spec, gbz, saves_in)
    by_name = {n: a for a, _, n in syms}
    regions = os.path.join(run, 'regions')
    with open(regions, 'w') as f:
        for a, _, n in syms:
            if n.startswith('os_'):
                f.write('%x os\n' % a)
    out = os.path.join(run, 'model.out')
    pa = '%s,mark=%x,regions=%s,out=%s,last=%d' % (plugin, by_name['perf_frame_mark'], regions, out, len(steps))
    if model_args:
        pa += ',' + model_args
    # A fixed command line and no environment: the guest's stack starts at
    # the same place every run (deterministic cycle counts).
    cmd = ['env', '-i', calc_speed.QEMU, '-cpu', 'arm926', '-plugin', pa,
           os.path.relpath(ELF, run)] + args
    res = subprocess.run(cmd, cwd=run, capture_output=True, text=True)
    if res.returncode != 0 or not res.stdout.rstrip().endswith('ok'):
        sys.exit('libbench %s (%s) failed (exit %d):\n%s%s' % (name, model, res.returncode, res.stdout,
                                                                 res.stderr))
    return name, model, out, res.stdout


# ---- Where the cycles went ----

def code_owners(map_path):
    """(start, end, library, member) of every input section in the link map."""
    holders = []
    pending = None
    rx = re.compile(r'^ (\.\S+)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$')
    in_map = False
    for line in open(map_path):
        if line.startswith('Linker script and memory map'):
            in_map = True
            continue
        if not in_map:
            continue
        m = rx.match(line.rstrip('\n'))
        if m:
            sec = m.group(1) or pending
            pending = None
            start, size, src = int(m.group(2), 16), int(m.group(3), 16), m.group(4).strip()
            if size and sec and sec.startswith('.text'):
                holders.append((start, start + size, src))
            continue
        s = line.strip()
        pending = s if s.startswith('.') and ' ' not in s else None
    holders.sort()
    return holders


def library(src):
    m = re.search(r'lib(\w+?)\.a\(([^)]*)\)', src)
    if m:
        member = re.sub(r'^lib\w_a-|\.o$', '', m.group(2))
        return {'c': 'newlib', 'z': 'zlib', 'gcc': 'libgcc', 'm': 'libm'}.get(m.group(1), m.group(1)), member
    m = re.search(r'zlib_(\w+)\.o$', src)
    if m:
        return 'zlib', m.group(1)
    if src.endswith('libbench_os.o'):
        return 'libsyscalls', ''
    return 'gpSP', ''


STDIO = set('fclose fflush fgets findfp flags fopen fputc fputs fread fseek fseeko ftell ftello fvwrite fwalk '
            'fwrite makebuf refill remove rename stdio ungetc wbuf wsetup putc getc closer fstatr isattyr '
            'lseekr openr readr renamer statr unlinkr writer sysclose syslseek sysopen sysread sysstat syswrite'
            .split())
PRINTF = set('printf fprintf fiprintf snprintf sprintf sprint_r ssprint_r ssputs_r svfprintf vfprintf vfiprintf '
             'vsnprintf dtoa mprec siscanf svfiscanf sccl sscanf'.split())


def group(lib, member, fn):
    if lib == 'zlib':
        if fn in ('inflate_fast', 'inflate_table', 'longest_match', 'fill_window', 'adler32_z', 'crc32_little',
                  'deflate_fast', 'deflate_slow'):
            return 'zlib ' + {'adler32_z': 'adler32', 'crc32_little': 'crc32'}.get(fn, fn)
        if member in ('inflate', 'inffast', 'inftrees', 'uncompr', 'infback'):
            return 'zlib inflate, other (inflate, updatewindow...)'
        if member == 'trees':
            return 'zlib trees (compress_block, build_tree...)'
        if member in ('deflate', 'compress'):
            return 'zlib deflate, other'
        if member in ('adler32', 'crc32'):
            return 'zlib ' + member
        return 'zlib other'
    if lib == 'newlib':
        if fn in ('memcpy', 'memmove', 'memset', 'memcmp'):
            return 'newlib ' + fn
        if member in STDIO:
            return 'newlib stdio (fread, fwrite, buffers...)'
        if member in PRINTF:
            return 'newlib printf/scanf'
        if member.startswith('str') or member in ('ctype_', 'atoi', 'memchr-stub', 'strlen-stub'):
            return 'newlib strings'
        return 'newlib other (qsort...)'
    if lib == 'libsyscalls':
        return 'libsyscalls (newlib to the OS)'
    if lib in ('libgcc', 'libm'):
        return lib
    return 'gpSP (its own code)'


def analyse(out, syms, holders):
    lines = open(out).read().split('\n')
    names = lines[2].split()[1:]
    n = len(names)
    os_region = names.index('os') if 'os' in names else -1
    frames = []
    for line in lines:
        if line.startswith('frame'):
            v = [int(x) for x in line.split()[1:]]
            frames.append(sum(c for i, c in enumerate(v[:n]) if i != os_region))
    addrs = [a for a, _, _ in syms]
    starts = [o[0] for o in holders]
    funcs = collections.Counter()
    for line in lines[3:]:
        p = line.split()
        if len(p) != 7 or int(p[1]) == os_region:
            continue
        pc = int(p[0], 16)
        i = bisect.bisect_right(addrs, pc) - 1
        fn = '?'
        if i >= 0:
            a, size, sym = syms[i]
            fn = sym if not size or pc < a + size else '?'
        j = bisect.bisect_right(starts, pc) - 1
        src = holders[j][2] if j >= 0 and pc < holders[j][1] else ''
        funcs[(fn, src)] += int(p[3])
    return frames, funcs


def ms(cycles, mhz):
    return cycles * calc_speed.SCALE / (mhz * 1000.0)


def calls_of(stdout):
    """{step: [(op, size, count)]} from the bench's 'calls' lines."""
    calls = collections.defaultdict(list)
    for line in stdout.split('\n'):
        if line.startswith('calls '):
            step, rest = line[6:].split(' ', 1)
            op, tail = rest.split('|')
            size, count = tail.split()
            calls[int(step)].append((op, int(size), int(count)))
    return calls


def describe_calls(entries):
    """'fread 128 x 131072 B' style, OS calls apart."""
    by_op = collections.OrderedDict()
    for op, size, count in entries:
        by_op.setdefault(op, []).append((size, count))
    out = []
    for op, sizes in by_op.items():
        total = sum(c for _, c in sizes)
        if op == 'fopen':
            detail = ', '.join('%d "%s"' % (c, chr(s)) for s, c in sizes)
            out.append('fopen %d (%s)' % (total, detail))
        elif op == 'fseek':
            detail = ', '.join('%d %s' % (c, ('SET', 'CUR', 'END')[s] if s < 3 else s) for s, c in sizes)
            out.append('fseek %d (%s)' % (total, detail))
        elif op in ('fread', 'fwrite', 'OS fread', 'OS fwrite', 'OS malloc', 'OS realloc', 'OS memset'):
            sizes = sorted(sizes, key=lambda sc: (-sc[1] * max(sc[0], 1), -sc[0]))
            nbytes = sum(s * c for s, c in sizes)
            shown = ', '.join('%dx%d' % (c, s) for s, c in sizes[:6]) + (', ...' if len(sizes) > 6 else '')
            out.append('%s %d, %s B (%s)' % (op, total, '{:,}'.format(nbytes), shown))
        else:
            out.append('%s %d' % (op, total))
    return out


def baseline(path):
    """'data' lines of an earlier table: {(path, step, model): cycles}."""
    base = {}
    if not path:
        return base
    for line in open(path):
        if line.startswith('data\t'):
            _, name, step, model, cycles = line.rstrip('\n').split('\t')
            base[(name, step, model)] = int(cycles)
    return base


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit('usage: tests/libbench.sh LABEL [BASELINE.txt]')
    label = sys.argv[1]
    base = baseline(sys.argv[2] if len(sys.argv) == 3 else None)
    for f in [EMERALD, EMERALD_ZIP, AW, BIOS] + [g for _, g in LIST_GAMES] + list(STATES.values()):
        if not os.path.exists(os.path.join(GAMES, f)):
            sys.exit('libbench needs %s in %s ($GBA_GAMES)' % (f, GAMES))
    plugin = build()
    syms = calc_speed.symbols(ELF)
    holders = code_owners(ELF + '.map')
    gbz = gbz_input()
    saves_in, saves_notes = saves_inputs()
    paths = path_list()
    assert len(set(p[0] for p in paths)) == len(paths)
    results = {}
    done = set()
    with concurrent.futures.ThreadPoolExecutor(JOBS) as pool:
        while len(done) < len(paths):
            # the paths whose inputs (other paths' files) are made
            wave = [p for p in paths if p[0] not in done and deps(p[4]) <= done]
            items = [(p, m) for p in wave for m in MODELS]
            for name, model, out, stdout in pool.map(lambda it: run_path(it, plugin, syms, gbz, saves_in), items):
                results[(name, model)] = (analyse(out, syms, holders), stdout)
            done |= set(p[0] for p in wave)

    lines = []
    w = lines.append
    data = []
    delta = bool(base)

    def cell(name, step, model, cycles, mhz):
        data.append('data\t%s\t%s\t%s\t%d' % (name, step, model, cycles))
        s = '%8.1f' % ms(cycles, mhz)
        if delta:
            b = base.get((name, step, model))
            s += ' %+7.1f' % (ms(cycles - b, mhz)) if b is not None else '     new'
        return s

    w('gpSP load/save benchmark (tests/libbench.sh), label %s' % label)
    w('Estimated ms on the calculator (ARM926 model x %.2f): CX II at 396 MHz, older CX at 132 MHz (memory'
      % calc_speed.SCALE)
    w('timings scaled to the same ns: an estimate, the CX was never measured). CPU time only: the OS\'s file')
    w('system and heap are not modeled (their calls and bytes are counted instead). Deterministic: the same')
    w('build gives the same numbers.' + (' Deltas against %s (ms, + is slower).' % sys.argv[2] if delta else ''))
    w('Inputs: $GBA_GAMES (Emerald .gba and .zip, Advance Wars, the states), Emerald .gbz by tools/gbz.py,')
    w('in-game saves: %s.' % '; '.join(saves_notes))
    w('')
    def count(name, what, n):
        data.append('data\t%s\t%s\tcount\t%d' % (name, what, n))
        s = '%7d' % n
        if delta:
            b = base.get((name, what, 'count'))
            s += ' %+6d' % (n - b) if b is not None else '    new'
        return s

    def file_calls(stdout):
        """OS file calls (not the heap's) and the bytes the OS read and wrote"""
        n = nbytes = 0
        for entries in calls_of(stdout).values():
            for op, size, c in entries:
                if op.startswith('OS ') and op not in ('OS malloc', 'OS free', 'OS realloc', 'OS memset'):
                    n += c
                    if op in ('OS fread', 'OS fwrite'):
                        nbytes += size * c
        return n, nbytes

    def fs_cell(name, step, entries):
        """the file system's estimated ms (OS_MS), CX II and CX"""
        t = os_ms(entries)
        data.append('data\t%s\t%s\tfs\t%d' % (name, step, round(t * 1000)))
        s = '%7.1f %7.1f' % (t, t * OS_CX)
        if delta:
            b = base.get((name, step, 'fs'))
            s += ' %+7.1f' % ((t * 1000 - b) / 1000.0) if b is not None else '     new'
        return s

    if delta:
        w('%-24s %8s %7s %8s %7s %7s %6s %9s %7s %7s %7s' % ('path', 'CX II', 'delta', 'CX', 'delta', 'OS', 'delta',
                                                          'OS KB r+w', 'FS CXII', 'FS CX', 'delta'))
    else:
        w('%-24s %8s %8s %7s %9s %7s %7s' % ('path', 'CX II', 'CX', 'OS', 'OS KB r+w', 'FS CXII', 'FS CX'))
    for name, title, args, steps, spec in paths:
        row = '%-24s' % name
        for model, mhz, _ in MODELS:
            (frames, _), _ = results[(name, model)]
            row += ' ' + cell(name, 'total', model, sum(frames), mhz)
        n, nbytes = file_calls(results[(name, 'CX II')][1])
        row += ' %s %9.0f' % (count(name, 'OS file calls', n), nbytes / 1024.0)
        row += ' ' + fs_cell(name, 'total', [e for es in calls_of(results[(name, 'CX II')][1]).values() for e in es])
        w(row)
    w('(ms; OS: the file calls the OS gets (fopen, fread, fwrite, fseek, ftell, fclose, stat, rename, unlink,')
    w(' folders), and the KB it reads and writes, which take the file system\'s time on top; FS: that time')
    w(' estimated from the calls (OS_MS in tests/libbench.py, fitted on the reference CX II timings; the CX')
    w(' %.1f times it)' % OS_CX)
    w('')

    # The calculator's own numbers against the model's (CX II).
    w('Against the calculator (CX II, its gpsp_states_time.txt.tns: the reference Emerald state, here the')
    w('route state; real/model for the CPU parts, real - model for the file parts = the OS\'s file system):')
    w('  %-28s %8s %8s %11s' % ('part', 'model', 'real', ''))
    for (name, step), real in REAL.items():
        (frames, _), _ = results[(name, 'CX II')]
        i = [p for p in paths if p[0] == name][0][3].index(step)
        model_ms = ms(frames[i], 396.0)
        if step in ('copy', 'pack', 'picture', 'unpack', 'apply'):
            w('  %-28s %8.1f %8.1f %6.2f real/model' % (name + ' ' + step, model_ms, real, real / model_ms))
        else:
            w('  %-28s %8.1f %8.1f %6.1f ms file system' % (name + ' ' + step, model_ms, real, real - model_ms))
    w('')

    for name, title, args, steps, spec in paths:
        w('== %s: %s' % (name, title))
        w('   libbench %s' % ' '.join(args))
        (frames, funcs), stdout = results[(name, 'CX II')]
        (frames_cx, funcs_cx), _ = results[(name, 'CX')]
        calls = calls_of(stdout)
        if delta:
            w('   %-36s %8s %7s %8s %7s  %7s %7s %7s' % ('step', 'CX II', 'delta', 'CX', 'delta', 'FS CXII',
                                                       'FS CX', 'delta'))
        else:
            w('   %-36s %8s %8s  %7s %7s' % ('step', 'CX II', 'CX', 'FS CXII', 'FS CX'))
        for i, step in enumerate(steps):
            w('   %-36s %s %s  %s' % (step[:36], cell(name, step, 'CX II', frames[i], 396.0),
                                      cell(name, step, 'CX', frames_cx[i], 132.0),
                                      fs_cell(name, step, calls.get(i + 1, []))))
            for c in describe_calls(calls.get(i + 1, [])):
                w('       %s' % c)
        w('   %-36s %s %s' % ('total', cell(name, 'total', 'CX II', sum(frames), 396.0),
                               cell(name, 'total', 'CX', sum(frames_cx), 132.0)))
        groups = collections.Counter()
        groups_cx = collections.Counter()
        for (fn, src), cycles in funcs.items():
            lib, member = library(src)
            groups[group(lib, member, fn)] += cycles
        for (fn, src), cycles in funcs_cx.items():
            lib, member = library(src)
            groups_cx[group(lib, member, fn)] += cycles
        total = max(1, sum(groups.values()))
        w('   by library and function (ms, share of the CX II time):')
        for g, cycles in groups.most_common():
            if ms(cycles, 396.0) < 0.05 and ms(groups_cx[g], 132.0) < 0.05:
                continue
            w('     %-46s %s %s %5.1f%%' % (g, cell(name, 'group ' + g, 'CX II', cycles, 396.0),
                                            cell(name, 'group ' + g, 'CX', groups_cx[g], 132.0),
                                            100.0 * cycles / total))
        w('   top functions (CX II ms):')
        dem = calc_speed.demangle([fn for fn, _ in funcs])
        top = collections.Counter()
        for (fn, src), cycles in funcs.items():
            lib, _ = library(src)
            top['%s [%s]' % (dem.get(fn, fn), lib)] += cycles
        for fn, cycles in top.most_common(8):
            if ms(cycles, 396.0) >= 0.05:
                w('     %-46s %8.2f' % (fn[:46], ms(cycles, 396.0)))
        made = [l for l in stdout.split('\n') if l and not l.startswith('calls ')]
        w('   made: %s' % ' | '.join(made))
        w('')
    seen = set()
    lines.extend(d for d in data if not (d in seen or seen.add(d)))
    os.makedirs(PERF, exist_ok=True)
    out = os.path.join(PERF, 'libbench-%s.txt' % label)
    with open(out, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('\n'.join(l for l in lines if not l.startswith('data\t')))
    print('(written to %s)' % os.path.relpath(out, ROOT))


if __name__ == '__main__':
    main()
