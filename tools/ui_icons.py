#!/usr/bin/env python3
"""The menus' icons: drawn here as strokes on a 16-unit grid, one stroke
weight (1.5 px at 16 px, 1.25 px at 12 px), round caps and joins, rendered
8x8 supersampled to 16 levels of coverage. Outer edges sit on whole pixels
(a stroke's centre is 0.75 px inside the shape's edge), so the outline is
crisp at 1x and only the inner side of each stroke is soft.

    python3 -I tools/ui_icons.py > nspire_ui_icons.h
    python3 -I tools/ui_icons.py --sheet OUT.png LABEL_FONT.ttf
"""
import sys

from PIL import Image, ImageDraw, ImageFont

SS = 8

# Each icon: a list of ops on the 16-unit grid (x right, y down; 0-16 are
# pixel edges). ("l", pts) polyline, ("c", pts) closed polyline,
# ("r", x0, y0, x1, y1, radius) rectangle outline by its outer edges,
# ("o", cx, cy, r) circle by its outer radius, ("a", cx, cy, r, start, end)
# arc, ("f", pts) filled polygon, ("d", cx, cy, r) filled dot,
# ("fr", x0, y0, x1, y1) filled rectangle.
ICONS = [
    ("play", [("c", [(5, 3.5), (12.5, 8), (5, 12.5)])]),
    ("save", [("l", [(2.75, 9.5), (2.75, 13.25), (13.25, 13.25), (13.25, 9.5)]),
              ("l", [(8, 2.75), (8, 9.5)]), ("l", [(5, 6.75), (8, 9.75), (11, 6.75)])]),
    ("load", [("l", [(2.75, 9.5), (2.75, 13.25), (13.25, 13.25), (13.25, 9.5)]),
              ("l", [(8, 3), (8, 10)]), ("l", [(5, 6), (8, 3), (11, 6)])]),
    ("link", [("l", [(2.75, 5.5), (13, 5.5)]), ("l", [(10.25, 2.75), (13.25, 5.5), (10.25, 8.25)]),
              ("l", [(13.25, 10.5), (3, 10.5)]), ("l", [(5.75, 7.75), (2.75, 10.5), (5.75, 13.25)])]),
    ("controller", [("r", 1, 4, 15, 12.5, 3.5),
                    ("l", [(5, 6.5), (5, 10)]), ("l", [(3.25, 8.25), (6.75, 8.25)]),
                    ("d", 10.25, 9.25, 1.0), ("d", 12.25, 7.25, 1.0)]),
    ("cable", [("r", 4, 5, 12, 11, 1.5), ("l", [(6.25, 2), (6.25, 5)]), ("l", [(9.75, 2), (9.75, 5)]),
               ("l", [(8, 11), (8, 14.5)])]),
    ("battery", [("r", 1, 4, 14, 12, 1.5), ("fr", 14, 6, 15.5, 10)]),
    ("settings", [("l", [(2.75, 4.75), (7.75, 4.75)]), ("l", [(13.25, 4.75), (13.25, 4.75)]),
                  ("l", [(2.75, 11.25), (2.75, 11.25)]), ("l", [(8.25, 11.25), (13.25, 11.25)]),
                  ("o", 10.5, 4.75, 2.5), ("o", 5.5, 11.25, 2.5)]),
    ("controls", [("c", [(6.25, 1.75), (9.75, 1.75), (9.75, 6.25), (14.25, 6.25), (14.25, 9.75), (9.75, 9.75),
                         (9.75, 14.25), (6.25, 14.25), (6.25, 9.75), (1.75, 9.75), (1.75, 6.25), (6.25, 6.25)])]),
    ("folder", [("c", [(1.75, 3.75), (6.25, 3.75), (7.75, 5.5), (14.25, 5.5), (14.25, 12.75), (1.75, 12.75)])]),
    ("game", [("c", [(3.25, 1.75), (10.25, 1.75), (12.75, 4.25), (12.75, 14.25), (3.25, 14.25)]),
              ("l", [(6, 5.25), (9.5, 5.25)])]),
    ("warning", [("c", [(8, 2), (14.5, 13.75), (1.5, 13.75)]), ("l", [(8, 6.5), (8, 9.5)]),
                 ("d", 8, 11.75, 0.95)]),
    ("check", [("l", [(3, 8.5), (6.5, 12), (13, 4.5)])]),
    ("clock", [("o", 8, 8, 6.5), ("l", [(8, 4.5), (8, 8), (10.5, 9.5)])]),
    ("speed", [("a", 8, 10, 6.5, 180, 360), ("l", [(2.25, 10), (13.75, 10)]), ("l", [(8, 10), (11, 6)])]),
    ("back", [("l", [(13.25, 8), (3, 8)]), ("l", [(7, 3.75), (2.75, 8), (7, 12.25)])]),
    ("delete", [("l", [(2.25, 4.25), (13.75, 4.25)]), ("l", [(6, 4), (6, 2.25), (10, 2.25), (10, 4)]),
                ("l", [(3.75, 4.5), (4.5, 14.25), (11.5, 14.25), (12.25, 4.5)])]),
    ("calculator", [("r", 3, 1, 13, 15, 2), ("fr", 5, 3, 11, 6),
                    ("fr", 5, 8, 7, 10), ("fr", 9, 8, 11, 10), ("fr", 5, 11, 7, 13), ("fr", 9, 11, 11, 13)]),
    ("power", [("a", 8, 8.75, 5.75, 300, 600), ("l", [(8, 1.75), (8, 7.5)])]),
    ("swap", [("a", 8, 8, 5.5, 200, 340), ("l", [(11.25, 3.25), (13.25, 5.75), (10.5, 6.75)]),
              ("a", 8, 8, 5.5, 20, 160), ("l", [(4.75, 12.75), (2.75, 10.25), (5.5, 9.25)])]),
    ("next", [("l", [(2.75, 8), (13, 8)]), ("l", [(9, 3.75), (13.25, 8), (9, 12.25)])]),
    ("menu", [("l", [(3, 4.75), (13, 4.75)]), ("l", [(3, 8), (13, 8)]), ("l", [(3, 11.25), (13, 11.25)])]),
    ("sun", [("o", 8, 8, 3.25), ("l", [(8, 1.25), (8, 2.5)]), ("l", [(8, 13.5), (8, 14.75)]),
             ("l", [(1.25, 8), (2.5, 8)]), ("l", [(13.5, 8), (14.75, 8)]),
             ("l", [(3.25, 3.25), (4.1, 4.1)]), ("l", [(11.9, 11.9), (12.75, 12.75)]),
             ("l", [(12.75, 3.25), (11.9, 4.1)]), ("l", [(4.1, 11.9), (3.25, 12.75)])]),
    ("bolt", [("f", [(9.5, 1), (3, 9.25), (7.5, 9.25), (6.5, 15), (13, 6.75), (8.5, 6.75)])]),
    ("info", [("o", 8, 8, 6.5), ("l", [(8, 7.25), (8, 11.25)]), ("d", 8, 4.85, 0.95)]),
]
NAMES = [n for n, _ in ICONS]

# ui18: Settings' pages as the store's tiles: a glyph each, drawn as the icons are (the same 16-unit grid, the
# same ops) but larger and heavier (GLYPH_SET), one stroke for all of them. ui_page_glyph (nspire_ui.c) draws them.
CONTROLLER_OPS = dict(ICONS)["controller"]
GLYPHS = [
    ("sound", [("c", [(2.25, 6), (5.25, 6), (8.75, 2.75), (8.75, 13.25), (5.25, 10), (2.25, 10)]),
               ("a", 9.25, 8, 3.25, -50, 50), ("a", 9.25, 8, 6.25, -50, 50)]),
    ("controller", CONTROLLER_OPS),
    ("bluetooth", [("l", [(4.25, 5), (11.5, 11), (8, 14.25), (8, 1.75), (11.5, 5), (4.25, 11)])]),
    ("keys", [("r", 1, 3, 15, 13, 1.75), ("fr", 3.75, 5.5, 5.75, 7.5), ("fr", 7, 5.5, 9, 7.5),
              ("fr", 10.25, 5.5, 12.25, 7.5), ("fr", 5.25, 9, 10.75, 10.75)]),
    ("picture", [("r", 1, 2.5, 15, 13.5, 1.75), ("l", [(3.5, 11), (6.75, 7.25), (9.25, 9.75), (10.75, 8.25), (12.5, 10)]),
                 ("d", 11, 5.75, 1.25)]),
    ("speed", dict(ICONS)["speed"]),
    ("saving", dict(ICONS)["save"]),
    ("link", dict(ICONS)["cable"]),
    ("cheats", [("l", [(5.25, 4), (1.75, 8), (5.25, 12)]), ("l", [(10.75, 4), (14.25, 8), (10.75, 12)]),
                ("l", [(9.25, 2.75), (6.75, 13.25)])]),
    ("system", [("r", 4, 4, 12, 12, 1.25), ("l", [(6.5, 1.25), (6.5, 4)]), ("l", [(9.5, 1.25), (9.5, 4)]),
                ("l", [(6.5, 12), (6.5, 14.75)]), ("l", [(9.5, 12), (9.5, 14.75)]),
                ("l", [(1.25, 6.5), (4, 6.5)]), ("l", [(1.25, 9.5), (4, 9.5)]),
                ("l", [(12, 6.5), (14.75, 6.5)]), ("l", [(12, 9.5), (14.75, 9.5)])]),
]
GLYPH_NAMES = [n for n, _ in GLYPHS]
GLYPH_SET = (24, 2.25)


def render(ops, size, stroke):
    s = size / 16.0
    W = size * SS
    im = Image.new("L", (W, W), 0)
    d = ImageDraw.Draw(im)
    sw = stroke * SS

    def P(x, y):
        return (x * s * SS, y * s * SS)

    def poly(pts, closed):
        pts = [P(*p) for p in pts]
        if closed:
            pts = pts + [pts[0], pts[1]]
        d.line(pts, fill=255, width=int(round(sw)), joint="curve")
        for p in (pts if not closed else pts[:-1]):
            r = sw / 2
            d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=255)

    for op in ops:
        k = op[0]
        if k in ("l", "c"):
            poly(op[1], k == "c")
        elif k == "r":
            _, x0, y0, x1, y1, r = op
            in0 = stroke / s
            box_o = [*P(x0, y0), *P(x1, y1)]
            box_o = [box_o[0], box_o[1], box_o[2] - 1, box_o[3] - 1]
            d.rounded_rectangle(box_o, radius=r * s * SS, outline=255, width=int(round(sw)))
        elif k == "o":
            _, cx, cy, r = op
            c = P(cx, cy)
            R = r * s * SS
            d.ellipse([c[0] - R, c[1] - R, c[0] + R - 1, c[1] + R - 1], outline=255, width=int(round(sw)))
        elif k == "a":
            _, cx, cy, r, a0, a1 = op
            c = P(cx, cy)
            R = r * s * SS - sw / 2
            d.arc([c[0] - R - sw / 2, c[1] - R - sw / 2, c[0] + R + sw / 2, c[1] + R + sw / 2], a0, a1, fill=255,
                  width=int(round(sw)))
            import math
            for a in (a0, a1):   # round caps
                px, py = c[0] + R * math.cos(math.radians(a)), c[1] + R * math.sin(math.radians(a))
                d.ellipse([px - sw / 2, py - sw / 2, px + sw / 2, py + sw / 2], fill=255)
        elif k == "f":
            d.polygon([P(*p) for p in op[1]], fill=255)
        elif k == "d":
            _, cx, cy, r = op
            c = P(cx, cy)
            R = r * s * SS
            d.ellipse([c[0] - R, c[1] - R, c[0] + R, c[1] + R], fill=255)
        elif k == "fr":
            _, x0, y0, x1, y1 = op
            d.rectangle([*P(x0, y0), P(x1, y1)[0] - 1, P(x1, y1)[1] - 1], fill=255)
    small = im.resize((size, size), Image.BOX)
    return [[min(15, round(small.getpixel((x, y)) / 255 * 15)) for x in range(size)] for y in range(size)]


def pack(px):
    out = bytearray()
    for row in px:
        r = list(row) + ([0] if len(row) & 1 else [])
        out += bytes(r[i] | (r[i + 1] << 4) for i in range(0, len(r), 2))
    return out


SETS = ((16, 1.5), (12, 1.25))


def header():
    lines = ["/* Made by tools/ui_icons.py (drawn there). Don't edit. The ids for", 
             " * everyone; the bits only where UI_ICON_BITS is defined (nspire_ui.c). */", "",
             "#ifndef NSPIRE_UI_ICONS_H", "#define NSPIRE_UI_ICONS_H", ""]
    lines.append("enum ui_icon_id")
    lines.append("{")
    for i in range(0, len(NAMES), 6):
        lines.append("  " + " ".join("UI_ICON_%s," % n.upper() for n in NAMES[i:i + 6]))
    lines.append("  UI_ICON_COUNT")
    lines.append("};")
    lines.append("")
    lines.append("/* ui18: Settings' pages (ui_page_glyph, %d px) */" % GLYPH_SET[0])
    lines.append("enum ui_glyph_id")
    lines.append("{")
    for i in range(0, len(GLYPH_NAMES), 6):
        lines.append("  " + " ".join("UI_GLYPH_%s," % n.upper() for n in GLYPH_NAMES[i:i + 6]))
    lines.append("  UI_GLYPH_COUNT")
    lines.append("};")
    lines.append("#define UI_GLYPH_SIZE %d" % GLYPH_SET[0])
    lines.append("")
    lines.append("#endif")
    lines.append("")
    lines.append("#if defined(UI_ICON_BITS) && !defined(NSPIRE_UI_ICON_BITS_DONE)")
    lines.append("#define NSPIRE_UI_ICON_BITS_DONE")
    lines.append("")
    total = 0
    for size, stroke in SETS:
        data = bytearray()
        for name, ops in ICONS:
            data += pack(render(ops, size, stroke))
        total += len(data)
        lines.append("static const u8 ui_icons_%d[%d] = {" % (size, len(data)))
        for i in range(0, len(data), 24):
            lines.append("  " + ",".join(str(v) for v in data[i:i + 24]) + ",")
        lines.append("};")
        lines.append("")
    size, stroke = GLYPH_SET
    data = bytearray()
    for name, ops in GLYPHS:
        data += pack(render(ops, size, stroke))
    total += len(data)
    lines.append("static const u8 ui_glyphs_%d[%d] = {" % (size, len(data)))
    for i in range(0, len(data), 24):
        lines.append("  " + ",".join(str(v) for v in data[i:i + 24]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("/* %d bytes */" % total)
    lines.append("#endif")
    return "\n".join(lines)


def sheet(out, font):
    S = 6
    cols = 7
    f = ImageFont.truetype(font, 15)
    cw, ch = 190, 16 * S + 40
    rows = (len(ICONS) + cols - 1) // cols
    im = Image.new("RGB", (cols * cw + 20, rows * ch + 40 + 150), (0x13, 0x11, 0x1A))
    d = ImageDraw.Draw(im)
    fg, bg = (0xF2, 0xF0, 0xF7), (0x13, 0x11, 0x1A)
    for i, (name, ops) in enumerate(ICONS):
        x, y = 10 + (i % cols) * cw, 20 + (i // cols) * ch
        for size, stroke in SETS:
            px = render(ops, size, stroke)
            big = Image.new("RGB", (size, size), bg)
            for yy in range(size):
                for xx in range(size):
                    a = px[yy][xx] / 15
                    big.putpixel((xx, yy), tuple(int(bg[k] + (fg[k] - bg[k]) * a) for k in range(3)))
            if size == 16:
                im.paste(big.resize((16 * S, 16 * S), Image.NEAREST), (x, y))
                im.paste(big, (x + 16 * S + 8, y + 70))
            else:
                im.paste(big.resize((12 * 4, 12 * 4), Image.NEAREST), (x + 16 * S + 8, y))
                im.paste(big, (x + 16 * S + 34, y + 72))
        d.text((x, y + 16 * S + 6), name, font=f, fill=(0xB9, 0xB4, 0xC6))
    # the battery's levels, as the header draws them
    y = 20 + rows * ch + 10
    d.text((10, y), "battery levels (16 px at 6x; the code fills the level, red only when low)", font=f, fill=(0xB9, 0xB4, 0xC6))
    base = render(dict(ICONS)["battery"], 16, 1.5)
    for k, lvl in enumerate((4, 3, 2, 1)):
        px = [row[:] for row in base]
        for yy in range(7, 9):
            for xx in range(4, 4 + int(8 * lvl / 4)):
                px[yy][xx] = 15
        col = (0xFF, 0x6B, 0x74) if lvl == 1 else fg
        big = Image.new("RGB", (16, 16), bg)
        for yy in range(16):
            for xx in range(16):
                a = px[yy][xx] / 15
                big.putpixel((xx, yy), tuple(int(bg[q] + (col[q] - bg[q]) * a) for q in range(3)))
        im.paste(big.resize((96, 96), Image.NEAREST), (10 + k * 130, y + 26))
    im.save(out)


def glyph_sheet(out, stroke=None):
    """The page glyphs at 1x and 4x, black on white (ui18's tiles), for a look."""
    size, st = GLYPH_SET
    st = stroke or st
    im = Image.new("RGB", (len(GLYPHS) * (size * 4 + 16) + 16, size * 4 + size + 40), (255, 255, 255))
    for i, (name, ops) in enumerate(GLYPHS):
        px = render(ops, size, st)
        g = Image.new("RGB", (size, size), (255, 255, 255))
        for yy in range(size):
            for xx in range(size):
                v = 255 - int(px[yy][xx] / 15 * 255)
                g.putpixel((xx, yy), (v, v, v))
        x = 16 + i * (size * 4 + 16)
        im.paste(g.resize((size * 4, size * 4), Image.NEAREST), (x, 16))
        im.paste(g, (x, size * 4 + 24))
    im.save(out)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--glyphs":
        glyph_sheet(sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else None)
    elif len(sys.argv) > 1 and sys.argv[1] == "--sheet":
        sheet(sys.argv[2], sys.argv[3])
    else:
        print(header())
