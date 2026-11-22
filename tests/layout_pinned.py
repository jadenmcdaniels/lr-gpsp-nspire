#!/usr/bin/env python3
"""The game's code and data stay where nspire_lineio.ld puts them when cold
code changes:

    python3 -I tests/layout_pinned.py [FILE.c ...]   (top folder, toolchain on PATH)

Builds the calculator program, then again with 4 KB of code and 4 KB of
data (and a string) added to each FILE (default gui.c: a cold, -Os file;
nspire_ui.c and nspire_frameskip.c are frontend files at -O3), each put
back after; then compares
the place of every symbol of the game (in the link map: LTO's objects, the
core's assembly, the pinned library pieces). Equal, or moved by whole 4 KB
ways (the caches can't tell those apart: .bss after a grown .data), passes.
"""
import os
import re
import shutil
import subprocess
import sys

# GPSP_LAYOUT_ROOT: a copy of the tree to build in (tests/gate.sh: this test changes gui.c while it runs)
ROOT = os.environ.get('GPSP_LAYOUT_ROOT') or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
GAME = re.compile(r'(\.ltrans\.o|/arm_stub\.o|/bios_data\.o|/upscale_aspect\.o|/scanline_fill16\.o|'
                  r'/blend_landscape\.o|/blend_portrait\.o|/zehn_entry\.o|/crt0\.o)$')


def pinned_library():
    """(archive(member), section) of the library pieces nspire_lineio.ld pins."""
    out = set()
    for l in open(os.path.join(ROOT, 'nspire_lineio.ld')):
        m = re.match(r'\s*\*(\S+\.a):(\S+)\((\S+)\)\s*$', l)
        if m:
            out.add(('%s(%s)' % (m.group(1), m.group(2)), m.group(3)))
    return out
DUMMY = '''
/* tests/layout_pinned.py: 4 KB of cold code and data (removed after) */
volatile unsigned char layout_pinned_pad[4096] = { 1 };
void layout_pinned_fn(void);
void layout_pinned_fn(void)
{
  int i;
  for (i = 0; i < 4096; i++)
    layout_pinned_pad[i] = (unsigned char)(layout_pinned_pad[(i * 7) & 4095] + i);
  printf("layout_pinned %d: a string only this build has\\n", layout_pinned_pad[3]);
}
/* a constructor (.init_array, kept): --gc-sections keeps the rest too */
static void __attribute__((constructor)) layout_pinned_keep(void)
{
  if (layout_pinned_pad[0] == 0x55)
    layout_pinned_fn();
}
'''


def build():
    r = subprocess.run(['make', '-s', '-j%d' % (os.cpu_count() or 4), 'gpsp_libretro.elf'], cwd=ROOT, capture_output=True, text=True)
    if r.returncode:
        sys.exit('build failed:\n' + r.stdout[-2000:] + r.stderr[-2000:])


def ranges():
    out, cur, lib = [], None, pinned_library()
    for l in open(os.path.join(ROOT, 'gpsp_libretro.map')):
        m = re.match(r'^ (\.\S+)\s*$', l)
        if m:
            cur = m.group(1)
            continue
        m = re.match(r'^ (\.\S+)?\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+)$', l)
        if m and (m.group(1) or cur) and (GAME.search(m.group(4)) or
                                          (os.path.basename(m.group(4)), m.group(1) or cur) in lib):
            a = int(m.group(2), 16)
            out.append((a, a + int(m.group(3), 16)))
        cur = None
    return out


def game():
    out = subprocess.run(['arm-none-eabi-nm', '-S', os.path.join(ROOT, 'gpsp_libretro.elf')],
                         capture_output=True, text=True, check=True).stdout
    r, g = ranges(), {}
    for l in out.splitlines():
        m = re.match(r'([0-9a-f]+) ([0-9a-f]+) (\S) (.*)$', l)
        if m:
            a = int(m.group(1), 16)
            if any(lo <= a < hi for lo, hi in r):
                g[m.group(4)] = (a, int(m.group(2), 16))
    return g


def main():
    build()
    a = game()
    failed = 0
    for name in sys.argv[1:] or ['gui.c']:
        failed += one(a, name)
    build()   # the tree's own program again
    return 1 if failed else 0


def one(a, name):
    gui = os.path.join(ROOT, name)
    saved = open(gui).read()
    try:
        with open(gui, 'a') as f:
            f.write('#include <stdio.h>\n' + DUMMY)
        build()
        b = game()
    finally:
        with open(gui, 'w') as f:
            f.write(saved)
        os.utime(gui)
    if 'layout_pinned_fn' in b or 'layout_pinned_pad' in b:
        sys.exit('FAIL: the cold code counted as the game\'s')
    linked = subprocess.run(['arm-none-eabi-nm', '-S', os.path.join(ROOT, 'gpsp_libretro.elf')],
                            capture_output=True, text=True).stdout
    if not re.search(r' 00001000 [dD] layout_pinned_pad$', linked, re.M) or 'layout_pinned_fn' not in linked:
        print('FAIL: the 4 KB added to %s are not in the program (gc?)' % name)
        return 1
    same = sum(1 for n in a if b.get(n) == a[n])
    way = [n for n in a if n in b and b[n] != a[n] and b[n][1] == a[n][1] and (b[n][0] - a[n][0]) % 4096 == 0]
    moved = [n for n in a if n in b and b[n] != a[n] and n not in way]
    gone = [n for n in a if n not in b]
    for n in moved[:10]:
        print('  moved: %s 0x%x/%d -> 0x%x/%d' % (n, a[n][0], a[n][1], b[n][0], b[n][1]))
    for n in gone[:10]:
        print('  gone: %s' % n)
    ok = not moved and not gone and len(a) > 300
    print('%s: %d game symbols, %d at the same address, %d moved by whole 4 KB ways, %d moved otherwise, '
          '%d gone, with 4 KB of code and data added to %s' % ('PASS' if ok else 'FAIL', len(a), same, len(way),
                                                              len(moved), len(gone), name))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
