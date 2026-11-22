#!/usr/bin/env python3
"""Writes the game's function pins into nspire_lineio.ld (where the
game's code is pinned): every function the speed model runs, and every one of
the hot stretch (update_gba ... arm_stub.S's nspire_hot_end), at the place in
a 4 KB instruction cache way it has in a reference build, in that build's
order.

    python3 -I tools/pin_layout.py REF.elf LISTINGS_DIR

Run in the top folder after a build (it reads gpsp_libretro.elf and
gpsp_libretro.map for the sections' names: -ffunction-sections at the link,
NSPIRE_PINNED_LAYOUT); LISTINGS_DIR holds tests/speed_table.sh's SCENE-CALC.txt
of the reference (the functions each scene runs). Then build again:
tests/calc_checks.py checks every pin, tests/layout_pinned.py that cold code
moves none of them.

A pin is '. = ALIGN(...) + OFFSET;' then the section: the function starts
the next address at OFFSET in a way (a gap of zeros before it, free in the
packed file). The hot stretch's pins never jump a way ('>= . ?'): a function
that grew pushes the next one a few bytes, the stretch stays one stretch
(the CX II's on-chip copy, nspire_sram.c, takes it whole).
"""
import glob
import os
import re
import subprocess
import sys

BEGIN = '  /* pins: tools/pin_layout.py */\n'
END = '  /* end of pins */\n'


def model_hot(ldir):
    hot = {}
    for f in glob.glob(os.path.join(ldir, '*.txt')):
        for l in open(f):
            m = re.match(r'(\S+)\s+(.*?)\s+(\d+)\s+(\d+)\s+([\d.]+)', l)
            if m and not l.startswith('part') and not l.startswith('/'):
                n = m.group(2).strip()
                hot[n] = max(hot.get(n, 0), int(m.group(4)))
    return hot


def nm(elf):
    """(address, name, demangled name, size) of every function."""
    raw = subprocess.run(['arm-none-eabi-nm', '-n', elf], capture_output=True, text=True, check=True).stdout
    dem = subprocess.run(['arm-none-eabi-nm', '-n', '-C', elf], capture_output=True, text=True, check=True).stdout
    sized = subprocess.run(['arm-none-eabi-nm', '-S', elf], capture_output=True, text=True, check=True).stdout
    size = {}
    for l in sized.splitlines():
        p = l.split()
        if len(p) == 4:
            size[p[3]] = int(p[1], 16)
    out = []
    for r, d in zip(raw.splitlines(), dem.splitlines()):
        m = re.match(r'([0-9a-f]+) ([tTwW]) (\S+)$', r)
        if m:
            out.append((int(m.group(1), 16), m.group(3), d.split(' ', 2)[2], size.get(m.group(3), 0)))
    return out


def input_sections(mapfile):
    secs, cur = [], None
    for l in open(mapfile):
        m = re.match(r'^ (\.text\S*)\s*$', l)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r'^ (\.text\S*)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)$', l)
        if m and (m.group(1) or cur):
            secs.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1) or cur, m.group(4)))
        cur = None
    return secs


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.split('\n\n')[1])
    ref, ldir = sys.argv[1:]
    hot = model_hot(ldir)

    def runs(d):   # the listings cut names at 60 characters
        return d in hot or any(len(h) >= 40 and d.startswith(h[:45]) for h in hot)
    R = nm(ref)
    N, NSIZE = {}, {}
    for a, raw, d, size in nm('gpsp_libretro.elf'):
        N.setdefault(raw, a)
        NSIZE.setdefault(raw, size)
    strip = lambda n: re.sub(r'\.lto_priv\.\d+', '', n)
    by_strip = {}
    for n in N:
        by_strip.setdefault(strip(n), []).append(n)
    secs = input_sections('gpsp_libretro.map')
    ug = next(a for a, raw, d, size in R if raw == 'update_gba')
    he = next(a for a, raw, d, size in R if raw == 'nspire_hot_end')
    pins, seen, missing = [], set(), []
    for a, raw, d, rsize in R:
        if a < 0x3200 or a >= 0x400000:          # crt0, the entry stub, the per-line I/O; .jit
            continue
        if not (ug <= a < he or runs(d)):
            continue
        name = raw if raw in N else (by_strip[strip(raw)][0] if len(by_strip.get(strip(raw), [])) == 1 else None)
        sec = next(((s, n, f) for s, size, n, f in secs if name in N and s <= N[name] < s + size), None)
        if sec is None:
            missing.append(raw)
            continue
        start, sname, f = sec
        if (sname, f) in seen:
            continue
        seen.add((sname, f))
        off = (a - (N[name] - start)) & 0xfff
        if ug < a < he and '.ltrans' in f and NSIZE.get(name, 0) > rsize:
            # A function of the stretch that grew ends where it ended (the
            # next one keeps its place) if the one before leaves room.
            off = (off - ((NSIZE[name] - rsize + 3) & ~3)) & 0xfff
        fname = re.sub(r'^.*/', '', f)
        if '.ltrans' in fname:
            spec = '*(%s)' % sname
        elif '(' in fname:
            spec = '*%s(%s)' % (fname.replace('(', ':').rstrip(')'), sname)
        else:
            spec = '*%s(%s)' % (fname, sname)
        pins.append((a, off, spec))
    pins.sort()
    text = ''
    for a, off, spec in pins:
        if ug < a < he:
            text += '  . = . - (. & 0xfff) + 0x%03x >= . ? . - (. & 0xfff) + 0x%03x : .;\n' % (off, off)
        else:
            text += '  . = ALIGN(. + 0x1000 - 0x%03x, 0x1000) - 0x1000 + 0x%03x;\n' % (off, off)
        text += '  %s\n' % spec
    ld = open('nspire_lineio.ld').read()
    i, j = ld.index(BEGIN) + len(BEGIN), ld.index(END)
    open('nspire_lineio.ld', 'w').write(ld[:i] + text + ld[j:])
    print('pin_layout: %d pins written; not in this build: %s' % (len(pins), ', '.join(missing) or 'none'))


if __name__ == '__main__':
    main()
