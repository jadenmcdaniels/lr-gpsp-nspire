#!/usr/bin/env python3
"""One PNG per real screen, drawn through the real menu code: gpsp-host (the
calculator's whole frontend on the PC, build/gpsp-host) is driven by scripts
through the screens a player reaches, from the game list to the game, and the
frames it draws are written as PNGs, twice the size (640 x 480), named by what
they show:

    tests/host/ui_wired.py OUTDIR [NAME...]     all scenes, or the ones named

This is what gui.c and the other screens really put on the screen, with the
PC tests' ROMs; the "court" scene draws the main screens with real games
(court-<what it shows>.png: the design's own pictures). Needs
tests/host/run.sh's build first (make in tests/host). Only the standard
library is needed."""

import concurrent.futures
import os
import shutil
import struct
import sys
import time
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import host_tests as H   # noqa: E402
import r19nav as N   # noqa: E402

SCALE = 2
OUT = "."
made = []


def write_png(src, dst, scale=SCALE):
    """A screenshot (.ppm, 320 x 240) as a PNG, each pixel scale x scale."""
    data = open(src, "rb").read()
    header = b"P6\n320 240\n255\n"
    assert data.startswith(header), src
    px = data[len(header):]
    rows = []
    for y in range(240):
        row = px[y * 960:(y + 1) * 960]
        if scale > 1:
            row = b"".join(row[i:i + 3] * scale for i in range(0, len(row), 3))
        rows += [b"\0" + row] * scale

    def chunk(kind, body):
        c = struct.pack(">I", len(body)) + kind + body
        return c + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 320 * scale, 240 * scale, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))
    open(dst, "wb").write(png)


BG, ACCENT = (248, 252, 248), (0, 0, 0)   # the plain look: a white ground (as the screen has it), the picked thing black

# r19 (docs/ui/r19.txt, nspire_ui.h, nspire_ui_screens.c): the grid every black block stands on. UI_MARGIN 8 is
# the one left edge and the right edge's 312; a block reaches 4 either side of it.
#   the top row's block (ui_nav): rows 5-22 (UI_HEAD_Y, UI_LINE_H 18), as wide as its word and 7 more: SETTINGS
#     from x 4, QUIT to x 314, SAVES and GAMES between
#   an item's name (the games', the saves', the folders'): the picture 96 x 64 at x 8 / 112 / 216 (rows from 30 and
#     126), the name under it centred, the picked one's block 18 high from the row 65 under the picture's top (rows
#     95-112 and 191-208), x 4-107 / 108-211 / 212-315
#   Settings' grid: the same blocks, 104 wide, 18 high, 36 apart from row 36
#   a settings page's tile plate (UI_TILE_BLOCK 29 high, UI_TILE_PITCH 45 from row 36): three columns' (as the
#     grid's), two columns' (4-159 / 160-315), or a row of its own (4-315)
#   a list's line (the games' lines view, Bluetooth's devices, the Keys list): 18 high on the body's lines from row
#     36, every 18, x 4-315 (the Keys list beside the keypad: 4-135; the ROM manager's beside its picture: 4-155; the
#     Bluetooth page's list starts under its one row of tiles: from row 36 + 45)
#   the band (rows 222-239): a question's answer block, 18 high, from row 222
#   the Keys screen's keypad (x from 144): a lit key (13 high), the touchpad's arrow
COLUMN_PLATES = {(4, 107), (108, 211), (212, 315)}
PLATES = COLUMN_PLATES | {(4, 159), (160, 315), (4, 315)}
KP_X = 144
LINES = tuple(range(36, 222, 18))
LINES_UNDER_TILES = tuple(range(36 + 45, 222, 18))
NAME_ROWS = (95, 191)           # an item's name block: 30 + 65 and 126 + 65
GRID_ROWS = (36, 72, 108, 144)  # Settings' grid
ITEM_SCREENS = ("game_list", "menu_", "store", "saves", "court-games", "court-paused", "court-store", "court-saves",
                "court-menu", "drive_saves_save", "drive_saves_state")
KEYS_SCREENS = ("controls", "settings_keys", "keys", "court-keys")


def components(px):
    """The solid black parts of a screenshot: [(x0, y0, x1, y1)], each one's bounding box; 4-connected black
    pixels, the white text inside a block being holes in it. (Black text is a part of its own, small.)"""
    black = bytearray(320 * 240)
    for y in range(240):
        for x in range(316):   # (a scroll bar is at x 316-319: not a part of a block it stands beside)
            if px(x, y) == ACCENT:
                black[y * 320 + x] = 1
    out = []
    for start in range(320 * 240):
        if not black[start]:
            continue
        black[start] = 0
        stack, x0, y0, x1, y1 = [start], 320, 240, 0, 0
        while stack:
            p = stack.pop()
            y, x = divmod(p, 320)
            if x < x0: x0 = x
            if x > x1: x1 = x
            if y < y0: y0 = y
            if y > y1: y1 = y
            for q, ok in ((p - 1, x > 0), (p + 1, x < 319), (p - 320, y > 0), (p + 320, y < 239)):
                if ok and black[q]:
                    black[q] = 0
                    stack.append(q)
        out.append((x0, y0, x1, y1))
    return out


def audit(src, name, battery=False):
    """The grid of r19 (the comment above) checked on the screen as drawn: every black block (a black part at
    least 20 wide and 13 high: the text's own black is smaller) stands where the grid puts one: the tab row's on
    rows 5-22 inside x 4-315; in the body a list's line (18 high, on the 18-row lines from row 36, x 4-315, or
    the Keys list's 4-135), Settings' grid (18 high, 36 apart, the three columns' plates), a tile's plate (29
    high on the 45-row pitch, the store's under its pictures: rows 93 / 133 / 189, as wide as one of PLATES), in
    the band a question's answer (rows 222-239); the Keys screen's keypad (x from 144) a key (13 high). No
    block is drawn in the tab row and in the body at once (one black block says where the arrows are), nothing
    is drawn left of x 4 or right of 315 (a scroll bar's thin line apart), nothing but a block between x 4 and
    7, no text past x 312 (the battery's right end); the band's left corner is the ground; with the battery
    shown ('battery N' in the script) its text ends at the band's right end (x 312). Over the game there is
    nothing to check. Returns what is wrong, in words."""
    if name.startswith("game_") and not name.startswith("game_list") or "-play" in name or \
            name.endswith("_play"):
        return []
    data = open(src, "rb").read()[15:]

    def px(x, y):
        i = (y * 320 + x) * 3
        return tuple(data[i:i + 3])

    bad = []
    def rim(c):
        """A block has a solid black outline (its text is inside it); a game picture's black pixels don't."""
        x0, y0, x1, y1 = c
        return all(px(x, y0) == ACCENT and px(x, y1) == ACCENT for x in range(x0, x1 + 1)) and \
            all(px(x0, y) == ACCENT and px(x1, y) == ACCENT for y in range(y0, y1 + 1))
    blocks = [c for c in components(px) if c[2] - c[0] + 1 >= 20 and c[3] - c[1] + 1 >= 13 and rim(c)]
    top, body = [], []
    for x0, y0, x1, y1 in blocks:
        w, h = x1 - x0 + 1, y1 - y0 + 1
        where = "(x %d-%d, rows %d-%d)" % (x0, x1, y0, y1)
        if y1 < 36:
            top.append((x0, y0, x1, y1))
            if (y0, y1) != (5, 22) or x0 < 4 or x1 > 315:
                bad.append("a block in the tab row %s isn't rows 5-22 inside x 4-315" % (where,))
        elif y0 >= 222:
            if (y0, y1) != (222, 239) or x0 < 4 or x1 > 315:
                bad.append("a block in the band %s isn't rows 222-239 inside x 4-315" % (where,))
        elif (w, h) == (96, 64) and x0 in (8, 112, 216) and y0 in (30, 126):
            pass   # a game's picture that is all black (a game's black screen), not a block
        elif x0 >= KP_X and name.startswith(KEYS_SCREENS) and h == 13:
            pass   # the keypad's keys
        else:
            body.append((x0, y0, x1, y1))
            if h == 18:
                keys = name.startswith(KEYS_SCREENS)
                lines = LINES_UNDER_TILES if name.startswith("bluetooth") else LINES
                line = y0 in lines and ((x0, x1) == (4, 315) or (keys and (x0, x1) == (4, 135)) or
                                        (name.startswith("rom_manager") and (x0, x1) == (4, 155)))
                grid = (x0, x1) in COLUMN_PLATES and (y0 in NAME_ROWS or y0 in GRID_ROWS)
                if not (line or grid):
                    bad.append("a line's block %s isn't on the grid (x 4-315 on the lines from row 36; an item's name or "
                               "Settings' page: x 4-107 / 108-211 / 212-315 on rows 95 / 191 / 36 + 36k; the Keys list's "
                               "4-135)" % (where,))
            elif h == 29:
                if not ((x0, x1) in PLATES and (y0 - 36) % 45 == 0):
                    bad.append("a plate %s isn't a settings tile's on the grid (29 high, rows 36 + 45k, x as wide as one "
                               "of PLATES)" % (where,))
            else:
                bad.append("a block %s is neither a line's (18 high) nor a tile's plate (29 high)" % (where,))
    if any(px(315, y) == ACCENT and px(316, y) == ACCENT for y in range(36, 222)):
        bad.append("a scroll bar touches the picked line's block (the block ends at x 315, the bar starts at 316: the "
                   "grids' bars start at 317)")
    if top and body:
        bad.append("a block in the tab row %s and one in the body %s: one black block says where the arrows are" %
                   (top[0], body[0]))
    # the edges: nothing left of x 4 or right of 315; between 4 and 7 only a block; no text past 312
    covered = lambda x, y: any(b[0] <= x <= b[2] and b[1] <= y <= b[3] for b in blocks)
    edge = []
    for y in range(240):
        for x in list(range(0, 8)) + list(range(313, 320)):
            if px(x, y) != BG and not covered(x, y):
                edge.append((x, y))
    thin = [c for c in components_of(edge)]
    for x0, y0, x1, y1 in thin:
        if x0 >= 316 and x1 - x0 + 1 <= 4:
            continue   # a scroll bar
        bad.append("ink on the margin (x %d-%d, rows %d-%d): the left edge is x 8 (a block's x 4) and the right 312" %
                   (x0, x1, y0, y1))
    # the Bluetooth page's list: a name at the left, its kind or state at the right ("CONTROLLER . WEAK", "CONNECTING"):
    # the two are 12 px apart by design, 10 at least here (the widest gap of the line is not a word's space)
    if name.startswith("bluetooth") and name not in ("bluetooth_in_settings", "bluetooth_controller_page", "bluetooth_le_controller_page") and "sound" not in name:
        for k in range(1, 12):
            y0 = 36 + 45 + 18 * (k - 1)
            if y0 + 18 > 222:
                break
            picked = px(5, y0 + 9) == ACCENT
            ink = [x for x in range(8, 313) if any((min(px(x, y)) > 150 if picked else px(x, y) != BG)
                                                   for y in range(y0 + 3, y0 + 15))]
            if len(ink) < 2 or ink[-1] < 280:
                continue
            gaps = [b_ - a_ - 1 for a_, b_ in zip(ink, ink[1:]) if b_ - a_ > 1]
            if (max(gaps) if gaps else 0) < 10:
                bad.append("the Bluetooth page, row %d: the kind at the right touches the name (widest gap %d)" %
                           (y0, max(gaps) if gaps else 0))
    # the Controller list: its sentences end clear of the band (a row of ground between them)
    if name.startswith("settings_controller") and name != "settings_controller_tile" and "details" not in name:
        if [1 for y in range(213, 222) for x in range(8, 313) if px(x, y) != BG]:
            bad.append("the Controller page's text runs down to the band (rows 213-221 are not ground)")
    # the keys' list beside the keypad (x 8-135): a name at the left, its key (or "NO KEY", grey) at the right; when
    # there is one, 6 px of ground at least (the widest gap of the line is not a word's space) between them
    if name.startswith(KEYS_SCREENS) and "view" not in name and "controller" not in name:
        for k in range(10):
            y0 = 36 + 18 * k
            picked = px(5, y0 + 9) == ACCENT
            ink = [x for x in range(8, 133) if any((min(px(x, y)) > 150 if picked else px(x, y) != BG)
                                                   for y in range(y0 + 3, y0 + 15))]
            if len(ink) < 2:
                continue
            gaps = [(b_ - a_ - 1, a_) for a_, b_ in zip(ink, ink[1:]) if b_ - a_ > 1]
            widest = max(gaps)[0] if gaps else 0
            if ink[-1] >= 126 and widest < 6:
                bad.append("the keys' list, row %d: the key at the right touches the name (ink x %d-%d, widest gap %d)" %
                           (y0, ink[0], ink[-1], widest))
    # the Cheats page: ten lines, a name at the left and ON / OFF at the right (at most 30 px wide, 8 px of ground
    # before it: the name never runs into it), a scroll bar at the right edge when there are more codes than lines
    if name.startswith("cheats_page"):
        for k in range(10):
            y0 = 36 + 18 * k
            picked = px(5, y0 + 9) == ACCENT
            ink = [x for x in range(8, 313) if any((min(px(x, y)) > 200 if picked else px(x, y) != BG)
                                                   for y in range(y0 + 3, y0 + 15))]
            if not ink:
                continue
            right = left = ink[-1]
            for x in reversed(ink[:-1]):
                if left - x > 8:
                    break
                left = x
            if right - left + 1 > 30 or right < 300:
                bad.append("a code's line (row %d): the ON / OFF at the right is missing or runs into the name (ink "
                           "x %d-%d)" % (y0, left, right))
        bar = [1 for y in range(30, 217) for x in range(316, 320) if px(x, y) != BG]
        if ("from_a_drive" in name) == bool(bar):
            bad.append("the Cheats page's scroll bar: %s with %s codes" % ("shown" if bar else "missing",
                                                                         "few" if "from_a_drive" in name else "many"))
    # the header row: its first ink is the left edge (x 8, a block's x 4)
    head = [x for y in range(6, 22) for x in range(0, 320) if px(x, y) != BG]
    if head and not 4 <= min(head) <= 10:
        bad.append("the top row starts at x %d: the left edge is 8 (a block's 4)" % min(head))
    # an item screen: each unpicked name (rows 96-111 and 192-207) is centred under its picture (x 8 / 112 / 216,
    # 96 wide); the picked one is a block (it scrolls when too long)
    if name.startswith(ITEM_SCREENS) and "question" not in name and "lines" not in name and not name.endswith("_none"):
        for y0 in NAME_ROWS:
            for c in range(3):
                x0, x1 = 8 + 104 * c, 8 + 104 * c + 95
                if any(b[1] == y0 and b[0] == x0 - 4 for b in blocks):
                    continue
                xs = [x for y in range(y0 + 1, y0 + 16) for x in range(x0 - 4, x1 + 5) if px(x, y) != BG]
                if xs and abs((min(xs) + max(xs)) / 2 - (x0 + 47.5)) > 4:
                    bad.append("a name under the picture at x %d, rows %d-%d, is not centred (ink x %d-%d, the picture's "
                               "centre %d)" % (x0, y0 + 1, y0 + 15, min(xs), max(xs), x0 + 47))
    # Settings' tiles and More's (three across: x 4-107 / 108-211 / 212-315, rows 36 + 45k, 29 high): each tile's two
    # lines (or its name alone) are centred in its column (the tile's centre 8 + 104c + 48, +-5), the words of two
    # neighbours don't touch (2 px of ground between), and More (one screenful) has no scroll bar
    if is_tiles(name):
        for r_ in range(4):
            y0 = 36 + 45 * r_
            cols = []
            for c in range(3):
                x0, x1 = 4 + 104 * c, 107 + 104 * c
                picked = px(x0, y0) == ACCENT and px(x0, y0 + 28) == ACCENT
                xs = [x for y in range(y0, y0 + 29) for x in range(x0, x1 + 1)
                      if (min(px(x, y)) > 200 if picked else px(x, y) != BG)]
                if not xs:
                    cols.append(None)
                    continue
                cols.append((min(xs), max(xs)))
                if abs((min(xs) + max(xs)) / 2.0 - (8 + 104 * c + 47.5)) > 5 and not (picked and name.endswith("changing")):
                    bad.append("a tile's words (column %d, row %d, ink x %d-%d) are not centred in the tile (centre %d)" %
                               (c, r_, min(xs), max(xs), 8 + 104 * c + 47))
            for c in range(2):
                if cols[c] and cols[c + 1] and cols[c + 1][0] - cols[c][1] < 3:
                    bad.append("the words of two tiles touch (row %d, columns %d and %d: x %d and %d)" %
                               (r_, c, c + 1, cols[c][1], cols[c + 1][0]))
        if is_more(name) and [1 for y in range(30, 217) for x in range(316, 320) if px(x, y) != BG]:
            bad.append("More is one screenful and shows a scroll bar at the right edge")
    # (a question dims what is behind it; a value being changed has its note's block down there)
    changing = "chang" in name or name.endswith("_note")
    if not name.startswith("question_") and not changing and px(0, 239) != BG:
        bad.append("the bottom left corner is not the ground: %s" % (px(0, 239),))
    if battery:
        # the band's words end 12 px before the battery: its text ("82%", "100%+") is the last cluster of ink, at most
        # 46 px wide, with 10 px of ground before it
        cols_ = [x for x in range(0, 320) if any(px(x, y) != BG for y in range(222, 240))]
        if cols_:
            right, left = cols_[-1], cols_[-1]
            for x in reversed(cols_[:-1]):
                if left - x > 10:
                    break
                left = x
            if right - left + 1 > 46:
                bad.append("the band's text runs into the battery (its last cluster of ink is x %d-%d)" % (left, right))
        xs = [x for y in range(222, 240) for x in range(240, 320) if px(x, y) != BG]
        if not xs or max(xs) > 312 or max(xs) < 306:
            bad.append("the battery's text doesn't end at the band's right end (x 312): ends at %s" %
                       (max(xs) if xs else None))
    return bad


TILE_SCREENS = ("settings_word", "settings_volume", "settings_brightness", "settings_size", "settings_fps",
                "settings_autosave", "settings_controller_tile", "settings_more", "settings_tiles_after_more",
                "no_game_settings", "no_game_more", "court-settings", "court-brightness", "drive_saves_setting",
                "game_settings")


def is_tiles(name):
    return name == "settings" or name.startswith(TILE_SCREENS)


def is_more(name):
    return (name.startswith("settings_more") and name != "settings_more_tile") or name == "no_game_more" or \
        name.startswith(("drive_saves_more", "drive_saves_setting"))


def components_of(points):
    """Bounding boxes of 8-connected groups of (x, y) points."""
    left = set(points)
    out = []
    while left:
        p = left.pop()
        stack, x0, y0, x1, y1 = [p], p[0], p[1], p[0], p[1]
        while stack:
            x, y = stack.pop()
            x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    q = (x + dx, y + dy)
                    if q in left:
                        left.discard(q)
                        stack.append(q)
        out.append((x0, y0, x1, y1))
    return out


def keep(run, shot, name, battery=False):
    """The run's screenshot 'shot' as OUT/NAME.png (battery: the script had set the battery: audited too)."""
    src = os.path.join(run.out, shot + ".ppm")
    for why in audit(src, name, battery)[:3]:
        H.failures.append("%s: %s" % (name, why))
    write_png(src, os.path.join(OUT, name + ".png"))
    made.append(name)


def scrolled(run, before, after, what):
    """r19: a picked name too long for its place runs to the left after a second (30 px a second): the screen a few
    seconds on is not the one before (the same bytes: it does not scroll)."""
    p = lambda n: open(os.path.join(run.out, n + ".ppm"), "rb").read()
    H.check(p(before) != p(after), "%s: the picked name scrolls (the screen 'after' differs from 'before')" % what)


# The games (names as a player's files have them) and what they have saved.
GAMES = ("Pokemon - Emerald Version (USA, Europe)", "Legend of Zelda, The - The Minish Cap (USA)",
         "Fire Emblem - The Sacred Stones (USA)", "Mario & Luigi - Superstar Saga (USA)",
         "Advance Wars 2 - Black Hole Rising (USA)", "Mega Man Battle Network 3 - White (USA)",
         "Sonic Advance 2 (USA)", "WarioWare, Inc. - Mega Microgame$! (USA)")


def set_age(path, when):
    """A state's file made as old as when (UTC seconds): its file time, and (save17) the save time in its header
    (GPSZ, reserved[1] at 28), which the menus take before the file's."""
    with open(path, "r+b") as f:
        if f.read(4) == b"GPSZ":
            f.seek(28)
            f.write(struct.pack("<I", int(when)))
    os.utime(path, (when, when))


def age_saves(run, ages):
    """Gives each game's newest state a file time of that many seconds ago (the list says 'Yesterday')."""
    now = time.time()
    saves = os.path.join(run.dir, "zgpSP_files")
    for name, seconds in ages.items():
        for f in sorted(os.listdir(saves)):
            if f.startswith(name) and f.endswith(".svs.tns"):
                set_age(os.path.join(saves, f), now - seconds)


def play_and_save(run, games, saves):
    """The same calculator, played: saves = {game: how many states}; each game is started from the list
    (in name order, case blind), saved that many times and left with Q (which closes gpSP)."""
    order = sorted(games, key=str.lower)
    for game, count in saves.items():
        run = H.Run(run.name + "_" + str(order.index(game)), reuse=run)
        run.run("wait 1\n" + H.pick(order.index(game) + 1) + "wait 1.5\n" +
                "press S\nwait 0.6\n" * count + "press Q\nwait 1\nquit\n")
    return run


def scene_list():
    """GAMES: the games as items (a picture, one centred name), the folders after them, the arrows in the grid or on
    the top row; its other looks."""
    # no games at all: where games go
    r = H.Run("w_list_none", roms=())
    r.run("wait 1\nshot a\nquit\n")
    keep(r, "a", "game_list_empty")

    # one game, never played; then played
    r = H.Run("w_list_one", roms=(GAMES[0],)).run("wait 1.2\nshot a\nquit\n")
    keep(r, "a", "game_list_one_new")
    r = H.Run("w_list_one_saved", roms=(GAMES[0],)).run(
        "wait 1\npress enter\nwait 1.5\npress S\nwait 0.5\npress S\nwait 0.5\npress Q\nwait 1\nquit\n")
    r2 = H.Run("w_list_one_saved2", reuse=r).run("wait 1.5\nshot a\nquit\n")
    keep(r2, "a", "game_list_one_saved")

    # several, some played; two folders after the games
    games = GAMES[:5]
    order = sorted(games, key=str.lower)
    first = H.Run("w_list_many", roms=games)
    # (a copy of a calculator remembers the folder it was in, so the folders are made in the first one)
    os.makedirs(os.path.join(first.gba, "Homebrew"))
    os.makedirs(os.path.join(first.gba, "GBC"))
    base = play_and_save(first, games, {order[0]: 3, order[1]: 6, order[3]: 4})
    age_saves(base, {order[0]: 2 * 3600, order[1]: 26 * 3600, order[3]: 9 * 86400})
    r = H.Run("w_list_many2", reuse=base).run(
        "wait 1.5\nshot a\npress right\npress right\nwait 1\nshot b\npress up\nwait 0.4\nshot top_row\n"
        "press right\nwait 0.4\nshot top_row_quit\npress left\npress down\npress down\npress right\npress right\nwait 0.4\nshot folder\n"
        "press down\nwait 0.6\nshot back_item\npress enter\nwait 0.6\nshot up_a_folder\npress enter\nwait 0.6\nshot entered_again\n"
        "press down\npress right\npress right\npress enter\nwait 0.6\nshot folder_empty\n"
        "quit\n")
    keep(r, "a", "game_list")
    keep(r, "b", "game_list_third")
    keep(r, "top_row", "game_list_top_row")
    keep(r, "top_row_quit", "game_list_top_row_quit")
    keep(r, "folder", "game_list_folder_picked")
    keep(r, "back_item", "game_list_back_item")
    keep(r, "up_a_folder", "game_list_up_a_folder")
    keep(r, "entered_again", "game_list_folder_entered_again")
    keep(r, "folder_empty", "game_list_folder_opened")
    H.check([r.notes[k]["ui"] for k in ("a", "b", "top_row", "top_row_quit", "folder", "back_item")] ==
            ["list:0", "list:2", "main:%d" % H.NO_GAME["games"], "main:%d" % H.NO_GAME["exit"], "list:5", "list:7"],
            "game list: the screen reports (the games, then the folders): %s" % [(k, r.notes[k]["ui"]) for k in r.notes])

    # long: scrolled, the number; a name too long scrolls
    many = tuple("Game %02d - Long Name That Goes On For Quite A While Indeed (USA)" % i for i in range(1, 14))
    r = H.Run("w_list_long", roms=many).run(
        "wait 1\n" + "press down\n" * 4 + "wait 0.3\nshot a\nwait 3\nshot b\nwait 2\nshot c\nquit\n")
    keep(r, "a", "game_list_long")
    keep(r, "b", "game_list_long_name_scrolling")
    scrolled(r, "a", "b", "game list")

    # moving a game (tab)
    r = H.Run("w_list_moving", reuse=base).run(
        "wait 1.5\npress tab\nwait 0.3\npress right\nwait 0.4\nshot moving\npress enter\nquit\n")
    keep(r, "moving", "game_list_moving")


def scene_list_names_and_battery():
    """Names with region and revision tags, a release number and ", The" (the list shows the clean name),
    and the band's battery in each state it can be in (r19: the band's right end, not the header's)."""
    games = ("0001 - Advance Wars (Europe) (En,Fr,De)", "Pokemon - Ruby Version (USA, Europe) (Rev 2)",
             "Metroid - Zero Mission (USA, Australia)", "Legend of Zelda, The - A Link to the Past & Four Swords (USA)",
             "Kirby & The Amazing Mirror (USA)")
    r = H.Run("w_list_tags", roms=games).run("""wait 1.2
shot tags
battery 82
press right
press left
wait 0.4
shot battery_82
battery 12
press right
press left
wait 0.4
shot battery_12
battery 64 charging
press right
press left
wait 0.4
shot battery_charging
battery -1
press right
press left
wait 0.4
shot battery_unknown
quit
""")
    keep(r, "tags", "game_list_region_tags")
    for n, name in (("battery_82", "game_list_battery_82"), ("battery_12", "game_list_battery_12_red"),
                    ("battery_charging", "game_list_battery_charging")):
        keep(r, n, name, battery=True)
    keep(r, "battery_unknown", "game_list_battery_not_known")


def scene_menu():
    """The paused menu: it opens on SAVES, the arrows in its tiles (RESUME, SAVE, the states), the top row
    (SETTINGS SAVES GAMES QUIT) and what is under each of its words."""
    # a game with saves, a few minutes to days old
    r = H.Run("w_menu", roms=("Pokemon - Emerald Version (USA, Europe)",))
    r.run("wait 1\npress enter\nwait 1.5\n" + "press S\nwait 0.6\n" * 6 + "press Q\nwait 1\nquit\n")
    saves = os.path.join(r.dir, "zgpSP_files")
    now = time.time()
    for slot, seconds in enumerate((9 * 86400, 3 * 86400, 26 * 3600, 5 * 3600, 600, 20)):
        path = os.path.join(saves, "Pokemon - Emerald Version (USA, Europe).gba%d.svs.tns" % slot)
        set_age(path, now - seconds)
    r = H.Run("w_menu2", reuse=r)
    r.run("""wait 1
press enter
wait 2
press menu
wait 0.5
shot resume
press right
wait 0.4
shot save
press right
wait 0.5
shot state
press down
wait 0.5
shot second_row
press up
press up
wait 0.4
shot row
press right
wait 0.4
shot games
press right
wait 0.4
shot quit_word
press left
press left
press left
wait 0.4
shot settings_word
press right
press right
press down
wait 0.5
shot back_in_the_tiles
quit
""")
    for n, name in (("resume", "menu_resume"), ("save", "menu_save_tile"), ("state", "menu_state_picked"),
                    ("second_row", "menu_state_second_row"), ("row", "menu_top_row"), ("games", "menu_games_word"),
                    ("quit_word", "menu_quit_word"), ("settings_word", "menu_settings_word"),
                    ("back_in_the_tiles", "menu_games_picked")):
        keep(r, n, name)
    ui = lambda *k: [r.notes[x]["ui"] for x in k]
    H.check(ui("resume", "save", "state", "row", "games", "quit_word", "settings_word") ==
            ["saves:0", "saves:1", "saves:2", "main:%d" % H.MAIN["saves"], "main:%d" % H.MAIN["games"],
             "main:%d" % H.MAIN["exit"], "main:%d" % H.MAIN["settings"]],
            "menu: the screen reports: %s" % [(k, r.notes[k]["ui"]) for k in r.notes])


def scene_settings():
    """Settings (r19 fourth pass): the top row's SETTINGS with its tiles drawn and none picked, the arrows in the tiles
    (Volume first), a range being changed (between < >), Screen size at each of its three values, FPS counter cycled,
    More (one screenful, no scroll bar), About, the Controller screen."""
    n_set = N.settings_names()
    last = len(N.more_names()) - 1
    to_settings = "battery 82\nwait 1\npress enter\nwait 2\npress menu\nwait 0.5\n" + N.to_settings_row()
    size = N.settings_index("size")
    r = H.Run("w_settings", roms=("Pokemon - Emerald Version (USA, Europe)",)).run(to_settings + """wait 0.4
shot word
press down
wait 0.4
shot top
press enter
wait 0.3
shot volume_changing
press left
wait 0.3
shot volume_changed
press esc
wait 0.3
shot volume_put_back
press right
press enter
wait 0.3
shot brightness_changing
press esc
press right
wait 0.3
shot size_fit
press enter
wait 0.3
shot size_a
press enter
wait 0.3
shot size_b
press enter
wait 0.3
shot size_c
press right
press enter
wait 0.3
shot fps_cycled
press right
press enter
wait 0.3
shot autosave_changing
press esc
""" + N.tile_moves(N.settings_index("autosave"), N.settings_index("controller")) + """wait 0.3
shot controller_tile
press enter
wait 0.8
shot controller
press esc
wait 0.3
""" + N.tile_moves(N.settings_index("controller"), N.settings_index("more")) + """wait 0.3
shot more_tile
press enter
wait 0.4
shot more_top
""" + N.tile_moves(0, N.more_index("saves")) + """wait 0.3
shot more_saves
press enter
wait 0.8
shot more_saves_page
press esc
wait 0.5
""" + N.tile_moves(N.more_index("saves"), 0) + """press enter
wait 0.3
shot more_value_cycled
""" + N.tile_moves(0, last) + """wait 0.4
shot more_last
press enter
wait 0.5
shot about
press esc
wait 0.4
shot more_after_about
press esc
wait 0.3
shot tiles_after_more
press esc
wait 0.3
battery 100 charging
wait 0.3
press left
press right
wait 0.3
shot row_charging
quit
""")
    for n, name in (("word", "settings_word"), ("top", "settings"), ("volume_changing", "settings_volume_changing"),
                    ("volume_changed", "settings_volume_changed"), ("volume_put_back", "settings_volume_put_back"),
                    ("brightness_changing", "settings_brightness_changing"), ("size_fit", "settings_size_fit"),
                    ("size_a", "settings_size_stretch"), ("size_b", "settings_size_original"), ("size_c", "settings_size_fit_again"),
                    ("fps_cycled", "settings_fps_cycled"), ("autosave_changing", "settings_autosave_changing"),
                    ("controller_tile", "settings_controller_tile"), ("controller", "settings_controller"),
                    ("more_tile", "settings_more_tile"), ("more_top", "settings_more"),
                    ("more_value_cycled", "settings_more_value_cycled"), ("more_saves", "settings_more_saves_tile"), ("more_saves_page", "settings_more_saves_page"), ("more_last", "settings_more_last_tile"),
                    ("about", "settings_about"), ("more_after_about", "settings_more_after_about"),
                    ("tiles_after_more", "settings_tiles_after_more"), ("row_charging", "settings_word_charging")):
        keep(r, n, name, battery=True)
    ui = lambda k: r.notes[k]["ui"]
    H.check(ui("word") == "main:%d" % H.MAIN["settings"] and ui("top") == "Settings:0" and ui("volume_changing") == "Settings:0" and
            ui("brightness_changing") == "Settings:1" and ui("size_fit") == "Settings:%d" % size and
            ui("controller_tile") == "Settings:%d" % N.settings_index("controller") and
            ui("more_top") == "More:0" and ui("more_saves_page").startswith("saves:") and ui("about") == "About:0" and
            ui("tiles_after_more") == "Settings:%d" % N.settings_index("more") and ui("more_last") == "More:%d" % last,
            "settings: the screen reports: %s" % [(k, r.notes[k]["ui"]) for k in r.notes])
    # the three values of Screen size are three different screens, and the fourth press is the first again
    p = lambda n: open(os.path.join(r.out, n + ".ppm"), "rb").read()
    H.check(len({p("size_fit"), p("size_a"), p("size_b")}) == 3 and p("size_c") == p("size_fit"),
            "settings: Screen size has three values (Small, Fit, Full): three screens, then round")


CHEAT_NAMES = ("Max Money", "Max BP", "Walk Through Walls", "No Random Battle", "Infinite Rare Candy", "Master Ball x99",
               "Shiny Wild Pokemon", "Always Catch", "Max Coins", "Fast Hatching", "Exp x2", "No Poison Damage", "Safari Steps",
               "Instant Text", "Run Indoors", "Max Berries", "Everything Unlocked And A Very Long Name That Cuts",
               "Level 100 Starter", "All TMs", "All HMs", "Max Happiness", "Infinite Repel", "Daycare Egg Now",
               "Master Code")


def cheat_file(n=len(CHEAT_NAMES)):
    """A .cht of n codes (the engine's CodeBreaker form "XXXXXXXX+YYYY"), all off."""
    out = ["cheats = %d" % n, ""]
    for i in range(n):
        out += ['cheat%d_desc = "%s"' % (i, CHEAT_NAMES[i % len(CHEAT_NAMES)]), 'cheat%d_code = "320375D4+%04X"' % (i, i),
                "cheat%d_enable = false" % i, ""]
    return "\n".join(out)


def scene_cheats():
    """Cheats (r19 section 6): Settings with the Cheats tile ("N ON"), the page with codes on, scrolled to its end, the
    "20 codes on at most" band, a short page, and the page from a USB drive (a real FAT image beside the PC tests)."""
    game = "Pokemon - Emerald Version (USA, Europe)"
    to_tile = N.settings_tile("cheats", cheats=True)
    r = H.Run("w_cheats", roms=(game,), files={game + ".cht": cheat_file()}).run("battery 82\nwait 1\npress enter\nwait 2\npress menu\nwait 0.6\n" +
        to_tile + """wait 0.4
shot tile
press enter
wait 0.5
shot page
press enter
press down
press down
press enter
press down
press enter
wait 0.3
shot some_on
press up
press up
press up
press up
wait 0.3
shot end
press up
wait 0.3
shot end2
press esc
wait 0.4
shot tile_after
quit
""")
    l = H.Run("w_cheats_limit", roms=(game,), files={game + ".cht": cheat_file()}).run("battery 82\nwait 1\npress enter\nwait 2\npress menu\nwait 0.6\n" +
        to_tile + "press enter\nwait 0.5\n" + "press enter\npress down\n" * 20 + "wait 0.3\nshot twenty\npress enter\nwait 0.4\nshot limit\nquit\n")
    keep(l, "twenty", "cheats_page_20_on", battery=True)
    keep(l, "limit", "cheats_page_20_on_at_most", battery=True)
    H.check(l.notes["limit"]["ui"] == "cheats:20", "cheats: the 21st code is refused on the picked line: %s" % l.notes["limit"]["ui"])
    for n, name in (("tile", "settings_cheats_tile"), ("page", "cheats_page"), ("some_on", "cheats_page_some_on"),
                    ("end", "cheats_page_scrolled_to_its_end"), ("end2", "cheats_page_scrolled_up_a_line"),
                    ("tile_after", "settings_cheats_tile_with_codes_on")):
        keep(r, n, name, battery=True)
    ui = lambda k: r.notes[k]["ui"]
    H.check(ui("tile") == "Settings:%d" % N.settings_index("cheats", cheats=True) and ui("page") == "cheats:0" and
            ui("some_on") == "cheats:3" and ui("end") == "cheats:%d" % (len(CHEAT_NAMES) - 1) and
            ui("tile_after") == "Settings:%d" % N.settings_index("cheats", cheats=True),
            "cheats: the screen reports: %s" % [(k, r.notes[k]["ui"]) for k in r.notes])
    # a file with no usable code (an Action Replay one): the tile stays, saying NONE USABLE; enter opens no page
    nu = H.Run("w_cheats_none", roms=(game,), files={game + ".cht": 'cheats = 1\n\ncheat0_desc = "AR"\ncheat0_code = "82000568 423F0000"\ncheat0_enable = false\n'}).run(
        "battery 82\nwait 1\npress enter\nwait 2\npress menu\nwait 0.6\n" + to_tile + "wait 0.4\nshot tile\npress enter\nwait 0.5\nshot after_enter\nquit\n")
    keep(nu, "tile", "settings_cheats_tile_none_usable", battery=True)
    keep(nu, "after_enter", "settings_cheats_none_usable_enter_opens_no_page", battery=True)
    H.check(nu.notes["after_enter"]["ui"] == "Settings:%d" % N.settings_index("cheats", cheats=True),
            "cheats: enter on NONE USABLE opens no page: %s" % nu.notes["after_enter"]["ui"])
    # a short file, from a USB drive
    base = os.path.join(H.HERE, H.BUILD, "drive-work", "fat16.img")
    if os.path.exists(base):
        import subprocess
        img = os.path.join(H.WORK, "w_cheats_drive.img")
        os.makedirs(H.WORK, exist_ok=True)
        subprocess.run(["cp", "--sparse=always", base, img], check=True)
        tmp = os.path.join(H.WORK, "w_cheats_drive.cht")
        open(tmp, "w").write(cheat_file(6))
        subprocess.run(["mcopy", "-o", "-i", img, tmp, "::/" + game + ".cht"], env=dict(os.environ, MTOOLS_SKIP_CHECK="1"), check=True)
        d = H.Run("w_cheats_drive", roms=(game,)).run("battery 82\nwait 1\npress enter\nwait 2\npress menu\nwait 0.6\n" + to_tile +
            "wait 0.4\nshot tile\npress enter\nwait 0.5\npress enter\nwait 0.3\nshot page\nquit\n", env={"GPSP_USB_IMAGE": img})
        keep(d, "tile", "settings_cheats_tile_from_a_drive", battery=True)
        keep(d, "page", "cheats_page_from_a_drive", battery=True)
    else:
        print("cheats: the drive's page skipped (no %s: run drive_tests.sh first)" % base, file=sys.stderr)


def scene_controller():
    """Settings > Controller (r19, the reference first findings): one list, a line a connected controller (its name left,
    USB or BLUETOOTH right); none: one sentence; a sentence under the list for what the port asks (after 2 s); D opens
    Details; the band "ENTER RESTART USB  D DETAILS"."""
    game = ("Pokemon - Emerald Version (USA, Europe)",)
    to = "press menu\nwait 0.5\n" + N.settings_tile("controller") + "press enter\nwait 0.8\n"
    r = H.Run("w_controller_none", roms=game).run("battery 82\nwait 1\npress enter\nwait 2\n" + to + "wait 2.5\nshot none\nquit\n")
    keep(r, "none", "settings_controller_none", battery=True)
    r = H.Run("w_controller_usb", roms=game).run("battery 82\nwait 1\npress enter\nwait 2\npad in xinput\nwait 1.5\n" + to + "wait 2.5\nshot usb\n"
                                                 "press d\nwait 0.8\nshot details\nquit\n")
    keep(r, "usb", "settings_controller_usb", battery=True)
    keep(r, "details", "settings_controller_details", battery=True)
    H.check(r.notes["usb"]["ui"].startswith("controller:") and r.notes["details"]["ui"].startswith("controller"),
            "controller: the list and Details report controller:<state>: %s %s" % (r.notes["usb"]["ui"], r.notes["details"]["ui"]))
    r = H.Run("w_controller_both", roms=game).run("battery 82\nwait 1\npress enter\nwait 2\npad in xinput\nwait 1.5\nbt dongle in\nwait 1.5\n"
                                                  "bt pad in dualsense DualSense Wireless Controller\nwait 1.5\n" + to + "wait 2.5\nshot both\nquit\n",
                                                  env={"GPSP_HOST_BT": "1"})
    keep(r, "both", "settings_controller_usb_and_bluetooth", battery=True)
    r = H.Run("w_controller_sentence", roms=game).run("battery 82\nwait 1\npress enter\nwait 2\npad port 2\nwait 1\n" + to + "wait 3\nshot sentence\nquit\n")
    keep(r, "sentence", "settings_controller_sentence", battery=True)


def scene_usb_item():
    """The games' grid with the USB drive's item ("USB DRIVE", no slash; other folders keep theirs), from a real FAT image
    beside the PC tests (run drive_tests.sh first)."""
    base = os.path.join(H.HERE, H.BUILD, "drive-work", "fat16.img")
    if not os.path.exists(base):
        print("usb_item: skipped (no %s: run drive_tests.sh first)" % base, file=sys.stderr)
        return
    import subprocess
    img = os.path.join(H.WORK, "w_usb_item.img")
    os.makedirs(H.WORK, exist_ok=True)
    subprocess.run(["cp", "--sparse=always", base, img], check=True)
    r = H.Run("w_usb_item", roms=GAMES[:2])
    os.makedirs(os.path.join(r.gba, "GBC"))
    r.run("battery 82\nwait 1.2\nshot grid\npress right\npress right\nwait 0.4\nshot folder\npress down\npress left\nwait 0.4\nshot usb\n"
          "press right\nwait 0.4\nshot back\nquit\n", env={"GPSP_USB_IMAGE": img})
    keep(r, "grid", "game_list_with_usb_drive", battery=True)
    keep(r, "folder", "game_list_folder_item_with_slash", battery=True)
    keep(r, "usb", "game_list_usb_drive_item", battery=True)
    keep(r, "back", "game_list_back_after_usb_drive", battery=True)
    H.check([r.notes[k]["ui"] for k in ("folder", "usb", "back")] == ["list:2", "list:3", "list:4"],
            "usb item: the games, GBC/, USB DRIVE, BACK: %s" % [(k, r.notes[k]["ui"]) for k in r.notes])


def scene_link():
    """Settings > Link: the Link tile for each state (r19: Link is the first tile of the page, no tab or card; its
    value says what is plugged in or linked), the wait screen, the messages over the game."""
    start = "wait 1\npress enter\nwait 1.5\n"
    to_link = "press menu\nwait 0.5\n" + N.settings_tile("link") + "wait 0.5\n"
    leave = "press esc\npress esc\npress esc\nwait 0.5\nquit\n"
    for name, script, extra, png in (
            ("pad", "pad in xinput\nwait 1\n" + to_link + "shot a\n" + leave, [], "settings_link_controller"),
            ("swap", "pad port 2\nwait 1.5\npad port 0\nwait 0.12\n" * 4 + "pad port 2\nwait 1\n" + to_link + "shot a\n" + leave,
             [], "settings_link_swap_the_cables_ends"),
            ("nothing", "pad port 1\nwait 9\n" + to_link + "shot a\n" + leave, [], "settings_link_nothing_plugged"),
            ("found", "pad port 1\npad calc node\nwait 12\n" + to_link + "shot a\n" + leave, [],
             "settings_link_other_calculator_found"),
            ):
        r = H.Run("w_link_" + name).run(start + script, "Pattern.gba.tns", extra)
        keep(r, "a", png)

    # picked Link here, not there yet
    runs = [H.Run("w_link_looking_%d" % p) for p in (1, 2)]
    sock = os.path.join(H.WORK, "w_link_looking.sock")
    scripts = ("link\nwait 1\n" + to_link + "shot a\n" + leave, "wait 12\nquit\n")
    procs = [r.start(start + script, "Pattern.gba.tns", ["--link", sock, "--player", str(p)])
             for p, (r, script) in enumerate(zip(runs, scripts), 1)]
    for r, proc in zip(runs, procs):
        r.finish(proc)
    keep(runs[0], "a", "settings_link_cable_came_out")

    # two linked: player 2 opens its menu, player 1 waits for it (the wait screen)
    runs = [H.Run("w_link_two_%d" % p) for p in (1, 2)]
    sock = os.path.join(H.WORK, "w_link_two.sock")
    cost = "cost frame 4\ncost send 0.1\ncost receive 0.1\nframes 30\nlink\nframes 10\n"
    one = cost + "wait 3\nshot waiting\nwait 9\nshot game\nwait 1\nquit\n"
    two = cost + "wait 0.5\npress menu\nwait 0.5\n" + N.settings_tile("link") + "wait 0.5\nshot menu\nwait 7\n" + \
        "press esc\npress esc\npress esc\nwait 1\nframes 30\nwait 6\nquit\n"
    procs = [r.start(script, "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-device", "exact",
                                                 "--link-delay", "20", "--link-slices"])
             for p, (r, script) in enumerate(zip(runs, (one, two)), 1)]
    for r, proc in zip(runs, procs):
        r.finish(proc)
    keep(runs[1], "menu", "settings_link_linked")
    keep(runs[0], "waiting", "link_wait_other_in_menu")
    keep(runs[0], "game", "game_linked_icon")


def scene_game():
    """What is drawn over the game: the speed readout, messages (toasts), the controller's and the link's icons."""
    cfg = {"gpsp_qol.cfg.tns": H.owner_config()}   # the speed readout on
    r = H.Run("w_game", files=cfg).run(H.REAL_COSTS + """wait 1
press enter
wait 4
shot speed
press S
wait 0.3
shot saved
wait 3
press F
wait 4
shot fast
press F
wait 3
pad in xinput
wait 0.6
shot controller
quit
""")
    slow = H.Run("w_game_slow", files=cfg).run("""cost frame 40
cost line 25
wait 1
press enter
wait 6
shot slow
battery 12
wait 1
shot low_battery
quit
""")
    keep(slow, "slow", "game_speed_slow")
    keep(slow, "low_battery", "game_low_battery_icon")
    for n, name in (("speed", "game_speed_readout"), ("saved", "game_toast_saved"), ("fast", "game_speed_fast_forward"),
                    ("controller", "game_toast_controller_connected")):
        keep(r, n, name)


def scene_controls():
    """Controls: the keys on a drawing of the keypad, setting a key, a key that is taken, Reset; the
    controller's view (a GBA drawn) with its own button names, setting one, a calculator key refused."""
    to_controls = ("wait 1\npress enter\nwait 2\npad in xinput\nwait 1.5\npress menu\nwait 0.5\n" +
                   N.settings_tile("keys") + "press enter\nwait 0.5\n")
    r = H.Run("w_controls", roms=("Pokemon - Emerald Version (USA, Europe)",)).run(to_controls + """shot top
press down
press down
press down
press down
wait 0.3
shot a_button
press enter
wait 0.3
shot press_a_key
press var
wait 0.4
shot key_taken
press esc
wait 0.3
press up
press up
press up
press up
press up
wait 0.3
shot reset
press enter
wait 0.4
shot reset_question
press esc
wait 0.3
press up
press up
press up
wait 0.3
shot nothing_bound
press down
press down
press down
press down
wait 0.3
press right
wait 0.4
press down
press down
press down
press down
wait 0.3
shot controller_a_button
press enter
wait 0.3
shot controller_press_a_button
press var
wait 0.3
shot controller_wrong_key
press Pad_Y
wait 0.4
shot controller_a_button_new
press down
press enter
wait 0.3
press Pad_Y
wait 0.4
shot controller_key_taken
press esc
wait 0.3
press down
press down
press down
press down
press down
wait 0.3
shot controller_menu
quit
""")
    for n, name in (("top", "controls"), ("a_button", "controls_a_button"), ("press_a_key", "controls_press_a_key"),
                    ("key_taken", "controls_key_taken"), ("reset", "controls_reset_line"),
                    ("reset_question", "controls_reset_question"), ("nothing_bound", "controls_no_key_line"),
                    ("controller_a_button", "controls_controller_view"),
                    ("controller_press_a_button", "controls_controller_press_a_button"),
                    ("controller_wrong_key", "controls_controller_wrong_key"),
                    ("controller_a_button_new", "controls_controller_a_button_set"),
                    ("controller_key_taken", "controls_controller_key_taken"),
                    ("controller_menu", "controls_controller_menu")):
        keep(r, n, name)

    # the very first start: "The keys", then the game with its message
    os.environ["GPSP_HOST_HINTS"] = "1"
    try:
        r = H.Run("w_first_run", roms=("Pokemon - Emerald Version (USA, Europe)",)).run(
            "wait 1\npress enter\nwait 2\nshot keys\npress enter\nwait 0.6\nshot game\nquit\n")
    finally:
        del os.environ["GPSP_HOST_HINTS"]
    keep(r, "keys", "controls_first_run")
    keep(r, "game", "game_toast_press_menu")


def scene_questions():
    """The questions and messages over a screen: replace a save, start over, quit, delete saves,
    a save that can't be written."""
    r = H.Run("w_questions", roms=("Pokemon - Emerald Version (USA, Europe)",))
    r.run("wait 1\npress enter\nwait 1.5\n" + "press S\nwait 0.6\n" * 3 + "press Q\nwait 1\nquit\n")
    saves = os.path.join(r.dir, "zgpSP_files")
    now = time.time()
    for slot, seconds in enumerate((3 * 3600, 25 * 60, 120)):
        path = os.path.join(saves, "Pokemon - Emerald Version (USA, Europe).gba%d.svs.tns" % slot)
        set_age(path, now - seconds)
    # S on a saved state saves over it, after a question
    r = H.Run("w_questions2", reuse=r).run("wait 1\npress enter\nwait 2\npress menu\nwait 0.5\n" + H.saves_tile(0) +
                                           """wait 0.3
shot picked
press S
wait 0.5
shot replace
press right
wait 0.3
shot replace_red
press esc
wait 0.4
shot kept
quit
""")
    keep(r, "picked", "saves_state_picked_for_saving_over")
    keep(r, "replace", "question_replace_slot")
    keep(r, "replace_red", "question_replace_slot_red_picked")
    keep(r, "kept", "question_replace_slot_kept")
    H.check(r.notes["picked"]["ui"] == "saves:2" and r.notes["replace"]["ui"].startswith("dialog:") and
            r.notes["kept"]["ui"] == "saves:2",
            "questions: S on a state asks, esc keeps it and the arrows stay on the tile: %s" % [(k, r.notes[k]["ui"]) for k in r.notes])

    # Settings, System, Game: Restart
    r1 = H.Run("w_questions2b", reuse=r).run("wait 1\npress enter\nwait 2\npress menu\nwait 0.5\n" + N.more_tile("game") +
                                             "press enter\nwait 0.5\nshot start_over\nquit\n")
    keep(r1, "start_over", "question_start_over")

    # SAVES: del on a state, A (all) then del
    r2 = H.Run("w_questions3", reuse=r).run("wait 1\npress enter\nwait 2\npress menu\nwait 0.5\n" + H.saves_tile(0) +
        "press d\nwait 0.5\nshot delete\n"
        "press esc\nwait 0.4\npress A\nwait 0.3\npress d\nwait 0.5\nshot delete_all\nquit\n")
    keep(r2, "delete", "question_delete_save")
    keep(r2, "delete_all", "question_delete_saves")

    # no room for the save
    r3 = H.Run("w_questions4", roms=("Pokemon - Emerald Version (USA, Europe)",)).run(
        "wait 1\npress enter\nwait 1.5\npress menu\nwait 0.5\n" + H.SAVE_NEW + "wait 0.6\nshot full\nquit\n",
        options=["--free-storage", "1"])
    keep(r3, "full", "question_cant_save")


def scene_no_game():
    """The menus when the game that was picked would not start: only what needs no game (SETTINGS GAMES QUIT)."""
    r = H.Run("w_nogame", roms=("Pattern", "Pokemon - Emerald (USA)"))
    H.words_file(r.file("Pokemon - Emerald (USA).gba.tns"), [0xE59F0000, 0xE12FFF10, 0x910803DF])
    r.run(H.REAL_COSTS + "wait 1\npress enter\nwait 2\npress menu\nwait 0.5\n" + H.to_main("game_list") +
          "press enter\nwait 0.8\npress right\npress enter\nwait 1\nshot stopped\npress enter\nwait 0.6\nshot main\n"
          "press right\nwait 0.3\nshot row\npress right\nwait 0.4\nshot quit_word\npress left\npress left\nwait 0.3\nshot settings\npress down\n" +
          "press right\n" * N.settings_index("more", game=False) + "wait 0.3\nshot more_tile\npress enter\nwait 0.4\nshot more\n" +
          "press right\n" * N.more_index("saves", game=False) + "wait 0.3\nshot more_saves\npress enter\nwait 0.5\nshot more_saves_enter\nquit\n")
    keep(r, "main", "no_game_menu")
    keep(r, "row", "no_game_games_word")
    keep(r, "quit_word", "no_game_quit_word")
    keep(r, "settings", "no_game_settings")
    keep(r, "more_tile", "no_game_more_tile")
    keep(r, "more", "no_game_more")
    keep(r, "more_saves", "no_game_more_saves_tile")
    keep(r, "more_saves_enter", "no_game_more_saves_tile_enter")
    H.check([r.notes[k]["ui"] for k in ("main", "row", "quit_word", "settings")] ==
            ["main:%d" % H.NO_GAME[k] for k in ("settings", "games", "exit", "settings")],
            "no game: the top row has SETTINGS GAMES QUIT: %s" % [(k, r.notes[k]["ui"]) for k in r.notes])


def scene_loading():
    """Starting a game: the loading bar (with a save's picture, without), a game that stops, a zip that can't be unpacked."""
    big = H.Run("w_loading", roms=())
    path = big.file("Big.gba.tns")
    H.pattern_rom(path)
    with open(path, "ab") as f:
        f.write(b"\xff" * (4 * 1024 * 1024 - os.path.getsize(path)))
    big.run(H.REAL_COSTS + "cost read 600\nwait 1\npress enter\nwait 1.2\nshot loading\nwait 3\npress S\nwait 1\nquit\n")
    keep(big, "loading", "loading")
    again = H.Run("w_loading2", reuse=big).run(H.REAL_COSTS + "cost read 600\nwait 1\npress enter\nwait 1.2\nshot loading\nquit\n")
    keep(again, "loading", "loading_with_a_save")

    r = H.Run("w_damaged", roms=("Pattern",))
    H.words_file(r.file("Pokemon - Emerald (USA).gba.tns"), [0xE59F0000, 0xE12FFF10, 0x910803DF])
    r.run(H.REAL_COSTS + "wait 1\npress right\npress enter\nwait 1\nshot stopped\nquit\n")
    keep(r, "stopped", "loading_stopped")


def scene_loading_zip():
    """A zip made a .gbz, once; a zip that does not fit and cannot be made one."""
    r = H.Run("w_zip", roms=())
    tmp = r.file("rom.tmp")
    H.mixed_rom(tmp)
    with zipfile.ZipFile(r.file("Big Game (USA).zip.tns"), "w", zipfile.ZIP_DEFLATED) as z:
        z.write(tmp, "roms/Big Game.gba")
    os.remove(tmp)
    made = H.Run("w_zip_made", reuse=r).run(H.REAL_COSTS + """cost read 600
wait 1
press enter
wait 2.4
shot making
wait 1.5
shot checking
wait 6
quit
""", options=("--rom-memory", "2"))
    keep(made, "making", "loading_getting_it_ready")
    keep(made, "checking", "loading_getting_it_ready_checking")
    full = H.Run("w_zip_full", reuse=r).run(H.REAL_COSTS + """wait 1
press enter
wait 4
shot error
quit
""", options=("--rom-memory", "2", "--free-storage", "512"))
    keep(full, "error", "loading_couldnt_start")


def scene_managers():
    """Settings, System, Games: the ROM manager; a game's saves page."""
    games = GAMES[:3]
    base = play_and_save(H.Run("w_managers", roms=games), games, {sorted(games, key=str.lower)[0]: 4})
    r = H.Run("w_managers2", reuse=base).run("wait 1\npress enter\nwait 2\npress menu\nwait 0.5\n" + N.more_tile("games") +
                                             """press enter
wait 0.6
shot rom_manager
wait 2.2
shot rom_manager_scrolling
quit
""")
    keep(r, "rom_manager", "rom_manager")
    keep(r, "rom_manager_scrolling", "rom_manager_name_scrolling")
    scrolled(r, "rom_manager", "rom_manager_scrolling", "ROM manager")

    # r19: a game's saves are a page of tiles, "<GAME> / SAVES", from S on the game in the grid (the paused game's
    # saves are SAVES, the top row's)
    r = H.Run("w_managers3", reuse=base).run("wait 1\npress s\nwait 0.8\nshot saves_page\nquit\n")
    keep(r, "saves_page", "saves_page")

    # nothing in them: a game without saves; a folder without games
    e = H.Run("w_managers_empty", roms=("Pattern",)).run("wait 1\npress s\nwait 0.8\nshot saves\nquit\n")
    keep(e, "saves", "saves_page_empty")
    # (no games at all: the arrows in the grid on its BACK item: esc to the top row (GAMES), left to SETTINGS)
    n = H.Run("w_managers_nogames", roms=()).run("wait 1\n" + ("press esc\nwait 0.3\npress left\npress down\n" + "press right\n" * N.settings_index("more", game=False) + "press enter\nwait 0.4\n" + "press right\n" * N.more_index("games", game=False) + "press enter\nwait 0.5\n") + "shot games\nquit\n")
    keep(n, "games", "rom_manager_empty")


# Emerald's play in scene_court: three more saves, going on in each direction
EMERALD_PLAY = """wait 1
""" + H.pick(5) + """wait 4
press S
wait 0.8
hold right
wait 0.9
release right
wait 0.3
press S
wait 0.8
hold down
wait 0.7
release down
hold left
wait 0.9
release left
wait 0.3
press S
wait 0.8
hold up
wait 1.2
release up
wait 0.3
press S
wait 0.8
press Q
wait 1.5
quit
"""


def scene_court():
    """The design's own pictures, court-<what it shows>.png: the main screens with real games (the ones in
    $GBA_GAMES, linked where they are, never copied; skipped when they aren't there). Each game is
    played a little and saved first, so it has a save's picture; Emerald and Advance Wars go on from
    the saves kept beside them."""
    real = ("Advance Wars (USA) (Rev 1)", "Fire Emblem - The Sacred Stones (USA, Australia)", "Golden Sun (USA, Europe)",
            "Legend of Zelda, The - The Minish Cap (USA)", "Pokemon - Emerald Version (USA, Europe)")
    if not all(os.path.exists(os.path.join(H.GBA_GAMES, g + ".gba")) for g in real):
        print("court: skipped (the games aren't in %s)" % H.GBA_GAMES, file=sys.stderr)
        return
    r = H.Run("w_court", roms=())
    saves = os.path.join(r.dir, "zgpSP_files")
    os.makedirs(saves)
    for g in real:
        os.symlink(os.path.join(H.GBA_GAMES, g + ".gba"), r.file(g + ".gba.tns"))
    for state, game in (("aw_latest.svs.tns", real[0]), ("emerald_route_grass.svs.tns", real[4])):
        if os.path.exists(os.path.join(H.GBA_GAMES, state)):
            shutil.copy(os.path.join(H.GBA_GAMES, state), os.path.join(saves, game + ".gba0.svs.tns"))

    # each game played and saved: the two with a save go on from it; the others run their opening.
    # (Each game in a calculator of its own, side by side: a game's files are its own, and the games' saves are
    # put together in this one's folder after. The games are the scene's slow part: seconds of play at fast
    # forward.)
    plays = {
        1: "wait 1\n" + H.pick(1) + "wait 4\npress S\nwait 0.8\npress Q\nwait 1.5\nquit\n",
        2: "wait 1\n%swait 2\npress F\nwait %d\npress F\nwait 1\npress S\nwait 0.8\npress Q\nwait 1.5\nquit\n" % (H.pick(2), 14),
        3: "wait 1\n%swait 2\npress F\nwait %d\npress F\nwait 1\npress S\nwait 0.8\npress Q\nwait 1.5\nquit\n" % (H.pick(3), 16),
        4: "wait 1\n%swait 2\npress F\nwait %d\npress F\nwait 1\npress S\nwait 0.8\npress Q\nwait 1.5\nquit\n" % (H.pick(4), 12),
        5: EMERALD_PLAY,
    }

    def play(n):
        g = H.Run("w_court_g%d" % n, roms=())
        gsaves = os.path.join(g.dir, "zgpSP_files")
        os.makedirs(gsaves)
        for game in real:
            os.symlink(os.path.join(H.GBA_GAMES, game + ".gba"), g.file(game + ".gba.tns"))
        for state, game in (("aw_latest.svs.tns", real[0]), ("emerald_route_grass.svs.tns", real[4])):
            if game == real[n - 1] and os.path.exists(os.path.join(H.GBA_GAMES, state)):
                shutil.copy(os.path.join(H.GBA_GAMES, state), os.path.join(gsaves, game + ".gba0.svs.tns"))
        g.run(plays[n])
        for f in os.listdir(gsaves):
            if f.startswith(real[n - 1]):
                shutil.copy(os.path.join(gsaves, f), os.path.join(saves, f))

    with concurrent.futures.ThreadPoolExecutor(max_workers=len(plays)) as pool:
        for future in [pool.submit(play, n) for n in sorted(plays, reverse=True)]:
            future.result()
    now = time.time()
    ages = {real[0]: 2 * 86400, real[1]: 5 * 86400, real[2]: 22 * 86400, real[3]: 3 * 3600}
    emerald = sorted((f for f in os.listdir(saves) if f.startswith(real[4]) and f.endswith(".svs.tns")),
                     key=lambda f: int(f[len(real[4]) + 4:-8]))
    for f in os.listdir(saves):
        for game, age in ages.items():
            if f.startswith(game) and f.endswith(".svs.tns"):
                set_age(os.path.join(saves, f), now - age)
    for f, age in zip(emerald, (9 * 86400, 26 * 3600, 5 * 3600, 40 * 60, 12 * 60, 3 * 60, 40, 20)):
        set_age(os.path.join(saves, f), now - age)

    # the arrows as the plain look has them: the game list a grid of three (up/down a row, left/right a
    # column); the paused menu one row (left/right along it, up/down another game on Resume, another slot
    # on Save and Load); a settings page two columns (up/down a row, left/right a column)
    r.run("""battery 82
wait 1.2
press down
press right
wait 0.8
shot games
press up
wait 0.8
shot games_other
press down
press enter
wait 3.5
shot play
press menu
wait 0.5
shot paused
press right
wait 0.6
shot save
press right
wait 0.6
shot state
press down
wait 0.6
shot state_older
press up
press up
wait 0.3
press left
wait 0.4
shot settings
press down
wait 0.4
shot picture
press right
press enter
wait 0.3
shot colours
press esc
wait 0.3
""" + "press right\n" * (N.settings_index("keys") - 1) + """press enter
wait 0.5
shot keys
press down
press down
press down
press down
wait 0.3
shot keys_a
quit
""")
    for n, name in (("games", "court-games"), ("games_other", "court-games-other"), ("play", "court-play"),
                    ("paused", "court-paused"), ("save", "court-save"), ("state", "court-state"),
                    ("state_older", "court-state-older"),
                    ("settings", "court-settings"), ("picture", "court-settings-tiles"), ("colours", "court-brightness-changing"),
                    ("keys", "court-keys"), ("keys_a", "court-keys-a")):
        keep(r, n, name, battery=name not in ("court-play",))

    # UI17: the store with the games' own pictures, its lines, Emerald's Saves (eight states) from the store and
    # from the paused menu's Load
    r.run("""battery 82
wait 1.2
shot store
press down
press right
wait 1
shot store_emerald
press v
wait 1
shot store_lines
press v
wait 0.6
press s
wait 2.5
shot saves
press right
wait 1
shot saves_picked
press esc
wait 0.6
press enter
wait 3
press menu
wait 1
shot saves_from_game
press down
wait 0.8
shot second_row
quit
""")
    for n, name in (("store", "court-store"), ("store_emerald", "court-store-emerald"), ("store_lines", "court-store-lines"),
                    ("saves", "court-saves"), ("saves_picked", "court-saves-picked"), ("second_row", "court-saves-from-game-second-row"),
                    ("saves_from_game", "court-saves-from-game")):
        keep(r, n, name)
    # a state picked in the store's Saves: the loading screen shows that one's picture (slot 4's, 40 min old)
    r.run(H.REAL_COSTS + """cost read 600
wait 1.2
press down
press right
wait 0.6
press s
wait 2.5
shot saves_slot4
press down
press left
wait 1
shot saves_slot4
press enter
wait 1.5
shot loading_picked
quit
""")
    keep(r, "saves_slot4", "court-saves-slot-4-picked")
    keep(r, "loading_picked", "court-loading-picked-slot")

# ---- UI17: the store (the game list's grid of tiles, or its lines) and the Saves place ----

def to_game(i):
    """Script lines: in a game list just opened, from its first game to the i-th (0 the first): the store's
    grid of three."""
    return "press down\n" * (i // 3) + "press right\n" * (i % 3)


def store_games(name, games, saves, ages):
    """A calculator with games, each in saves played and saved that many times (started from the store by
    its place, in name order), the newest state of each in ages that many seconds old."""
    order = sorted(games, key=str.lower)
    r = H.Run(name, roms=games)
    for game, count in saves.items():
        r = H.Run(name + "_" + str(order.index(game)), reuse=r)
        r.run("wait 1\n" + to_game(order.index(game)) + "press enter\nwait 1.5\n" + "press S\nwait 0.6\n" * count +
              "press Q\nwait 1\nquit\n")
    age_saves(r, ages)
    return r, order


def scene_store():
    """The game list as the store (UI17): the grid of tiles, each its picture, name and facts; the picked one
    on its black plate; a second row picked; a name too long scrolling; many games (which of how many);
    the lines (v); a game's Saves (s)."""
    games = GAMES
    base, order = store_games("w_store", games, {GAMES[0]: 3, GAMES[1]: 6, GAMES[3]: 4, GAMES[6]: 1},
                              {GAMES[0]: 2 * 3600, GAMES[1]: 26 * 3600, GAMES[3]: 9 * 86400, GAMES[6]: 600})
    r = H.Run("w_store2", reuse=base).run("battery 82\nwait 0.8\nshot grid\n" + to_game(4) + "wait 0.7\nshot second_row\n"
                                          "wait 2.5\nshot scrolling\npress v\nwait 1\nshot lines\npress down\npress down\n"
                                          "wait 1\nshot lines_down\npress v\nwait 1\nshot grid_again\n"
                                          "press s\nwait 1.5\nshot saves\nquit\n")
    scrolled(r, "second_row", "scrolling", "store")
    for n, name in (("grid", "store"), ("second_row", "store_second_row"), ("scrolling", "store_name_scrolling"),
                    ("lines", "store_lines"), ("lines_down", "store_lines_down"), ("grid_again", "store_back_to_grid"),
                    ("saves", "store_saves")):
        keep(r, n, name)

    # many games: which of how many, the lines scrolled
    many = tuple("Game %03d (USA)" % i for i in range(1, 41))
    m = H.Run("w_store_many", roms=many).run("wait 1\n" + "press down\n" * 5 + "press right\nwait 0.6\nshot grid\n"
                                            "press v\nwait 0.6\nshot lines\n" + "press down\n" * 12 +
                                            "wait 0.6\nshot lines_far\nquit\n")
    keep(m, "grid", "store_many")
    keep(m, "lines", "store_many_lines")
    keep(m, "lines_far", "store_many_lines_scrolled")


def write_game_save(run, game, size=0x8000, packed=False):
    """The game's own save (its cartridge's memory) as gpSP writes it, in the saves folder."""
    saves = os.path.join(run.dir, "zgpSP_files")
    os.makedirs(saves, exist_ok=True)
    data = bytes((i * 7) & 0xFF for i in range(size))
    name = os.path.join(saves, game + ".gba.sav" + (".gz" if packed else "") + ".tns")
    if packed:
        import gzip
        data = gzip.compress(data)
    open(name, "wb").write(data)


def scene_saves():
    """SAVES (the paused game's saves as tiles: RESUME, SAVE, the game's own save, the states; r19 has no Save and Load
    strips or Saves place for it): marks swept with m held, unpacking, the question before a delete; none; with a USB drive
    (the PC's stand-in): the places on tab, the drive's tiles; another game's saves, from the grid's S, copied to the
    drive."""
    game = GAMES[0]
    r = H.Run("w_saves", roms=(game,))
    r.run("wait 1\npress enter\nwait 1.5\n" + "press S\nwait 0.6\n" * 7 + "press Q\nwait 1\nquit\n")
    write_game_save(r, game)
    saves = os.path.join(r.dir, "zgpSP_files")
    now = time.time()
    for slot, seconds in enumerate((12 * 86400, 4 * 86400, 26 * 3600, 5 * 3600, 3000, 600, 30)):
        path = os.path.join(saves, game + ".gba%d.svs.tns" % slot)
        set_age(path, now - seconds)
    p = H.Run("w_saves2", reuse=r).run("""battery 82
wait 1
press enter
wait 2
press menu
wait 1
shot place
press right
press right
wait 0.8
shot game_save
press left
press left
press down
wait 0.8
shot place_second
press down
wait 0.8
shot place_down
press up
hold m
wait 0.2
press right
press right
wait 0.2
release m
wait 0.5
shot marked
press U
wait 1.5
shot uncompressed
press A
wait 0.3
press d
wait 0.5
shot question
press esc
wait 0.5
shot back
press up
press up
wait 0.5
shot top_row
quit
""")
    for n, name in (("place", "saves"), ("game_save", "saves_game_save_picked"), ("place_second", "saves_picked"),
                    ("place_down", "saves_second_row"), ("marked", "saves_marked"),
                    ("uncompressed", "saves_uncompressed"), ("question", "saves_delete_question"),
                    ("back", "saves_after_the_question"), ("top_row", "saves_top_row")):
        keep(p, n, name, battery=True)
    H.check([p.notes[k]["ui"] for k in ("place", "game_save", "place_second", "place_down", "top_row")] ==
            ["saves:0", "saves:2", "saves:3", "saves:6", "main:%d" % H.MAIN["saves"]],
            "saves: the screen reports (RESUME, SAVE, the game's save, then the states): %s" %
            [(k, p.notes[k]["ui"]) for k in p.notes])

    # enter on IN-GAME SAVE asks first
    ig = H.Run("w_saves_ig", reuse=r).run("wait 1\npress enter\nwait 2\npress menu\nwait 1\npress right\npress right\npress enter\nwait 0.6\n"
                                          "shot ask\nquit\n")
    keep(ig, "ask", "question_restart_from_in_game_save")
    H.check(ig.notes["ask"]["ui"].startswith("dialog:"), "saves: enter on IN-GAME SAVE asks first: %s" % ig.notes["ask"]["ui"])

    # a game without any save: RESUME and SAVE alone
    e = H.Run("w_saves_none", roms=("Pattern",)).run("wait 1\npress enter\nwait 2\npress menu\nwait 0.8\nshot none\nquit\n")
    keep(e, "none", "saves_none")

    env = {"GPSP_HOST_DRIVE_SAVES": "1"}
    d = H.Run("w_saves_drive", reuse=r).run("""wait 1
press enter
wait 2
press menu
wait 1.5
shot calc
press tab
wait 1.5
shot drive
press right
press right
wait 0.6
shot drive_state
press d
wait 0.5
shot drive_question
press right
press enter
wait 1
shot drive_deleted
quit
""", env=env)
    keep(d, "calc", "saves_with_drive")
    keep(d, "drive", "saves_drive_tab")
    keep(d, "drive_state", "saves_drive_state_picked")
    keep(d, "drive_question", "saves_drive_delete_question")
    keep(d, "drive_deleted", "saves_drive_deleted")

    # another game's states: its page from S on the grid, while a game is paused; T copies one to the drive
    o, order = store_games("w_saves_other", (GAMES[0], GAMES[2]), {GAMES[2]: 2}, {})
    o = H.Run("w_saves_other2", reuse=o).run("wait 1\n" + to_game(order.index(GAMES[0])) + "press enter\nwait 2\n"
                                             "press menu\nwait 0.5\n" + H.to_main("game_list") + "press enter\nwait 1\n"
                                             "press s\nwait 1.5\nshot other\npress t\nwait 1\nshot copied\nquit\n", env=env)
    keep(o, "other", "saves_other_game_while_paused")
    keep(o, "copied", "saves_other_game_copied")


# ---- r16: the screens other work brings, drawn now with made-up words (the widget spec) ----

# Settings > Saving: "Saves on" comes after Resume on start, Save on quit, Save to, Autosave, Confirm delete
# (Autosave to only with Autosave on), and is there only while a drive can hold saves
SAVES_ON = N.more_index("saves_on", drive=True, autosave_to=True)


def scene_drive_saves():
    """Saves on a USB drive (nspire_places.h; the PC's stand-in, GPSP_HOST_DRIVE_SAVES=1: three made-up slots on
    the drive): SAVES with the two places (tab switches them), and Saving's "Saves on"."""
    env = {"GPSP_HOST_DRIVE_SAVES": "1"}
    r = H.Run("w_drive_saves", roms=("Pokemon - Emerald Version (USA, Europe)",))
    r.run("wait 1\npress enter\nwait 1.5\n" + "press S\nwait 0.6\n" * 2 + "press Q\nwait 1\nquit\n", env=env)
    r = H.Run("w_drive_saves2", reuse=r).run("""wait 1
press enter
wait 2
press menu
wait 0.5
press right
wait 0.5
shot save_calculator
press tab
wait 0.5
press right
shot save_drive
press right
wait 0.5
shot state_drive
press tab
wait 0.5
press right
press right
wait 0.5
shot state_calculator
""" + N.settings_tile("autosave") + H.change("right") + "wait 0.3\n" +
        N.tile_moves(N.settings_index("autosave"), N.settings_index("more")) + """press enter
wait 0.4
shot more_fullest
""" + N.tile_moves(0, SAVES_ON) + """wait 0.3
shot saves_on
press enter
wait 0.3
shot saves_on_changed
quit
""", env=env)
    for n, name in (("save_calculator", "drive_saves_save_calculator"), ("save_drive", "drive_saves_save_drive"),
                    ("state_drive", "drive_saves_state_drive"), ("state_calculator", "drive_saves_state_calculator"),
                    ("more_fullest", "drive_saves_more_fullest"), ("saves_on", "drive_saves_setting"),
                    ("saves_on_changed", "drive_saves_setting_changed")):
        keep(r, n, name)


def scene_bluetooth():
    """Bluetooth (nspire_bluetooth.h; the PC's stand-in, GPSP_HOST_BT=1: the reference DualSense, paired, and Switch
    Pro Controller, earbuds, and as a phone lists them (r19-bt): a nameless LE pad by its address, a TV, one with
    a long name; a nameless LE beacon not listed; the earbuds far off: WEAK): Settings' list
    with it, its page, a found controller picked, the nameless pad, the TV picked (ENTER PAIR) and tried ("Not a
    controller or headphones."), a long name scrolling, pairing (ENTER STOP), stopped (r19-bt: enter again), forgetting, a
    paired controller connecting (ENTER STOP DEL FORGET); an LE controller (an Xbox pad) found, pairing,
    connected."""
    env = {"GPSP_HOST_BT": "1"}
    bt = N.settings_index("bluetooth")   # (Bluetooth comes after Controller when the build has it)
    r = H.Run("w_bluetooth", roms=("Pokemon - Emerald Version (USA, Europe)",)).run("""wait 1
press enter
wait 2
pad in xinput
wait 1.5
press menu
wait 0.5
""" + N.settings_tile("bluetooth") + """wait 0.4
shot settings
press enter
wait 0.5
shot page
press right
wait 0.3
shot search
press down
wait 0.3
shot paired
press down
wait 0.3
shot found_controller
press down
wait 0.3
shot found_nameless_pad
press down
press down
wait 0.3
shot found_other
shot long_name
wait 2.2
shot long_name_scrolling
press enter
wait 1.6
shot found_other_tried
press up
press up
press up
bt connect hold
press enter
wait 0.4
shot pairing
press enter
wait 0.4
shot stopped
press up
wait 0.3
press del
wait 0.5
shot forget
press esc
wait 0.3
press enter
wait 0.5
press enter
wait 0.4
shot connecting
bt connect release
wait 1.5
press esc
wait 0.3
""" + N.tile_moves(bt, N.settings_index("controller")) + """press enter
wait 0.8
shot controller
quit
""", env=env)
    # (the driver keeps 31 characters of a name: this one comes cut there, the page ends it with "...", and it fits its
    # line: nothing to scroll, the two screens are the same; a real scroll is the grid's, game_list_long_name_scrolling)
    p = lambda n: open(os.path.join(r.out, n + ".ppm"), "rb").read()
    H.check(p("long_name") == p("long_name_scrolling"), "Bluetooth page: a name that fits (cut by the driver, with its "
            "ellipsis) stays still")
    for n, name in (("settings", "bluetooth_in_settings"), ("page", "bluetooth"), ("search", "bluetooth_search_picked"),
                    ("paired", "bluetooth_paired_picked"),
                    ("found_controller", "bluetooth_found_controller"), ("found_other", "bluetooth_found_other"),
                    ("found_nameless_pad", "bluetooth_found_nameless_pad"),
                    ("found_other_tried", "bluetooth_found_other_tried"),
                    ("long_name", "bluetooth_long_name"), ("long_name_scrolling", "bluetooth_long_name_scrolling"),
                    ("pairing", "bluetooth_pairing"), ("stopped", "bluetooth_stopped"),
                    ("forget", "bluetooth_forget_question"), ("connecting", "bluetooth_connecting"),
                    ("controller", "bluetooth_controller_page")):
        keep(r, n, name)
    # r19-bt (a phone's list): a TV, a phone, a computer with their words, a
    # nameless LE beacon not listed; the TV picked (ENTER PAIR) and tried: "Not a controller or headphones."
    r = H.Run("w_bluetooth_phone", roms=("Pokemon - Emerald Version (USA, Europe)",)).run("""wait 1
press enter
wait 1
bt dongle in
wait 1
bt pad in dualsense DualSense Wireless Controller
bt near tv -50 Example TV Set Model 0001
bt near phone -55 Galaxy S24
bt near computer -70 DESKTOP-7Q2K4M
bt near unknown-le -40
wait 1
press menu
wait 0.5
""" + N.settings_tile("bluetooth") + """wait 0.4
press enter
wait 1
press down
wait 0.3
press down
wait 0.3
shot phone_tv
press enter
wait 1.6
shot phone_tv_tried
quit
""")
    for n, name in (("phone_tv", "bluetooth_like_a_phone"), ("phone_tv_tried", "bluetooth_tv_tried")):
        keep(r, n, name)
    # r19-bt: an LE controller (the reference Xbox Wireless Controller) found, pairing (ENTER STOP), connected
    # (its own devices, not GPSP_HOST_BT's five: a list that scrolls draws its bar against the picked line's
    # block, nspire_ui_screens.c's ui_list_page_draw, left as it is)
    r = H.Run("w_bluetooth_le", roms=("Pokemon - Emerald Version (USA, Europe)",)).run("""wait 1
press enter
wait 1
bt dongle in
wait 1
bt pad in dualsense DualSense Wireless Controller
bt near pad-le -44 Xbox Wireless Controller
bt near pad -60 Pro Controller
bt near headphones -80 Example Buds
bt connect hold
wait 1
press menu
wait 0.5
""" + N.settings_tile("bluetooth") + """wait 0.4
press enter
wait 1
press down
wait 0.3
press down
wait 0.3
shot le_found
press enter
wait 0.4
shot le_pairing
bt connect release
wait 1.5
shot le_connected
press esc
wait 0.3
""" + N.tile_moves(N.settings_index("bluetooth"), N.settings_index("controller")) + """press enter
wait 0.8
shot le_controller
quit
""")
    for n, name in (("le_found", "bluetooth_le_found"), ("le_pairing", "bluetooth_le_pairing"),
                    ("le_connected", "bluetooth_le_connected"), ("le_controller", "bluetooth_le_controller_page")):
        keep(r, n, name)
    # bt10: Volume's band with earbuds and a USB sound adapter both in (Output Automatic: both play, its band says so),
    # then Output Bluetooth or USB from the settings file (the Sound output tile is gone: r19 fourth pass)
    def volume_band(name, output):
        o = H.owner_config()
        o[22] = 0xB7000001 | (output << 4)
        return H.Run(name, roms=("Pokemon - Emerald Version (USA, Europe)",), files={"gpsp.cfg.tns": o}).run("""wait 1
press enter
wait 2
bt dongle in
wait 1.5
bt headphones in Example Headphones
pad in sound
wait 1.5
press menu
wait 0.5
""" + N.settings_tile("volume") + """wait 0.4
shot volume
quit
""", env=env)
    keep(volume_band("w_bluetooth_sound", 0), "volume", "bluetooth_sound_both")
    keep(volume_band("w_bluetooth_sound_usb", 1), "volume", "bluetooth_sound_output_usb")
    keep(volume_band("w_bluetooth_sound_bt", 2), "volume", "bluetooth_sound_headphones_only")


# The toasts over a game: what the frontend says, and the two of the USB hub's work (final/usb8) that are longer
# than the band's one line (shown in two parts, each for its share of its reading time; the message line keeps 63
# characters: nspire_frontend.h OSD_TEXT_MAX, the pinned code's), with the shorter words proposed for them.
TOASTS = (
    ("saved_slot_3", "Saved to slot 3"),
    ("bt_controller", "Controller 2 connected (Bluetooth)"),
    ("bt_headphones", "Headphones connected"),
    ("bt_dongle_hung", "Bluetooth dongle stopped answering: restarting it"),
    ("bt_dongle_replug", "Bluetooth dongle stopped answering: replug it"),
    ("bt_dongle_restart", "Bluetooth: restarting the dongle"),
    ("hub_start", "A device on the hub doesn't start. Try another hub port, or a hub with its own power."),
    ("hub_current", "The USB hub reports too much current. Use a hub with its own power."),
    ("hub_start_short", "A hub device didn't start: try another port"),
    ("hub_current_short", "Hub draws too much current: use a powered hub"),
)


def scene_bluetooth_many():
    """The Bluetooth page as a phone's (r19-bt): 28 named devices of every kind (CONTROLLER, HEADPHONES, SPEAKER, PHONE,
    COMPUTER, TV, KEYBOARD, MOUSE, OTHER) in range: 24 lines at most, the scroll bar, the first and the last screen."""
    kinds = ("pad", "headphones", "speaker", "phone", "computer", "tv", "keyboard", "mouse", "other")
    near = "".join("bt near %s -%d Device %02d of the %s kind\n" % (kinds[i % len(kinds)], 40 + i, i + 1, kinds[i % len(kinds)])
                   for i in range(28))
    r = H.Run("w_bluetooth_many", roms=("Pokemon - Emerald Version (USA, Europe)",)).run("battery 82\nwait 1\npress enter\nwait 2\nbt dongle in\n" + near +
        "wait 1.5\npress menu\nwait 0.5\n" + N.settings_tile("bluetooth") + "press enter\nwait 3\nshot top\n" +
        "press right\npress down\n" + "press down\n" * 8 + "wait 0.4\nshot middle\n" + "press down\n" * 30 + "wait 0.5\nshot end\npress up\nwait 0.4\nshot before_end\nquit\n",
        env={"GPSP_HOST_BT": "1"})
    keep(r, "top", "bluetooth_many_top", battery=True)
    keep(r, "middle", "bluetooth_many_middle", battery=True)
    keep(r, "end", "bluetooth_many_end", battery=True)
    keep(r, "before_end", "bluetooth_many_before_end", battery=True)


def scene_toasts():
    """Toasts over a game picture (the toast script line: nspire_osd_message), each one's first part and, for one
    longer than the band's line, its second part."""
    script = "wait 1\npress enter\nwait 2\n"
    for name, text in TOASTS:
        script += "toast %s\nwait 0.4\nshot %s\nwait 4.2\nshot %s_2\nwait 3\n" % (text, name, name)
    r = H.Run("w_toasts").run(script + "quit\n")
    for name, text in TOASTS:
        keep(r, name, "game_toast_" + name)
        if len(text) > 49:  # 316 px at the menu font's fixed advance
            keep(r, name + "_2", "game_toast_" + name + "_part_2")


SCENES = [scene_bluetooth_many, scene_usb_item, scene_controller, scene_cheats, scene_list, scene_list_names_and_battery, scene_menu, scene_settings, scene_link, scene_game, scene_controls, scene_questions,
          scene_no_game, scene_loading, scene_loading_zip, scene_managers, scene_court,
          scene_drive_saves, scene_bluetooth, scene_toasts, scene_store, scene_saves]


def main():
    global OUT
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    OUT = os.path.abspath(args[0])
    wanted = args[1:]
    os.makedirs(OUT, exist_ok=True)
    for scene in SCENES:
        if not wanted or scene.__name__[6:] in wanted:
            print("scene", scene.__name__[6:], file=sys.stderr)
            scene()
    for name in made:
        print(name + ".png")
    if H.failures:
        print("\n".join("FAIL " + f for f in H.failures))
        sys.exit(1)


if __name__ == "__main__":
    main()
