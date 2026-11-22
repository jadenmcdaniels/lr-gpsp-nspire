#!/usr/bin/env python3
"""gpSP on the simulated calculator (build/gpsp-host): whole scenarios, as a
player would go through them, checked from the screenshots and the report.

    host_tests.py [NAME...]     all tests, or the ones named

Each test gets a folder in build/host_tests/NAME: gba/ is the calculator's
folder (gpSP, ROMs, settings), out/ has the screenshots (.ppm), the report
and the script. Only the standard library is needed. The ROMs are the
pattern ROM (rom/pattern.s): a picture with no black pixel and a white bar
that moves a column every frame, with its frame count at 0x02000000."""

import hashlib
import os
import re
import shutil
import zipfile
import struct
import subprocess
import time
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# GPSP_TEST_BUILD=build-san: the sanitizer build (make SAN=1, tests/san.sh).
BUILD = os.environ.get("GPSP_TEST_BUILD", "build")
HOST = os.path.join(HERE, BUILD, "gpsp-host")
PROGRAM = "_gpSP.tns"   # what gpsp-host runs as, in the game folder
# The most seconds one gpsp-host run may take before it counts as hung (a guard against a hang, not a check:
# every timing the tests check is the run's own simulated time). tests/gate.sh raises it: the longest runs
# (real games played at fast forward, tens of seconds of CPU) take longer with the whole gate on the machine.
HANG = int(os.environ.get("GPSP_TEST_HANG", "300"))
# GPSP_TEST_WORK=DIR: the runs' folders there (tests/gate.sh: one for each part, so that parts run
# side by side; keep the path short, the link tests' sockets are in it).
WORK = os.environ.get("GPSP_TEST_WORK") or os.path.join(HERE, BUILD, "host_tests")

# What the calculator's CPU spends, roughly (PocketSNES's measurements on
# a CX II): a frame's emulation, a drawn line, turning a whole
# 320x240 picture, a touchpad read.
REAL_COSTS = """cost frame 7
cost line 25
cost turn 3
cost touchpad 0.4
"""

BLACK = (0, 0, 0)
# The first theme's colours as the screen has them (nspire_ui.c): the picked line's block, and the one mark of
# colour (a progress bar's fill, the picked slot's frame)
ACCENT = (0, 0, 0)
MARK = (0, 0, 0)

failures = []
checks = 0


def check(cond, what):
    global checks
    checks += 1
    if not cond:
        failures.append(what)
        print("  FAIL " + what)
    return cond


# ---- Files ----

def pattern_rom(path):
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "rom", "make_test_rom.py"), path], check=True)


def link_rom(path):
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "rom", "make_link_rom.py"), path], check=True)


def settings_from_file(run, words):
    """A calculator that already ran (reuse=) takes the settings from its program file, where gpSP keeps them: put
    them in its saves folder's copy instead, and give it a program file with none."""
    words_file(run.save("gpsp.cfg.tns"), words)
    with open(os.path.join(run.gba, PROGRAM), "wb") as f:
        f.write(bytes(range(256)) * 16)


def words_file(path, words):
    with open(path, "wb") as f:
        f.write(struct.pack("<%dI" % len(words), *words))


def read_words(path):
    data = open(path, "rb").read()
    return list(struct.unpack("<%dI" % (len(data) // 4), data))


KEYS = {"8": 103, "5": 87, "4": 40, "6": 36, "del": 90, "var": 82, "scratch": 91, "doc": 100,
        "shift": 121, "ctrl": 122, "menu": 102, "F": 65, "L": 49, "S": 23, "Q": 34, "+": 99, "-": 83}


def old_config(scale):
    """gpsp.cfg.tns as gpSP builds before this one wrote it (148 bytes)."""
    o = [0] * 37
    o[0] = scale
    for i, k in enumerate(["8", "5", "4", "6", "del", "var", "scratch", "doc", "shift", "ctrl",
                           "menu", "F", "L", "S", "Q"]):
        o[7 + i] = KEYS[k]
    o[23] = 0
    o[24] = 1           # automatic frameskip
    o[25] = 4
    o[26] = 0xFF000000
    return o


def qol1_config(scale, cpu):
    """gpsp_qol.cfg.tns as the first QoL build wrote it (QOL1)."""
    o = old_config(scale) + [0] * (72 - 37)
    o[37] = 0x314C4F51
    for i in range(15):
        o[38 + i] = o[7 + i]
    o[58] = o[59] = o[60] = 1
    o[61] = cpu
    return o


def owner_config():
    """The reference gpsp_qol.cfg.tns, as read off the calculator on
    2026-10-14 (QOL3): scaled/blended 3:2, interframe blending, frameskip
    off, no sprite limit, Partial SMC, 16 MB ROM buffer, the FPS
    counter on, brightness 9, gpSP's default keys."""
    keys = [0x67, 0x57, 0x28, 0x24, 0x5a, 0x52, 0x5b, 0x64, 0x79, 0x7a, 0x66, 0x41, 0x31, 0x17, 0x22]
    o = [1, 1, 0, 0, 1, 0, 4] + keys + [0, 1, 2, 1, 0xFF000000, 0, 1, 1, 1, 16, 0, 1, 0, 0, 0, 0x334C4F51]
    o += keys + [0, 0x54, 0x56, 0, 0]          # actions 0-19: ) and ( for the slots
    o += [0, 1, 1, 0, 1, 9, 0x63, 0x53, 1] + [0] * 5
    assert len(o) == 72
    return o


# ---- Screenshots ----

class Shot:
    def __init__(self, path, ui=""):
        """ui: the menus' screen on show and its picked line, "main:2"
        (nspire_ui.h), or "" for anything else."""
        self.ui = ui
        self.screen, _, picked = ui.partition(":")
        self.picked = int(picked) if picked else None
        data = open(path, "rb").read()
        header, self.pixels = data[:15], data[15:]
        assert header == b"P6\n320 240\n255\n", header
        self.w, self.h = 320, 240

    def at(self, x, y):
        i = (y * self.w + x) * 3
        return tuple(self.pixels[i:i + 3])

    def box(self, y0=0, y1=240):
        """The smallest rectangle around everything that isn't black."""
        xs, ys = [], []
        for y in range(y0, y1):
            row = [x for x in range(self.w) if self.at(x, y) != BLACK]
            if row:
                xs += [row[0], row[-1]]
                ys.append(y)
        return (min(xs), min(ys), max(xs), max(ys)) if xs else None

    def active_menu_line(self, screen=None):
        """The picked line of a menu, counting the lines it shows from 0
        (the main menu's, or a settings page's: screen "Graphics"...);
        None if that menu isn't on the screen."""
        if self.screen in ("", "list", "folders") or (screen and self.screen != screen):
            return None
        return self.picked

    def active_list_row(self):
        """The picked game of the game list, counting from the first."""
        return self.picked if self.screen == "list" else None

    def bar_x(self):
        """The test ROM's white bar, in screen columns (None if not seen)."""
        y = 120
        xs = [x for x in range(self.w) if min(self.at(x, y)) >= 240]
        return xs[0] if xs else None


# ---- The menus ----

# The menus (r19, docs/ui/r19.txt): one screen. Its top row has a word in each corner and the selector between
# them: with a game paused SETTINGS, SAVES, GAMES, QUIT (MAIN: their places), without one SETTINGS, GAMES, QUIT
# (NO_GAME). The arrows are on the row (left and right move along it, wrapping; down or enter goes into what is
# under the picked word; "main:N" is reported) or in the body under the picked word (up from its top row, or esc,
# goes back to the row). SAVES is the paused game's saves as tiles: RESUME (saves:0), SAVE (saves:1), the game's
# own save if it has one, then the states, the newest first. GAMES is the games' grid, the folders after the
# games ("list:K"). Settings' pages are a grid three across under SETTINGS (SETTINGS: their places; Bluetooth,
# only with it, after Controller; Cheats, only with codes, before System); each page's tiles by name (PAGES: as
# shown with the defaults on a CX II with a game open), in their order: right goes to the next one (across
# rows).
# The paused menu OPENS on SAVES with the arrows in its tiles, RESUME picked; the list at gpSP's start opens with
# the arrows in the games.
MAIN = {"settings": 0, "saves": 1, "games": 2, "exit": 3}
NO_GAME = {"settings": 0, "games": 1, "exit": 2}
# Settings (r19, third pass: one screen of settings): with the block on SETTINGS ("main:0") its tiles are drawn under
# the row; down or enter puts the arrows in them ("Settings:K", K the place among the tiles shown), up from the first
# row of tiles or esc goes back to the row. The tiles: SET_TILES; the last, More, opens one page of the rest
# ("More:K"): MORE_TILES. left/right: the tile before/after in order, wrapping; up/down by row.
BT = os.path.exists(os.path.join(os.environ.get("BT_DIR", os.path.join(HERE, "..", "..", "..", "nspire-bt")),
                                 "include", "nspire_bt.h"))


def set_tiles(game=True, cheats=False):
    """The tiles of Settings, in order, as shown with the defaults on a CX II (Bluetooth where gpsp-host has it
    (../nspire-bt there, as tests/host/Makefile builds it); Link with a game; Cheats with codes)."""
    return (["Volume", "Brightness", "Screen size", "FPS counter", "Autosave", "Keys", "Controller"] +
            (["Bluetooth"] if BT else []) + (["Link"] if game else []) + (["Cheats"] if cheats else []) + ["More"])


def more_tiles(game=True, autosave=False, drive=False, cx2=True):
    """The tiles of More, in order, one screenful (Autosave to only with Autosave on, Saves on with a USB drive, LCD fix
    on the older CX, Game with a game)."""
    return (["Frame skip", "Save to"] + (["Autosave to"] if autosave else []) + (["Saves on"] if drive else []) +
            ["Link device", "Settings for", "Keys for"] + ([] if cx2 else ["LCD fix"]) +
            (["Game"] if game else []) + ["Saves", "Games"] + ["About"])


# the tiles' places (with a game), by lowercase name; the old page names the tests used point at their first tile
SETTINGS = dict((name.lower().replace(" ", "_"), i) for i, name in enumerate(set_tiles()))
PAGES = {"settings": set_tiles(), "more": more_tiles()}
# How many values each setting has: up to CYCLE_MAX, enter is the next value (cycle()); more is a range, changed
# between chevrons (change()). (Frame skip: Auto and Off; a file's Manual shows until it is changed.)
CYCLE_MAX = 4
# Screen size's values, one setting of three: Original (unscaled, 240x160 in the middle: box (40, 40, 279, 199)), Fit
# (scaled_aspect, smoothed: (0, 14, 319, 226); a file's scaled_raw shows as Fit too, the sharp picture (0, 13, ...) a
# new file has), Stretch (fullscreen: the whole screen); enter is the next one, round
SIZES = ["Original", "Fit", "Stretch"]


def size_cycles(to, frm="Fit"):
    """How many enters take Screen size from one value to another (a new calculator shows Fit: scaled_raw)."""
    return (SIZES.index(to) - SIZES.index(frm)) % len(SIZES)


# Where each setting the tests name is: ("settings" or "more", its tile). (The names are the tests' own, from the
# menus before r19 and before.) Gone in r19 (set them in a settings file instead): Frameskip value, Frameskip variation,
# No sprite limit, ROM buffer size, Update backup, Compress saves; Smoothing is gone (Fit is smoothed).
WHERE = {
    "volume": ("settings", "Volume"),
    "size": ("settings", "Screen size"), "brightness": ("settings", "Brightness"),
    "show_fps": ("settings", "FPS counter"),
    "new_slot": ("more", "Save to"), "restart": ("more", "Game"),
    "saves": ("more", "Saves"), "rom_manager": ("more", "Games"),
    "about": ("more", "About"),
    "Link": ("more", "Link device"),
    "Autosave": ("settings", "Autosave"),
    "Keys are for": ("more", "Keys for"), "Settings are for": ("more", "Settings for"),
    "Frame skip": ("more", "Frame skip"), "pace": ("more", "Frame skip"), "Frameskip type": ("more", "Frame skip"),
}
# (the old pages' names the tests used for the tile that stood first on it, now Settings' tiles)
TILE_OF = {"sound": "Volume", "picture": "Screen size", "saving": "Autosave", "link": "Link",
           "system": "More", "keys": "Keys", "controller": "Controller", "bluetooth": "Bluetooth", "more": "More"}

# The paused menu as it opens is SAVES, the arrows in its tiles, RESUME picked. Script lines from there:
RESUME = "press enter\n"                              # back to the game (esc and menu do the same)
SAVE_NEW = "press right\npress enter\n"               # the SAVE tile: saved, and back to the game
SAVES_TO_ROW = "press up\n"                           # the arrows to the top row (on SAVES: "main:1")


def saves_tile(n, game_save=False):
    """Script lines: from the paused menu as it opens (RESUME picked) to the n-th state's tile (0: the newest):
    the tiles are RESUME, SAVE, the game's own save when it has one (game_save), then the states, three across;
    left and right stop at a row's ends, so the walk goes down first, then along the row."""
    i = 2 + (1 if game_save else 0) + n
    return "press down\n" * (i // 3) + "press right\n" * (i % 3)


def load_state(n=0, game_save=False):
    """Script lines: from the paused menu as it opens, the n-th state (0: the newest) loaded: back in the game."""
    return saves_tile(n, game_save) + "press enter\n"


def page_moves(a, b):
    """Script lines: among Settings' tiles or More's, from tile a to tile b (their places): right is the next one,
    left the one before (the tiles go round: the way that is shorter is not looked for)."""
    return "press right\n" * (b - a) if b > a else "press left\n" * (a - b)


def settings_moves(a, b):
    """Script lines: in Settings' tiles, from tile a to tile b (their places, SETTINGS)."""
    return page_moves(a, b)


def settings_leave(at=0, side="left"):
    """Script lines: from Settings' tiles back to the top row (esc: on SETTINGS), then to the word at its left (QUIT:
    the row wraps) or its right (SAVES with a game, GAMES without)."""
    return "press esc\n" + ("press left\n" if side == "left" else "press right\n")


def to_main(word):
    """Script lines: from the paused menu as it opens (SAVES, the arrows in its tiles) to a word of the top row,
    the arrows on the row: up to the row (on SAVES), then left or right. ("game_list": GAMES: the grid is shown
    under the row, the arrows still on the row: enter or down then goes into its items; "saves": the row's SAVES;
    "controls": Settings, into its tiles, to Keys; "rom_manager": on into More, to its tile.)"""
    if word == "game_list":
        word = "games"
    if word == "controls":
        return to_setting("keys")
    if word == "rom_manager":
        return to_setting(word)
    d = MAIN[word] - MAIN["saves"]
    return "press up\n" + ("press right\n" * d if d >= 0 else "press left\n" * -d)


def to_setting(line):
    """Script lines: from the paused menu as it opens to SETTINGS, down into its tiles and to one of them (a tile's
    name or one of the old page names: "sound", "keys", "controller", "more": enter then opens it); or, for a setting
    (WHERE), its tile: on Settings' own, or, for More's, on into More (enter on its tile) and to its tile there.
    (With a game: Link is a tile there.)"""
    into = to_main("settings") + "press down\n"
    tiles = set_tiles()
    if line in TILE_OF or line in SETTINGS:
        return into + settings_moves(0, tiles.index(TILE_OF.get(line, line.replace("_", " ").title())))
    where, name = WHERE[line]
    if where == "settings":
        return into + settings_moves(0, tiles.index(name))
    return (into + settings_moves(0, tiles.index("More")) + "press enter\nwait 0.3\n" +
            page_moves(0, more_tiles().index(name)))


def shown_at(line):
    """What the screen report says (Shot.ui) with that setting's tile picked: "Settings:2", "More:7"."""
    where, name = WHERE[line]
    return "%s:%d" % (where.capitalize(), PAGES[where].index(name))


# Without a game (the grid as gpSP starts, the arrows in its games): esc to the top row (GAMES), left to
# SETTINGS, down into its tiles, to More, enter; there Game isn't shown: Games (the ROM manager)
# is after Keys for.
NO_GAME_ROM_MANAGER = ("press esc\nwait 0.3\npress left\npress down\n" +
                       settings_moves(0, set_tiles(False).index("More")) + "press enter\nwait 0.3\n" +
                       page_moves(0, more_tiles(False).index("Games")) + "press enter\nwait 0.5\n")


def to_advanced(label):
    """Script lines: from the paused menu as it opens to a setting by its old Advanced label (WHERE)."""
    return to_setting(label)


def pick(n):
    """Script lines: in a games' grid just opened (the arrows in its items, the first one picked), the n-th game
    (1 the first) started: the arrows to it in the grid of three, then enter."""
    i = n - 1
    return "press down\n" * (i // 3) + "press right\n" * (i % 3) + "press enter\n"


def cycle(times=1):
    """Script lines: a settings page's picked value changed, for a setting with up to CYCLE_MAX values: enter is
    the next value, each press (r19; a switch flips)."""
    return "press enter\n" * times


def change(key, times=1):
    """Script lines: a settings page's picked range (more than CYCLE_MAX values: Volume, Brightness, CPU speed,
    Autosave) changed: enter puts it between chevrons, left or right change it (stopping at its ends), enter
    keeps it."""
    return "press enter\n" + ("press %s\n" % key) * times + "press enter\n"


# ---- Running gpSP ----

READOUTS = set()   # the speed readout's forms seen ("60", "60  drawn: 45", "--"), with "fps: " checked
# The readout's forms (final/pace, docs/ui/spec.txt): the game's frames a
# second, then "  drawn: N" when fewer pictures than that (60 at most) are
# drawn: a name, a colon and the number, two spaces between (earlier
# builds had ", N drawn"). Earlier still "28/30" (pictures against automatic's
# level) and "3.2x" in fast forward; now the plain number.
READOUT_FORM = re.compile(r"(\d+)(?:  drawn: (\d+))?$")


def readout(value):
    """A note's fps= field: the speed readout ("fps: 60", "fps: 60  drawn:
    45", "fps: --"; '_' for its spaces in the report) without its
    "fps: ", which it must have, spaces put back; "-" with no readout on screen."""
    if value == "-":
        return value
    check(value.startswith("fps:_"), "the speed readout says \"fps: \" first: %r" % value)
    value = (value[5:] if value.startswith("fps:_") else value).replace("_", " ")
    form = READOUT_FORM.match(value)
    check(value == "--" or form is not None, "the speed readout is a number (and pictures drawn): %r" % value)
    READOUTS.add("--" if value == "--" else "drawn" if form and form.group(2) else "n")
    return value


def readout_numbers(value):
    """(game frames a second, pictures drawn a second) a readout says; the
    pictures are the game's (60 at most) when it doesn't say; None for
    "--" or none."""
    form = READOUT_FORM.match(value)
    if not form:
        return None
    game = int(form.group(1))
    return game, int(form.group(2)) if form.group(2) else min(game, 60)


class Run:
    def __init__(self, name, roms=("Pattern",), files=None, reuse=None, in_documents=False):
        """in_documents: gpSP and the games right in My Documents (no gba
        folder)."""
        self.name = name
        self.cx = False
        self.select_writes_ok = False   # R12: the legacy fallback's tests (Run.finish)
        self.in_documents = reuse.in_documents if reuse else in_documents
        self.dir = os.path.join(WORK, name)
        self.gba = self.dir if self.in_documents else os.path.join(self.dir, "gba")
        self.out = os.path.join(self.dir, "out")
        if reuse is None:
            shutil.rmtree(self.dir, ignore_errors=True)
            os.makedirs(self.gba)
            for rom in roms:
                pattern_rom(os.path.join(self.gba, rom + ".gba.tns"))
            # The program file, which keeps the settings for all games.
            with open(os.path.join(self.gba, PROGRAM), "wb") as f:
                f.write(bytes(range(256)) * 16)
        else:
            # The same calculator as an earlier run (program, settings, saves).
            shutil.rmtree(self.dir, ignore_errors=True)
            shutil.copytree(reuse.dir, self.dir, ignore=shutil.ignore_patterns("out"))
        os.makedirs(self.out, exist_ok=True)
        for fname, content in (files or {}).items():
            path = os.path.join(self.gba, fname)
            if isinstance(content, list):
                words_file(path, content)
            else:
                with open(path, "w") as f:
                    f.write(content)

    def run(self, script, rom=None, options=(), env=None):
        return self.finish(self.start(script, rom, options, env))

    def start(self, script, rom=None, options=(), env=None):
        """gpsp-host starts (more options: options; env: more environment
        variables); finish() waits for it."""
        path = os.path.join(self.out, "script.txt")
        with open(path, "w") as f:
            f.write(script)
        options = list(options)
        self.cx = "--model" in options and options[options.index("--model") + 1] == "cx"
        # R12: a CX II whose first timer never counts (--timer-dead) falls back
        # to the legacy time base, which tries R11t's 32 kHz select (put back)
        if "--timer-dead" in options:
            self.select_writes_ok = True
        args = ([HOST] + list(options) + (["--documents", self.dir] if self.in_documents else []) +
                [path, self.out, self.gba] + ([rom] if rom else []))
        return subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                env=dict(os.environ, **env) if env else None)

    def pictures(self, gaps=False, times=False):
        """With GPSP_HOST_PICTURES=1 in env: each picture shown, as (the
        core's frame count, a hash of the whole screen buffer); gaps: and
        how many game frames before it the frame blended with was (0: not
        blended); times: (frame count, ms when it was shown)."""
        rows = [line.split() for line in open(os.path.join(self.out, "pictures.txt"))]
        if times:
            return [(int(f), float(t)) for f, _, _, t in rows]
        if gaps:
            return [(int(f), h, int(g)) for f, h, g, _ in rows]
        return [(int(f), h) for f, h, _, _ in rows]

    def raw_pictures(self):
        """With GPSP_HOST_PICTURES=raw: each shown buffer (320x240 pixels,
        as bytes), in pictures.txt's order."""
        data = open(os.path.join(self.out, "pictures.bin"), "rb").read()
        size = 320 * 240 * 2
        return [data[i:i + size] for i in range(0, len(data), size)]

    def finish(self, proc):
        _, self.stderr = proc.communicate(timeout=HANG)
        self.exit = proc.returncode
        self.report = {}
        self.notes = {}
        report = os.path.join(self.out, "report.txt")
        if not os.path.exists(report):
            # It died before writing it: what it said (a sanitizer's report).
            with open(os.path.join(self.out, "stderr.txt"), "w") as f:
                f.write(self.stderr)
            raise AssertionError("%s: gpsp-host exit %d, no report: %s" % (
                self.name, self.exit, "\n".join(self.stderr.strip().splitlines()[:12])))
        for line in open(report, encoding="utf-8"):
            parts = line.split()
            if parts[0] in ("shot", "note"):
                # "... backlight=120 game=Name with spaces.gba.tns"
                before, game = line.rstrip("\n").split(" game=", 1)
                fields = dict(p.split("=", 1) for p in before.split()[2:])
                fields["game"] = game
                fields["fps"] = readout(fields["fps"])
                for key in ("frame", "shown", "backlight"):
                    fields[key] = int(fields[key])
                self.notes[parts[1]] = fields
            elif parts[0] == "load_error":
                # as it is: split() would take its non-breaking spaces apart
                self.report[parts[0]] = line.rstrip("\n").split(" ", 1)[1]
            else:
                self.report[parts[0]] = " ".join(parts[1:])
        check(self.exit == 0, "%s: gpsp-host exit %d %s" % (self.name, self.exit, self.stderr.strip()))
        # R11w: a run that restarted the calculator (a USB shutdown that did
        # not finish in its time, nspire_exit.c) ends in a reset, which gives
        # the OS its keypad, its timers and everything else back as at power
        # on: the hand-back's and the timers' checks below are about a
        # hand-back, which a restart replaces; they don't apply there (the
        # restart's own checks are pad_tests.py test_r11w_exit_bounded's).
        if self.report.get("restarted") == "1":
            for line in self.stderr.splitlines():
                check(False, "%s: %s" % (self.name, line))
            return self
        # R6: no key pressed in gpSP is left in the OS's queue at exit (the
        # document browser would act on it: it deleted gpSP's program file).
        # (GPSP_HOST_OS_KEYS=poll: an OS that reads its keys whatever its
        # keypad line; no exit can take what it queued: see
        # test_os_keys_after_exit.)
        check(self.report.get("os_keys_left", "0") == "0" or os.environ.get("GPSP_HOST_OS_KEYS") == "poll",
              "%s: the OS's key queue empty at exit: left %s" % (self.name, self.report.get("os_keys_left")))
        # EXIT-1: every way out ends with the hand-back, once: the keypad
        # line on, and nothing lets the OS run after it (on the calculators
        # no key worked after quitting: the main task sat in the OS).
        # EXIT-R3: a report without the fields fails (they were taken as
        # passing).
        check(self.report.get("os_keypad_irq_end") == "on" and self.report.get("hand_backs") == "1" and
              self.report.get("os_after_hand_back") == "0",
              "%s: the hand-back last, once, keypad line on: keypad %s, hand-backs %s, OS ran after it %s times" %
              (self.name, self.report.get("os_keypad_irq_end"), self.report.get("hand_backs"),
               self.report.get("os_after_hand_back")))
        # R11t: the timer block's clock select is one word for two timers (the
        # other a CX II OS's): at the end it is the OS's again, every bit, and
        # no write changed a bit that isn't gpSP's own timer's (host_hw.c says
        # which on stderr, a failure below too).
        check(self.report.get("timer_clock_end") == self.report.get("timer_clock_os") and
              self.report.get("fast_clock_end") == self.report.get("fast_clock_os") and
              self.report.get("clock_foreign_writes") == "0",
              "%s: the clock selects as the OS had them at the end: 900C0080 %s (OS %s), 90010080 %s (OS %s), %s "
              "writes to bits not gpSP's" % (self.name, self.report.get("timer_clock_end"),
                                            self.report.get("timer_clock_os"), self.report.get("fast_clock_end"),
                                            self.report.get("fast_clock_os"), self.report.get("clock_foreign_writes")))
        # R12: a CX II writes nothing to the select (R11t's 2 in its bits 1:0
        # made the OS's USB wait 44 s), but in the legacy fallback a test asks
        # for (select_writes_ok); the first timer at the end as the OS last
        # set it (gpSP's own put back, the OS's never written).
        if not self.cx and not self.select_writes_ok:
            check(self.report.get("timer_clock_writes") == "0", "%s: a CX II wrote the clock select %s times" %
                  (self.name, self.report.get("timer_clock_writes")))
        check(self.report.get("timer1_end") is not None and self.report.get("timer1_end") == self.report.get("timer1_os"),
              "%s: the first timer at the end %s, as the OS last set it %s" % (self.name, self.report.get("timer1_end"),
                                                                              self.report.get("timer1_os")))
        # QUIT6: and its interrupt line quiet at the end: one raised with its
        # enable on is taken by the OS for ever (the CX II froze after the
        # quit of every run past 357.9 s, when gpSP's count passed zero).
        check((self.report.get("timer1_irq_end") or "x x").split()[1] == "0",
              "%s: the first timer's interrupt at the end (raw, masked) %s: line 18 raised for the OS (count passed "
              "zero %s times)" % (self.name, self.report.get("timer1_irq_end"), self.report.get("timer1_zeros")))
        check(int(self.report.get("writes_to_shown", 1)) == 0,
              "%s: no writes to the buffer on the screen (tearing): %s" % (self.name,
                                                                          self.report.get("writes_to_shown")))
        # 2026-11-11: what nspire_present_frame shows of the game's picture is
        # what the picture drew (host_hw.c __wrap_nspire_present_frame): the
        # text bands, drawn only when their text changes, covered the CX II's
        # top picture line then (it flashed black). Fullscreen's boxes aren't
        # counted (they go over the picture by design).
        check(self.report.get("picture_overwritten") == "0",
              "%s: the game's picture drawn over in %s frames, the first %s" %
              (self.name, self.report.get("picture_overwritten"), self.report.get("picture_overwritten_first")))
        # Outside the picture and the bands, every shown buffer is
        # black (a link wait screen stayed in the lines the picture skips).
        check(self.report.get("picture_stale", "0") == "0",
              "%s: a screen drawn before left in %s pictures, the first %s" %
              (self.name, self.report.get("picture_stale"), self.report.get("picture_stale_first")))
        for line in self.stderr.splitlines():
            check(False, "%s: %s" % (self.name, line))
        return self

    def shot(self, name):
        return Shot(os.path.join(self.out, name + ".ppm"), self.notes.get(name, {}).get("ui", ""))

    def file(self, name):
        return os.path.join(self.gba, name)

    def save(self, name):
        """A file in the saves folder (My Documents/zgpSP_files)."""
        return os.path.join(self.dir, "zgpSP_files", name)


# ---- The tests ----

def test_play():
    """List, game, picture size and speed, leaving with Q."""
    r = Run("play").run(REAL_COSTS + """
wait 1
shot list
press enter
wait 3
shot game
note a
wait 1
note b
shot game2
press Q
wait 1
quit
""")
    check(r.shot("list").active_list_row() == 0, "play: the first game is picked")
    box = r.shot("game").box()
    check(box == (0, 13, 319, 226), "play: the 320x214 picture, %s" % (box,))
    a, b = r.notes["a"], r.notes["b"]
    check(57 <= b["frame"] - a["frame"] <= 61, "play: about 60 frames a second, %d" % (b["frame"] - a["frame"]))
    check(r.shot("game").bar_x() != r.shot("game2").bar_x(), "play: the picture moves")
    check(a["game"] == "Pattern.gba.tns", "play: the game")
    check(os.path.exists(r.save("Pattern.gba0.svs.tns")), "play: Q saved a state")
    check(r.report["lcd_base_end"] == "os", "play: the OS's screen is back at the end")
    return r


def test_timer_speed():
    """An older CX's timer counts slower than the 32768 a second it's set
    up for (games ran slow with the FPS counter at 60/60): it's checked
    against the real-time clock and corrected, so after a few seconds games
    run 60 frames a real second and the counter tells the truth. The CX
    II's timer (32768) is left as it is; a fast one is slowed down."""
    script = REAL_COSTS + """wait 1
press enter
wait 8
note a
wait 3
note b
press Q
wait 1
quit
"""
    for rate in (32768, 8192, 16384, 65536):
        r = Run("timer_%d" % rate, files={"gpsp_qol.cfg.tns": owner_config()}).run(
            script, options=["--model", "cx", "--timer-rate", str(rate)])
        a, b = r.notes["a"], r.notes["b"]
        per_second = (b["frame"] - a["frame"]) / (float(b["time"]) - float(a["time"]))
        check(57 <= per_second <= 61, "timer at %d: %.1f game frames a real second" % (rate, per_second))
        fps = re.match(r"\d+", b["fps"])
        check(fps and 57 <= int(fps.group()) <= 61, "timer at %d: the counter, %s" % (rate, b["fps"]))


def test_cx_timer_by_cpu():
    """An older CX also times its timer against its CPU clock at start (the
    CX CAS kept showing 60/60 with the game slow, the real-time clock check
    alone not correcting it): with a real-time clock that doesn't move, the
    timer at 8192 a second ran 15 game frames a real second with the counter
    at 60/60; timed by the CPU clock it's right from the start. A CX II
    never uses it. The CX writes what it found into the clean-exit marker
    (gpsp_link_run.txt.tns; its own gpsp_timer.txt.tns until EXIT-3)."""
    script = REAL_COSTS + """wait 1
press enter
wait 2
note a
wait 3
note b
press Q
wait 1
quit
"""
    for name, cpu_hz, want_ok in (("frozen_rtc", None, False), ("by_cpu", "8192", True)):
        env = dict(os.environ)
        if cpu_hz:
            env["GPSP_HOST_CX_TIMER_HZ"] = cpu_hz
        r = Run("cxtimer_" + name, files={"gpsp_qol.cfg.tns": owner_config()})
        path = os.path.join(r.out, "script.txt")
        with open(path, "w") as f:
            f.write(script)
        proc = subprocess.run([HOST, "--model", "cx", "--timer-rate", "8192", "--rtc-frozen", "1", path, r.out, r.gba],
                              env=env, capture_output=True, text=True, timeout=300)
        check(proc.returncode == 0, "%s: gpsp-host exit %d %s" % (name, proc.returncode, proc.stderr.strip()))
        notes = {}
        for line in open(os.path.join(r.out, "report.txt")):
            parts = line.split()
            if parts[0] == "note":
                fields = dict(p.split("=", 1) for p in line.split(" game=")[0].split()[2:])
                fields["fps"] = readout(fields["fps"])
                notes[parts[1]] = fields
        a, b = notes["a"], notes["b"]
        per_second = (int(b["frame"]) - int(a["frame"])) / (float(b["time"]) - float(a["time"]))
        ok = 57 <= per_second <= 61
        check(ok == want_ok, "%s: %.1f game frames a real second (counter %s)" % (name, per_second, b["fps"]))
        log = r.save("gpsp_link_run.txt.tns")
        text = open(log).read() if os.path.exists(log) else ""
        check(("by the CPU clock: %s" % (cpu_hz or 0)) in text, "%s: the exit marker says it: %r" % (name, text[:600]))


def test_cx_timer_search():
    """An older CX whose first timer doesn't run with any clock select (the
    older CX CAS: no game held to its speed, the link's waits
    all over at once) counts with the next timer its OS leaves alone: the
    fast timer, divided down if it's fast (here 12 MHz: by 256), or, if the
    OS runs that one, the second timer's other half, never writing the OS's
    tick's clock select (the PC build says so on stderr, a failure). Games
    are held to 60 frames a real second, and the exit marker
    (gpsp_link_run.txt.tns) says which timer it was. With none running it
    says so."""
    script = REAL_COSTS + """wait 1
press enter
wait 6
note a
wait 3
note b
press Q
wait 1
quit
"""
    cases = (("first_dead", ["--timer-dead", "2"], "timer used: fast timer at 90010000, divided by 1"),
             ("first_dead_12mhz", ["--timer-dead", "2", "--timer-rate", "12000000"],
              "timer used: fast timer at 90010000, divided by 256"),
             ("fast_is_os", ["--timer-dead", "3"], "timer used: second timer's other half at 900d0020, divided by 1"),
             ("none", ["--timer-dead", "1"], "timer: doesn't run"))
    for name, extra, want in cases:
        r = Run("cxsearch_" + name, files={"gpsp_qol.cfg.tns": owner_config()}).run(
            script, options=["--model", "cx"] + extra)
        a, b = r.notes["a"], r.notes["b"]
        per_second = (b["frame"] - a["frame"]) / (float(b["time"]) - float(a["time"]))
        if name != "none":
            check(57 <= per_second <= 61, "timer search, %s: %.1f game frames a real second" % (name, per_second))
        log = r.save("gpsp_link_run.txt.tns")
        text = open(log).read() if os.path.exists(log) else ""
        check(want in text, "timer search, %s: the exit marker says %r: %r" % (name, want, text[:900]))
        check("timers tried: first timer (select 2): doesn't run" in text,
              "timer search, %s: the first timer tried first: %r" % (name, text[:400]))


def test_timer_clock_shared():
    """R11t: the timer block's clock select (0x900C0080) is one word for its
    two timers, and a CX II's OS runs the other one (0x900C0020, control
    0xE2, its interrupt on). gpSP wrote the whole word (0xA) and so set the
    OS's timer's bits too. Now it writes only its own timer's field (bits
    1:0: 32 kHz is 2), keeps every other bit as the OS had it, puts its own
    field back at exit, and logs the word before and after both writes and
    the OS's timer's count before and after its write. host_hw.c models the
    word (each timer at its own field's clock) and fails every run that
    changes a bit not gpSP's (the old write did: the default OS word here is
    00000004). Each case: the game held to 60 frames a second (gpSP's timer
    at 32 kHz), the word as the OS had it at the end (Run.finish), the lines
    in the link log, the exit marker and the after-the-hand-back file.

    R12 (2026-11-15; this test changed with the rule): on the CX II even
    gpSP's own field was too much (a CX II run log: with 2 in bits
    1:0 the OS's 44-tick wait before a port reset took 44 s), so a CX II
    writes nothing to the select at all: the cases there check that no write
    was made (the report's timer_clock_writes), the marker says so, and the
    game is held to 60 frames a second at whatever clock the OS's word gives
    gpSP's timer (33 MHz, 32 kHz, 33 MHz, 10 MHz here: the time base timed
    by the CPU clock and refined by the real-time clock). The older CX keeps
    R11t's rule (its OS runs no other timer in the block)."""
    script = REAL_COSTS + """wait 1
press enter
wait 3
note a
wait 3
note b
press Q
wait 1
quit
"""
    for model in ("cx2", "cx"):
        for os_word in (0x4, 0xA, 0x0, 0x10D):
            mine = (os_word & ~3) | 2
            name = "clock_%s_%x" % (model, os_word)
            r = Run(name, files={"gpsp_qol.cfg.tns": owner_config()}).run(
                script, options=["--model", model, "--timer-clock-os", "%x" % os_word])
            a, b = r.notes["a"], r.notes["b"]
            per_second = (b["frame"] - a["frame"]) / (float(b["time"]) - float(a["time"]))
            check(57 <= per_second <= 61, "%s: %.1f game frames a real second (gpSP's timer at 32 kHz)" % (name, per_second))
            check(r.report.get("timer_clock_end") == "%08x" % os_word, "%s: the word at the end %s" %
                  (name, r.report.get("timer_clock_end")))
            marker = open(r.save("gpsp_link_run.txt.tns"), encoding="utf-8").read()
            if model == "cx2":
                # R12: nothing written on a CX II (see above)
                check(r.report.get("timer_clock_writes") == "0", "%s: no write to the select: %s" %
                      (name, r.report.get("timer_clock_writes")))
                want = "the OS had %08x; gpSP writes nothing to it on a CX II (R12: time base own)" % os_word
                check("timer clock select 900c0080" in marker and want in marker,
                      "%s: the exit marker says %r: %r" % (name, want, marker[:900]))
                check("timers at start, read before any write: select %08x at 900c0080" % os_word in marker,
                      "%s: the block as found in the marker: %r" % (name, marker[:1500]))
                after = open(r.save("gpsp_exit_after.txt.tns"), encoding="utf-8").read()
                back = "reads %08x; at exit 900c0000 put back as the OS had it: control 00, load 00000000" % os_word
                check(back in after, "%s: the after-the-hand-back file says %r: %r" % (name, back, after))
                continue
            if mine == os_word:
                want = "the OS had %08x; gpSP wrote nothing, keeping %08x, reads %08x" % (os_word, os_word, os_word)
            else:
                want = "the OS had %08x; gpSP wrote %08x, reads %08x" % (os_word, mine, mine)
            check("timer clock select 900c0080" in marker and want in marker,
                  "%s: the exit marker says %r: %r" % (name, want, marker[:700]))
            if model == "cx2" and mine != os_word:
                check(re.search(r"the other timer 900c0020: control e2, load [0-9a-f]+, counted \d+ in 5000 reads "
                                r"before the write, \d+ after \(unchanged\)", marker),
                      "%s: the OS's timer counted before and after, unchanged: %r" % (name, marker[:700]))
            elif model == "cx":
                check("the other timer 900c0020: control 00, not running" in marker,
                      "%s: a CX's OS doesn't run the other timer: %r" % (name, marker[:700]))
            if mine != os_word:
                check("to put back at exit %08x" % os_word in marker, "%s: the marker says what goes back: %r" %
                      (name, marker[:700]))
            after = open(r.save("gpsp_exit_after.txt.tns"), encoding="utf-8").read()
            back = ("at exit it read %08x, gpSP wrote %08x, reads %08x" % (mine, os_word, os_word) if mine != os_word else
                    "at exit it read %08x, gpSP wrote %08x (nothing: unchanged), reads %08x" % (os_word, os_word, os_word))
            check(back in after, "%s: the after-the-hand-back file says %r: %r" % (name, back, after))


def time_base_line(text):
    """The marker's 'time base:' line (R12), or ''."""
    return next((x for x in text.split("\n") if x.startswith("time base: ")), "")


def test_r12_time_base():
    """R12: a CX II's time base at the clock the OS selected, which gpSP no
    longer sets (the select is never written): own (the OS doesn't run
    900C0000: gpSP runs it free, undivided) at 10, 12 and 32.768 MHz, with
    the CPU loop's first timing 0.3% off (refined by the real-time clock);
    borrowed (the OS runs it: followed, nothing written) free running and
    periodic, rewritten by the OS (a jump: counted, no time made of it) and
    stopped by the OS (gpSP takes it then, and puts it back at exit as the
    OS left it); legacy (the OS runs it periodic every 6.5 ms: too short to
    follow) R11t's 32 kHz, the select written. Each: the game at 60 frames a
    real second 10 s in, the marker's mode and why, the speed in use within
    0.1% of the true clock (own, borrowed), the first timer at the end as
    the OS last set it (Run.finish), the select not written but in legacy."""
    script = REAL_COSTS + "wait 1\npress enter\nwait 10\nnote a\nwait 3\nnote b\npress Q\nwait 1\nquit\n"
    os1 = ["--timer-clock-os", "1"]
    cases = (("own_10mhz", os1, 10e6, None, "own (the OS doesn't run 900c0000 (control 00)"),
             ("own_12mhz", os1 + ["--timer-sel1-hz", "12000000"], 12e6, None, "own ("),
             ("own_32mhz", os1 + ["--timer-sel1-hz", "32768000"], 32.768e6, None, "own ("),
             ("own_cpu_off", os1 + ["--timer-sel1-hz", "12000000"], 12e6, "3000", "own ("),
             ("borrowed_free", os1 + ["--timer1-os", "free"], 10e6, None,
              "borrowed (the OS runs 900c0000 (control 82, free running)"),
             ("borrowed_periodic", os1 + ["--timer-sel1-hz", "12000000", "--timer1-os", "periodic:7fffffff"], 12e6, None,
              "borrowed (the OS runs 900c0000 (control c2, periodic)"),
             ("borrowed_jump", os1 + ["--timer1-os", "free", "--timer1-os-jump", "5"], 10e6, None, "borrowed ("),
             ("borrowed_stop", os1 + ["--timer1-os", "free", "--timer1-os-stop", "5"], 10e6, None, "own ("),
             ("legacy", os1 + ["--timer1-os", "periodic:ffff"], 0, None,
              "legacy (the OS runs 900c0000 periodic (control c2), every 6 ms"))
    for name, opts, hz, ppm, mode in cases:
        r = Run("r12_" + name, files={"gpsp_qol.cfg.tns": owner_config()})
        r.select_writes_ok = name == "legacy"
        r.run(script, options=opts, env={"GPSP_HOST_CPU_TIMING_PPM": ppm} if ppm else None)
        a, b = r.notes["a"], r.notes["b"]
        per_second = (b["frame"] - a["frame"]) / (float(b["time"]) - float(a["time"]))
        check(57 <= per_second <= 61, "r12 %s: %.1f game frames a real second" % (name, per_second))
        marker = open(r.save("gpsp_link_run.txt.tns"), encoding="utf-8").read()
        line = time_base_line(marker)
        check(line.startswith("time base: " + mode), "r12 %s: the marker's time base %r: %r" % (name, mode, line))
        check("timers at start, read before any write: select 00000001 at 900c0080; 900c0000: load" in marker and
              "at 900d0080; 900d0000: load 00000148" in marker, "r12 %s: both blocks as found: %r" % (name, marker[:2000]))
        if hz:
            m = re.search(r"; (\d+)\.(\d+) counts/s in use", line)
            used = float(m.group(1) + "." + m.group(2)) if m else 0
            check(abs(used - hz) / hz < 0.001, "r12 %s: the speed in use %.2f, the clock %.0f" % (name, used, hz))
            check(r.report.get("timer_clock_writes") == "0", "r12 %s: the select not written" % name)
        if ppm:
            check(re.search(r"the real-time clock: [1-9]\d* refinement", line), "r12 %s: refined: %r" % (name, line))
        if name.startswith("borrowed") and name != "borrowed_stop":
            check(r.report.get("timer1_writes") == "0", "r12 %s: nothing written to the OS's timer: %s" %
                  (name, r.report.get("timer1_writes")))
        if name == "borrowed_jump":
            check("follower: 1 jump(s)" in line, "r12 jump: counted: %r" % line)
        if name == "borrowed_stop":
            check(re.search(r"the OS stopped 900c0000 [56] s in: gpSP's since", line), "r12 stop: taken: %r" % line)
            check(r.report.get("timer1_end") == "02 ffffffff", "r12 stop: put back as the OS left it: %s" %
                  r.report.get("timer1_end"))
        if name == "legacy":
            check(int(r.report.get("timer_clock_writes", "0")) >= 2 and r.report.get("timer_clock_end") == "00000001",
                  "r12 legacy: R11t's write and its put-back: %s writes, %s at the end" %
                  (r.report.get("timer_clock_writes"), r.report.get("timer_clock_end")))
        print("  R12 %s: %.1f frames a second; %s" % (name, per_second, line[:200]))


def test_quit6_timer_wrap():
    """QUIT6 (the calculator froze on a quit and needed a
    reset). gpSP runs 0x900C0000 free from the top (own time base); an SP804
    sets its raw interrupt when the count reaches zero, whatever the enable,
    and the exit put the OS's control back (20 on every CX II: interrupt
    enabled), raising line 18 for good: every CX II run past 2^32 counts at
    12 MHz (357.9 s) froze after its clean exit. Here the first timer as
    every CX II has it (--timer1-os reset: control 20, value ffffffff) at a
    clock that passes zero in 10.7 s: the put-back clears the interrupt
    (Run.finish: line 18 quiet at the end), the marker says before the
    put-back that the count passed zero and that it is cleared, the file
    after the hand-back what the OS got. A run that never passed zero says
    so; an interrupt the OS had raised (enable off) when gpSP took the timer
    stays as the OS had it."""
    os_reset = ["--timer-clock-os", "1", "--timer1-os", "reset", "--timer-sel1-hz", "400000000"]
    long_script = REAL_COSTS + "wait 1\npress enter\nwait 13\nnote a\npress Q\nwait 1\nquit\n"
    short_script = REAL_COSTS + "wait 1\npress enter\nwait 3\nnote a\npress Q\nwait 1\nquit\n"
    cases = (("wrapped", os_reset, long_script, "0 0", "1"),
             ("short", os_reset, short_script, "0 0", "0"),
             ("os_raised", ["--timer-clock-os", "1", "--timer-sel1-hz", "400000000", "--timer1-os-raised"],
              long_script, "1 0", "1"))
    for name, opts, script, irq_end, raw_before in cases:
        r = Run("quit6_" + name, files={"gpsp_qol.cfg.tns": owner_config()}).run(script, options=opts)
        zeros = int(r.report.get("timer1_zeros", "0"))
        check((zeros >= 1) == (name != "short"), "quit6 %s: the count passed zero %d times" % (name, zeros))
        check(r.report.get("timer1_irq_end") == irq_end, "quit6 %s: the interrupt at the end (raw, masked) %s, want %s" %
              (name, r.report.get("timer1_irq_end"), irq_end))
        marker = open(r.save("gpsp_link_run.txt.tns"), encoding="utf-8").read()
        after = open(r.save("gpsp_exit_after.txt.tns"), encoding="utf-8").read()
        if name == "wrapped":
            check(r.report.get("timer1_end") == "20 00000000", "quit6: put back as the OS had it: %s" %
                  r.report.get("timer1_end"))
            check("900c0000's interrupt now: raw 1 (its count passed zero), the OS had 0 when gpSP took it: the "
                  "put-back clears it before the OS's control goes back" in marker,
                  "quit6: the marker says it before the put-back: %r" % marker[:3000])
        check("interrupt lines before the put-backs: raised and enabled" in marker,
              "quit6 %s: the marker has the interrupt lines: %r" % (name, marker[:3000]))
        why = ("its count never passed zero" if raw_before == "0" else
               "the OS had it raised when gpSP took it" if name == "os_raised" else "its count had passed zero")
        want = ("its interrupt: raw %s before the put-back (%s), raw %s masked 0 after (the OS's line quiet)" %
                (raw_before, why, irq_end.split()[0]))
        check(want in after, "quit6 %s: the file after the hand-back says %r: %r" % (name, want, after))
        check("interrupt lines after the hand-back: raised and enabled 00000000" in after and "(18:" not in after,
              "quit6 %s: no line raised for the OS after the hand-back: %r" % (name, after))
        print("  QUIT6 %s: count passed zero %d times; interrupt at the end %s" % (name, zeros,
                                                                                 r.report.get("timer1_irq_end")))


def test_counter_tells_truth():
    """The FPS counter says how many frames were shown in the last real
    second, whatever the timer does: 60/60 only at full speed. The CX CAS
    showed 60/60 with the game slow: the timer paced the frames and counted
    them too, so a timer running slow or not at all (time then only moves
    by waiting, a frame's time each frame) always counted 60. Now it counts
    against the real-time clock. Checked against the frames the game really
    ran each real second (the test ROM's frame number), on both models,
    with the calculator's costs and an older CX's (slower than 60)."""
    cx_costs = "cost frame 12\ncost line 75\ncost turn 3\ncost touchpad 0.4\n"
    # After the slow timer's correction (a few seconds in), the speed holds.
    script = """wait 1
press enter
wait 10
note a
wait 3
note b
press Q
wait 1
quit
"""
    cases = (("cx2", REAL_COSTS, []), ("cx2_dead", REAL_COSTS, ["--timer-dead", "1"]),
             ("cx", cx_costs, []), ("cx_slow", cx_costs, ["--timer-rate", "8192"]),
             ("cx_dead", cx_costs, ["--timer-dead", "1"]), ("cx_dead_fast", REAL_COSTS, ["--timer-dead", "1"]))
    for name, costs, extra in cases:
        model = "cx2" if name.startswith("cx2") else "cx"
        r = Run("truth_" + name, files={"gpsp_qol.cfg.tns": owner_config()}).run(
            costs + script, options=["--model", model] + extra)
        a, b = r.notes["a"], r.notes["b"]
        real = (b["frame"] - a["frame"]) / (float(b["time"]) - float(a["time"]))
        shown = READOUT_FORM.match(b["fps"])
        ok = shown and abs(int(shown.group(1)) - real) <= 2.5
        check(ok, "%s: the counter says %s, the game ran %.1f frames a real second" % (name, b["fps"], real))


def test_text_goes_away():
    """Messages and the counter over the picture go away when they should,
    on the CX II and on an older CX (landscape LCD), where "Resumed from
    slot" stayed at the bottom for good and the counter's digits piled up:
    the bands outside the picture are drawn black with their text whenever
    it changes, also after the menu."""
    for model in ("cx2", "cx"):
        first = Run("text_%s" % model, files={"gpsp_qol.cfg.tns": owner_config()}).run(
            REAL_COSTS + "wait 1\npress enter\nwait 2\npress Q\nwait 1\nquit\n", options=["--model", model])
        r = Run("text2_%s" % model, reuse=first).run(REAL_COSTS + """wait 1
press enter
wait 1
shot message
wait 3
shot gone
press menu
wait 0.5
press esc
wait 3
shot after_menu
quit
""", options=["--model", model])
        msg = r.shot("message").box(228, 240)
        check(msg is not None, "%s: the message shows at first" % model)
        for name in ("gone", "after_menu"):
            shot = r.shot(name)
            check(shot.box(227, 240) is None, "%s: nothing left at the bottom (%s): %s" % (model, name, shot.box(227, 240)))
            top = shot.box(0, 12)
            check(top is not None and top[2] < 60,
                  "%s: only the counter at the top (%s): %s" % (model, name, top))


def test_bands_beside_picture():
    """The FPS counter and the messages (the text bands, lines 0-12 and
    227-239, drawn only when their text changes) never go over the game's
    picture, and the picture never over them, at each picture size on both
    LCDs. 2026-11-11: the CX II's 3:2 picture began at line 12, the top
    band's last line, so its top row flashed black in each frame that drew
    the band (the counter changing; 3 frames after the menu), and the black
    under it and the sharp 3:2 picture's black lines erased the bands' edge
    lines (12, 227) in the frames after. Run.finish requires the report's
    picture_overwritten 0 in every test (host_hw.c: each shown picture
    against what its picture function drew); here with the counter
    changing, a message and the menu. Fullscreen's boxes go over the
    picture by design (not checked)."""
    def three_shots(name):
        return "\n".join("shot %s_%d\nwait 0.02" % (name, i) for i in range(3)) + "\n"

    for model in ("cx2", "cx"):
        for scale, size in ((0, "unscaled"), (1, "3:2"), (3, "3:2 sharp")):
            cfg = owner_config()
            cfg[0] = scale
            r = Run("bands_%s_%d" % (model, scale), files={"gpsp_qol.cfg.tns": cfg}).run(REAL_COSTS + """wait 1
press enter
wait 2
press S
wait 0.6
""" + three_shots("message") + """wait 2.5
press menu
wait 0.3
press esc
wait 1
cost frame 15
wait 2
cost frame 7
wait 1
press Q
wait 1
quit
""", options=["--model", model])
            what = "%s, %s" % (model, size)
            check(int(r.report.get("picture_frames_checked", 0)) > 200,
                  "%s: pictures checked: %s" % (what, r.report.get("picture_frames_checked")))
            for i in range(3):
                shot = r.shot("message_%d" % i)
                # (r16: the message is its words on the band, no chip: its capitals' rows, 229-237)
                check(any(shot.at(x, y) != BLACK for x in range(320) for y in range(229, 238)),
                      "%s: the message's words (rows 229-237) in each frame, frame %d" % (what, i))


# Pokemon Emerald, for test_menu_in_fade: the game and a state of the
# the reference (CX II slot 24: Mauville, in front of its Pokemon Center), read
# where they are (the ROM linked, never copied).
GBA_GAMES = os.environ.get("GBA_GAMES", "")
EMERALD = "Pokemon - Emerald Version (USA, Europe).gba"


def emerald_states():
    return os.environ.get("EMERALD_STATES", "")


def emerald_run(name, model, words, script, pictures="1"):
    """gpsp-host on the model with the settings words (gpsp.cfg.tns), the
    Emerald state as slot 1 (resumed at the start), the pictures listed."""
    r = Run(name, roms=())
    saves = os.path.join(r.dir, "zgpSP_files")
    os.makedirs(saves)
    os.symlink(os.path.join(GBA_GAMES, EMERALD), r.file(EMERALD + ".tns"))
    os.symlink(os.path.join(GBA_GAMES, "gba_bios.bin"), os.path.join(saves, "gba_bios.bin.tns"))
    shutil.copy(os.path.join(emerald_states(), EMERALD + "24.svs.tns"), os.path.join(saves, EMERALD + "0.svs.tns"))
    words_file(os.path.join(saves, "gpsp.cfg.tns"), words)
    costs = "cost frame 4\ncost line 25\n" if model == "cx2" else "cost frame 12\ncost line 75\n"
    return r.run(costs + script, rom=EMERALD + ".tns", options=["--model", model],
                 env={"GPSP_HOST_PICTURES": pictures, "GPSP_HOST_CLOCK": "1794450000"})


def test_menu_in_fade():
    """The menu opened and left while the game fades (walking into
    Mauville's Pokemon Center: the door, the fade out, black, the fade in)
    changes no picture: every picture shown, from the first after the menu
    (and before it), is the one the same run without the menu showed for
    that game frame (2026-11-11, the CX CAS: "after the menu, while going
    into a Pokemon Center, bits of the screen flash in another place").
    Both LCDs' pictures (the CX II's turned 3:2, the CX's hand-written one:
    its C twin here), frameskip off, every other frame, automatic (the
    CX's own), with and without interframe blending, the menu at three
    points of the fade. The FPS counter is off (its text depends on time).
    Needs Emerald, its BIOS and the reference states (skipped without)."""
    states = emerald_states()
    needs = [os.path.join(GBA_GAMES, EMERALD), os.path.join(GBA_GAMES, "gba_bios.bin"),
             os.path.join(states, EMERALD + "24.svs.tns")]
    missing = [n for n in needs if not os.path.exists(n)]
    if missing:
        print("  skipped: no %s" % ", ".join(missing))
        return
    walk = "wait 1\nframes 200\nhold 8\nframes 70\nrelease 8\n"
    # after the release: the fade out about 5-19 frames on, black, the fade in about 83-100
    points = (8, 50, 88)
    for model in ("cx2", "cx"):
        for fs_type, fs_value, fs_name in ((2, 0, "frameskip off"), (1, 1, "every other frame"),
                                           (0, 1, "automatic frameskip")):
            for mix in (0, 1):
                words = owner_config()
                words[24], words[25], words[30] = fs_type, fs_value, mix
                words[26] |= 1   # PACE_RULE_MARK: keep the test's fs_type (Off stays Off)
                words[33] = words[62] = 0   # no FPS counter
                what = "%s, %s, blending %s" % (model, fs_name, "on" if mix else "off")
                tag = "fade_%s_%d%d_%d" % (model, fs_type, fs_value, mix)
                plain = emerald_run(tag, model, words, walk + "frames 300\nquit\n").pictures()
                first = plain[0][0]
                by_frame = dict(plain)
                before = {plain[i][0]: plain[i - 1][0] for i in range(1, len(plain))}
                for at in points:
                    r = emerald_run("%s_menu%d" % (tag, at), model, words, walk + "frames %d\nshot frozen\n"
                                    "press menu\nwait 0.4\nshot menu\npress esc\nwait 0.5\nframes 200\nquit\n"
                                    % at)
                    check(r.notes["menu"]["ui"].startswith("saves"), "%s, menu at %d: the menu was open: %s" %
                          (what, at, r.notes["menu"]["ui"]))
                    pics = r.pictures()
                    menu_frame = first + 270 + at   # about: the menu came after this many frames
                    after = compared = 0
                    wrong = []
                    for i, (frame, h) in enumerate(pics):
                        if frame not in by_frame or (i and before.get(frame) != pics[i - 1][0]):
                            continue   # not drawn in both, or after different frames (blending)
                        compared += 1
                        after += frame > menu_frame
                        if by_frame[frame] != h:
                            wrong.append(frame - first)
                    check(not wrong, "%s, menu at %d: pictures differ from the run without the menu at frames %s"
                          % (what, at, wrong[:10]))
                    # every picture both runs drew after the menu (frames drawn in
                    # turn: the same ones in both)
                    expected = sum(1 for f, _ in plain if menu_frame < f <= pics[-1][0])
                    need = 20 if (model == "cx" and fs_type == 0) else max(60, expected - 2)
                    check(after >= need, "%s, menu at %d: %d pictures compared after the menu, "
                          "of %d both drew (%d in all)" % (what, at, after, expected, compared))



def blend_line(text, when):
    """The picture log line's blending counts (log_picture), as (blended,
    with a frame 2 or more before, largest gap)."""
    m = re.search(r"picture %s: .*?blending on: (\d+) pictures blended, (\d+) with a frame 2 or more before, "
                  r"largest gap (\d+)" % re.escape(when), text)
    return tuple(int(v) for v in m.groups()) if m else None


def test_picture_lines():
    """The picture log lines (log_picture). The
    LCD's "DMA ran dry" bit was cleared only by a line, so the first said
    "since boot" (here: set before gpSP, GPSP_HOST_LCD_UNDERFLOW): now a
    game's start clears it. A line 1 s after the start and 1 s after each
    menu (not timed: no busy wait) tells the menu's exit from the play after
    it. The first line's busy wait (two LCD refreshes) is not the first
    frame's time: the first pictures come a refresh apart (before: 11 ms,
    then 14 ms, catching up)."""
    for model in ("cx", "cx2"):
        cfg = owner_config()
        r = Run("picture_lines_%s" % model, files={"gpsp_qol.cfg.tns": cfg}).run(
            REAL_COSTS + "wait 1\npress enter\nwait 2\npress menu\nwait 0.3\npress esc\nwait 2\npress Q\nwait 1\nquit\n",
            options=["--model", model], env={"GPSP_HOST_PICTURES": "1", "GPSP_HOST_LCD_UNDERFLOW": "1"})
        marker = open(r.save("gpsp_link_run.txt.tns"), encoding="utf-8").read()
        lines = dict((m.group(1), m.group(2)) for m in re.finditer(r"^picture (.*?): (.*)$", marker, re.M))
        want = ["at the game's first frame", "1 s after the game's start", "after the menu (1)",
                "1 s after the menu (1)", "at the quit"]
        check(list(lines) == want, "%s: the marker's picture lines: %s" % (model, list(lines)))
        first = lines.get("at the game's first frame", "")
        check("DMA ran dry: no" in first, "%s: the first line counts from the game's start: %s" % (model, first))
        for when in want:
            timed = not when.startswith("1 s after")
            text = lines.get(when, "")
            check(re.match(r"LCD refresh \d+\.\d ms" if timed else "LCD refresh not timed", text),
                  "%s: %s: %s" % (model, when, text[:40]))
        times = [t for _, t in r.pictures(times=True)[:8]]
        gaps = [round(b - a, 1) for a, b in zip(times, times[1:])]
        check(len(gaps) == 7 and all(15.5 <= g <= 18 for g in gaps),
              "%s: the first pictures a refresh apart: %s ms" % (model, gaps))


def test_blend_gaps():
    """Interframe blending mixes a picture with the
    last one drawn, and with frameskip that is 2-3 game frames earlier (the
    core draws nothing in a skipped frame); the CX's own settings
    (automatic frameskip, about 23 of 60 drawn on the calculator, and
    blending) blend every picture so. test_menu_in_fade compares runs with
    and without the menu and can't see what both show: here, over the walk
    into Mauville's Pokemon Center, each blended picture's partner is the
    picture shown before it (pictures.txt's gap), frameskip off blends only
    neighbours, 1 in 3 and automatic blend frames 2-3 apart, and against
    the same run without blending those pictures are double images over
    much of the screen (the walk's scroll, the door, the fades): something
    gpSP draws, the same with and without the menu. The log line after the
    menu and at the quit counts the same as pictures.txt. Needs Emerald."""
    states = emerald_states()
    needs = [os.path.join(GBA_GAMES, EMERALD), os.path.join(GBA_GAMES, "gba_bios.bin"),
             os.path.join(states, EMERALD + "24.svs.tns")]
    missing = [n for n in needs if not os.path.exists(n)]
    if missing:
        print("  skipped: no %s" % ", ".join(missing))
        return
    walk = "wait 1\nframes 200\nhold 8\nframes 70\nrelease 8\n"
    ghost = {}
    script = walk + "frames 100\npress menu\nwait 0.4\npress esc\nwait 0.5\nframes 100\npress Q\nwait 1\nquit\n"
    for model, fs_type, fs_value, fs_name in (("cx", 2, 0, "frameskip off"), ("cx", 1, 2, "1 in 3"),
                                              ("cx", 0, 1, "automatic frameskip"), ("cx2", 1, 2, "1 in 3")):
        runs = {}
        for mix in (1, 0):
            words = owner_config()
            words[24], words[25], words[30] = fs_type, fs_value, mix
            words[26] |= 1   # PACE_RULE_MARK: keep the test's fs_type (Off stays Off)
            words[33] = words[62] = 0   # no FPS counter
            runs[mix] = emerald_run("gaps_%s_%d%d_%d" % (model, fs_type, fs_value, mix), model, words, script,
                                    pictures="raw")
        what = "%s, %s" % (model, fs_name)
        r = runs[1]
        pics = r.pictures(gaps=True)
        gaps = [g for _, _, g in pics]
        # the partner is the picture shown before (the menu changes nothing: frames don't advance in it)
        partner = [i for i in range(1, len(pics)) if pics[i][2] != pics[i][0] - pics[i - 1][0]]
        check(len(pics) > 100 and pics[0][2] == 0 and not partner,
              "%s: each picture blended with the one shown before it: %d pictures, first gap %d, others at %s"
              % (what, len(pics), pics[0][2], partner[:5]))
        far = sum(1 for g in gaps if g > 1)
        if fs_type == 2:
            check(max(gaps) == 1, "%s: blends only neighbouring frames: largest gap %d" % (what, max(gaps)))
        else:
            check(far >= len(pics) * 9 // 10 and max(gaps) <= 3,
                  "%s: blends frames 2-3 apart: %d of %d pictures, largest gap %d" % (what, far, len(pics),
                                                                                       max(gaps)))
        marker = open(r.save("gpsp_link_run.txt.tns"), encoding="utf-8").read()
        menu = blend_line(marker, "after the menu (1)")
        whole = (sum(1 for g in gaps if g), far, max(gaps))
        check(menu is not None and menu[0] <= whole[0] and menu[1] <= whole[1] and menu[2] <= whole[2]
              and menu[0] >= 100, "%s: the log line after the menu counts the pictures before it: %s of %s"
              % (what, menu, whole))
        quit_line = blend_line(marker, "at the quit")
        check(quit_line is not None, "%s: a line at the quit" % what)
        # (and a line 1 s after the start and after the menu)
        parts = [blend_line(marker, w) for w in ("at the game's first frame", "1 s after the game's start",
                                                 "after the menu (1)", "1 s after the menu (1)", "at the quit")]
        check(None not in parts and sum(p[0] for p in parts) == whole[0] and sum(p[1] for p in parts) == whole[1]
              and max(p[2] for p in parts) == whole[2],
              "%s: the log lines count every blended picture: %s, pictures.txt %s" % (what, parts, whole))
        # against the same frames without blending: how far each picture is
        # from the game's own (the green of every pixel, summed: the double
        # image's weight), over the whole run
        plain = runs[0].pictures(gaps=True)
        plain_raw = dict((f, b) for (f, _, _), b in zip(plain, runs[0].raw_pictures()))
        line = 320 * 2
        doubled = compared = 0
        weight = 0
        for (f, _, g), b in zip(pics, r.raw_pictures()):
            if not g or f not in plain_raw:
                continue
            compared += 1
            x = plain_raw[f]
            lines = 0
            for y in range(240):
                bl, xl = b[y * line:(y + 1) * line], x[y * line:(y + 1) * line]
                if bl == xl:
                    continue
                lines += 1
                bw, xw = struct.unpack("<320H", bl), struct.unpack("<320H", xl)
                weight += sum(abs((p >> 5 & 63) - (q >> 5 & 63)) for p, q in zip(bw, xw))
            doubled += lines >= 100
        ghost[(model, fs_type)] = weight / max(compared, 1)
        print("  %s: %d of %d blended pictures differ from the unblended one in 100 or more lines; "
              "%.0f green steps a picture" % (what, doubled, compared, ghost[(model, fs_type)]))
        check(compared >= 100 and (doubled >= 10 or fs_type == 2),
              "%s: the walk and the door blended show double images: %d of %d pictures" % (what, doubled, compared))
    # frames 2-3 apart: a heavier double image than neighbours'
    check(ghost[("cx", 1)] > 1.4 * ghost[("cx", 2)] and ghost[("cx", 0)] > 1.4 * ghost[("cx", 2)],
          "blending frames 2-3 apart doubles more than neighbours: %s" % sorted(ghost.items()))


def test_menu():
    """The menu opens, moves with short presses and with arrows held, and
    closes, with the calculator's costs."""
    r = Run("menu").run(REAL_COSTS + """
wait 1
press enter
wait 2
press menu
wait 0.5
shot m0
press up
wait 0.2
shot m1
tap right
wait 0.2
shot m2
press left
wait 0.2
shot m3
hold left
wait 0.7
release left
wait 0.2
shot held
press esc
wait 0.5
note back
wait 1
note later
shot game
quit
""")
    check(r.shot("m0").ui == "saves:0", "menu: it opens on SAVES, RESUME picked: %s" % r.shot("m0").ui)
    lines = [r.shot(n).active_menu_line("main") for n in ("m1", "m2", "m3")]
    check(lines == [MAIN["saves"], MAIN["games"], MAIN["saves"]],
          "menu: up to the top row, right to GAMES, left back to SAVES: %s" % (lines,))
    # From SAVES (word 1), one step at once and then 15 a second after 0.4 s: 6 steps at the shot, left round
    # the top row's four words (the row wraps).
    # (a held arrow steps at once and repeats on the paused menu's top row: fixed in 4644804)
    held = r.shot("held").active_menu_line("main")
    check(held == (MAIN["saves"] - 6) % len(MAIN), "menu: a held arrow repeats: word %s" % (held,))
    check(r.notes["later"]["frame"] - r.notes["back"]["frame"] >= 55, "menu: the game carries on after it")
    check(r.shot("game").box() == (0, 13, 319, 226), "menu: the picture is back whole")


def test_menu_slow_game():
    """A game too slow for full speed: the menu still answers."""
    r = Run("menu_slow").run(REAL_COSTS + """
cost frame 30
wait 1
press enter
wait 2
press menu
wait 0.5
tap up
wait 0.3
shot m1
press esc
wait 1
note a
wait 1
note b
quit
""")
    check(r.shot("m1").active_menu_line() == MAIN["saves"], "slow game: the menu moved")
    fps = r.notes["b"]["frame"] - r.notes["a"]["frame"]
    check(28 <= fps <= 34, "slow game: runs as fast as it can, %d frames a second" % fps)


def test_old_settings():
    """Settings from older builds and from the first QoL build: the picture
    is the full 320x214 one even if they said unscaled."""
    for name, files in (("old_cfg", {"gpsp.cfg.tns": old_config(0)}),
                        ("qol1_cfg", {"gpsp_qol.cfg.tns": qol1_config(0, 1)})):
        r = Run(name, files=files).run("""
wait 1
press enter
wait 3
shot game
press Q
wait 1
quit
""")
        box = r.shot("game").box()
        check(box == (0, 13, 319, 226), "%s: the 320x214 picture, %s" % (name, box))
        words = read_words(r.save("gpsp.cfg.tns"))
        check(len(words) == 82 and words[37] == 0x334C4F51, "%s: saved as QOL3" % name)
        check(words[0] == 3, "%s: scaling saved as scaled/unblended: %d" % (name, words[0]))
        if name == "qol1_cfg":
            check(words[61] == 1003, "qol1_cfg: 432 MHz kept: speed %d (1000 + 3: chosen)" % words[61])
        if name == "old_cfg":
            check(not os.path.exists(r.file("gpsp.cfg.tns")),
                  "old_cfg: the old build's file moved into zgpSP_files (and the new copy written over it)")


def test_legacy_renderer_setting():
    """A settings file from a build that had the Legacy video renderer, with
    it selected (word 36 = 1): it loads, the game runs with every other
    setting as in the same file with video.cc selected, and the file written
    back has 0 in that word (reserved)."""
    saved, boxes = {}, {}
    for name, renderer in (("legacy_cfg", 1), ("legacy_ctl", 0)):
        o = owner_config()
        o[26] |= 1
        o[36] = renderer
        r = Run(name, files={"gpsp_qol.cfg.tns": o}).run("""
wait 1
press enter
wait 3
shot game
press Q
wait 1
quit
""")
        boxes[name] = r.shot("game").box()
        saved[name] = read_words(r.save("gpsp.cfg.tns"))
    check(boxes["legacy_cfg"] == boxes["legacy_ctl"] and boxes["legacy_cfg"][2] > 0,
          "legacy_cfg: the game's picture as with video.cc selected: %s %s" % (boxes["legacy_cfg"], boxes["legacy_ctl"]))
    words = saved["legacy_cfg"]
    check(len(words) == 82 and words[36] == 0, "legacy_cfg: word 36 written as 0 (%s)" % words[36:37])
    check(words == saved["legacy_ctl"], "legacy_cfg: every setting as with video.cc selected: %s"
          % [i for i in range(len(words)) if words[i] != saved["legacy_ctl"][i]])
    check(words[28] == 1 and words[31] == 16 and words[33] == 1 and words[24] == 2,
          "legacy_cfg: settings around word 36 kept: %s" % words[24:36])


def test_unscaled_chosen():
    """Unscaled chosen in this build: the GBA's own 240x160."""
    first = Run("unscaled").run("""
wait 1
press enter
wait 2
press menu
wait 0.3
""" + to_setting("size") + cycle(size_cycles("Original")) + """wait 0.3
shot option
press esc
press esc
wait 2.5
shot game
press Q
wait 1
quit
""")
    box = first.shot("game").box()
    check(box == (40, 40, 279, 199), "unscaled: 240x160 in the middle, %s" % (box,))
    again = Run("unscaled2", reuse=first).run("""
wait 1
press enter
wait 3
shot game
quit
""")
    check(again.shot("game").box() == (40, 40, 279, 199), "unscaled: kept for the next start")


def test_settings_select_first():
    """On Settings' tiles the arrows move among them: right the next one, left back, down the row under it, up back;
    there are no tabs to turn. enter on a setting of a few values is its next value at once (no edit state; four enters
    go round Screen size's three values); on a range (Brightness) it starts changing the value (then left/right change
    it, enter keeps it, esc puts it back). (r16: a list, up and down moved, left and right changed the value at once.)"""
    r = Run("select_first").run("""
wait 1
press enter
wait 2
press menu
wait 0.3
""" + to_setting("size") + """press right
wait 0.2
shot right
press left
wait 0.2
shot left
press down
wait 0.2
shot down
press up
wait 0.2
shot up
press tab
wait 0.2
shot tab
press enter
wait 0.2
shot cycled
press enter
press enter
wait 0.2
shot cycled_round
press left
wait 0.2
shot brightness
press enter
press left
wait 0.2
shot editing
press right
wait 0.2
shot still_editing
press left
press esc
wait 0.2
shot cancelled
press esc
press esc
wait 2.5
shot game
quit
""")
    at = shown_at("size")
    check(r.shot("right").ui == "Settings:3", "select first: right moves to the next tile: %s" % r.shot("right").ui)
    check(r.shot("left").ui == at, "select first: left moves back: %s" % r.shot("left").ui)
    check(r.shot("down").ui == "Settings:5", "select first: down moves to the tile under it (the third column's second): %s"
          % r.shot("down").ui)
    check(r.shot("up").ui == at and r.shot("up").pixels == r.shot("left").pixels,
          "select first: up moves back, nothing changed: %s" % r.shot("up").ui)
    check(r.shot("tab").ui == at, "select first: tab does nothing: %s" % r.shot("tab").ui)
    check(r.shot("cycled").ui == at and r.shot("cycled").pixels != r.shot("up").pixels,
          "select first: one enter is the next value: %s" % r.shot("cycled").ui)
    check(r.shot("cycled_round").ui == at and r.shot("cycled_round").pixels == r.shot("up").pixels,
          "select first: three enters round Screen size's three values")
    check(r.shot("brightness").ui == shown_at("brightness"), "select first: on Brightness: %s" % r.shot("brightness").ui)
    check(r.shot("editing").ui == shown_at("brightness") and r.shot("editing").pixels != r.shot("brightness").pixels,
          "select first: enter picks a range, left changes it: %s" % r.shot("editing").ui)
    check(r.shot("still_editing").ui == shown_at("brightness"), "select first: right changes it too, the tile stays: %s"
          % r.shot("still_editing").ui)
    check(r.shot("cancelled").ui == shown_at("brightness") and r.shot("cancelled").pixels == r.shot("brightness").pixels,
          "select first: esc puts it back and stays on the tile: %s" % r.shot("cancelled").ui)
    check(r.shot("game").box() == (0, 14, 319, 226), "select first: three enters round: Fit again, now the smoothed one: %s"
          % (r.shot("game").box(),))


def test_brightness():
    """+ and -, the menu line, and the calculator's own brightness back."""
    r = Run("brightness").run("""
backlight 120
wait 1
note list
press enter
wait 2
note start
press +
wait 0.3
note up1
press +
press +
press +
press +
press +
press +
wait 0.3
note top
press -
wait 0.3
note down1
press menu
wait 0.3
""" + to_setting("brightness") + change("left", 2) + """wait 0.3
note menu_dimmer
press esc
press esc
wait 1
press Q
wait 1
quit
""")
    n = r.notes
    check(n["list"]["backlight"] == 120 and n["start"]["backlight"] == 120, "brightness: untouched at first")
    check(n["up1"]["backlight"] == 100, "brightness: + one level up from the nearest (120 -> 100): %d"
          % n["up1"]["backlight"])
    check(n["top"]["backlight"] == 0, "brightness: + stops at the brightest: %d" % n["top"]["backlight"])
    check(n["down1"]["backlight"] == 25, "brightness: - one level: %d" % n["down1"]["backlight"])
    check(n["menu_dimmer"]["backlight"] == 75, "brightness: the menu line, two down: %d"
          % n["menu_dimmer"]["backlight"])
    check(int(r.report["backlight_end"]) == 120, "brightness: the calculator's own back when gpSP closes")
    words = read_words(r.save("gpsp.cfg.tns"))
    check(words[63] == 7, "brightness: level 7 saved: %d" % words[63])

    again = Run("brightness2", reuse=r).run("""
backlight 160
wait 1
note list
press enter
wait 1
press Q
wait 1
quit
""")
    check(again.notes["list"]["backlight"] == 75, "brightness: level 7 from the start next time: %d"
          % again.notes["list"]["backlight"])
    check(int(again.report["backlight_end"]) == 160, "brightness: and the calculator's own back at the end")

    keep = Run("brightness3", files={"gpsp_qol.cfg.tns": qol1_config(3, 0)}).run("""
backlight 200
wait 1
press enter
wait 1
press menu
wait 0.3
press esc
wait 0.5
press Q
wait 1
quit
""")
    check(int(keep.report["backlight_writes"]) == 0, "brightness: never touched unless asked")

    # An older CX: its backlight is the LCD contrast (0x900F0020), higher
    # brighter, 0x11a to 0x1ce over the ten levels; the OS's normal 0x174 is
    # nearest level 6.
    cx = Run("brightness_cx").run("""
wait 1
note list
press enter
wait 2
note start
press +
wait 0.3
note up1
press +
press +
press +
press +
press +
press +
wait 0.3
note top
press -
wait 0.3
note down1
press menu
wait 0.3
""" + to_setting("brightness") + change("left", 2) + """wait 0.3
note menu_dimmer
press esc
press esc
wait 1
press Q
wait 1
quit
""", options=["--model", "cx"])
    n = cx.notes
    check(n["start"]["backlight"] == 0x174, "CX brightness: untouched at first: %x" % n["start"]["backlight"])
    check(n["up1"]["backlight"] == 0x11A + 6 * 20, "CX brightness: + one level up from the nearest (6 -> 7): %x"
          % n["up1"]["backlight"])
    check(n["top"]["backlight"] == 0x1CE, "CX brightness: + stops at the brightest: %x" % n["top"]["backlight"])
    check(n["down1"]["backlight"] == 0x11A + 8 * 20, "CX brightness: - one level: %x" % n["down1"]["backlight"])
    check(n["menu_dimmer"]["backlight"] == 0x11A + 6 * 20, "CX brightness: the menu line, two down: %x"
          % n["menu_dimmer"]["backlight"])
    check(int(cx.report["backlight_end"]) == 0x174, "CX brightness: the calculator's own back when gpSP closes")


def test_states():
    """Save states: new slots, loading, and resuming on start."""
    r = Run("states").run(REAL_COSTS + """
wait 1
press enter
wait 2
press S
wait 1
note saved0
press S
wait 2.5
note saved1
press L
wait 0.1
note loaded
press Q
wait 1
quit
""")
    check(os.path.exists(r.save("Pattern.gba0.svs.tns")) and os.path.exists(r.save("Pattern.gba1.svs.tns")),
          "states: S saved slots 0 and 1")
    check(os.path.exists(r.save("Pattern.gba2.svs.tns")), "states: Q saved slot 2")
    check(r.notes["loaded"]["frame"] < r.notes["saved1"]["frame"], "states: L went back to slot 1")
    size = os.path.getsize(r.save("Pattern.gba0.svs.tns"))
    check(size < 20000, "states: compressed (%d bytes)" % size)

    again = Run("states2", reuse=r).run("""
wait 1
press enter
wait 0.2
note resumed
quit
""")
    check(again.notes["resumed"]["frame"] > 100, "states: the next start resumes from the newest: frame %d"
          % again.notes["resumed"]["frame"])

    fresh = Run("states3", reuse=r).run("""
wait 1
hold shift
press enter
release shift
wait 0.2
note fresh
quit
""")
    check(fresh.notes["fresh"]["frame"] < 30, "states: shift+enter starts without the state: frame %d"
          % fresh.notes["fresh"]["frame"])


def gpsz_to_raw(path):
    """A state as older gpSP builds wrote it: the raw 416 KB, no header."""
    import zlib
    data = open(path, "rb").read()
    assert data[:4] == b"GPSZ"
    with open(path, "wb") as f:
        f.write(zlib.decompress(data[32:]))


def gpsz_unflag(path):
    """A state as the first QoL build wrote it: no flags in its header."""
    data = bytearray(open(path, "rb").read())
    data[20:24] = b"\0\0\0\0"
    open(path, "wb").write(data)


def test_file_types():
    """ROMs opened from the documents start gpSP by its name (_gpSP): its
    earlier name's file types (gpsp_qol, as on a calculator) are
    pointed at it, another program's are left alone, and the file isn't
    rewritten when nothing changes."""
    cfg_lines = ["ext.sfc=_PocketSNES", "ext.zip=_PocketSNES", "ext.gba=gpsp_qol",
                 "ext.gbz=gpsp_qol", "ext.gbc=gbc4nspire", ""]
    r = Run("file_types")
    ndless = os.path.join(r.dir, "ndless")
    os.makedirs(ndless)
    cfg = os.path.join(ndless, "ndless.cfg.tns")
    with open(cfg, "w") as f:
        f.write("\n".join(cfg_lines))
    r.run("wait 1\nquit\n")
    expected = [l.replace("gpsp_qol", "_gpSP") for l in cfg_lines]
    check(open(cfg).read().split("\n") == expected, "file types: gpsp_qol's now _gpSP's: %r" % open(cfg).read())
    check(r.report["registered"] == "gba=_gpSP,gbz=_gpSP", "file types: registered %s" % r.report["registered"])

    other = Run("file_types_other")
    os.makedirs(os.path.join(other.dir, "ndless"))
    cfg = os.path.join(other.dir, "ndless", "ndless.cfg.tns")
    text = "ext.gba=gpspnspire_gpsp_launcher\r\next.gbz=_gpSP\r\n"
    with open(cfg, "w", newline="") as f:
        f.write(text)
    os.utime(cfg, (1000000000, 1000000000))
    other.run("wait 1\nquit\n")
    check(open(cfg, newline="").read() == text, "file types: another program's kept")
    check(os.stat(cfg).st_mtime == 1000000000, "file types: not rewritten when nothing changes")

    none = Run("file_types_none").run("wait 1\nquit\n")
    check(not os.path.exists(os.path.join(none.dir, "ndless")), "file types: no ndless folder made")
    check(none.report["registered"] == "gba=_gpSP,gbz=_gpSP", "file types: registered without a file")



def test_new_calculator():
    """On a calculator that never had gpSP: the first start makes My
    Documents/zgpSP_files, everything gpSP writes goes there or into the
    program file, and the game folder keeps only the program and games."""
    r = Run("new_calculator", roms=("Pattern", "Second"))
    program = r.file(PROGRAM)
    bare = os.path.getsize(program)
    r.run(REAL_COSTS + """wait 1
press right
press tab
wait 0.2
press left
press enter
wait 0.3
press enter
wait 1.5
press S
wait 0.5
press +
wait 0.3
press Q
wait 1
""")
    check(sorted(os.listdir(r.gba)) == sorted([PROGRAM, "Pattern.gba.tns", "Second.gba.tns"]),
          "new calculator: only the program and games in the game folder: %s" % sorted(os.listdir(r.gba)))
    check(sorted(os.listdir(r.dir)) == ["gba", "out", "zgpSP_files"],
          "new calculator: My Documents has the saves folder: %s" % sorted(os.listdir(r.dir)))
    saves = sorted(os.listdir(r.save("")))
    for name in ("gpsp.cfg.tns", "gpsp_romdir.tns", "gpsp_order_gba.txt.tns", "Second.gba0.svs.tns"):
        check(name in saves, "new calculator: %s in zgpSP_files: %s" % (name, saves))
    check("gpsp_qol.cfg.tns" not in saves, "new calculator: no gpsp_qol.cfg.tns: %s" % saves)
    # Made at start: the OS's list is read again at exit (the new folder).
    check(r.report["osscr_refreshes"] == "1", "new calculator: the file list read again at exit: %s"
          % r.report["osscr_refreshes"])
    check(os.path.getsize(program) == bare + 4096 + 16, "new calculator: the settings in the program file (%d -> %d)"
          % (bare, os.path.getsize(program)))
    stored = open(program, "rb").read()[bare:bare + 328]
    check(stored == open(r.save("gpsp.cfg.tns"), "rb").read(),
          "new calculator: the program's settings are the copy's")
    level = read_words(r.save("gpsp.cfg.tns"))[63]
    check(level != 0, "new calculator: brightness up saved (level %d)" % level)

    # The settings come from the program: without the copy, still there.
    os.remove(r.save("gpsp.cfg.tns"))
    again = Run("new_calculator2", reuse=r).run("wait 1\nquit\n")
    # gpSP sets its brightness level at start: (10 - level) * 25 on the register.
    check(int(again.report["backlight_end"]) == (10 - level) * 25,
          "new calculator: settings read from the program file (backlight %s, level %d)"
          % (again.report["backlight_end"], level))


def test_no_gba_folder():
    """gpSP needs no gba folder: right in My Documents with the games next
    to it, the first start still makes My Documents/zgpSP_files, and the
    saves and gpSP's files go there; My Documents keeps the program, the
    games and that folder."""
    r = Run("no_gba_folder", roms=("Pattern", "Second"), in_documents=True)
    bare = os.path.getsize(r.file(PROGRAM))
    r.run(REAL_COSTS + """wait 1
shot list
press right
press enter
wait 1.5
note game
press S
wait 0.5
press Q
wait 1
""")
    check(r.notes["game"]["game"] == "Second.gba.tns", "no gba folder: the second game starts Second: %s" % r.notes["game"]["game"])
    left = sorted(n for n in os.listdir(r.dir) if n != "out")
    check(left == sorted([PROGRAM, "Pattern.gba.tns", "Second.gba.tns", "zgpSP_files"]),
          "no gba folder: My Documents has the program, the games and zgpSP_files: %s" % left)
    saves = sorted(os.listdir(r.save(""))) if os.path.isdir(r.save("")) else []
    for name in ("gpsp.cfg.tns", "gpsp_romdir.tns", "Second.gba0.svs.tns"):
        check(name in saves, "no gba folder: %s in zgpSP_files: %s" % (name, saves))
    check(os.path.getsize(r.file(PROGRAM)) == bare + 4096 + 16, "no gba folder: the settings in the program file")
    again = Run("no_gba_folder2", reuse=r).run("wait 1\npress right\npress enter\nwait 1.5\nnote game\nquit\n")
    check(again.notes["game"]["frame"] > 90, "no gba folder: Second resumed from its state (frame %d)"
          % again.notes["game"]["frame"])


def test_old_saves_folder():
    """A calculator as the builds before left it: everything
    in My Documents/zgpSP_saves (or gpSP_saves, the name before that), the
    settings as gpsp_qol.cfg.tns and in the program, an older build's
    ..cfg.tns in My Documents, the BIOS at the top of My Documents (the CX
    CAS's). A new _gpSP.tns sent over the old one (no settings inside it
    yet) moves the folder into zgpSP_files and removes it, takes the
    settings from the copy there (now gpsp.cfg.tns), moves ..cfg.tns and the
    BIOS in; the order, the brightness and the newest state hold, and
    another program's file in My Documents stays."""
    first = Run("old_saves_folder_first", roms=("Alpha", "Bravo")).run(REAL_COSTS + """wait 1
press right
press tab
wait 0.2
press left
press enter
wait 0.3
press enter
wait 1.5
press S
wait 0.5
press +
press +
wait 0.3
press Q
wait 1
""")
    for old in ("zgpSP_saves", "gpSP_saves"):
        r = Run("old_saves_folder_" + old, reuse=first)
        with open(r.save("gpsp_romdir.tns"), "w") as f:   # the last folder: this calculator's gba
            f.write(r.gba)
        old_dir = os.path.join(r.dir, old)
        os.rename(r.save(""), old_dir)
        os.rename(os.path.join(old_dir, "gpsp.cfg.tns"), os.path.join(old_dir, "gpsp_qol.cfg.tns"))
        words_file(os.path.join(r.dir, "..cfg.tns"), [0] * 16)
        with open(os.path.join(r.dir, "notes.txt.tns"), "w") as f:
            f.write("another program's file")
        with open(os.path.join(r.dir, "gba_bios.bin.tns"), "w") as f:
            f.write("not a BIOS: the built-in one stays in use")
        with open(r.file(PROGRAM), "wb") as f:   # the new build, as sent
            f.write(bytes(range(256)) * 16)
        level = read_words(os.path.join(old_dir, "gpsp_qol.cfg.tns"))[63]
        r.run(REAL_COSTS + """wait 1
note list
press enter
wait 1.5
note game
press Q
wait 1
""")
        left = sorted(n for n in os.listdir(r.dir) if n != "out")
        check(left == ["gba", "notes.txt.tns", "zgpSP_files"],
              "%s: it, ..cfg.tns and the BIOS gone from My Documents: %s" % (old, left))
        check(sorted(os.listdir(r.gba)) == sorted([PROGRAM, "Alpha.gba.tns", "Bravo.gba.tns"]),
              "%s: the game folder has the program and the games: %s" % (old, sorted(os.listdir(r.gba))))
        for name in ("gpsp.cfg.tns", "gpsp_order_gba.txt.tns", "gpsp_romdir.tns", "Bravo.gba0.svs.tns",
                     "..cfg.tns", "gba_bios.bin.tns"):
            check(os.path.exists(r.save(name)), "%s: %s in zgpSP_files" % (old, name))
        check(not os.path.exists(r.save("gpsp_qol.cfg.tns")), "%s: gpsp_qol.cfg.tns renamed" % old)
        stored = open(r.file(PROGRAM), "rb").read()
        check(len(stored) == 4096 + 4096 + 16 and stored[4096:4096 + 328] == open(r.save("gpsp.cfg.tns"), "rb").read(),
              "%s: the settings went into the new program file" % old)
        # gpSP sets its brightness level at start: (10 - level) * 25 on the register.
        check(level != 0 and r.notes["list"]["backlight"] == (10 - level) * 25,
              "%s: the brightness kept (level %d, backlight %d)" % (old, level, r.notes["list"]["backlight"]))
        check(r.notes["game"]["game"] == "Bravo.gba.tns", "%s: the list's first game is Bravo: %s"
              % (old, r.notes["game"]["game"]))
        check(r.notes["game"]["frame"] > 90, "%s: Bravo resumed from its state (frame %d)"
              % (old, r.notes["game"]["frame"]))
        check(r.report["osscr_refreshes"] == "1", "%s: the moves make the OS read its list again at exit: %s"
              % (old, r.report["osscr_refreshes"]))
        # The next start finds nothing to move: leaving is quick (no refresh).
        again = Run("old_saves_folder_%s_again" % old, reuse=r).run(REAL_COSTS + "wait 1\npress enter\nwait 1.5\npress Q\nwait 1\n")
        check(again.report["osscr_refreshes"] == "0", "%s: then a game and a new state: no refresh at exit: %s"
              % (old, again.report["osscr_refreshes"]))


def test_old_layout():
    """A calculator with files from builds before the saves folder (as a real
    one had them): states, saves, settings (older builds' too), game order, last
    folder and BIOS next to the games and gpSP move into zgpSP_files on the
    first start (a game in another folder when it starts), and nothing is
    lost: the newest state resumes, the settings and the order hold. The
    game folder keeps only the program and the games; another emulator's
    save stays."""
    first = Run("old_layout_saves", roms=("Alpha", "Bravo")).run(REAL_COSTS + """wait 1
press right
press tab
wait 0.2
press left
press enter
wait 0.3
press enter
wait 1.5
press S
wait 0.5
press +
press +
wait 0.3
press Q
wait 1
""")
    r = Run("old_layout", roms=("Alpha", "Bravo"))
    os.makedirs(os.path.join(r.gba, "More"))
    pattern_rom(os.path.join(r.gba, "More", "Inner.gba.tns"))
    # The first run's files, put where the old builds kept them.
    for name in os.listdir(first.save("")):
        if name.startswith("gpsp_order"):
            shutil.copy(first.save(name), r.file("gpsp_order.txt.tns"))
        elif name == "gpsp.cfg.tns":   # then gpsp_qol.cfg.tns
            shutil.copy(first.save(name), r.file("gpsp_qol.cfg.tns"))
        elif name not in ("gpsp_romdir.tns", "gpsp_states_time.txt.tns", "gpsp_link_run.txt.tns",
                          "gpsp_exit_after.txt.tns") and not name.startswith("gpsp_link") and \
                not name.endswith(".id.tns"):   # not from older builds (the exit marker: every quit since EXIT-3;
            # the file after the hand-back: since EXIT-R4; the link log: written in every game since
            # 2026-11-11's picture line, and not one of the files the move takes)
            shutil.copy(first.save(name), r.file(name))
    shutil.copy(r.file("Bravo.gba0.svs.tns"), os.path.join(r.gba, "More", "Inner.gba4.svs.tns"))
    for name, text in (("gpsp.cfg.tns", "older build"), ("Game.gbc.sav.tns", "other emulator"),
                       ("Alpha.gba.cfg.tns", "older build's game file"), ("Bravo.cfg.tns", "older still"),
                       ("gba_bios.bin.tns", "not a BIOS: the built-in one stays in use")):
        with open(r.file(name), "w") as f:
            f.write(text)
    r.run(REAL_COSTS + """wait 1
shot list
note list
press enter
wait 1.5
note game
press Q
wait 1
""")
    left = sorted(os.listdir(r.gba))
    check(left == sorted([PROGRAM, "Alpha.gba.tns", "Bravo.gba.tns", "More", "Game.gbc.sav.tns"]),
          "old layout: only the program, games, folders and another emulator's save stay: %s" % left)
    for name in ("Bravo.gba0.svs.tns", "gpsp_order_gba.txt.tns", "gpsp.cfg.tns",
                 "Alpha.gba.cfg.tns", "Bravo.cfg.tns", "gba_bios.bin.tns"):
        check(os.path.exists(r.save(name)), "old layout: %s moved into zgpSP_files" % name)
    check(not os.path.exists(r.save("gpsp_qol.cfg.tns")) and os.path.getsize(r.save("gpsp.cfg.tns")) == 328,
          "old layout: gpsp_qol.cfg.tns is gpsp.cfg.tns now, over the older build's")
    stored = open(r.file(PROGRAM), "rb").read()
    check(len(stored) == 4096 + 4096 + 16 and stored[4096:4096 + 328] == open(r.save("gpsp.cfg.tns"), "rb").read(),
          "old layout: the settings went into the program file on the first start")
    check(open(r.save("gpsp_order_gba.txt.tns")).read().split()[0] == "Bravo.gba.tns",
          "old layout: the game order kept (Bravo first)")
    check(r.notes["game"]["game"] == "Bravo.gba.tns", "old layout: the list's first game is Bravo: %s"
          % r.notes["game"]["game"])
    check(r.notes["game"]["frame"] > 90, "old layout: Bravo resumed from its state (frame %d)" % r.notes["game"]["frame"])
    check(r.report["backlight_start"] != r.report["backlight_end"] or r.notes["list"]["backlight"] != 120,
          "old layout: the brightness setting kept")
    check(os.path.exists(os.path.join(r.gba, "More", "Inner.gba4.svs.tns")),
          "old layout: a game in another folder keeps its files there until it starts")

    inner = Run("old_layout_inner", reuse=r).run(REAL_COSTS + "wait 1.5\nquit\n",
                                                 os.path.join("More", "Inner.gba.tns"))
    check(not os.path.exists(os.path.join(inner.gba, "More", "Inner.gba4.svs.tns")) and
          os.path.exists(inner.save("Inner.gba4.svs.tns")), "old layout: moved when its game starts")


def gbz_rom(path):
    """The ROM in a .gbz, read by tools/gbz.py (which checks its CRC)."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("gbz", os.path.join(HERE, "..", "..", "tools", "gbz.py"))
    gbz = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gbz)
    with open(path, "rb") as f:
        return gbz.unpack(f.read())


def test_rom_manager():
    """The ROM manager, from the menu without a game: A marks all, C makes
    them .gbz files (as tools/gbz.py reads them; a zip too, named after the
    ROM in it), U makes them .gba files back, the same bytes; the game order
    follows the new names. Then on the game that's running: it keeps
    running, from its new file."""
    r = Run("rom_manager", roms=("Alpha", "Bravo"))
    originals = {n: open(r.file(n + ".gba.tns"), "rb").read() for n in ("Alpha", "Bravo")}
    zip_rom(r.file("Charlie (USA).zip.tns"), inner="Charlie.gba", note=False)
    originals["Charlie"] = zip_member_data(r.file("Charlie (USA).zip.tns"), "Charlie.gba")
    open_manager = NO_GAME_ROM_MANAGER
    r.run("""wait 1
press right
press tab
wait 0.2
press left
press enter
wait 0.3
""" + open_manager + """shot manager
press A
wait 0.2
press C
wait 1
shot compressed
press esc
wait 0.3
press esc
wait 0.5
shot list
quit
""")
    files = sorted(os.listdir(r.gba))
    check(files == sorted([PROGRAM, "Alpha.gbz.tns", "Bravo.gbz.tns", "Charlie.gbz.tns"]),
          "rom manager: .gbz files for all (the zip too): %s" % files)
    for name, data in originals.items():
        path = r.file(name + ".gbz.tns")
        if os.path.exists(path):
            check(gbz_rom(path) == data, "rom manager: %s.gbz holds %s.gba as it was" % (name, name))
    order = open(r.save("gpsp_order_gba.txt.tns")).read().split("\n")
    check(order[:2] == ["Bravo.gbz.tns", "Alpha.gbz.tns"], "rom manager: the order follows: %s" % order)
    check(r.shot("manager").box(20, 50) is not None and r.shot("compressed").box(20, 50) is not None,
          "rom manager: its list")

    back = Run("rom_manager_back", reuse=r).run("""wait 1
""" + open_manager + """press A
wait 0.2
press U
wait 1
shot uncompressed
quit
""")
    files = sorted(os.listdir(back.gba))
    check(files == sorted([PROGRAM, "Alpha.gba.tns", "Bravo.gba.tns", "Charlie.gba.tns"]),
          "rom manager: U made .gba files: %s" % files)
    for name, data in originals.items():
        path = back.file(name + ".gba.tns")
        check(os.path.exists(path) and open(path, "rb").read() == data, "rom manager: %s.gba as it was" % name)

    # The game that's running: zipped under it, it plays on.
    play = Run("rom_manager_running", reuse=back).run(REAL_COSTS + """wait 1
press enter
wait 1.5
press menu
wait 0.3
""" + to_main("rom_manager") + """press enter
wait 0.5
press C
wait 1
shot running
press esc
wait 0.3
press esc
wait 0.3
press esc
wait 0.3
press esc
wait 0.3
wait 0.5
note a
wait 1
note b
press S
wait 0.5
quit
""")
    check(not os.path.exists(play.file("Bravo.gba.tns")) and os.path.exists(play.file("Bravo.gbz.tns")),
          "rom manager: the running game compressed")
    check(play.notes["a"]["game"] == "Bravo.gbz.tns", "rom manager: it runs from its .gbz now: %s"
          % play.notes["a"]["game"])
    check(play.notes["b"]["frame"] > play.notes["a"]["frame"] + 50, "rom manager: the running game goes on")
    check(os.path.exists(play.save("Bravo.gba0.svs.tns")), "rom manager: its states keep their name")


def test_states_from_older_builds():
    """Older builds' states and the first QoL build's (as on a real
    calculator: Emerald's slot 6 from the QoL build, then slots 0 and 1 from
    the old build): the most recently written one resumes, until this build
    has saved one."""
    a = Run("older_states").run("""
wait 1
press enter
wait 1
press S
note s0
wait 1
press S
note s1
wait 1
press S
note s2
wait 0.5
quit
""")
    s0, s1, s2 = (a.notes[n]["frame"] for n in ("s0", "s1", "s2"))
    gpsz_to_raw(a.save("Pattern.gba0.svs.tns"))
    gpsz_to_raw(a.save("Pattern.gba1.svs.tns"))
    gpsz_unflag(a.save("Pattern.gba2.svs.tns"))
    check(os.path.getsize(a.save("Pattern.gba1.svs.tns")) == 425984, "older states: a raw 416 KB state")
    os.utime(a.save("Pattern.gba2.svs.tns"), (1000 + 10 * 60, 1000 + 10 * 60))   # the QoL build's, 16:10
    os.utime(a.save("Pattern.gba0.svs.tns"), (1000 + 20 * 60, 1000 + 20 * 60))   # the old build's, 16:20
    os.utime(a.save("Pattern.gba1.svs.tns"), (1000 + 23 * 60, 1000 + 23 * 60))   # and 16:23

    b = Run("older_states2", reuse=a).run("""
wait 1
press enter
wait 0.3
note resumed
press S
note s3
wait 0.5
quit
""")
    resumed = b.notes["resumed"]["frame"]
    check(s1 <= resumed < s2, "older states: slot 1, the last written, resumed: frame %d (slots %d %d %d)"
          % (resumed, s0, s1, s2))
    check(os.path.exists(b.save("Pattern.gba3.svs.tns")), "older states: the next save goes to slot 3")

    # The calculator was reset since: this build's state has the older date,
    # but its count still makes it the newest.
    os.utime(b.save("Pattern.gba3.svs.tns"), (500, 500))
    c = Run("older_states3", reuse=b).run("""
wait 1
press enter
wait 0.3
note resumed
quit
""")
    s3 = b.notes["s3"]["frame"]
    check(c.notes["resumed"]["frame"] >= s3, "older states: this build's slot 3 resumes, whatever the dates: "
          "frame %d (slot 3 at %d)" % (c.notes["resumed"]["frame"], s3))


GAMES = ["Advance Wars", "Fire Emblem", "Golden Sun", "Minish Cap", "Pokemon Emerald", "Kirby",
         "Metroid", "Mario Kart", "Castlevania", "Zelda", "Wario"]


def test_number_keys():
    """The number keys start nothing (they used to load games; they navigate instead): in the game list, as in every menu, 8 2 5 4 6 move as the
    arrows do (8 up, 2 and 5 down, 4 left, 6 right) and the other digits do nothing; on the paused menu the
    other digits pick no slot and start no game either."""
    first = Run("numbers", roms=GAMES)
    order = sorted(GAMES, key=str.lower)
    r = Run("numbers_list", reuse=first).run("""
wait 1
press 1
press 3
press 0
press 9
press 7
wait 0.5
shot digits
note digits
press 6
press 6
wait 0.3
shot right
press 2
wait 0.3
shot down
press 4
wait 0.3
shot left
press 8
wait 0.3
shot up
press 5
wait 0.3
shot down5
press right
wait 0.3
shot arrows
press enter
wait 1
note started
press menu
wait 0.4
shot menu
press 3
press 1
press 7
press 9
press 0
wait 0.4
shot menu_digits
note menu_digits
quit
""")
    check(r.shot("digits").ui == "list:0" and r.notes["digits"]["game"] in ("", "-"),
          "numbers: 1 3 0 9 7 in the list start nothing and pick nothing: %s, game %r"
          % (r.shot("digits").ui, r.notes["digits"]["game"]))
    for shot, want in (("right", 2), ("down", 5), ("left", 4), ("up", 1), ("down5", 4), ("arrows", 5)):
        check(r.shot(shot).ui == "list:%d" % want, "numbers: %s in the list: picked %s, want %d"
              % (shot, r.shot(shot).ui, want))
    check(r.notes["started"]["game"] == order[5] + ".gba.tns", "numbers: enter starts the picked game: %s"
          % r.notes["started"]["game"])
    check(r.shot("menu_digits").ui == r.shot("menu").ui and r.notes["menu_digits"]["game"] == order[5] + ".gba.tns",
          "numbers: digits on the paused menu change nothing: %s then %s, %s"
          % (r.shot("menu").ui, r.shot("menu_digits").ui, r.notes["menu_digits"]["game"]))


def test_list_grid_edges():
    """r19, the game list's grid of three across (docs/ui/r19.txt): left and right stop at the row's ends (a
    shorter last row: its last game), down from the last row goes round to the first, up from the first row goes
    to the tabs (GAMES), and down or enter there goes back into the games."""
    r = Run("list_edges", roms=GAMES).run("""wait 1
press left
wait 0.2
shot left_edge
press right
press right
press right
press right
wait 0.2
shot right_edge
press down
press down
press down
wait 0.2
shot last_row
press right
press right
press right
wait 0.2
shot short_row_end
press down
wait 0.2
shot wrapped
press up
wait 0.2
shot tabs
press down
wait 0.2
shot back_in
press up
wait 0.2
press enter
wait 0.2
shot enter_in
quit
""")
    want = (("left_edge", "list:0"), ("right_edge", "list:2"), ("last_row", "list:11"), ("short_row_end", "list:11"),
            ("wrapped", "list:2"), ("tabs", "main:%d" % NO_GAME["games"]), ("back_in", "list:2"),
            ("enter_in", "list:2"))
    for shot, ui in want:
        check(r.shot(shot).ui == ui, "list grid: %s: %s, want %s" % (shot, r.shot(shot).ui, ui))


def test_digit_keys_in_game():
    """r19: in a game 8 5 4 6 are Up Down Left Right (as they were), and 7 9 1 3 do nothing (they were the
    diagonals) unless the player binds them."""
    held = ""
    for k in ("8", "5", "4", "6", "7", "9", "1", "3"):
        held += "hold %s\nwait 0.2\npeek k%s 4000130:2\nrelease %s\nwait 0.2\n" % (k, k, k)
    r = Run("digit_keys").run("wait 1\npress enter\nwait 1.5\n" + held + "quit\n")

    def pressed(run, name):
        return ~struct.unpack("<H", open(os.path.join(run.out, "%s.bin" % name), "rb").read())[0] & 0x3FF

    for k, want in (("8", "Up"), ("5", "Down"), ("4", "Left"), ("6", "Right")):
        check(pressed(r, "k" + k) == GBA_BITS[want], "digit keys: %s is %s in a game: %03x" % (k, want, pressed(r, "k" + k)))
    for k in ("7", "9", "1", "3"):
        check(pressed(r, "k" + k) == 0, "digit keys: %s does nothing in a game unless bound: %03x" % (k, pressed(r, "k" + k)))


def test_move_and_sort():
    """tab moves a game, the order stays, shift+tab sorts A to Z."""
    games = ["Alpha", "Bravo", "Charlie", "Delta"]
    r = Run("move", roms=games).run("""
wait 1
press down
press down
press down
press tab
wait 0.2
shot moving
press up
press up
press up
press enter
wait 0.3
shot moved
quit
""")
    order = open(r.save("gpsp_order_gba.txt.tns")).read().split()
    check(order == ["Delta.gba.tns", "Alpha.gba.tns", "Bravo.gba.tns", "Charlie.gba.tns"],
          "move: Delta to the top: %s" % order)
    check(r.shot("moved").active_list_row() == 0, "move: the highlight stays on it")

    again = Run("move2", reuse=r).run("""
wait 1
press enter
wait 0.5
note started
quit
""")
    check(again.notes["started"]["game"] == "Delta.gba.tns", "move: the order is kept, the first game is Delta")

    cancel = Run("move3", reuse=r).run("""
wait 1
press tab
press down
press down
press esc
wait 0.3
shot list
press enter
wait 0.5
note started
quit
""")
    check(cancel.notes["started"]["game"] == "Delta.gba.tns", "move: esc puts it back")
    check(open(cancel.save("gpsp_order_gba.txt.tns")).read().split()[0] == "Delta.gba.tns",
          "move: esc doesn't save")

    sort = Run("move4", reuse=r).run("""
wait 1
hold shift
press tab
release shift
wait 0.3
press enter
wait 0.5
note started
quit
""")
    check(not os.path.exists(sort.save("gpsp_order_gba.txt.tns")), "sort: the order is forgotten")
    check(sort.notes["started"]["game"] == "Alpha.gba.tns", "sort: A to Z again, the first game is Alpha")


def test_hotkeys_in_menu_keys():
    """Controls lists the brightness keys, and a key can be changed."""
    r = Run("keys").run("""
wait 1
press enter
wait 1
press menu
wait 0.3
""" + to_main("controls") + """press enter
wait 0.3
shot keys_top
press up
press up
press up
wait 0.3
shot brighter_line
press enter
wait 0.2
press 9
wait 0.3
shot changed
press esc
press esc
wait 0.5
press 9
wait 0.3
note brighter
press Q
wait 1
quit
""")
    check(r.shot("brighter_line").active_menu_line("keys") == 20, "keys: on Brightness up, from the top "
          "three lines up (Reset, Brightness down, Brightness up): %s" % r.shot("brighter_line").ui)
    check(r.notes["brighter"]["backlight"] < 120, "keys: 9 now makes it brighter: %d"
          % r.notes["brighter"]["backlight"])


def test_folders():
    """A game in a folder (a folder is an item of the games' grid after the games: enter on it opens it): its
    saves go there, and gpSP opens there next time."""
    r = Run("folders")
    os.makedirs(os.path.join(r.gba, "Games"))
    pattern_rom(os.path.join(r.gba, "Games", "Inner.gba.tns"))
    r.run("""
wait 1
press right
wait 0.2
press enter
wait 0.5
shot inside
press enter
wait 1
note playing
press Q
wait 1
quit
""")
    check(r.shot("inside").ui == "list:0", "folders: enter opens the folder, its game picked: %s" % r.shot("inside").ui)
    check(r.notes["playing"]["game"] == "Inner.gba.tns", "folders: the game in the folder")
    check(os.path.exists(r.save("Inner.gba0.svs.tns")), "folders: its state is there")
    again = Run("folders2", reuse=r).run("""
wait 1
press enter
wait 0.5
note playing
quit
""")
    check(again.notes["playing"]["game"] == "Inner.gba.tns", "folders: opens in the last folder")


def test_gbz():
    """A compressed ROM (.gbz.tns) is listed, plays, and has the .gba's saves."""
    r = Run("gbz", roms=())
    rom = os.path.join(r.dir, "Packed.gba.tns")
    pattern_rom(rom)
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "..", "..", "tools", "gbz.py"), "pack", "-o", r.gba,
                    rom], check=True, capture_output=True)
    r.run("""
wait 1
press enter
wait 2
note a
wait 1
note b
press S
wait 0.5
quit
""")
    check(r.notes["a"]["game"] == "Packed.gbz.tns", "gbz: listed and started: %s" % r.notes["a"]["game"])
    check(55 <= r.notes["b"]["frame"] - r.notes["a"]["frame"] <= 61, "gbz: plays")
    check(os.path.exists(r.save("Packed.gba0.svs.tns")), "gbz: states named after the .gba")


def test_list_details_after_game():
    """UI-2: back in the list after saving a state, the picked game's details
    (picture, count) are looked up again, not shown from before the game."""
    r = Run("list_details", roms=("Alpha", "Bravo")).run(REAL_COSTS + """
wait 1.2
shot before
press enter
wait 1.5
press S
wait 0.5
press menu
wait 0.3
press up
press up
wait 0.2
press enter
wait 1.2
shot after
quit
""")
    check(r.shot("before").pixels != r.shot("after").pixels,
          "list details: the saved state shows when the list is back (not the old details)")


def test_switch_game_os_paths():
    """The paused menu's other game starts (GAMES, its grid, enter; r19: Resume has no other games), with the calculator's own paths (--os-cwd: its
    OS gives the folder as "\\documents\\gba\\"). 2026-11-17, the reference CX II: up on Resume to another game,
    enter, and it didn't start ("Couldn't start it"): the other games' paths start with that backslash and the
    start took them for names in the folder, joining them to it again (gpsp_states_time.txt.tns: "ROM load
    \\documents\\gba\\/\\documents\\gba\\/Pokemon - Emerald Version (USA, Europe).gbz.tns: 1.7 ms FAILED"). The PC's
    folder starts with '/', so no test saw it. Big .gbz games named as the reference are (spaces, a comma,
    parentheses): one from the list, then the other from the paused menu, then back, each with its own saves."""
    names = ("Legend of Zelda, The - The Minish Cap (USA)", "Pokemon - Emerald Version (USA, Europe)")
    r = Run("switch_os_paths", roms=())
    for i, name in enumerate(names):
        tmp = r.file(name + ".gba.tns")
        big_rom(tmp, megabytes=8, seed=11 + i)
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "..", "..", "tools", "gbz.py"), "pack", "-o", r.gba,
                        tmp], check=True, capture_output=True)
        os.remove(tmp)
    r.run(REAL_COSTS + """wait 1
press enter
wait 3
note first
press S
wait 0.6
press menu
wait 0.5
""" + to_main("game_list") + """press enter
wait 0.6
press right
wait 0.6
shot other
press enter
wait 4
shot switched
note switched
press S
wait 0.6
press menu
wait 0.5
""" + to_main("game_list") + """press enter
wait 0.6
press enter
wait 4
note back
quit
""", options=["--os-cwd", "1"])
    gbz = [n + ".gbz.tns" for n in names]
    check(r.notes["first"]["game"] == gbz[0], "switch, the OS's paths: the first game from the list: %s"
          % r.notes["first"]["game"])
    check(r.shot("other").ui == "list:1", "switch, the OS's paths: GAMES, enter, right picks the other game: %s"
          % r.shot("other").ui)
    check(r.notes["switched"]["game"] == gbz[1] and r.shot("switched").ui != "stopped",
          "switch, the OS's paths: enter on the other game starts it: %s (%s; %s)"
          % (r.notes["switched"]["game"], r.shot("switched").ui, r.report.get("load_error")))
    check(r.notes["back"]["game"] == gbz[0], "switch, the OS's paths: and back to the first: %s"
          % r.notes["back"]["game"])
    for n in names:
        check(saves_named(r, n + ".gba0.svs.tns"), "switch, the OS's paths: %s has its state, as the game's own: %s"
              % (n, saves_named(r, n[:6])))


def test_menu_load_and_exit():
    """Load new game from the menu (the first game is saved), then Exit gpSP."""
    r = Run("load_exit", roms=("Alpha", "Bravo")).run(REAL_COSTS + """
wait 1
press enter
wait 1.5
note alpha
press menu
wait 0.3
""" + to_main("game_list") + """wait 0.2
shot load_line
press enter
wait 0.5
press right
press enter
wait 1
note bravo
press menu
wait 0.3
""" + to_main("exit") + """wait 0.2
shot exit_line
press enter
wait 1
note after_quit
quit
""")
    check(r.shot("load_line").active_menu_line("main") == MAIN["games"],
          "load: on Games: %s" % r.shot("load_line").ui)
    check(r.notes["bravo"]["game"] == "Bravo.gba.tns", "load: the other game runs")
    check(os.path.exists(r.save("Alpha.gba0.svs.tns")), "load: the first game was saved on leaving")
    check(r.shot("exit_line").active_menu_line("main") == MAIN["exit"], "exit: on Quit")
    # Quit asks nothing (no extra step on the way out): enter on it leaves, saving first
    # (Save when I quit is on)
    check("after_quit" not in r.notes, "exit: Quit leaves at once, no question")
    check("quit" not in r.notes, "exit: gpSP closed by itself")
    check(os.path.exists(r.save("Bravo.gba0.svs.tns")), "exit: the game was saved")
    check(r.report["lcd_base_end"] == "os", "exit: the OS's screen back")
    check(r.report["osscr_refreshes"] == "1" and r.report["osscr_refresh_lcd"] == "os",
          "exit: the OS's file list refreshed once, after its screen is back (%s %s)"
          % (r.report["osscr_refreshes"], r.report["osscr_refresh_lcd"]))


def test_frameskip():
    """A game too slow to draw every frame keeps its speed by skipping drawing."""
    r = Run("frameskip").run("""
cost frame 9
cost line 50
cost turn 3
wait 1
press enter
wait 2
note a
wait 2
note b
quit
""")
    a, b = r.notes["a"], r.notes["b"]
    game_fps = (b["frame"] - a["frame"]) / 2.0
    shown_fps = (b["shown"] - a["shown"]) / 2.0
    check(57 <= game_fps <= 61, "frameskip: the game keeps full speed: %.1f" % game_fps)
    check(20 <= shown_fps < 55, "frameskip: some frames aren't drawn: %.1f shown" % shown_fps)


def test_auto_frameskip():
    """Automatic frameskip (the Frameskip value 4 at most in a row) draws as
    many frames as the game's full speed leaves time for. A game whose
    drawn frames take ~20 ms and skipped ones 9 draws 2 in 3 (40 a second;
    one in two before 2026-10-16, then 43 drawing whenever it was at most a
    frame late, which hunted). An older CX whose timers don't run can't tell a late frame:
    there it was never skipping (41 frames a second); now each real second
    sets how many it skips. Both keep the game at full speed."""
    script = """wait 1
press enter
wait 8
note a
wait 4
note b
quit
"""
    config = owner_config()
    config[24], config[25] = 0, 4   # automatic, 4 at most
    cx_costs = "cost frame 12\ncost line 75\ncost turn 3\ncost touchpad 0.4\n"
    cases = (("heavy_cx2", "cost frame 9\ncost line 50\ncost turn 3\n", ["--model", "cx2"], 39),
             ("cx_no_timer", cx_costs, ["--model", "cx", "--timer-dead", "1"], 15))
    for name, costs, options, least in cases:
        r = Run("autoskip_" + name, files={"gpsp_qol.cfg.tns": config}).run(costs + script, options=options)
        a, b = r.notes["a"], r.notes["b"]
        seconds = float(b["time"]) - float(a["time"])
        game = (b["frame"] - a["frame"]) / seconds
        shown = (b["shown"] - a["shown"]) / seconds
        check(57 <= game <= 63, "auto frameskip, %s: the game at full speed: %.1f" % (name, game))
        check(least <= shown < game - 5, "auto frameskip, %s: %.1f drawn a second (at least %d)" % (name, shown, least))


def test_auto_frameskip_levels():
    """Automatic frameskip on an older CX (it must reflect the true
    maximum, frames /30 or /20, and work properly): for
    a CPU budget of about 100%, 70%, 45% and 30% of what drawing every frame
    takes, it keeps the game at full speed with as many pictures as the
    frames' costs leave time for, within about a second, and otherwise
    (30%) runs as fast as its floor (one picture in Frameskip value + 1 = 5)
    lets it. The counter says the game's frames and the pictures drawn.

    Changed with the pace rule (2026-11-15, final/pace): a picture is left
    out one at a time when the game falls behind, and the readout is
    "fps: 60  drawn: 35" (nspire_fps_full is full speed's 60)."""
    config = owner_config()
    config[24], config[25] = 0, 4   # automatic, 4 at most
    F = 1000.0 / 59.7275
    cases = (("100", 5.0, 10.0, True), ("70", 7.5, 15.0, True),
             ("45", 9.0, 25.0, True), ("30", 18.0, 36.0, False))
    notes = "".join("wait 0.25\nnote t%d\n" % i for i in range(24))
    for name, game_ms, draw_ms, keeps in cases:
        costs = "cost frame %.2f\ncost line %.2f\ncost turn 0\ncost touchpad 0.4\n" % (game_ms, draw_ms * 1000 / 160)
        r = Run("autolevel_" + name, files={"gpsp_qol.cfg.tns": config}).run(
            costs + "wait 1\npress enter\nwait 0.2\nnote start\n" + notes + "quit\n", options=["--model", "cx"])
        start = float(r.notes["start"]["time"])
        seen = [(float(r.notes["t%d" % i]["time"]) - start, r.notes["t%d" % i]["fps"]) for i in range(24)]
        settled = [readout_numbers(fps) for t, fps in seen if t >= 1.2 and fps != "--"]
        fulls = set(r.notes["t%d" % i]["full"] for i in range(24) if seen[i][0] >= 1.2)
        check(fulls == {"60"}, "auto frameskip levels, %s%%: full speed is 60: %s" % (name, sorted(fulls)))
        a, b = r.notes["t8"], r.notes["t23"]
        seconds = float(b["time"]) - float(a["time"])
        game = (b["frame"] - a["frame"]) / seconds
        drawn = (b["shown"] - a["shown"]) / seconds
        skipped_ms, drawn_ms = game_ms + 0.4, game_ms + 0.4 + draw_ms
        what = "auto frameskip, %s%%: %.1f game frames, %.1f drawn a second; readouts %s" % (
            name, game, drawn, " | ".join(fps for t, fps in seen))
        if keeps:
            ideal = 59.7275 * (1 - max(0.0, (drawn_ms - F) / draw_ms))
            check(58.5 <= game <= 61, what + " (full speed)")
            check(ideal - 2 <= drawn <= ideal + 1.5, what + " (only as many left out as needed: %.1f)" % ideal)
        else:
            expect = 5 * 1000.0 / (drawn_ms + 4 * skipped_ms)
            check(abs(game - expect) <= 2, what + " (as fast as one in 5 goes: %.1f)" % expect)
            check(abs(drawn - expect / 5) <= 1, what + " (the floor held)")
        check(all(n and abs(n[0] - game) <= 2.5 and abs(n[1] - drawn) <= 2.5 for n in settled[-8:]),
              what + " (the readout says both)")


def test_fps_counter_and_fast_forward():
    """The FPS counter shows over the picture; F runs the game faster."""
    r = Run("fps").run(REAL_COSTS + """
wait 1
press enter
wait 1
press menu
wait 0.3
""" + to_setting("show_fps") + cycle() + """wait 0.2
shot option
press esc
press esc
wait 2
shot counter
note a
press F
wait 1
note ff_a
shot ff
wait 1
note ff_b
press F
wait 1
quit
""")
    box = r.shot("counter").box(0, 12)
    check(box is not None and box[0] < 10, "fps: the counter is drawn at the top: %s" % (box,))
    ff = r.notes["ff_b"]["frame"] - r.notes["ff_a"]["frame"]
    shown = r.notes["ff_b"]["shown"] - r.notes["ff_a"]["shown"]
    check(ff >= 75, "fast forward: %d frames a second" % ff)
    check(58 <= shown <= 61, "fast forward: %d pictures a second" % shown)
    box = r.shot("ff").box(0, 12)
    check(box is not None and box[2] < 60, "fast forward: the counter as the speed (fps: 80), %s" % (box,))
    n = readout_numbers(r.notes["ff_b"]["fps"])
    check(n is not None and "drawn" not in r.notes["ff_b"]["fps"] and abs(n[0] - ff) <= 3,
          "fast forward: the counter says the game's frames a second: %s for %d" % (r.notes["ff_b"]["fps"], ff))


def test_fast_forward_pace():
    """Fast forward at max (no top speed) with frameskip off (the reference
    settings) or auto: a picture at every LCD refresh, as normal play
    draws (60 pictures a second at most, however many game frames run), and the
    game as fast as the rest of the time allows: 60 + (1000 - 60 x a drawn
    frame's ms) / a skipped frame's ms game frames a second. 2026-11-10 to
    11-12 pictures made way for the speed (15 a second). The counter shows
    the speed (1.6x)."""
    def ff_run(name, costs, cfg=None):
        return Run(name, files={"gpsp_qol.cfg.tns": cfg or owner_config()}).run(costs + """
wait 1
press enter
wait 1
note normal_a
wait 2
note normal_b
press F
wait 1
note ff_a
wait 2
note ff_b
shot counter
press F
wait 1
note back_a
wait 1
note back_b
quit
""")

    # A drawn frame takes 5 + 4 + 4.5 + 0.4 ms, a skipped one 5.4, a
    # refresh 16.7: a picture each refresh, and 60 + (1000 - 60 x 13.9) /
    # 5.4 = 91 game frames a second.
    heavy = ff_run("ff_heavy", "cost frame 5\ncost line 25\ncost turn 4.5\ncost touchpad 0.4\n")
    n = heavy.notes
    game = (n["ff_b"]["frame"] - n["ff_a"]["frame"]) / 2.0
    shown = (n["ff_b"]["shown"] - n["ff_a"]["shown"]) / 2.0
    check(game >= 86, "fast forward, heavy game: %.1f game frames a second" % game)
    check(58 <= shown <= 61, "fast forward, heavy game: %.1f pictures a second" % shown)
    normal = (n["normal_b"]["frame"] - n["normal_a"]["frame"]) / 2.0
    check(58 <= normal <= 61, "fast forward, heavy game: normal speed before: %.1f" % normal)
    back = n["back_b"]["frame"] - n["back_a"]["frame"]
    check(57 <= back <= 61, "fast forward, heavy game: normal speed after: %d" % back)
    box = heavy.shot("counter").box(0, 12)
    check(box is not None and 10 < box[2] < 60, "fast forward: the counter, fps: 91: %s" % (box,))
    numbers = readout_numbers(n["ff_b"]["fps"])
    check(numbers is not None and "drawn" not in n["ff_b"]["fps"] and abs(numbers[0] - game) <= 3,
          "fast forward: the counter %s, %.1f game frames a second" % (n["ff_b"]["fps"], game))

    # The reference Advance Wars as the PC estimate has it (tests/speed_scenes.sh,
    # 2026-10-16: 6.1 ms running a frame, 4.1 drawing, 4.0 for the picture).
    aw = ff_run("ff_advance_wars", "cost frame 6.1\ncost line 25.5\ncost turn 4.0\ncost touchpad 0.4\n")
    n = aw.notes
    game = (n["ff_b"]["frame"] - n["ff_a"]["frame"]) / 2.0
    shown = (n["ff_b"]["shown"] - n["ff_a"]["shown"]) / 2.0
    # 60 + (1000 - 60 x 14.6) / 6.5 = 79.
    check(game >= 75, "fast forward, Advance Wars' costs: %.1f game frames a second" % game)
    check(58 <= shown <= 61, "fast forward, Advance Wars' costs: %.1f pictures a second" % shown)

    # Heavier (7 + 4.8 + 6 + 0.4 ms a drawn frame, over a refresh): never
    # slower than normal play, never fewer pictures. Since final/pace the
    # the reference stored Off is Automatic (nspire_config.c PACE_RULE_MARK).
    over = ff_run("ff_too_heavy", "cost frame 7\ncost line 30\ncost turn 6\ncost touchpad 0.4\n")
    n = over.notes
    game = (n["ff_b"]["frame"] - n["ff_a"]["frame"]) / 2.0
    shown = (n["ff_b"]["shown"] - n["ff_a"]["shown"]) / 2.0
    normal = (n["normal_b"]["frame"] - n["normal_a"]["frame"]) / 2.0
    normal_shown = (n["normal_b"]["shown"] - n["normal_a"]["shown"]) / 2.0
    check(game >= normal - 1, "fast forward, heavy: %.1f game frames a second (normal play %.1f)" % (game, normal))
    check(shown >= normal_shown - 1.5, "fast forward, heavy: %.1f pictures a second (normal play %.1f)" % (shown, normal_shown))
    check(58.5 <= game <= 62 and 49 <= shown <= 57,
          "fast forward, heavy: the game at 60 (%.1f), the pictures that leaves (%.1f, about 52-55)" % (game, shown))

    # Auto frameskip: the same.
    cfg = owner_config()
    cfg[24] = 0   # auto
    auto = ff_run("ff_auto_frameskip", "cost frame 5\ncost line 25\ncost turn 4.5\ncost touchpad 0.4\n", cfg)
    n = auto.notes
    game = (n["ff_b"]["frame"] - n["ff_a"]["frame"]) / 2.0
    shown = (n["ff_b"]["shown"] - n["ff_a"]["shown"]) / 2.0
    # (normal play draws them all here: so does fast forward)
    check(game >= 86, "fast forward, auto frameskip: %.1f game frames a second" % game)
    check(58 <= shown <= 61, "fast forward, auto frameskip: %.1f pictures a second" % shown)

    # A light game: 60 + (1000 - 60 x 6.0) / 2.4 = 327, 60 pictures.
    light = ff_run("ff_light", "cost frame 2\ncost line 10\ncost turn 2\ncost touchpad 0.4\n")
    n = light.notes
    game = (n["ff_b"]["frame"] - n["ff_a"]["frame"]) / 2.0
    shown = (n["ff_b"]["shown"] - n["ff_a"]["shown"]) / 2.0
    check(game >= 310, "fast forward, light game: %.1f game frames a second" % game)
    check(58 <= shown <= 61, "fast forward, light game: %.1f pictures a second" % shown)


def test_fast_forward_max_speed():
    """The fast forward's top speed (a settings file's word 67: tenths, 0 for max; r19: no
    tile for it, the calculator always runs it unlimited). Fast forward holds the speed chosen (waiting
    when it's ahead) with a picture each LCD refresh while that fits,
    normal play is untouched. A speed the game can't reach with a picture
    at every refresh is not reached: pictures don't make way (since
    2026-11-12; before, down to 10 a second)."""
    def limited(name, tenths, costs="cost frame 2\ncost line 10\ncost turn 2\n"):
        cfg = owner_config()
        cfg[67] = tenths
        return Run(name, files={"gpsp_qol.cfg.tns": cfg}).run(costs + """cost touchpad 0.4
wait 1
press enter
wait 1
note normal_a
wait 2
note normal_b
press F
wait 1
note ff_a
wait 2
note ff_b
quit
""")

    for tenths, low, high in ((15, 88, 91), (11, 64.5, 67), (30, 177, 181)):
        r = limited("ff_max_%d" % tenths, tenths)
        n = r.notes
        game = (n["ff_b"]["frame"] - n["ff_a"]["frame"]) / 2.0
        shown = (n["ff_b"]["shown"] - n["ff_a"]["shown"]) / 2.0
        normal = (n["normal_b"]["frame"] - n["normal_a"]["frame"]) / 2.0
        check(low <= game <= high, "fast forward max %d fps: %.1f game frames a second" % (6 * tenths, game))
        check(58 <= shown <= 61, "fast forward max %d fps: %.1f pictures a second" % (6 * tenths, shown))
        check(58 <= normal <= 61, "fast forward max %d fps: normal play %.1f" % (6 * tenths, normal))
        numbers = readout_numbers(n["ff_b"]["fps"])
        check(numbers is not None and abs(numbers[0] - 6 * tenths * 59.7275 / 60) <= 1.5 and "drawn" not in n["ff_b"]["fps"],
              "fast forward max %d fps: the readout %s" % (6 * tenths, n["ff_b"]["fps"]))

    # 3.0x out of reach with a picture each refresh (5.4 ms a frame
    # running, 8.5 more drawing): 60 pictures still, and the game as fast as
    # max makes it (about 95 game frames a second; 2026-11-10 to 11-12
    # pictures made way for it: 10-14 a second, 165-180 game frames).
    r = limited("ff_max_unreachable", 30, "cost frame 5\ncost line 25\ncost turn 4.5\n")
    n = r.notes
    game = (n["ff_b"]["frame"] - n["ff_a"]["frame"]) / 2.0
    shown = (n["ff_b"]["shown"] - n["ff_a"]["shown"]) / 2.0
    check(58 <= shown <= 61, "fast forward max 3.0x out of reach: %.1f pictures a second" % shown)
    check(86 <= game < 100, "fast forward max 3.0x out of reach: %.1f game frames a second" % game)


def test_touchpad_in_fast_forward():
    """OUT-04 (2026-11-14): the touchpad (an I2C read through the OS) is
    scanned once a game frame at normal speed, and 60 times a real second in
    fast forward (3.0x: 180 game frames), not 180; an arrow on it still
    reaches the game (REG_P1) within 1/60 s and a game frame, pressed and
    let go."""
    cfg = owner_config()
    cfg[67] = 30
    arrows = ""
    for i in range(8):
        # each press at another phase of the 1/60 s between scans
        arrows += ("hold up\nwait 0.024\npeek down%d 4000130:2\nrelease up\nwait 0.024\npeek up%d 4000130:2\n"
                   "wait %.4f\n" % (i, i, 0.05 + 0.0021 * i))
    r = Run("touchpad_ff", files={"gpsp_qol.cfg.tns": cfg}).run("""cost frame 2
cost line 10
cost turn 2
cost touchpad 0.4
wait 1
press enter
wait 1
note normal_a
wait 2
note normal_b
press F
wait 1
note ff_a
wait 2
note ff_b
""" + arrows + """press F
wait 0.5
quit
""")
    n = r.notes

    def rate(part, what):
        return (int(n[part + "_b"][what]) - int(n[part + "_a"][what])) / 2.0

    game, scans = rate("normal", "frame"), rate("normal", "scans")
    check(58 <= game <= 61 and abs(scans - game) <= 1, "touchpad: normal speed, a scan a frame: %.1f scans, %.1f frames a second"
          % (scans, game))
    game, scans = rate("ff", "frame"), rate("ff", "scans")
    check(game >= 170, "touchpad: fast forward at 3.0x: %.1f game frames a second" % game)
    check(57 <= scans <= 61, "touchpad: fast forward, 60 scans a second: %.1f (%.1f game frames)" % (scans, game))

    def p1(name):
        return struct.unpack("<H", open(os.path.join(r.out, name + ".bin"), "rb").read())[0]
    for i in range(8):
        check(not p1("down%d" % i) & 0x40, "touchpad: fast forward, up held 24 ms is seen (press %d: P1 %03x)" % (i, p1("down%d" % i)))
        check(p1("up%d" % i) & 0x40, "touchpad: fast forward, up let go 24 ms is seen (press %d: P1 %03x)" % (i, p1("up%d" % i)))


FF_TRACES = os.path.join(HERE, "ff")


def ff_rhythm(scene, calc, tenths=0, seconds=4, frameskip=None, pad=False, lcd_timed=False):
    """Fast forward's pictures on a calculator (calc: cx2, cx192 or cx: the
    CX's 132 MHz and 192 MHz), each frame costing what the speed model gave
    for that Emerald scene (ff/SCENE-CALC.txt, ff/make_traces.sh): normal
    play for 2 s, then fast forward at top speed tenths (0: max). The CX
    CAS's settings on a CX (automatic frameskip, value 1), the reference on the
    CX II (off), or frameskip = (type, value). What the LCD took at each
    refresh (gpsp-host --frame-trace): pictures a second, the gaps between
    new pictures in refreshes and game frames, how often that step changes.
    pad: a USB pad (xinput) in from the start. lcd_timed: the LCD timed by
    its registers, from Ndless's 54 a second (gpsp-host --lcd-timed)."""
    cfg = owner_config()
    cfg[67] = tenths
    if frameskip:
        cfg[24], cfg[25] = frameskip
    elif calc != "cx2":
        cfg[24], cfg[25] = 0, 1
    r = Run("ff_rhythm_%s_%s_%d%s%s" % (scene, calc, tenths, "_pad" if pad else "", "_lcd" if lcd_timed else ""),
            files={"gpsp_qol.cfg.tns": cfg})
    frames = os.path.join(r.out, "frames.txt")
    r.run(("""cost touchpad 0.4
cost trace %s
wait 1
press enter
""" + ("pad in xinput\n" if pad else "") + """wait 3
note normal_a
wait 2
note normal_b
press F
wait 1
note ff_a
wait %d
note ff_b
press F
wait 1
quit
""") % (os.path.join(FF_TRACES, "%s-%s.txt" % (scene, calc)), seconds),
          options=["--frame-trace", frames] + (["--model", "cx"] if calc != "cx2" else []) +
                  (["--lcd-timed", "1"] if lcd_timed else []))
    n = r.notes
    out = {"fps": n["ff_b"]["fps"], "run": r}
    for part in ("normal", "ff"):
        t0, t1 = float(n[part + "_a"]["time"]) * 1000, float(n[part + "_b"]["time"]) * 1000
        pictures = []
        for line in open(frames):
            p = line.split()
            if p[0] == "p" and t0 <= float(p[1]) < t1:
                pictures.append((int(p[2]), int(p[3])))   # refresh, game frame
        secs = (t1 - t0) / 1000
        refreshes = [b[0] - a[0] for a, b in zip(pictures, pictures[1:])]
        steps = [b[1] - a[1] for a, b in zip(pictures, pictures[1:])]
        out[part] = {"game": (n[part + "_b"]["frame"] - n[part + "_a"]["frame"]) / secs,
                     "pictures": len(pictures) / secs, "refreshes": refreshes, "steps": steps,
                     "step_changes": sum(1 for a, b in zip(steps, steps[1:]) if a != b)}
    return out


def ff_rhythm_line(scene, calc, tenths, r):
    """One line of ff_rhythm.py's table."""
    def spread(v, scale=1.0):
        if not v:
            return "-"
        mean = sum(v) / len(v)
        sd = (sum((x - mean) ** 2 for x in v) / len(v)) ** 0.5
        return "%.1f-%.1f mean %.2f sd %.2f" % (min(v) * scale, max(v) * scale, mean * scale, sd * scale)
    f, nm = r["ff"], r["normal"]
    return ("%-6s %-5s %-4s normal %5.1f g/s %4.1f p/s | FF %6.1f g/s %.2fx %5.1f p/s | gap ms %s | step %s, "
            "changes %d/%d | counter %s" % (scene, calc, "%d.%d" % (tenths // 10, tenths % 10) if tenths else "max",
                                            nm["game"], nm["pictures"], f["game"], f["game"] / 59.7275,
                                            f["pictures"], spread(f["refreshes"], 1000 / 60.0), spread(f["steps"]),
                                            f["step_changes"], max(1, len(f["steps"]) - 1), r["fps"]))


def test_fast_forward_even():
    """Fast forward draws as many pictures as normal play, evenly (60 a
    second at most), with each frame
    costing what the speed model gave for Emerald (ff/*.txt): on a CX II
    (frameskip off) a picture at every LCD refresh but a rare late one,
    the game frames between them two neighbouring numbers (2 2 2 1 at
    1.7x, never 1 to 4 at random), and the game faster than normal play
    wherever drawing leaves time; on a CX (automatic frameskip, value 1)
    what normal play draws there, one in 2, never slower than it. R9F drew
    15 a second at max (pictures made way for the speed): 66-83 ms apart,
    11-13 game frames each."""
    def steps_ok(steps):
        """The two commonest steps are neighbours and make 95% of them."""
        counts = sorted(((steps.count(s), s) for s in set(steps)), reverse=True)
        top = counts[:2]
        share = sum(c for c, _ in top) / float(len(steps))
        return (len(top) < 2 or abs(top[0][1] - top[1][1]) == 1) and share >= 0.95, counts

    for calc, scene, tenths in (("cx2", "town", 0), ("cx2", "wipe", 0), ("cx2", "battle", 20), ("cx2", "near", 0),
                                ("cx", "town", 0), ("cx", "wipe", 0)):
        r = ff_rhythm(scene, calc, tenths)
        f, nm = r["ff"], r["normal"]
        what = "fast forward even, %s %s %s: %s" % (calc, scene, "%.1fx" % (tenths / 10.0) if tenths else "max",
                                                     ff_rhythm_line(scene, calc, tenths, r))
        # As many pictures as normal play (60 a second on a CX II), even
        # in the heaviest scene (the wild-battle wipe) to within one
        # picture and a half.
        check(f["pictures"] >= nm["pictures"] - 1.5, what + " (pictures as normal play)")
        if calc == "cx2":
            check(f["pictures"] >= 57, what + " (60 pictures a second)")
            once = f["refreshes"].count(1) / float(len(f["refreshes"]))
            check(once >= 0.95 and max(f["refreshes"]) <= 2, what + " (a picture each refresh: %.0f%%)" % (100 * once))
        else:
            # One picture in 2 frames, as normal play, and never further apart.
            check(max(f["refreshes"]) <= max(nm["refreshes"]) and set(f["steps"]) == {2},
                  what + " (normal play's one in 2; normal's gaps %s)" % sorted(set(nm["refreshes"])))
        ok, counts = steps_ok(f["steps"])
        check(ok, what + " (steps %s)" % counts)
        # Never slower than normal play; faster wherever drawing leaves time.
        check(f["game"] >= nm["game"] - 1, what + " (not slower than normal play)")
        if (calc, scene) == ("cx2", "town"):
            check(f["game"] >= 100, what + " (the game's speed)")
        if (calc, scene) == ("cx2", "near"):
            # A drawn frame of 14.75 ms (a refresh less 1.9 ms)
            # still leaves time for skipped frames between the 60 pictures:
            # 60 + (1000 - 60 x 14.75) / 6.4 = 78 (all drawn before: 60).
            check(f["game"] >= 75, what + " (skipped frames between the pictures)")
        if tenths:
            check(abs(f["game"] - 119.5) <= 2 and set(f["steps"]) <= {1, 2, 3} and f["steps"].count(2) >= 0.95 * len(f["steps"]),
                  what + " (2.0x: two frames a picture)")

    # With a USB pad in (its OS ticks take time between frames): the pictures stay at each refresh; the game frames between
    # them get ragged and fewer, so the step rule is left out.
    r = ff_rhythm("town", "cx2", 0, pad=True)
    f, nm = r["ff"], r["normal"]
    what = "fast forward even, cx2 town max with a pad: " + ff_rhythm_line("town", "cx2", 0, r)
    once = f["refreshes"].count(1) / float(len(f["refreshes"]))
    check(f["pictures"] >= 57 and f["pictures"] >= nm["pictures"] - 1.5, what + " (60 pictures a second)")
    check(once >= 0.95 and max(f["refreshes"]) <= 2, what + " (a picture each refresh: %.0f%%)" % (100 * once))
    check(f["game"] >= nm["game"] + 15, what + " (faster than normal play)")


def test_lcd_gba_rate():
    """In fast forward the readout said around 53-54 frames drawn where 60
    should be drawn. Ndless leaves the CX II's LCD
    at 360 lines a refresh, 18.5 ms (54 a second, the logs' "LCD refresh"),
    so no more pictures could be seen. gpSP's start moves it to a GBA frame
    by its front porches (nspire.c lcd_tune) and says so in the LCD line;
    then fast forward shows a picture at each refresh, about 60 a second,
    and normal play every frame it draws."""
    r = ff_rhythm("town", "cx2", 0, lcd_timed=True)
    f, nm = r["ff"], r["normal"]
    what = "LCD at a GBA frame, cx2 town max: " + ff_rhythm_line("town", "cx2", 0, r)
    log = ""
    for name in ("gpsp_link_run.txt.tns", "gpsp_link.txt.tns"):
        path = r["run"].save(name)
        if os.path.exists(path):
            log += open(path, "rb").read().decode("latin-1")
    tuned = re.search(r"refresh 18\.[45] ms tuned to (\d+)\.(\d) ms \((\d+) lines; found [0-9a-f]{8} [0-9a-f]{8}, "
                      r"put back at the quit\)", log)
    check(tuned and 165 <= int(tuned.group(1)) * 10 + int(tuned.group(2)) <= 170,
          "LCD at a GBA frame: tuned from 18.5 ms: %s" % (tuned.group(0) if tuned else log[:300]))
    check(f["pictures"] >= 57, what + " (pictures at each refresh, about 60)")
    check(nm["pictures"] >= 59, what + " (normal play's pictures all seen: it waits for the LCD to take one)")


def test_fast_forward_follows_scene():
    """With automatic frameskip (an older CX's
    default, value 4), fast forward follows normal play's level as the scene
    changes, as normal play does. It kept the level it was switched on at:
    on in a light scene that turns heavy, 41.7 game frames a second where
    normal play does 60 (slower); on in a heavy scene (the wild-battle wipe)
    that turns light, 12.7 pictures a second where normal play draws 60.
    In each scene fast forward is never slower than normal play there, and
    never draws fewer pictures: against normal play's own seconds in that
    scene (a timer) and against a twin run with the same scenes and no fast
    forward (both; without a timer only the twin, as there normal play's
    level depends on the scenes before)."""
    heavy = "cost frame 14\ncost line 60\ncost turn 8\n"
    cfg = owner_config()
    cfg[24], cfg[25] = 0, 4   # automatic, 4 at most
    # Without a timer the game runs unpaced, so the light scene there is one
    # that draws every frame at about 70 game frames a second.
    for model, extra, light in (("cx", [], "cost frame 2\ncost line 10\ncost turn 2\n"),
                                ("cx_no_timer", ["--timer-dead", "1"], "cost frame 5\ncost line 25\ncost turn 4.5\n")):
        def script(f):
            return ("cost touchpad 0.4\n" + light + "wait 1\npress enter\nwait 3\nnote nl_a\nwait 2\nnote nl_b\n" +
                    f + "wait 1\n" + heavy + "wait 3\nnote fh_a\nwait 2\nnote fh_b\n" +
                    light + "wait 3\nnote fl_a\nwait 2\nnote fl_b\n" + f +
                    heavy + "wait 3\nnote nh_a\nwait 2\nnote nh_b\n" +
                    f + "wait 1\n" + light + "wait 3\nnote fl2_a\nwait 2\nnote fl2_b\n" +
                    f + "wait 1\nquit\n")
        n = Run("ff_scene_" + model, files={"gpsp_qol.cfg.tns": cfg}).run(
            script("press F\n"), options=["--model", "cx"] + extra).notes
        twin = Run("ff_scene_twin_" + model, files={"gpsp_qol.cfg.tns": cfg}).run(
            script("wait 0.25\n"), options=["--model", "cx"] + extra).notes

        def rate(notes, part, what):
            return ((notes[part + "_b"][what] - notes[part + "_a"][what]) /
                    (float(notes[part + "_b"]["time"]) - float(notes[part + "_a"]["time"])))
        line = ", ".join("%s %.1f g/s %.1f p/s (normal %.1f, %.1f)" % (p, rate(n, p, "frame"), rate(n, p, "shown"),
                                                                       rate(twin, p, "frame"), rate(twin, p, "shown"))
                         for p in ("nl", "nh", "fh", "fl", "fl2"))
        for ff, normal, what in (("fh", "nh", "on while light, then heavy"), ("fl", "nl", "then light again"),
                                 ("fl2", "nl", "on while heavy, then light")):
            refs = [(twin, ff)] + ([(n, normal)] if model == "cx" else [])
            for notes, part in refs:
                check(rate(n, ff, "frame") >= rate(notes, part, "frame") - 1,
                      "fast forward follows the scene, %s, %s: not slower than normal play (%s)" % (model, what, line))
                check(rate(n, ff, "shown") >= rate(notes, part, "shown") - 1.5,
                      "fast forward follows the scene, %s, %s: as many pictures as normal play (%s)" % (model, what, line))


def test_pace_rule():
    """The pace rule (2026-11-15, final/pace)."""
    F = 1000.0 / 59.7275

    def run(name, costs, cfg):
        r = Run("pace_" + name, files={"gpsp_qol.cfg.tns": cfg}).run(costs + """cost touchpad 0.4
wait 1
press enter
wait 3
note n_a
wait 3
note n_b
press F
wait 2
note f_a
wait 3
note f_b
press F
wait 1
quit
""")
        n = r.notes
        out = {}
        for part in ("n", "f"):
            a, b = n[part + "_a"], n[part + "_b"]
            secs = float(b["time"]) - float(a["time"])
            out[part] = ((b["frame"] - a["frame"]) / secs, (b["shown"] - a["shown"]) / secs, b["fps"])
            print("  pace rule: %s %s: game %.1f, pictures %.1f, readout %s" %
                  ((name, "normal play" if part == "n" else "fast forward") + out[part]))
        return out

    def ideal(drawn_ms, skipped_ms):
        return 59.7275 * (1 - min(1.0, max(0.0, (drawn_ms - F) / (drawn_ms - skipped_ms))))

    def drawn_ms(name, costs):
        cfg = owner_config()
        cfg[26] = 0xFF000001
        r = Run("pace_off_" + name, files={"gpsp_qol.cfg.tns": cfg}).run(
            costs + "cost touchpad 0.4\nwait 1\npress enter\nwait 2\nnote a\nwait 3\nnote b\nquit\n")
        a, b = r.notes["a"], r.notes["b"]
        game = (b["frame"] - a["frame"]) / (float(b["time"]) - float(a["time"]))
        return 1000.0 / game

    def line(what, r):
        return "%s: game %.1f, pictures %.1f, readout %s" % ((what,) + r)

    refcfg = owner_config()
    default = owner_config()
    default[24], default[25] = 0, 4
    r = run("spare", "cost frame 5\ncost line 25\ncost turn 3\n", refcfg)
    game, shown, fps = r["n"]
    check(58.5 <= game <= 61 and shown >= 58.5 and readout_numbers(fps) and "drawn" not in fps,
          line("pace rule (a) time to spare", r["n"]))
    game, shown, fps = r["f"]
    numbers = readout_numbers(fps)
    check(58.5 <= shown <= 60.5 and game >= 100 and "drawn" not in fps and numbers is not None and
          abs(numbers[0] - game) <= 4, line("pace rule (d) fast forward with room", r["f"]))
    heavier = "cost frame 9\ncost line 30\ncost turn 3.5\n"
    r = run("heavier", heavier, refcfg)
    game, shown, fps = r["n"]
    want = ideal(drawn_ms("heavier", heavier), 9.4)
    numbers = readout_numbers(fps)
    check(58.5 <= game <= 61 and want - 2 <= shown <= want + 1.5 and shown < 59 and numbers and
          abs(numbers[1] - shown) <= 2.5 and "drawn" in fps,
          line("pace rule (b) 4%% too heavy (%.1f pictures)" % want, r["n"]))
    for name, cfg, value in (("cas1", refcfg, 1), ("cas4", default, 4)):
        r = run(name, "cost frame 16\ncost line 62.5\ncost turn 2\n", cfg)
        game, shown, fps = r["n"]
        k = value + 1
        floor_game = 1000.0 * k / (28.4 + value * 16.4)
        numbers = readout_numbers(fps)
        check(abs(game - floor_game) <= 2 and abs(shown - game / k) <= 1 and numbers and abs(numbers[0] - game) <= 2.5,
              line("pace rule (c) far too heavy, value %d (as fast as one in %d goes: %.1f)" % (value, k, floor_game),
                   r["n"]))
    ff_heavy = "cost frame 9\ncost line 37.5\ncost turn 3\n"
    r = run("ff_heavy", ff_heavy, refcfg)
    game, shown, fps = r["f"]
    want = ideal(drawn_ms("ff_heavy", ff_heavy), 9.4)
    numbers = readout_numbers(fps)
    check(58.5 <= game <= 62 and want - 3 <= shown <= want + 3 and numbers and abs(numbers[0] - game) <= 3,
          line("pace rule (e) fast forward, 60 pictures too many (%.1f pictures)" % want, r["f"]))
    game, shown, fps = r["n"]
    check(58.5 <= game <= 61 and want - 3 <= shown <= want + 2,
          line("pace rule (e) the same scene in normal play", r["n"]))


def test_frameskip_off_once():
    """(g) of the pace rule (2026-11-15, final/pace): stored Off is Automatic once."""
    costs = "cost frame 9\ncost line 37.5\ncost turn 3\ncost touchpad 0.4\n"
    play = "wait 3\nnote a\nwait 3\nnote b\n"

    def rates(r):
        a, b = r.notes["a"], r.notes["b"]
        secs = float(b["time"]) - float(a["time"])
        return (b["frame"] - a["frame"]) / secs, (b["shown"] - a["shown"]) / secs

    first = Run("off_once", files={"gpsp_qol.cfg.tns": owner_config()})
    first.run(costs + "wait 1\npress enter\n" + play + "quit\n")
    game, shown = rates(first)
    check(58.5 <= game <= 61 and shown < 56, "off once: the stored Off plays as Automatic: %.1f game frames, %.1f pictures"
          % (game, shown))
    path = first.save("gpsp.cfg.tns")
    words = read_words(path)
    check(len(words) == 82 and words[24] == 0 and words[26] == 0xFF000001,
          "off once: written Automatic with the mark: %d words, type %s, word 26 %08x" %
          (len(words), words[24] if len(words) > 24 else "-", words[26] if len(words) > 26 else 0))
    # (r19: Frame skip has no tile: Off "chosen" is the file's Off with the mark that says it was chosen)
    off_words = list(words)
    off_words[24] = 2
    chosen = Run("off_once_chosen", files={"gpsp.cfg.tns": off_words})
    chosen.run(costs + "wait 1\npress enter\nwait 1\n" + play + "quit\n")
    game, shown = rates(chosen)
    check(game < 57 and abs(shown - game) <= 1.5, "off once: Off chosen draws every picture: %.1f game frames, %.1f pictures"
          % (game, shown))
    words = read_words(chosen.save("gpsp.cfg.tns"))
    check(words[24] == 2 and words[26] == 0xFF000001, "off once: the chosen Off written with the mark: type %d, %08x" %
          (words[24], words[26]))
    again = Run("off_once_again", reuse=chosen)
    again.run(costs + "wait 1\npress enter\n" + play + "quit\n")
    game, shown = rates(again)
    check(game < 57 and abs(shown - game) <= 1.5, "off once: the chosen Off stays Off: %.1f game frames, %.1f pictures"
          % (game, shown))


def test_blended():
    """The blended pictures (one pass, nspire_blend.c) with interframe
    blending: where they are, the FPS counter and messages over them (a text
    band is only drawn again when its text changes), fullscreen, and the
    bands after the menu."""
    def three_shots(name):
        return "\n".join("shot %s_%d\nwait 0.02" % (name, i) for i in range(3)) + "\n"

    # (r19: the settings file gives the 3:2 picture, Fit as a file's scaled_aspect, with interframe blending and the
    # FPS counter on, the reference: Flicker fix has no tile, and Screen size's Fit is the sharp picture)
    r = Run("blended", files={"gpsp_qol.cfg.tns": owner_config()}).run(REAL_COSTS + """
wait 1
press enter
wait 3
shot aspect
note a
wait 1
note b
press S
wait 0.5
""" + three_shots("saved") + """wait 2.5
""" + three_shots("after") + """press menu
wait 0.3
""" + to_setting("size") + cycle(size_cycles("Stretch", "Fit")) + """press esc
press esc
wait 2
shot full
press S
wait 0.5
shot full_saved
wait 2.5
""" + three_shots("full_after") + """press menu
wait 0.3
""" + to_setting("size") + cycle(size_cycles("Fit", "Stretch")) + """press esc
press esc
wait 1.5
""" + three_shots("back") + """press Q
wait 1
quit
""")
    # lines 14-226 (13 black): the text bands are lines 0-12 and 227-239
    box = r.shot("aspect").box(13, 240)
    check(box == (0, 14, 319, 226), "blended: the 320x213 picture at lines 14-226, %s" % (box,))
    top = r.shot("aspect").box(0, 13)
    check(top is not None and top[2] < 100, "blended: the FPS counter in the top band, %s" % (top,))
    frames = r.notes["b"]["frame"] - r.notes["a"]["frame"]
    check(57 <= frames <= 61, "blended: about 60 frames a second, %d" % frames)
    for i in range(3):
        check(r.shot("saved_%d" % i).box(228, 240) is not None, "blended: the message, frame %d" % i)
        check(r.shot("after_%d" % i).box(227, 240) is None, "blended: the message gone, frame %d" % i)
        check(r.shot("after_%d" % i).box(0, 12) is not None, "blended: the counter still there, frame %d" % i)
    full = r.shot("full")
    check(full.box() == (0, 0, 319, 239), "blended: fullscreen fills the screen, %s" % (full.box(),))
    check(full.at(319, 239) != BLACK and full.at(160, 120) != BLACK, "blended: fullscreen's picture")
    # the plate's top row, over the message's text (nspire_osd.c)
    check(r.shot("full_saved").at(160, 227) == BLACK, "blended: fullscreen's message on its plate")
    for i in range(3):
        check(r.shot("full_after_%d" % i).at(160, 235) != BLACK,
              "blended: fullscreen's picture back under the message, frame %d" % i)
    for i in range(3):
        back = r.shot("back_%d" % i)
        check(back.box(13, 240) == (0, 14, 319, 226), "blended: 3:2 again after the menu, frame %d: %s" % (i, back.box(13, 240)))
        top = back.box(0, 13)
        check(top is not None and top[2] < 100,
              "blended: only the counter in the top band after the menu, frame %d: %s" % (i, top))


def zip_rom(path, inner="Pattern.gba", note=True):
    """A zip with the pattern ROM in it (and a text file before it, unless
    note is false: a zip with more than the ROM is kept when it is made a
    .gbz, ZIP-3)."""
    tmp = path + ".rom"
    pattern_rom(tmp)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        if note:
            z.writestr("readme.txt", "a zipped game\n")
        z.write(tmp, inner)
    os.remove(tmp)


def zip_member_data(path, inner):
    with zipfile.ZipFile(path) as z:
        return z.read(inner)


def mixed_rom(path, megabytes=4):
    """The pattern ROM, then a megabyte of bytes that don't compress (pages
    a .gbz stores as they are) and text-like bytes (pages it deflates), to
    megabytes, with an odd size so the last page is partly past the end."""
    import random
    rng = random.Random(5)
    pattern_rom(path)
    data = bytearray(open(path, "rb").read())
    data += rng.randbytes(1024 * 1024)
    while len(data) < megabytes * 1024 * 1024 - 1000:
        data += bytes(rng.choice(b"ABCDEFGH ") for _ in range(4096))
    data = data[:megabytes * 1024 * 1024 - 1000]
    with open(path, "wb") as f:
        f.write(data)
    return bytes(data)


def test_zip_to_gbz():
    """A zip too big for the memory free (gpsp-host --rom-memory) becomes a
    .gbz on the loading screen, once: named after the ROM in the zip as
    tools/gbz.py names it, it loads and resumes from the state saved while
    it was a zip, the zip is gone and the game's place in the order is the
    .gbz's. Without the storage for the .gbz (--free-storage), or with a ROM
    in the zip that isn't a .gba, nothing changes and the message says
    why."""
    r = Run("zip_to_gbz", roms=("Alpha", "Zed"))
    tmp = r.file("rom.tmp")
    data = mixed_rom(tmp)
    zip_name = "Big Game (USA).zip.tns"
    with zipfile.ZipFile(r.file(zip_name), "w", zipfile.ZIP_DEFLATED) as z:
        z.write(tmp, "roms/Big Game.gba")
    os.remove(tmp)
    os.makedirs(r.save(""), exist_ok=True)
    with open(r.save("gpsp_order_gba.txt.tns"), "w") as f:
        f.write("Zed.gba.tns\n%s\n" % zip_name)
    # With the memory: the zip plays as it is; a state is saved.
    r.run("""wait 1
press right
press enter
wait 2
note saved
press S
wait 1
quit
""")
    check(r.notes["saved"]["game"] == zip_name, "zip to gbz: the zip plays: %s" % r.notes["saved"]["game"])
    check(os.path.exists(r.file(zip_name)) and not os.path.exists(r.file("Big Game.gbz.tns")),
          "zip to gbz: a zip that fits stays a zip")
    check(os.path.exists(r.save("Big Game.gba0.svs.tns")), "zip to gbz: its state")
    saved_frame = r.notes["saved"]["frame"]

    # Not the storage for the .gbz: nothing changes, it says why.
    full = Run("zip_to_gbz_full", reuse=r).run(REAL_COSTS + """wait 1
press right
press enter
wait 4
shot error
press enter
wait 1
shot list
quit
""", options=("--rom-memory", "2", "--free-storage", "512"))
    names = sorted(os.listdir(full.gba))
    check(names == sorted([PROGRAM, "Alpha.gba.tns", "Zed.gba.tns", zip_name]),
          "zip to gbz, no storage: nothing changed: %s" % names)
    why = full.report.get("load_error", "")
    # A number and its unit are joined by a non-breaking space: never
    # wrapped over two lines.
    check("storage" in why and "4\u00a0MB" in why and "2\u00a0MB free" in why and "as it was" in why,
          "zip to gbz, no storage: it says why: %r" % why)
    error = full.shot("error")
    check(error.ui == "stopped" and full.shot("list").ui.startswith("list"),
          "zip to gbz, no storage: the message, then the list: %s %s" % (error.ui, full.shot("list").ui))
    # Its lines end clear of the footer (its hairline on row 216): the rows
    # over it are empty.
    back = error.at(5, 5)
    near = [y for y in range(200, 216) for x in range(320) if error.at(x, y) != back]
    check(not near, "zip to gbz, no storage: a gap over the key: rows %s" % sorted(set(near)))

    # Q while the .gbz is being made: back to the list, nothing changed.
    cancel = Run("zip_to_gbz_cancel", reuse=r).run(REAL_COSTS + """cost read 600
wait 1
press right
press enter
wait 1.5
tap Q
wait 0.5
shot list
quit
""", options=("--rom-memory", "2"))
    names = sorted(os.listdir(cancel.gba))
    check(names == sorted([PROGRAM, "Alpha.gba.tns", "Zed.gba.tns", zip_name]) and
          cancel.shot("list").ui.startswith("list"), "zip to gbz, cancelled: the list, nothing changed: %s %s"
          % (cancel.shot("list").ui, names))

    # Short of memory: it becomes a .gbz, which loads with its state.
    conv = Run("zip_to_gbz_made", reuse=r).run(REAL_COSTS + """cost read 600
wait 1
press right
press enter
wait 1.5
shot converting
wait 1.2
shot checking
wait 5.8
note a
wait 1
note b
press menu
wait 0.3
""" + to_main("game_list") + """press enter
wait 0.5
press right
press enter
wait 3
note again
press Q
wait 1
""", options=("--rom-memory", "2"))
    shot = conv.shot("converting")
    check(shot.ui == "converting", "zip to gbz: the loading screen says so: %s" % shot.ui)
    # The bar, under the picture and the name (x 88-231, rows 184-189): part way (the mark's colour, then its track).
    left, right = shot.at(89, LOADING_BAR_Y), shot.at(230, LOADING_BAR_Y)
    check(left == MARK and right != MARK, "zip to gbz: its bar is part way: %s %s" % (left, right))
    # Then the new .gbz read back and checked, further along the bar.
    checking = conv.shot("checking")
    left, right = checking.at(160, LOADING_BAR_Y), checking.at(230, LOADING_BAR_Y)
    check(checking.ui == "checking" and left == MARK and right != MARK,
          "zip to gbz: then it checks the .gbz: %s %s %s" % (checking.ui, left, right))
    gbz = conv.file("Big Game.gbz.tns")
    check(os.path.exists(gbz) and not os.path.exists(conv.file(zip_name)) and
          not os.path.exists(gbz + ".tmp"), "zip to gbz: the .gbz, named after the ROM in the zip; the zip gone: %s"
          % sorted(os.listdir(conv.gba)))
    if os.path.exists(gbz):
        check(gbz_rom(gbz) == data, "zip to gbz: the .gbz holds the ROM, as tools/gbz.py reads it")
        with open(gbz, "rb") as f:
            blob = f.read()
        pages = (len(data) + 32767) // 32768
        lengths = [b - a for a, b in zip(*[struct.unpack_from("<%dI" % (pages + 1), blob, 32)[i:]
                                            for i in (0, 1)])]
        check(lengths.count(32768) >= 30 and len([n for n in lengths if n < 32768]) >= 60,
              "zip to gbz: pages stored as they are and deflated: %d and %d" %
              (lengths.count(32768), len([n for n in lengths if n < 32768])))
    log = conv.save("gpsp_convert.txt.tns")
    text = open(log).read() if os.path.exists(log) else ""
    stages = ("model ", "result ok", "level 6 window 13 memlevel 6", "free_mb ", "bytes_in ", "bytes_out ",
              "read_inflate_ms ", "deflate_ms ", "write_ms ", "verify_ms ", "total_ms ")
    check(all(("\n" + text).find("\n" + k) >= 0 for k in stages),
          "zip to gbz: gpsp_convert.txt.tns has every stage line: %r" % text)
    check("\nbytes_in %d\n" % len(data) in text,
          "zip to gbz: the log's bytes in are the ROM's: %r" % text)
    check(conv.notes["a"]["game"] == "Big Game.gbz.tns", "zip to gbz: the .gbz plays: %s" % conv.notes["a"]["game"])
    check(conv.notes["a"]["frame"] > saved_frame and conv.notes["b"]["frame"] > conv.notes["a"]["frame"] + 50,
          "zip to gbz: from the zip's state (frame %d), and on: %d, %d" % (saved_frame, conv.notes["a"]["frame"],
                                                                         conv.notes["b"]["frame"]))
    order = open(conv.save("gpsp_order_gba.txt.tns")).read().split("\n")
    check(order[:2] == ["Zed.gba.tns", "Big Game.gbz.tns"], "zip to gbz: its place in the order: %s" % order)
    check(conv.notes["again"]["game"] == "Big Game.gbz.tns", "zip to gbz: the list has it there: %s"
          % conv.notes["again"]["game"])
    check(all(n.startswith("Big Game.gba") for n in saves_named(conv, "Big Game")),
          "zip to gbz: its files keep the zip's names: %s" % saves_named(conv, "Big Game"))
    check("quit" not in conv.notes and conv.report.get("osscr_refreshes") == "1",
          "zip to gbz: the OS's file list is read again at exit, for the new file: %s"
          % conv.report.get("osscr_refreshes"))

    # ZIP-3: a zip with a note besides the ROM is made a .gbz and stays.
    more = Run("zip_to_gbz_more", roms=())
    tmp = more.file("rom.tmp")
    mixed_rom(tmp)
    with zipfile.ZipFile(more.file("Pack.zip.tns"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("readme.txt", "a zipped game\n")
        z.write(tmp, "Pack.gba")
    os.remove(tmp)
    more.run("""wait 1
press enter
wait 5
quit
""", options=("--rom-memory", "2"))
    names = sorted(os.listdir(more.gba))
    check(names == sorted([PROGRAM, "Pack.zip.tns", "Pack.gbz.tns"]),
          "zip to gbz: a zip with more in it stays: %s" % names)

    # A ROM in the zip that isn't a .gba: not made into a .gbz on its own.
    agb = Run("zip_to_gbz_agb", roms=())
    tmp = agb.file("rom.tmp")
    mixed_rom(tmp)
    with zipfile.ZipFile(agb.file("Other.zip.tns"), "w", zipfile.ZIP_DEFLATED) as z:
        z.write(tmp, "Other.agb")
    os.remove(tmp)
    agb.run("""wait 1
press enter
wait 2
shot error
quit
""", options=("--rom-memory", "2"))
    why = agb.report.get("load_error", "")
    check(sorted(os.listdir(agb.gba)) == sorted([PROGRAM, "Other.zip.tns"]) and agb.shot("error").ui == "stopped",
          "zip to gbz, an .agb: nothing changed: %s" % sorted(os.listdir(agb.gba)))
    check("4\u00a0MB" in why and "2\u00a0MB is free" in why and "Manage games" in why and
          "Close open documents" in why, "zip to gbz, an .agb: it says what to do: %r" % why)


# The loading screen's bar (nspire_ui_screens.c ui_loading_draw): under the picture (as the paused menu has it,
# rows 60-155) and a one-line name (baseline 174), rows 184-189.
LOADING_BAR_Y = 160   # (r19: the bar under the picture and the name, x 88-231, 6 rows from 158)


def test_loading_screen():
    """Starting a big game shows its name and a bar while the ROM is read."""
    r = Run("loading", roms=())
    path = r.file("Big.gba.tns")
    pattern_rom(path)
    with open(path, "ab") as f:
        f.write(b"\xff" * (4 * 1024 * 1024 - os.path.getsize(path)))
    r.run(REAL_COSTS + """cost read 600
wait 1
press enter
wait 1.2
shot loading
wait 3
shot game
press S
wait 1
quit
""")
    loading = r.shot("loading")
    back = loading.at(5, 5)
    check(loading.ui == "loading", "loading: the loading screen: %s" % loading.ui)
    # No state to resume from, so no picture: "Loading", the game's name and
    # the bar in the middle of the screen.
    text = [x for y in range(76, 110) for x in range(320) if loading.at(x, y) != back]
    check(text and abs(min(text) + max(text) - 320) <= 4, "loading: the name in the middle, x %s-%s"
          % (min(text) if text else None, max(text) if text else None))
    left, right = loading.at(89, LOADING_BAR_Y), loading.at(230, LOADING_BAR_Y)
    check(left == MARK and right != MARK, "loading: the bar is part way, %s %s" % (left, right))
    check(r.shot("game").box() == (0, 13, 319, 226), "loading: then the game")

    # With a state to resume from, its picture above: clear of the text.
    again = Run("loading2", reuse=r).run(REAL_COSTS + """cost read 600
wait 1
press enter
wait 1.2
shot loading
quit
""")
    loading = again.shot("loading")
    back = loading.at(5, 5)
    # The preview is centered horizontally below the header.
    rows = [y for y in range(40, 240) if any(loading.at(x, y) != back for x in range(100, 220))]
    bands = []
    for y in rows:
        if bands and y == bands[-1][1] + 1:
            bands[-1][1] = y
        else:
            bands.append([y, y])
    check(len(bands) >= 3 and bands[0] == [68, 131],
          "loading: with a state, its picture on top: rows %s" % bands)
    if bands:
        xs = [x for x in range(320) if any(loading.at(x, y) != back for y in range(68, 132))]
        check(xs and (xs[0], xs[-1]) == (112, 207), "loading: centered 96-pixel preview")
    if len(bands) >= 2:
        check(bands[1][0] - bands[0][1] >= 5, "loading: a gap between the picture and the text: rows %s" % bands)


def test_zipped_game():
    """A zipped game plays, its states are named after the ROM in the zip,
    and a zip without a ROM says so and goes back to the list."""
    r = Run("zipped", roms=())
    zip_rom(r.file("Game.zip.tns"))
    with zipfile.ZipFile(r.file("Broken.zip.tns"), "w") as z:
        z.writestr("notes.txt", "no game here")
    r.run(REAL_COSTS + """wait 1
shot list
press right
press enter
wait 2
shot game
note a
wait 1
note b
press S
wait 1
quit
""")
    check(r.shot("game").box() == (0, 13, 319, 226), "zip: the picture")
    check(r.notes["b"]["frame"] > r.notes["a"]["frame"] + 50, "zip: the game runs")
    check(os.path.exists(r.save("Pattern.gba0.svs.tns")), "zip: the state is named after the ROM in it")

    bad = Run("zipped_broken", reuse=r).run("""wait 1
press enter
wait 1
shot error
press enter
wait 1
shot list
quit
""")
    error = bad.shot("error").box(84, 160)   # (under the game's name in big words, rows 56-74)
    check(error is not None, "broken zip: it says why")
    check(bad.shot("list").active_list_row() == 0, "broken zip: then the file list")


def test_damaged_rom():
    """A game that jumps where no code can run (as an Advance Wars from a bad
    dump does, to 910803DE, as it starts) stops with a message instead of
    crashing; a key brings back the file list, and no state is saved of it."""
    r = Run("damaged", roms=("Pattern",))
    # ldr r0, [pc]; bx r0; .word 0x910803DF: a Thumb jump past the GBA's memory
    words_file(r.file("Damaged.gba.tns"), [0xE59F0000, 0xE12FFF10, 0x910803DF])
    r.run(REAL_COSTS + """wait 1
press enter
wait 1
shot stopped
press enter
wait 1
shot list
press right
press enter
wait 2
shot game
quit
""")
    stopped = r.shot("stopped")
    back = stopped.at(5, 5)

    def text(y0, y1):
        return [stopped.at(x, y) for y in range(y0, y1) for x in range(320) if stopped.at(x, y) != back]

    check(stopped.ui == "stopped", "damaged ROM: the stopped screen: %s" % stopped.ui)
    # (the title in rows 12-25, the game's name in rows 49-61, why from row 73, the key in the band)
    title = text(8, 30) and text(46, 64)
    check(title, "damaged ROM: \"Stopped\" and the game's name")
    why = text(70, 200)
    check(why, "damaged ROM: it says why, under the name")
    check(text(222, 238), "damaged ROM: and to press a key")
    bar = [p for y in range(LOADING_BAR_Y - 2, LOADING_BAR_Y + 4) for p in (stopped.at(x, y) for x in range(88, 232))
           if p != back]
    check(not bar, "damaged ROM: nothing else (no loading bar)")
    check(r.shot("list").active_list_row() == 0, "damaged ROM: then the file list")
    check(r.shot("game").box() == (0, 13, 319, 226), "damaged ROM: and another game starts")
    states = [f for f in os.listdir(os.path.join(r.dir, "zgpSP_files")) if f.startswith("Damaged") and ".svs" in f]
    check(not states, "damaged ROM: no state saved of it: %s" % states)


def test_damaged_gbz_page():
    """ZIP-2: a .gbz with a damaged page that isn't read until the game runs
    (a 3 MB ROM with a 2 MB buffer, jumping into the page) stops the game
    with the stopped screen, as a damaged ROM does, instead of running 0xFF
    code; a damaged page inside the buffer refuses the game at load."""
    import struct
    r = Run("damaged_gbz", roms=())
    rom = os.path.join(r.dir, "Jump.gba.tns")
    with open(rom, "wb") as f:
        f.write(struct.pack("<3I", 0xE59F0000, 0xE12FFF10, 0x08230000) + bytes(3 * 1024 * 1024 - 12))
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "..", "..", "tools", "gbz.py"), "pack", "-o", r.gba,
                    rom], check=True, capture_output=True)
    gbz = r.file("Jump.gbz.tns")
    with open(gbz, "r+b") as f:
        f.seek(32 + 4 * 70)
        start = struct.unpack("<I", f.read(4))[0]
        f.seek(start + 8)
        b = f.read(1)[0]
        f.seek(start + 8)
        f.write(bytes([b ^ 0xFF]))
    r.run(REAL_COSTS + """wait 1
press enter
wait 1
shot stopped
press enter
wait 1
shot list
quit
""", options=("--rom-memory", "2"))
    stopped = r.shot("stopped")
    check(stopped.ui == "stopped", "damaged .gbz page: the game is stopped: %s" % stopped.ui)
    check(r.shot("list").active_list_row() == 0, "damaged .gbz page: then the file list")


def test_slot_keys():
    """) and ( pick the next and previous slot: L loads the one picked."""
    r = Run("slot_keys").run(REAL_COSTS + """wait 1
press enter
wait 1
press S
note saved
wait 1
press )
wait 0.3
press L
wait 0.1
note empty
wait 0.5
press (
wait 0.3
press L
wait 0.1
note loaded
quit
""")
    saved, empty, loaded = (r.notes[n]["frame"] for n in ("saved", "empty", "loaded"))
    check(empty > saved + 30, "slot keys: ) picked the empty slot 1: %d after %d" % (empty, saved))
    check(abs(loaded - saved) < 15, "slot keys: ( went back to slot 0: %d, saved at %d" % (loaded, saved))


def test_save_manager():
    """The save manager (r19: the paused menu as it opens is the Saves place, SAVES, as tiles: Resume, Save, then the
    states, the newest first) lists the states; U, A and C, and del (asking
    first) work on the files."""
    start = """wait 1
press enter
wait 1
"""
    open_manager = """press menu
wait 0.5
"""
    r = Run("save_manager").run(REAL_COSTS + start + """press S
wait 0.5
press S
wait 0.5
""" + open_manager + """shot manager
""" + saves_tile(1) + """press U
wait 0.5
shot uncompressed
quit
""")
    slot0, slot1 = r.save("Pattern.gba0.svs.tns"), r.save("Pattern.gba1.svs.tns")
    check(os.path.getsize(slot0) > 400000, "save manager: U uncompressed slot 0 (%d bytes)" % os.path.getsize(slot0))
    check(os.path.getsize(slot1) < 100000, "save manager: slot 1 still compressed")
    check(r.shot("manager").box(20, 40) is not None, "save manager: its list")

    r2 = Run("save_manager2", reuse=r).run(start + open_manager + """press A
wait 0.2
press C
wait 0.5
""" + saves_tile(1) + """press d
wait 0.3
press esc
wait 0.3
shot kept
press d
wait 0.3
shot confirm
press right
press enter
wait 0.5
shot deleted
quit
""")
    check(r2.exit == 0 and os.path.getsize(r2.save("Pattern.gba1.svs.tns")) < 100000,
          "save manager: A then C compressed them all")
    check(r2.shot("confirm").ui == "dialog:0", "save manager: del asks, No picked: %s" % r2.shot("confirm").ui)
    check(not os.path.exists(r2.save("Pattern.gba0.svs.tns")), "save manager: del, Delete deleted slot 0")
    check(os.path.exists(r2.save("Pattern.gba1.svs.tns")), "save manager: only the one picked")


def test_saves_place():
    """UI17's Saves place, the data-safety findings: a marks the states only (never the game's own
    save), so a then del keeps that save; the game's own save is deleted alone, after its own question, its
    stray .tmp with it; a state of the paused game picked in the store's Saves is loaded in place (no leaving:
    Save when I quit can't write over it first); a game of the same name in another folder (another game
    code) shows its own saves, never the paused one's."""
    def set_code(path, code):
        with open(path, "r+b") as f:
            f.seek(0xAC)
            f.write(code)
    r = Run("saves_place", roms=("Zelda",))
    set_code(r.file("Zelda.gba.tns"), b"AAAE")
    os.makedirs(os.path.join(r.gba, "B"))
    pattern_rom(os.path.join(r.gba, "B", "Zelda.gba.tns"))
    set_code(os.path.join(r.gba, "B", "Zelda.gba.tns"), b"BBBE")
    r.run("wait 1\npress enter\nwait 1.5\npress S\nwait 0.5\npress S\nwait 0.5\npress S\nwait 0.5\npress Q\nwait 1\nquit\n")
    os.makedirs(os.path.dirname(r.save("x")), exist_ok=True)
    with open(r.save("Zelda.gba.sav.tns"), "wb") as f:
        f.write(bytes(range(256)) * 128)
    states = sorted(f for f in os.listdir(os.path.dirname(r.save("x"))) if f.endswith(".svs.tns"))
    check(len(states) >= 3, "saves place: the states made: %s" % states)
    older = r.save(states[0])
    before = open(older, "rb").read()

    # the paused game's oldest state picked from the store: loaded in place, nothing written over it. (a: from
    # GAMES' S on the paused game, "ZELDA / SAVES": its own save first, the states after; b: from the paused menu
    # as it opens, SAVES: Resume, Save, its own save, the states.) Left and right stop at a row's ends: down first.
    start = "wait 1\npress enter\nwait 1.5\npress menu\nwait 0.3\n"
    n_old = len(states)
    tile = lambda i: "press down\n" * (i // 3) + "press right\n" * (i % 3)
    a = Run("saves_place_pick", reuse=r).run(start + to_main("games") + "press enter\nwait 1\npress s\nwait 1\n"
                                            "shot place\n" + tile(n_old) + "wait 0.5\nshot oldest\npress enter\n"
                                            "wait 1\nshot back\nquit\n")
    check(a.shot("place").ui.startswith("saves:"), "saves place: the store's s opens it: %s" % a.shot("place").ui)
    check(a.shot("back").ui == "", "saves place: enter loaded it, back in the game: %s" % a.shot("back").ui)
    after = sorted(f for f in os.listdir(os.path.dirname(a.save("x"))) if f.endswith(".svs.tns"))
    check(after == states and open(a.save(states[0]), "rb").read() == before,
          "saves place: the paused game's pick loaded in place, no state written: %s -> %s" % (states, after))
    a2 = Run("saves_place_pick2", reuse=r).run(start + "shot place\n" + saves_tile(n_old - 1, game_save=True) +
                                              "wait 0.5\nshot oldest\npress enter\nwait 1\nshot back\nquit\n")
    check(a2.shot("place").ui == "saves:0" and a2.shot("oldest").ui == "saves:%d" % (n_old + 2) and a2.shot("back").ui == "",
          "saves place: the paused menu's SAVES, the oldest state loaded in place: %s %s %s"
          % (a2.shot("place").ui, a2.shot("oldest").ui, a2.shot("back").ui))
    after = sorted(f for f in os.listdir(os.path.dirname(a2.save("x"))) if f.endswith(".svs.tns"))
    check(after == states and open(a2.save(states[0]), "rb").read() == before,
          "saves place: SAVES' pick loaded in place, no state written: %s -> %s" % (states, after))

    # the same name in folder B, another game code: its own (no) saves; a, del there deletes none of the paused one's
    b = Run("saves_place_other", reuse=r).run(start + to_main("games") + "press enter\nwait 1\npress right\nwait 0.3\n"
                                              "press enter\nwait 1\npress s\nwait 1\nshot place\npress a\n"
                                              "press d\nwait 0.4\npress right\npress enter\nwait 0.6\nquit\n")
    check(b.shot("place").ui == "saves:0", "saves place: folder B's Zelda opens on nothing of its own: %s"
          % b.shot("place").ui)
    left = sorted(f for f in os.listdir(os.path.dirname(b.save("x"))) if f.endswith(".svs.tns"))
    check(left == states and os.path.exists(b.save("Zelda.gba.sav.tns")),
          "saves place: folder B's Zelda (another code) left the paused one's alone: %s, sav %s"
          % (left, os.path.exists(b.save("Zelda.gba.sav.tns"))))

    # a then del: the states go, the game's own save stays; then it alone, its .tmp with it
    with open(r.save("Zelda.gba.sav.tns.tmp"), "wb") as f:
        f.write(bytes(range(256)) * 128)
    c = Run("saves_place_delete", reuse=r).run(start + "wait 0.5\npress a\npress d\nwait 0.4\n"
                                               "shot asked\npress right\npress enter\nwait 0.8\nshot all_gone\n"
                                               "press right\npress right\nwait 0.2\npress d\nwait 0.4\nshot asked_own\npress right\npress enter\nwait 0.6\n"
                                               "shot own_gone\nquit\n")
    check(c.shot("asked").ui == "dialog:0" and c.shot("asked_own").ui == "dialog:0",
          "saves place: both deletes asked: %s %s" % (c.shot("asked").ui, c.shot("asked_own").ui))
    check(c.shot("all_gone").ui == "saves:0", "saves place: after a, del: only Resume, Save and the game's own save are left: %s"
          % c.shot("all_gone").ui)
    check(not os.path.exists(c.save("Zelda.gba.sav.tns")) and not os.path.exists(c.save("Zelda.gba.sav.tns.tmp")),
          "saves place: the game's own save deleted alone, with its .tmp")


def questions(run):
    """The questions a run asked and how each was answered (nspire_dialog.c's link log lines): [(title, answer)]."""
    text = ""
    for name in ("gpsp_link_run.txt.tns", "gpsp_link.txt.tns"):
        path = run.save(name)
        if os.path.exists(path):
            text += open(path, "rb").read().decode("latin-1")
    return re.findall(r'ui: question "([^"]*)": (.*)', text)


def test_saves_place_marks():
    """ui18, the Saves place: m marks (or unmarks) the picked tile only, the cursor stays (it does not go on to
    the next); m held while the arrows move marks every tile the cursor comes to with
    what the first got (a sweep unmarks the same way); a sweep and a (mark all) pass the game's own save, never
    marked, so the marked ones deleted leave it; the GBA's A (del) answers the delete question as enter does, B
    (var) as esc; an answer is taken only once the key that asked it is let go."""
    r = Run("saves_marks", roms=("Zelda",))
    r.run("wait 1\npress enter\nwait 1.5\n" + "press S\nwait 0.5\n" * 6 + "press Q\nwait 1\nquit\n")
    with open(r.save("Zelda.gba.sav.tns"), "wb") as f:
        f.write(bytes(range(256)) * 128)
    def states(run=r):
        d = os.path.dirname(run.save("x"))
        return sorted((int(f[len("Zelda.gba"):-len(".svs.tns")]) for f in os.listdir(d)
                       if f.startswith("Zelda.gba") and f.endswith(".svs.tns")), reverse=True)
    before = states()
    check(len(before) >= 6, "marks: the states made: %s" % before)
    # (r19: the tiles are Resume 0, Save 1, the game's own save 2, then the states newest first from 3, three across, left
    # and right stopping at a row's ends: the sweeps go along row 1 (3 4 5), then up and down)
    place = "wait 1\npress enter\nwait 1.5\npress menu\nwait 0.8\npress down\nwait 0.2\n"
    a = Run("saves_marks_a", reuse=r).run(place + """press m
wait 0.2
shot m_one
press d
wait 0.4
press esc
wait 0.3
press m
wait 0.2
hold m
wait 0.2
press right
press right
press down
wait 0.2
release m
wait 0.2
shot swept
press d
wait 0.4
press esc
wait 0.3
hold m
wait 0.2
press up
wait 0.2
release m
wait 0.2
press d
wait 0.4
press esc
wait 0.3
hold m
wait 0.2
press left
press left
press up
press right
press right
wait 0.2
release m
wait 0.2
shot on_own
press d
wait 0.4
shot asked
press right
press del
wait 0.8
press a
wait 0.2
press d
wait 0.4
press var
wait 0.4
shot answered_b
quit
""")
    check(a.shot("m_one").ui == "saves:3", "marks: m leaves the cursor where it is: %s" % a.shot("m_one").ui)
    check(a.shot("swept").ui == "saves:8", "marks: the sweep's cursor: %s" % a.shot("swept").ui)
    asked = questions(a)
    want = [("Delete slot %d?" % (before[0] + 1), "esc"), ("Delete these 4 saves?", "esc"),
            ("Delete these 2 saves?", "esc"), ("Delete these 3 saves?", "Delete"),
            ("Delete these %d saves?" % (len(before) - 3), "esc")]
    check(asked == want, "marks: a tap marks one, a sweep four, a sweep back unmarks two, one over the game's own "
          "save marks three (not it), a all the states; A (del) answers Delete, B (var) esc: %s" % asked)
    check(a.shot("on_own").ui == "saves:2" and a.shot("asked").ui == "dialog:0" and
          a.shot("answered_b").ui.startswith("saves:"),
          "marks: on the game's own save, its question, B back: %s %s %s"
          % (a.shot("on_own").ui, a.shot("asked").ui, a.shot("answered_b").ui))
    check(states(a) == before[3:] and os.path.exists(a.save("Zelda.gba.sav.tns")),
          "marks: the three newest states deleted, the game's own save kept: %s -> %s" % (before, states(a)))

    # the key that asked held: enter while del is still down doesn't answer; let go, it does
    held = states(a)
    b = Run("saves_marks_held", reuse=a).run(place + """hold d
wait 0.4
press right
press enter
wait 0.4
shot held
release d
wait 0.3
shot let_go
press enter
wait 0.8
quit
""")
    check(b.shot("held").ui == "dialog:1" and b.shot("let_go").ui == "dialog:1",
          "marks: no answer while the del that asked is held: %s %s" % (b.shot("held").ui, b.shot("let_go").ui))
    check(questions(b) == [("Delete slot %d?" % (held[0] + 1), "Delete")] and states(b) == held[1:],
          "marks: let go, enter answers: %s, %s -> %s" % (questions(b), held, states(b)))


def test_confirm_delete():
    """ui18, Settings > Saving > Confirm delete (On by default): Off, the Saves place deletes states at once, the
    game's own save still asks; kept in word 71's bit 12 (older builds read the file as before: its size and every
    other word the same); a settings file r17 wrote (no such bit) loads with it On and every value unchanged."""
    r = Run("confirm_delete", roms=("Zelda",))
    r.run("wait 1\npress enter\nwait 1.5\n" + "press S\nwait 0.5\n" * 3 + "press Q\nwait 1\nquit\n")
    with open(r.save("Zelda.gba.sav.tns"), "wb") as f:
        f.write(bytes(range(256)) * 128)
    count = lambda run: len([f for f in os.listdir(os.path.dirname(run.save("x"))) if f.endswith(".svs.tns")])
    words_on = read_words(r.save("gpsp.cfg.tns"))
    check(len(words_on) == 82 and not words_on[71] & 0x1000, "confirm delete: On by default, bit 12 clear: %08x"
          % (words_on[71] if len(words_on) > 71 else 0))
    states_before = count(r)
    # (r19: Ask to delete has no tile any more: the settings file says it, bit 12 of word 71)
    off_words = list(words_on)
    off_words[71] |= 0x1000
    off = Run("confirm_delete_off", reuse=r)
    settings_from_file(off, off_words)
    off.run(
        "wait 1\npress enter\nwait 1.5\npress menu\nwait 0.8\n"
        "press down\nwait 0.8\npress d\nwait 0.6\n"
        "shot deleted\npress up\npress right\npress right\nwait 0.2\npress d\nwait 0.4\nshot own\n"
        "press esc\nwait 0.3\npress esc\nwait 0.3\npress esc\nwait 0.5\nquit\n")
    check(off.shot("deleted").ui.startswith("saves:") and count(off) == states_before - 1,
          "confirm delete off: a state deleted at once, no question: %s, %d -> %d states"
          % (off.shot("deleted").ui, states_before, count(off)))
    check(off.shot("own").ui == "dialog:0" and os.path.exists(off.save("Zelda.gba.sav.tns")),
          "confirm delete off: the game's own save still asks (kept on No): %s" % off.shot("own").ui)
    check([t for t, _ in questions(off)] == ["Delete Zelda's own save?"], "confirm delete off: one question, the "
          "game's own save's: %s" % questions(off))
    words_off = read_words(off.save("gpsp.cfg.tns"))
    check(len(words_off) == 82 and words_off[71] == words_on[71] | 0x1000 and
          words_off[:71] + words_off[72:] == words_on[:71] + words_on[72:],
          "confirm delete off: kept as word 71's bit 12, the file's size and every other word as before")
    # r17's file (its words as this build writes them with Confirm delete On, some values not the defaults)
    old = list(words_on)
    old[71] = 0x80000000 | 3 | (0 << 4)    # Autosave every 2 min, Autosave to the picked slot
    old[66] = 0                            # Compress saves off
    old[63] = 4                            # Brightness 4 of 10
    r17 = Run("confirm_delete_r17", roms=("Zelda",), files={"gpsp.cfg.tns": old}).run(
        "wait 1\npress enter\nwait 1.5\npress S\nwait 0.5\npress menu\nwait 0.8\n" + saves_tile(0) +
        "press d\nwait 0.4\nshot asked\n" + "press esc\nwait 0.4\n" * 3 +
        "press Q\nwait 1\nquit\n")
    check(r17.shot("asked").ui == "dialog:0", "r17's settings: Confirm delete On (del asks): %s" % r17.shot("asked").ui)
    check(read_words(r17.save("gpsp.cfg.tns")) == old, "r17's settings: written back unchanged, every value as it was")


def test_saves_place_other_save():
    """UI17: another game's own save deleted in the store's Saves (a game is paused) takes its .tmp and
    .rollback with it, so the next start of that game finds no save to bring back (recover_tmp)."""
    r = Run("saves_other_save", roms=("Alpha", "Beta"))
    saves = os.path.join(r.dir, "zgpSP_files")
    os.makedirs(saves, exist_ok=True)
    names = ("Beta.gba.sav.tns", "Beta.gba.sav.tns.tmp", "Beta.gba.sav.tns.rollback")
    for n in names:
        with open(os.path.join(saves, n), "wb") as f:
            f.write(bytes(range(256)) * 128)
    a = Run("saves_other_save_del", reuse=r).run(
        "wait 1\npress enter\nwait 1.5\npress menu\nwait 0.3\n" + to_main("games") + "press enter\nwait 1\n"
        "press right\nwait 0.3\npress s\nwait 1\nshot place\npress d\nwait 0.4\nshot asked\npress right\n"
        "press enter\nwait 0.6\nshot deleted\npress esc\nwait 0.5\npress esc\nwait 0.5\npress Q\nwait 1\nquit\n")
    check(a.shot("asked").ui == "dialog:0", "other save: del on its own save asks: %s" % a.shot("asked").ui)
    left = [n for n in names if os.path.exists(a.save(n))]
    check(not left, "other save: deleted with its .tmp and .rollback: left %s" % left)
    b = Run("saves_other_save_start", reuse=a).run("wait 1\npress right\npress enter\nwait 2\npress Q\nwait 1\nquit\n")
    left = [n for n in names + ("Beta.gba.sav.gz.tns",) if os.path.exists(b.save(n))]
    check(not left, "other save: its next start brings none back: %s" % left)


def test_every_menu_line():
    """Settings' every tile and More's every tile: the highlight is on it ("Settings:K", "More:K"), and its texts fit
    (the PC build complains on stderr about a text cut short, which fails the run): on Settings each setting is shown at
    every value (r19: a setting of a few values goes to its next value with each enter, all the way round; a range
    (Volume, Brightness, Autosave: the longest values among them) is put between chevrons, changed to its
    ends one step each way and put back by esc); More's tiles are walked in order, About opened and left. The
    screen is the same one the whole way: Settings' tiles under the top row, More one page behind its last tile."""
    ranges = {"Volume": 20, "Brightness": 10, "Autosave": 8}
    rounds = {"Screen size": 3, "FPS counter": 3}
    tiles, more = set_tiles(), more_tiles()
    lines = ["wait 1", "press enter", "wait 1.5", "press menu", "wait 0.3", to_main("settings").strip(), "shot row",
             "press down", "wait 0.2"]
    for i, name in enumerate(tiles):
        lines += ["shot s%02d" % i]
        if name in ranges:
            lines += ["press enter", "wait 0.15", "shot s%02d_editing" % i] + ["press left"] * ranges[name] + \
                     ["wait 0.15", "shot s%02d_low" % i] + ["press right"] * (2 * ranges[name]) + \
                     ["wait 0.15", "shot s%02d_high" % i, "press esc", "wait 0.15", "shot s%02d_back" % i]
        elif name in rounds:
            for k in range(rounds[name]):
                lines += ["press enter", "wait 0.15", "shot s%02d_value%d" % (i, k)]
        if name != "More":
            lines += ["press right", "wait 0.15"]
    lines += ["press enter", "wait 0.4"]
    for i, name in enumerate(more):
        lines += ["shot m%02d" % i, "press right", "wait 0.12"]
    lines += ["shot wrapped", "press left", "press enter", "wait 0.3", "shot about", "press esc", "wait 0.3", "shot out_about",
              "press esc", "wait 0.3", "shot out_more", "press esc", "wait 0.3", "shot out_row", "quit"]
    r = Run("every_menu_line").run("\n".join(lines) + "\n")
    check(r.shot("row").ui == "main:%d" % MAIN["settings"], "every menu line: SETTINGS on the row: %s" % r.shot("row").ui)
    check(len(tiles) <= 12 and len(tiles) == len(set(tiles)), "every menu line: Settings has %d tiles, at most 12 (four rows of three)" % len(tiles))
    for i, name in enumerate(tiles):
        check(r.shot("s%02d" % i).ui == "Settings:%d" % i, "every menu line: the highlight on %s: %s" % (name, r.shot("s%02d" % i).ui))
        if name in ranges:
            same = [r.shot("s%02d_%s" % (i, k)).ui for k in ("editing", "low", "high", "back")]
            check(same == ["Settings:%d" % i] * 4 and r.shot("s%02d_low" % i).pixels != r.shot("s%02d_high" % i).pixels and
                  r.shot("s%02d_back" % i).pixels == r.shot("s%02d" % i).pixels,
                  "every menu line: %s edited from end to end stays picked, esc puts it back: %s" % (name, same))
        elif name in rounds:
            shots = [r.shot("s%02d_value%d" % (i, k)) for k in range(rounds[name])]
            check(all(sh.ui == "Settings:%d" % i for sh in shots) and len(set(sh.pixels for sh in shots)) == rounds[name],
                  "every menu line: %s's %d values each shown, the tile stays picked: %s" % (name, rounds[name], [sh.ui for sh in shots]))
    seen = [r.shot("m%02d" % i).ui for i in range(len(more))]
    check(seen == ["More:%d" % i for i in range(len(more))], "every menu line: More highlights %s" % seen)
    check(len(more) <= 24 + 1, "every menu line: More has %d tiles" % len(more))
    check(r.shot("wrapped").ui == "More:0", "every menu line: right of More's last tile goes round: %s" % r.shot("wrapped").ui)
    check(r.shot("about").ui == "About:0", "every menu line: About: %s" % r.shot("about").ui)
    check(r.shot("out_about").ui == shown_at("about"), "every menu line: esc, on About: %s" % r.shot("out_about").ui)
    check(r.shot("out_more").ui == "Settings:%d" % tiles.index("More") and r.shot("out_row").ui == "main:%d" % MAIN["settings"],
          "every menu line: esc from More: its tile on Settings, esc: the row: %s %s" % (r.shot("out_more").ui, r.shot("out_row").ui))


def test_rom_manager_leftovers():
    """ZIP-5: opening the ROM manager removes the .gbz/.gba temporary files
    a cut conversion left, and no other .tmp file."""
    r = Run("rom_manager_tmp", roms=("Alpha",))
    for name in ("Cut.gbz.tns.tmp", "Cut2.gba.tns.tmp", "Other.sav.tns.tmp"):
        with open(r.file(name), "wb") as f:
            f.write(b"x" * 100)
    r.run("wait 1\n" + NO_GAME_ROM_MANAGER + "quit\n")
    files = sorted(os.listdir(r.gba))
    check(files == sorted([PROGRAM, "Alpha.gba.tns", "Other.sav.tns.tmp"]),
          "rom manager: cut conversions' .tmp files removed, others stay: %s" % files)


def test_link_setting():
    """Settings > Link > Device: auto, Wireless adapter, link cable and
    link cable (exact), whose name fits (enter is the next one); the choice is kept (word 68)."""
    r = Run("link_setting").run("wait 1\npress enter\nwait 1.5\npress menu\nwait 0.3\n" + to_advanced("Link") +
                                cycle(3) + "shot exact\npress esc\nwait 0.3\n"
                                "press esc\npress esc\nwait 1\nquit\n")
    check(r.shot("exact").ui == shown_at("Link"), "link setting: on the Link line: %s" % r.shot("exact").ui)
    words = read_words(r.save("gpsp.cfg.tns"))
    check(words[68] == 3, "link setting: link cable (exact) kept as 3: %d" % words[68])


def test_link_page():
    """r19, one screen of settings: linking is Settings' LINK tile (a game paused; no Link tab, no Link page): its
    value is the link's state; without a game there is no such tile (the tiles go round: left of the first is More,
    the last), and Link device and Link speed are More's."""
    r = Run("link_page").run("wait 1\npress enter\nwait 1.5\npress menu\nwait 0.3\n" + to_setting("link") +
                            "wait 0.3\nshot with_game\npress right\nwait 0.2\nshot more\n"
                            "press right\npress left\npress left\nwait 0.2\nshot before\nquit\n")
    tiles = set_tiles()
    check(r.shot("with_game").ui == "Settings:%d" % tiles.index("Link") and
          r.shot("more").ui == "Settings:%d" % tiles.index("More") and r.shot("before").ui == "Settings:%d" % tiles.index("Link"),
          "link page: with a game the Link tile is before More: %s %s %s"
          % (r.shot("with_game").ui, r.shot("more").ui, r.shot("before").ui))
    check("Link device" in PAGES["more"] and "Link" in PAGES["settings"],
          "link page: Link is a tile of Settings, Link device is More's")
    n = Run("link_page_none").run("wait 1\npress esc\nwait 0.3\npress left\npress down\nwait 0.3\nshot first\n"
                                 "press left\nwait 0.3\nshot last\nquit\n")
    none = set_tiles(False)
    check(n.shot("first").ui == "Settings:0" and n.shot("last").ui == "Settings:%d" % (len(none) - 1) and
          len(none) == len(tiles) - 1,
          "link page: without a game no Link tile (%d tiles, the last More): %s %s" % (len(none), n.shot("first").ui, n.shot("last").ui))
    check(r.shot("with_game").pixels != n.shot("first").pixels, "link page: it looks different without a game")


def test_settings_tiles():
    """r19, one screen of settings: with the block on SETTINGS ("main:0") its tiles are drawn under the row, none
    picked; down or enter puts the arrows in them ("Settings:K"), up from the first row of tiles or esc goes back to
    the row; coming back in the arrows are on the tile they left (reset when the menu is opened again); left and right
    go to the tile before/after in order and go round, up and down by row (down from the last row goes round, up from
    the first goes to the row). With a game there are ten tiles (Link is one), three across."""
    tiles = set_tiles()
    n = len(tiles)
    r = Run("settings_tiles").run("""wait 1
press enter
wait 1.5
press menu
wait 0.6
press up
press left
wait 0.3
shot row
press down
wait 0.2
shot in_down
press up
wait 0.2
shot out_up
press enter
wait 0.2
shot in_enter
press right
press right
wait 0.2
shot third
press esc
wait 0.3
shot out_esc
press down
wait 0.2
shot remembered
press down
wait 0.2
shot row2
press down
wait 0.2
shot row3
press down
wait 0.2
shot row4
press down
wait 0.2
shot wrapped_down
press up
wait 0.2
shot up_from_first
press down
press left
wait 0.2
shot wrapped_left
press right
wait 0.2
shot wrapped_right
press esc
wait 0.3
press esc
wait 1
press menu
wait 0.6
press up
press left
wait 0.3
press down
wait 0.2
shot reopened
quit
""")
    ui = lambda name: r.shot(name).ui
    row = "main:%d" % MAIN["settings"]
    check(ui("row") == row and ui("in_down") == "Settings:0" and ui("out_up") == row and ui("in_enter") == "Settings:0",
          "settings tiles: down and enter go in (the first tile), up from the first row goes out: %s %s %s %s"
          % (ui("row"), ui("in_down"), ui("out_up"), ui("in_enter")))
    check(r.shot("row").pixels != r.shot("in_down").pixels,
          "settings tiles: on the row the tiles are drawn with none picked, going in picks one")
    check(ui("third") == "Settings:2" and ui("out_esc") == row and ui("remembered") == "Settings:2",
          "settings tiles: esc goes back to the row, down comes back to the tile left: %s %s %s"
          % (ui("third"), ui("out_esc"), ui("remembered")))
    check([ui("row2"), ui("row3"), ui("row4"), ui("wrapped_down")] == ["Settings:5", "Settings:8", "Settings:%d" % (n - 1), "Settings:%d" % ((n - 1) % 3)],
          "settings tiles: down by row (the last row's shorter: its last tile), then round to the first row: %s"
          % [ui("row2"), ui("row3"), ui("row4"), ui("wrapped_down")])
    check(ui("up_from_first") == row, "settings tiles: up from the first row goes to the row: %s" % ui("up_from_first"))
    check(ui("wrapped_left") == "Settings:%d" % (n - 1) and ui("wrapped_right") == "Settings:0",
          "settings tiles: left of the first is the last (More), right of the last the first: %s %s"
          % (ui("wrapped_left"), ui("wrapped_right")))
    check(ui("reopened") == "Settings:0", "settings tiles: opened again, the arrows start on the first tile: %s" % ui("reopened"))


def test_volume_tile():
    """r19: Volume is Settings' first tile, a range of 0/20 to 20/20: enter, left/right (stopping at the ends), enter keeps
    it; esc puts it back; the value is stored (word 3: bit 21, 15 in bits 16-20 after 14 and one step down)."""
    r = Run("volume_tile").run("""wait 1
press enter
wait 1.5
press menu
wait 0.5
""" + to_setting("volume") + """wait 0.2
shot start
press enter
""" + "press left\n" * 14 + """wait 0.2
shot zero
press left
press left
press left
press left
press left
press left
wait 0.2
shot zero_again
""" + "press right\n" * 20 + """wait 0.2
shot max
press right
press right
wait 0.2
shot max_again
press esc
wait 0.2
shot restored
press enter
press left
wait 0.2
shot thirteen
press enter
wait 0.2
shot kept
press esc
press esc
wait 0.5
press Q
wait 1
quit
""")
    s = r.shot
    check(s("start").ui == "Settings:0" and s("zero").ui == "Settings:0",
          "volume tile: on Settings' first tile: %s %s" % (s("start").ui, s("zero").ui))
    check(s("zero").pixels == s("zero_again").pixels and s("max").pixels == s("max_again").pixels and
          s("zero").pixels != s("max").pixels, "volume tile: stops at 0 and at 20")
    check(s("restored").pixels == s("start").pixels, "volume tile: esc puts the 14 back")
    check(s("thirteen").pixels != s("start").pixels and s("kept").pixels != s("start").pixels and s("kept").ui == "Settings:0",
          "volume tile: left and enter keep 13")
    words = read_words(r.save("gpsp.cfg.tns"))
    check(words[3] & (1 << 21) and (words[3] >> 16) & 0x1F == 13, "volume tile: 13 stored in word 3 (bit 21, bits 16-20): %08x" % words[3])


def more_walk(name, tiles, game=True, files=None, options=()):
    """A run that opens More and goes right through it twice round: the report of each tile's place."""
    if game:
        pre = "wait 1\npress enter\nwait 1.5\npress menu\nwait 0.6\n" + to_setting("more")
    else:
        pre = "wait 1\npress esc\nwait 0.3\npress left\npress down\n" + page_moves(0, set_tiles(False).index("More"))
    lines = [pre.strip(), "press enter", "wait 0.4"]
    for i in range(len(tiles) + 1):
        lines += ["shot m%02d" % i, "press right", "wait 0.12"]
    lines += ["press left", "press left", "wait 0.2", "shot last", "press esc", "wait 0.3", "shot back", "quit"]
    r = Run(name, files=files).run("\n".join(lines) + "\n", options=options)
    seen = [r.shot("m%02d" % i).ui for i in range(len(tiles) + 1)]
    check(seen == ["More:%d" % i for i in range(len(tiles))] + ["More:0"],
          "%s: More has %d tiles %s, in order, going round: %s" % (name, len(tiles), tiles, seen))
    check(r.shot("last").ui == "More:%d" % (len(tiles) - 1),
          "%s: left of the first tile is the last, About: %s" % (name, r.shot("last").ui))
    back = set_tiles(game)
    check(r.shot("back").ui == "Settings:%d" % back.index("More"),
          "%s: esc from More: Settings' tiles with More picked: %s" % (name, r.shot("back").ui))
    return r


def test_more_tiles():
    """r19, More: one screenful, nothing scrolls: Save to, [Autosave to: only with Autosave on], [Saves on: with a
    drive], Link device, Settings for, Keys for, [LCD fix: older CX only], [Game: with a game], Saves, Games, About. Every tile is reached; the hidden ones are hidden in each condition made here (no game; Autosave
    off; a CX II); esc goes back to Settings with More picked; About from More and back. (Saves on needs a USB drive,
    which this file's runs don't have: drive_save_tests.py has it.)"""
    cfg = owner_config()
    cfg[71] = 0x80000000 | 1 | (0 << 4)    # Autosave every 30 s, to the picked slot
    more_walk("more_game", more_tiles())
    more_walk("more_autosave", more_tiles(autosave=True), files={"gpsp_qol.cfg.tns": cfg})
    more_walk("more_no_game", more_tiles(False), game=False)
    more_walk("more_cx", more_tiles(cx2=False), options=["--model", "cx"])
    check("Autosave to" not in more_tiles() and "Saves on" not in more_tiles() and "LCD fix" not in more_tiles() and
          "Game" not in more_tiles(False),
          "more tiles: the hidden ones are hidden in the default list")
    r = Run("more_about").run("wait 1\npress enter\nwait 1.5\npress menu\nwait 0.5\n" + to_setting("about") +
                             "shot tile\npress enter\nwait 0.4\nshot about\npress esc\nwait 0.4\nshot back\nquit\n")
    check(r.shot("tile").ui == shown_at("about") and r.shot("about").ui == "About:0" and r.shot("back").ui == shown_at("about"),
          "more tiles: About from More and back to its tile: %s %s %s" % (r.shot("tile").ui, r.shot("about").ui, r.shot("back").ui))


def test_more_saves():
    """r19, More > Saves (Manage): with a game paused, enter opens that game's saves as a page (ui_shown "saves:K": the
    states newest first, no Resume/Save tiles), D deletes the picked one after its question, esc goes back to More with
    Saves picked, and enter on a state loads it and the game resumes from it; without a game enter opens nothing."""
    seed = Run("more_saves_seed")
    seed.run(REAL_COSTS + "wait 1\npress enter\nwait 1.5\n" + "press S\nwait 1\n" * 3 + "quit\n")
    folder = os.path.dirname(seed.save("x"))
    states = lambda run: sorted(f for f in os.listdir(os.path.dirname(run.save("x"))) if f.endswith(".svs.tns"))
    r = Run("more_saves", reuse=seed)
    before = states(r)
    r.run(REAL_COSTS + "wait 1\npress enter\nwait 2\npress menu\nwait 0.6\n" + to_setting("saves") + "shot tile\npress enter\nwait 0.5\n"
          "shot page\npress right\npress right\npress right\nwait 0.2\nshot last\npress left\npress left\npress d\nwait 0.4\nshot asked\n"
          "press right\npress enter\nwait 0.6\nshot deleted\npress esc\nwait 0.4\nshot back\npress enter\nwait 0.5\npress enter\nwait 1\n"
          "note loaded\nquit\n")
    after = states(r)
    check(r.shot("tile").ui == shown_at("saves") and r.shot("page").ui == "saves:0" and r.shot("last").ui == "saves:%d" % (len(before) - 1),
          "more saves: enter on Saves opens the game's saves, %d states, no Resume/Save tiles: %s %s %s"
          % (len(before), r.shot("tile").ui, r.shot("page").ui, r.shot("last").ui))
    gone = sorted(set(before) - set(after))
    check(r.shot("asked").ui == "dialog:0" and len(gone) == 1 and len(after) == len(before) - 1,
          "more saves: D asks, and deletes the picked state alone: %s %s" % (r.shot("asked").ui, gone))
    check(r.shot("back").ui == shown_at("saves"), "more saves: esc goes back to More with Saves picked: %s" % r.shot("back").ui)
    check(r.notes["loaded"]["ui"] == "" and any(t.startswith("Loaded slot") for t in osd_toasts(r)),
          "more saves: enter on a state loads it and the game resumes from it: %r %s" % (r.notes["loaded"]["ui"], osd_toasts(r)))
    n = Run("more_saves_none", roms=("Alpha",))
    n.run("wait 1\npress esc\nwait 0.3\npress left\npress down\n" + page_moves(0, set_tiles(False).index("More")) +
          "press enter\nwait 0.3\n" + page_moves(0, more_tiles(False).index("Saves")) + "shot tile\npress enter\nwait 0.4\nshot after\nquit\n")
    check(n.shot("tile").ui == "More:%d" % more_tiles(False).index("Saves") and n.shot("after").ui == n.shot("tile").ui and
          any(n.shot("after").at(x, y) != n.shot("tile").at(x, y) for y in range(222, 240) for x in range(250)),
          "more saves: without a game enter opens nothing, the band says where to press S: %s %s" % (n.shot("tile").ui, n.shot("after").ui))


def test_keys_all_cleared():
    """r19: with every key cleared the Keys list says "NO KEY" on each line (the lines "Brightness up" and "Brightness down"
    among them), all of it fitting (the PC build complains on stderr about a text cut short, which fails the run);
    Reset keys puts them back."""
    r = Run("keys_cleared").run("wait 1\npress enter\nwait 1.5\npress menu\nwait 0.6\n" + to_setting("keys") + "press enter\nwait 0.4\nshot start\n" +
                               "press del\nwait 0.1\npress down\nwait 0.1\n" * 22 + "shot cleared\n" + "press down\n" * 3 +
                               "wait 0.2\nshot bottom\npress up\npress up\npress up\nwait 0.2\npress esc\nwait 0.3\nshot back\nquit\n")
    check(r.shot("start").ui == "keys:0" and r.shot("start").pixels != r.shot("cleared").pixels and r.shot("bottom").ui.startswith("keys:"),
          "keys cleared: every line cleared, the list drawn: %s %s %s" % (r.shot("start").ui, r.shot("cleared").ui, r.shot("bottom").ui))
    check(r.shot("back").ui == "Settings:%d" % set_tiles().index("Keys"),
          "keys cleared: esc goes back to Settings with Keys picked: %s" % r.shot("back").ui)


def test_frame_skip_tile():
    """r19: Frame skip is More's first tile: enter cycles Auto and Off (Manual, from an older file, shows until it is
    changed and is never offered: from it enter goes to Off); the choice is stored (word 24: 0 Auto, 2 Off, 1 Manual)."""
    def run(name, kind, presses):
        cfg = owner_config()
        cfg[24], cfg[25], cfg[26] = kind, 3, 0xFF000001
        r = Run(name, files={"gpsp_qol.cfg.tns": cfg})
        r.run("wait 1\npress enter\nwait 1.5\npress menu\nwait 0.5\n" + to_setting("pace") + "wait 0.2\nshot tile\n" + cycle(presses) +
              "wait 0.3\nshot after\npress esc\npress esc\npress esc\nwait 1\npress Q\nwait 1\nquit\n")
        return r, read_words(r.save("gpsp.cfg.tns"))[24]
    auto, w_auto = run("fs_auto", 0, 0)
    off, w_off = run("fs_off", 2, 0)
    manual, w_manual = run("fs_manual", 1, 0)
    a1, s_a1 = run("fs_auto_1", 0, 1)
    a2, s_a2 = run("fs_auto_2", 0, 2)
    m1, s_m1 = run("fs_manual_1", 1, 1)
    o1, s_o1 = run("fs_off_1", 2, 1)
    check(auto.shot("tile").ui == shown_at("pace") and shown_at("pace") == "More:0",
          "frame skip: More's first tile: %s" % auto.shot("tile").ui)
    t = lambda r: r.shot("tile").pixels
    check(len({t(auto), t(off), t(manual)}) == 3, "frame skip: Auto, Off and Manual show as three different tiles")
    check((w_auto, w_off, w_manual) == (0, 2, 1), "frame skip: the file's values read back and kept: %s" % ((w_auto, w_off, w_manual),))
    check(s_a1 == 2 and a1.shot("after").pixels == t(off), "frame skip: enter on Auto: Off, stored (%d)" % s_a1)
    check(s_a2 == 0 and a2.shot("after").pixels == t(auto), "frame skip: a second enter: Auto again, Manual skipped (%d)" % s_a2)
    check(s_m1 == 2 and m1.shot("after").pixels == t(off), "frame skip: a file's Manual shows until enter, which goes to Off (%d)" % s_m1)
    check(s_o1 == 0 and o1.shot("after").pixels == t(auto), "frame skip: enter on Off: Auto (%d)" % s_o1)


def test_screen_size_values():
    """r19: Screen size has three values, one enter the next, round: Original (unscaled, word 0 = 0), Fit (scaled_aspect, 1:
    the smoothed picture) and Stretch (fullscreen, 2); a file's scaled_raw (3) shows as Fit (the tile is the same picture as with
    scaled_aspect) and one enter takes it to Stretch; each value is stored and read back; the band says a sentence of its
    own for the picked value."""
    def run(name, scale, presses, files=None):
        cfg = owner_config()
        cfg[0] = scale
        cfg[62] = 0   # (no FPS counter over the picture)
        r = Run(name, files={"gpsp_qol.cfg.tns": cfg} if files is None else files)
        r.run("wait 1\npress enter\nwait 1.5\npress menu\nwait 0.5\n" + to_setting("size") + "wait 0.2\nshot tile\n" +
              cycle(presses) + "wait 0.3\nshot after\npress esc\npress esc\nwait 1\npress Q\nwait 1\nquit\n")
        return r
    stored = {}
    for name, scale, presses, want in (("size_raw", 3, 1, 2), ("size_stretch", 2, 1, 0), ("size_original", 0, 1, 1),
                                       ("size_aspect", 1, 1, 2), ("size_round", 3, 3, 1)):
        r = run(name, scale, presses)
        stored[name] = (r, read_words(r.save("gpsp.cfg.tns"))[0])
        check(stored[name][1] == want, "screen size: from scale %d, %d enter(s): stored %d, want %d" %
              (scale, presses, stored[name][1], want))
    check(stored["size_aspect"][0].shot("tile").pixels == stored["size_raw"][0].shot("tile").pixels,
          "screen size: a file's scaled_raw shows as Fit, the tile the same as scaled_aspect's")
    check(stored["size_stretch"][0].shot("tile").pixels != stored["size_raw"][0].shot("tile").pixels and
          stored["size_original"][0].shot("tile").pixels != stored["size_raw"][0].shot("tile").pixels and
          stored["size_original"][0].shot("tile").pixels != stored["size_stretch"][0].shot("tile").pixels,
          "screen size: Original, Fit and Stretch are three different tiles")
    for name, box in (("size_raw", (0, 0, 319, 239)), ("size_stretch", (40, 40, 279, 199)), ("size_original", (0, 14, 319, 226))):
        again = Run(name + "_again", reuse=stored[name][0]).run("wait 1\npress enter\nwait 3\nshot game\nquit\n")
        check(again.shot("game").box() == box, "screen size: %s read back at the next start: %s" % (name, again.shot("game").box()))
    # the band says a sentence while the tile is picked, another for each value
    b = Run("size_band").run("wait 1\npress enter\nwait 1.5\npress menu\nwait 0.5\n" + to_setting("size") +
                            "wait 0.3\nshot b0\npress enter\nwait 0.3\nshot b1\npress enter\nwait 0.3\nshot b2\n"
                            "press right\nwait 0.3\nshot other\nquit\n")
    band = lambda sh: tuple(sh.at(x, y) for y in range(222, 240) for x in range(0, 250))
    check(len({band(b.shot(n)) for n in ("b0", "b1", "b2")}) == 3 and all(any(p != band(b.shot("other"))[0] for p in band(b.shot(n)))
          for n in ("b0", "b1", "b2")) and band(b.shot("b0")) != band(b.shot("other")),
          "screen size: the band has a sentence of its own for each of the three values, and another on another tile")


def test_fast_settings():
    """r19, the speed choices made for the player (nspire_config_fast): with GPSP_HOST_FAST in the environment (the
    calculator always) a settings file's frame mixing (off), RAM policy (Fast), blending (on), fast forward's top speed (none) and scaled_raw (it
    becomes scaled_aspect: Fit is the smoothed picture) are overridden, and the settings saved afterwards hold the fast values for exactly those
    words, every other word as it was; the picture is the one scaled_aspect gives; a game's own settings file the same.
    Without GPSP_HOST_FAST the file is honoured exactly."""
    CHANGED = {0, 30, 34, 35, 67}
    script = "wait 1\npress enter\nwait 2\nshot game\npress menu\nwait 0.6\npress esc\nwait 1\nshot after\npress Q\nwait 1\nquit\n"

    def base():
        o = owner_config()
        o[0] = 3                      # scaled_raw
        o[24], o[25], o[26] = 2, 1, 0xFF000001   # frame skip Off, chosen (the pace rule's mark), value 1
        o[29] = 1                     # game clock On
        o[30] = 1                     # frame mixing on
        o[34] = 1                     # blending off
        o[35] = 1                     # RAM policy Safe
        o[58], o[59], o[60] = 0, 1, 1  # save to same slot, save on quit, auto resume
        o[67] = 30                    # fast forward 3.0x
        o[70] = 0x80000000 | 0x40000000 | (4 << 8)   # turbo 5 a second
        return o

    manual = base()
    manual[24], manual[25] = 1, 0
    controls = {}
    for name, cfg in (("off", base()), ("manual", manual)):
        c = Run("fast_ctrl_" + name, files={"gpsp_qol.cfg.tns": cfg})
        c.run(REAL_COSTS + script)
        controls[name] = c
        w = read_words(c.save("gpsp.cfg.tns"))
        # (d) without GPSP_HOST_FAST the file is honoured exactly
        check(w[0] == 3 and w[24] == cfg[24] and w[30] == 1 and w[34] == 1 and w[35] == 1 and w[67] == 30,
              "fast settings, %s without GPSP_HOST_FAST: the file's values kept (scale %d, type %d, mixing %d, "
              "blending %d, RAM %d, fast forward %d)" % (name, w[0], w[24], w[30], w[34], w[35], w[67]))
        f = Run("fast_" + name, files={"gpsp_qol.cfg.tns": cfg})
        f.run(REAL_COSTS + script, env={"GPSP_HOST_FAST": "1"})
        fw = read_words(f.save("gpsp.cfg.tns"))
        check(fw[0] == 1 and fw[24] == w[24] and fw[25] == w[25] and fw[30] == 0 and fw[34] == 0 and fw[35] == 0 and fw[67] == 0,
              "fast settings, %s: saved as the fast values (scale %d, type %d, value %d, mixing %d, blending %d, RAM %d, "
              "fast forward %d)" % (name, fw[0], fw[24], fw[25], fw[30], fw[34], fw[35], fw[67]))
        other = [i for i in range(len(fw)) if i not in CHANGED and fw[i] != w[i]]
        check(not other, "fast settings, %s: every other word, frame skip type and value included, as without it (game clock, BIOS, turbo, save on quit, auto "
              "resume, save to, CPU speed ...): words %s differ" % (name, other))
        # the picture: scaled_aspect's, whatever the file asked
        raw = base()
        raw[0] = 1
        rr = Run("fast_raw_" + name, files={"gpsp_qol.cfg.tns": raw})
        rr.run(REAL_COSTS + script, env={"GPSP_HOST_FAST": "1"})
        pic = lambda run, shot: run.shot(shot).box(13, 240)   # (below the FPS counter's band)
        check(pic(f, "game") == pic(rr, "game") == (0, 14, 319, 226) and pic(f, "after") == pic(rr, "after") and
              pic(controls[name], "game") == (0, 13, 319, 226),
              "fast settings, %s: the picture is scaled_aspect's (0, 14, 319, 226), as asked for; without it the file's "
              "scaled_raw's: %s %s %s" % (name, pic(f, "game"), pic(rr, "game"), pic(controls[name], "game")))
    # a game's own settings file ("Settings for: This game"): frame mixing and scaling of its own
    for fast in (False, True):
        g = Run("fast_game_%s" % ("on" if fast else "off"))
        os.makedirs(os.path.dirname(g.save("x")), exist_ok=True)
        with open(g.save("Pattern.gba.settings.tns"), "w") as fh:
            fh.write("version=5\nown_settings=1\nframe_mixing=1\nscreen_scale=3\n")
        g.run(REAL_COSTS + script, env={"GPSP_HOST_FAST": "1"} if fast else None)
        text = open(g.save("Pattern.gba.settings.tns")).read()
        lines = dict(l.split("=", 1) for l in text.split() if "=" in l)
        check(lines.get("frame_mixing") == ("0" if fast else "1") and lines.get("screen_scale") == ("1" if fast else "3"),
              "fast settings, a game's own file %s GPSP_HOST_FAST: frame mixing and scaling saved as %s/%s"
              % ("with" if fast else "without", lines.get("frame_mixing"), lines.get("screen_scale")))
        check(g.shot("game").box() == ((0, 14, 319, 226) if fast else (0, 13, 319, 226)),
              "fast settings, a game's own file %s GPSP_HOST_FAST: the picture %s" % ("with" if fast else "without", g.shot("game").box()))


# ---- Cheats (r19: nspire_cheats.c: the game's RetroArch .cht file; docs/ui/r19.txt section 6) ----

def cht_code(i):
    """A CodeBreaker code writing black over a run of 12 pixels of the pattern ROM's mode 3 picture (a 16-bit write
    each, at 0x0600BC48 + 2 i: the pattern has no black pixel, so the picture shows whether it runs)."""
    return "+".join("8600%04X+0000" % (0xBC48 + 2 * (i * 40 + k)) for k in range(12))


def cht_text(codes, order=None, eol="\n", header=True):
    """A .cht file: codes is a list of (desc or None, code, enable ("true"/"false" or None: no enable line)), numbered
    from 0; order: the numbers in the order their lines come."""
    out = ["cheats = %d" % len(codes), ""] if header else []
    for n in (order if order is not None else range(len(codes))):
        d, c, e = codes[n]
        if d is not None:
            out.append('cheat%d_desc = "%s"' % (n, d))
        out.append('cheat%d_code = "%s"' % (n, c))
        if e is not None:
            out.append("cheat%d_enable = %s" % (n, e))
        out.append("")
    return eol.join(out) + (eol if eol == "\r\n" else "")


def blacks(shot):
    """Black pixels inside the picture (the mode 3 pattern has none)."""
    return sum(1 for y in range(20, 220) for x in range(320) if shot.at(x, y) == (0, 0, 0))


def cheat_calc(name, files=None, roms=("Pattern",)):
    """A calculator whose game's folder ("gba") and zgpSP_files ("saves") hold the files given: [(place, name, bytes or str)]."""
    r = Run(name, roms=roms)
    for place, fname, content in files or []:
        folder = r.gba if place == "gba" else os.path.dirname(r.save("x"))
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, fname), "wb") as f:
            f.write(content if isinstance(content, bytes) else content.encode("latin-1"))
    return r


CHEATS_TILE = set_tiles(cheats=True).index("Cheats")
OPEN_CHEATS = ("press menu\nwait 0.6\npress up\npress left\npress down\n" + "press right\n" * CHEATS_TILE +
               "press enter\nwait 0.3\n")   # from the running game to the Cheats page ("cheats:0")
START_GAME = "wait 1\npress enter\nwait 1.5\n"
LEAVE_CHEATS = "press esc\nwait 0.3\npress esc\nwait 0.3\npress esc\nwait 1\n"   # page, tiles, row: back in the game
GOOD = [("Code %d" % i, cht_code(i), "false") for i in range(30)]


def test_cheats_find():
    """A cheat file is found by name, in the game's folder first, then zgpSP_files: its Cheats tile (after Link, before
    More) is there, and the page lists its codes. No file, a file for another game ("Pattern 2.cht.tns", "Pattern
    Deluxe.cht.tns": only a bracket tag may follow the game's name), a file that is not a cheat file, an empty file and
    one over the limit: no tile."""
    two = cht_text(GOOD[:2])
    three = cht_text(GOOD[:3])
    not_cheats = "just some text\nnothing here\n"
    cases = [
        ("no file", [], 0), ("game's folder", [("gba", "Pattern.cht.tns", two)], 2),
        ("zgpSP_files", [("saves", "Pattern.cht.tns", three)], 3),
        ("both: the game's folder wins", [("gba", "Pattern.cht.tns", two), ("saves", "Pattern.cht.tns", three)], 2),
        ("a bracket tag", [("gba", "Pattern (Code Breaker).cht.tns", two)], 2),
        ("a tag in zgpSP_files", [("saves", "Pattern (Code Breaker).cht", three)], 3),
        ("another number", [("gba", "Pattern 2.cht.tns", two)], 0), ("a longer name", [("gba", "Pattern Deluxe.cht.tns", two)], 0),
        ("no space before the tag", [("gba", "Pattern(Code Breaker).cht.tns", two)], 0),
        ("upper case", [("gba", "PATTERN.CHT.TNS", two)], 2), ("lower case", [("gba", "pattern.cht", two)], 2),
        ("without .tns", [("gba", "Pattern.cht", three)], 3),
        ("the exact name before a tag", [("gba", "Pattern (Code Breaker).cht.tns", three), ("gba", "Pattern.cht.tns", two)], 2),
        ("not a cheat file", [("gba", "Pattern.cht.tns", not_cheats)], 0), ("an empty file", [("gba", "Pattern.cht.tns", "")], 0),
        ("over the limit", [("gba", "Pattern.cht.tns", two + "#" * (200 * 1024))], 0),
    ]
    for i, (what, files, want) in enumerate(cases):
        r = cheat_calc("cheats_find_%d" % i, files)
        r.run(START_GAME + "press menu\nwait 0.6\npress up\npress left\npress down\npress left\nshot tile\n" +
              ("press right\n" * (CHEATS_TILE + 1) + "press enter\nwait 0.3\nshot page\npress up\nwait 0.2\nshot last\n" if want else "") + "quit\n")
        has = r.shot("tile").ui == "Settings:%d" % (len(set_tiles(cheats=True)) - 1)
        check(has == bool(want), "cheats find, %s: the Cheats tile %s (the last tile is %s)" % (what, "shown" if has else "not shown", r.shot("tile").ui))
        if want:
            check(r.shot("page").ui == "cheats:0" and r.shot("last").ui == "cheats:%d" % (want - 1),
                  "cheats find, %s: the page lists %d codes: %s %s" % (what, want, r.shot("page").ui, r.shot("last").ui))
    plain = cheat_calc("cheats_find_none")
    plain.run(START_GAME + "press menu\nwait 0.6\npress up\npress left\npress down\npress left\nshot tile\nquit\n")
    check(plain.shot("tile").ui == "Settings:%d" % (len(set_tiles(cheats=False)) - 1),
          "cheats find: no file, Settings' tiles as before (the last, More): %s" % plain.shot("tile").ui)


def test_cheats_page():
    """The Cheats page: every kept code listed in the order of the file's N (a file whose lines come in another order
    too), a line a code, up/down going round, left/right ten lines (stopping at the ends), the scroll with 30 codes,
    names over 30 characters and with odd characters drawn with nothing on stderr (Run.finish checks it); esc goes back to
    Settings with Cheats picked."""
    names = ["A rather long name for a cheat that goes on and on and on", 'Quotes "inside" and \'single\'', "Tab\there and caf\xe9 \xc3\xa9",
             "100% {braces} [brackets] <angle> & more", "x" * 150, "UPPER lower 123 !@#$%^*()_+-=~`;:,.?/"]
    codes = [(names[i % len(names)], cht_code(i), "false") for i in range(30)]
    r = cheat_calc("cheats_page", [("gba", "Pattern.cht.tns", cht_text(codes))])
    r.run(START_GAME + OPEN_CHEATS + "shot p0\npress up\nshot up\npress down\nshot down0\npress right\nshot r1\npress right\nshot r2\n"
          "press right\nshot r3\npress right\nshot r4\npress left\nshot l1\npress left\npress left\npress left\nshot l4\n"
          "press down\nshot d\n" + "press down\n" * 9 + "shot scrolled\npress esc\nwait 0.3\nshot back\nquit\n")
    seen = [r.shot(n).ui for n in ("p0", "up", "down0", "r1", "r2", "r3", "r4", "l1", "l4", "d", "scrolled", "back")]
    check(seen == ["cheats:0", "cheats:29", "cheats:0", "cheats:10", "cheats:20", "cheats:29", "cheats:29", "cheats:19", "cheats:0",
                   "cheats:1", "cheats:10", "Settings:%d" % CHEATS_TILE],
          "cheats page: 30 codes: up wraps to the last, left/right ten at a time, stopping at the ends, esc back to the tile: %s" % seen)
    check(r.shot("p0").pixels != r.shot("scrolled").pixels and r.shot("p0").pixels != r.shot("r2").pixels,
          "cheats page: the list scrolls with the pick")
    # the lines come in another order: the page lists by N, so the first line is cheat 2 (flip it: its enable line)
    shuffled = [("Third", cht_code(2), "false"), ("First", cht_code(0), "false"), ("Second", cht_code(1), "false")]
    order = [2, 0, 1]
    o = cheat_calc("cheats_order", [("gba", "Pattern.cht.tns", cht_text(shuffled, order=order))])
    o.run(START_GAME + OPEN_CHEATS + "press enter\nwait 0.2\nshot first\npress down\nshot second\n" + LEAVE_CHEATS + "quit\n")
    text = open(o.file("Pattern.cht.tns")).read().split("\n")
    on = [l for l in text if l.startswith("cheat") and l.endswith("= true")]
    check(on == ["cheat0_enable = true"] and o.shot("second").ui == "cheats:1",
          "cheats page: the codes are listed in the order of N, not of the file's lines: %s" % on)


def test_cheats_effect():
    """A code's effect on the running game: a CodeBreaker write of black over a run of the pattern's pixels (it has none):
    on, the picture shows black; off in the file, the picture is the original; turned on in the page it shows; and turned
    off again then the game started again, the original picture (the pattern ROM never repaints those pixels, so a game
    that is still running keeps what the code wrote)."""
    text = cht_text(GOOD[:2])
    off = cheat_calc("cheats_effect_off", [("gba", "Pattern.cht.tns", text)])
    off.run(START_GAME + "shot game\nquit\n")
    check(blacks(off.shot("game")) == 0, "cheats effect: with the code off the picture has no black: %d" % blacks(off.shot("game")))
    r = cheat_calc("cheats_effect", [("gba", "Pattern.cht.tns", text)])
    r.run(START_GAME + "shot before\n" + OPEN_CHEATS + "press enter\nwait 0.2\n" + LEAVE_CHEATS + "wait 1\nshot on\n"
          + OPEN_CHEATS + "press enter\nwait 0.2\n" + LEAVE_CHEATS + "quit\n")
    check(blacks(r.shot("before")) == 0 and blacks(r.shot("on")) >= 12,
          "cheats effect: turned on in the page, back in the game the picture has the black write: %d -> %d" %
          (blacks(r.shot("before")), blacks(r.shot("on"))))
    check("cheat0_enable = false" in open(r.file("Pattern.cht.tns")).read(), "cheats effect: turned off again, the file says so")
    again = Run("cheats_effect_again", reuse=r)
    again.run(START_GAME + "shot game\nquit\n")
    check(blacks(again.shot("game")) == 0, "cheats effect: off, and the game started again: the original picture: %d" %
          blacks(again.shot("game")))


def test_cheats_persistence():
    """The switches are written back into the file: after flipping two codes and leaving the page exactly those two
    cheatN_enable lines are changed and every other byte is as it was; a file with no enable lines gets them appended; a
    file with CRLF line ends and one with no newline at its end both survive a flip intact; quitting and starting again,
    the codes are on and the effect is there from the first frame."""
    flip = START_GAME + OPEN_CHEATS + "press enter\nwait 0.2\npress down\npress down\npress enter\nwait 0.2\n" + LEAVE_CHEATS
    # 1. the plain file: codes 0 and 2 on
    text = cht_text(GOOD[:4])
    r = cheat_calc("cheats_persist", [("gba", "Pattern.cht.tns", text)])
    r.run(flip + "wait 0.5\nshot on\nquit\n")
    want = text.replace("cheat0_enable = false", "cheat0_enable = true").replace("cheat2_enable = false", "cheat2_enable = true")
    got = open(r.file("Pattern.cht.tns"), "rb").read().decode("latin-1")
    check(got == want, "cheats persistence: exactly the two enable lines changed, every other byte as it was")
    again = Run("cheats_persist_again", reuse=r)
    again.run("wait 1\npress enter\nwait 0.15\nshot first\nwait 1\nshot later\nquit\n")
    check(blacks(again.shot("first")) >= 12 and blacks(again.shot("later")) >= 12,
          "cheats persistence: started again, the effect is there from the first frame: %d %d" %
          (blacks(again.shot("first")), blacks(again.shot("later"))))
    check(open(again.file("Pattern.cht.tns"), "rb").read().decode("latin-1") == want, "cheats persistence: untouched by a start that changed nothing")
    # 2. no enable lines: appended
    bare = cht_text([(d, c, None) for d, c, _ in GOOD[:3]])
    b = cheat_calc("cheats_persist_bare", [("gba", "Pattern.cht.tns", bare)])
    b.run(flip + "quit\n")
    got = open(b.file("Pattern.cht.tns"), "rb").read().decode("latin-1")
    check(got.startswith(bare.rstrip("\n")) and got.endswith("cheat0_enable = true\ncheat1_enable = false\ncheat2_enable = true\n"),
          "cheats persistence: a file with no enable lines gets them appended: %r" % got[-100:])
    # 3. CRLF and no newline at the end
    crlf = cht_text(GOOD[:4], eol="\r\n")
    c = cheat_calc("cheats_persist_crlf", [("gba", "Pattern.cht.tns", crlf)])
    c.run(flip + "quit\n")
    want = crlf.replace("cheat0_enable = false", "cheat0_enable = true").replace("cheat2_enable = false", "cheat2_enable = true")
    check(open(c.file("Pattern.cht.tns"), "rb").read().decode("latin-1") == want, "cheats persistence: CRLF line ends survive a flip intact")
    nonl = cht_text(GOOD[:3]).rstrip("\n")
    n = cheat_calc("cheats_persist_nonl", [("gba", "Pattern.cht.tns", nonl)])
    n.run(START_GAME + OPEN_CHEATS + "press down\npress down\npress enter\nwait 0.2\n" + LEAVE_CHEATS + "quit\n")
    want = nonl[:nonl.rindex("false")] + "true"
    check(open(n.file("Pattern.cht.tns"), "rb").read().decode("latin-1") == want,
          "cheats persistence: a file with no newline at its end survives a flip of its last line intact")
    # 4. flipped and left by quitting the game with the page never left? (the game closing writes it too)
    q = cheat_calc("cheats_persist_quit", [("gba", "Pattern.cht.tns", text)])
    q.run(START_GAME + OPEN_CHEATS + "press enter\nwait 0.2\npress esc\npress esc\npress esc\nwait 1\npress Q\nwait 1\nquit\n")
    check("cheat0_enable = true" in open(q.file("Pattern.cht.tns")).read(), "cheats persistence: written back")


def test_cheats_limits():
    """Limits and bad input: 21 codes turned on (the 21st refused: the file has 20 on); a file with cheatN_enable = true
    on 25 codes starts with 20 on; 200 codes (160 kept); an Action Replay 8+8 code, a 9-digit address and an empty code left
    out, a missing desc kept (as "Code 3")."""
    many = [("C%d" % i, cht_code(i), "false") for i in range(30)]
    r = cheat_calc("cheats_21", [("gba", "Pattern.cht.tns", cht_text(many))])
    r.run(START_GAME + OPEN_CHEATS + "press enter\npress down\n" * 21 + "wait 0.2\n" + LEAVE_CHEATS + "quit\n")
    text = open(r.file("Pattern.cht.tns")).read()
    on = [l for l in text.split("\n") if l.endswith("_enable = true")]
    check(len(on) == 20 and "cheat20_enable = false" in text, "cheats limits: 21 turned on, the 21st refused: %d on" % len(on))
    allon = [("C%d" % i, cht_code(i), "true") for i in range(25)]
    s = cheat_calc("cheats_25", [("gba", "Pattern.cht.tns", cht_text(allon))])
    s.run(START_GAME + "shot game\n" + OPEN_CHEATS + "press down\n" * 24 + "press enter\nwait 0.2\nshot refused\n"
          "press up\npress up\npress up\npress up\npress up\npress up\npress up\npress up\npress up\npress up\n"
          "press up\npress up\npress up\npress up\npress up\npress up\npress up\npress up\npress up\npress up\n"
          "press up\npress up\npress up\npress up\npress enter\nwait 0.2\n" + LEAVE_CHEATS + "quit\n")
    left = [l for l in open(s.file("Pattern.cht.tns")).read().split("\n") if l.endswith("_enable = true")]
    check(len(left) == 19, "cheats limits: 25 enable lines true: 20 start on (the 21st-25th refused: one flipped off leaves 19 true): %d" % len(left))
    # 200 codes, 160 kept
    big = cheat_calc("cheats_200", [("gba", "Pattern.cht.tns", cht_text([("C%d" % i, cht_code(i % 40), "false") for i in range(200)]))])
    big.run(START_GAME + OPEN_CHEATS + "press up\nwait 0.2\nshot last\nquit\n")
    check(big.shot("last").ui == "cheats:159", "cheats limits: 200 codes, 160 kept: %s" % big.shot("last").ui)
    # bad codes: kept A, a missing desc and B; the 8+8, the 9-digit address and the empty one left out
    bad = [("A", cht_code(0), "false"), ("AR", "12345678 9ABCDEF0", "false"), ("Nine", "860000000+0000", "false"),
           ("Empty", "", "false"), (None, cht_code(1), "false"), ("B", cht_code(2), "false"), ("Junk", "ZZZZZZZZ ZZZZ", "false")]
    d = cheat_calc("cheats_bad", [("gba", "Pattern.cht.tns", cht_text(bad))])
    d.run(START_GAME + OPEN_CHEATS + "press up\nwait 0.2\nshot last\npress down\npress down\npress enter\nwait 0.2\n" + LEAVE_CHEATS + "quit\n")
    check(d.shot("last").ui == "cheats:2", "cheats limits: only the three runnable codes listed: %s" % d.shot("last").ui)
    check("cheat4_enable = true" in open(d.file("Pattern.cht.tns")).read(),
          "cheats limits: the code with no desc is kept, and its enable line is its own N's (4): the third listed")
    for name, content in (("a file that is all bad codes", cht_text([("AR", "12345678 9ABCDEF0", "false")])),):
        x = cheat_calc("cheats_allbad", [("gba", "Pattern.cht.tns", content)])
        x.run(START_GAME + "press menu\nwait 0.6\npress up\npress left\npress down\npress left\nshot tile\n"
              "press left\nwait 0.2\npress enter\nwait 0.4\nshot entered\nquit\n")
        # a file with no code the engine runs still has its tile (it says "NONE USABLE"; without it the player is left
        # wondering why the file does nothing): the last tile is More, the one before it Cheats; enter there opens
        # no page (the band says CodeBreaker codes only)
        tiles = len(set_tiles(cheats=True))
        check(x.shot("tile").ui == "Settings:%d" % (tiles - 1),
              "cheats limits: %s: its tile is shown: %s" % (name, x.shot("tile").ui))
        check(x.shot("entered").ui == "Settings:%d" % (tiles - 2),
              "cheats limits: %s: enter on its tile opens no page: %s" % (name, x.shot("entered").ui))


def test_cheats_other_game():
    """Another game started from GAMES while one with a cheat on is paused: the new game has no codes on (its picture
    is unaffected) and its own file is the one listed; going back to the first game its switch is as it was left."""
    a_text = cht_text([("A", cht_code(0), "false")])
    b_text = cht_text([("B1", cht_code(5), "false"), ("B2", cht_code(6), "false")])
    r = cheat_calc("cheats_two", [("gba", "Alpha.cht.tns", a_text), ("gba", "Pattern.cht.tns", b_text)], roms=("Alpha", "Pattern"))
    switch = "press menu\nwait 0.6\n" + to_main("game_list") + "press enter\nwait 0.6\n%swait 2\n"
    r.run(START_GAME + OPEN_CHEATS + "press enter\nwait 0.2\n" + LEAVE_CHEATS + "wait 1\nshot a_on\n" + switch % pick(2) +
          "shot b\nnote b_game\n" + OPEN_CHEATS + "press up\nwait 0.2\nshot b_last\n" + LEAVE_CHEATS +
          switch % pick(1) + "note a_back\n" + OPEN_CHEATS + "press enter\nwait 0.2\n" + LEAVE_CHEATS + "quit\n")
    check(r.notes["b_game"]["game"] == "Pattern.gba.tns" and blacks(r.shot("a_on")) >= 12 and blacks(r.shot("b")) == 0,
          "cheats other game: A's cheat is on in A, the new game B has none on: %d %d" % (blacks(r.shot("a_on")), blacks(r.shot("b"))))
    check(r.shot("b_last").ui == "cheats:1", "cheats other game: B lists its own file's two codes: %s" % r.shot("b_last").ui)
    check(r.notes["a_back"]["game"] == "Alpha.gba.tns" and "cheat0_enable = false" in open(r.file("Alpha.cht.tns")).read(),
          "cheats other game: back in A, its switch was on as left (turned off by the next flip): %s" % open(r.file("Alpha.cht.tns")).read())
    check("true" not in open(r.file("Pattern.cht.tns")).read(), "cheats other game: B's file untouched")


def test_cheats_states():
    """Save states with a cheat on: saving and loading still work and the cheat stays on."""
    r = cheat_calc("cheats_states", [("gba", "Pattern.cht.tns", cht_text(GOOD[:2]))])
    r.run(START_GAME + OPEN_CHEATS + "press enter\nwait 0.2\n" + LEAVE_CHEATS + "press S\nwait 1\nnote saved\nwait 1\npress L\nwait 1\n"
          "note loaded\nshot after\n" + OPEN_CHEATS + "press enter\nwait 0.2\n" + LEAVE_CHEATS + "quit\n")
    toasts = osd_toasts(r)
    check(any(t.startswith("Saved to slot") for t in toasts) and any(t.startswith("Loaded slot") for t in toasts),
          "cheats states: saving and loading work with a cheat on: %s" % toasts)
    check(blacks(r.shot("after")) >= 12, "cheats states: the picture after the load has the cheat's write")
    check("cheat0_enable = false" in open(r.file("Pattern.cht.tns")).read(),
          "cheats states: the cheat was still on after the load (the next flip turned it off): %s" % open(r.file("Pattern.cht.tns")).read())


def test_link_delay():
    """Two gpsp-hosts linked by a socket (host_link.c) whose messages take
    0, 10 or 25 ms to come (--link-delay), on the cable as wired, the game
    not using it: a script of frames ("frames N") runs the same frames at
    all, and "measure" counts the link's report lines from it to "measure
    stop". A game not using the cable has a message a frame each way (no
    nspire_link.c CABLE_TELL): player 2 runs each of player 1's frames
    once its end comes, and player 1 waits for player 2's frame 3 before
    its own (CABLE_LEAD), whose end comes two delays and player 2's frame
    (its costs: 4 ms, two messages' 0.1 ms) after player 1's: two frames
    are room for 10 ms, at 25 ms they take 54.4 ms, 36.8 frames a
    second."""
    script = "cost frame 4\ncost send 0.1\ncost receive 0.1\nframes 30\nlink\nframes 10\nmeasure\n" \
             "frames 120\nmeasure stop\nnote done\nframes 30\nquit\n"
    delays = (0, 10, 25)
    seen = {}
    for delay in delays:
        runs = [Run("link_delay%d_p%d" % (delay, p)) for p in (1, 2)]
        sock = os.path.join(WORK, "link_delay%d.sock" % delay)
        procs = [r.start(script, "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-device", "exact",
                                                     "--link-delay", str(delay)])
                 for p, r in enumerate(runs, 1)]
        for r, proc in zip(runs, procs):
            r.finish(proc)
        seen[delay] = runs
        for p, r in enumerate(runs, 1):
            name = "link delay %d ms, player %d" % (delay, p)
            check(r.report.get("link", "").startswith("Linked: player %d (Link cable, exact)" % p) or
                  r.report.get("link") == "The link cable came out.", "%s: linked: %s" % (name, r.report.get("link")))
            check(r.report.get("link_frames") == "119", "%s: 119 frames after the first measured: %s"
                  % (name, r.report.get("link_frames")))
            sent = int(r.report.get("link_messages_sent", 0))
            check(119 <= sent <= 121, "%s: a message a frame: %d" % (name, sent))
    for p in (0, 1):
        frames = [seen[d][p].notes["done"]["frame"] for d in delays]
        check(len(set(frames)) == 1, "link delay: player %d's frames the same at every delay: %s" % (p + 1, frames))
        fps = [float(seen[d][p].report.get("link_fps", 0)) for d in delays]
        check(59.5 <= fps[0] <= 59.8, "link delay: player %d at 0 ms has full speed: %s" % (p + 1, fps[0]))
        check(59.5 <= fps[1] <= 59.8, "link delay: player %d at 10 ms has full speed: %s" % (p + 1, fps[1]))
        check(36.3 <= fps[2] <= 37.3, "link delay: player %d at 25 ms, two delays and a frame in two: %s"
              % (p + 1, fps[2]))
        wait = float(seen[25][p].report.get("link_wait", 0))
        check(wait > 1.0, "link delay: player %d waited on the link at 25 ms: %s s" % (p + 1, wait))


POKE_CODE = "BPEE"


def test_link_real_model():
    """R2: the real logs' NavNet (2026-11-10, a CX II and a CX CAS): a write
    holds the game ~40 ms (--link-write), a message takes ~42 ms one way and
    reaches a waiting game on the OS's 10 ms tick (--link-os), and the
    games have the delay the measured round trip asked for. Before R2 (each
    message its own write, --link-nohold) a frame's write alone took more
    than two frames' time: 8 frames a second as on the calculator; held
    and sent together, the writes are a part of the frames' time and the
    link runs at about half speed or better."""
    script = "cost frame 4\ncost send 0.1\ncost receive 0.1\nframes 30\nlink\nframes 10\nmeasure\n" \
             "frames 300\nmeasure stop\nnote done\nframes 30\nquit\n"
    fps = {}
    for mode, extra in (("held", []), ("each", ["--link-nohold"])):
        runs = [Run("link_real_%s_p%d" % (mode, p)) for p in (1, 2)]
        for r in runs:   # the game code the stand-in is for
            with open(os.path.join(r.gba, "Pattern.gba.tns"), "r+b") as f:
                f.seek(0xAC)
                f.write(POKE_CODE.encode())
        sock = os.path.join(WORK, "link_real_%s.sock" % mode)
        procs = [r.start(script, "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-device", "cable", "--link-delay", "2", "--link-os", "--link-write", "40", "--link-slow", "260", "3",
                                                     "--stand-in", POKE_CODE + ":poke"] + extra)
                 for p, r in enumerate(runs, 1)]
        for r, proc in zip(runs, procs):
            r.finish(proc)
        fps[mode] = [float(r.report.get("link_fps", 0)) for r in runs]
        log = open(os.path.join(r.dir, "zgpSP_files", "gpsp_link.txt.tns")).read()
        m = re.search(r"round trip, 8-byte messages \(\d+\): min (\d+)", log)
        check(m and 80000 <= int(m.group(1)) <= 130000,
              "link real model %s: the modelled round trip's minimum is the calculators' (84 ms, here 100): %s"
              % (mode, m and m.group(1)))
        print("   real model %s: fps %s, report %s" % (mode, fps[mode], {k: v for k, v in runs[0].report.items() if k.startswith("link")}))
    check(max(fps["each"]) < 12, "link real model: each message its own write: the calculator's ~8 frames a second: %s" % fps["each"])
    check(min(fps["held"]) >= 30, "link real model: held messages go together: at least 30 frames a second: %s" % fps["held"])


def test_link_fast():
    """The fast transport (nspire_link_fast.c) as nspire_link.c sees it, on
    link_real_model's NavNet (the calculators' writes, ticks and round
    trips): both offering it (the CX II as host, the CX as device) it comes
    up after the hellos, the pings measure it (under 20 ms), the delay is 2
    frames and the link runs at full speed; an older gpSP (no offer) links
    over NavNet as before with one line why; a device that never answers
    gives up and links over NavNet; a fall back mid-link carries on; a USB
    drive plugged in mid-link (the arbiter's no: nspire_arb_fast_refusal)
    makes the fast transport let go, and the link goes on over NavNet."""
    script = "cost frame 4\ncost send 0.1\ncost receive 0.1\nframes 30\nlink\nframes 10\nmeasure\n" \
             "frames 300\nmeasure stop\nnote done\nframes 30\nquit\n"
    letgo = script.replace("frames 300\n", "frames 100\npad in drive\nframes 200\n")
    modes = (("up", ["host"], ["device"]), ("old", ["host"], []), ("noanswer", ["host"], ["noanswer"]),
             ("fall", ["host", "100"], ["device", "100"]), ("compat", ["host"], ["device", None, "compatible"]),
             ("crash", ["host"], ["device"]), ("letgo", ["host"], ["device"]), ("nomarker", ["host"], ["device"]))
    fps, logs, fast_dirs = {}, {}, {}
    for mode, f1, f2 in modes:
        runs = [Run("link_fast_%s_p%d" % (mode, p)) for p in (1, 2)]
        for r in runs:
            with open(os.path.join(r.gba, "Pattern.gba.tns"), "r+b") as f:
                f.seek(0xAC)
                f.write(POKE_CODE.encode())
        sock = os.path.join(WORK, "link_fast_%s.sock" % mode)
        procs = []
        if mode == "crash":
            # Player 2's last run died with the fast transport in use; an
            # r16 build's marker (fast17: gpsp_fast.txt.tns becomes gpsp_fast.run)
            d = os.path.join(runs[1].dir, "zgpSP_files")
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "gpsp_fast.txt.tns"), "w") as f:
                f.write("in use at clock 1234 s\n")
        if mode == "nomarker":
            # F3: player 2's marker can't be written (a directory, not empty,
            # in its place): it doesn't bring the fast transport up.
            d = os.path.join(runs[1].dir, "zgpSP_files", "gpsp_fast.run")
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "keep"), "w") as f:
                f.write("x")
        for p, (r, ff) in enumerate(zip(runs, (f1, f2)), 1):
            extra = (["--link-fast", ff[0]] if ff else []) + (["--link-fast-fall", ff[1]] if len(ff) > 1 and ff[1] else []) + \
                    (["--link-speed", ff[2]] if len(ff) > 2 else [])
            procs.append(r.start(letgo if mode == "letgo" and p == 1 else script, "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-device", "cable", "--link-delay", "42", "--link-os",
                                                             "--link-write", "40", "--link-slow", "260", "3", "--stand-in", POKE_CODE + ":poke"] + extra))
        for r, proc in zip(runs, procs):
            r.finish(proc)
        fps[mode] = [float(r.report.get("link_fps", 0)) for r in runs]
        logs[mode] = [open(os.path.join(r.dir, "zgpSP_files", "gpsp_link.txt.tns")).read() for r in runs]
        fast_dirs[mode] = [r.dir for r in runs]
        check(all(r.notes.get("done") for r in runs), "link fast %s: both ran their frames" % mode)
        m = re.search(r"delay: (\d+) frames", logs[mode][0])
        rt = re.search(r"round trip, 8-byte messages \(\d+\): min (\d+), median (\d+)", logs[mode][0])
        print("   fast %s: fps %s, delay %s, round trip min/median %s us" % (mode, fps[mode], m and m.group(1), str(rt and rt.groups())))
        if mode == "up":
            check(all("fast transport: up" in l for l in logs[mode]), "link fast: up on both sides")
            check(m and int(m.group(1)) <= 2, "link fast: a delay of 1-2 frames: %s" % (m and m.group(1)))
            check(rt and int(rt.group(2)) < 20000, "link fast: the pings' round trip under 20 ms: %s" % str(rt and rt.groups()))
            check(min(fps[mode]) >= 55, "link fast: full speed: %s" % fps[mode])
        elif mode == "letgo":
            check("fast transport: up" in logs[mode][0] and
                  "seamless: the fast link lets go of the USB controller: a USB drive is in" in logs[mode][0] and
                  "fell back to NavNet: let go: a USB drive is in" in logs[mode][0],
                  "link fast letgo: the drive mid-link made player 1 let go of the controller, said why")
            check(min(fps[mode]) >= 20, "link fast letgo: the link goes on over NavNet: %s" % fps[mode])
        elif mode == "fall":
            # After the fall back the round trip is measured again over
            # NavNet and both raise the delay (FOLLOW-UP 2): NavNet's speed
            # again, not a NavNet round trip held every frame (9 fps).
            check(min(fps[mode]) >= 25, "link fast fall: back to NavNet's speed: %s" % fps[mode])
            check(all(re.search(r"delay: round trips over NavNet after the fall back", l) for l in logs[mode]),
                  "link fast fall: both measured again")
            check(any(re.search(r"delay: raised to (\d+) frames", l) for l in logs[mode]) or
                  all(re.search(r"this side asks (\d+) frames \(2 now\)", l) for l in logs[mode]),
                  "link fast fall: the delay raised")
        elif mode == "nomarker":
            # (The model has no FAIL to the other side: only player 2 is checked.)
            check("fast transport: up" not in logs[mode][1], "link fast nomarker: player 2 not up")
        else:
            check(all("fast transport: up" not in l for l in logs[mode]), "link fast %s: not up" % mode)
            check(min(fps[mode]) >= 20, "link fast %s: the link runs as over NavNet: %s" % (mode, fps[mode]))
    check(sum("doesn't offer it (an older build" in l for l in logs["old"]) == 1, "link fast old: one line why (the side that tried)")
    check(all("not used" in l for l in logs["noanswer"]), "link fast noanswer: both say why")
    check(all("fell back to NavNet" in l for l in logs["fall"]), "link fast fall: both fell back")
    check(min(fps["up"]) > max(fps["old"]), "link fast: faster than NavNet: %s vs %s" % (fps["up"], fps["old"]))
    check("this side: Link speed is Compatible" in logs["compat"][1] and "or its Link speed is Compatible" in logs["compat"][0],
          "link fast compat: the Compatible side says so, the other copes")
    check("the last run died with it in use at clock 1234" in logs["crash"][1] and
          "Fast link turned off after a crash" in logs["crash"][1] and "turned off after a crash" in logs["crash"][1],
          "link fast crash: player 2 turned it off, said and toasted")
    check(all(not os.path.exists(os.path.join(r, "zgpSP_files", "gpsp_fast.run")) for r in fast_dirs["up"]),
          "link fast up: the marker gone after the link")
    check(open(os.path.join(fast_dirs["crash"][1], "zgpSP_files", "gpsp_fast.run")).read().startswith("off after a crash"),
          "link fast crash: the marker keeps it off")
    check(not os.path.exists(os.path.join(fast_dirs["crash"][1], "zgpSP_files", "gpsp_fast.txt.tns")),
          "link fast crash: r16's marker name gone (fast17: renamed)")
    # (the old name's going reads the OS's list again at a quit: core_tests' file list
    # checks; these runs end by the script, not through quit())
    check("this side: the crash marker could not be written" in logs["nomarker"][1],
          "link fast nomarker: player 2 couldn't write its marker: not tried, said why")
    # The pair runs at the slower side's own speed: a CX-like player 2 (28 ms
    # of emulation a frame: ~35 a second alone; the real CX CAS's frames in
    # the SEAM1 log were ~30 ms besides NavNet's writes) links over the fast
    # transport at its unlinked speed (nothing in the link's loop holds it
    # back) and over NavNet's timing far below it.
    cx_script = "cost frame %d\ncost send 0.3\ncost receive 0.3\nframes 30\nlink\nframes 10\nmeasure\n" \
                "frames 600\nmeasure stop\nnote done\nframes 30\nquit\n"
    alone = Run("link_fast_cx_alone")
    alone.run("cost frame 28\ncost send 0.3\ncost receive 0.3\nframes 40\nnote from\nframes 600\nnote done\nquit\n",
              "Pattern.gba.tns")
    a0, a1 = alone.notes["from"], alone.notes["done"]
    alone_fps = (int(a1["frame"]) - int(a0["frame"])) / (float(a1["time"]) - float(a0["time"]))
    cx_fps = {}
    for mode, fast in (("fast", True), ("navnet", False)):
        runs = [Run("link_fast_cx_%s_p%d" % (mode, p)) for p in (1, 2)]
        for r in runs:
            with open(os.path.join(r.gba, "Pattern.gba.tns"), "r+b") as f:
                f.seek(0xAC)
                f.write(POKE_CODE.encode())
        sock = os.path.join(WORK, "link_fast_cx_%s.sock" % mode)
        procs = []
        for p, (r, cost, role) in enumerate(zip(runs, (4, 28), ("host", "device")), 1):
            procs.append(r.start(cx_script % cost, "Pattern.gba.tns",
                                 ["--link", sock, "--player", str(p), "--link-device", "cable", "--link-delay", "42", "--link-os",
                                  "--link-write", "40", "--link-slow", "260", "3", "--stand-in", POKE_CODE + ":poke"] +
                                 (["--link-fast", role] if fast else [])))
        for r, proc in zip(runs, procs):
            r.finish(proc)
        cx_fps[mode] = [float(r.report.get("link_fps", 0)) for r in runs]
    print("   fast cx (28 ms frames): linked fast %s, over NavNet %s, alone %.2f" % (cx_fps["fast"], cx_fps["navnet"], alone_fps))
    check(min(cx_fps["fast"]) >= 0.97 * alone_fps, "link fast cx: the CX-like side at its own speed (%.2f alone) over the fast "
          "transport: %s" % (alone_fps, cx_fps["fast"]))
    check(max(cx_fps["navnet"]) < 30, "link fast cx: over NavNet's timing it is held below 30: %s" % cx_fps["navnet"])


def test_link_fast_os():
    """LINK2 (the r6 logs, 2026-11-11: the CX II host, the CX CAS device at
    192 MHz, Emerald in Dewford, the fast transport up 141 s): the CAS alone
    makes ~46 game frames a second (23 pictures, automatic frameskip 1),
    linked 35.2. Its frames waited for the CX II 8 times in all; its
    seconds without an OS sleep made 45-46 frames, with them 30-44: each
    sleep (1.6 a second, the frame's spare time after the frameskip's
    catch-up, nspire_wait_until) came back after 50-82 ms, once 363. The
    model: the CAS's frames 10 ms + 140 us a drawn line (46.4 alone), its
    OS tick 2.5 ms, each sleep 60 ms more while the fast transport is up;
    the CX II's 4 ms + 25 us, tick 10.25 ms. Before (--link-fast-os, R9F:
    the linked frame always gave the OS its spare time) the pair runs ~34;
    now the linked frame gives none while the fast transport carries the
    link, and the pair runs at the CAS's own speed (less the messages'
    0.6 ms a frame), its seconds with no OS sleep at all.

    final/pace: the CAS's frames are 3 ms + 40 us a drawn line here (12.4 ms
    drawn and the messages' 0.6: about 3.7 ms to spare a frame). The r6 costs
    (10 ms + 140 us, 46 a second at the floor of one picture in 2) no longer
    have any spare time: R9F's spare time came from the frame loop's catch-up,
    which put a frame 6 frames late back on time, so the frames after it were
    early and waited; the loop now carries on a frame behind (a game slower
    than full speed has no time to wait away), and before and now both ran
    45.8 there, no sleep in either; a scene at full speed by leaving pictures
    out (6 ms + 80 us) has its frames up to half a frame late, never early,
    and no sleep either (59.7 both). The LINK2 rule needs frames with spare
    time to show: these."""
    config = owner_config()
    config[24], config[25] = 0, 1   # the CAS's: automatic, 1 at most
    cx = "cost frame 3\ncost line 40\ncost turn 3\ncost send 0.3\ncost receive 0.3\n"
    cx2 = "cost frame 4\ncost line 25\ncost turn 3\ncost send 0.3\ncost receive 0.3\n"
    alone = Run("link_fast_os_alone", files={"gpsp_qol.cfg.tns": config})
    alone.run(cx + "wait 1\npress enter\nwait 3\nnote from\nwait 10\nnote done\nquit\n", "Pattern.gba.tns",
              options=["--model", "cx", "--os-tick", "2.5"])
    a0, a1 = alone.notes["from"], alone.notes["done"]
    alone_fps = (a1["frame"] - a0["frame"]) / (float(a1["time"]) - float(a0["time"]))
    script = "%sframes 30\nlink\nframes 10\nmeasure\nnote m0\nframes 900\nnote m1\nmeasure stop\nnote done\nframes 30\nquit\n"
    fps, idle, pictures = {}, {}, {}
    for mode in ("before", "now"):
        runs = [Run("link_fast_os_%s_p%d" % (mode, p), files={"gpsp_qol.cfg.tns": config}) for p in (1, 2)]
        for r in runs:
            with open(os.path.join(r.gba, "Pattern.gba.tns"), "r+b") as f:
                f.seek(0xAC)
                f.write(POKE_CODE.encode())
        sock = os.path.join(WORK, "link_fast_os_%s.sock" % mode)
        procs = []
        for p, r in enumerate(runs, 1):
            opts = (["--model", "cx", "--os-tick", "2.5", "--os-busy-fast", "60"] if p == 2 else ["--os-tick", "10.25"]) + \
                   (["--link-fast-os"] if mode == "before" else []) + \
                   ["--link", sock, "--player", str(p), "--link-device", "cable", "--link-delay", "42", "--link-os",
                    "--link-write", "40", "--link-slow", "260", "3", "--stand-in", POKE_CODE + ":poke",
                    "--link-fast", "host" if p == 1 else "device"]
            procs.append(r.start(script % (cx2 if p == 1 else cx), "Pattern.gba.tns", opts))
        for r, proc in zip(runs, procs):
            r.finish(proc)
        check(all(r.notes.get("done") for r in runs), "link fast os %s: both ran their frames" % mode)
        fps[mode] = [float(r.report.get("link_fps", 0)) for r in runs]
        m0, m1 = runs[1].notes["m0"], runs[1].notes["m1"]
        pictures[mode] = (m1["shown"] - m0["shown"]) / (float(m1["time"]) - float(m0["time"]))   # the CAS's
        last = open(os.path.join(runs[1].dir, "zgpSP_files", "gpsp_link_last.txt.tns")).read()
        idle[mode] = [int(n) for n in re.findall(r"OS sleeps \d+ \(longest \d+ ms, (\d+) idle\)", last)]
        check(all("fast transport: up" in open(os.path.join(r.dir, "zgpSP_files", "gpsp_link.txt.tns")).read() for r in runs),
              "link fast os %s: the fast transport up on both" % mode)
    print("   fast os (the CAS with time to spare): alone %.2f, linked before %s, now %s; the CAS's pictures a second "
          "before %.1f, now %.1f; its idle OS sleeps a second before %s, now %s" %
          (alone_fps, fps["before"], fps["now"], pictures["before"], pictures["now"], idle["before"][-5:],
           idle["now"][-5:]))
    # R9F's loss: its sleeps' 60 ms more. Since final/pace automatic frameskip
    # holds the game at full speed by leaving pictures out, so the loss is in
    # the pictures (before the rule it was the game's frames: ~34 of 46)
    check(max(fps["before"]) < 0.85 * alone_fps or pictures["before"] < 0.85 * pictures["now"],
          "link fast os: the model shows R9F's loss (%.2f alone): %s game frames, %.1f pictures a second (now %.1f)"
          % (alone_fps, fps["before"], pictures["before"], pictures["now"]))
    check(min(fps["now"]) >= 0.95 * alone_fps, "link fast os: the pair at the CAS's own speed (%.2f alone): %s" % (alone_fps, fps["now"]))
    check(len(idle["now"]) >= 5 and not any(idle["now"]) and any(idle["before"]),
          "link fast os: the linked CAS's seconds give the OS no tick now (before %s, now %s)" % (idle["before"], idle["now"]))


def test_link_hold_frames():
    """R3: the R2 logs' CX CAS (one channel, its game writing: 69 ms a write,
    227 writes in 788 frames, 37% of its time, 18.7 frames a second, never
    waiting for the CX II) held what it sends 7 frames counted in 60ths of a
    second: 117 ms, 2.2 of its frames. Counted in frames (R3) a write goes
    every 7 of them and the link runs faster; R2's hold is --link-hold-time."""
    script = "cost frame 33\ncost send 0.1\ncost receive 0.1\nframes 30\nlink\nframes 10\nmeasure\n" \
             "frames 300\nmeasure stop\nnote done\nframes 30\nquit\n"
    fps, writes = {}, {}
    for mode, extra in (("frames", []), ("time", ["--link-hold-time"])):
        runs = [Run("link_hold_%s_p%d" % (mode, p)) for p in (1, 2)]
        for r in runs:
            with open(os.path.join(r.gba, "Pattern.gba.tns"), "r+b") as f:
                f.seek(0xAC)
                f.write(POKE_CODE.encode())
        sock = os.path.join(WORK, "link_hold_%s.sock" % mode)
        procs = [r.start(script, "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-device", "cable", "--link-delay", "100", "--link-os", "--link-write", "69",
                                                     "--stand-in", POKE_CODE + ":poke"] + extra)
                 for p, r in enumerate(runs, 1)]
        for r, proc in zip(runs, procs):
            r.finish(proc)
        fps[mode] = [float(r.report.get("link_fps", 0)) for r in runs]
        log = open(os.path.join(runs[0].dir, "zgpSP_files", "gpsp_link.txt.tns")).read()
        m = re.search(r"delay: (\d+) frames", log)
        print("   hold %s: fps %s, delay %s" % (mode, fps[mode], m and m.group(1)))
    check(min(fps["frames"]) >= 1.03 * max(fps["time"]),
          "link hold in frames: a slow side's writes spread over more frames: %s vs %s" % (fps["frames"], fps["time"]))


def test_link_wait_screen():
    """R4: "Waiting for the other GBA" only for a stall of 2 s or more
    (it flashed at the calculators' 0.4-0.9 s round trips, R3): writes of
    1.5 s every 40th (player 1's: player 2 waits) show nothing
    more than none (--link-slices: waits end after their slice);
    a 3 s write every 40th shows it, and the game goes on after it."""
    script = "cost frame 4\ncost send 0.1\ncost receive 0.1\nframes 30\nlink\nframes 10\nmeasure\n" \
             "frames 400\nmeasure stop\nnote done\nframes 30\nquit\n"
    screens = {}
    for mode, slow in (("none", []), ("slow", ["1500", "40"]), ("stall", ["3000", "40"])):
        runs = [Run("link_wait_%s_p%d" % (mode, p)) for p in (1, 2)]
        for r in runs:
            with open(os.path.join(r.gba, "Pattern.gba.tns"), "r+b") as f:
                f.seek(0xAC)
                f.write(POKE_CODE.encode())
        sock = os.path.join(WORK, "link_wait_%s.sock" % mode)
        procs = [r.start(script, "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-device", "cable",
                                                     "--link-delay", "60", "--link-os", "--link-write", "40", "--link-slices"] +
                         (["--link-slow"] + slow if p == 1 and slow else []) + ["--stand-in", POKE_CODE + ":poke"])
                 for p, r in enumerate(runs, 1)]
        for r, proc in zip(runs, procs):
            r.finish(proc)
        screens[mode] = [int(r.report.get("link_wait_screens", "-1")) for r in runs]
        check(all(r.notes.get("done") for r in runs), "link wait screen %s: both ran their frames" % mode)
    # (Linking's own round trips can show it once: "none" has no slow writes.)
    check(screens["slow"] == screens["none"], "link wait screen: none more for 1.5 s writes: %s (without: %s)"
          % (screens["slow"], screens["none"]))
    check(max(screens["stall"]) > max(screens["none"]), "link wait screen: shown for 3 s stalls: %s (without: %s)"
          % (screens["stall"], screens["none"]))


def test_link_wait_leaves_nothing():
    """The "waiting for the other GBA" screen is
    drawn whole into one screen buffer; the game's pictures leave some
    lines alone (the CX's 3:2 picture: line 226; the unscaled picture: its
    border), so the wait screen's lines stayed there and showed in every
    third frame until the next menu. Run.finish requires picture_stale 0:
    outside the picture and the text bands, every shown buffer is black.
    Here a 3 s stall shows the screen, on both LCDs, at each picture size."""
    script = "cost frame 4\ncost send 0.1\ncost receive 0.1\nframes 30\nlink\nframes 10\n" \
             "frames 200\nnote done\nframes 30\nquit\n"
    for model in ("cx", "cx2"):
        for scale in (0, 1, 3):
            name = "link_stale_%s_%d" % (model, scale)
            cfg = owner_config()
            cfg[0] = scale
            runs = [Run("%s_p%d" % (name, p), files={"gpsp_qol.cfg.tns": cfg}) for p in (1, 2)]
            for r in runs:
                with open(os.path.join(r.gba, "Pattern.gba.tns"), "r+b") as f:
                    f.seek(0xAC)
                    f.write(POKE_CODE.encode())
            sock = os.path.join(WORK, "%s.sock" % name)
            procs = [r.start(script, "Pattern.gba.tns", ["--model", model, "--link", sock, "--player", str(p),
                                                         "--link-device", "cable", "--link-delay", "60", "--link-os",
                                                         "--link-write", "40", "--link-slices"] +
                             (["--link-slow", "3000", "40"] if p == 1 else []) + ["--stand-in", POKE_CODE + ":poke"])
                     for p, r in enumerate(runs, 1)]
            for r, proc in zip(runs, procs):
                r.finish(proc)
            check(all(r.notes.get("done") for r in runs), "%s: both ran their frames" % name)
            check(max(int(r.report.get("link_wait_screens", "0")) for r in runs) > 0,
                  "%s: the wait screen was shown: %s" % (name, [r.report.get("link_wait_screens") for r in runs]))
            check(all(int(r.report.get("picture_frames_checked", "0")) > 100 for r in runs),
                  "%s: pictures checked: %s" % (name, [r.report.get("picture_frames_checked") for r in runs]))


def test_os_keys_after_exit():
    """R6: after a linked trade the CX CAS's file browser, once gpSP had
    quit, pressed del, right and enter by itself and deleted gpSP's program
    file: the game's keys had reached the OS, which ran (its keypad's
    interrupt handled) in every OS sleep while linked, and queued them. The
    model (host_platform.c): the OS looks at the keys whenever it runs if its
    keypad interrupt is on (or always, GPSP_HOST_OS_KEYS=poll), and the
    report counts what's still queued at exit. Linked, player 1 leaves by
    Exit gpSP holding enter, player 2 by the end of its script (exit
    without gpSP's quit); and unlinked."""
    p1 = "cost frame 4\ncost send 0.1\ncost receive 0.1\nframes 30\nlink\nframes 10\n" \
         "press del\npress right\npress enter\nhold up\nframes 20\nrelease up\nhold on\nwait 0.2\nrelease on\n" \
         "frames 20\nnote played\npress menu\nwait 0.3\npress up\npress right\npress right\nwait 0.2\nhold enter\nwait 0.6\nrelease enter\n" \
         "wait 0.5\npress enter\nwait 1\nquit\n"
    p2 = "cost frame 4\ncost send 0.1\ncost receive 0.1\nframes 30\nlink\nframes 10\n" \
         "press del\npress right\npress enter\nframes 40\nnote played\nquit\n"
    alone = REAL_COSTS + "wait 1\npress del\npress right\npress enter\nwait 0.5\npress menu\nwait 0.3\n" \
        "press up\npress right\npress right\nwait 0.2\npress enter\nwait 0.5\npress enter\nwait 1\nquit\n"
    for mode in ("irq", "poll"):
        old = os.environ.get("GPSP_HOST_OS_KEYS")
        os.environ["GPSP_HOST_OS_KEYS"] = mode
        try:
            runs = [Run("os_keys_%s_p%d" % (mode, p)) for p in (1, 2)]
            sock = os.path.join(WORK, "os_keys_%s.sock" % mode)
            procs = [r.start(script, "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-device",
                                                          "exact", "--link-delay", "20", "--link-os"])
                     for p, (r, script) in enumerate(zip(runs, (p1, p2)), 1)]
            for r, proc in zip(runs, procs):
                r.finish(proc)
            single = Run("os_keys_%s_alone" % mode).run(alone, "Pattern.gba.tns")
        finally:
            if old is None:
                del os.environ["GPSP_HOST_OS_KEYS"]
            else:
                os.environ["GPSP_HOST_OS_KEYS"] = old
        for r in runs + [single]:
            name = "OS keys (%s) %s" % (mode, r.name)
            check(r.report.get("os_keypad_irq_end") == "on", "%s: the OS's keypad interrupt on again" % name)
            check(r.report.get("hand_backs") == "1" and r.report.get("os_after_hand_back") == "0",
                  "%s: the hand-back last, nothing let the OS run after it: %s, %s" %
                  (name, r.report.get("hand_backs"), r.report.get("os_after_hand_back")))
            queued = int(r.report["os_keys_queued"])
            if mode == "irq":
                check(queued == 0, "%s: the OS saw no key while gpSP ran: %d" % (name, queued))
                check(r.report.get("os_keys_left") == "0", "%s: none left in the OS's queue at exit: %s"
                      % (name, r.report.get("os_keys_left")))
            marker = open(r.save("gpsp_link_run.txt.tns"), encoding="utf-8").read()
            check(marker.startswith("clean") and "hand-back writes: keypad line on" in marker and
                  "steps, ms:" in marker and "after the hand-back: gpsp_exit_after.txt.tns follows" in marker, "%s: the clean marker has the exit's steps and words: %r" % (name, marker))
        check("played" in runs[0].notes and "quit" not in runs[0].notes, "OS keys (%s): player 1 left by Exit gpSP" % mode)
        check("quit" not in single.notes, "OS keys (%s): unlinked, left by Exit gpSP" % mode)
        if mode == "poll":
            # EXIT-1: R6 took such keys at exit with Ndless's get_event; on
            # both calculators that drain is where the main task sat after
            # quitting (no key worked). No safe way to take them is known,
            # and no calculator has shown an OS that reads its keys with
            # its keypad line off ("turned on by the OS 0 times", the CX II's
            # 10498 OS sleeps and the CX CAS's 1062): the model's are left,
            # said here, not checked.
            check(int(runs[0].report["os_keys_queued"]) > 0,
                  "OS keys (poll): the model's OS saw the linked game's keys: %s" % runs[0].report["os_keys_queued"])
            print("  OS keys (poll): left in the model's queue at exit (no drain since EXIT-1): %s" %
                  ", ".join(r.report.get("os_keys_left", "?") for r in runs + [single]))


def test_exit_paths():
    """EXIT-1: after quitting no key worked on either calculator: every way
    out must end with the hand-back, once, the OS's keypad line on and
    nothing letting the OS run after it (the report: hand_backs,
    os_keypad_irq_end, os_after_hand_back; Run.finish checks them for every
    run of every suite). Here the ways out the other tests don't take, the
    clean marker's lines, and that the check catches an exit that lets the
    OS run after the hand-back."""
    cases = (
        ("quit_key", REAL_COSTS + "wait 1\npress enter\nwait 1.5\npress Q\nwait 1\nnote after\nquit\n", True),
        # UI: Quit is the main menu's last line; it asks nothing (the second enter is never read)
        ("menu_exit", REAL_COSTS + "wait 1\npress enter\nwait 1.5\npress menu\nwait 0.3\npress up\npress right\npress right\nwait 0.2\n"
                      "press enter\nwait 0.5\npress enter\nwait 1\nnote after\nquit\n", True),
        ("in_the_list", REAL_COSTS + "wait 1\nquit\n", False),
        ("script_end", REAL_COSTS + "wait 1\npress enter\nwait 1.5\nquit\n", False),
        ("held_key", REAL_COSTS + "wait 1\npress enter\nwait 1.5\nhold Q\nwait 3\nrelease Q\nnote after\nquit\n", True),
    )
    runs = {}
    for name, script, by_gpsp in cases:
        r = runs[name] = Run("exit_" + name).run(script)
        if by_gpsp:
            check("after" not in r.notes and "quit" not in r.notes, "exit %s: gpSP quit by itself" % name)
        elif by_gpsp is not None:
            check("quit" in r.notes, "exit %s: the script ended it" % name)
        check(r.report.get("hand_backs") == "1" and r.report.get("os_keypad_irq_end") == "on" and
              r.report.get("os_after_hand_back") == "0",
              "exit %s: the hand-back, keypad line on, nothing after: %s %s %s" % (
                  name, r.report.get("hand_backs"), r.report.get("os_keypad_irq_end"), r.report.get("os_after_hand_back")))
        marker = open(r.save("gpsp_link_run.txt.tns"), encoding="utf-8").read()
        check(marker.startswith("clean at clock") and "steps, ms:" in marker and "total" in marker and
              re.search(r"\nhardware at start: irq [0-9a-f]{8} \(keypad line on\), keypad [0-9a-f]{8} [0-9a-f]{8} "
                        r"status [0-9a-f]{8} enable [0-9a-f]{8}, gpio enable [0-9a-f]{8} status [0-9a-f]{8}, "
                        r"cursor [0-9a-f]{8} xy [0-9a-f]{8}\n", marker) and
              "\nhand-back writes: keypad line on; the OS turned the keypad line on 0 times" in marker,
              "exit %s: the marker has the steps and the words (nspire_os.c's wording): %r" % (name, marker[:600]))
        # EXIT-R4: a file after the hand-back, with the words read back (the
        # keypad line on), only when every key was up.
        after = r.save("gpsp_exit_after.txt.tns")
        if name == "held_key":
            check("after the hand-back: no file (a key was still down)" in marker and not os.path.exists(after),
                  "exit held_key: no file after the hand-back with a key down")
        else:
            clock = re.match(r"clean at clock (\d+) s,", marker)
            text = open(after, encoding="utf-8").read() if os.path.exists(after) else ""
            check("after the hand-back: gpsp_exit_after.txt.tns follows" in marker and clock and
                  text.startswith("after the hand-back of the run marked clean at clock %s s, build " % clock.group(1)) and
                  re.search(r"\nafter the hand-back: irq [0-9a-f]{8} \(keypad line on\), ", text),
                  "exit %s: the file after the hand-back, the marker's clock, the line on: %r" % (name, text))
            # r19-usb-exit: what came after the marker's steps (its own write, the link log's last lines),
            # and gpSP's whole exit to the hand-back, at least the steps' total
            m = re.search(r"\nafter the marker's steps, ms: the marker and the link log's last lines written (\d+); "
                          r"gpSP's exit, its first step to the hand-back, (\d+) ms in all \(after this file: Ndless's "
                          r"unload and the OS's redraw, which gpSP can't time\)", text)
            total = re.search(r"; total (\d+) ms", marker)
            check(m and total and int(m.group(2)) >= int(total.group(1)),
                  "exit %s: the file says the time after the marker's steps: %r" % (name, text[-400:]))
        # r19-usb-exit: a step's OS sleeps (and the first look for the OS's sleep) in brackets after its ms
        for note in re.findall(r" \d+ (\[[^]]*\])", marker.split("\n")[1] if "\n" in marker else ""):
            check(re.fullmatch(r"\[(finding the OS's sleep \d+ ms(, )?)?(\d+ OS sleeps? \d+ ms)?\]", note),
                  "exit %s: a step's note is the OS's sleeps: %r" % (name, note))
        if by_gpsp:
            check(re.search(r", quit \d+,", marker) and "pad driver shut down" in marker and "screen back" in marker,
                  "exit %s: gpSP's quit's steps are in it: %r" % (name, marker[:400]))
            # r19-usb-exit: the writes after the USB stops have steps of their own (they were in "screen back":
            # 384 ms of the CX II's evening quit), and the quit's files one a step
            check(re.search(r"USB drive shut down[^,]*, perf log written \(the sound's seconds\) \d+[^,]*, "
                            r"Bluetooth log written \d+[^,]*, memory and brightness given back \d+[^,]*, screen back ",
                            marker) and
                  re.search(r", perf log written \d+[^,]*, state times written \d+[^,]*, folder written \d+", marker),
                  "exit %s: the steps after the USB stops and the quit's files apart: %r" % (name, marker[:1200]))
        if name == "held_key":
            check("keys still down (Ndless waits for them)" in marker, "exit held_key: the wait for the keys is bounded")
            check(r.report.get("os_keys_left") == "0", "exit held_key: no key reached the OS: %s" % r.report.get("os_keys_left"))
    # EXIT-R1: two games left for another from the menu (gui.c menu_load
    # runs nspire_leave_game), minutes of play in all, then Exit gpSP: the
    # exit's clock starts at the quit, not at the first switch (its steps
    # filled the 32 slots after ~6 switches and the real quit's were lost).
    # UI: Settings, Games and saves, Choose another game (to_main("game_list"))
    switch = "press menu\nwait 0.3\n" + to_main("game_list") + "wait 0.2\npress enter\nwait 0.5\n%swait 20\n"
    r = Run("exit_after_switches", roms=("Alpha", "Bravo")).run(
        REAL_COSTS + "wait 1\npress enter\nwait 20\n" + switch % pick(2) + switch % pick(1) + switch % pick(2) +
        "press menu\nwait 0.3\npress up\npress right\npress right\nwait 0.2\npress enter\nwait 0.5\npress enter\nwait 1\nnote after\nquit\n")
    check("after" not in r.notes and r.notes.get("quit") is None, "exit after switches: gpSP quit by itself")
    marker = open(r.save("gpsp_link_run.txt.tns"), encoding="utf-8").read()
    m = re.search(r"steps, ms: ([^,;]+?) (\d+),.*; total (\d+) ms", marker)
    check(m and m.group(1) == "quit chosen" and int(m.group(2)) <= 50 and int(m.group(3)) < 15000,
          "exit after switches: the first step is the quit's, with a small time, the total the quit's: %r" %
          ((m.groups(),) if m else (marker[:300],)))
    check(marker.count("link closed") == 1 and marker.count("game's settings closed") == 1,
          "exit after switches: one leave_game's steps only (not the switches'): %r" % marker[:500])
    # EXIT-R4: the next run's log pairs the file with the marker: what was
    # read back after the hand-back, or that the file never came (the main
    # task stopped after the marker: in the hand-back or the OS's file call).
    next_script = REAL_COSTS + "wait 1\npress enter\nwait 1\npad port 1\nwait 2\npress Q\nwait 1\nquit\n"
    for name, drop in (("exit_next_run", False), ("exit_next_run_missing", True)):
        r = Run(name, reuse=runs["quit_key"])
        if drop:
            os.remove(r.save("gpsp_exit_after.txt.tns"))
        r.run(next_script)
        log = open(r.save("gpsp_link.txt.tns"), encoding="utf-8", errors="replace").read()
        if drop:
            check(re.search(r"the last run's exit: its marker \(clean at clock \d+ s\) said gpsp_exit_after.txt.tns "
                            r"follows, and it is missing: its main task stopped after the marker", log),
                  "exit, next run: the missing file is said: %r" % [l for l in log.splitlines() if "last run" in l])
        else:
            check(re.search(r"the last run's exit \(clean at clock \d+ s\): after the hand-back: irq [0-9a-f]{8} "
                            r"\(keypad line on\)", log),
                  "exit, next run: what was read back after the hand-back: %r" %
                  [l for l in log.splitlines() if "last run" in l])
    # EXIT-R6: the model's OS turns its keypad line on in an OS slice at the
    # quit that nothing checks after (the link's shutdown; NavNet's
    # StopService had no check after it), with Q still held: the pad glue's
    # stop() takes interrupts around the driver's shutdown, and must turn
    # the line off first, or the OS's handler queues the key (Run.finish:
    # os_keys_left 0).
    os.environ["GPSP_HOST_OS_LINE_ON"] = "stop"
    try:
        r = Run("exit_line_on_at_stop").run(REAL_COSTS + "wait 1\npress enter\nwait 1.5\nhold Q\nwait 3\n"
                                            "release Q\nnote after\nquit\n")
    finally:
        del os.environ["GPSP_HOST_OS_LINE_ON"]
    check(r.report.get("os_line_turned_on") == "1" and int(r.report.get("os_keypad_retaken", "0")) >= 1 and
          r.report.get("os_keys_queued") == "0",
          "exit, line on at the link's shutdown: turned off before the pad shutdown's interrupts, no key queued: "
          "on %s, retaken %s, queued %s" % (r.report.get("os_line_turned_on"), r.report.get("os_keypad_retaken"),
                                            r.report.get("os_keys_queued")))
    # The check of the check: an exit handler after gpSP's that sleeps into
    # the OS and masks the line again.
    r = Run("exit_late_sleeper")
    path = os.path.join(r.out, "script.txt")
    with open(path, "w") as f:
        f.write(REAL_COSTS + "wait 1\npress enter\nwait 1.5\npress Q\nwait 1\n")
    env = dict(os.environ, GPSP_HOST_LATE_SLEEP="1")
    proc = subprocess.run([HOST, path, r.out, r.gba], env=env, capture_output=True, text=True, timeout=300)
    report = dict(l.split(" ", 1) for l in open(os.path.join(r.out, "report.txt")).read().splitlines() if " " in l)
    check(proc.returncode == 0 and report.get("os_after_hand_back", "").strip() == "1" and
          report.get("os_keypad_irq_end", "").strip() == "on",
          "exit late sleeper: the report counts the OS sleep after the hand-back and the line stays on: %s %s" % (
              report.get("os_after_hand_back"), report.get("os_keypad_irq_end")))


def test_link_timer_dead():
    """A link with an older CX whose timer doesn't run (none found): time
    there moves only as frames are paced, and its waits for the other side
    must still be waits (on the CX CAS they all ended at once, and it gave
    up 0.3 s after linking). It links, runs the measured frames and says
    nothing on stderr; then player 1 quits and player 2, waiting on the
    link in its next frame, gives up on it."""
    script = "cost frame 4\ncost send 0.1\ncost receive 0.1\nframes 30\nlink\nframes 10\nmeasure\n" \
             "frames 120\nmeasure stop\nnote done\nframes 30\nquit\n"
    runs = [Run("link_dead_p%d" % p) for p in (1, 2)]
    sock = os.path.join(WORK, "link_dead.sock")
    procs = [r.start(script, "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-device", "exact"] +
                     (["--model", "cx", "--timer-dead", "1"] if p == 2 else []))
             for p, r in enumerate(runs, 1)]
    for r, proc in zip(runs, procs):
        r.finish(proc)
    for p, r in enumerate(runs, 1):
        name = "link, player 2's timer dead, player %d" % p
        check(r.report.get("link", "").startswith("Linked: player %d (Link cable, exact)" % p) or
              r.report.get("link") in ("The link cable came out.", "The other GBA unlinked"),
              "%s: linked: %s" % (name, r.report.get("link")))
        check(r.report.get("link_frames") == "119", "%s: 119 frames after the first measured: %s"
              % (name, r.report.get("link_frames")))
        check(not r.stderr.strip(), "%s: nothing on stderr: %s" % (name, r.stderr.strip()[:200]))


def test_link_esc_in_wait():
    """LNK-1: "esc unlinks" on the wait screen. Player 2 opens its menu
    (MSG_MENU 1: player 1's wait has no 15 s give-up then) and stays in it;
    player 1, waiting, presses esc: the link ends then, not when player 2
    comes back. An esc held from the game into the wait doesn't unlink.
    Then a menu left open for over 5 minutes: player 1 gives up on it."""
    common = "cost frame 4\ncost send 0.1\ncost receive 0.1\nframes 30\nlink\nframes 10\n"
    cases = (("esc", common + "wait 3\npress esc\nwait 1\nnote after_esc\nframes 30\nquit\n",
              common + "press menu\nwait 8\npress esc\nwait 1\nframes 30\nquit\n"),
             ("held", common + "hold esc\nframes 2\nnote held\nwait 3\nrelease esc\nframes 30\nquit\n",
              common + "press menu\nwait 6\npress esc\nwait 1\nframes 30\nquit\n"),
             ("menu20", common + "frames 120\nnote done\nquit\n",
              common + "press menu\nwait 20\npress esc\nwait 1\nframes 200\nquit\n"),
             ("long", common + "frames 30\nquit\n",
              common + "press menu\nwait 310\npress esc\nwait 1\nframes 30\nquit\n"))
    for mode, s1, s2 in cases:
        runs = [Run("link_esc_%s_p%d" % (mode, p)) for p in (1, 2)]
        sock = os.path.join(WORK, "link_esc_%s.sock" % mode)
        procs = [r.start(s, "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-device", "exact",
                                                "--link-delay", "20", "--link-slices"])
                 for p, (r, s) in enumerate(zip(runs, (s1, s2)), 1)]
        for r, proc in zip(runs, procs):
            r.finish(proc)
        p1 = runs[0]
        if mode == "esc":
            check(p1.report.get("link") == "Not linked", "esc in a link wait: player 1 unlinked: %s" % p1.report.get("link"))
            check(runs[1].report.get("link") == "The other GBA unlinked",
                  "esc in a link wait: player 2 heard it: %s" % runs[1].report.get("link"))
        elif mode == "held":
            check(p1.report.get("link", "").startswith("Linked") or p1.report.get("link") == "The other GBA unlinked",
                  "esc held from the game: still linked: %s" % p1.report.get("link"))
        elif mode == "menu20":
            # The give-up counts from the other leaving its menu, not the wait's start.
            check("done" in p1.notes and p1.report.get("link", "").startswith("Linked"),
                  "menu open 20 s: player 1 still linked: %s" % p1.report.get("link"))
        else:
            check(p1.report.get("link") == "The other GBA stopped answering.",
                  "menu open over 5 min: player 1 gave up: %s" % p1.report.get("link"))
            log = open(os.path.join(p1.dir, "zgpSP_files", "gpsp_link.txt.tns")).read()
            at = [int(l.split()[0]) for l in log.splitlines() if "unlinked:" in l]
            check(at and 300000 <= at[0] < 305000, "menu open over 5 min: given up at 5 min: %s ms" % at)
        for r in runs:
            check(not r.stderr.strip(), "esc in a link wait (%s) %s: nothing on stderr: %s"
                  % (mode, r.name, r.stderr.strip()[:200]))


def test_link_auto():
    """Linking on its own: two gpsp-hosts whose cable is in from the start
    (--link-auto, the calculators' link_cable_watch) link with no Link
    picked, at the lockstep's full speed. Player 1 unlinks; player 2 hears
    it, and neither links again on its own (the cable's still in, and
    neither connected anew)."""
    runs = [Run("link_auto_p%d" % p) for p in (1, 2)]
    sock = os.path.join(WORK, "link_auto.sock")
    script = "cost frame 4\ncost send 0.1\ncost receive 0.1\nframes 30\nmeasure\nframes 120\nmeasure stop\n" \
             "note linked\n%sframes 120\nnote after\nquit\n"
    procs = [r.start(script % ("unlink\n" if p == 1 else ""), "Pattern.gba.tns",
                     ["--link", sock, "--player", str(p), "--link-auto"]) for p, r in enumerate(runs, 1)]
    for r, proc in zip(runs, procs):
        r.finish(proc)
    for p, r in enumerate(runs, 1):
        name = "link on its own, player %d" % p
        check(r.report.get("link_protocol") == "3", "%s: linked, protocol 3: %s" % (name, r.report.get("link_protocol")))
        check(int(r.report.get("link_frames", 0)) >= 100, "%s: frames linked: %s" % (name, r.report.get("link_frames")))
        check(float(r.report.get("link_fps", 0)) >= 59.0, "%s: full speed: %s" % (name, r.report.get("link_fps")))
        want = "Not linked" if p == 1 else "The other GBA unlinked"
        check(r.report.get("link") == want, "%s: unlinked and stays so: %s" % (name, r.report.get("link")))
        check(not r.stderr.strip(), "%s: nothing on stderr: %s" % (name, r.stderr.strip()[:200]))


def test_link_auto_relink():
    """Linking on its own, then player 1 unlinks and, a while later, picks
    Link again: its new connection is the other's "connects anew", which
    lets player 2's hold go, so both link a second time (the PC's watch
    says 2 only once a side connected, as NavNet's). With the calculators'
    NavNet timing (--link-os: what's sent goes only when the OS runs) and
    a game with spare time, the link still runs at full speed."""
    # "linked 600": frames until the second link is up (real time decides
    # when the other's hello comes: under load it came after a fixed wait).
    script = "cost frame 4\ncost send 0.1\ncost receive 0.1\nframes 60\n%sframes 120\n%slinked 600\nframes 30\n" \
             "measure\nframes 120\nmeasure stop\nframes %d\nquit\n"
    # The two gpsp-hosts' messages also come in real time, and on a busy
    # machine (load ~11, other builds) the speed read 55.2-55.6 a second,
    # on this commit and the one before: the pair runs again if the
    # first run's speed is short (up to three runs: once more wasn't enough
    # at load ~5), and the speed fails only if every run's is short.
    for attempt in (1, 2, 3):
        runs = [Run("link_relink%d_p%d" % (attempt, p)) for p in (1, 2)]
        sock = os.path.join(WORK, "link_relink%d.sock" % attempt)
        procs = [r.start(script % (("unlink\n", "link\n", 30) if p == 1 else ("", "", 60)), "Pattern.gba.tns",
                         ["--link", sock, "--player", str(p), "--link-auto", "--link-os", "--link-delay", "5"])
                 for p, r in enumerate(runs, 1)]
        for r, proc in zip(runs, procs):
            r.finish(proc)
        fps = [float(r.report.get("link_fps", 0)) for r in runs]
        if min(fps) >= 58.0:
            break
        print("  re-Link: %s frames a second (load?): once more" % fps)
    for p, r in enumerate(runs, 1):
        name = "link on its own, re-Link, player %d" % p
        check(r.report.get("link_count") == "2", "%s: linked twice: %s" % (name, r.report.get("link_count")))
        check(int(r.report.get("link_frames", 0)) >= 100, "%s: frames linked again: %s"
              % (name, r.report.get("link_frames")))
        check(float(r.report.get("link_fps", 0)) >= 58.0, "%s: full speed with the OS's timing: %s"
              % (name, r.report.get("link_fps")))
        check(not r.stderr.strip(), "%s: nothing on stderr: %s" % (name, r.stderr.strip()[:200]))


def test_link_exact():
    """The link cable as wired between two of tests/host/rom/link.s: 9
    transfers a frame from player 1's interrupts, each side's word hanging
    on all the other's before it, the CPUs halted between them (halt) or
    spinning (A held: spin). Each runs the same words at the same cycles
    at any delay (the report's link_transfer_sum, both players' the same),
    and nspire_link.c's PC checks say nothing on stderr (a promised word
    is the one given, no transfer before the time told). Halted, player 2
    promises its word before player 1 asks (nspire_link.c), so 1 ms each
    way costs no speed; spinning, a transfer waits for the round trip."""
    script = "cost frame 4\ncost line 25\ncost send 0.1\ncost receive 0.1\nframes 30\n%slink\nframes 10\n" \
             "measure\nframes 300\nmeasure stop\nframes %d\nquit\n"
    seen = {}
    for mode in ("halt", "spin"):
        for delay in (0, 1):
            runs = [Run("link_exact_%s%d_p%d" % (mode, delay, p), roms=()) for p in (1, 2)]
            sock = os.path.join(WORK, "link_exact_%s%d.sock" % (mode, delay))
            for r in runs:
                link_rom(os.path.join(r.gba, "Link.gba.tns"))
            procs = [r.start(script % ("hold del\n" if mode == "spin" else "", 30 if p == 1 else 5), "Link.gba.tns",
                             ["--link", sock, "--player", str(p), "--link-device", "exact",
                              "--link-delay", str(delay)])
                     for p, r in enumerate(runs, 1)]
            for r, proc in zip(runs, procs):
                r.finish(proc)
            seen[mode, delay] = runs
            for p, r in enumerate(runs, 1):
                name = "link exact, %s at %d ms, player %d" % (mode, delay, p)
                check(r.report.get("link", "").startswith("Linked: player %d (Link cable, exact)" % p) or
                      r.report.get("link") == "The link cable came out.", "%s: linked: %s" % (name, r.report.get("link")))
                check(int(r.report.get("link_transfers", 0)) > 1500, "%s: transfers: %s"
                      % (name, r.report.get("link_transfers")))
                check(not r.stderr.strip(), "%s: nothing on stderr: %s" % (name, r.stderr.strip()[:200]))
    for mode in ("halt", "spin"):
        sums = [r.report.get("link_transfer_sum") for d in (0, 1) for r in seen[mode, d]]
        check(len(set(sums)) == 1, "link exact, %s: the same words at every delay, both players: %s" % (mode, sums))
    fps = dict(((m, d), float(seen[m, d][0].report.get("link_fps", 0))) for m in ("halt", "spin") for d in (0, 1))
    check(fps["halt", 0] >= 59.5 and fps["spin", 0] >= 59.5, "link exact: full speed at 0 ms: %s" % fps)
    check(fps["halt", 1] >= 59.5, "link exact: halted, full speed at 1 ms: %s" % fps["halt", 1])
    check(fps["spin", 1] < 50, "link exact: spinning, a round trip a transfer at 1 ms: %s" % fps["spin", 1])


def test_peek():
    """A script's "peek NAME ADDR:LEN": the game's RAM to out/NAME.bin (the
    link tests read battles with it) and a note. The test ROM's frame count
    (0x02000000) is the note's frame; IWRAM reads too, other addresses 0."""
    r = Run("peek").run("frames 30\npeek count 2000000:4\npeek iw 3007ff8:8\npeek rom 8000000:4\nquit\n",
                        "Pattern.gba.tns")
    count = open(os.path.join(r.out, "count.bin"), "rb").read()
    check(len(count) == 4 and struct.unpack("<I", count)[0] == r.notes["count"]["frame"] > 0,
          "peek: the frame count %s, note's %s" % (count.hex(), r.notes.get("count", {}).get("frame")))
    check(len(open(os.path.join(r.out, "iw.bin"), "rb").read()) == 8, "peek: 8 bytes of IWRAM")
    check(open(os.path.join(r.out, "rom.bin"), "rb").read() == bytes(4), "peek: not RAM reads 0")


def big_rom(path, megabytes=4, seed=1):
    """The pattern ROM padded with bytes that don't compress, to megabytes:
    big enough that reading it takes a while (cost read)."""
    import random
    pattern_rom(path)
    with open(path, "ab") as f:
        f.write(random.Random(seed).randbytes(megabytes * 1024 * 1024 - os.path.getsize(path)))


def saves_named(r, start):
    folder = os.path.join(r.dir, "zgpSP_files")
    return sorted(n for n in os.listdir(folder) if n.startswith(start)) if os.path.isdir(folder) else []


def test_cancel_loading():
    """Q on the loading screen goes back to the list, for a plain ROM, a zip
    and a .gbz, without writing anything; the next start loads. A Q held
    from before the load doesn't cancel it, and doesn't reach the game (Q
    is Save+Exit) when it's let go."""
    r = Run("cancel", roms=())
    big_rom(r.file("Big.gba.tns"))
    huge = os.path.join(r.dir, "Huge.gba.tns")
    big_rom(huge, seed=2)
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "..", "..", "tools", "gbz.py"), "pack", "-o", r.gba,
                    huge], check=True, capture_output=True)
    os.remove(huge)
    tmp = r.file("Inner.gba")
    big_rom(tmp, seed=3)
    with zipfile.ZipFile(r.file("Packed.zip.tns"), "w", zipfile.ZIP_STORED) as z:
        z.write(tmp, "Packed.gba")
    os.remove(tmp)
    # The list: Big, Huge (gbz), Packed (zip).
    r.run(REAL_COSTS + """cost read 600
wait 1
press enter
wait 0.8
shot loading
tap Q
wait 0.4
shot cancelled_rom
press right
press enter
wait 0.8
press Q
wait 0.4
shot cancelled_gbz
press right
press right
press enter
wait 0.8
tap Q
wait 0.4
shot cancelled_zip
hold Q
press enter
wait 4
note started
release Q
wait 1
note after_q
press menu
wait 0.3
quit
""")
    loading = r.shot("loading")
    check(loading.ui == "loading", "cancel: the loading screen first: %s" % loading.ui)
    hint = [x for y in range(222, 236) for x in range(320) if loading.at(x, y) != loading.at(5, 5)]
    check(hint and min(hint) == 8 and max(hint) < 160, "cancel: the Q hint at the bottom, at the left margin (r19: x 8): "
          "x %s-%s" % (min(hint) if hint else None, max(hint) if hint else None))
    for name in ("cancelled_rom", "cancelled_gbz", "cancelled_zip"):
        check(r.shot(name).ui.startswith("list"), "cancel: %s: back to the list: %s" % (name, r.shot(name).ui))
    check(r.notes["started"]["game"] == "Big.gba.tns", "cancel: Q held from before: it loads: %s"
          % r.notes["started"]["game"])
    check(r.notes["after_q"]["game"] == "Big.gba.tns" and "quit" in r.notes,
          "cancel: letting go of the held Q doesn't Save+Exit")
    check(not saves_named(r, "Huge") and not saves_named(r, "Packed"),
          "cancel: nothing written for the cancelled games: %s" % (saves_named(r, "Huge") + saves_named(r, "Packed")))

    # Cancelled from a running game's Game list: the list again, the game
    # left (saved as on leaving), then another start works.
    again = Run("cancel2", reuse=r).run(REAL_COSTS + """cost read 600
wait 1
press enter
wait 4
note first
press menu
wait 0.3
""" + to_main("game_list") + """press enter
wait 0.5
press right
press right
press enter
wait 0.8
tap Q
wait 0.4
shot list_again
press enter
wait 4
note second
quit
""")
    check(again.shot("list_again").ui.startswith("list"), "cancel: from a game: the list: %s"
          % again.shot("list_again").ui)
    check(again.notes["second"]["game"] == "Big.gba.tns", "cancel: from a game, then a start works: %s"
          % again.notes["second"]["game"])


def test_no_game_menu():
    """No game open (r19): esc on the game list goes to the tabs, and esc there picks Quit (enter on it quits
    gpSP; no key quits by itself; before r19 esc on the list closed gpSP at once); the menu key does the same
    as esc; the top row has Settings, Games and Quit (no Saves, Folders: the arrows wrap: left of Settings is Quit); Settings has
    its pages in a grid of three across (no Cheats), the first page Sound, no wrapping at its edges; System has
    no Game line (About the last)."""
    r = Run("no_game_esc").run("wait 1\nshot list\npress esc\nwait 0.5\nshot tabs\npress esc\nwait 0.5\n"
                              "shot quit_picked\nwait 1\npress enter\nwait 1\nquit\n")
    check(r.shot("list").ui == "list:0", "no game: the list starts with its first game picked: %s" % r.shot("list").ui)
    check(r.shot("tabs").ui == "main:%d" % NO_GAME["games"], "no game: esc on the list: the arrows on the tabs, Games: %s"
          % r.shot("tabs").ui)
    check(r.shot("quit_picked").ui == "main:%d" % NO_GAME["exit"] and "quit_picked" in r.notes,
          "no game: esc on the tabs picks Quit, gpSP still there: %s" % r.shot("quit_picked").ui)
    check("quit" not in r.notes and r.exit == 0, "no game: enter on Quit quits gpSP (exit %s)" % r.exit)
    # the menu key does what esc does, in the list and on the tabs
    r = Run("no_game_menu_key").run("wait 1\npress menu\nwait 0.5\nshot tabs\npress menu\nwait 0.5\nshot quit_picked\n"
                                   "wait 0.5\nquit\n")
    check(r.shot("tabs").ui == "main:%d" % NO_GAME["games"] and r.shot("quit_picked").ui == "main:%d" % NO_GAME["exit"] and "quit" in r.notes,
          "no game: the menu key is esc: %s %s" % (r.shot("tabs").ui, r.shot("quit_picked").ui))
    none = set_tiles(False)
    r = Run("no_game_menu").run("""wait 1
press esc
wait 0.4
press left
wait 0.2
shot settings_tab
press left
wait 0.2
shot last
press right
wait 0.2
shot wrapped
press right
wait 0.2
shot games_tab
press left
wait 0.2
press enter
wait 0.3
shot tiles
press left
wait 0.2
shot wrap_last
press right
wait 0.2
shot wrap_first
press down
wait 0.2
shot row_two
press esc
wait 0.3
shot tabs_again
press down
wait 0.2
shot remembered
""" + page_moves(3, none.index("More")) + """press enter
wait 0.4
shot more_first
press left
wait 0.2
shot more_last
press enter
wait 0.3
shot about
press esc
wait 0.3
shot more_back
press esc
wait 0.3
shot tiles_back
quit
""")
    check(r.shot("last").ui == "main:%d" % NO_GAME["exit"] and r.shot("wrapped").ui == "main:%d" % NO_GAME["settings"] and
          r.shot("games_tab").ui == "main:%d" % NO_GAME["games"],
          "no game: three words, wrapping (left of Settings is Quit): %s %s %s"
          % (r.shot("last").ui, r.shot("wrapped").ui, r.shot("games_tab").ui))
    check(r.shot("settings_tab").ui == "main:%d" % NO_GAME["settings"] and r.shot("tiles").ui == "Settings:0",
          "no game: Settings, enter into its tiles, the first picked: %s %s" % (r.shot("settings_tab").ui, r.shot("tiles").ui))
    check(r.shot("wrap_last").ui == "Settings:%d" % (len(none) - 1) and r.shot("wrap_first").ui == "Settings:0" and
          r.shot("row_two").ui == "Settings:3",
          "no game: Settings shows %d tiles (no Link, no Cheats), going round, down a row of three: %s %s %s"
          % (len(none), r.shot("wrap_last").ui, r.shot("wrap_first").ui, r.shot("row_two").ui))
    check(r.shot("tabs_again").ui == "main:%d" % NO_GAME["settings"] and r.shot("remembered").ui == "Settings:3",
          "no game: esc from the tiles: the top row; back in, on the tile left: %s %s"
          % (r.shot("tabs_again").ui, r.shot("remembered").ui))
    check(r.shot("more_first").ui == "More:0" and r.shot("more_last").ui == "More:%d" % (len(more_tiles(False)) - 1) and
          r.shot("about").ui == "About:0" and r.shot("more_back").ui == r.shot("more_last").ui and
          r.shot("tiles_back").ui == "Settings:%d" % none.index("More"),
          "no game: More has no Game tile, About the last, esc back to its tile, esc to More's: %s %s %s %s"
          % (r.shot("more_first").ui, r.shot("more_last").ui, r.shot("more_back").ui, r.shot("tiles_back").ui))


def test_questions():
    """The questions and messages over the menu (UI part 2): Save on a slot that holds one asks to replace
    it (esc keeps it, Replace does it); Quit asks nothing (it saves first when Save when I quit is on, not
    when it is off); a save that can't be written says so with one OK."""
    game = "Pokemon - Emerald Version (USA, Europe)"
    r = Run("questions", roms=(game,)).run("wait 1\npress enter\nwait 1.5\npress S\nwait 0.6\npress Q\nwait 1\nquit\n")
    # (S saved slot 0, Q saved slot 1 on its way out: slot 1 is the newest, the picked one)
    slot0 = r.save(game + ".gba1.svs.tns")
    old = time.time() - 7200
    os.utime(slot0, (old, old))
    # Save on quit off (r19: no tile: the settings file, word 59), then Save
    quit_off = read_words(r.save("gpsp.cfg.tns"))
    quit_off[59] = 0
    r = Run("questions2", reuse=r)
    settings_from_file(r, quit_off)
    r.run("wait 1\npress enter\nwait 2\npress menu\nwait 0.8\n" + """press right
press right
wait 0.2
press S
wait 0.4
shot replace
press esc
wait 0.4
shot kept
press S
wait 0.3
press right
press enter
wait 0.6
shot replaced
wait 0.5
quit
""")
    # (r16: a save goes back to the game, which is when the settings changed above are written)
    check(r.shot("replace").ui == "dialog:0", "questions: S on a state (r19: over it) asks, No picked: %s" % r.shot("replace").ui)
    check(r.shot("kept").ui == "saves:2",
          "questions: esc keeps it, the arrows back on the state that asked (saves:2): %s" % r.shot("kept").ui)
    check(r.shot("replaced").ui == "", "questions: Replace saved, back to the game: %s" % r.shot("replaced").ui)
    check(os.path.getmtime(r.save(game + ".gba1.svs.tns")) > old + 3600, "questions: Replace wrote slot 1 (newer than the old one)")

    # Quit asks nothing (no extra step on the way out). With Save when I quit off (as
    # set above) it leaves saving nothing: the picked slot's file stays as it was, no new one
    r2 = Run("questions3", reuse=r)
    before = os.path.getmtime(r2.save(game + ".gba1.svs.tns"))
    r2.run("wait 1\npress enter\nwait 2\npress menu\nwait 0.3\n" + to_main("exit") +
           "wait 0.2\nshot quit_line\npress enter\nwait 1\nnote after\nquit\n")
    check(r2.shot("quit_line").ui == "main:%d" % MAIN["exit"], "questions: on Quit: %s" % r2.shot("quit_line").ui)
    check("after" not in r2.notes and "quit" not in r2.notes, "questions: Quit leaves at once, no question")
    check(os.path.getmtime(r2.save(game + ".gba1.svs.tns")) == before and
          not os.path.exists(r2.save(game + ".gba2.svs.tns")), "questions: Quit left, saving nothing")

    # a full calculator: the save says so
    r3 = Run("questions4", roms=(game,)).run("wait 1\npress enter\nwait 1.5\npress menu\nwait 0.5\npress right\n"
                                           "press enter\nwait 0.6\nshot full\npress enter\nwait 0.4\nshot after\nquit\n",
                                           options=["--free-storage", "1"])
    check(r3.shot("full").ui == "dialog:0", "questions: a save that can't be written says so: %s" % r3.shot("full").ui)
    check(r3.shot("after").ui == "saves:1", "questions: OK, and the arrows are back on the SAVE tile: %s" % r3.shot("after").ui)

    # the first start on a calculator: The keys, then the game; the second start goes straight in
    os.environ["GPSP_HOST_HINTS"] = "1"
    try:
        r4 = Run("first_run", roms=(game,)).run("wait 1\npress enter\nwait 2\nshot keys\nnote before\npress enter\nwait 1\nnote after\nquit\n")
        r5 = Run("first_run2", reuse=r4).run("wait 1\npress enter\nwait 2\nshot second\nnote game\nquit\n")
    finally:
        del os.environ["GPSP_HOST_HINTS"]
    check(r4.shot("keys").ui == "keys_hint", "first run: The keys come first: %s" % r4.shot("keys").ui)
    check(r4.notes["after"]["frame"] > r4.notes["before"]["frame"], "first run: enter starts the game")
    check(r5.shot("second").ui == "" and r5.notes["game"]["frame"] > 60, "first run: the second start goes straight in: %s" % r5.shot("second").ui)


def test_menu_ab_keys():
    """The keys on A and B (del, var) are Enter and Esc in the list and the
    menus; the A that started a game doesn't press A in it."""
    r = Run("abkeys").run("""
wait 1
hold del
wait 1.5
note started
peek held 4000130:2
release del
wait 0.3
hold del
wait 0.2
peek pressed 4000130:2
release del
wait 0.3
press menu
wait 0.3
""" + to_main("settings") + """press del
wait 0.3
shot grid
press del
wait 0.3
shot settings
press var
wait 0.3
shot main
press var
wait 0.3
shot tabs
press var
wait 0.5
note resumed
quit
""")
    p1 = lambda name: struct.unpack("<H", open(os.path.join(r.out, name + ".bin"), "rb").read())[0]
    check(r.notes["started"]["game"] == "Pattern.gba.tns", "ab: A (del) starts the game from the list")
    check(p1("held") & 1, "ab: the A that started it isn't pressed in the game: P1 %03x" % p1("held"))
    check(not p1("pressed") & 1, "ab: a new A is: P1 %03x" % p1("pressed"))
    check(r.shot("grid").ui == "Settings:0", "ab: A on the Settings word goes into its tiles: %s" % r.shot("grid").ui)
    check(r.shot("settings").ui == "Settings:0" and r.shot("settings").pixels != r.shot("grid").pixels,
          "ab: A on Volume starts changing it: %s" % r.shot("settings").ui)
    check(r.shot("main").ui == "Settings:0" and r.shot("main").pixels == r.shot("grid").pixels,
          "ab: B puts the value back, the arrows stay on the tile: %s" % r.shot("main").ui)
    check(r.shot("tabs").ui == "main:%d" % MAIN["settings"] and r.shot("tabs").pixels != r.shot("main").pixels,
          "ab: B again, the arrows on the top row: %s" % r.shot("tabs").ui)
    check(r.notes["resumed"]["ui"] == "", "ab: B again resumes the game: %s" % r.notes["resumed"]["ui"])


def test_autosave():
    """Autosave (Saving): a state each time the game has run that long, the
    menu's time not counted, to a new slot each time (nothing overwritten)
    or to the picked slot (the same one each time). Every 30 s here: 35 s
    of play, 40 s in the menu, 30 more of play make two autosaves."""
    script = """wait 1
press enter
wait 35
note first
press menu
wait 40
press esc
wait 0.5
note menu
wait 30
note second
quit
"""
    for name, new_slot in (("new_slot", 1), ("picked_slot", 0)):
        config = owner_config()
        config[59] = config[60] = 0   # no save when leaving, no load on start
        config[71] = 0x80000000 | 1 | (new_slot << 4)   # every 30 s
        r = Run("autosave_" + name, files={"gpsp_qol.cfg.tns": config}).run(REAL_COSTS + script)
        slots = [os.path.exists(r.save("Pattern.gba%d.svs.tns" % i)) for i in range(4)]
        want = [True, True, False, False] if new_slot else [True, False, False, False]
        check(slots == want, "autosave, %s: slots 0-3 saved: %s (want %s)" % (name, slots, want))
        check(r.notes["first"]["frame"] > 30 * 59, "autosave, %s: the game ran on: %s" % (name, r.notes["first"]))
    # Kept in the settings file (word 71) and read back; older files: off.
    config = owner_config()
    config[71] = 0x80000000 | 6 | (0 << 4)   # every 5 min, the picked slot
    r = Run("autosave_setting", files={"gpsp_qol.cfg.tns": config}).run("wait 1\npress enter\nwait 1\nquit\n")
    words = struct.unpack("<72I", open(r.save("gpsp.cfg.tns"), "rb").read()[:288])
    check(words[71] == 0x80000006, "autosave: the settings file keeps every 5 min, the picked slot: %08x" % words[71])


# The GBA's buttons in REG_P1 (0x4000130), a bit each, 0 while pressed.
GBA_BITS = {"Up": 0x040, "Down": 0x080, "Left": 0x020, "Right": 0x010, "A": 0x001, "B": 0x002,
            "L": 0x200, "R": 0x100, "Start": 0x008, "Select": 0x004}
GBA_ORDER = ["Up", "Down", "Left", "Right", "A", "B", "L", "R", "Start", "Select"]   # ACTION_UP ... ACTION_SELECT
LETTERS = {"A": 71, "B": 70, "C": 69, "D": 67, "E": 66, "G": 55, "H": 54, "I": 53, "J": 51, "K": 50,
           "M": 39, "N": 38, "O": 37, "P": 35, "R": 33, "T": 22, "U": 21, "V": 19, "W": 18, "X": 17}


def turbo_config(plain, turbo, speed=0):
    """This build's settings (82 words): the reference, the GBA buttons on the
    keys in 'plain' (10 key numbers, ACTION_UP ... ACTION_SELECT, 0 none) and
    their turbo on those in 'turbo', turbo at speed + 1 presses a second."""
    o = owner_config() + [0] * 10
    for i in range(10):
        o[38 + i] = plain[i]
        o[72 + i] = turbo[i]
    o[70] = 0x80000000 | 0x40000000 | (speed << 8)
    return o


def peeks(prefix, n=4, gap=0.5):
    return "".join("wait %s\npeek %s%d 4000130:2\n" % (gap, prefix, i) for i in range(n))


def pattern(run, prefix, bit, n=4):
    """'1' where the button was down at each peek."""
    return "".join("0" if struct.unpack("<H", open(os.path.join(run.out, "%s%d.bin" % (prefix, i)), "rb").read())[0]
                   & bit else "1" for i in range(n))


def others(run, prefix, bit, n=4):
    """The other buttons down at any of the peeks (bits)."""
    down = 0
    for i in range(n):
        down |= ~struct.unpack("<H", open(os.path.join(run.out, "%s%d.bin" % (prefix, i)), "rb").read())[0] & 0x3FF
    return down & ~bit


TURBO = ("1010", "0101")


def test_turbo():
    """Turbo is a binding of its own (2026-11-12): on the Controls screen,
    A's line, tab, then a key: that key is Turbo A, beside A's own key.
    Held alone the turbo key makes A go down and up at the speed set (1 a
    second here: peeks every 0.5 s see it by turns), A's key alone holds
    it down, both together hold it down; the D-pad's turbo the same way.
    The settings keep it; Speed's Turbo rate is four choices (10/s to 30/s, r19), so the 1 a second here comes
    from the settings file (a value from an older build, said as it is)."""
    prep = Run("turbo_prep")
    prep.run("wait 1\npress enter\nwait 1\npress Q\nwait 1\nquit\n")
    words = read_words(prep.save("gpsp.cfg.tns"))
    words[70] &= ~0xFF00   # turbo: 1 a second
    r = Run("turbo", files={"gpsp.cfg.tns": words}).run("""
wait 1
press enter
wait 1
press menu
wait 0.3
""" + to_main("controls") + """press enter
wait 0.3
""" + "press down\n" * 4 + """wait 0.2
shot a_line
press tab
wait 0.2
shot a_turbo_capture
press =
wait 0.3
shot a_turbo_set
press up
press up
press up
press up
press tab
wait 0.2
press K
wait 0.3
shot up_turbo_set
press esc
press esc
press esc
wait 0.5
hold del
wait 0.25
""" + peeks("plain") + """release del
wait 0.5
hold =
wait 0.25
""" + peeks("turbo") + """hold del
""" + peeks("both") + """release del
""" + peeks("turbo_again") + """release =
wait 0.5
hold K
wait 0.25
""" + peeks("up") + """release K
wait 0.5
quit
""")
    check(r.shot("a_line").active_menu_line("keys") == 4, "turbo: A's line: %s" % r.shot("a_line").ui)
    check(r.shot("a_turbo_capture").ui == "keys_turbo:4", "turbo: tab waits for A's turbo key: %s"
          % r.shot("a_turbo_capture").ui)
    check(r.shot("a_turbo_set").ui == "keys:4" and r.shot("up_turbo_set").ui == "keys:0",
          "turbo: set, back on the list: %s %s" % (r.shot("a_turbo_set").ui, r.shot("up_turbo_set").ui))
    a = GBA_BITS["A"]
    check(pattern(r, "plain", a) == "1111", "turbo: A's key (del) alone: A held down: %s" % pattern(r, "plain", a))
    check(pattern(r, "turbo", a) in TURBO, "turbo: the turbo A key (=) alone: A down and up by turns: %s"
          % pattern(r, "turbo", a))
    check(pattern(r, "both", a) == "1111", "turbo: both held: A down: %s" % pattern(r, "both", a))
    check(pattern(r, "turbo_again", a) in TURBO, "turbo: A's key let go, = still held: by turns again: %s"
          % pattern(r, "turbo_again", a))
    check(pattern(r, "up", GBA_BITS["Up"]) in TURBO and not others(r, "up", GBA_BITS["Up"]),
          "turbo: K, turbo up: up by turns, nothing else: %s" % pattern(r, "up", GBA_BITS["Up"]))
    words = struct.unpack("<82I", open(r.save("gpsp.cfg.tns"), "rb").read())
    check(words[72 + 4] & 0xFFFF == 72 and words[72] & 0xFFFF == LETTERS["K"] and (words[70] >> 8) & 0xFF == 0 and
          words[38 + 4] & 0xFFFF == KEYS["del"], "turbo: the settings keep turbo A on =, turbo up on K, 1 a second, "
          "A on del: %08x %08x %08x" % (words[76], words[72], words[70]))


def test_turbo_every_button():
    """Each of the ten GBA buttons on a key of its own and its turbo on
    another (a settings file of this build): held, the plain key holds its
    button down and nothing else; the turbo key presses its button by turns
    at the speed set, nothing else. At 15 a second the turbo button is down
    half the frames; at 30, every other frame."""
    plain = [LETTERS[k] for k in "ABCDEGHIJK"]
    turbo = [LETTERS[k] for k in "MNOPRTUVWX"]
    script = "wait 1\npress enter\nwait 1\n"
    for i, name in enumerate(GBA_ORDER):
        script += "hold %s\nwait 0.25\n%srelease %s\nwait 0.3\n" % ("ABCDEGHIJK"[i], peeks("p%d_" % i), "ABCDEGHIJK"[i])
        script += "hold %s\nwait 0.25\n%srelease %s\nwait 0.3\n" % ("MNOPRTUVWX"[i], peeks("t%d_" % i), "MNOPRTUVWX"[i])
    r = Run("turbo_every", files={"gpsp_qol.cfg.tns": turbo_config(plain, turbo)}).run(script + "quit\n")
    for i, name in enumerate(GBA_ORDER):
        bit = GBA_BITS[name]
        check(pattern(r, "p%d_" % i, bit) == "1111" and not others(r, "p%d_" % i, bit),
              "every button: %s on %s: held down, alone: %s %x" % (name, "ABCDEGHIJK"[i], pattern(r, "p%d_" % i, bit),
                                                                   others(r, "p%d_" % i, bit)))
        check(pattern(r, "t%d_" % i, bit) in TURBO and not others(r, "t%d_" % i, bit),
              "every button: turbo %s on %s: by turns, alone: %s %x" % (name, "MNOPRTUVWX"[i],
                                                                       pattern(r, "t%d_" % i, bit), others(r, "t%d_" % i, bit)))
    # the speed: frames down out of 120 game frames in a row (peeks a frame apart)
    for speed, want in ((14, 30), (29, 30), (0, 30)):
        fr = Run("turbo_speed_%d" % speed, files={"gpsp_qol.cfg.tns": turbo_config(plain, turbo, speed)}).run(
            "wait 1\npress enter\nwait 1\nhold M\nwait 0.1\nframes 1\n" +
            "".join("peek f%d 4000130:2\nframes 1\n" % i for i in range(120)) + "release M\nquit\n")
        got = "".join("0" if struct.unpack("<H", open(os.path.join(fr.out, "f%d.bin" % i), "rb").read())[0] & 0x40
                      else "1" for i in range(120))
        presses = sum(1 for i in range(1, 120) if got[i] == "1" and got[i - 1] == "0") + (got[0] == "1")
        check(got.count("1") in (want * 2 - 1, want * 2, want * 2 + 1) and abs(presses - (speed + 1) * 2) <= 1,
              "turbo speed %d a second: %d of 120 frames down, %d presses: %s" % (speed + 1, got.count("1"), presses, got))


def test_turbo_old_settings():
    """A settings file of the old scheme (72 words): the Turbo choice "A"
    (word 70 bits 0-3 = 2) and the Turbo key = (word 69). A's key (del) is
    turbo A now; B's key (var) still B, and = held with var turbo B. The
    file is written back in this build's 82 words, the choice kept."""
    config = owner_config()
    config[69] = 72   # the Turbo key: = (key 72)
    config[70] = 0x80000000 | (0 << 8) | 2   # on for A, 1 a second
    r = Run("turbo_old", files={"gpsp_qol.cfg.tns": config}).run(
        "wait 1\npress enter\nwait 1\nhold del\nwait 0.25\n" + peeks("a") + "release del\nwait 0.3\nhold var\nwait 0.25\n" +
        peeks("b") + "hold =\nwait 0.1\n" + peeks("tb") + "release =\nrelease var\nwait 0.3\npress menu\nwait 0.3\npress esc\n"
        "wait 0.5\nquit\n")
    check(pattern(r, "a", GBA_BITS["A"]) in TURBO, "old settings, turbo on A: del is turbo A: %s" % pattern(r, "a", 1))
    check(pattern(r, "b", GBA_BITS["B"]) == "1111", "old settings: var still B: %s" % pattern(r, "b", 2))
    check(pattern(r, "tb", GBA_BITS["B"]) in TURBO, "old settings: = held with var: turbo B: %s" % pattern(r, "tb", 2))
    data = open(r.save("gpsp.cfg.tns"), "rb").read()
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    check(len(data) == 328 and words[70] & 0xF == 2 and words[70] >> 30 == 3 and words[72 + 4] & 0xFFFF == KEYS["del"] and
          words[72 + 5] == KEYS["var"] | (72 << 8), "old settings: written as 82 words: %d bytes, %08x %08x %08x"
          % (len(data), words[70] if len(words) > 70 else 0, words[76] if len(words) > 76 else 0,
             words[77] if len(words) > 77 else 0))


def osd_toasts(r):
    """The toasts of a run, in order (out/pad.txt's osd lines), each once in a row."""
    out = []
    for line in open(os.path.join(r.out, "pad.txt"), errors="replace"):
        if "osd " in line:
            text = line.split("osd ", 1)[1].strip()
            if text and (not out or out[-1] != text):
                out.append(text)
    return out


def link_log(r):
    path = r.save("gpsp_link.txt.tns")
    return open(path, errors="replace").read() if os.path.exists(path) else ""


def test_save17_memory():
    """save17: states would not save (out of memory or storage). The CX II's run: a state resumed, then every save
    failed ("out of space?": the minute's autosave 60 s after the resume, a
    new slot's, the menu's) and a later load said "That save can't be
    loaded". On r16 each save needed ~0.95 MB of fresh memory, a load ~0.5
    MB. The heap here has one absolute size for every build: what r16 holds
    once the game runs plus 3.5 MB. (1) Someone else (hog) then takes all
    but 64 KB of what r16 would leave: it gets 1 MB less now (the saves'
    kept memory: hog_short says so), and the autosaves, S, the menu's Save
    and Load and the game's own save all work, the heap the same after each
    round. (2) The heap in pieces (no block over 128 KB) with plenty free in
    all: the same. (3) Too little at the start with the configured cache:
    shrink the cache and keep the workspace. Fragmentation before that
    reservation can still prevent it: "not enough memory" (the toast, the
    dialog, the link log's lines with step and sizes). (4) A full calculator
    says what the OS took; (5) a file that can't be made says that. Each
    message keeps to the toast band's one line (53 characters)."""
    KEPT = 425984 + 454839 + 144576     # the state's buffer, the file's, the packer's (nspire_states.c)
    cfg = owner_config()
    cfg[71] = 0x80000000 | 2           # autosave every minute, to the picked slot
    first = Run("save17", files={"gpsp_qol.cfg.tns": cfg}).run(
        REAL_COSTS + "wait 1\npress enter\nwait 3\npress S\nwait 2\npress Q\nwait 1\nquit\n")
    times = open(first.save("gpsp_states_time.txt.tns")).read()   # (written by gpSP's own quit: Q)
    check(re.search(r"saves' memory kept, after at \d+ s: largest \d+", times) is not None and
          "states:" not in link_log(first),
          "save17: the game's start keeps the saves' memory (gpsp_states_time.txt.tns says so and how much is left, "
          "the link log nothing): %s" % [l for l in times.splitlines() if "memory" in l or "Memory" in l][:3])
    check(re.search(r"save slot 1 at \d+ s: largest \d+( or more)?; USB drive's driver \d+$", times, re.M) is not None,
          "save17: each save looks at the memory free: %s" % [l for l in times.splitlines() if "slot" in l][:4])
    probe = Run("save17_probe", reuse=first).run(REAL_COSTS + "wait 1\npress enter\nwait 2\nnote resumed\nquit\n")
    base = int(probe.notes["resumed"]["heap"]) - KEPT    # what r16 held with the game running
    limit = base // 1024 + 3584                          # one absolute heap for every build
    hog = limit - base // 1024 - 64                      # all but 64 KB of what r16 would have left

    # (r19: the paused menu opens on SAVES: the SAVE tile is right, enter (over the picked slot: asks, Replace is right, enter); a state loads with enter: Resume, Save, the game's own
    # save, then the states)
    menu_save = "press menu\nwait 0.5\n" + SAVE_NEW + "wait 1\npress right\npress enter\nwait 1.5\nshot saved{n}\n"
    menu_load = "press menu\nwait 0.5\n" + load_state(0, game_save=True) + "wait 1.5\nshot loaded{n}\n"
    rounds = "".join("wait 61\npoke 0E000000:%02X\nwait 1\npress S\nwait 2\n" % (0x30 + n) + menu_save.format(n=n) +
                     menu_load.format(n=n) + "note round%d\n" % n for n in range(3))
    seen = []
    for name, squeeze in (("short", "hog %d\n" % hog), ("pieces", "frag 128\n")):
        r = Run("save17_" + name, reuse=first).run(REAL_COSTS + "wait 1\npress enter\nwait 2\nnote resumed\n" + squeeze +
                                                   rounds + "quit\n", options=["--heap-limit", str(limit)])
        toasts = osd_toasts(r)
        seen += toasts
        check(not [t for t in toasts if t.startswith("Couldn't")], "save17 %s: nothing fails: %s" % (name, toasts))
        for want in ("Resumed from slot 1", "Autosaved to slot 1", "Saved to slot 1", "Loaded slot 1"):
            check(want in toasts, "save17 %s: %r: %s" % (name, want, toasts))
        for n in range(3):
            check(r.shot("saved%d" % n).ui == "" and r.shot("loaded%d" % n).ui == "",
                  "save17 %s: the menu's Save and Load went back to the game, round %d: %s, %s" %
                  (name, n, r.shot("saved%d" % n).ui, r.shot("loaded%d" % n).ui))
        heaps = [r.notes["round%d" % n]["heap"] for n in range(3)]
        check(heaps[0] == heaps[1] == heaps[2], "save17 %s: gpSP's heap the same after each round: %s" % (name, heaps))
        sav = [f for f in os.listdir(os.path.dirname(r.save("x"))) if f.startswith("Pattern.gba.sav")]
        check(sav and "Couldn't write game save" not in " ".join(toasts),
              "save17 %s: the game's own save written: %s" % (name, sav))
        if name == "short":
            # the cost, seen: someone else gets the 1 MB the saves keep
            short = int(r.report["hog_short"])
            check(abs(short - KEPT) < 128 * 1024, "save17: the hog got %d bytes less than on r16 (the kept %d)" %
                  (short, KEPT))

    # (3) a cache that crowds out saves shrinks; truly fragmented startup still says why it cannot save.
    r = Run("save17_margin", reuse=first).run(REAL_COSTS + "wait 1\npress enter\nwait 2\npress S\nwait 2\nquit\n",
                                              options=["--heap-limit", str(base // 1024 + 1300)])
    check("NOT KEPT" not in link_log(r) and "Saved to slot 1" in osd_toasts(r) and
          "saves' memory kept" in open(r.save("gpsp_states_time.txt.tns")).read(),
          "save18: the cache yields room for saves and the 512 KB margin: %s, %s" %
          (re.findall(r"states: .*", link_log(r)), osd_toasts(r)))
    menu_dialog = "press menu\nwait 0.5\n" + SAVE_NEW + "wait 1\npress right\npress enter\nwait 1\nshot failed\nquit\n"
    r = Run("save17_no_room", reuse=first).run(REAL_COSTS + "wait 1\nfrag 128\npress enter\nwait 2\npress S\nwait 2\n" + menu_dialog,
                                               options=["--heap-limit", str(base // 1024 + 300)])
    toasts = osd_toasts(r)
    seen += toasts
    check("Couldn't resume from slot 1: not enough memory" in toasts and
          "Couldn't save to slot 1: not enough memory" in toasts, "save17: no memory says so: %s" % toasts)
    check(r.shot("failed").ui == "dialog:0", "save17: the menu's Save says it couldn't: %s" % r.shot("failed").ui)
    log = link_log(r)
    check(re.search(r"states: memory for saves and loads NOT KEPT", log) and
          re.search(r"states: save slot 1 failed: memory, at the state's copy; errno \d+; needs 425984 bytes; "
                    r"the largest free block \d+", log) and
          re.search(r"states: load slot 1 failed: memory", log),
          "save17: the link log says memory, the step and the sizes: %s" % re.findall(r"states: .*", log))

    # (4) the calculator full: what the OS took before it said no
    r = Run("save17_full", reuse=first).run(REAL_COSTS + "wait 1\npress enter\nwait 2\npress S\nwait 2\nquit\n",
                                            options=["--free-storage", "12"])
    full = [re.match(r"Couldn't save to slot 1: storage full, (\d+) KB free$", t) for t in osd_toasts(r)]
    full = [int(m.group(1)) for m in full if m]
    seen += osd_toasts(r)
    check(full and full[0] <= 12, "save17: a full calculator says so, with what is free: %s" % osd_toasts(r))

    # (5) a file that can't be made (the OS's open files all taken)
    r = Run("save17_files", reuse=first).run(REAL_COSTS + "wait 1\npress enter\nwait 2\npress S\nwait 2\nquit\n",
                                             options=["--max-files", "1"])
    check("Couldn't save to slot 1: file create failed" in osd_toasts(r),
          "save17: a file that can't be made says so: %s" % osd_toasts(r))
    seen += osd_toasts(r)
    failures_said = [t for t in seen if t.startswith("Couldn't")]
    check(failures_said and max(len(t) for t in failures_said) <= 53,
          "save17: the failures' toasts keep to the band's one line (53 characters): %s" % sorted(set(failures_said)))

def test_save18_startup_memory():
    """A 16 MB ROM cache must not make saves unusable when the game starts.
    The same heap that left r17 only 300 KB after loading a ROM still has
    enough memory if disposable ROM pages give way to the save workspace.
    Both ordinary and compressed 16 MB ROMs resume, save, load and autosave;
    the state restores actual RAM bytes, and the cache choice stays.
    """
    kept = 425984 + 454839 + 144576
    cfg = owner_config()
    cfg[71] = 0x80000000 | 2
    for kind in ("gba", "gbz"):
        seed = Run("save18_startup_" + kind + "_seed", files={"gpsp_qol.cfg.tns": cfg})
        rom = seed.file("Pattern.gba.tns")
        with open(rom, "ab") as f:
            f.write(b"\xff" * (16 * 1024 * 1024 - f.tell()))
        if kind == "gbz":
            subprocess.run([sys.executable, "-I", os.path.join(HERE, "..", "..", "tools", "gbz.py"),
                            "pack", "-o", seed.gba, rom], check=True, capture_output=True)
            os.remove(rom)
        seed.run(REAL_COSTS + "wait 1\npress enter\nwait 3\npoke 2000100:5A\npress S\nwait 2\n"
                 "note baseline\npress Q\nwait 1\nquit\n")
        check("Saved to slot 1" in osd_toasts(seed), "save18 %s: seed state exists" % kind)
        limit = (int(seed.notes["baseline"]["heap"]) - kept) // 1024 + 300
        r = Run("save18_startup_" + kind, reuse=seed).run(
            REAL_COSTS + "wait 1\npress enter\nwait 2\npeek resumed 2000100:1\n"
            "poke 2000100:33\npress S\nwait 2\npoke 2000100:99\npress L\nwait 2\n"
            "peek loaded 2000100:1\nnote running\npoke 0E000000:5A\nwait 62\n"
            "press Q\nwait 1\nquit\n", options=["--heap-limit", str(limit)])
        toasts = osd_toasts(r)
        check(not [t for t in toasts if t.startswith("Couldn't")],
              "save18 %s: tight startup saves work: %s" % (kind, toasts))
        for want in ("Resumed from slot 1", "Saved to slot 1", "Loaded slot 1", "Autosaved to slot 1"):
            check(want in toasts, "save18 %s: %s: %s" % (kind, want, toasts))
        for name, want in (("resumed", b"\x5a"), ("loaded", b"\x33")):
            got = open(os.path.join(r.out, name + ".bin"), "rb").read()
            check(got == want, "save18 %s: %s restores RAM: %s" % (kind, name, got.hex()))
        times = open(r.save("gpsp_states_time.txt.tns")).read()
        check("saves' memory kept" in times and "NOT KEPT" not in link_log(r),
              "save18 %s: workspace kept with a smaller ROM cache" % kind)
        words = read_words(r.save("gpsp.cfg.tns"))
        check(words[31] == 16, "save18 %s: user's 16 MB choice stays: %d" % (kind, words[31]))
        check(any(n.startswith("Pattern.gba.sav") for n in os.listdir(os.path.dirname(r.save("x")))),
              "save18 %s: battery save written under the same heap limit" % kind)


def test_save18_switch_memory():
    """A trimmed local cache grows for a whole ZIP and shrinks for local play again.
    The ZIP cannot page, so its mandatory ROM allocation takes precedence;
    coming back must restore the state and reserve the save workspace again.
    """
    kept = 425984 + 454839 + 144576
    seed = Run("save18_switch_seed", roms=("Alpha",), files={"gpsp_qol.cfg.tns": owner_config()})
    with open(seed.file("Alpha.gba.tns"), "ab") as f:
        f.write(b"\xff" * (16 * 1024 * 1024 - f.tell()))
    with zipfile.ZipFile(seed.file("Zulu.zip.tns"), "w", zipfile.ZIP_DEFLATED) as z:
        z.write(seed.file("Alpha.gba.tns"), "Zulu.gba")
    seed.run(REAL_COSTS + "wait 1\npress enter\nwait 3\npoke 2000100:5A\npress S\nwait 2\n"
             "note baseline\npress Q\nwait 1\nquit\n")
    limit = (int(seed.notes["baseline"]["heap"]) - kept) // 1024 + 500
    switch = "press menu\nwait 0.5\n" + to_main("game_list") + "press enter\nwait 0.5\n%s" + "wait 4\n"
    r = Run("save18_switch", reuse=seed).run(
        REAL_COSTS + "wait 1\npress enter\nwait 2\nnote local\n" + switch % pick(2) +
        "note zipped\nwait 1\nnote zip_running\n" + switch % pick(1) +
        "note back\npeek restored 2000100:1\npress S\nwait 2\npress Q\nwait 1\nquit\n",
        options=["--heap-limit", str(limit)])
    n = r.notes
    check(n["local"]["game"] == "Alpha.gba.tns" and int(n["local"]["rom_mb"]) < 16,
          "save18 switch: local cache shrank: %s" % n["local"])
    check(n["zipped"]["game"] == "Zulu.zip.tns" and int(n["zipped"]["rom_mb"]) == 16 and
          n["zip_running"]["frame"] > n["zipped"]["frame"],
          "save18 switch: the ZIP grows to its full ROM and plays: %s" % n["zipped"])
    check(n["back"]["game"] == "Alpha.gba.tns" and int(n["back"]["rom_mb"]) < 16,
          "save18 switch: local cache shrinks again: %s" % n["back"])
    check(open(os.path.join(r.out, "restored.bin"), "rb").read() == b"\x5a",
          "save18 switch: coming back restores the actual saved RAM")
    check("Saved to slot 1" in osd_toasts(r) and not [t for t in osd_toasts(r) if t.startswith("Couldn't")],
          "save18 switch: local save still works after the ZIP: %s" % osd_toasts(r))


def owner_library(name, games=629, played=40, states=1878):
    """A calculator with a library like the reference (gpsp_states_time.txt.tns: 629 games, 1,878 states): the
    pattern ROM under 629 long names, 1,878 states (one real one, linked) over 40 of them, in the saves folder."""
    seed = Run(name + "_seed").run(REAL_COSTS + "wait 1\npress enter\nwait 2\npress S\nwait 2\nquit\n")
    r = Run(name, roms=())
    rom = os.path.join(r.dir, "pattern.rom")
    pattern_rom(rom)
    titles = ("Pokemon", "Golden Sun", "Fire Emblem", "Advance Wars", "Zelda")
    names = ["%s %03d - %s (USA, Europe)" % (titles[i % 5], i, "The Long Subtitle Of The Game"[:10 + i % 18])
             for i in range(games)]
    for n in names:
        os.link(rom, os.path.join(r.gba, n + ".gba.tns"))
    saves = os.path.dirname(r.save("x"))
    os.makedirs(saves, exist_ok=True)
    order = sorted(names)
    for k in range(states):
        game = order[(k % played) * (games // played)]
        os.link(seed.save("Pattern.gba0.svs.tns"), os.path.join(saves, "%s.gba%d.svs.tns" % (game, k // played)))
    return r


def test_save17_library():
    """r17: gpSP's heap with the reference library (629 games, 1,878 states over 40 of them): what grows with it
    is the saves folder's names (~120 KB), the game list's names (~50 KB, while the list is up) and the menus'
    pictures (12 at most, 27 KB each), all bounded. A game's start that would leave less than 512 KB after the
    saves' memory gives up those caches first, and keeps it (the link log says so)."""
    KEPT = 425984 + 454839 + 144576
    lib = owner_library("save17_library")
    # (r19: the grid's 210 rows, down from the first to the last and back up; up from the first row would leave
    # for the tabs; in the paused menu down into Resume's tile, right through the other games played lately)
    walk = "wait 2\nnote list\n" + "press down\nwait 0.3\n" * 209 + "note scrolled\n" + "press up\nwait 0.2\n" * 209 + \
           "press enter\nwait 3\nnote game\npress menu\nwait 1\npress down\n" + "press right\nwait 0.4\n" * 20 + \
           "note menu\npress esc\npress esc\nwait 1\npress S\nwait 2\nnote saved\nquit\n"
    r = Run("save17_library_walk", reuse=lib).run(REAL_COSTS + walk)
    n = {k: int(r.notes[k]["heap"]) for k in ("list", "scrolled", "game", "menu", "saved")}
    print("  heap with the reference library: %s" % n)
    check(n["scrolled"] - n["list"] <= 12 * 27648 + 16384, "library: scrolling it all keeps at most 12 pictures: +%d bytes" %
          (n["scrolled"] - n["list"]))
    # (r19: the paused menu is SAVES, tiles with the states' pictures: at most 12 of them, 27 KB each, while it is
    # up, and given back after it; the menus' caches used to be built later)
    check(n["game"] - n["scrolled"] <= KEPT and n["saved"] == n["game"] and 0 <= n["menu"] - n["game"] <= 12 * 27648,
          "library: the game's start adds the saves' memory and no more, the menu at most 12 pictures (given back), "
          "a save nothing: %s" % n)
    check(r.notes["game"]["ui"] == "" and "Resumed from slot" in " ".join(osd_toasts(r)), "library: the game resumed: %s" %
          osd_toasts(r))
    # the same walk with what is left after the keep under 512 KB unless the caches go
    limit = n["scrolled"] // 1024 + KEPT // 1024 + 300
    r = Run("save17_library_tight", reuse=lib).run(REAL_COSTS + walk, options=["--heap-limit", str(limit)])
    check("states: memory for saves and loads kept after the menus' caches were given up" in link_log(r) and
          "Saved to slot" in " ".join(osd_toasts(r)) and not [t for t in osd_toasts(r) if t.startswith("Couldn't")],
          "library: tight: the caches given up for the saves' memory, and saves work: %s %s" %
          (re.findall(r"states: .*", link_log(r)), osd_toasts(r)))


def test_save17_review2():
    """save17. N1: the memory looks hold one block at a time, 1 MB at most: an allocation of
    someone else's (256 KB: a dongle, the drive, the link) at any of gpSP's allocations, with 1.5 MB free
    after the saves' memory, always fits (--concurrent). N2: S while the game's own save has the kept
    buffers and there's no memory of its own: the state waits for them a few frames (the game's save a
    part a frame, its OS calls served) and is saved. N3: a short write with no errno (Ndless often sets
    none) in a game's frame is "file write failed", not full; the menu's save, outside a frame, measures
    the storage and says full."""
    KEPT = 425984 + 454839 + 144576
    cfg = owner_config()
    first = Run("save17r2", files={"gpsp_qol.cfg.tns": cfg}).run(
        REAL_COSTS + "wait 1\npress enter\nwait 2\npress S\nwait 2\npress Q\nwait 1\nquit\n")
    probe = Run("save17r2_probe", reuse=first).run(REAL_COSTS + "wait 1\npress enter\nwait 2\nnote resumed\nquit\n")
    held = int(probe.notes["resumed"]["heap"])
    menu_save = "press menu\nwait 0.5\n" + SAVE_NEW + "wait 1\npress right\npress enter\nwait 1.5\n"
    menu_load = "press menu\nwait 0.5\n" + load_state(0, game_save=True) + "wait 1.5\n"
    r = Run("save17r2_concurrent", reuse=first).run(
        REAL_COSTS + "wait 1\npress enter\nwait 2\npress S\nwait 2\n" + menu_save + menu_load +
        "press menu\nwait 11\npress esc\nwait 1\npress menu\nwait 1\npress esc\nwait 1\nquit\n",
        options=["--heap-limit", str(held // 1024 + 1536), "--concurrent", "256"])
    check(r.report.get("concurrent_short") == "0" and not [t for t in osd_toasts(r) if t.startswith("Couldn't")],
          "save17 N1: someone else's 256 KB always fits beside the memory looks: short %s times; %s" %
          (r.report.get("concurrent_short"), osd_toasts(r)))
    r = Run("save17r2_wait", reuse=first).run(
        REAL_COSTS + "wait 1\npress enter\nwait 2\nfrag 128\npoke 0E000000:5A\nwait 0.18\npress S\nwait 3\npress Q\nwait 1\nquit\n")
    times = open(r.save("gpsp_states_time.txt.tns")).read()
    check(re.search(r"save slot 1 waited \d+ frames for the game's save", times) and "Saved to slot 1" in osd_toasts(r)
          and not [t for t in osd_toasts(r) if t.startswith("Couldn't")],
          "save17 N2: the state waited for the game's save and was saved: %s %s" %
          ([l for l in times.splitlines() if "waited" in l], osd_toasts(r)))
    r = Run("save17r2_silent", reuse=first).run(
        REAL_COSTS + "wait 1\npress enter\nwait 2\npress S\nwait 2\n" + menu_save + "shot told\nquit\n",
        options=["--free-storage", "12", "--storage-errno", "0"])
    check("Couldn't save to slot 1: file write failed" in osd_toasts(r) and
          not [t for t in osd_toasts(r) if "storage full" in t],
          "save17 N3: a short write with no errno in a frame is a write failure: %s" % osd_toasts(r))
    check(r.shot("told").ui == "dialog:0" and re.search(r"states: save slot 1 failed: storage full, at write; errno 0",
                                                        link_log(r)),
          "save17 N3: the menu's save measures the storage and says full: %s" % re.findall(r"states: .*", link_log(r)))


def test_save17_game_save():
    """The game's own save (its battery memory, nspire_backup.c). r16/r17's host runs: 137 of 693 ended
    with "Couldn't write the game's save (storage full?)": a game with no save memory (the pattern ROM,
    and any game before its first save) was "written" at each quit with Compress saves on, and that
    failed. Now there is nothing to write and nothing said. A save that does fail says why, as a state's
    does: full storage the OS says so (ENOSPC), or measured outside a frame; a write failure in a frame."""
    cfg = owner_config()   # (Compress saves on)
    r = Run("save17_gs_none", files={"gpsp_qol.cfg.tns": cfg}).run(
        REAL_COSTS + "wait 1\npress enter\nwait 2\npress Q\nwait 1\nquit\n")
    names = os.listdir(os.path.dirname(r.save("x")))
    check(not [t for t in osd_toasts(r) if t.startswith("Couldn't")] and not [n for n in names if ".sav" in n],
          "game save: no save memory, nothing written, nothing said at the quit: %s %s" % (osd_toasts(r), names))
    # the game saves in a frame (poke) with 1 KB of storage: the OS says ENOSPC: full, and what it took (the link
    # log's line can't be written on a calculator that full)
    r = Run("save17_gs_full", files={"gpsp_qol.cfg.tns": cfg}).run(
        REAL_COSTS + "wait 1\npress enter\nwait 2\npoke 0E000000:5A\nwait 1\nquit\n", options=["--free-storage", "1"])
    said = [t for t in osd_toasts(r) if t.startswith("Couldn't write game save")]
    check(said and re.match(r"Couldn't write game save: storage full, \d+ KB free$", said[0]) and len(said[0]) <= 53,
          "game save: storage full says so, with what the OS took: %s" % osd_toasts(r))
    # the same with no errno from the OS: in a frame a write failure, not a guess of full
    r = Run("save17_gs_silent", files={"gpsp_qol.cfg.tns": cfg}).run(
        REAL_COSTS + "wait 1\npress enter\nwait 2\npoke 0E000000:5A\nwait 1\nquit\n",
        options=["--free-storage", "1", "--storage-errno", "0"])
    check("Couldn't write game save: file write failed" in osd_toasts(r),
          "game save: a silent short write in a frame is a write failure: %s" % osd_toasts(r))


def test_gui_ab():
    """ui18 part 2: GBA A/B act as Enter/Esc across frontend screens, menus, Settings, dialogs, first-run hints,
    speed test and loading cancel (both calc bindings and USB gamepad A/B), except raw capture on Keys (still
    captures var; conflict prompt accepts del/var) and the in-game link cable wait screen (Esc-only)."""
    # 1. Menus, Settings grid/pages, editing/cancel, dialogs, Keys capture + conflict prompt, speed test (calc + pad)
    r = Run("gui_ab_menu").run("""wait 1
press del
wait 1.5
note started
pad in xinput
wait 0.4
press menu
wait 0.3
""" + to_setting("size") + """wait 0.3
shot pic_pad
press Pad_A
wait 0.2
shot pic_flip
press Pad_A
press Pad_A
wait 0.2
shot pic_round
press left
wait 0.2
shot pic_range
press Pad_A
wait 0.2
press right
wait 0.2
shot pic_edit
press Pad_B
wait 0.2
shot pic_cancel
press Pad_B
wait 0.3
shot main_back
press down
""" + page_moves(SETTINGS["brightness"], SETTINGS["more"]) + """press del
wait 0.3
""" + page_moves(0, PAGES["more"].index("Game")) + """press enter
wait 0.3
shot restart_q
press var
wait 0.3
shot restart_no
press Pad_A
wait 0.3
press right
press Pad_A
wait 0.6
note restarted
press menu
wait 0.3
""" + to_setting("keys") + """press Pad_A
wait 0.3
press down
press down
press down
press down
press down
press down
wait 0.2
press enter
wait 0.2
press var
wait 0.3
shot keys_conflict
press del
wait 0.3
shot keys_after_conflict
press up
press up
wait 0.2
shot keys_a
press del
wait 0.2
shot keys_cleared
press enter
wait 0.2
press del
wait 0.3
press var
wait 0.3
quit
""")
    check(r.notes["started"]["game"] == "Pattern.gba.tns", "gui_ab: del launches game")
    check(r.shot("pic_pad").ui == shown_at("size"), "gui_ab: on Screen size: %s" % r.shot("pic_pad").ui)
    check(r.shot("pic_flip").ui == shown_at("size") and r.shot("pic_flip").pixels != r.shot("pic_pad").pixels and
          r.shot("pic_round").pixels == r.shot("pic_pad").pixels,
          "gui_ab: Pad_A on a setting of a few values is its next value (three of them round): %s" % r.shot("pic_flip").ui)
    check(r.shot("pic_edit").ui == shown_at("brightness") and r.shot("pic_edit").pixels != r.shot("pic_range").pixels and
          r.shot("pic_cancel").ui == shown_at("brightness") and r.shot("pic_cancel").pixels == r.shot("pic_range").pixels,
          "gui_ab: Pad_A/B edit & cancel a range (Brightness) in Picture: %s %s"
          % (r.shot("pic_edit").ui, r.shot("pic_cancel").ui))
    check(r.shot("main_back").ui == "main:%d" % MAIN["settings"], "gui_ab: Pad_B returns to the top row")
    check(r.shot("restart_q").ui == "dialog:0" and r.shot("restart_no").ui == "More:%d" % PAGES["more"].index("Game"),
          "gui_ab: var cancels Start over dialog: %s -> %s" % (r.shot("restart_q").ui, r.shot("restart_no").ui))
    check("Game restarted" in osd_toasts(r), "gui_ab: Pad_A confirms Start over dialog: %s" % osd_toasts(r))
    check(r.shot("keys_conflict").ui == "keys_clash:0" and r.shot("keys_after_conflict").ui == "keys:6",
          "gui_ab: capturing var triggers conflict prompt instead of cancelling screen, del answers Keep: %s -> %s"
          % (r.shot("keys_conflict").ui, r.shot("keys_after_conflict").ui))
    check(r.shot("keys_a").ui == "keys:4" and r.shot("keys_cleared").ui == "keys:4",
          "gui_ab: del on Controls row clears binding without leaving")

    # 2. First-run hints dismissed with var and Pad_B
    os.environ["GPSP_HOST_HINTS"] = "1"
    try:
        h1 = Run("gui_ab_hint_var").run("wait 1\npress enter\nwait 2\nshot hint\npress var\nwait 0.8\nnote play\nquit\n")
        h2 = Run("gui_ab_hint_pad").run("wait 1\npad in xinput\nwait 0.6\npress enter\nwait 2\nshot hint\npress Pad_B\nwait 0.8\nnote play\nquit\n")
    finally:
        del os.environ["GPSP_HOST_HINTS"]
    check(h1.shot("hint").ui == "keys_hint" and h1.notes["play"]["ui"] == "" and h1.notes["play"]["frame"] > 0,
          "gui_ab: var dismisses first-run hint")
    check(h2.shot("hint").ui == "keys_hint" and h2.notes["play"]["ui"] == "" and h2.notes["play"]["frame"] > 0,
          "gui_ab: Pad_B dismisses first-run hint")

    # 3. Loading cancel (tap var, var, held var guard, Pad_B) and "Couldn't start it" (Pad_B / var)
    lc = Run("gui_ab_load", roms=())
    big_rom(lc.file("Big.gba.tns"))
    tmp = lc.file("Inner.gba")
    big_rom(tmp, seed=2)
    with zipfile.ZipFile(lc.file("Packed.zip.tns"), "w", zipfile.ZIP_STORED) as z:
        z.write(tmp, "Packed.gba")
    os.remove(tmp)
    # (Big.gba.tns = 0, Packed.zip.tns = 1)
    lc.run(REAL_COSTS + """cost read 200
wait 1
press enter
wait 0.35
shot loading_rom
tap var
wait 0.4
shot cancelled_var
cost read 600
press right
press enter
wait 0.5
shot loading_zip
press var
wait 0.4
shot cancelled_zip
pad in xinput
wait 0.5
press enter
wait 0.6
hold Pad_B
wait 0.4
release Pad_B
wait 0.4
shot cancelled_pad
hold var
press enter
wait 4
release var
wait 0.5
note loaded_held
quit
""")
    check(lc.shot("loading_rom").ui == "loading" and lc.shot("cancelled_var").ui == "list:0",
          "gui_ab: tap var cancels ROM load: %s -> %s" % (lc.shot("loading_rom").ui, lc.shot("cancelled_var").ui))
    check(lc.shot("loading_zip").ui == "loading" and lc.shot("cancelled_zip").ui == "list:0",
          "gui_ab: press var cancels zip load: %s -> %s" % (lc.shot("loading_zip").ui, lc.shot("cancelled_zip").ui))
    check(not os.path.exists(lc.file("Packed.gbz.tns")), "gui_ab: cancelled zip left no .gbz")
    check(lc.shot("cancelled_pad").ui == "list:0",
          "gui_ab: Pad_B cancels ROM load: %s" % lc.shot("cancelled_pad").ui)
    check(lc.notes["loaded_held"]["game"] == "Big.gba.tns" and lc.notes["loaded_held"]["ui"] == "",
          "gui_ab: held var from before load does not cancel it")

    br = Run("gui_ab_broken", roms=())
    with open(br.file("Broken.gba.tns"), "wb") as f:
        f.write(b"not a valid rom")
    br.run("""wait 1
pad in xinput
wait 0.5
press enter
wait 0.5
shot stopped_pad
press Pad_B
wait 0.4
shot back_pad
press enter
wait 0.5
shot stopped_var
press var
wait 0.4
shot back_var
quit
""")
    check(br.shot("stopped_pad").ui == "stopped" and br.shot("back_pad").ui == "list:0",
          "gui_ab: Pad_B dismisses Couldn't start it: %s -> %s"
          % (br.shot("stopped_pad").ui, br.shot("back_pad").ui))
    check(br.shot("stopped_var").ui == "stopped" and br.shot("back_var").ui == "list:0",
          "gui_ab: var dismisses Couldn't start it: %s -> %s"
          % (br.shot("stopped_var").ui, br.shot("back_var").ui))

    # 4. Link cable wait screen keeps Esc-only (var does not unlink)
    runs = [Run("gui_ab_link_p%d" % p) for p in (1, 2)]
    sock = os.path.join(WORK, "gui_ab_link.sock")
    common = "cost frame 4\ncost send 0.1\ncost receive 0.1\nframes 30\nlink\nframes 10\n"
    s1 = common + "wait 2\npress var\nwait 2\nframes 30\nnote done\nquit\n"
    s2 = common + "press menu\nwait 6\npress esc\nwait 1\nframes 30\nquit\n"
    procs = [r.start(s, "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-device", "exact",
                                            "--link-delay", "20", "--link-slices"])
             for p, (r, s) in enumerate(zip(runs, (s1, s2)), 1)]
    for r, proc in zip(runs, procs):
        r.finish(proc)
    check("Linked" in runs[0].report.get("link", ""),
          "gui_ab: var during link wait screen does not unlink: %s" % runs[0].report.get("link"))


def test_save_load_carousel():
    """r19, the SAVES grid of the paused menu (it replaces ui18's Save/Load strips): Resume, Save, the game's own save
    ("In-game save"), then every state, the newest first, three across, nothing wraps (left and right stop at a
    row's ends, down from the last row stays); the picked tile is in colour with its name on a black block, the
    others stay where they are while the pick moves and the grid scrolls a row at the edge; enter on a state loads
    it, enter on the own save restarts from it, S on a state asks before saving over it, S held from the game into
    the menu saves nothing, S on Resume saves as Save does; the game's own save is never overwritten by S; the
    GBA's A and B act as enter and esc."""
    r = Run("carousel_seed")
    r.run(REAL_COSTS + "wait 1\npress enter\nwait 1.2\n" +
          "".join("wait 1\npress S\nwait 0.4\n" for _ in range(5)) + "quit\n")
    sav_bytes = bytes([0x55] + [i & 0xFF for i in range(32767)])
    with open(r.save("Pattern.gba.sav.tns"), "wb") as f:
        f.write(sav_bytes)
    sav_sha = hashlib.sha256(sav_bytes).hexdigest()

    def tile_patch(shot, col, row):
        x0 = 8 + col * 104
        y0 = 30 + 96 * row
        return [shot.at(x, y) for y in range(y0 + 2, y0 + 62, 4) for x in range(x0 + 4, x0 + 92, 4)]

    def plate(shot):
        """The picked tile's black block under its picture (rows 65-82 below the picture's top), as (col, row):
        a list of them, empty when the arrows are on the top row."""
        found = []
        for row in range(2):
            for c, (x0, x1) in enumerate(((4, 107), (108, 211), (212, 315))):
                if all(shot.at(x, 30 + 96 * row + 65) == BLACK for x in range(x0, x1 + 1)):
                    found.append((c, row))
        return found

    # 1. the grid: Resume 0, Save 1, In-game save 2; the states newest first: slot 5 is 3, 4 is 4, 3 is 5, 2 is 6
    a = Run("carousel_nav", reuse=r).run(REAL_COSTS + """wait 1
press enter
wait 2
press menu
wait 0.8
shot t0
press right
wait 0.3
shot t1
press right
wait 0.3
shot t2
press right
wait 0.3
shot t2_stop
press down
wait 0.3
shot t5
press left
press left
wait 0.3
shot t3
press left
wait 0.3
shot t3_stop
press down
wait 0.4
shot t6
press down
wait 0.3
shot t6_stop
press up
press up
wait 0.3
shot t0_again
press up
wait 0.3
shot row
quit
""")
    places = (("t0", "saves:0", (0, 0)), ("t1", "saves:1", (1, 0)), ("t2", "saves:2", (2, 0)),
              ("t2_stop", "saves:2", (2, 0)), ("t5", "saves:5", (2, 1)), ("t3", "saves:3", (0, 1)),
              ("t3_stop", "saves:3", (0, 1)), ("t6", "saves:6", (0, 1)), ("t6_stop", "saves:6", (0, 1)),
              )
    for name, ui, at in places:
        check(a.shot(name).ui == ui and plate(a.shot(name)) == [at],
              "carousel %s: picked %s, its block at %s: %s %s" % (name, ui, at, a.shot(name).ui, plate(a.shot(name))))
    # (t6: the grid scrolled a row, so the picked tile is in the lower row again; t0_again: two ups from slot 2's
    # tile, slot 5's tile and then Resume)
    check(a.shot("t0_again").ui == "saves:0" and plate(a.shot("t0_again")) == [(0, 0)],
          "carousel: up twice from the scrolled grid: %s %s" % (a.shot("t0_again").ui, plate(a.shot("t0_again"))))
    check(a.shot("row").ui == "main:%d" % MAIN["saves"] and plate(a.shot("row")) == [],
          "carousel: up from the top row of tiles: the arrows on the top row, no tile's block: %s %s"
          % (a.shot("row").ui, plate(a.shot("row"))))
    # a tile that isn't picked stays where it is while the pick moves: slot 4's tile (col 1, row 1) at t3 is
    # the same picture when the grid has scrolled a row (t6: it is in the upper row)
    check(tile_patch(a.shot("t3"), 1, 1) == tile_patch(a.shot("t6"), 1, 0),
          "carousel: the grid scrolled a row, the tiles unchanged")
    check(tile_patch(a.shot("t3"), 2, 1) == tile_patch(a.shot("t3_stop"), 2, 1),
          "carousel: left at a row's end: nothing moved")

    # 2. enter on a state loads it (slot 2 is the seventh tile); S on a state asks (esc keeps), Replace saves over it
    b = Run("carousel_load", reuse=r).run(REAL_COSTS + "wait 1\npress enter\nwait 2\npress menu\nwait 0.8\n" +
                                         load_state(3, game_save=True) + """wait 0.5
note loaded_older
press menu
wait 0.8
""" + saves_tile(1, game_save=True) + """wait 0.3
shot on_four
press S
wait 0.4
shot ask_over
press esc
wait 0.4
shot asked_esc
note asked_esc
press S
wait 0.4
press right
press enter
wait 0.6
note over_saved
quit
""")
    check(b.notes["loaded_older"]["ui"] == "" and "Loaded slot 2" in osd_toasts(b),
          "carousel: enter on an older state loads it, back in the game: %s" % osd_toasts(b))
    check(b.shot("on_four").ui == "saves:4" and b.shot("ask_over").ui == "dialog:0" and
          b.shot("asked_esc").ui == "saves:4" and
          questions(b) == [("Replace slot 4?", "esc"), ("Replace slot 4?", "Replace")],
          "carousel: S on a state asks first, esc keeps it, Replace saves over it: %s %s %s q=%s"
          % (b.shot("on_four").ui, b.shot("ask_over").ui, b.shot("asked_esc").ui, questions(b)))
    check("Saved over slot 4" in osd_toasts(b), "carousel: Replace saves over it with its toast: %s" % osd_toasts(b))

    # 3. S held from the game into the menu saves nothing there; S on Resume then saves as Save does; the own save
    # is never overwritten by S, enter on it restarts from it. (The files tell: the game's S makes the next slot,
    # a second save would make one more.)
    def slots(run):
        return sorted(f for f in os.listdir(os.path.dirname(run.save("x"))) if f.endswith(".svs.tns"))
    held_script = """wait 1
press enter
wait 3
hold S
wait 0.2
press menu
wait 1.5
shot s_still_held
release S
wait 1.5
shot released
%squit
"""
    c0 = Run("carousel_held", reuse=r).run(REAL_COSTS + held_script % "")
    c = Run("carousel_safety", reuse=r).run(REAL_COSTS + held_script % "press S\nwait 1\nnote after_release_s\n")
    check(c0.shot("released").ui == "saves:0" and
          len(slots(c0)) == len(slots(r)) + 1,
          "carousel: S held from the game into the menu saved once (the game's), nothing in the menu: %s %s, %d -> %d states"
          % (c0.shot("s_still_held").ui, c0.shot("released").ui, len(slots(r)), len(slots(c0))))
    check(c0.shot("s_still_held").ui == "saves:0",
          "carousel: the menu opens at once with S still held (the menu key pressed over it): %r" % c0.shot("s_still_held").ui)
    check(c.notes["after_release_s"]["ui"] == "" and len(slots(c)) == len(slots(r)) + 2,
          "carousel: a fresh S on Resume saves as Save does and goes back to the game: %r, %d -> %d states"
          % (c.notes["after_release_s"]["ui"], len(slots(r)), len(slots(c))))
    e = Run("carousel_own", reuse=r).run(REAL_COSTS + """wait 1
press enter
wait 3
note playing
press menu
wait 0.8
press right
press right
wait 0.3
shot on_game_save
press s
wait 0.8
shot after_s_on_sav
press enter
wait 0.5
shot asks_restart
note asked
press esc
wait 0.5
shot kept_playing
note kept_a
press enter
wait 0.5
press enter
wait 0.5
shot kept_b
note kept_b
press enter
wait 0.5
press right
press enter
wait 0.4
note after_restart
quit
""")
    check(e.shot("on_game_save").ui == "saves:2" and e.shot("after_s_on_sav").ui.startswith("saves:") and
          hashlib.sha256(open(e.save("Pattern.gba.sav.tns"), "rb").read()).hexdigest() == sav_sha,
          "carousel: s on the In-game save never overwrites the battery save: %s %s"
          % (e.shot("on_game_save").ui, e.shot("after_s_on_sav").ui))
    check(e.shot("asks_restart").ui == "dialog:0" and
          [t for t, _ in questions(e)][:1] == ["Restart from the in-game save?"],
          "carousel: enter on the In-game save asks first, the safe answer picked: %s %s"
          % (e.shot("asks_restart").ui, questions(e)))
    check(e.shot("kept_playing").ui == "saves:2" and e.shot("kept_b").ui == "saves:2" and
          e.notes["kept_a"]["frame"] > e.notes["playing"]["frame"] and "Loaded in-game save" not in osd_toasts(e)[:-1] and
          [a for _, a in questions(e)][:2] == ["esc", "Keep playing"],
          "carousel: esc and Keep playing leave the game alone, the tile still picked: %s %s %s"
          % (e.shot("kept_playing").ui, e.shot("kept_b").ui, questions(e)))
    check([a for _, a in questions(e)][2:] == ["Restart"] and e.notes["after_restart"]["ui"] == "" and
          e.notes["after_restart"]["frame"] < 30 and osd_toasts(e)[-1] == "Loaded in-game save",
          "carousel: Restart restarts from the in-game save (frame %d): %s %s"
          % (e.notes["after_restart"]["frame"], osd_toasts(e), questions(e)))

    # 4. The GBA's A is enter here (del with the default keys: Resume resumes, Save saves, a state loads, never a
    # question to delete), B is esc (straight back to the game); D deletes
    d = Run("carousel_ab", reuse=r).run(REAL_COSTS + """wait 1
press enter
wait 3
press menu
wait 0.8
press del
wait 0.6
note a_resumes
press menu
wait 0.8
""" + load_state(0, game_save=True).replace("press enter", "press del") + """wait 0.8
note a_loads
press menu
wait 0.8
press var
wait 0.5
note b_resumes
quit
""")
    check(d.notes["a_resumes"]["ui"] == "" and d.notes["a_loads"]["ui"] == "" and "Loaded slot 5" in osd_toasts(d) and
          questions(d) == [] and len(slots(d)) == len(slots(r)) and d.notes["b_resumes"]["ui"] == "",
          "carousel: GBA A (del) on Resume resumes, on a state loads it with no question to delete, B resumes at once: "
          "%r %r %r %s q=%s" % (d.notes["a_resumes"]["ui"], d.notes["a_loads"]["ui"], d.notes["b_resumes"]["ui"],
                                osd_toasts(d), questions(d)))
    # one esc (or the menu key) from the opened menu resumes; on the top row too
    for key in ("esc", "menu"):
        k = Run("carousel_%s" % key, reuse=r).run(REAL_COSTS + """wait 1
press enter
wait 3
press menu
wait 0.8
press %s
wait 0.6
note resumed
press menu
wait 0.8
press up
wait 0.3
press %s
wait 0.6
note resumed_from_row
quit
""" % (key, key))
        check(k.notes["resumed"]["ui"] == "" and k.notes["resumed_from_row"]["ui"] == "",
              "carousel: one %s from the opened menu resumes, and from the top row: %r %r"
              % (key, k.notes["resumed"]["ui"], k.notes["resumed_from_row"]["ui"]))
    # with A moved off del, del is the menus' clear key and deletes (D always does)
    keys = owner_config()[38:48]
    keys[4] = LETTERS["K"]
    u = Run("del_unbound", files={"gpsp_qol.cfg.tns": turbo_config(keys, [0] * 10)})
    u.run(REAL_COSTS + "wait 1\npress enter\nwait 1.5\n" + "press S\nwait 0.5\n" * 3 + "quit\n")
    u2 = Run("del_unbound2", reuse=u).run(REAL_COSTS + "wait 1\npress enter\nwait 2\npress menu\nwait 0.8\n" +
                                         saves_tile(0) + """press del
wait 0.4
shot asked
press esc
wait 0.4
shot kept
quit
""")
    check(u2.shot("asked").ui == "dialog:0" and u2.shot("kept").ui == "saves:2" and
          len(questions(u2)) == 1 and questions(u2)[0][0].startswith("Delete slot") and len(slots(u2)) == len(slots(u)),
          "carousel: with A not on del, del deletes (asks, esc keeps it and the tile): %s %s %s"
          % (u2.shot("asked").ui, u2.shot("kept").ui, questions(u2)))


def test_saves_grid_full():
    """The SAVES grid keeps a tile for Resume, Save, the game's own save and every
    state, and the arrays behind it held one too few: with 1000 states (or 999 and the game's own save) the last
    tile overwrote a global. The arrows reach the last tile (2 + [1] + the states: "saves:1001"), D on it asks and
    deletes that state (the oldest, slot 1) and no other file of the folder changes; nothing on stderr."""
    for name, states, own in (("full_1000", 1000, False), ("full_999_own", 999, True)):
        seed = Run(name + "_seed")
        seed.run(REAL_COSTS + "wait 1\npress enter\nwait 2\npress S\nwait 1\nquit\n")
        r = Run(name, roms=("Pattern",))
        folder = os.path.dirname(r.save("x"))
        os.makedirs(folder, exist_ok=True)
        for k in range(states):
            os.link(seed.save("Pattern.gba0.svs.tns"), os.path.join(folder, "Pattern.gba%d.svs.tns" % k))
        if own:
            with open(os.path.join(folder, "Pattern.gba.sav.tns"), "wb") as f:
                f.write(bytes([0x55]) + bytes(32767))
        last = 1 + (1 if own else 0) + states
        keep = lambda: dict((n, (os.path.getsize(os.path.join(folder, n)), os.path.getmtime(os.path.join(folder, n))))
                            for n in os.listdir(folder) if n.endswith((".svs.tns", ".sav.tns")))
        before = keep()
        a = r.run("wait 1\npress enter\nwait 2\npress menu\nwait 1\nshot opened\n" +
                  "press down\nwait 0.03\n" * (last // 3) + "press right\n" * (last % 3) +
                  "wait 0.5\nshot last\npress right\nwait 0.2\nshot still_last\npress d\nwait 0.6\nshot asked\n"
                  "press right\npress enter\nwait 1\nshot deleted\nquit\n")
        check(r.shot("opened").ui == "saves:0", "%s: the paused menu opens on SAVES: %s" % (name, r.shot("opened").ui))
        check(r.shot("last").ui == "saves:%d" % last and r.shot("still_last").ui == "saves:%d" % last,
              "%s: the arrows reach the last tile (%d) and stop: %s %s"
              % (name, last, r.shot("last").ui, r.shot("still_last").ui))
        after = keep()
        gone = sorted(set(before) - set(after))
        check(r.shot("asked").ui == "dialog:0" and gone == ["Pattern.gba0.svs.tns"] and
              all(after[n] == before[n] for n in after) and set(after) <= set(before),
              "%s: D on the last state asks, and deletes that slot alone: %s %s" % (name, r.shot("asked").ui, gone))


def test_about_page():
    """Enter on Settings > More > About does nothing (it used to leave two levels at once, the
    next esc closing the menu); esc goes back to More with About picked."""
    r = Run("about_page").run("wait 1\npress enter\nwait 1.5\npress menu\nwait 0.5\n" + to_setting("about") +
                             "press enter\nwait 0.4\nshot about\npress enter\nwait 0.4\nshot enter_again\n"
                             "press esc\nwait 0.4\nshot back\nnote back\nquit\n")
    check(r.shot("about").ui == "About:0" and r.shot("enter_again").ui == "About:0",
          "about: enter on About changes nothing: %s %s" % (r.shot("about").ui, r.shot("enter_again").ui))
    check(r.shot("back").ui == shown_at("about") and r.shot("enter_again").pixels == r.shot("about").pixels,
          "about: esc goes back to More with About picked: %s" % r.shot("back").ui)


def test_no_game_row_after_cancel():
    """After a start cancelled with Q from GAMES (a game paused before) and after a game
    that stops on a damaged page, there is no game, so the grid's top row has three words: GAMES reports "main:1",
    left of it is SETTINGS (main:0), right of it QUIT (main:2)."""
    import struct
    row = """press esc
wait 0.4
shot games
press left
wait 0.3
shot settings
press right
press right
wait 0.3
shot quit
quit
"""
    def check_row(r, what):
        check([r.shot(n).ui for n in ("games", "settings", "quit")] ==
              ["main:%d" % NO_GAME["games"], "main:%d" % NO_GAME["settings"], "main:%d" % NO_GAME["exit"]],
              "%s: three words, GAMES main:1, left SETTINGS main:0, right QUIT main:2: %s"
              % (what, [r.shot(n).ui for n in ("games", "settings", "quit")]))
    # a start cancelled with Q
    r = Run("row_cancel", roms=("Alpha",))
    big_rom(r.file("Zbig.gba.tns"))
    r.run(REAL_COSTS + "cost read 600\nwait 1\npress enter\nwait 2\npress menu\nwait 0.6\n" + to_main("game_list") +
          "press enter\nwait 0.5\npress right\npress enter\nwait 0.5\nshot loading\ntap Q\nwait 0.5\nshot list\n" + row)
    check(r.shot("loading").ui == "loading" and r.shot("list").ui.startswith("list"),
          "row after a cancelled start: the loading screen, then the list: %s %s" % (r.shot("loading").ui, r.shot("list").ui))
    check_row(r, "row after a cancelled start")
    # a game that stops
    j = Run("row_stopped", roms=("Alpha",))
    rom = os.path.join(j.dir, "Jump.gba.tns")
    with open(rom, "wb") as f:
        f.write(struct.pack("<3I", 0xE59F0000, 0xE12FFF10, 0x08230000) + bytes(3 * 1024 * 1024 - 12))
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "..", "..", "tools", "gbz.py"), "pack", "-o", j.gba, rom],
                   check=True, capture_output=True)
    with open(j.file("Jump.gbz.tns"), "r+b") as f:
        f.seek(32 + 4 * 70)
        start = struct.unpack("<I", f.read(4))[0]
        f.seek(start + 8)
        b = f.read(1)[0]
        f.seek(start + 8)
        f.write(bytes([b ^ 0xFF]))
    j.run(REAL_COSTS + "wait 1\npress enter\nwait 2\npress menu\nwait 0.6\n" + to_main("game_list") +
          "press enter\nwait 0.5\npress right\npress enter\nwait 1.5\nshot stopped\npress enter\nwait 0.6\nshot list\n" + row,
          options=("--rom-memory", "2"))
    check(j.shot("stopped").ui == "stopped" and j.shot("list").ui.startswith("list"),
          "row after a game that stopped: the stopped screen, then the list: %s %s" % (j.shot("stopped").ui, j.shot("list").ui))
    check_row(j, "row after a game that stopped")


TESTS = [test_play, test_timer_speed, test_cx_timer_by_cpu, test_cx_timer_search, test_timer_clock_shared, test_r12_time_base, test_quit6_timer_wrap, test_counter_tells_truth, test_text_goes_away, test_bands_beside_picture, test_menu_in_fade, test_picture_lines, test_blend_gaps, test_menu, test_menu_slow_game, test_old_settings, test_legacy_renderer_setting, test_unscaled_chosen, test_settings_select_first,
         test_brightness, test_states, test_number_keys, test_list_grid_edges, test_digit_keys_in_game, test_move_and_sort,
         test_hotkeys_in_menu_keys, test_folders, test_gbz, test_switch_game_os_paths, test_menu_load_and_exit, test_list_details_after_game, test_frameskip, test_auto_frameskip, test_auto_frameskip_levels,
         test_fps_counter_and_fast_forward, test_fast_forward_pace, test_fast_forward_max_speed, test_touchpad_in_fast_forward, test_fast_forward_even, test_lcd_gba_rate, test_fast_forward_follows_scene, test_pace_rule, test_frameskip_off_once, test_blended, test_loading_screen, test_zipped_game, test_zip_to_gbz, test_damaged_rom, test_damaged_gbz_page,
         test_slot_keys, test_save_manager, test_saves_place, test_saves_place_marks, test_confirm_delete,
         test_saves_place_other_save, test_every_menu_line, test_states_from_older_builds,
         test_file_types, test_new_calculator, test_no_gba_folder, test_old_layout, test_old_saves_folder,
         test_rom_manager, test_rom_manager_leftovers, test_link_setting, test_link_page, test_link_delay, test_link_real_model, test_link_fast, test_link_fast_os, test_link_hold_frames, test_link_wait_screen, test_link_wait_leaves_nothing, test_os_keys_after_exit, test_exit_paths, test_link_timer_dead, test_link_esc_in_wait, test_link_auto, test_link_auto_relink, test_link_exact, test_peek,
         test_cancel_loading, test_no_game_menu, test_questions, test_menu_ab_keys, test_turbo, test_turbo_every_button, test_turbo_old_settings, test_autosave,
         test_gui_ab, test_save_load_carousel,
         test_save17_memory, test_save18_startup_memory, test_save18_switch_memory, test_save17_library,
         test_save17_review2, test_save17_game_save, test_saves_grid_full, test_about_page,
         test_no_game_row_after_cancel, test_settings_tiles, test_volume_tile, test_more_tiles, test_more_saves, test_keys_all_cleared, test_frame_skip_tile, test_screen_size_values, test_fast_settings,
         test_cheats_find, test_cheats_page, test_cheats_effect, test_cheats_persistence, test_cheats_limits,
         test_cheats_other_game, test_cheats_states]


def main():
    wanted = sys.argv[1:]
    os.makedirs(WORK, exist_ok=True)
    for test in TESTS:
        name = test.__name__[5:]
        if wanted and name not in wanted:
            continue
        print(name)
        test()
    if not wanted:
        # every form of the speed readout came by with "fps: " (readout())
        check(READOUTS >= {"n", "drawn", "--"}, "the speed readout's forms seen with \"fps: \": %s" % sorted(READOUTS))
    print("%d checks, %d failed" % (checks, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
