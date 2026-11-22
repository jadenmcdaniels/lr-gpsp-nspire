#!/usr/bin/env python3
"""The speed model runs the calculator's layout.

The ARM926's caches pick a line's set from its address modulo a way (16 KB
4-way instruction cache: 4 KB; 8 KB 4-way data cache: 2 KB). Two builds of
the same code are timed alike only if their code and data sit at the same
addresses modulo 4 KB. The calculator's build (gpsp_libretro.elf, Ndless's
gold and ldscript) and the model's (tests/arm perf: the same core with
perf.c instead of the frontend's main, BFD ld) place everything differently,
and LTO splits the hot section into other pieces in each.

So the model is linked twice: once with -ffunction-sections (every function
its own input section) and a map, then with a section ordering file this
script writes, which puts every input section the two builds share at the
calculator's address modulo 4 KB: code, and data too when the model is
linked with -fdata-sections (tests/arm/Makefile MIRROR_DATA=1, the
default: each variable its own section; it costs the model ARM's section
anchors, +0.7% game instructions against the calculator's code). With
MIRROR_DATA=0 LTO's data (the frontend's buffers, the renderer's tables)
stays where it falls. arm_stub.S pins the GBA's memory and the dynarec's
caches in both builds.

    mirror_layout.py write CALC_ELF MODEL_MAP MODEL_ELF OUT.ld
    mirror_layout.py check CALC_ELF MODEL_ELF        (what landed where)

A section is matched by its first symbol that the calculator's ELF has too
(the same name, or the same name before LTO's ".lto_priv.N"/".constprop.N"
suffixes when that is unique on both sides). Everything else (perf.c, the
model's stubs, the C library) follows. Without CALC_ELF ("-") it writes the
model's own pinned layout: the hot functions from a 4 KB way on, as
common.h's NSPIRE_HOT_FIRST does in the calculator build.

Only the place relative to a 4 KB way is copied: Ndless loads the program
wherever the OS's malloc puts it (the layout line of gpsp_perf.txt.tns), so
the calculator's absolute addresses vary from launch to launch; the parts'
places against each other don't, and those are what this keeps.
"""

import re
import subprocess
import sys

WAY = 0x1000
OUT_SECTIONS = ('.text', '.rodata', '.data', '.bss')
SUFFIX = re.compile(r'\.(lto_priv|constprop|isra|part|cold|localalias)\.?\d*.*$')

# Without the calculator's ELF: the hot code as common.h's NSPIRE_HOT puts it
# there (update_gba first, at the start of a way), then arm_stub.S's hot part.
OWN_HOT = ['update_gba', 'execute_spsr_restore_body', 'execute_store_cpsr_body',
           'block_lookup_translate_thumb', 'block_lookup_address_thumb', 'block_lookup_translate_arm',
           'block_lookup_address_arm', 'set_cpu_mode', 'check_and_raise_interrupts', 'check_interrupt',
           'flag_interrupt', 'nspire_video_line_ready', 'nspire_flush_lines', 'sound_timer_queue32',
           'sound_timer', 'serial_next_event', 'update_serial', 'serialfe_update', 'serialpoke_update',
           'serialaw_update', 'rfu_update', 'serialmsio_update']


def nm(elf):
    """name -> address (names defined twice, such as two files' statics, are
    left out: they can't be matched)."""
    out = subprocess.run(['arm-none-eabi-nm', '--defined-only', elf], capture_output=True, text=True,
                         check=True).stdout
    seen, dup = {}, set()
    for line in out.splitlines():
        p = line.split()
        if len(p) != 3 or p[1] in 'aAUuNn' or p[2].startswith('$'):
            continue
        a, name = int(p[0], 16), p[2]
        if name in seen and seen[name] != a:
            dup.add(name)
        seen[name] = a
    for name in dup:
        del seen[name]
    return seen


def by_base(names):
    base = {}
    for n in names:
        base.setdefault(SUFFIX.sub('', n), []).append(n)
    return {b: v[0] for b, v in base.items() if len(v) == 1}


def parse_map(path):
    """The input sections of the model's link: (output section, name, address,
    size, file), in address order."""
    out, cur_out, pending = [], None, None
    started = False
    for line in open(path, errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('Linker script and memory map'):
            started = True
            continue
        if not started:
            continue
        m = re.match(r'^(\.\S+)(\s+0x([0-9a-f]+)\s+0x([0-9a-f]+))?', line)
        if m:
            cur_out = m.group(1)
            pending = None
            continue
        m = re.match(r'^ (\.\S+|COMMON)\s*$', line)
        if m:
            pending = m.group(1)
            continue
        m = re.match(r'^ (\.\S+|COMMON)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$', line)
        if m:
            out.append((cur_out, m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)))
            pending = None
            continue
        m = re.match(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$', line)
        if m and pending:
            out.append((cur_out, pending, int(m.group(1), 16), int(m.group(2), 16), m.group(3)))
            pending = None
            continue
        pending = None
    return [s for s in out if s[0] in OUT_SECTIONS and s[3] > 0]


def holders(path):
    """name -> (input section, its address) for the symbols the model's map
    lists under an input section (global ones)."""
    out, cur = {}, None
    for line in open(path, errors='replace'):
        m = re.match(r'^ (\.\S+|COMMON)(\s+0x([0-9a-f]+)\s+0x[0-9a-f]+\s+\S.*)?$', line)
        if m:
            cur = (m.group(1), int(m.group(3), 16)) if m.group(3) else (m.group(1), None)
            continue
        m = re.match(r'^\s+0x([0-9a-f]+)\s+0x[0-9a-f]+\s+\S', line)
        if m and cur and cur[1] is None:
            cur = (cur[0], int(m.group(1), 16))
            continue
        m = re.match(r'^\s+0x([0-9a-f]+)\s+([A-Za-z_.$][\w.$]*)\s*$', line)
        if m and cur:
            out[m.group(2)] = cur
    return out


def pattern(sec, file, count):
    """How the ordering file names this input section (None: it can't)."""
    if count[sec] == 1:
        return '*(%s)' % sec
    if '.ltrans' in file or file.startswith('/tmp'):
        return None   # LTO's temporary objects: no name to give
    m = re.match(r'^.*/([^/(]+\.a)\(([^)]+)\)$', file)
    if m:
        return '*%s:%s(%s)' % (m.group(1), m.group(2), sec)   # a library member
    return '*%s(%s)' % (file.split('/')[-1], sec)


def write(calc_elf, model_map, model_elf, out_path):
    sections = parse_map(model_map)
    placed = holders(model_map)
    model = nm(model_elf)
    addrs = sorted((a, n) for n, a in model.items())
    count = {}
    for s in sections:
        count[s[1]] = count.get(s[1], 0) + 1
    entries = {o: [] for o in OUT_SECTIONS}
    if calc_elf != '-':
        calc = nm(calc_elf)
        calc_base = by_base(calc)
        import bisect
        keys = [a for a, _ in addrs]
        tables = []
        for out_sec, sec, a, size, file in sections:
            pat = pattern(sec, file, count)
            if not pat:
                continue
            i = bisect.bisect_left(keys, a)
            anchor = None
            while i < len(addrs) and addrs[i][0] < a + size:
                n = addrs[i][1]
                if n.startswith('layout_pad_'):
                    break   # tests/layout_moves.sh's pads: a move, not copied
                if placed.get(n, (sec, a)) != (sec, a):
                    # (speed10: a label the map puts at another section's
                    # end, at this one's start: arm_stub.S's nspire_hot_end
                    # before .text.hot.gpsp.fast. Not this section's.)
                    i += 1
                    continue
                cn = n if n in calc else calc_base.get(SUFFIX.sub('', n))
                if cn is not None and cn in calc:
                    anchor = (n, addrs[i][0] - a, calc[cn])
                    break
                i += 1
            if anchor is None:
                if out_sec == '.rodata' and sec.startswith('.rodata.') and sec[len('.rodata.'):] in calc:
                    tables.append((sec, size, pat))
                continue
            n, off, ca = anchor
            entries[out_sec].append((ca, (ca - off) % WAY, pat, n))
        # speed10: a function's own constants (GCC's .rodata.FUNCTION: a
        # switch's jump table) have no symbol to match, and the
        # calculator's are other bytes (its code differs a little: a byte
        # table there, halfwords here). They fell where the unplaced
        # sections did and moved with any of them (update_gba's serial
        # switch, Emerald's wild battle wipe +4% in the model with the
        # calculator's place unmoved): each now at a place of its own in a
        # way, from its name, the same in every build.
        import zlib
        for sec, size, pat in tables:
            mod = (zlib.crc32(sec.encode()) % (WAY // 64)) * 64
            entries['.rodata'].append((1 << 32, mod, pat, sec))
    else:
        for n in OWN_HOT:
            if '.text.' + n in count:
                entries['.text'].append((len(entries['.text']), 0 if n == 'update_gba' else None,
                                         '*(.text.%s)' % n, n))
        hot = [s for s in sections if s[1] == '.text.hot.gpsp']
        if hot:
            entries['.text'].append((len(entries['.text']), None, pattern('.text.hot.gpsp', hot[0][4], count),
                                     'arm_stub hot'))
    with open(out_path, 'w') as f:
        f.write('/* Written by tests/arm/mirror_layout.py from %s: each input section at the\n'
                '   calculator build\'s address modulo 4 KB. Do not edit. */\n' % calc_elf)
        for out_sec in OUT_SECTIONS:
            ents = sorted(entries[out_sec])
            if out_sec == '.bss':
                # layout_pad.S's data pad (tests/layout_moves.sh), when linked:
                # after what is placed here, so LTO's data after it moves.
                ents.append((None, None, '*(.bss.layout_pad)', 'layout_pad.S'))
            if not ents:
                continue
            f.write('%s : {\n' % out_sec)
            for _, mod, pat, n in ents:
                if mod is not None:
                    # Section-relative (the output sections start a way:
                    # layout_anchor.S): ABSOLUTE(.) here is re-read over
                    # ld's sizing passes and the gaps grow to 40 KB.
                    f.write('  . = ALIGN(. + 0x%x - 0x%03x, 0x%x) - 0x%x + 0x%03x;\n'
                            % (WAY, mod, WAY, WAY, mod))
                f.write('  %s   /* %s */\n' % (pat, n))
            f.write('}\n')
    n = sum(len(v) for v in entries.values())
    print('mirror_layout: %d sections placed (%s)' % (n, ', '.join('%s %d' % (o, len(entries[o]))
                                                                   for o in OUT_SECTIONS)))


def check(calc_elf, model_elf, quiet=False):
    """Every symbol both builds have: the same place in a 4 KB way? Prints
    the share of code that is, weighted by size."""
    def sized(elf):
        out = subprocess.run(['arm-none-eabi-nm', '-S', '--defined-only', elf], capture_output=True,
                             text=True, check=True).stdout
        d, dup = {}, set()
        for line in out.splitlines():
            p = line.split()
            if len(p) == 4 and p[2] in 'tTdDbBrRwW':
                if p[3] in d:
                    dup.add(p[3])
                d[p[3]] = (int(p[0], 16), int(p[1], 16), p[2].lower())
        for x in dup:
            del d[x]
        return d
    calc, model = sized(calc_elf), sized(model_elf)
    cb, mb = by_base(calc), by_base(model)
    tot = {'t': [0, 0], 'd': [0, 0]}
    missed = []
    for n, (a, size, t) in model.items():
        cn = n if n in calc else cb.get(SUFFIX.sub('', n)) if mb.get(SUFFIX.sub('', n)) == n else None
        if cn is None or cn not in calc:
            continue
        kind = 't' if t == 't' else 'd'
        same = (a - calc[cn][0]) % WAY == 0
        tot[kind][0] += size
        tot[kind][1] += size if same else 0
        if not same:
            missed.append((size, n, a % WAY, calc[cn][0] % WAY, kind))
    for kind, label in (('t', 'code'), ('d', 'data')):
        all_, same = tot[kind]
        print('mirror_layout: %s both builds have: %d bytes, %.1f%% at the calculator\'s place in a 4 KB way'
              % (label, all_, 100.0 * same / max(1, all_)))
    if not quiet:
        for kind in ('t', 'd'):
            for size, n, ma, ca, _ in sorted((m for m in missed if m[4] == kind), reverse=True)[:8]:
                print('  not mirrored: %-60s %6d bytes (model %03x, calculator %03x)' % (n[:60], size, ma, ca))
    return tot


def main():
    if len(sys.argv) == 6 and sys.argv[1] == 'write':
        write(*sys.argv[2:6])
    elif len(sys.argv) in (4, 5) and sys.argv[1] == 'check':
        check(sys.argv[2], sys.argv[3], quiet=len(sys.argv) == 5)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
