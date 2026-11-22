#!/usr/bin/env python3
"""Makes nspire_ui_font.h: the menus' type as pre-drawn glyphs.

    python3 -I tools/ui_font.py FONTDIR > nspire_ui_font.h

One family: IBM Plex Mono (IBM, SIL Open Font License 1.1; licence in
docs/fonts/), Regular and Medium: IBMPlexMono-Regular.ttf and
-Medium.ttf (a build of IBM Plex Mono whose outlines differ from
Google Fonts' copy), saved in FONTDIR under those
names. One size, 11 px, everywhere. Needs Pillow and freetype-py (pip
install freetype-py; FREETYPE_PY may point at a folder it was installed
into).

How a glyph is drawn: FreeType's light hinting (vertical only: baseline,
x-height and cap height land on whole pixels, horizontal stems stay where
the outline puts them, so letter spacing is the font's own), at one or two
positions between pixels, kept as 16 levels of coverage (4 bits a pixel).
Pen positions use the unhinted advances and the font's kerning (GPOS, via
Pillow/raqm), in 1/64 pixel. Digits get one width (tabular): each digit is
centred in the widest digit's cell, so counters and times don't jitter.

Characters: ASCII 32-126, plus a few drawn from control codes (see
nspire_ui.h): ellipsis, middle dot, single angle quotes, arrows. The big
words' cuts have no small letters (they are only ever set in capitals):
codes 32-96.
"""
import os
import sys

from PIL import ImageFont

try:
    import freetype as ft
except ImportError:   # freetype-py installed with pip --target
    sys.path.insert(0, os.environ.get("FREETYPE_PY", ""))
    import freetype as ft

GAMMA = 0.80    # coverage ** GAMMA: light text on a dark ground keeps its stems

FILES = {"regular": "IBMPlexMono-Regular.ttf", "medium": "IBMPlexMono-Medium.ttf"}
CAP = 0.698     # the family's capital height over its em

# slot, file, size (px: "em" the em, "cap" the capitals' height), weight,
# width (the wdth axis, 100 is normal), tracking (px), characters, positions
# between pixels.
SLOTS = [
    # Display / header titles ("caps" only, Medium weight). d62 is slightly
    # tighter so ui_fit_cuts can step down gracefully for long game titles.
    ("d62", "medium", ("em", 13.5), 0, 0, -0.75, "caps", 2),
    ("d75", "medium", ("em", 15), 0, 0, -0.70, "caps", 2),
    ("d100", "medium", ("em", 15), 0, 0, -0.65, "caps", 2),
    ("d125", "medium", ("em", 16), 0, 0, -0.60, "caps", 2),
    # everything read or chosen
    ("body", "regular", ("em", 13), 0, 0, -0.50, "all", 2),
    ("body_strong", "medium", ("em", 13), 0, 0, -0.70, "all", 2),
    # labels, key names, speed over the game
    ("small", "regular", ("em", 12), 0, 0, -0.60, "all", 2),
    ("small_strong", "medium", ("em", 12), 0, 0, -0.75, "all", 2),
    # in-game OSD banners & toasts (must fit 50-char notices inside 286 px)
    ("osd", "medium", ("em", 11), 0, 0, -0.65, "all", 2),
]

EXTRA = {1: "…", 2: "·", 3: "‹", 4: "›", 5: "←", 6: "↑", 7: "→", 8: "↓"}
DIGITS = [ord(c) for c in "0123456789"]


def codes_of(charset):
    return list(EXTRA) + list(range(32, 97 if charset == "caps" else 127))


def char(code):
    return EXTRA.get(code, chr(code))


class Face:
    """One cut of the family: FreeType for the shapes, Pillow (raqm) for
    advances and kerning."""

    def __init__(self, path, size, wght, wdth, track, phases):
        self.size, self.track, self.phases = size, track, phases
        self.ft = ft.Face(path)
        self.ft.set_char_size(int(round(size * 64)))
        self.pil = ImageFont.truetype(path, size)
        if wght:   # a variable font: its weight and width
            coords = []
            for a in self.ft.get_variation_info().axes:
                coords.append(wght if a.tag == "wght" else wdth if a.tag == "wdth" else a.default)
            self.ft.set_var_design_coords(coords)
            self.pil.set_variation_by_axes(coords)
        m = self.ft.size
        self.ascent = (m.ascender + 63) >> 6
        self.descent = (-m.descender + 63) >> 6
        self.tab = max(self.pil.getlength(chr(d)) for d in DIGITS)

    def advance(self, code):
        if code in DIGITS:
            return self.tab + self.track
        return self.pil.getlength(char(code)) + self.track

    def kern(self, a, b):
        if a in DIGITS or b in DIGITS:
            return 0.0
        ca, cb = char(a), char(b)
        return self.pil.getlength(ca + cb) - self.pil.getlength(ca) - self.pil.getlength(cb)

    def glyph(self, code, phase):
        """-> x, y (top-left from the pen, y from the baseline), w, h,
        4-bit rows (two pixels a byte, rows padded to bytes)."""
        dx = phase * 64 // self.phases
        if code in DIGITS:   # centred in the tabular cell
            dx += int(round((self.tab - self.pil.getlength(chr(code))) / 2 * 64))
        self.ft.set_transform(ft.Matrix(65536, 0, 0, 65536), ft.Vector(dx, 0))
        self.ft.load_char(char(code), ft.FT_LOAD_RENDER | ft.FT_LOAD_TARGET_LIGHT)
        g = self.ft.glyph
        b = g.bitmap
        w, h = b.width, b.rows
        if not w or not h:
            return 0, 0, 0, 0, b""
        buf = b.buffer
        px = [[min(15, round((buf[j * b.pitch + i] / 255) ** GAMMA * 15)) for i in range(w)] for j in range(h)]
        # crop empty edges (rows taken off the top move the top down)
        while px and not any(px[0]):
            px.pop(0)
        top = g.bitmap_top - (h - len(px))
        while px and not any(px[-1]):
            px.pop()
        if not px:
            return 0, 0, 0, 0, b""
        left = g.bitmap_left
        while all(r[0] == 0 for r in px):
            for r in px:
                r.pop(0)
            left += 1
        while all(r[-1] == 0 for r in px):
            for r in px:
                r.pop()
        w, h = len(px[0]), len(px)
        rows = bytearray()
        for r in px:
            line = list(r) + ([0] if w & 1 else [])
            rows += bytes(line[i] | (line[i + 1] << 4) for i in range(0, len(line), 2))
        return left, -top, w, h, bytes(rows)


def font_data(face, codes, kern_min=0.5):
    data = bytearray()
    glyphs, advances = [], []
    for code in range(0, 127):
        if code not in codes:
            glyphs += [(0, 0, 0, 0, 0)] * face.phases
            advances.append(0)
            continue
        advances.append(round(face.advance(code) * 64))
        for ph in range(face.phases):
            x, y, w, h, bits = face.glyph(code, ph)
            glyphs.append((len(data), x, y, w, h))
            data += bits
    kern = []
    for a in codes:
        for b in codes:
            if a < 32 or b < 32:
                continue
            k = face.kern(a, b)
            if abs(k) >= kern_min:
                kern.append(((a << 8) | b, round(k * 64)))
    kern.sort()
    return data, glyphs, advances, kern


def main(fontdir):
    out = ["/* Made by tools/ui_font.py from IBM Plex Mono (SIL Open Font License 1.1; source and",
           " * licence in docs/fonts/). Don't edit. */", ""]
    total = 0
    for name, which, (unit, px), wght, wdth, track, charset, phases in SLOTS:
        size = px / CAP if unit == "cap" else px
        face = Face(os.path.join(fontdir, FILES[which]), size, wght, wdth, track, phases)
        data, glyphs, advances, kern = font_data(face, codes_of(charset))
        assert len(data) < 65536, name
        total += len(data) + len(glyphs) * 6 + len(kern) * 4 + 127 * 2
        out.append("static const u8 ui_font_%s_bits[%d] = {" % (name, len(data)))
        for i in range(0, len(data), 24):
            out.append("  " + ",".join("%d" % v for v in data[i:i + 24]) + ",")
        out.append("};")
        out.append("static const struct ui_glyph ui_font_%s_glyphs[%d] = {" % (name, len(glyphs)))
        for i in range(0, len(glyphs), 4):
            out.append("  " + ", ".join("{%d,%d,%d,%d,%d}" % g for g in glyphs[i:i + 4]) + ",")
        out.append("};")
        out.append("static const u16 ui_font_%s_advance[127] = {" % name)
        for i in range(0, 127, 16):
            out.append("  " + ",".join(str(a) for a in advances[i:i + 16]) + ",")
        out.append("};")
        out.append("static const struct ui_kern ui_font_%s_kern[%d] = {" % (name, max(1, len(kern))))
        for i in range(0, len(kern), 8):
            out.append("  " + ", ".join("{0x%04X,%d}" % k for k in kern[i:i + 8]) + ",")
        if not kern:
            out.append("  {0, 0}")
        out.append("};")
        out.append("const struct ui_font ui_font_%s = { ui_font_%s_glyphs, ui_font_%s_bits, "
                   "ui_font_%s_advance, ui_font_%s_kern, %d, %d, %d, %d };"
                   % (name, name, name, name, name, len(kern), face.ascent, face.descent, phases))
        out.append("")
    out.append("/* %d bytes */" % total)
    print("\n".join(out))


if __name__ == "__main__":
    main(sys.argv[1])
