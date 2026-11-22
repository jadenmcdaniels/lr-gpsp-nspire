#!/usr/bin/env python3
"""Saves and save states on a USB drive (2026-11-17, final/usbsave8, on the
menus of final/ui7): gpSP's whole frontend on the simulated calculator, the
drive nspire-usbms's fake one over a FAT image (GPSP_USB_IMAGE), written for
real. The paused menu's Save and Load have the two places as tabs (tab), Load
copies a slot to the other place (c), Settings > More has "Saves on" while a
drive is in (nspire_places.h; the functions behind them: nspire_drive_saves.c).

    drive_save_tests.py IMAGE [NAME...]

IMAGE is one of tests/host/drive_tests.sh's (build/drive-work/fat16.img,
fat32.img). Each test works on its own copy of it (the image itself stays as
it is), reads the copy back with mtools, and ends with fsck.vfat -n on it."""
import gzip
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import host_tests as h
import r19nav as N

IMAGE = KIND = None   # (main() sets them from the command line: par.py runs each test in a process of its own)
ONLY = []
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")

GAME = "Game"                       # Game.gba.tns on the calculator: "Game.gba" names its saves
SAV = "/gpsp/saves/Game.gba.sav"    # its save on the drive
STATE = "/gpsp/states/Game.gba%d.svs"

# r19 third pass: "Saves on" is a tile of Settings > More (after Ask to delete; there only while a drive can hold
# saves); More's tiles are numbered K in "More:K" (r19nav.more_names says which)
SAVES_ON = N.more_index("saves_on", drive=True)
TO_SAVES_ON = N.more_tile("saves_on", drive=True)
START = "wait 1\npress enter\nwait 2\n"            # the list's first game started
MENU = "press menu\nwait 0.6\n"                    # the paused menu (its safe point's work done)
# r19 (docs/ui/r19.txt): the menu opens on SAVES, the arrows in its tiles, RESUME picked; the tiles are RESUME, SAVE,
# the game's own save when it has one, then the states, the newest first. enter on SAVE saves and goes back to the
# game; on a state loads it; tab switches the place (calculator / USB drive); C copies the picked state.
ON_SAVE = MENU + "press right\nwait 0.4\n"          # the SAVE tile picked: the strip of the place Saves on names
ON_LOAD = MENU + h.saves_tile(0) + "wait 0.4\n"       # the newest state picked (no game save tile before it)
ON_LOAD_GS = MENU + h.saves_tile(0, True) + "wait 0.4\n"   # the same where the game has its own save too
TAB = "press tab\nwait 0.6\n"                       # the other place's tiles (RESUME picked there)
SAVE_ON_DRIVE = MENU + TAB + "press right\nwait 0.4\n"        # the drive's SAVE tile
LOAD_DRIVE = MENU + TAB + h.saves_tile(0) + "wait 0.4\n"       # the drive's newest state
ENTER = "press enter\nwait 1.2\n"                   # saved or loaded: back in the game, a toast says where
MANAGER = MENU + "wait 0.4\n"                        # the paused game's saves: the SAVES tiles, M marks, T copies
LEAVE_MANAGER = "press esc\nwait 0.3\npress esc\nwait 0.3\npress esc\nwait 0.5\n"
# (esc three times first: from a page or a strip, its grid, the tabs: back to the game, where esc does nothing)
QUIT = "press esc\nwait 0.3\npress esc\nwait 0.3\npress esc\nwait 0.5\npress Q\nwait 1\n"


def image_copy(name):
    path = os.path.join(h.WORK, "%s-%s.img" % (name, KIND))
    os.makedirs(h.WORK, exist_ok=True)
    subprocess.run(["cp", "--sparse=always", IMAGE, path], check=True)
    return path


def drive_names(img, folder):
    """The file names in a folder of the image ([] when there is none)."""
    p = subprocess.run(["mdir", "-b", "-i", img, "::" + folder], env=MT, capture_output=True, text=True)
    if p.returncode:
        return []
    return sorted(line.strip().split("/")[-1] for line in p.stdout.splitlines() if line.strip() and not line.endswith("/"))


def drive_read(img, path):
    p = subprocess.run(["mcopy", "-i", img, "::" + path, "-"], env=MT, capture_output=True)
    return p.stdout if p.returncode == 0 else None


def drive_write(img, path, data):
    parts = path.strip("/").split("/")
    for i in range(1, len(parts)):
        subprocess.run(["mmd", "-i", img, "::/" + "/".join(parts[:i])], env=MT, capture_output=True)
    tmp = os.path.join(h.WORK, "drive_write.tmp")
    with open(tmp, "wb") as f:
        f.write(data)
    subprocess.run(["mcopy", "-o", "-i", img, tmp, "::" + path], env=MT, check=True)


def fsck(img, what):
    p = subprocess.run(["fsck.vfat", "-n", img], capture_output=True, text=True)
    h.check(p.returncode == 0, "%s: the drive passes fsck.vfat -n afterwards: %s" % (what, (p.stdout + p.stderr).strip()[-300:]))


def digest(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def config(saves_on_drive=True):
    """gpsp.cfg.tns as this build writes it, default settings but Saves on (word 71 bit 5)."""
    o = h.owner_config()
    o[24], o[25] = 1, 4          # automatic frameskip (the reference file has it off)
    o[58] = 1                    # New slot per save, as a new calculator has it
    o[71] = 0x80000000 | 0x10 | (0x20 if saves_on_drive else 0)
    return o


def calc_save(r):
    """The calculator's save of the game, unpacked (None: none)."""
    plain, packed = r.save("Game.gba.sav.tns"), r.save("Game.gba.sav.gz.tns")
    if os.path.exists(packed):
        return gzip.decompress(open(packed, "rb").read())
    return open(plain, "rb").read() if os.path.exists(plain) else None


def put_calc_save(r, data):
    for name in ("Game.gba.sav.tns", "Game.gba.sav.gz.tns"):
        if os.path.exists(r.save(name)):
            os.remove(r.save(name))
    os.makedirs(os.path.dirname(r.save("x")), exist_ok=True)
    with open(r.save("Game.gba.sav.gz.tns"), "wb") as f:
        f.write(gzip.compress(data))


def sram(*head):
    """A 32 KB save: the bytes given, then 0xFF (the save memory as it starts)."""
    return bytes(head) + b"\xff" * (0x8000 - len(head))


def said(r):
    """What gpSP told the player about the saves' place (its link log), in order."""
    path = r.save("gpsp_link.txt.tns")
    if not os.path.exists(path):
        return []
    return [line.split('saves: said "', 1)[1].rstrip().rstrip('"') for line in open(path, errors="replace")
            if 'saves: said "' in line]


def log_has(r, text):
    path = r.save("gpsp_link.txt.tns")
    return os.path.exists(path) and text in open(path, errors="replace").read()


def toasts(r):
    """The toasts of a run, in order (pad.txt's osd lines)."""
    out = []
    for line in open(os.path.join(r.out, "pad.txt"), errors="replace"):
        if "osd " in line:
            text = line.split("osd ", 1)[1].strip()
            if text and (not out or out[-1] != text):
                out.append(text)
    return out


def toasted(r, text):
    return any(text in t for t in toasts(r))


def calc_states(r):
    return sorted(f for f in os.listdir(os.path.dirname(r.save("x"))) if ".svs" in f)


def run(name, script, img, files=None, reuse=None, env=None, roms=(GAME,), options=()):
    """(Scripts end with Q, gpSP's own way out: the game is left, its saves written. The "quit" after
    it only ends a run that Q did not: a check then says what was left undone.)"""
    r = h.Run(name + "-" + KIND, roms=roms, files=files, reuse=reuse)
    r.run(script + "wait 1.5\nquit\n", env=dict({"GPSP_USB_IMAGE": img}, **(env or {})), options=options)
    # every toast of the run one line of 63 characters at most (OSD_TEXT_MAX), the saves' own 50 at most
    long = [t for t in toasts(r) if len(t) > 63 or (("USB" in t or "drive" in t) and len(t) > 50)]
    h.check(not long, "%s: every toast fits its line: %s" % (name, long))
    return r


# ---- The tests ----

def test_tabs():
    """Saves on: Calculator (as every calculator has it), a drive in: the paused
    menu's Save and Load have the drive as a second tab. Saved there, loaded
    from there (the game is on the calculator); nothing of it on the
    calculator; the game's own save is not put on the drive. Then another
    calculator loads that state from the drive."""
    img = image_copy("tabs")
    r = run("usbsave-tabs",
            START + "poke 0E000000:5A\nwait 1\n" + ON_SAVE + "shot save_calc\n" + TAB + "press right\nshot save_drive\n" + ENTER +
            "note saved\nwait 1.5\nnote before\n" + MENU + "shot load_calc\n" + TAB + h.saves_tile(0) + "shot load_drive\n" + ENTER +
            "note after\nwait 1\n" + QUIT, img, files={"gpsp.cfg.tns": config(False)})
    n = r.notes
    h.check(n["save_calc"]["ui"] == "saves:1" and n["load_calc"]["ui"].startswith("saves:"),
            "tabs: the paused menu's SAVES, the SAVE tile picked: %s %s" % (n["save_calc"]["ui"], n["load_calc"]["ui"]))
    h.check(r.shot("save_calc").pixels != r.shot("save_drive").pixels and r.shot("load_calc").pixels != r.shot("load_drive").pixels,
            "tabs: tab shows the other place's strip")
    h.check(toasted(r, "Saved to slot 1 (USB drive)"), "tabs: Save on the drive's tab says where it went: %s" % toasts(r))
    h.check(toasted(r, "Loaded slot 1 (USB drive)"), "tabs: Load on the drive's tab says where from: %s" % toasts(r))
    h.check(n["saved"]["ui"] == "" and n["after"]["ui"] == "", "tabs: both go back to the game: %r %r" % (n["saved"]["ui"], n["after"]["ui"]))
    h.check(0 < n["after"]["frame"] < n["before"]["frame"],
            "tabs: loaded back from the drive: the game's frame %d, before the load %d" % (n["after"]["frame"], n["before"]["frame"]))
    names = drive_names(img, "/gpsp/states")
    h.check(names == ["Game.gba0.svs"], "tabs: the drive has the state as /gpsp/states/Game.gba<slot>.svs: %s" % names)
    state = drive_read(img, STATE % 0) or b""
    h.check(state[:4] == b"GPSZ" and len(state) > 1000, "tabs: it is a gpSP state (%d bytes)" % len(state))
    # (the one the calculator has: Save when I quit, on the way out: Saves on is Calculator)
    h.check(calc_states(r) == ["Game.gba0.svs.tns"], "tabs: the calculator has the quit's state alone: %s" % calc_states(r))
    h.check(drive_read(img, "/gpsp/saves/Game.gba.id") == b"\0\0\0\0",
            "tabs: the game's name is its own on the drive (/gpsp/saves/Game.gba.id, its 4-byte code)")
    h.check(calc_save(r) == sram(0x5A) and drive_names(img, "/gpsp/saves") == ["Game.gba.id"],
            "tabs: the game's own save stays the calculator's alone with Saves on: Calculator: %s" % drive_names(img, "/gpsp/saves"))
    h.check(log_has(r, "drive: /gpsp/states/Game.gba0.svs") and log_has(r, "written"),
            "tabs: the write is in the link's log (what it took, how it ended)")
    words = h.read_words(r.save("gpsp.cfg.tns"))
    h.check(not words[71] & 0x20 and said(r) == [], "tabs: Saves on is as it was, nothing more to tell: %08x %s" % (words[71], said(r)))
    fsck(img, "tabs")

    # another calculator, the same game on it, no save of it: Load, tab, enter
    b = run("usbsave-tabs-b", START + "wait 1\nnote fresh\n" + LOAD_DRIVE + "shot load_drive\n" + ENTER + "note loaded\nwait 0.5\n" + QUIT,
            img, files={"gpsp.cfg.tns": config(False)})
    h.check(toasted(b, "Loaded slot 1 (USB drive)") and not toasted(b, "Resumed"),
            "tabs: another calculator loads it from the drive's tab: %s" % toasts(b))
    h.check(b.notes["loaded"]["frame"] > 0, "tabs: and plays on from it: frame %d" % b.notes["loaded"]["frame"])
    h.check(drive_names(img, "/gpsp/states") == ["Game.gba0.svs"] and drive_read(img, STATE % 0) == state,
            "tabs: the drive's state is as it was")
    fsck(img, "tabs (another calculator)")
    return [r.shot("save_calc"), r.shot("save_drive"), r.shot("load_drive")]


def test_setting():
    """Settings > More, "Saves on" (the tile is there while a drive is in): set
    to USB drive, the menu's Save and Load open on the drive's tab, Save when I
    quit saves there, the game's own save is copied there. Without a drive the
    line is not there, the saves are the calculator's, and that is said once."""
    img = image_copy("setting")
    r = run("usbsave-setting",
            START + MENU + TO_SAVES_ON + "wait 0.3\nshot row\n" + h.cycle() + "wait 0.4\nshot on\n"
            "press esc\nwait 0.3\npress esc\nwait 0.3\npress esc\nwait 1\npoke 0E000000:33\nwait 1\n" + ON_SAVE + "shot save\n" + ENTER +
            "note saved\nwait 1\nnote before\n" + ON_LOAD + ENTER + "note after\nwait 0.5\n" + QUIT, img,
            files={"gpsp.cfg.tns": config(False)})
    n = r.notes
    h.check(n["row"]["ui"] == "More:%d" % SAVES_ON, "setting: More has Saves on as its tile %d: %s" % (SAVES_ON, n["row"]["ui"]))
    words = h.read_words(r.save("gpsp.cfg.tns"))
    h.check(words[71] & 0x20, "setting: Saves on: USB drive kept in the settings (word 71 bit 5): %08x" % words[71])
    h.check(toasted(r, "Saved to slot 1 (USB drive)") and toasted(r, "Loaded slot 1 (USB drive)"),
            "setting: Save and Load open on the drive's tab then: %s" % toasts(r))
    h.check(0 < n["after"]["frame"] < n["before"]["frame"], "setting: loaded from the drive: frame %d, before %d" %
            (n["after"]["frame"], n["before"]["frame"]))
    names = drive_names(img, "/gpsp/states")
    # (the second: Save when I quit, on the way out)
    h.check(names == ["Game.gba0.svs", "Game.gba1.svs"], "setting: the menu's save and the quit's are on the drive: %s" % names)
    h.check(calc_states(r) == [], "setting: no state on the calculator: %s" % calc_states(r))
    h.check(drive_read(img, SAV) == sram(0x33) and calc_save(r) == sram(0x33),
            "setting: the game's own save is on both, the drive's plain (32 KB, what a PC emulator reads)")
    h.check(said(r) == [], "setting: nothing to tell the player: %s" % said(r))
    fsck(img, "setting")

    # the same calculator without a drive: no such line, the saves on the calculator, said once
    # (the drive taken out before the game starts: the PC's stand-in for no drive)
    b = run("usbsave-setting-b", "wait 1\npad image out\nwait 0.5\npress enter\nwait 2\n" + MENU + N.more_tile("about", drive=True) +
            "wait 0.3\nshot row\npress esc\nwait 0.3\npress esc\nwait 0.3\npress esc\nwait 3.5\n" + ON_SAVE + "shot save\n" + ENTER + "note saved\n" + QUIT,
            img, reuse=r)
    h.check(b.notes["row"]["ui"] != "More:%d" % (len(N.more_names(drive=True)) - 1), "setting: without a drive More has one tile fewer (the last is not there): %s" % b.notes["row"]["ui"])
    h.check(said(b) == ["No USB drive: saves on the calculator"], "setting: said once: %s" % said(b))
    h.check(toasted(b, "No USB drive: saves on the calculator"), "setting: as a toast: %s" % toasts(b))
    h.check(any(t.startswith("Saved to slot") and "USB" not in t for t in toasts(b)) and len(calc_states(b)) == 2,
            "setting: Save saves on the calculator, and the quit: %s %s" % (toasts(b), calc_states(b)))
    words = h.read_words(b.save("gpsp.cfg.tns"))
    h.check(words[71] & 0x20, "setting: the setting itself stays (the drive may come back): %08x" % words[71])
    return [r.shot("row"), r.shot("on")]


def test_frames():
    """Saves on: USB drive, states saved while the game's frames run (the save
    key): written to the calculator, never to the drive from a frame; copied
    to the drive when the menu opens (the calculator keeps its own). The next
    start resumes from the newest of both places."""
    img = image_copy("frames")
    r = run("usbsave-frames",
            START + "press S\nwait 1\nnote saved\nwait 1\nnote later\npress L\nwait 0.5\nnote loaded\nwait 1\n"
            + MENU + "shot menu\npress esc\nwait 0.3\npress esc\nwait 1\npress S\nwait 1\n" + QUIT,
            img, files={"gpsp.cfg.tns": config()})
    n = r.notes
    h.check(toasted(r, "Saved to slot 1") and not toasted(r, "Saved to slot 1 (USB"),
            "frames: the save key says Saved to slot 1 (the calculator's): %s" % toasts(r))
    h.check(0 < n["loaded"]["frame"] < n["later"]["frame"],
            "frames: the load key loads it: frame %d after, %d before" % (n["loaded"]["frame"], n["later"]["frame"]))
    log = open(r.save("gpsp_link.txt.tns"), errors="replace").read()
    names = drive_names(img, "/gpsp/states")
    # slot 1 at the menu, slot 2 (the second S) and slot 3 (Save when I quit) on the way out
    h.check(names == ["Game.gba0.svs", "Game.gba1.svs", "Game.gba2.svs"], "frames: the drive has them all at the end: %s" % names)
    h.check(calc_states(r) == ["Game.gba0.svs.tns", "Game.gba1.svs.tns"], "frames: the calculator keeps the two the key saved: %s" % calc_states(r))
    h.check(all(drive_read(img, STATE % s) == open(r.save("Game.gba%d.svs.tns" % s), "rb").read() for s in (0, 1)),
            "frames: the drive's copies are the same bytes")
    osd = [line for line in open(os.path.join(r.out, "pad.txt")) if "osd " in line]
    wrote = [float(line.split()[0]) for line in log.splitlines() if "drive: /gpsp/states/Game.gba0.svs" in line]
    saved = [float(line.split()[0]) for line in osd if "Saved to slot 1" in line]
    h.check(wrote and saved and wrote[0] / 1000.0 > saved[0] + 2,
            "frames: it went to the drive when the menu opened, seconds after the key: saved at %s s, written at %s ms"
            % (saved[:1], wrote[:1]))
    h.check(said(r) == [], "frames: nothing to tell the player: %s" % said(r))
    counts = [struct.unpack_from("<I", drive_read(img, STATE % s) or b"\0" * 12, 8)[0] for s in range(3)]
    h.check(counts == [1, 2, 3], "frames: the drive's slots count on in the order they were saved: %s" % counts)
    fsck(img, "frames")
    b = run("usbsave-frames-b", START + "wait 0.5\nnote resumed\n" + QUIT, img, reuse=r)
    h.check(toasted(b, "Resumed from slot 3 (USB drive)"), "frames: the next start resumes from the newest, the drive's: %s" % toasts(b))

    # the power gone after the key (no exit code runs, nothing reached the drive): the state is the calculator's
    img = image_copy("frames-k")
    k = h.Run("usbsave-frames-k-" + KIND, roms=(GAME,), files={"gpsp.cfg.tns": config()})
    proc = k.start(START + "press S\nwait 1\ncrash\n", env={"GPSP_USB_IMAGE": img})
    proc.communicate(timeout=120)
    h.check(proc.returncode == 9 and calc_states(k) == ["Game.gba0.svs.tns"] and drive_names(img, "/gpsp/states") == [],
            "frames: the key's state is on the calculator when the run dies: %s" % calc_states(k))
    k2 = run("usbsave-frames-k2", START + "note resumed\n" + QUIT, img, reuse=k)
    h.check(toasted(k2, "Resumed from slot 1") and not toasted(k2, "Resumed from slot 1 (USB"),
            "frames: and the game resumes from it at the next start: %s" % toasts(k2))
    fsck(img, "frames (the run died)")


def test_autosave():
    """Autosave with Saves on: USB drive: every 30 s a state, written to the
    calculator while the game runs (no USB write in a frame), copied to the
    drive once the menu opens."""
    img = image_copy("autosave")
    cfg = config()
    cfg[71] |= 1                  # autosave every 30 s
    r = run("usbsave-autosave", START + "wait 32\nnote after\n" + MENU + "shot menu\npress esc\nwait 0.3\npress esc\nwait 0.5\n" + QUIT,
            img, files={"gpsp.cfg.tns": cfg})
    osd = [line for line in open(os.path.join(r.out, "pad.txt")) if "osd " in line]
    h.check(toasted(r, "Autosaved to slot 1"), "autosave: it saved by itself: %s" % toasts(r))
    log = open(r.save("gpsp_link.txt.tns"), errors="replace").read()
    wrote = [float(line.split()[0]) for line in log.splitlines() if "drive: /gpsp/states/Game.gba0.svs" in line]
    saved = [float(line.split()[0]) for line in osd if "Autosaved" in line]
    h.check(wrote and saved and wrote[0] / 1000.0 > saved[0] + 1,
            "autosave: the drive was written when the menu opened, not when it saved: saved at %s s, written at %s ms" % (saved[:1], wrote[:1]))
    h.check("Game.gba0.svs" in drive_names(img, "/gpsp/states") and "Game.gba0.svs.tns" in calc_states(r) and
            drive_read(img, STATE % 0) == open(r.save("Game.gba0.svs.tns"), "rb").read(),
            "autosave: the state is on the drive, the same bytes as the calculator's: %s" % drive_names(img, "/gpsp/states"))
    fsck(img, "autosave")


def test_copy():
    """Load's c: the picked slot copied to the other place's next free slot, the
    same bytes, both ways; again: the slot that has it, nothing written. A
    slot there is never written over."""
    img = image_copy("copy")
    a = run("usbsave-copy-a",
            START + "press S\nwait 1.5\npress S\nwait 1\n" + ON_LOAD + "shot load\npress T\nwait 1\nshot copied\n"
            "press T\nwait 1\nshot again\npress down\nwait 0.6\nshot older_picked\npress T\nwait 1\nshot older\n" + TAB +
            "press right\npress right\nshot drive\npress T\nwait 1\nshot back\npress esc\nwait 0.5\n" + QUIT, img, files={"gpsp.cfg.tns": config(False)})
    calc = {slot: open(a.save("Game.gba%d.svs.tns" % slot), "rb").read() for slot in (0, 1)}
    names = drive_names(img, "/gpsp/states")
    # (Load opens on the newest, slot 2: it is the drive's slot 1; then the calculator's slot 1: the drive's slot 2)
    h.check(names == ["Game.gba0.svs", "Game.gba1.svs"], "copy: to the drive: two states, each copied once: %s" % names)
    h.check(drive_read(img, STATE % 0) == calc[1] and drive_read(img, STATE % 1) == calc[0],
            "copy: the same bytes as the calculator's, in the drive's next free slots")
    # (three: the two the key saved and the quit's; the copy back from the drive found its state there)
    h.check(len(calc_states(a)) == 3, "copy: back to the calculator: it has that state already, none added: %s" % calc_states(a))
    h.check([a.notes[k]["ui"] for k in ("copied", "again", "older", "back")] == ["saves:2", "saves:2", "saves:3", "saves:2"],
            "copy: no question, no message (the arrows stay on their tile): %s" % [a.notes[k]["ui"] for k in ("copied", "again", "older", "back")])
    h.check(drive_names(img, "/gpsp/saves") == ["Game.gba.id"], "copy: the game's own save is not part of it")
    fsck(img, "copy")

    # another calculator: the drive's slot copied to it, then loaded there
    b = run("usbsave-copy-b", START + LOAD_DRIVE + "press T\nwait 1\nshot copied\n" + TAB + h.saves_tile(0) + "shot calc\n" + ENTER + "note loaded\nwait 0.5\n" +
            QUIT, img, files={"gpsp.cfg.tns": config(False)})
    h.check(calc_states(b)[:1] == ["Game.gba0.svs.tns"] and open(b.save("Game.gba0.svs.tns"), "rb").read() == calc[0],
            "copy: to the calculator: the drive's newest, the same bytes, slot 1: %s" % calc_states(b))
    h.check(toasted(b, "Loaded slot 1") and not toasted(b, "Loaded slot 1 (USB") and b.notes["loaded"]["frame"] > 0,
            "copy: and it loads from the calculator's tab: %s" % toasts(b))
    h.check(drive_names(img, "/gpsp/states") == names, "copy: the drive as it was: %s" % drive_names(img, "/gpsp/states"))
    fsck(img, "copy (another calculator)")

    # An older state copied to the drive is in its highest slot; Resume last save (Saves on: USB drive)
    # still takes the newest by its count. (Save when I quit off, so that the copy stays the highest.)
    img = image_copy("copy-older")
    cfg = config(False)
    cfg[59] = 0
    c = run("usbsave-copy-c", START + "press S\nwait 1.5\n" + QUIT, img, files={"gpsp.cfg.tns": cfg})
    words = h.read_words(c.save("gpsp.cfg.tns"))
    words[71] |= 0x20
    h.words_file(c.save("gpsp.cfg.tns"), words)
    os.remove(c.file(h.PROGRAM))           # (the settings kept in the program file: the copy's are read)
    with open(c.file(h.PROGRAM), "wb") as f:
        f.write(bytes(range(256)) * 16)
    d = run("usbsave-copy-d", START + "wait 2\n" + ON_SAVE + ENTER + "wait 1\n" + LOAD_DRIVE + "shot calc\npress T\nwait 1\nshot copied\n"
            "press esc\nwait 0.5\n" + QUIT, img, reuse=c)
    counts = [struct.unpack_from("<I", drive_read(img, STATE % s) or b"\0" * 12, 8)[0] for s in range(2)]
    h.check(drive_names(img, "/gpsp/states") == ["Game.gba0.svs", "Game.gba1.svs"] and counts[0] > counts[1],
            "copy: an older state copied after a newer one: the drive's slot 2 is the older: counts %s" % counts)
    e = run("usbsave-copy-e", START + "wait 0.5\nnote resumed\n" + QUIT, img, reuse=d)
    h.check(toasted(e, "Resumed from slot 1 (USB drive)"), "copy: Resume last save takes the newest by its count, slot 1: %s" % toasts(e))
    fsck(img, "copy (an older state)")
    return [a.shot("copied"), a.shot("back")]


def test_manager():
    """The save manager (UI17: the Saves place, opened on the newest state; left: the game's own save; A marks the
    states only, so the game's own save is copied picked alone) with a drive in: T copies the
    picked or marked ones to the drive: states to its next free slots, the
    game's own save as its .sav there; another save there is replaced only
    after a question."""
    img = image_copy("manager")
    a = run("usbsave-manager-a",
            START + "poke 0E000000:42\nwait 1\npress S\nwait 1.5\npress S\nwait 1\n" + MANAGER +
            "shot list\npress A\nwait 0.2\npress T\nwait 1.5\nshot copied\npress T\nwait 1\nshot again\n"
            # (UI17: A marks the states only; the game's own save is copied picked alone)
            "press A\nwait 0.2\npress left\npress left\npress left\npress right\npress right\npress T\nwait 1\n" + LEAVE_MANAGER + QUIT,
            img, files={"gpsp.cfg.tns": config(False)})
    calc = {slot: open(a.save("Game.gba%d.svs.tns" % slot), "rb").read() for slot in (0, 1)}
    h.check(a.notes["list"]["ui"] == "saves:0" and a.notes["copied"]["ui"].startswith("saves:"), "manager: the save manager: %s" % a.notes["list"]["ui"])
    names = drive_names(img, "/gpsp/states")
    h.check(names == ["Game.gba0.svs", "Game.gba1.svs"], "manager: T, all marked: both states on the drive, once: %s" % names)
    h.check(sorted((drive_read(img, STATE % s) or b"") for s in (0, 1)) == sorted(calc.values()), "manager: the same bytes as the calculator's")
    h.check(drive_read(img, SAV) == sram(0x42), "manager: and the game's own save, plain")
    h.check(os.path.exists(a.save("Game.gba0.svs.tns")) and calc_save(a) == sram(0x42), "manager: the calculator keeps its own")

    # another save of the game on the drive: asked; No keeps it, Replace replaces it
    drive_write(img, SAV, sram(9, 9, 9))
    c = run("usbsave-manager-b", START + MANAGER + "press right\npress right\npress T\nwait 1\nshot asked\npress enter\nwait 0.8\nshot kept\n" + LEAVE_MANAGER + QUIT,
            img, reuse=a)
    h.check(c.notes["asked"]["ui"] == "dialog:0", "manager: another save on the drive: a question, No picked: %s" % c.notes["asked"]["ui"])
    h.check(drive_read(img, SAV) == sram(9, 9, 9), "manager: No: the drive's is kept")
    d = run("usbsave-manager-c", START + MANAGER + "press right\npress right\npress T\nwait 1\npress right\npress enter\nwait 1\nshot replaced\n" + LEAVE_MANAGER + QUIT,
            img, reuse=c)
    h.check(drive_read(img, SAV) == sram(0x42), "manager: Replace: the drive has the calculator's")
    h.check(len(drive_names(img, "/gpsp/states")) == 2, "manager: the states there were not touched: %s" % drive_names(img, "/gpsp/states"))
    fsck(img, "manager")
    return [a.shot("list"), a.shot("copied"), c.shot("asked")]


def test_other_game_copy():
    """UI17: a state of another game copied to the drive from the store's Saves while a game is paused. The
    paused Zelda is "~CODE"-tagged (a Zelda of another game code in folder B started first and owns the name);
    the copy keeps no drive count for it then: the other Zelda's mark file ("Zelda.gba.sav.usb.tns") is not
    written under the wrong identity."""
    img = image_copy("samename")
    base = h.Run("usbsave-samename-base-" + KIND, roms=("Zelda",))

    def set_code(path, code):
        with open(path, "r+b") as f:
            f.seek(0xAC)
            f.write(code)
    set_code(base.file("Zelda.gba.tns"), b"AAAE")
    os.makedirs(os.path.join(base.gba, "B"))
    h.pattern_rom(os.path.join(base.gba, "B", "Zelda.gba.tns"))
    set_code(os.path.join(base.gba, "B", "Zelda.gba.tns"), b"BBBE")
    cfg = {"gpsp.cfg.tns": config(False)}
    # (the folders are items after the games: the root's grid is Zelda, "B/", "USB DRIVE/"; B's is its Zelda, "BACK")
    to_b = "press right\nwait 0.3\nshot on_b\nnote on_b\npress enter\nwait 1\nnote in_b\n"
    a = run("usbsave-samename-a", "wait 1\n" + to_b + "press enter\nwait 2\npress S\nwait 1\n" + QUIT, img,
            files=cfg, reuse=base)
    h.check(a.notes["on_b"]["ui"] == "list:1" and a.notes["in_b"]["ui"] == "list:0",
            "same name: B picked among the folders, opened: %s %s" % (a.notes["on_b"]["ui"], a.notes["in_b"]["ui"]))
    b_states = sorted(f for f in os.listdir(os.path.dirname(a.save("x"))) if f.endswith(".svs.tns"))
    # (the grid opens where the last run left it, in B: B's Zelda, USB DRIVE/, BACK: BACK, then the root's Zelda started)
    up = "wait 1\npress right\npress right\nwait 0.3\npress enter\nwait 1\npress enter\nwait 2\nnote paused\n"
    b = run("usbsave-samename-b", up + MENU + h.to_main("games") + "press enter\nwait 1\n" + to_b +
            "press s\nwait 1.5\nshot place\npress T\nwait 1.5\nshot copied\npress esc\nwait 0.5\npress esc\n"
            "wait 0.5\n" + QUIT, img, reuse=a)
    h.check(b.notes["paused"]["game"] == "Zelda.gba.tns" and b.notes["in_b"]["ui"] == "list:0",
            "same name: the root's Zelda paused, B's opened: %s %s" % (b.notes["paused"]["game"], b.notes["in_b"]["ui"]))
    h.check(b.notes["place"]["ui"].startswith("saves:") and b_states, "same name: folder B's Zelda's saves: %s %s"
            % (b.notes["place"]["ui"], b_states))
    h.check(drive_names(img, "/gpsp/states") != [], "same name: its state copied to the drive: %s"
            % drive_names(img, "/gpsp/states"))
    h.check(not os.path.exists(b.save("Zelda.gba.sav.usb.tns")),
            "same name: no mark file written for the other Zelda under the paused one's identity")
    fsck(img, "same name")
    return [b.shot("copied")]


def test_save_file():
    """The game's own save with Saves on: USB drive: written to the calculator
    while the game runs (as always), to the drive at the menu and at quit;
    taken from the drive at the next start when the calculator has none
    (another calculator); the calculator's kept and copied when it is the
    newer; the drive's taken when it was changed elsewhere; a question when
    nobody can say."""
    img = image_copy("sav")
    cfg = {"gpsp.cfg.tns": config()}
    a = run("usbsave-sav-a",
            START + "poke 0E000000:5A\npoke 0E000001:A5\nwait 1\n" + MENU + "note at_menu\npress esc\nwait 0.3\npress esc\nwait 0.5\n"
            "poke 0E000002:77\nwait 1\n" + QUIT, img, files=cfg)
    v2 = sram(0x5A, 0xA5, 0x77)
    h.check(drive_read(img, SAV) == v2, "save: on the drive at quit, plain (32 KB, what a PC emulator reads)")
    h.check(calc_save(a) == v2, "save: the calculator has the same")
    h.check(log_has(a, "drive: /gpsp/saves/Game.gba.sav, 32768 bytes: written"), "save: its writes are in the link's log")
    h.check(os.path.exists(a.save("Game.gba.sav.usb.tns")), "save: the sum both had is kept beside the calculator's")
    h.check(drive_names(img, "/gpsp/saves") == ["Game.gba.id", "Game.gba.sav"],
            "save: nothing else in /gpsp/saves: %s" % drive_names(img, "/gpsp/saves"))

    # another calculator (no save of the game): the drive's is the game's at its next start
    for name in ("Game.gba.sav.gz.tns", "Game.gba.sav.tns", "Game.gba.sav.usb.tns"):
        if os.path.exists(a.save(name)):
            os.remove(a.save(name))
    b = run("usbsave-sav-b", START + QUIT, img, reuse=a)
    h.check(calc_save(b) == v2, "save: picked up from the drive at the next start (the calculator had none)")
    h.check(log_has(b, "save: taken from the USB drive (32768 bytes)"), "save: the log says it was taken from the drive")
    h.check(said(b) == [], "save: nothing to tell the player then: %s" % said(b))

    # the game saves with the drive out: the calculator's is the newer; the player is told at the next start
    c = run("usbsave-sav-c", "wait 1\npad image out\nwait 0.5\npress enter\nwait 2\npoke 0E000003:11\nwait 1\n" + QUIT,
            img, reuse=b)
    v3 = sram(0x5A, 0xA5, 0x77, 0x11)
    h.check(calc_save(c) == v3 and drive_read(img, SAV) == v2, "save: saved with the drive out: the calculator has it, the drive the older one")
    h.check(said(c) == ["No USB drive: saves on the calculator"], "save: the player is told the drive is not in, once: %s" % said(c))
    d = run("usbsave-sav-d", START + QUIT, img, reuse=c)
    h.check(drive_read(img, SAV) == v3 and calc_save(d) == v3, "save: at the next start with the drive in, the calculator's newer save is kept and copied")
    h.check("Calculator's save was newer: drive updated" in said(d), "save: and the player is told: %s" % said(d))

    # the drive's changed elsewhere (a PC, another calculator), the calculator's as it was: the drive's is taken
    v4 = sram(1, 2, 3, 4, 5)
    drive_write(img, SAV, v4)
    e = run("usbsave-sav-e", START + QUIT, img, reuse=d)
    h.check(calc_save(e) == v4 and drive_read(img, SAV) == v4, "save: the drive's, changed elsewhere, is the game's at the next start")
    h.check(said(e) == [], "save: nothing to tell then: %s" % said(e))

    # both changed: nobody can say which is newer: a question; esc leaves both as they are
    v5, v6 = sram(5, 5, 5), sram(6, 6, 6)
    put_calc_save(e, v5)
    drive_write(img, SAV, v6)
    f = run("usbsave-sav-f", "wait 1\npress enter\nwait 2\nshot asked\npress esc\nwait 2\n" + QUIT, img, reuse=e)
    h.check(f.notes["asked"]["ui"] == "dialog:0", "save: two saves nobody can order: a question, the calculator's picked: %s" % f.notes["asked"]["ui"])
    h.check(calc_save(f) == v5 and drive_read(img, SAV) == v6, "save: esc: both as they were")
    h.check(said(f) == ["USB drive has another save: left as it is"], "save: and said so: %s" % said(f))
    # the calculator's picked: the drive gets it
    g = run("usbsave-sav-g", "wait 1\npress enter\nwait 2\npress enter\nwait 2\n" + QUIT, img, reuse=f)
    h.check(calc_save(g) == v5 and drive_read(img, SAV) == v5, "save: the calculator's picked: the drive has it now")
    # again, the drive's picked: the calculator gets it
    put_calc_save(g, v6)
    drive_write(img, SAV, sram(7, 7))
    i = run("usbsave-sav-h", "wait 1\npress enter\nwait 2\npress right\npress enter\nwait 2\n" + QUIT, img, reuse=g)
    h.check(calc_save(i) == sram(7, 7) and drive_read(img, SAV) == sram(7, 7), "save: the drive's picked: the calculator has it now")
    # a file there that is no save (an odd size): never replaced
    drive_write(img, SAV, b"not a save")
    j = run("usbsave-sav-i", START + "poke 0E000000:99\nwait 1\n" + QUIT, img, reuse=i)
    h.check(drive_read(img, SAV) == b"not a save" and calc_save(j) == sram(0x99, 7), "save: a file on the drive that is no save is left alone")
    fsck(img, "save")

    # the calculator's own compressed save put on a drive by a PC ("Game.gba.sav.gz"): read; once gpSP
    # writes the save there it is the plain one alone
    img = image_copy("savgz")
    drive_write(img, SAV + ".gz", gzip.compress(sram(9, 8, 7)))
    z = run("usbsave-sav-gz", START + MENU + "press esc\nwait 0.3\npress esc\nwait 0.5\npoke 0E000003:06\nwait 1\n" + QUIT, img, files=cfg)
    h.check(log_has(z, "save: taken from the USB drive (32768 bytes)") and calc_save(z) == sram(9, 8, 7, 6),
            "save: a .sav.gz on the drive is read")
    h.check(drive_read(img, SAV) == sram(9, 8, 7, 6) and "Game.gba.sav.gz" not in drive_names(img, "/gpsp/saves"),
            "save: and replaced by the plain .sav at the next write: %s" % drive_names(img, "/gpsp/saves"))
    fsck(img, "save (.gz)")
    return [f.shot("asked")]


def test_usb_game():
    """A game that is itself on the drive, Saves on: USB drive; and the other
    way round: the calculator's copy of that game ("Game.gba.tns" and the
    drive's "Game.gba" are one game) resumes from the state the drive's made."""
    img = image_copy("usbgame")
    name = "Long name with spaces.gba"
    # the list's u opens the drive; its GAMES folder; its first game
    r = run("usbsave-usbgame-a", "wait 1\npress u\nwait 1\npress enter\nwait 1\npress down\npress enter\nwait 3\n"
            "press enter\nwait 2\nnote playing\npoke 0E000000:61\nwait 1\n" + ON_SAVE + "shot save\n" + ENTER + "wait 1\nnote later\n" +
            QUIT, img, files={"gpsp.cfg.tns": config()}, roms=("Local",))
    h.check(r.notes["playing"]["game"] == name, "usb game: playing %s from the drive" % r.notes["playing"]["game"])
    names = drive_names(img, "/gpsp/states")
    h.check(names == [name + "0.svs", name + "1.svs"], "usb game: its states on the drive: %s" % names)
    h.check(drive_read(img, "/gpsp/saves/%s.sav" % name) == sram(0x61), "usb game: its save on the drive")
    # the same game copied to the calculator (as the PC's link software names it): one game with the drive's
    rom = drive_read(img, "/GAMES/" + name)
    b = h.Run("usbsave-usbgame-b-" + KIND, roms=(), files={"gpsp.cfg.tns": config()})
    with open(b.file(name + ".tns"), "wb") as f:
        f.write(rom)
    b.run(START + "wait 1\nnote resumed\n" + QUIT + "wait 1.5\nquit\n", env={"GPSP_USB_IMAGE": img})
    h.check(toasted(b, "Resumed from slot 2 (USB drive)"), "usb game: the calculator's copy resumes from the drive's newest state: %s" % toasts(b))
    h.check(b.notes["resumed"]["game"] == name + ".tns" and b.notes["resumed"]["frame"] >= r.notes["later"]["frame"] - 5,
            "usb game: at the frame it was left at: %d, left at %d" % (b.notes["resumed"]["frame"], r.notes["later"]["frame"]))
    packed = b.save(name + ".sav.gz.tns")
    h.check(os.path.exists(packed) and gzip.decompress(open(packed, "rb").read()) == sram(0x61),
            "usb game: and has the drive's save")
    fsck(img, "usb game")


def test_drive_leftovers():
    """What a write cut short leaves on the drive (nspire-usbms's usbms_put): a
    whole state as "<name>.tmp" alone takes its name when the game's slots are
    read; a .tmp that is no whole state, beside its state, is dropped and the
    state stays; a save's .tmp alone is the save."""
    img = image_copy("leftovers")
    a = run("usbsave-leftovers-a", START + ON_SAVE + ENTER + "poke 0E000000:44\nwait 1\n" + QUIT, img,
            files={"gpsp.cfg.tns": config()})
    state = drive_read(img, STATE % 0)
    drive_write(img, (STATE % 6) + ".tmp", state)              # whole, alone: slot 7
    drive_write(img, (STATE % 0) + ".tmp", state[:500])        # beside its state: not a whole state
    sav = drive_read(img, SAV)
    subprocess.run(["mdel", "-i", img, "::" + SAV], env=MT, check=True)
    drive_write(img, SAV + ".tmp", sav)
    for name in ("Game.gba.sav.gz.tns", "Game.gba.sav.tns", "Game.gba.sav.usb.tns"):
        if os.path.exists(a.save(name)):
            os.remove(a.save(name))
    b = run("usbsave-leftovers-b", START + ON_LOAD + "shot load\npress esc\nwait 0.5\n" + QUIT, img, reuse=a)
    names = drive_names(img, "/gpsp/states")
    h.check("Game.gba6.svs" in names and not [n for n in names if n.endswith(".tmp")],
            "leftovers: the whole .tmp is slot 7, the cut one is gone: %s" % names)
    h.check(drive_read(img, STATE % 0) == state and drive_read(img, STATE % 6) == state,
            "leftovers: the state beside the cut .tmp is as it was, slot 7 whole")
    h.check(calc_save(b) == sav and drive_read(img, SAV) == sav and drive_names(img, "/gpsp/saves") == ["Game.gba.id", "Game.gba.sav"],
            "leftovers: the save's whole .tmp is the save again: %s" % drive_names(img, "/gpsp/saves"))
    fsck(img, "leftovers")


def test_pulled():
    """The drive pulled out. On the drive's tab, before Save: the menus say it
    could not be saved (their message), nothing is lost, the calculator's tab
    saves. With Saves on: USB drive and the game running: the states the keys
    save are the calculator's, said once; with the drive back they are copied
    to it and saves go there again, and that is said."""
    img = image_copy("pulled")
    r = run("usbsave-pulled",
            START + SAVE_ON_DRIVE + ENTER + "wait 0.5\n" + SAVE_ON_DRIVE + "shot on_drive\npad image out\nwait 0.8\nshot gone\n"
            "press enter\nwait 1\nshot after\nnote after\npress enter\nwait 1\nnote then\npress esc\nwait 0.5\npress esc\nwait 0.5\n" + QUIT,
            img, files={"gpsp.cfg.tns": config(False)})
    names = drive_names(img, "/gpsp/states")
    h.check(names == ["Game.gba0.svs"], "pulled: the drive keeps what it had, nothing half written: %s" % names)
    h.check(len(calc_states(r)) >= 1 and toasted(r, "Saved to slot 1 (USB drive)"),
            "pulled: the save made after it is on the calculator: %s %s" % (calc_states(r), toasts(r)))
    h.check(not any("USB drive)" in t for t in toasts(r)[1:]), "pulled: nothing is said to be on the drive after it left: %s" % toasts(r))
    fsck(img, "pulled")

    # Saves on: USB drive, pulled while the game runs: the key's states, the menu's Save; then back in
    img2 = image_copy("pulled2")
    q = run("usbsave-pulled2", START + "press S\nwait 1\npad image out\nwait 0.5\npress S\nwait 1\n" + ON_SAVE + "shot save\n" + ENTER +
            "wait 6\nnote told\npoke 0E000000:31\nwait 1\npad image in\nwait 1\n" + MENU + "shot back\npress esc\nwait 0.3\npress esc\nwait 3\n"
            + ON_SAVE + ENTER + QUIT, img2, files={"gpsp.cfg.tns": config()})
    # (the key's two and the menu's with the drive out)
    h.check(calc_states(q) == ["Game.gba%d.svs.tns" % s for s in range(3)], "pulled: the states saved without the drive are the calculator's: %s" % calc_states(q))
    h.check(said(q) == ["No USB drive: saves on the calculator", "Saves are on the USB drive again"],
            "pulled: said once when it left, once when it was back: %s" % said(q))
    h.check(toasted(q, "No USB drive: saves on the calculator") and toasted(q, "Saves are on the USB drive again"),
            "pulled: both as toasts: %s" % toasts(q))
    names = drive_names(img2, "/gpsp/states")
    # the key's two copied once the drive is back, then the menu's save there and the quit's
    h.check(len(names) == 4 and all(drive_read(img2, STATE % s) == open(q.save("Game.gba%d.svs.tns" % s), "rb").read() for s in (0, 1)),
            "pulled: the drive back: the key's states copied to it, and the saves after go there: %s" % names)
    h.check(calc_save(q) == sram(0x31) and drive_read(img2, SAV) == sram(0x31), "pulled: the game's own save is on both again")
    fsck(img2, "pulled (while the game ran)")

    # pulled while the game runs and the game left without the menu: nobody saw a line then; it is said at
    # the next game's start (the drive still out); with the drive in again the game resumes from the
    # calculator's state, the newest
    img3 = image_copy("pulled3")
    s1 = run("usbsave-pulled3-a", START + ON_SAVE + ENTER + "wait 1\npress S\nwait 1\npad image out\nwait 0.5\n" + QUIT, img3,
             files={"gpsp.cfg.tns": config()})
    h.check(len(calc_states(s1)) == 2 and said(s1) == [],
            "pulled: left with the drive out: the key's state and the quit's are the calculator's, nothing was shown: %s %s" % (calc_states(s1), said(s1)))
    s2 = run("usbsave-pulled3-b", "wait 1\npad image out\nwait 0.5\npress enter\nwait 2\n" + QUIT, img3, reuse=s1)
    h.check(said(s2) == ["No USB drive: saves on the calculator"], "pulled: said at the next start: %s" % said(s2))
    h.check(drive_names(img3, "/gpsp/states") == ["Game.gba0.svs"], "pulled: the drive has what it had")
    s3 = run("usbsave-pulled3-c", START + "wait 0.5\nnote resumed\n" + QUIT, img3, reuse=s2)
    h.check(any(t.startswith("Resumed from slot") and "USB" not in t for t in toasts(s3)),
            "pulled: with the drive in again the game resumes from the calculator's state, the newest of both: %s" % toasts(s3))
    fsck(img3, "pulled (left without the menu)")


def test_refused():
    """A write-protected drive. Saves on: USB drive: found at the game's start,
    no place for saves, the saves on the calculator, said once; not a byte of
    it changes. Saves on: Calculator: Save on its tab is refused (the menus'
    message), then it is no place for saves. A full drive: Save there says it
    could not; the calculator's tab saves; nothing half written is left."""
    img = image_copy("protected")
    before = digest(img)
    r = run("usbsave-protected", START + "poke 0E000000:21\nwait 1\n" + ON_SAVE + "shot save\n" + ENTER + "wait 0.5\n" + QUIT,
            img, files={"gpsp.cfg.tns": config()}, env={"USBMS_FAKE_PROTECT": "1"})
    h.check(digest(img) == before, "protected: not a byte of the drive changed")
    h.check(said(r) == ["USB drive read-only: saves on the calculator"], "protected: said once: %s" % said(r))
    h.check(calc_states(r) == ["Game.gba0.svs.tns", "Game.gba1.svs.tns"] and calc_save(r) == sram(0x21),
            "protected: the states and the save are the calculator's: %s" % calc_states(r))
    h.check(toasted(r, "Saved to slot 1") and not toasted(r, "(USB drive)"), "protected: Save saves on the calculator: %s" % toasts(r))
    fsck(img, "protected")
    r = run("usbsave-protected-b", START + SAVE_ON_DRIVE + "shot drive\npress enter\nwait 1\nshot told\npress enter\nwait 0.6\nshot after\n"
            "press right\npress enter\nwait 6\nnote then\n" + QUIT, img, files={"gpsp.cfg.tns": config(False)}, env={"USBMS_FAKE_PROTECT": "1"})
    h.check(digest(img) == before, "protected: Saves on: Calculator, Save on the drive's tab: not a byte of the drive changed")
    h.check(r.notes["told"]["ui"] == "dialog:0", "protected: the menus say it could not be saved: %s" % r.notes["told"]["ui"])
    h.check(not toasted(r, "(USB drive)") and toasted(r, "Saved to slot 1") and len(calc_states(r)) == 2,
            "protected: the next Save is the calculator's (the drive is no place for saves): %s %s" % (toasts(r), calc_states(r)))
    h.check(said(r) == ["USB drive read-only: saves on the calculator"] and toasted(r, "USB drive read-only: saves on the calculator"),
            "protected: why its tab went is said back in the game: %s" % said(r))

    # full: all but 6 KB taken (room for the folders; a state is 12 KB, the game's save 32)
    img = image_copy("full")
    free = int(subprocess.run(["mdir", "-i", img, "::/"], env=MT, capture_output=True, text=True).stdout
               .split("bytes free")[0].splitlines()[-1].replace(" ", ""))
    filler = os.path.join(h.WORK, "filler-%s.bin" % KIND)
    with open(filler, "wb") as f:
        f.truncate(free - 6 * 1024)
    subprocess.run(["mcopy", "-i", img, filler, "::/FILLER.BIN"], env=MT, check=True)
    os.remove(filler)
    r = run("usbsave-full", START + "poke 0E000000:22\nwait 1\n" + ON_SAVE + "shot save\npress enter\nwait 1\nshot told\npress enter\nwait 0.6\n"
            + TAB + "shot calc\npress right\n" + ENTER + "wait 6\nnote later\n" + QUIT, img, files={"gpsp.cfg.tns": config()})
    h.check(r.notes["told"]["ui"] == "dialog:0", "full: Save on the drive's tab: the menus say it could not: %s" % r.notes["told"]["ui"])
    h.check(calc_save(r) == sram(0x22), "full: the game's own save is on the calculator")
    h.check(len(calc_states(r)) == 2 and toasted(r, "Saved to slot 1") and not toasted(r, "(USB drive)"),
            "full: the calculator's tab saves, and the quit: %s %s" % (calc_states(r), toasts(r)))
    h.check("USB drive full: saves on the calculator" in said(r), "full: said: %s" % said(r))
    h.check(not [f for f in drive_names(img, "/gpsp/states") + drive_names(img, "/gpsp/saves") if f.endswith(".tmp") or f.endswith(".svs")],
            "full: no state and no part of a file left on the drive: %s %s" % (drive_names(img, "/gpsp/states"), drive_names(img, "/gpsp/saves")))
    fsck(img, "full")


def test_older_state():
    """With a drive in and Saves on: USB drive, an OLDER calculator state X loaded from SAVES (tab to the
    calculator's, pick X, enter) stays the game's slot: back in the game the Load state key loads X again ("Loaded slot X"),
    and with Save to = Same slot the Save state key writes slot X, not the newest (the files' bytes say which). The same
    through GAMES > S on the paused game itself (its page's in-place load)."""
    cfg = config()
    cfg[58] = 0          # Save to: Same slot
    cfg[59] = 0          # Save when I quit off: the files stay as the test left them
    base_cfg = list(cfg)
    base_cfg[58] = 1     # (New slot per save, to make three states)
    base_img = image_copy("older-base")
    base = run("usbsave-older-base", START + "press S\nwait 1\npress S\nwait 1\npress S\nwait 1\n" + QUIT, base_img,
               files={"gpsp.cfg.tns": base_cfg})
    h.check(calc_states(base) == ["Game.gba%d.svs.tns" % i for i in range(3)], "older state: three states to pick from: %s" % calc_states(base))
    # the states by their calc files; X = slot 1 (file 0), the oldest: the third state tile on the calculator's tab
    # (Save to: Same slot, set on the page: the settings the base run wrote are the program file's)
    SAME_SLOT = MENU + N.more_tile("save_to") + "press enter\nwait 0.3\npress esc\npress esc\npress esc\nwait 0.5\n"
    ways = (("menu", MENU + TAB + h.saves_tile(2) + "wait 0.4\n"),
            ("games", MENU + h.to_main("game_list") + "press enter\nwait 1\npress s\nwait 1.5\n" + TAB + "press right\npress right\nwait 0.4\n"))
    for way, pick in ways:
        img = image_copy("older-" + way)
        r = run("usbsave-older-" + way, START + SAME_SLOT + pick + ENTER + "note loaded\nwait 1\npress L\nwait 1\nnote again\n"
                "press S\nwait 1.5\nnote saved\n" + QUIT, img, reuse=base, files={"gpsp.cfg.tns": cfg})
        loads = [line.split("osd ", 1)[1].strip() for line in open(os.path.join(r.out, "pad.txt")) if "osd Loaded slot" in line]
        h.check(len(loads) == 2 and all(x.startswith("Loaded slot 1") for x in loads),
                "older state (%s): loaded X from SAVES, and the Load state key loads X again, not the newest: %s" % (way, loads))
        saved = [open(r.save("Game.gba%d.svs.tns" % i), "rb").read() for i in range(3)]
        before = [open(base.save("Game.gba%d.svs.tns" % i), "rb").read() for i in range(3)]
        h.check(saved[0] != before[0] and saved[1] == before[1] and saved[2] == before[2],
                "older state (%s): the Save state key wrote slot X (slot 1), not the newest: changed %s" %
                (way, [saved[i] != before[i] for i in range(3)]))
        fsck(img, "older state (%s)" % way)


def test_hidden_tile():
    """Settings > More with SAVES ON picked, then the drive taken out: the tile reported as picked is one
    that is shown (Saves on is hidden), enter changes that shown tile (the settings differ from a run without the enter),
    and Saves on's stored value (word 71 bit 5) is as it was."""
    shown = len(N.more_names())      # the tiles with no drive: Saves on is not there
    img = image_copy("hidden")
    got = {}
    for name, extra in (("enter", "press enter\nwait 0.4\nshot changed\n"), ("none", "")):
        r = run("usbsave-hidden-" + name, START + MENU + TO_SAVES_ON + "wait 0.3\nshot on\npad image out\nwait 1.5\nshot gone\n" +
                extra + QUIT, img, files={"gpsp.cfg.tns": config(False)})
        h.check(r.notes["on"]["ui"] == "More:%d" % SAVES_ON, "hidden tile (%s): Saves on picked: %s" % (name, r.notes["on"]["ui"]))
        k = int(r.notes["gone"]["ui"].split(":")[1])
        h.check(r.notes["gone"]["ui"].startswith("More:") and k < shown,
                "hidden tile (%s): after the drive left the picked tile is a shown one (< %d): %s" % (name, shown, r.notes["gone"]["ui"]))
        got[name] = h.read_words(r.save("gpsp.cfg.tns"))
    h.check(got["enter"][71] & 0x20 == got["none"][71] & 0x20 == 0, "hidden tile: Saves on's stored value is unchanged: %08x %08x" %
            (got["enter"][71], got["none"][71]))
    h.check(got["enter"] != got["none"], "hidden tile: enter changed the shown tile (the settings differ from the run without it)")


def test_other_game():
    """A drive that has another game's saves under the same name (its code in
    /gpsp/saves/Game.gba.id): this game never reads or replaces them; its own
    go beside them as Game.gba~CODE..., as on the calculator (ZIP-6)."""
    img = image_copy("other")
    theirs, their_state = sram(0xAB, 0xCD), b"another game's state" * 100
    drive_write(img, "/gpsp/saves/Game.gba.id", b"BPEE")
    drive_write(img, SAV, theirs)
    drive_write(img, STATE % 0, their_state)
    r = run("usbsave-other", START + "poke 0E000000:0F\nwait 1\n" + ON_SAVE + "shot save\n" + ENTER + "wait 0.5\n" + QUIT,
            img, files={"gpsp.cfg.tns": config()})
    h.check(drive_read(img, SAV) == theirs and drive_read(img, STATE % 0) == their_state and
            drive_read(img, "/gpsp/saves/Game.gba.id") == b"BPEE", "other game: its save, state and name are as they were")
    h.check(calc_save(r) == sram(0x0F), "other game: this game did not get its save")
    names = drive_names(img, "/gpsp/states") + drive_names(img, "/gpsp/saves")
    h.check("Game.gba~____0.svs" in names and "Game.gba~____.sav" in names, "other game: this game's own files beside them: %s" % names)
    h.check(drive_read(img, "/gpsp/saves/Game.gba~____.sav") == sram(0x0F), "other game: with its own save")
    h.check(toasted(r, "Saved to slot 1 (USB drive)"), "other game: its first save is its slot 1 there: %s" % toasts(r))
    h.check(said(r) == [], "other game: nothing to tell: %s" % said(r))
    fsck(img, "other game")


def test_off():
    """Saves on: Calculator (as every calculator has it): a drive that is in is
    not written, whatever is saved on the calculator's tab or by the keys."""
    img = image_copy("off")
    before = digest(img)
    r = run("usbsave-off", START + "poke 0E000000:10\nwait 1\npress S\nwait 1\n" + ON_SAVE + ENTER + "wait 0.5\n" + ON_LOAD + ENTER + QUIT,
            img, files={"gpsp.cfg.tns": config(False)})
    h.check(digest(img) == before, "off: not a byte of the drive changed")
    h.check(len(calc_states(r)) == 3 and calc_save(r) == sram(0x10), "off: the saves are the calculator's: %s" % calc_states(r))
    h.check(said(r) == [] and not os.path.exists(r.save("Game.gba.sav.usb.tns")) and not toasted(r, "USB"),
            "off: nothing said, nothing kept for a drive: %s" % toasts(r))


def test_hub_restart():
    """(2026-11-18, Hub 5) The drive on a USB hub whose power fell: gpSP
    restarts the port's power (between game frames, never from the menus),
    and the hub comes back 8 s later (the restart itself waits for the hub's
    own answer, behind the OS's explore of the dead hub: ~9 s after the fall). A save in that time goes to the
    calculator, and what is said is that the drive is coming back, not that
    there is none; once it is back saves go to it again. Nothing on the drive
    is half written."""
    img = image_copy("hubrestart")
    r = run("usbsave-hubrestart", START + "pad port 1\npad hub in\npad hub image\nwait 2\npad hub late 8\n"
            "pad hub blip\nwait 9\npress S\nwait 1\n" + ON_SAVE + ENTER + "wait 20\n" + ON_SAVE + ENTER + QUIT,
            img, files={"gpsp.cfg.tns": config()})
    h.check(toasted(r, "Hub lost power, restarted it. A powered hub helps"), "hub restart: the restart said: %s" % toasts(r))
    h.check(said(r)[:1] == ["USB drive coming back: saves on the calculator"],
            "hub restart: a save meanwhile says the drive is coming back: %s" % said(r))
    h.check("No USB drive: saves on the calculator" not in said(r), "hub restart: never 'no USB drive': %s" % said(r))
    h.check(len(calc_states(r)) >= 1, "hub restart: the saves meanwhile are on the calculator: %s" % calc_states(r))
    h.check(len(drive_names(img, "/gpsp/states")) >= 1, "hub restart: saves on the drive again after: %s" %
            drive_names(img, "/gpsp/states"))
    fsck(img, "hub restart")


def test_hub_dead_quit():
    """(2026-11-18) The game left while the drive's hub may be dead (its
    power just fell, the hub not asked yet): the game's save goes to the
    calculator and the drive is not asked anything (each command to a dead
    drive waits for its time limit); the drive keeps what it had."""
    img = image_copy("hubdeadquit")
    r = run("usbsave-hubdeadquit", START + "pad port 1\npad hub in\npad hub image\nwait 2\npoke 0E000000:31\nwait 1\n"
            "pad hub blip\nwait 0.5\n" + QUIT, img, files={"gpsp.cfg.tns": config()})
    h.check(calc_save(r) == sram(0x31), "hub dead at the quit: the game's save on the calculator")
    h.check(drive_read(img, SAV) != sram(0x31), "hub dead at the quit: the drive not written meanwhile")
    h.check(log_has(r, "hub: the port's power fell under the hub"), "hub dead at the quit: the fall seen")
    fsck(img, "hub dead at the quit")


def test_memory():
    """save17: with the heap in pieces (no block over 128 KB: gpsp-host's frag), plenty free in all,
    a state saved to the drive, loaded from it, and Load's c copying the calculator's to the drive all work:
    they use the saves' kept memory (r16 needed 416-490 KB in one piece for each). The game's own save too."""
    img = image_copy("memory")
    r = run("usbsave-memory", START + "frag 128\npoke 0E000000:5A\nwait 1\npress S\nwait 1.5\n" + SAVE_ON_DRIVE +
            ENTER + "note saved\n" + MENU + h.saves_tile(0, True) + "press T\nwait 1\nshot copied\n" + TAB + h.saves_tile(0) + ENTER + "note loaded\n" + QUIT,
            img, files={"gpsp.cfg.tns": config(False)})
    t = toasts(r)
    h.check(not [x for x in t if x.startswith("Couldn't")], "memory: nothing fails with the heap in pieces: %s" % t)
    h.check(len(drive_names(img, "/gpsp/states")) >= 2, "memory: saved to the drive and copied there: %s" %
            drive_names(img, "/gpsp/states"))
    h.check(r.notes["copied"]["ui"] == "saves:3", "memory: the copy said nothing (it worked): %s" %
            r.notes["copied"]["ui"])
    h.check(any(x.startswith("Loaded slot") and "USB drive" in x for x in t), "memory: loaded from the drive: %s" % t)
    h.check(calc_save(r) is not None and calc_save(r)[:1] == b"\x5a", "memory: the game's own save written")
    fsck(img, "memory")


def test_needs_memory():
    """ms17: nspire-usbms takes its memory (320 KB) when a drive is opened, not at gpSP's start. A drive
    plugged in while the game runs, with too little memory left (gpsp-host's hog): at the menu (a safe
    point) the opening fails for memory; the menus' caches are given up and it is tried again, once; the
    key's state stays the calculator's and the player is told the truth ("needs memory", not "no USB
    drive"). With the memory back, the next menu opens it: the state is copied there, and that is said."""
    img = image_copy("needsmem")
    out = "wait 1\npad image out\nwait 0.5\n"
    probe = run("usbsave-needsmem-probe", out + START + "note running\n" + QUIT, img, files={"gpsp.cfg.tns": config()})
    held = int(probe.notes["running"]["heap"]) // 1024
    r = run("usbsave-needsmem", out + START + "hog 3800\npad image in\nwait 1\npress S\nwait 1\n" + MENU +
            "shot short\nnote short\npress esc\nwait 0.3\npress esc\nwait 1\nhog 0\nwait 0.5\n" + MENU + "note back\npress esc\nwait 0.3\npress esc\nwait 6\n" + QUIT,
            img, reuse=probe, options=["--heap-limit", str(held + 4096)])
    # (the game started with the drive out: that is said first, and is true then)
    h.check(said(r) == ["No USB drive: saves on the calculator", "USB drive needs memory: saved on the calculator",
                        "Saves are on the USB drive again"],
            "needs memory: no drive at the start, then why it isn't used, then that it has them again: %s" % said(r))
    h.check(toasted(r, "USB drive needs memory: saved on the calculator") and
            not [t for t in toasts(r) if t.startswith("Couldn't") and "game's save" not in t],
            "needs memory: the toast says so, no state fails: %s" % toasts(r))
    # (this flow's "Couldn't write the game's save" comes with no hog and no drive too: the test ROM's)
    h.check(log_has(r, "drive: not enough memory to open it: the menus' caches given up") and
            log_has(r, "drive: tried again after no memory"), "needs memory: the caches given up, tried again (link log)")
    names = drive_names(img, "/gpsp/states")
    calc = [open(r.save(n), "rb").read() for n in calc_states(r)]
    h.check(len(names) >= 1 and drive_read(img, STATE % 0) in calc,
            "needs memory: with the memory back the key's state is copied to the drive: %s" % names)
    times = open(r.save("gpsp_states_time.txt.tns"), errors="replace").read()
    h.check("USB drive's driver" in times, "needs memory: the memory log says what the drive's driver holds")
    fsck(img, "needs memory")


def test_margin():
    """ms17: the saves' memory is kept only if it leaves room for the rest (KEEP_MARGIN, 512 KB) and, while
    a drive could still be opened (the driver started, none open), for its opening and first save too
    (389 KB: cache, transfer memory, write buffer, a file's handle): with about 680 KB left after the keep,
    a run with the drive out now releases a ROM-cache block to keep saves with the 901 KB margin.
    The same run with no drive driver keeps its full cache (512 KB margin)."""
    img = image_copy("margin")
    out = "wait 1\npad image out\nwait 0.5\n"
    probe = run("usbsave-margin-probe", out + START + "note running\n" + QUIT, img, files={"gpsp.cfg.tns": config(False)})
    held = int(probe.notes["running"]["heap"]) // 1024
    limit = ["--heap-limit", str(held + 680)]
    r = run("usbsave-margin", out + START + "note running\n" + QUIT, img, reuse=probe, options=limit)
    h.check(not log_has(r, "NOT KEPT") and
            int(r.notes["running"]["rom_mb"]) == int(probe.notes["running"]["rom_mb"]) - 1,
            "margin: room for a drive's opening comes from one ROM-cache block, saves kept")
    q = h.Run("usbsave-margin-nodrive-" + KIND, reuse=probe)
    q.run(out + START + "note running\n" + QUIT + "wait 1.5\nquit\n", options=limit)
    h.check(not log_has(q, "NOT KEPT") and
            q.notes["running"]["rom_mb"] == probe.notes["running"]["rom_mb"],
            "margin: with no drive driver the same memory keeps saves and the full cache")


# ---- Cheats (r19, docs/ui/r19.txt section 6): a game's .cht file on the calculator or the USB drive ----

CHT = """cheats = 4

cheat0_desc = "Max Money"
cheat0_code = "82000568+423F"
cheat0_enable = false

cheat1_desc = "Max BP"
cheat1_code = "8200056A+000F"
cheat1_enable = false

cheat2_desc = "No Random Battle"
cheat2_code = "320375D4+0000"
cheat2_enable = false

cheat3_desc = "Action Replay one"
cheat3_code = "82000568 423F0000"
cheat3_enable = false
"""
CHT_CALC = CHT.replace('cheats = 4', 'cheats = 2').split("cheat2_desc")[0]    # two codes: the calculator's own
CHEATS_TILE = N.settings_tile("cheats", cheats=True) + "wait 0.4\n"
CHEATS_PAGE = CHEATS_TILE + "press enter\nwait 0.5\n"                       # the page open, cheats:0
BAND = (222 * 960, 240 * 960)


def band_of(r, shot):
    return r.shot(shot).pixels[BAND[0] - 15 * 0:BAND[1]]


def cht_run(name, script, img, calc=None, **kw):
    files = {"gpsp.cfg.tns": config(False)}
    if calc is not None:
        files["Game.cht"] = calc
    return run(name, script, img, files=files, **kw)


def test_cheats_found():
    """A game on the calculator, its .cht on the drive's top folder, in "cheats" and in "zgpSP_files": each found (the
    Cheats tile and its page), the band saying the drive; one on the calculator beside the game too: the calculator's
    wins (its two codes, the band saying the calculator); a game played from the drive with its .cht beside it."""
    page = START + MENU + CHEATS_PAGE + "shot page\n" + QUIT
    ref = None
    for where in ("/Game.cht", "/cheats/Game.cht", "/zgpSP_files/Game.cht", "/cheats/GAME.CHT.tns"):
        img = image_copy("cht-" + where.strip("/").replace("/", "-"))
        drive_write(img, where, CHT.encode())
        r = cht_run("usbcht-" + where.strip("/").replace("/", "-").replace(".", "_"), page, img)
        h.check(r.notes["page"]["ui"] == "cheats:0", "cheats: found at %s (its page opens): %s" % (where, r.notes["page"]["ui"]))
        if ref is None:
            ref = band_of(r, "page")
        h.check(band_of(r, "page") == ref, "cheats: %s: the band says the same (the drive)" % where)
        fsck(img, "cheats " + where)
    both = image_copy("cht-both")
    drive_write(both, "/Game.cht", CHT.encode())
    r = cht_run("usbcht-both", START + MENU + CHEATS_PAGE + "press down\npress down\nshot third\n" + QUIT, both, calc=CHT_CALC)
    h.check(r.notes["third"]["ui"] == "cheats:0", "cheats: the calculator's file wins (two codes: down twice comes round): %s" %
            r.notes["third"]["ui"])
    only = image_copy("cht-both-b")
    r2 = cht_run("usbcht-calc-only", page, only, calc=CHT_CALC)
    h.check(band_of(r2, "page") != ref, "cheats: a file on the calculator: the band says so, not the drive")
    # a game played from the drive, its file beside it (a product finding: a drive game's own
    # folder is not searched: "/GAMES/Long name with spaces.cht" is not found, "/Long name with spaces.cht" is)
    img = image_copy("cht-driveplayed")
    drive_write(img, "/GAMES/Long name with spaces.cht", CHT.encode())
    r3 = run("usbcht-driveplayed", "wait 1\npress u\nwait 1\npress enter\nwait 1\npress down\npress enter\nwait 3\npress enter\nwait 2\n" +
             MENU + CHEATS_PAGE + "shot page\n" + QUIT, img, roms=())
    h.check(r3.notes["page"]["ui"] == "cheats:0" and band_of(r3, "page") == ref,
            "cheats: a game played from the drive, its file beside it: found (the drive): %s" % r3.notes["page"]["ui"])


def test_cheats_drive_later():
    """The drive plugged in after the game started: no Cheats tile at first; once it is in and Settings is drawn again
    the tile is there, without a restart."""
    img = image_copy("cht-later")
    drive_write(img, "/Game.cht", CHT.encode())
    r = cht_run("usbcht-later", "wait 1\npad image out\nwait 0.5\npress enter\nwait 2\n" + MENU +
                N.settings_tile("link", cheats=False) + "wait 0.4\npress right\nwait 0.3\nnote more_before\n"
                # the drive comes; Settings is left for another word of the top row and entered again
                "pad image in\nwait 2\npress esc\nwait 0.4\npress right\nwait 0.4\npress left\nwait 0.4\npress down\nwait 0.4\n"
                "note more_after\npress left\nwait 0.4\nshot tile\nnote cheats\npress enter\nwait 0.5\nnote page\n" + QUIT, img)
    n = r.notes
    h.check(n["more_before"]["ui"] == "Settings:%d" % N.settings_index("more", cheats=False),
            "cheats: no drive: Link is followed by More (no tile): %s" % n["more_before"]["ui"])
    h.check(n["more_after"]["ui"] == "Settings:%d" % N.settings_index("more", cheats=True) and
            n["cheats"]["ui"] == "Settings:%d" % N.settings_index("cheats", cheats=True) and n["page"]["ui"] == "cheats:0",
            "cheats: with the drive in, the Cheats tile is there before More (no restart) and opens: %s %s %s" %
            (n["more_after"]["ui"], n["cheats"]["ui"], n["page"]["ui"]))


def test_cheats_write_back():
    """A code flipped and the page left: the drive's file is written (read back with mtools): only that enable line
    changed. Quit and start again: still on."""
    img = image_copy("cht-write")
    drive_write(img, "/Game.cht", CHT.encode())
    r = cht_run("usbcht-write", START + MENU + CHEATS_PAGE + "press enter\nwait 0.3\nshot flipped\npress esc\nwait 0.6\n" + QUIT, img)
    got = (drive_read(img, "/Game.cht") or b"").decode().splitlines()
    want = CHT.splitlines()
    diff = [(a, b) for a, b in zip(want, got) if a != b]
    h.check(len(got) == len(want) and diff == [("cheat0_enable = false", "cheat0_enable = true")],
            "cheats write-back: only the flipped code's enable line changed on the drive: %s" % diff)
    fsck(img, "cheats write-back")
    r2 = cht_run("usbcht-write-again", START + MENU + CHEATS_PAGE + "shot page\n" + QUIT, img, reuse=r)
    h.check(r2.shot("page").pixels == r.shot("flipped").pixels, "cheats write-back: started again: the code is still on")


def test_cheats_drive_pulled():
    """The drive pulled out while the page is open, a code flipped and the page left: nothing on stderr (the run's
    check), no hang, the code stays on for the running game; the drive's file is whole (the old content) when the drive
    is back."""
    img = image_copy("cht-pulled")
    drive_write(img, "/Game.cht", CHT.encode())
    r = cht_run("usbcht-pulled", START + MENU + CHEATS_PAGE + "shot before\npad image out\nwait 1.5\npress enter\nwait 0.3\nshot flipped\n"
                "press esc\nwait 0.8\n" + "press enter\nwait 0.5\nshot reopened\npress esc\nwait 0.5\n"
                "pad image in\nwait 2\n" + QUIT, img)
    top = lambda shot: r.shot(shot).pixels[:222 * 960 - 0][:60 * 960]
    h.check(top("before") != top("flipped"), "cheats pulled: the flip changed the page (the code is on)")
    h.check(r.notes["reopened"]["ui"].startswith("cheats:") and top("reopened") == top("flipped"),
            "cheats pulled: the code stays on for the running game (the page shows it on again): %s" % r.notes["reopened"]["ui"])
    h.check(drive_read(img, "/Game.cht") == CHT.encode(), "cheats pulled: the drive's file is whole, the old content, when it is back")
    fsck(img, "cheats pulled")


def test_cheats_drive_refused():
    """A write-protected drive and a full one: the flip still works for the running game, nothing on stderr (the
    run's check), the image as it was."""
    img = image_copy("cht-protected")
    drive_write(img, "/Game.cht", CHT.encode())
    before = digest(img)
    r = cht_run("usbcht-protected", START + MENU + CHEATS_PAGE + "shot before\npress enter\nwait 0.3\nshot flipped\npress esc\nwait 0.8\n"
                "press enter\nwait 0.5\nshot reopened\n" + QUIT, img, env={"USBMS_FAKE_PROTECT": "1"})
    h.check(r.shot("before").pixels[:60 * 960] != r.shot("flipped").pixels[:60 * 960] and
            r.shot("reopened").pixels[:60 * 960] == r.shot("flipped").pixels[:60 * 960],
            "cheats protected: the flip works for the running game")
    h.check(digest(img) == before, "cheats protected: not a byte of the drive changed")
    full = image_copy("cht-full")
    free = int(subprocess.run(["mdir", "-i", full, "::/"], env=MT, capture_output=True, text=True).stdout
               .split("bytes free")[0].splitlines()[-1].replace(" ", ""))
    drive_write(full, "/Game.cht", CHT.encode())
    free = int(subprocess.run(["mdir", "-i", full, "::/"], env=MT, capture_output=True, text=True).stdout
               .split("bytes free")[0].splitlines()[-1].replace(" ", ""))
    filler = os.path.join(h.WORK, "chtfiller-%s.bin" % KIND)
    for leave in (0, 4096, 16384, 65536):
        with open(filler, "wb") as f:
            f.truncate(max(free - leave, 0))
        p = subprocess.run(["mcopy", "-i", full, filler, "::/FILLER.BIN"], env=MT, capture_output=True)
        if p.returncode == 0:
            break
    os.remove(filler)
    r2 = cht_run("usbcht-full", START + MENU + CHEATS_PAGE + "shot before\npress enter\nwait 0.3\nshot flipped\npress esc\nwait 0.8\n"
                 "press enter\nwait 0.5\nshot reopened\n" + QUIT, full)
    h.check(r2.shot("before").pixels[:60 * 960] != r2.shot("flipped").pixels[:60 * 960] and
            r2.shot("reopened").pixels[:60 * 960] == r2.shot("flipped").pixels[:60 * 960], "cheats full: the flip works for the running game")
    fsck(full, "cheats full")


TESTS = [test_margin, test_needs_memory, test_memory, test_tabs, test_setting, test_frames, test_autosave, test_copy, test_manager, test_save_file, test_pulled, test_refused, test_usb_game, test_drive_leftovers, test_other_game, test_off, test_older_state, test_hidden_tile, test_other_game_copy, test_cheats_found, test_cheats_drive_later, test_cheats_write_back, test_cheats_drive_pulled, test_cheats_drive_refused, test_hub_restart, test_hub_dead_quit]

def main():
    global IMAGE, KIND, ONLY
    IMAGE = os.path.abspath(sys.argv[1])
    KIND = os.path.splitext(os.path.basename(IMAGE))[0]   # fat16, fat32
    ONLY = sys.argv[2:]
    for test in TESTS:
        if ONLY and test.__name__[5:] not in ONLY:
            continue
        print(test.__name__)
        test()
    print("drive saves (%s): %d checks, %d failed" % (KIND, h.checks, len(h.failures)))
    sys.exit(bool(h.failures))


if __name__ == "__main__":
    main()
