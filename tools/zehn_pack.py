#!/usr/bin/env python3
"""Packs genzehn's uncompressed program file smaller than genzehn --compress
(the Makefile's ZEHN_PACK; nspire/zehn_entry.S undoes it at the start):

    zehn_pack.py PLAIN.tns PROGRAM.elf OUT.tns [--no-relocate] [--zlib]

* The image ends where its last non-zero byte is: Ndless's Zehn loader
  fills the rest of the memory it gives the program with zeros after
  unpacking (zehn_loader.cpp, since compression came in), so .bss's
  4.5 MB of zeros needn't be in the stream.
* The stream is zopfli's (pigz -11), a zlib stream like genzehn's, only
  smaller; zlib's level 9 when pigz isn't there (--zlib: always).
* The ADD_BASE relocations (4 bytes each in the file, outside the
  compressed stream) go into the compressed image as a table of steps at
  the start of .bss, and the entry becomes nspire/zehn_entry.S's stub, which
  fixes those words and zeroes the table before crt0 runs.
  The loader's other relocations (the GOT's, SET_ZERO) stay with the
  loader. Without the stub in the ELF (or --no-relocate) the relocations
  stay in the file.
* The code after the stub has its B and BL instructions stored with the
  target's word number instead of the offset (xz's ARM branch filter; with
  a condition, only those 64 words away or more), and its PC-relative LDRs
  (any condition) with the literal's address mod 8 KB instead of U:imm12;
  the stub puts the offsets back first.

Prints one line: the sizes (and a WARNING line when it couldn't move the
relocations: the file is then correct but bigger). Every packed file is
loaded here as Ndless loads it (then the stub's work, in Python) and
compared with the plain file loaded the same way; any difference fails
the pack.
"""

import os
import shutil
import struct
import subprocess
import sys
import zlib

SIGNATURE = 0x6e68655a
ADD_BASE, ADD_BASE_GOT, SET_ZERO, FILE_COMPRESSED, UNALIGNED_RELOC = range(5)
MAGIC = 0x4c525a47          # "GZRL" in zehn_entry.S, before its four parameter words
PARAMS = 16                 # zehn_self, zehn_table, zehn_code_start, zehn_code_end


def is_branch(top):
    return top in (0xea, 0xeb)  # B, BL (always)


def is_literal_load(w):
    return w & 0x0f7f0000 == 0x051f0000  # ldr rX, [pc, #+-imm12] (any condition)


def is_cond_branch(w):
    return w & 0x0e000000 == 0x0a000000 and w >> 28 < 0xe  # B, BL (a condition)


NEAR = 64           # zehn_entry.S: a conditional branch nearer than 64 words stays as it is
WORDS = 1 << 24


def signed24(x):
    return x - WORDS if x & 0x800000 else x


def far_branch(y, base, sign):
    """A conditional branch's 24 bits y at word number base - 2: one nearer
    than NEAR words stays (y in -NEAR..NEAR); a farther one stores its
    target's word number, but a target in the program's first NEAR words
    (which would read as a near one) goes where no far one's can (base +
    target, its own place give or take NEAR). One to one for every y when
    2 * NEAR <= base <= 2^24 - 2 * NEAR (the caller checks)."""
    if -NEAR < signed24(y) < NEAR:
        return y
    if sign > 0:
        a = (base + y) % WORDS
        return (a + base) % WORDS if -NEAR < signed24(a) < NEAR else a
    d = signed24((y - base) % WORDS)
    a = d % WORDS if -NEAR < d < NEAR else y
    return (a - base) % WORDS


def branches(img, start, end, sign):
    """The ARM code filters: sign +1 stores each B/BL's target word number
    (a conditional one's when it is NEAR words away or more: far_branch),
    and each PC-relative LDR's U:imm12 as its literal's address mod 8 KB
    ("#-0", the one offset no address gives, as 4096); -1 puts the offsets
    back (what zehn_entry.S does). All are one to one on every word of their
    pattern (data words too)."""
    for i in range(start, end, 4):
        if is_branch(img[i + 3]):
            v = img[i] | img[i + 1] << 8 | img[i + 2] << 16
            v = (v + sign * ((i + 8) >> 2)) & 0xffffff
            img[i], img[i + 1], img[i + 2] = v & 0xff, v >> 8 & 0xff, v >> 16
            continue
        w, = struct.unpack_from('<I', img, i)
        if is_cond_branch(w):
            base = (i + 8) >> 2
            if not 2 * NEAR <= base <= WORDS - 2 * NEAR:
                raise ValueError('a conditional branch at 0x%x: too near the start for its filter' % i)
            struct.pack_into('<I', img, i, w & 0xff000000 | far_branch(w & 0xffffff, base, sign))
            continue
        if not is_literal_load(w):
            continue
        u, imm = w >> 23 & 1, w & 0xfff
        if sign > 0:    # the offset (-0 as 4096) plus its place: the address mod 8 KB
            off = imm if u else 4096 if imm == 0 else 8192 - imm
            t = (off + i + 8) & 0x1fff
        else:           # back: the 13 bits less its place, as a signed offset
            off = (u << 12 | imm) - (i + 8) & 0x1fff
            t = off if off < 4096 else 0 if off == 4096 else 8192 - off
            t |= (off < 4096) << 12
        struct.pack_into('<I', img, i, w & ~0x00800fff | (t >> 12) << 23 | t & 0xfff)


class Zehn:
    def __init__(self, data):
        (self.sig, self.ver, self.file_size, rc, fc, es, self.alloc_size,
         self.entry) = struct.unpack_from('<8I', data)
        if self.sig != SIGNATURE or self.ver != 1:
            raise ValueError('not a Zehn file')
        p = 32
        self.relocs = [(w & 0xff, w >> 8) for w in struct.unpack_from('<%dI' % rc, data, p)]
        p += 4 * rc
        self.flags = data[p:p + 4 * fc]
        p += 4 * fc
        self.extra = data[p:p + es]
        p += es
        self.head = p
        self.body = data[p:self.file_size]
        self.mem = self.alloc_size - p      # the image's memory: its bytes, then zeros

    def compressed(self):
        return bool(self.relocs) and self.relocs[0][0] == FILE_COMPRESSED

    def image(self):
        if self.compressed():
            if self.relocs[0][1] != 0:
                raise ValueError('compression type %d' % self.relocs[0][1])
            raw = zlib.decompress(self.body)
        else:
            raw = self.body
        if len(raw) > self.mem:
            raise ValueError('image larger than its memory')
        return bytearray(raw) + bytearray(self.mem - len(raw))


def load(z, base):
    """The image as zehn_load leaves it at base (relocations applied)."""
    img = z.image()
    for t, off in z.relocs:
        if t in (FILE_COMPRESSED, UNALIGNED_RELOC):
            continue
        if off >= len(img):
            raise ValueError('relocation outside the image')
        if t == ADD_BASE:
            v, = struct.unpack_from('<I', img, off)
            struct.pack_into('<I', img, off, (v + base) & 0xffffffff)
        elif t == ADD_BASE_GOT:
            while True:
                v, = struct.unpack_from('<I', img, off)
                if v == 0xffffffff:
                    break
                struct.pack_into('<I', img, off, (v + base) & 0xffffffff)
                off += 4
        elif t == SET_ZERO:
            struct.pack_into('<I', img, off, 0)
        else:
            raise ValueError('relocation type %d' % t)
    return img


def run_stub(img, base, z):
    """What gpsp_zehn_entry does with the table (zehn_entry.S), in Python."""
    if z.entry == 0:
        return
    at = img.find(struct.pack('<I', MAGIC), z.entry, z.entry + 0x200)
    self_off, table, code_start, code_end = struct.unpack_from('<4I', img, at + 4)
    if table == 0:
        return
    if self_off != z.entry:
        raise ValueError('zehn_self %x is not the entry %x' % (self_off, z.entry))
    branches(img, code_start, code_end, -1)
    p, place = table, 0
    while True:
        step, shift = 0, 0
        while True:
            b = img[p]
            p += 1
            step |= (b & 0x7f) << shift
            shift += 7
            if not b & 0x80:
                break
        if step == 0:
            break
        place += step * 4
        v, = struct.unpack_from('<I', img, place)
        struct.pack_into('<I', img, place, (v + base) & 0xffffffff)
    end = table + (p - table + 3) // 4 * 4
    img[table:end] = bytes(end - table)


def elf_info(path):
    """Symbols (name: (value, size)) and sections (name: (addr, size, type))."""
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF' or d[4] != 1 or d[5] != 1:
        raise ValueError('%s: not a 32-bit little-endian ELF' % path)
    shoff, = struct.unpack_from('<I', d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2e)
    shdrs = [struct.unpack_from('<10I', d, shoff + i * shentsize) for i in range(shnum)]
    def strat(sec, off):
        o = shdrs[sec][4] + off
        return d[o:d.index(b'\0', o)].decode()
    sections, syms = {}, {}
    for h in shdrs:
        sections[strat(shstrndx, h[0])] = (h[3], h[5], h[1])
    for h in shdrs:
        if h[1] == 2:   # SHT_SYMTAB
            for i in range(h[5] // 16):
                name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', d, h[4] + 16 * i)
                if name:
                    syms.setdefault(strat(h[6], name), (value, size))
    return syms, sections


def leb(n):
    out = bytearray()
    while True:
        b = n & 0x7f
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return out


def relocate_in_image(z, img, elf):
    """Moves ADD_BASE into a table in img; returns (kept relocs, entry) or None."""
    syms, sections = elf_info(elf)
    if 'gpsp_zehn_entry' not in syms or '.bss' not in sections:
        return None
    entry = syms['gpsp_zehn_entry'][0]
    if z.entry != syms.get('_start', (0,))[0] or syms.get('__base', (0,))[0] != 0:
        raise ValueError('the program does not start at 0 with crt0')
    adds = sorted(off for t, off in z.relocs if t == ADD_BASE)
    kept = [(t, off) for t, off in z.relocs if t != ADD_BASE]
    if any(t == UNALIGNED_RELOC for t, _ in kept) or any(off & 3 for off in adds):
        raise ValueError('an unaligned relocation: the stub fixes words only')
    if len(set(adds)) != len(adds):
        raise ValueError('a place relocated twice')
    # The loader's own relocations keep their places: none may be one of ours.
    theirs = set()
    for t, off in kept:
        if t == ADD_BASE_GOT:
            o = off
            while struct.unpack_from('<I', img, o)[0] != 0xffffffff:
                theirs.add(o)
                o += 4
        elif t == SET_ZERO:
            theirs.add(off)
    if theirs & set(adds):
        raise ValueError('an ADD_BASE place the loader also fixes')
    at = img.find(struct.pack('<I', MAGIC), entry, entry + 0x200)
    if at < 0 or any(img[at + 4:at + 4 + PARAMS]):
        raise ValueError('no zehn_entry.S parameters after gpsp_zehn_entry')
    # The branch filter: the code after the stub (it runs before it's undone).
    code_start, code_end = at + 4 + PARAMS, sections['.text'][0] + sections['.text'][1]
    code_start = (code_start + 3) & ~3
    if any(code_start <= off < code_end and (is_branch(img[off + 3]) or
                                             is_cond_branch(struct.unpack_from('<I', img, off)[0]) or
                                             is_literal_load(struct.unpack_from('<I', img, off)[0]))
           for off in adds):
        raise ValueError('a relocated word looks like a branch or a literal load')
    table = bytearray()
    last = 0
    for off in adds:
        table += leb((off - last) // 4)
        last = off
    table += b'\0'
    table += bytes(-len(table) % 4)
    start = sections['.bss'][0]
    if start & 3 or start + len(table) > len(img) or any(img[start:start + len(table)]):
        raise ValueError('no zeros at .bss (0x%x) for the %d-byte table' % (start, len(table)))
    if adds and adds[-1] + 4 > start:
        raise ValueError('a relocation at or after .bss')
    if at + 4 + PARAMS > start:
        raise ValueError('the stub after .bss')
    img[start:start + len(table)] = table
    branches(img, code_start, code_end & ~3, +1)
    struct.pack_into('<4I', img, at + 4, entry, start, code_start, code_end & ~3)
    return kept, entry


def deflate(data, use_zlib):
    pigz = None if use_zlib else shutil.which('pigz')
    if pigz:
        r = subprocess.run([pigz, '-11', '-z', '-c'], input=bytes(data), capture_output=True)
        if r.returncode == 0 and zlib.decompress(r.stdout) == data:
            return r.stdout, 'zopfli'
    return zlib.compress(bytes(data), 9), 'zlib 9'


def main(argv):
    args = [a for a in argv if not a.startswith('--')]
    if len(args) != 3:
        sys.exit(__doc__.split('\n\n')[1])
    plain_path, elf, out = args
    plain = Zehn(open(plain_path, 'rb').read())
    if plain.compressed():
        sys.exit('%s: already compressed (genzehn without --compress)' % plain_path)
    img = plain.image()
    relocs, entry = plain.relocs, plain.entry
    moved = None
    if '--no-relocate' not in argv:
        try:
            moved = relocate_in_image(plain, img, elf)
        except ValueError as e:     # still a good file, only bigger: say so
            print('zehn_pack: WARNING: relocations and branches left as genzehn has them (%s): about 57 KB more' % e)
            img = plain.image()
    if moved:
        relocs, entry = moved
    stream = bytes(img).rstrip(b'\0')
    packed, how = deflate(stream, '--zlib' in argv)
    relocs = [(FILE_COMPRESSED, 0)] + relocs
    head = 32 + 4 * len(relocs) + len(plain.flags) + len(plain.extra)
    data = struct.pack('<8I', SIGNATURE, 1, head + len(packed), len(relocs), len(plain.flags) // 4,
                       len(plain.extra), head + plain.mem, entry)
    data += b''.join(struct.pack('<I', t | off << 8) for t, off in relocs)
    data += plain.flags + plain.extra + packed

    # Loaded as Ndless loads it (then the stub), it must be the plain file loaded.
    base = 0x11234000
    new = Zehn(data)
    got = load(new, base)
    run_stub(got, base, new)
    want = load(plain, base)
    if moved:   # the stub's parameter words are the packer's
        at = img.find(struct.pack('<I', MAGIC), entry, entry + 0x200) + 4
        want[at:at + PARAMS] = img[at:at + PARAMS]
    if got != want:
        sys.exit('zehn_pack: the packed program does not load as the plain one')
    with open(out + '.tmp', 'wb') as f:
        f.write(data)
    os.replace(out + '.tmp', out)
    print('zehn_pack: %s %d bytes (%s; %d relocations in the file, %s; image %d of %d bytes)'
          % (out, len(data), how, len(relocs),
             '%d in the image' % (len(plain.relocs) - len(relocs) + 1) if moved else 'none moved',
             len(stream), plain.mem))


if __name__ == '__main__':
    main(sys.argv[1:])
