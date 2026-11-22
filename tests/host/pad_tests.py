#!/usr/bin/env python3
"""USB gamepads in gpSP on the simulated calculator (build/gpsp-host, with
host_usbpad.c standing in for nspire-usbpad's driver): the pad's buttons as
keys, their jobs when bound to nothing, the menus, binding them, unplugging
mid-game, the messages, the pad settings file, OS time and the poll rate.

    pad_tests.py [NAME...]      all tests, or the ones named

The runs are host_tests.py's (its Run, check and menu helpers), in
build/host_tests/pad_*. Without nspire-usbpad (beside gpSP, or $USBPAD_DIR
as the Makefile takes it), gpsp-host has no pads and these are skipped."""

import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import host_tests as H  # noqa: E402
import r19nav as N  # noqa: E402

# Where the Makefile looks for nspire-usbpad (USBPAD_DIR, or beside gpSP).
USBPAD_H = os.path.join(os.environ.get("USBPAD_DIR", os.path.join(HERE, "..", "..", "..", "nspire-usbpad")),
                        "usbpad.h")
SETTINGS = "gpsp_usbpad.cfg.tns"

# The GBA's buttons in REG_P1 (0x4000130), a bit each, 0 while pressed.
GBA = {"A": 0x001, "B": 0x002, "Select": 0x004, "Start": 0x008, "Right": 0x010, "Left": 0x020,
       "Up": 0x040, "Down": 0x080, "R": 0x100, "L": 0x200}

START_GAME = "wait 1\npress enter\nwait 1\n"


def held_peek(key, name):
    """Script lines: the pad key held, the GBA's buttons peeked, let go."""
    return "hold %s\nwait 0.2\npeek %s 4000130:2\nrelease %s\nwait 0.2\n" % (key, name, key)


def p1(run, name):
    """The buttons the game saw at a peek, by name ("A", "A B", "none")."""
    value = struct.unpack("<H", open(os.path.join(run.out, name + ".bin"), "rb").read())[0]
    return " ".join(b for b, bit in GBA.items() if not value & bit) or "none"


def pad_log(run):
    """out/pad.txt: (time, text) for each line."""
    path = os.path.join(run.out, "pad.txt")
    if not os.path.exists(path):
        return []
    lines = []
    for line in open(path):
        t, _, text = line.rstrip("\n").partition(" ")
        lines.append((float(t), text))
    return lines


def toasts(run):
    # (R11w part 2: "This controller isn't supported" is the unsupported one's words now)
    return [(t, text[4:]) for t, text in pad_log(run) if text.startswith("osd Controller") or
            text.startswith("osd Plug") or text.startswith("osd This controller")]


def check_shutdown(run, name, timer=True):
    """usbpad_shutdown ran at exit (atexit), and the driver was never polled
    more than 80 times a second: with a timer, 12.5 ms apart at least
    (key_capture reads the keys flat out); without one, 80 in any second."""
    ends = [text for _, text in pad_log(run) if text.startswith("shutdown:")]
    H.check(len(ends) == 1, "%s: usbpad_shutdown at exit once: %s" % (name, ends))
    if ends:
        gap = float(ends[0].split()[3])
        most = int(ends[0].split()[8])
        H.check(not timer or gap >= 12.0, "%s: polls at least 12 ms apart: %s" % (name, ends[0]))
        H.check(most <= 81, "%s: at most 80 polls in a second: %s" % (name, ends[0]))


def test_pad_play():
    """Each pad key bound to nothing does its job in the game: right and top
    is A, bottom B, L1 L, R1 R, Start, Select, the D-pad
    the arrows. Unplugged with A held: A is let go at once. The messages,
    and the settings file written at the first connection."""
    jobs = [("Pad_A", "A"), ("Pad_X", "none"), ("Pad_B", "B"), ("Pad_Y", "none"), ("Pad_L1", "L"),
            ("Pad_L2", "none"), ("Pad_R1", "R"), ("Pad_R2", "none"), ("Pad_Start", "Start"),
            ("Pad_Select", "Select"), ("Pad_Up", "Up"), ("Pad_Right", "Right"), ("Pad_Home", "none"),
            ("Pad_L3", "none"), ("Pad_Aux", "none")]
    r = H.Run("pad_play").run(START_GAME + "pad in xinput\nwait 0.5\n" +
                              "".join(held_peek(key, "job_" + key) for key, _ in jobs) + """
press menu
wait 0.3
press esc
wait 0.5
hold Pad_A
wait 0.2
peek held 4000130:2
pad out
wait 0.2
peek out 4000130:2
release Pad_A
wait 1.5
quit
""")
    for key, want in jobs:
        got = p1(r, "job_" + key)
        H.check(got == want, "pad play: %s presses %s: %s" % (key, want, got))
    H.check(p1(r, "held") == "A", "pad play: A held before unplugging: %s" % p1(r, "held"))
    H.check(p1(r, "out") == "none", "pad play: unplugged, A let go at once: %s" % p1(r, "out"))
    said = [text for _, text in toasts(r)]
    H.check(said == ["Controller connected (Xbox 360)", "Controller disconnected"],
            "pad play: the messages: %s" % said)
    init = [text for _, text in pad_log(r) if text.startswith("init:")]
    H.check(init and "sleep_hook=1 port_hook=1" in init[0] and "press_at=50 release_at=35" in init[0],
            "pad play: OS sleep and port check handed over, default settings: %s" % init)
    path = r.save(SETTINGS)
    text = open(path).read() if os.path.exists(path) else ""
    H.check("layout=position" in text and "#045e:028e=labels" in text and "os_time=1" in text,
            "pad play: the settings file, with the pad's id in its example: %r" % text[:80])
    check_shutdown(r, "pad play")


def test_pad_one_source():
    """The default mapping, every controller class: each GBA button has
    exactly one pad key (no doubles), and every other key does nothing."""
    keys = ["Pad_A", "Pad_B", "Pad_X", "Pad_Y", "Pad_L1", "Pad_R1", "Pad_L2", "Pad_R2",
            "Pad_Start", "Pad_Select", "Pad_L3", "Pad_R3", "Pad_Aux"]
    want = {"Pad_A": "A", "Pad_B": "B", "Pad_L1": "L", "Pad_R1": "R", "Pad_Start": "Start",
            "Pad_Select": "Select"}
    for kind in ("xinput", "gip", "ds4", "dualsense", "switchpro", "hid"):
        r = H.Run("pad_one_" + kind).run(START_GAME + "pad in %s\nwait 0.5\n" % kind +
                                         "".join(held_peek(k, "k_" + k) for k in keys) + "quit\n")
        got = {k: p1(r, "k_" + k) for k in keys}
        src = {}
        for k, v in got.items():
            if v != "none":
                src.setdefault(v, []).append(k)
        H.check(got == {k: want.get(k, "none") for k in keys} and all(len(v) == 1 for v in src.values()),
                "pad one source: %s: one key per GBA button, the rest none: %s" % (kind, got))


def test_pad_no_doubles():
    """One controller button per GBA button, also once keys are bound: A put on Pad Y on the Controls
    screen's controller view leaves Pad A (A's default) doing nothing; a Pad key taken from another
    button asks first (No keeps it; Use for B moves it, and A is back on its default Pad A); the
    calculator's own key for A stays."""
    head = START_GAME + "pad in xinput\nwait 0.5\npress menu\nwait 0.3\n" + N.settings_tile("keys") + \
        "press enter\nwait 0.3\npress right\nwait 0.2\n" + "press down\n" * 4
    script = head + """press enter
wait 0.2
press Pad_Y
wait 0.3
shot a_set
press esc
press esc
press esc
wait 0.5
""" + "".join(held_peek(k, "k_" + k) for k in ("Pad_Y", "Pad_A", "Pad_B", "del")) + """press menu
wait 0.3
""" + N.settings_tile("keys") + "press enter\nwait 0.3\npress right\nwait 0.2\n" + "press down\n" * 5 + """press enter
wait 0.2
press Pad_Y
wait 0.3
shot clash
press esc
wait 0.3
shot kept
press enter
wait 0.2
press Pad_Y
wait 0.3
press down
press enter
wait 0.3
shot moved
press esc
press esc
press esc
wait 0.5
""" + "".join(held_peek(k, "m_" + k) for k in ("Pad_Y", "Pad_A", "Pad_B")) + "quit\n"
    r = H.Run("pad_no_doubles").run(script)
    H.check(p1(r, "k_Pad_Y") == "A", "no doubles: Pad Y is A: %s" % p1(r, "k_Pad_Y"))
    H.check(p1(r, "k_Pad_A") == "none", "no doubles: Pad A, A's default, does nothing now: %s" % p1(r, "k_Pad_A"))
    H.check(p1(r, "k_Pad_B") == "B", "no doubles: Pad B is still B: %s" % p1(r, "k_Pad_B"))
    H.check(p1(r, "k_del") == "A", "no doubles: the calculator's own key for A stays: %s" % p1(r, "k_del"))
    H.check(r.shot("clash").ui == "keys_clash:0", "no doubles: a taken key asks first, No picked: %s" % r.shot("clash").ui)
    H.check(r.shot("kept").ui.startswith("keys:"), "no doubles: esc keeps it: %s" % r.shot("kept").ui)
    H.check(p1(r, "m_Pad_Y") == "B", "no doubles: Use for B moved Pad Y to B: %s" % p1(r, "m_Pad_Y"))
    H.check(p1(r, "m_Pad_A") == "A", "no doubles: A is back on Pad A: %s" % p1(r, "m_Pad_A"))
    H.check(p1(r, "m_Pad_B") == "none", "no doubles: Pad B, B's default, does nothing now: %s" % p1(r, "m_Pad_B"))


def test_pad_menu():
    """The pad alone: Home opens the menu, the D-pad moves, A picks, B goes
    back, Home leaves; Select and Start held together 0.5 s open it too
    (not held that long, they don't)."""
    r = H.Run("pad_menu").run(START_GAME + """pad in dualsense
wait 0.5
press Pad_Home
wait 0.5
shot home
press Pad_Right
wait 0.2
shot down
press Pad_B
wait 0.5
note back
hold Pad_Select
hold Pad_Start
wait 0.3
release Pad_Start
release Pad_Select
wait 0.5
note short
hold Pad_Select
hold Pad_Start
wait 1.3
release Pad_Start
release Pad_Select
wait 0.5
note combo
press Pad_Up
wait 0.2
note row
press Pad_Left
wait 0.2
note settings_word
press Pad_A
wait 0.3
shot grid
press Pad_Right
wait 0.3
shot settings
press Pad_B
wait 0.3
shot main_again
press Pad_Home
wait 0.5
note left
quit
""")
    H.check(r.shot("home").ui == "saves:0", "pad menu: Home opens the menu on SAVES, RESUME picked: %s" % r.shot("home").ui)
    H.check(r.shot("down").ui == "saves:1", "pad menu: the D-pad moves (RESUME to SAVE): %s" % r.shot("down").ui)
    H.check(r.notes["back"]["ui"] == "", "pad menu: B resumes the game (one press, from SAVES' tiles): %s" % r.notes["back"]["ui"])
    H.check(r.notes["short"]["ui"] == "", "pad menu: Select+Start for 0.3 s: still the game: %s"
            % r.notes["short"]["ui"])
    H.check(r.notes["combo"]["ui"] == "saves:0", "pad menu: Select+Start for 1.3 s: the menu: %s"
            % r.notes["combo"]["ui"])
    H.check(r.notes["row"]["ui"] == "main:%d" % H.MAIN["saves"] and
            r.notes["settings_word"]["ui"] == "main:%d" % H.MAIN["settings"],
            "pad menu: up to the top row, left to SETTINGS: %s %s" % (r.notes["row"]["ui"], r.notes["settings_word"]["ui"]))
    H.check(r.shot("grid").ui == "Settings:0", "pad menu: A on Settings goes into its tiles (Volume): %s" % r.shot("grid").ui)
    H.check(r.shot("settings").ui == "Settings:1", "pad menu: the D-pad moves along the tiles: %s" % r.shot("settings").ui)
    H.check(r.shot("main_again").ui == "main:%d" % H.MAIN["settings"], "pad menu: B goes back to the top row: %s"
            % r.shot("main_again").ui)
    H.check(r.notes["left"]["ui"] == "", "pad menu: Home leaves the menu: %s" % r.notes["left"]["ui"])
    check_shutdown(r, "pad menu")


def test_pad_bind():
    """Pad keys bound on the Controls screen's controller view: A on Pad Y, B on Pad A. Bound,
    Pad A is B only; Pad L1, bound to nothing, is still L. The settings
    file keeps them (key numbers 135, 134); the next start has them."""
    script = START_GAME + "pad in xinput\nwait 0.5\npress menu\nwait 0.3\n" + N.settings_tile("keys") + \
        "press enter\nwait 0.3\npress right\nwait 0.2\n" + "press down\n" * 4 + """wait 0.2
shot a_line
press enter
wait 0.2
press Pad_Y
wait 0.3
press down
press enter
wait 0.2
press Pad_A
wait 0.3
shot after
press esc
press esc
press esc
wait 0.5
""" + held_peek("Pad_Y", "y") + held_peek("Pad_A", "a") + held_peek("Pad_L1", "x") + "quit\n"
    r = H.Run("pad_bind").run(script)
    H.check(r.shot("a_line").active_menu_line("keys") == 4, "pad bind: A's line: %s" % r.shot("a_line").ui)
    H.check(p1(r, "y") == "A", "pad bind: Pad Y, bound to A: %s" % p1(r, "y"))
    H.check(p1(r, "a") == "B", "pad bind: Pad A, bound to B, is B only: %s" % p1(r, "a"))
    H.check(p1(r, "x") == "L", "pad bind: Pad L1, bound to nothing, still L: %s" % p1(r, "x"))
    words = struct.unpack("<72I", open(r.save("gpsp.cfg.tns"), "rb").read()[:288])
    # the calculator's own key stays in the first place, the pad's key goes in the second (high half)
    H.check(words[38 + 4] >> 16 == 135 and words[38 + 5] >> 16 == 134,
            "pad bind: the settings file keeps keys 135 and 134 beside the calculator's: %x %x" % (words[42], words[43]))
    again = H.Run("pad_bind_again", reuse=r).run(START_GAME + "pad in xinput\nwait 0.5\n" +
                                                 held_peek("Pad_Y", "y") + held_peek("Pad_A", "a") + "quit\n")
    H.check(p1(again, "y") == "A" and p1(again, "a") == "B", "pad bind: kept for the next start: %s, %s"
            % (p1(again, "y"), p1(again, "a")))
    check_shutdown(r, "pad bind")


def test_pad_turbo():
    """Turbo on a controller (2026-11-12): every GBA button and every
    button's turbo bound to a controller button of its own (settings file,
    two runs: ten buttons each). The plain one holds its button down, alone;
    the turbo one presses it by turns (1 a second, peeks 0.5 s apart),
    alone. On the Controls screen's controller view, A's line, tab, then
    Pad X: Pad X is turbo A, and Pad A (A's by default) still A."""
    buttons = ["Pad_B", "Pad_A", "Pad_Y", "Pad_X", "Pad_L1", "Pad_R1", "Pad_L2", "Pad_R2", "Pad_L3", "Pad_R3"]
    numbers = [133, 134, 135, 136, 137, 138, 139, 140, 144, 145]
    calc = H.owner_config()[38:48]
    for kind in ("plain", "turbo"):
        pads = [c | (n << 16) for c, n in zip(calc, numbers)]
        config = H.turbo_config(pads if kind == "plain" else calc, pads if kind == "turbo" else [0] * 10)
        script = START_GAME + "pad in xinput\nwait 0.5\n"
        for i, name in enumerate(H.GBA_ORDER):
            script += "hold %s\nwait 0.25\n%srelease %s\nwait 0.3\n" % (buttons[i], H.peeks("%s%d_" % (kind, i)), buttons[i])
        r = H.Run("pad_turbo_" + kind, files={"gpsp_qol.cfg.tns": config}).run(script + "quit\n")
        for i, name in enumerate(H.GBA_ORDER):
            bit = H.GBA_BITS[name]
            got = H.pattern(r, "%s%d_" % (kind, i), bit)
            more = H.others(r, "%s%d_" % (kind, i), bit)
            want = ("1111",) if kind == "plain" else H.TURBO
            H.check(got in want and not more, "pad turbo: %s %s on %s: %s, others %x" % (kind, name, buttons[i], got, more))
        check_shutdown(r, "pad turbo " + kind)

    config = H.turbo_config(calc, H.owner_config()[64:64] + [0, 0, 0, 0, 0, 0, 0, 0, 0, 0])
    script = START_GAME + "pad in xinput\nwait 0.5\npress menu\nwait 0.3\n" + N.settings_tile("keys") + \
        "press enter\nwait 0.3\npress right\nwait 0.2\n" + "press down\n" * 4 + """press tab
wait 0.2
shot capture
press Pad_X
wait 0.3
shot after
press esc
press esc
press esc
wait 0.5
hold Pad_X
wait 0.25
""" + H.peeks("x") + "release Pad_X\nwait 0.3\nhold Pad_A\nwait 0.25\n" + H.peeks("a") + "release Pad_A\nwait 0.3\nquit\n"
    r = H.Run("pad_turbo_bind", files={"gpsp_qol.cfg.tns": config}).run(script)
    H.check(r.shot("capture").ui == "keys_turbo:4" and r.shot("after").ui == "keys:4",
            "pad turbo: tab on A's line waits for its turbo button: %s %s" % (r.shot("capture").ui, r.shot("after").ui))
    H.check(H.pattern(r, "x", 1) in H.TURBO, "pad turbo: Pad X, turbo A: by turns: %s" % H.pattern(r, "x", 1))
    H.check(H.pattern(r, "a", 1) == "1111", "pad turbo: Pad A still A: %s" % H.pattern(r, "a", 1))
    words = struct.unpack("<82I", open(r.save("gpsp.cfg.tns"), "rb").read())
    H.check(words[72 + 4] >> 16 == 136 or words[72 + 4] & 0xFFFF == 136, "pad turbo: kept in word 76: %08x" % words[76])
    check_shutdown(r, "pad turbo bind")


def test_pad_two():
    """Two pads both drive the GBA's one player; the messages number them
    only while both are in. Messages that come together show a second
    apart. One that comes in the menus waits for the game."""
    r = H.Run("pad_two").run("""wait 1
pad in xinput
wait 2
note list
press enter
wait 2
pad in ds4
wait 1.5
""" + held_peek("Pad_A", "both") + """pad out 1
wait 1.5
""" + held_peek("Pad_A", "second") + """pad out 2
wait 1.5
pad port 1
pad event replug
pad event power
pad event unsupported
wait 3.5
quit
""")
    said = toasts(r)
    texts = [text for _, text in said]
    # R11w part 2: the new words (no USB ids on screen: the page and the log
    # have them)
    H.check(texts == ["Controller connected (Xbox 360)", "Controller 2 connected (DualShock 4)",
                      "Controller 1 disconnected", "Controller disconnected", "Plug the controller in again",
                      "Controller needs more power: try a powered hub", "This controller isn't supported"],
            "pad two: the messages: %s" % texts)
    started = float(r.notes["list"]["time"])
    H.check(said and said[0][0] >= started, "pad two: plugged in at the file list (2 s before), said in the "
            "game: %s (list left at %.2f)" % (said[:1], started))
    last = [t for t, _ in said[-3:]]
    H.check(len(last) == 3 and last[1] - last[0] >= 0.99 and last[2] - last[1] >= 0.99,
            "pad two: messages that came together a second apart: %s" % last)
    H.check(p1(r, "both") == "A" and p1(r, "second") == "A", "pad two: either pad presses A: %s, %s"
            % (p1(r, "both"), p1(r, "second")))
    check_shutdown(r, "pad two")


def test_pad_settings():
    """The pad settings file: thresholds, the stick and os_time go to the
    driver; layout=labels makes the bottom button Pad A, and a pad's own
    line (its USB id) overrides that. Thresholds the driver refuses leave
    its defaults; the file isn't written over."""
    text = ("# mine\nstick_dpad=0\npress_at=60\nrelease_at=20\nlayout=labels\n045e:028e=position\n"
            "os_time=1\n")
    r = H.Run("pad_settings")
    os.makedirs(r.save(""), exist_ok=True)
    open(r.save(SETTINGS), "w").write(text)
    r.run(START_GAME + "pad in ds4\nwait 0.5\n" + held_peek("Pad_B", "ds4_bottom") +
          held_peek("Pad_A", "ds4_right") + "pad out\nwait 0.3\npad in xinput\nwait 0.5\n" +
          held_peek("Pad_B", "x360_bottom") + "quit\n")
    init = [t for _, t in pad_log(r) if t.startswith("init:")]
    H.check(init and "stick_dpad=0 press_at=60 release_at=20 os_time=1" in init[0],
            "pad settings: given to the driver: %s" % init)
    H.check(p1(r, "ds4_bottom") == "A" and p1(r, "ds4_right") == "B",
            "pad settings: labels: bottom A, right B: %s, %s" % (p1(r, "ds4_bottom"), p1(r, "ds4_right")))
    H.check(p1(r, "x360_bottom") == "B", "pad settings: 045e:028e=position: bottom B: %s" % p1(r, "x360_bottom"))
    H.check(open(r.save(SETTINGS)).read() == text, "pad settings: the file left as it was")

    bad = H.Run("pad_settings_bad")
    os.makedirs(bad.save(""), exist_ok=True)
    open(bad.save(SETTINGS), "w").write("stick_dpad=0\npress_at=20\nrelease_at=30\n")
    bad.run("wait 1\nquit\n")
    init = [t for _, t in pad_log(bad) if t.startswith("init:")]
    H.check(init and "stick_dpad=0 press_at=50 release_at=35" in init[0],
            "pad settings: refused thresholds: their defaults, the stick setting kept: %s" % init)


def test_pad_os_time():
    """While the driver asks for OS time the game's frames sleep an OS tick
    (in their spare time, or once in 0.5 s). With a pad in and os_time=1
    (the default: gpSP masks interrupts, so reports come only in the OS's
    ticks) it asks every poll and sleeps at least every ~2 frames."""
    r = H.Run("pad_os_time").run(START_GAME + """pad want on
wait 2
pad want off
wait 0.1
pad in xinput
wait 2
pad out
quit
""")
    sleeps = {}
    for _, text in pad_log(r):
        m = re.match(r"pad (\S+) ?(\S*) \(OS sleeps so far (\d+)\)$", text)
        if m:
            sleeps[(m.group(1) + " " + m.group(2)).strip()] = int(m.group(3))
    want = [sleeps.get(k) for k in ("want on", "want off", "in xinput", "out")]
    H.check(None not in want, "pad os time: logged: %s" % sleeps)
    if None not in want:
        on, off, pad_in, pad_out = want
        H.check(off - on >= 4, "pad os time: asked for 2 s: %d OS sleeps" % (off - on))
        H.check(pad_out - pad_in >= 50, "pad os time: a pad in, os_time=1: a tick at least every ~2 frames "
                "in 2 s: %d OS sleeps" % (pad_out - pad_in))
    check_shutdown(r, "pad os time")


def test_pad_stale_hints():
    """Messages a newer state has made wrong aren't shown: "Plug the
    controller in again", posted at the file list before the pad was
    replugged there; "may need more power" for the pad that then connected. A
    hint for another pad still shows."""
    r = H.Run("pad_stale").run("""wait 1
pad event replug
pad event power gip
pad event power ds4
wait 1
pad in gip
wait 1
press enter
wait 4
quit
""")
    texts = [text for _, text in toasts(r)]
    # (R11w part 2: the power message's new words)
    H.check(texts == ["Controller needs more power: try a powered hub", "Controller connected (Xbox One)"],
            "pad stale hints: the messages: %s" % texts)
    check_shutdown(r, "pad stale hints")


def test_pad_failed():
    """A pad that stopped answering (the driver's USBPAD_EV_FAILED, which
    comes after the DISCONNECTED that lets its player go): its buttons are
    let go at once, it says "Controller stopped answering" (its own words,
    no wider than the other pad messages), the log has the line with the USB
    id, and "Controller not supported" stays for what no driver could
    start. If the same pad connected again while the message still waited
    (at the file list), it isn't said."""
    r = H.Run("pad_failed").run(START_GAME + """pad in gip
wait 1.5
hold Pad_A
wait 0.3
peek held 4000130:2
pad event failed gip
wait 0.3
peek failed 4000130:2
release Pad_A
wait 2.5
pad event unsupported
wait 2
quit
""")
    texts = [text for _, text in toasts(r)]
    # R11w part 2: the new words, and the width limit the one rule's: 50
    # characters (was 36, the old pad messages' widest)
    H.check(texts == ["Controller connected (Xbox One)", "Controller disconnected", "Controller stopped. Plug it in again.",
                      "This controller isn't supported"], "pad failed: the messages: %s" % texts)
    H.check(all(len(t) <= 50 for t in texts), "pad failed: no message over 50 characters: %s" % texts)
    H.check(p1(r, "held") == "A", "pad failed: A held while it worked: %s" % p1(r, "held"))
    H.check(p1(r, "failed") == "none", "pad failed: its buttons let go at once: %s" % p1(r, "failed"))
    lines = link_log(r)
    H.check(has(lines, "pad: event: failed", "045e:02ea", "class Xbox One"), "pad failed: the log line: %s" % lines)
    H.check(has(lines, "pad: event: unsupported", "2563:0575"), "pad failed: unsupported still its own line")
    H.check(has(lines, "pad: diag (failure)"), "pad failed: the diag with the failure")
    H.check(has(lines, "seamless: OS side: match offer", "vid 045e"),
            "pad failed: the arbiter is told the OS offered it (no power cycle for it): %s" % lines)
    check_shutdown(r, "pad failed")
    r = H.Run("pad_failed_stale").run("""wait 1
pad in gip
wait 1
pad event failed gip
pad in gip
wait 1
press enter
wait 6
quit
""")
    texts = [text for _, text in toasts(r)]
    H.check(texts == ["Controller connected (Xbox One)", "Controller disconnected", "Controller connected (Xbox One)"],
            "pad failed: replugged before the message: it isn't said: %s" % texts)
    check_shutdown(r, "pad failed stale")


def sleeps_at(run):
    """OS sleeps so far at each logged pad line, by its words ("port 1")."""
    seen = []
    for _, text in pad_log(run):
        m = re.match(r"pad (\S+) ?(\S*) \(OS sleeps so far (\d+)\)$", text)
        if m:
            seen.append(((m.group(1) + " " + m.group(2)).strip(), int(m.group(3))))
    return seen


def test_pad_replug_port():
    """"Plug the controller in again" only while something's at the host's
    end of the USB port: not with nothing on it, nor at the device's end (a
    computer), nor on a CX whose controller is off with nothing plugged in
    (it reads "can't tell", which the driver takes for something)."""
    r = H.Run("pad_replug_port").run("""wait 1
pad port -1
pad event replug
wait 0.5
press enter
wait 4
pad port 0
pad event replug
wait 1.5
pad port 2
pad event replug
wait 1.5
pad port 1
pad event replug
wait 1.5
quit
""")
    texts = [text for _, text in toasts(r)]
    H.check(texts == ["Plug the controller in again"], "pad replug port: only with an adapter in: %s" % texts)
    check_shutdown(r, "pad replug port")


def test_pad_attach_time():
    """Before the driver sees a device the OS's hub work needs its ticks:
    after the port changes, with something at the host's end and no pad in,
    a tick at least every ~2 frames, in the menus too (they pace without
    sleeping: there the keys are read once a frame); at the device's end,
    nothing."""
    r = H.Run("pad_attach").run(START_GAME + """press menu
wait 0.5
pad port 2
wait 2
pad port 1
wait 2
pad port 0
wait 1
press esc
wait 0.5
pad port 1
wait 2
pad in xinput
wait 1
quit
""")
    seen = sleeps_at(r)
    words = [w for w, _ in seen]
    H.check(words == ["port 2", "port 1", "port 0", "port 1", "in xinput"], "pad attach: logged: %s" % seen)
    if len(seen) == 5:
        device = seen[1][1] - seen[0][1]
        menu = seen[2][1] - seen[1][1]
        game = seen[4][1] - seen[3][1]
        H.check(device <= 2, "pad attach: the device's end, in the menu: no OS time: %d sleeps in 2 s" % device)
        H.check(menu >= 30, "pad attach: an adapter in, in the menu: a tick every few frames: %d sleeps in 2 s"
                % menu)
        H.check(game >= 50, "pad attach: an adapter in, in the game: a tick every ~2 frames: %d sleeps in 2 s"
                % game)
    check_shutdown(r, "pad attach")


def test_pad_attach_ends():
    """That time ends: about 12 s after the port changed, with no pad, the
    OS gets the driver's idle ticks only (a few a second). In the menu: in
    the game the link watches a port with something at its host's end (a
    calculator's cable looks the same) and gives the OS every frame's spare
    time."""
    r = H.Run("pad_attach_ends").run(START_GAME + """press menu
wait 0.5
pad port 1
wait 16
pad want off
wait 2
pad want off
quit
""")
    seen = sleeps_at(r)
    H.check(len(seen) == 3, "pad attach ends: logged: %s" % seen)
    if len(seen) == 3:
        late = seen[2][1] - seen[1][1]
        H.check(late <= 12, "pad attach ends: 16 s on, a few a second: %d sleeps in 2 s" % late)
    check_shutdown(r, "pad attach ends")


def test_pad_link_refused():
    """Link picked with a pad in the port: refused (NavNet would open on its
    adapter, and the driver refuses new pads while a link is up)."""
    r = H.Run("pad_link_refused").run(START_GAME + "pad in xinput\nwait 0.5\nlink\nwait 0.5\nquit\n")
    shown = [text[4:] for _, text in pad_log(r) if text.startswith("osd ")]
    H.check("The USB port has a controller in it" in shown, "pad link refused: the message: %s" % shown)
    check_shutdown(r, "pad link refused")


def test_pad_gated():
    """A CX whose OS turned the USB controller off (nothing plugged in: it
    reads "can't tell"): the link doesn't watch the cable, which would give
    the OS every frame's spare time, but the OS gets a tick about every
    250 ms."""
    r = H.Run("pad_gated").run(START_GAME + "pad want off\npad port -1\nwait 2\npad want off\nquit\n")
    seen = sleeps_at(r)
    H.check(len(seen) == 3, "pad gated: logged: %s" % seen)
    if len(seen) == 3:
        n = seen[2][1] - seen[1][1]
        H.check(4 <= n <= 14, "pad gated: a tick about every 250 ms: %d sleeps in 2 s" % n)
    check_shutdown(r, "pad gated")


def test_pad_gated_quit():
    """EXIT-R2: the CX CAS after a link: something was on the port (the
    pad log written this session), then its OS gated the controller off
    (plugged reads -1, "can't tell"), and the user quits. The pad shutdown
    is the quiet one (no OS ticks, no log write before it: a gated
    controller has no clock, so nothing can be on its way) and the log is
    written once at the quit, by the exit's end."""
    r = H.Run("pad_gated_quit").run(START_GAME + "pad port 1\nwait 1\npad port -1\nwait 2\npress Q\nwait 1\n"
                                    "note after\nquit\n")
    H.check("after" not in r.notes, "pad gated quit: gpSP quit by itself")
    marker = open(r.save("gpsp_link_run.txt.tns"), encoding="utf-8").read()
    H.check("pad driver shut down (no wait)" in marker, "pad gated quit: the pad shutdown waits on nothing: %r" %
            marker[:600])
    H.check(r.report.get("log_file_writes_at_exit") == "1",
            "pad gated quit: the log written once at the quit: %s" % r.report.get("log_file_writes_at_exit"))
    log = open(r.save("gpsp_link.txt.tns"), encoding="utf-8", errors="replace").read()
    H.check(re.search(r"exit: shutting down, 0 pad\(s\) in.*plugged -1, gated 1, bits 00000000.*"
                      r"the controller is gated off: no wait", log),
            "pad gated quit: the exit line says why (plugged, gated, bits): %r" %
            [l for l in log.splitlines() if "shutting down" in l])
    check_shutdown(r, "pad gated quit")


def test_exit_untouched():
    """r19-usb-exit: a CX quit (r19, 926fa73; its exit marker: the cable's plain end all run, no
    device ever offered): "USB audio shut down 669 ... pad driver shut down
    101 ... USB drive shut down 100" of 1531 ms, each driver's shutdown
    waiting OS ticks for callbacks that never ran (the sound's 10 after its
    retire, with the run's first look for the OS's sleep; the pad driver's
    10; the drive's 10). The cable's plain end is quiet now as an empty port
    is (no host's driver is called in device mode), and with no offer ever
    the sound's and the drive's stops wait for nothing either. A device
    offered in the run (a pad in and out) or in at the quit: their usual
    stops, as before."""
    quiet_steps = ("USB audio shut down (no wait)", "pad driver shut down (no wait)", "USB drive shut down (no wait)")
    for name, port in (("cx2", 2), ("cx", 4)):
        r = H.Run("exit_untouched_" + name).run(START_GAME + "pad port %d\nwait 2\npress Q\nwait 1\nnote after\nquit\n"
                                                % port)
        H.check("after" not in r.notes, "exit untouched %s: gpSP quit by itself" % name)
        marker = run_marker(r)
        steps = marker.split("\n")[1] if marker.count("\n") > 1 else ""
        for step in quiet_steps:
            m = re.search(re.escape(step) + r" (\d+)( \[[^]]*\])?,", steps)
            H.check(m and not m.group(2), "exit untouched %s: %r, no OS sleep in it: %r" % (name, step, steps[:900]))
        log = "\n".join(link_log(r))
        H.check(re.search(r"exit: shutting down, 0 pad\(s\) in.*the port is the cable's plain end \(a USB device\): "
                          r"no wait", log),
                "exit untouched %s: the exit line says why: %r" % (name, [l for l in log.splitlines() if "shutting" in l]))
        check_shutdown(r, "exit untouched " + name)
    # a pad offered in the run, gone 2 s before the quit: the pad driver's stop is quiet as before (EXIT-3), the
    # sound's and the drive's are their usual ones (a callback of theirs may have run)
    r = H.Run("exit_offered_before").run(START_GAME + "pad in xinput\nwait 1\npad out\nwait 2\npress Q\nwait 1\n"
                                         "note after\nquit\n")
    marker = run_marker(r)
    H.check("pad driver shut down (no wait)" in marker and "USB audio shut down (no wait)" not in marker and
            "USB drive shut down (no wait)" not in marker and "USB audio shut down" in marker and
            "USB drive shut down" in marker,
            "exit offered before: the pad's stop quiet, the sound's and the drive's usual: %r" % marker[:900])
    check_shutdown(r, "exit offered before")
    # a pad in at the quit: every stop its usual one
    r = H.Run("exit_pad_in").run(START_GAME + "pad in xinput\nwait 1\npress Q\nwait 1\nnote after\nquit\n")
    marker = run_marker(r)
    H.check("(no wait)" not in marker and "pad driver shut down" in marker,
            "exit pad in: no stop without its wait: %r" % marker[:900])
    check_shutdown(r, "exit pad in")


def test_exit_log_buffers():
    """r19-usb-exit: the logs written a line at a time reach the OS a buffer
    at a time, and Ndless's newlib's is 1 KB (its _fstat gives no block size:
    libsyscalls/stdlib.cpp), one OS fwrite each: the CX II's evening quit
    wrote its 36 KB pad log in 153 ms (4.2 ms a KB) where a state's 8 KB parts
    take 0.75 ms a KB, and the CX's calls cost ~3.4 times as much. The quit's
    pad log, perf log (the sound's seconds) and state times go with 16 KB
    buffers (the report's log_buffer:NAME, the last setvbuf before each close,
    -1 not written)."""
    r = H.Run("exit_log_buffers").run(START_GAME + "pad in xinput\nwait 1\npress S\nwait 1\npress Q\nwait 1\n"
                                      "note after\nquit\n", env={"GPSP_HOST_AUDIO": "1"})
    H.check("after" not in r.notes, "exit log buffers: gpSP quit by itself")
    sizes = {k.split(":", 1)[1]: int(v) for k, v in r.report.items() if k.startswith("log_buffer:")}
    H.check(sizes.get("gpsp_pad.txt.tns") == 16384,
            "exit log buffers: the pad log written with a 16 KB buffer: %s" % sizes)
    for name, size in sizes.items():
        H.check(size in (-1, 16384), "exit log buffers: %s written with a 16 KB buffer (-1: not written): %d" %
                (name, size))
    check_shutdown(r, "exit log buffers")


def test_pad_toasts_wait():
    """gpSP's own messages keep their 2 s: "Resumed from slot" as the game
    starts (a pad plugged in at the file list says so after it), and a
    hotkey's (S: "Saved to slot") when a pad comes out just after. (Since
    final/pace6 the state is written over the next frames and "Saved to
    slot" said once it is, ~0.5 s after the key: the pad comes out 1 s
    after it, just after the message, as it came 0.2 s after it before.)"""
    first = H.Run("pad_wait_state").run(START_GAME + "press Q\nwait 1\nquit\n")
    r = H.Run("pad_wait", reuse=first).run("""wait 1
pad in xinput
wait 1
press enter
wait 4
press S
wait 1
pad out
wait 4
quit
""")
    shown = [(t, text[4:]) for t, text in pad_log(r) if text.startswith("osd ")]
    ours = [(t, text) for t, text in shown if text.startswith("Controller")]
    theirs = [(t, text) for t, text in shown if not text.startswith("Controller")]
    H.check([text for _, text in ours] == ["Controller connected (Xbox 360)", "Controller disconnected"],
            "pad toasts wait: the pad's messages: %s" % shown)
    resumed = [t for t, text in theirs if text.startswith("Resumed from slot")]
    saved = [t for t, text in theirs if text.startswith("Saved to slot")]
    H.check(len(resumed) == 1 and len(saved) == 1, "pad toasts wait: gpSP's messages: %s" % shown)
    if len(ours) == 2 and resumed and saved:
        H.check(ours[0][0] - resumed[0] >= 1.99, "pad toasts wait: connected %.2f s after \"Resumed\""
                % (ours[0][0] - resumed[0]))
        H.check(ours[1][0] - saved[0] >= 1.99, "pad toasts wait: disconnected %.2f s after \"Saved\""
                % (ours[1][0] - saved[0]))
        H.check(ours[1][0] - saved[0] <= 2.2, "pad toasts wait: but not much longer: %.2f s"
                % (ours[1][0] - saved[0]))
    check_shutdown(r, "pad toasts wait")


def test_pad_no_timer():
    """An older CX whose timer doesn't run: gpSP's time moves only as frames
    wait, so the driver's polls are counted against the real-time clock:
    80 in any second at most, in the game and on the Controls screen (which
    reads the keys flat out), and binding a pad key still works there."""
    script = H.REAL_COSTS + START_GAME + "pad in xinput\nwait 0.5\npress menu\nwait 0.3\n" + \
        N.settings_tile("keys") + "press enter\nwait 0.3\npress right\nwait 0.2\n" + "press down\n" * 4 + """wait 0.2
press enter
wait 2.5
press Pad_Y
wait 0.3
press esc
press esc
press esc
wait 1
""" + held_peek("Pad_Y", "y") + "quit\n"
    r = H.Run("pad_no_timer").run(script, options=["--model", "cx", "--timer-dead", "1"])
    H.check(p1(r, "y") == "A", "pad no timer: Pad Y bound to A on the Controls screen: %s" % p1(r, "y"))
    check_shutdown(r, "pad no timer", timer=False)


def link_log(run):
    """The link log's lines, without their ms."""
    path = run.save("gpsp_link.txt.tns")
    if not os.path.exists(path):
        return []
    return [l.split(None, 1)[1] if len(l.split(None, 1)) > 1 else "" for l in open(path, encoding="utf-8")]


def has(lines, *words):
    return any(all(w in l for w in words) for l in lines)


def test_pad_log():
    """gpsp_link.txt.tns tells the pad's story: the driver's start (init
    result, status, its diag), the port's changes, a pad's attach (USB id,
    class, slot), its unplug, replug hint, unsupported and power refusal
    events, the driver's diag at each and at exit; one file, led by the
    build id."""
    r = H.Run("pad_log").run(START_GAME + """pad port 1
wait 0.3
pad in xinput
wait 0.5
pad out
wait 0.5
pad event replug
wait 0.5
pad event power dualsense
wait 0.5
pad event unsupported
wait 0.5
quit
""")
    lines = link_log(r)
    H.check(lines and lines[0].startswith("gpSP build "), "pad log: led by the build id: %s" % lines[:1])
    H.check(has(lines, "pad: usbpad_init -> 0", "(ok)"), "pad log: init result: %s" % lines[:6])
    H.check(has(lines, "pad: diag (at start)", "backend=os registered="), "pad log: the driver's diag at start")
    H.check(has(lines, "pad: USB port", "(was"), "pad log: the port's change")
    H.check(has(lines, "pad: event: attach", "slot 0", "045e:028e", "Xbox 360"), "pad log: attach: %s" % lines)
    H.check(has(lines, "pad: diag (event)", "matches=1 attaches=1"), "pad log: the diag at attach")
    H.check(has(lines, "pad: event: detach", "slot 0"), "pad log: unplug")
    H.check(has(lines, "pad: event: replug hint", "2563:0575"), "pad log: replug hint")
    H.check(has(lines, "pad: event: power refusal", "054c:0ce6"), "pad log: power refusal: %s" % lines)
    H.check(has(lines, "pad: event: unsupported", "2563:0575"), "pad log: no-match (unsupported)")
    H.check(has(lines, "pad: diag (failure)", "usbd_do_request="), "pad log: full diag at a failure")
    H.check(has(lines, "pad: exit:", "forced OS ticks"), "pad log: the exit line")
    H.check(has(lines, "pad: diag (after shutdown)"), "pad log: the diag after the shutdown")
    check_shutdown(r, "pad log")


def test_pad_log_input():
    """S10: the log tells what the pad itself sent: the first report, then
    each button once (a second press adds no line), and again after a
    replug (a new attach is a new story)."""
    r = H.Run("pad_log_input").run(START_GAME + """pad in xinput
wait 0.5
press Pad_A
wait 0.3
press Pad_A
wait 0.3
press Pad_Up
wait 0.3
pad out
wait 0.5
pad in xinput
wait 0.5
press Pad_A
wait 0.3
quit
""")
    lines = link_log(r)
    n = lambda *w: sum(1 for l in lines if all(x in l for x in w))
    H.check(n("pad: first report", "045e:028e") == 2, "pad input: first report once per attach: %s" % lines)
    H.check(n("pad: button: slot 0, east") == 2, "pad input: A once per attach: %s" % lines)
    H.check(n("pad: button: slot 0, up") == 1, "pad input: Up once")


def test_pad_log_replug():
    """Unplug and plug the same pad again: two attaches, a detach between,
    and the OS sleeps at each."""
    r = H.Run("pad_log_replug").run(START_GAME + "pad in xinput\nwait 0.5\npad out\nwait 0.5\npad in xinput\nwait 0.5\nquit\n")
    lines = link_log(r)
    n = sum(1 for l in lines if "pad: event: attach" in l)
    H.check(n == 2 and has(lines, "pad: event: detach"), "pad log replug: attach, detach, attach: %s" % lines)
    H.check(has(lines, "pad: diag (event)", "matches=2 attaches=2"), "pad log replug: the diag counts both")


def test_pad_log_never_offered():
    """A failed attempt the log alone explains: something at the port, the
    OS never offers the driver a device (its match callback ran once, the
    attach didn't: a request failed with -5): the log says the port's
    state, that no device came in 12 s, the driver's counters and the
    last error, and the forced ticks."""
    r = H.Run("pad_log_never").run(START_GAME + """pad port 1
pad diag 1 0 -5
wait 20
quit
""")
    lines = link_log(r)
    gave = [l for l in lines if "OS offered the driver 1 time(s) in 12 s" in l]
    H.check(len(gave) == 1, "pad never: gave up once, after 12 s, saying it was offered (RDY-6): %s" % lines)
    H.check(has(lines, "pad: USB: PC socket"), "pad never: the controller's registers on the give-up (RDY-3): %s" % lines)
    H.check(has(lines, "pad: USB port", "forced OS ticks"), "pad never: the port's change and the OS time")
    H.check(has(lines, "pad: diag (gave up)", "matches=1 attaches=0", "last_err=-5"),
            "pad never: which callback ran, which request failed: %s" % lines)
    H.check(not has(lines, "pad: event: attach"), "pad never: no attach")
    H.check(has(lines, "forced OS ticks") and has(lines, "pad: exit: shutting down, 0 pad(s)"), "pad never: forced ticks counted")


def test_pad_log_exit():
    """The exit lines (PAD-6, RDY-9): the count of pads is taken before the
    shutdown empties the slots, the "shutting down" line is on the disk
    before it starts (a hang in the shutdown would leave it), the result and
    the clean marker come after; the shutdown's wait for a blocked attach
    (the driver's wait hook) is logged; and a run whose first log write is
    the exit's still ends with the clean marker."""
    r = H.Run("pad_log_exit").run(START_GAME + "pad in xinput\nwait 1\npad slowexit\nquit\n")
    lines = link_log(r)
    H.check(has(lines, "pad: exit: shutting down, 1 pad(s) in"), "pad exit: the count before the shutdown: %s" % lines)
    H.check(has(lines, "pad: exit: usbpad_shutdown returned (1 pad(s) were in)"), "pad exit: the result: %s" % lines)
    H.check(not has(lines, "pad: exit: 0 pad(s) in"), "pad exit: never the shutdown's own 0")
    first = [i for i, l in enumerate(lines) if "pad: exit: shutting down" in l]
    wait = [i for i, l in enumerate(lines) if "pad: exit: waiting for the USB controller" in l]
    done = [i for i, l in enumerate(lines) if "pad: exit: usbpad_shutdown returned" in l]
    before = [i for i, l in enumerate(lines) if "pad: diag (before shutdown)" in l]
    after = [i for i, l in enumerate(lines) if "pad: diag (after shutdown)" in l]
    H.check(first and wait and done and before and after and first[0] < before[0] < wait[0] < done[0] < after[0],
            "pad exit: shutting down, wait, result, in that order: %s" % lines)
    marker = run_marker(r)
    H.check(marker.startswith("clean"), "pad exit: the clean marker: %s" % marker)
    # The exit's lines are the run's first log write (the port was plugged with no
    # pad log before): the marker must still end as "clean", not "running".
    r = H.Run("pad_log_exit_first").run(START_GAME + "pad port 1\nquit\n")
    marker = run_marker(r)
    H.check(marker.startswith("clean"), "pad exit first write: the clean marker: %s" % marker)
    H.check(has(link_log(r), "pad: exit: shutting down"), "pad exit first write: it wrote the log: %s" % link_log(r))


def run_marker(run):
    path = run.save("gpsp_link_run.txt.tns")
    return open(path).read() if os.path.exists(path) else ""


def test_pad_log_stages():
    """How far the OS's USB task got, one line when each counter first
    moves (RDY-4, RDY-8): the first match (with the OS's offers), the first
    attach, the first report (its bytes and what the pad made of them), the
    first IN error, a synthesised configuration; the heartbeat's pad line;
    in the menus the lines reach the file before any game frame."""
    r = H.Run("pad_log_stages").run(START_GAME + """pad port 1
wait 0.3
pad in xinput
wait 1
pad error 2 17
wait 1
pad synth
wait 0.5
pad dropped 3
wait 12
quit
""")
    lines = link_log(r)
    H.check(has(lines, "pad: stage: the OS called the driver's match"), "pad stages: first match: %s" % lines)
    H.check(has(lines, "pad: diag (first match)", "match[0]:"), "pad stages: the OS's offers with the first match")
    H.check(has(lines, "pad: stage: the OS called the driver's attach"), "pad stages: first attach")
    H.check(has(lines, "pad: stage: the first report, 8 bytes", "parsed for slot 0"), "pad stages: first report: %s" % lines)
    H.check(has(lines, "pad: stage: the first IN error: status 17"), "pad stages: first IN error")
    H.check(has(lines, "pad: stage: the OS's configuration descriptor wasn't the whole set"), "pad stages: synth")
    m = [i for i, l in enumerate(lines) if "pad: stage: the OS called the driver's match" in l]
    a = [i for i, l in enumerate(lines) if "pad: stage: the OS called the driver's attach" in l]
    f = [i for i, l in enumerate(lines) if "pad: stage: the first report" in l]
    H.check(m and a and f and m[0] < a[0] < f[0], "pad stages: in the order the OS gets there")
    text = open(r.save("gpsp_link.txt.tns"), encoding="utf-8").read()
    beat = [l for l in text.split("\n") if "  pad: reports" in l]
    H.check(beat and "parsed" in beat[0] and "in_err 2 (last status 17)" in beat[0] and "dropped 3" in beat[0]
            and re.search(r"OS gap \d+ ms$", beat[0].rstrip()), "pad stages: the heartbeat's pad line: %s" % beat)
    # Only the menus run: a crash there (nothing written at exit) leaves the log.
    r = H.Run("pad_log_menu")
    r.start("pad port 1\nwait 0.3\npad in xinput\nwait 3\ncrash\n").communicate(timeout=120)
    path = r.save("gpsp_link.txt.tns")
    H.check(os.path.exists(path) and has(link_log(r), "pad: event: attach"),
            "pad menu flush: the attach is on the disk after a crash in the menus")
    H.check(not os.path.exists(r.save("gpsp_link_run.txt.tns")) or "clean" not in run_marker(r),
            "pad menu flush: and no clean marker")


def test_pad_log_offers():
    """match-fix (2026-11-11): the SEAM1 log had the driver's offers only at
    the first match (the other calculator) and when the glue gave up: the
    driver's diag (its match ring, reasons, raw words) again at the first
    offer after each port change."""
    r = H.Run("pad_log_offers").run(START_GAME + """pad port 1
wait 0.3
pad in xinput
wait 1
pad out 1
pad port 0
wait 1
pad port 1
wait 0.3
pad in xinput
wait 1
quit
""")
    lines = link_log(r)
    H.check(has(lines, "pad: diag (first match)", "match[0]:"), "pad offers: the first match's diag")
    H.check(has(lines, "pad: stage: the OS offered the driver a device after the port changed (2 offers so far)"),
            "pad offers: the second device's offer: %s" % [l for l in lines if "pad:" in l])
    H.check(has(lines, "pad: diag (offer)", "match[0]:"), "pad offers: its diag with the ring")
    H.check(not has(lines, "pad: diag (offer)", "addresses:"), "pad offers: level 1 (no addresses)")


def test_pad_log_quiet():
    """Nothing on the port: the pad glue writes no log (the last stays)."""
    r = H.Run("pad_log_quiet").run(START_GAME + "wait 1\nquit\n")
    H.check(not os.path.exists(r.save("gpsp_link.txt.tns")), "pad log quiet: no log file")


def test_pad_preplug():
    """'pad preplug': a pad on the bus from t0 that the OS never offers (the
    driver registered after it: usbshim.h:16-18), and a power cycle that does
    nothing ('pad cycle ignored': what S4's cycles meet when the OS won't
    re-offer): the glue ends up with matches=0 after 12 s."""
    r = H.Run("pad_preplug").run(START_GAME + "pad preplug xinput\npad cycle ignored\nwait 20\nquit\n")
    lines = link_log(r)
    H.check(len([l for l in lines if "OS offered no device in 12 s" in l]) == 1,
            "pad preplug: the 12 s give-up: %s" % lines)
    H.check(has(lines, "pad: diag (gave up)", "matches=0 attaches=0"), "pad preplug: no match callback: %s" % lines)
    H.check(not has(lines, "pad: event: attach"), "pad preplug: no attach: %s" % lines)
    H.check(not toasts(r) or "connected" not in toasts(r)[0][1], "pad preplug: no pad connected: %s" % toasts(r))
    st = [re.search(r"stage (\d)", l).group(1) for l in lines if "seamless: stage" in l]
    H.check(st == ["1", "2", "3", "4"], "pad preplug: the arbiter's stages 1 2 3 4 (hooks empty): %s" % st)


def powered(run, word):
    return [t for t, text in pad_log(run) if text.startswith("pad buspower " + word)]


def test_pad_cycle():
    """'pad cycle ok|ignored|late N' and 'pad buspower': a bus power cycle
    re-offers the device 300 ms after power returns (ok), never (ignored) or
    from the N-th cycle (late), and the device leaves while power is off."""
    cyc = "pad buspower off\nwait 0.5\npad buspower on\nwait 1\n"
    r = H.Run("pad_cycle_ok").run(START_GAME + "pad preplug xinput\nwait 1\npad cycle ok\n" + cyc + "quit\n")
    on, got = powered(r, "on"), [t for t, _ in toasts(r) if t]
    ev = [t for t, text in pad_log(r) if text == "osd Controller connected (Xbox 360)"]
    H.check(len(on) == 1 and len(ev) == 1 and 0.29 <= ev[0] - on[0] <= 0.5,
            "pad cycle ok: offered 300 ms after power: on %s connected %s" % (on, ev))
    r = H.Run("pad_cycle_ign").run(START_GAME + "pad preplug xinput\nwait 1\npad cycle ignored\n" + cyc + "quit\n")
    H.check(not [1 for t, text in pad_log(r) if "connected" in text], "pad cycle ignored: never offered: %s" % pad_log(r))
    r = H.Run("pad_cycle_late").run(START_GAME + "pad preplug xinput\nwait 1\npad cycle late 2\n" + cyc + "wait 0.5\nnote first\n" +
                                    cyc + "quit\n")
    ev = [t for t, text in pad_log(r) if text == "osd Controller connected (Xbox 360)"]
    on = powered(r, "on")
    H.check(len(on) == 2 and len(ev) == 1 and ev[0] > on[1], "pad cycle late 2: only the second cycle: on %s ev %s" % (on, ev))
    r = H.Run("pad_cycle_drop").run(START_GAME + "pad in xinput\nwait 1\npad buspower off\nwait 1\nquit\n")
    H.check(has(link_log(r), "pad: event: detach"), "pad buspower off: the pad goes: %s" % link_log(r))


def test_pad_resident():
    """'pad resident': offered at register time (at once with the driver up)."""
    r = H.Run("pad_resident").run(START_GAME + "pad resident ds4\nwait 1\nquit\n")
    lines = link_log(r)
    H.check(has(lines, "pad: event: attach") and has(lines, "pad: diag (event)", "matches=1 attaches=1"),
            "pad resident: attached: %s" % lines)


def test_pad_direct():
    """'pad direct' / 'pad readdesc' / 'pad reg': the preplugged device reads
    through a descriptor read, never offered; the registers follow the bus
    power (FOTG210 A_BUS_DROP/REQ, ChipIdea PP)."""
    r = H.Run("pad_direct").run(START_GAME + "pad preplug dualsense\npad readdesc\npad direct\npad readdesc\n"
                                "pad reg b0000030\npad reg b0000080 20\npad reg b0000030\npad reg b0000080 10\n"
                                "pad reg b0000030\nwait 1\nquit\n")
    t = [text for _, text in pad_log(r)]
    H.check("readdesc: none" in t and "readdesc: ok 054c:0ce6" in t, "pad direct: read only after 'direct': %s" % t)
    regs = [text for text in t if text.startswith("reg ")]
    H.check(regs[0].endswith("= 00000005") and regs[1].endswith("= 00000020") and regs[2].endswith("= 00000000")
            and regs[3].endswith("= 00080010") and regs[4].endswith("= 00000005"),
            "pad reg: PORTSC follows A_BUS_DROP/REQ: %s" % regs)
    r = H.Run("pad_pp").run(START_GAME + "pad port 3\npad preplug xinput\npad reg b0000184\npad reg b0000184 0\n"
                            "pad reg b0000184\npad reg b0000184 1000\npad reg b0000184\nwait 1\nquit\n")
    regs = [text for text in (x for _, x in pad_log(r)) if text.startswith("reg ")]
    v = [x[-8:] for x in regs]
    H.check(v[0] == "00001005" and v[2] == "00000000" and v[4] == "00001005",
            "pad reg: ChipIdea PP takes the device off the port and back: %s" % regs)


def reenum_lines(r):
    return [l for l in link_log(r) if "reenum: cycle" in l]


def test_pad_reenum():
    """S4: a pad plugged before gpSP starts is offered after the arbiter's
    bus power cycle, with no replug hint; 'cycle ignored': two cycles and
    on; 'cycle late 2': the second works; a pad plugged during play and a
    link (the calculator's cable): no cycle."""
    r = H.Run("reenum_ok").run(START_GAME + "pad preplug xinput\npad cycle ok\nwait 8\nquit\n")
    lines, cyc = link_log(r), reenum_lines(r)
    ev = [t for t, text in toasts(r) if text == "Controller connected (Xbox 360)"]
    on = powered(r, "on")
    H.check(len(cyc) == 1 and "cycle 1 (fotg)" in cyc[0] and "vbus_valid=1" in cyc[0], "reenum ok: one cycle 1: %s" % cyc)
    H.check(len(ev) == 1 and has(lines, "pad: event: attach"),
            "reenum ok: attached after the cycle, one toast: %s %s" % (ev, on))
    H.check(ev and ev[0] < 6.5, "reenum ok: attached about 3.6 s after the plug-in (the watch looks twice first: SEAM-2) (t=%s)" % ev)
    H.check(not [t for _, t in toasts(r) if "Plug the controller" in t] and not has(lines, "replug hint"),
            "reenum ok: no replug hint: %s" % toasts(r))
    r = H.Run("reenum_ign").run(START_GAME + "pad preplug xinput\npad cycle ignored\nwait 14\nquit\n")
    cyc = reenum_lines(r)
    H.check(len(cyc) == 2 and "cycle 1" in cyc[0] and "cycle 2" in cyc[1] and not has(link_log(r), "pad: event: attach"),
            "reenum ignored: exactly 2 cycles, no pad: %s" % cyc)
    H.check(has(link_log(r), "seamless: stage 4"), "reenum ignored: the stages go on: %s" % link_log(r))
    r = H.Run("reenum_late").run(START_GAME + "pad preplug xinput\npad cycle late 2\nwait 12\nquit\n")
    cyc = reenum_lines(r)
    H.check(len(cyc) == 2 and has(link_log(r), "pad: event: attach"), "reenum late 2: attached after the 2nd cycle: %s" % cyc)
    r = H.Run("reenum_ci").run(START_GAME + "pad port 3\npad preplug xinput\nwait 8\nquit\n")
    cyc = reenum_lines(r)
    H.check(len(cyc) == 1 and "(ci)" in cyc[0] and has(link_log(r), "pad: event: attach"), "reenum ChipIdea: PP cycle: %s" % cyc)
    r = H.Run("reenum_hot").run(START_GAME + "pad in xinput\nwait 6\nquit\n")
    H.check(not reenum_lines(r) and has(link_log(r), "pad: event: attach"), "reenum: hot plug, no cycle: %s" % reenum_lines(r))
    r = H.Run("reenum_dev").run(START_GAME + "pad port 2\nwait 8\nquit\n")
    H.check(not reenum_lines(r), "reenum: the device's end, no cycle")


def osd(run):
    return [(t, text[4:]) for t, text in pad_log(run) if text.startswith("osd ")]


# DETECT: the come-up's messages (nspire_link.c BRING_*).
COMEUP_DEVICE = "USB device in: connecting..."
COMEUP_NODE = "Calculator found: connecting..."
COMEUP_CABLE = "Link cable in: connecting..."
START_S = 2.25   # the game's first frame after START_GAME (seamless_tests.py START)


def test_toasts_seamless():
    """S7: the seamless toasts and only they, in order: nothing for an empty
    port or when the OS offers in time; 'Starting the USB device...' at the
    port's age 6 s with stage >= 2, then the last resort once; a port with
    nothing to power-cycle says to swap the cable; a node our service
    doesn't answer for 10 s; each once per port change, each <= 63 chars."""
    ok = lambda n, scr: [x for _, x in osd(H.Run(n).run(START_GAME + scr))]
    H.check(ok("s7_empty", "wait 20\nquit\n") == [], "s7: empty port, no toast")
    H.check(ok("s7_hot", "pad in xinput\nwait 12\nquit\n") == ["Controller connected (Xbox 360)"], "s7: hot plug, only the pad's")
    # DETECT: a device nobody claims for 1 s gets the come-up's message at once
    # after (BRING_HOST_QUIET_MS), kept up until the pad or the arbiter speaks
    r = H.Run("s7_ok").run(START_GAME + "pad preplug xinput\npad cycle ok\nwait 12\nquit\n")
    o = osd(r)
    H.check([x for _, x in o] == [COMEUP_DEVICE, "Controller connected (Xbox 360)"],
            "s7: offered after the cycle: the come-up's message, then only the pad's: %s" % o)
    H.check(o and o[0][0] - START_S <= 1.3, "s7: the come-up's message within 1.3 s of the plug: %s" % o)
    r = H.Run("s7_ign").run(START_GAME + "pad preplug xinput\npad cycle ignored\nwait 20\nquit\n")
    o = osd(r)
    # PAD3 R1 had the Controller page's toast follow the arbiter's last resort;
    # R11w part 2: one message that asks something per plug (the last resort's,
    # in plainer words): no page toast after it
    H.check([x for _, x in o] == [COMEUP_DEVICE, "Starting the USB device...", "Unplug the USB device and plug it in again"],
            "s7: ignored cycles: the come-up's, Starting, the last resort, once each: %s" % o)
    H.check(len(o) > 1 and 5.5 <= o[1][0] - 2.0 and o[1][0] <= 9.0, "s7: Starting about 6 s after the port change: %s" % o)
    H.check(not [1 for _, x in o if "Plug the controller" in x or "replug the controller" in x], "s7: no old replug hint")
    r = H.Run("s7_late").run(START_GAME + "pad preplug xinput\npad cycle late 2\nwait 14\nquit\n")
    H.check([x for _, x in osd(r)] == [COMEUP_DEVICE, "Starting the USB device...", "Controller connected (Xbox 360)"],
            "s7: second cycle works: %s" % osd(r))
    r = H.Run("s7_swap").run(START_GAME + "pad port 1\nwait 9\npad port 0\nwait 1\npad port 1\nwait 9\nquit\n")
    # R11w part 2: this checked "Swap the cable: adapter to the other
    # calculator" (each port change) and the page's toast: an empty adapter
    # (nothing to cycle) is what a controller pulled out leaves, and the swap
    # hint came with controllers (the reference report). Now the arbiter says
    # nothing there; the page's toast for an adapter that never had a device
    # (8 s), once per port change.
    H.check([x for _, x in osd(r)] == [H_TOAST, H_TOAST],
            "s7: nothing to cycle: no swap hint, the page's toast once per port change: %s" % osd(r))
    r = H.Run("s7_node").run(START_GAME + "pad port 1\npad calc node\nwait 14\nquit\n")
    o = osd(r)
    H.check([x for _, x in o] == [COMEUP_NODE, "Other calculator found: start gpSP there"],
            "s7: node without service: the come-up's message, then the hint: %s" % o)
    H.check(len(o) > 1 and 10.5 <= o[1][0] <= 13.5, "s7: after 10 s on the cable: %s" % o)
    H.check(all(len(x) <= 63 for _, x in osd(r) + osd(H.Run("s7_ign"))), "s7: texts fit")


def test_swap():
    """Pad, then calculator, then pad again, in one run with no setting
    touched and no menu: the link starts when the calculator's cable is the
    port's, ends when it's pulled, and the pad connects again; each toast
    once; the link never starts with the pad up (no 'controller in it'
    refusal) and the pad is never offered while the link has the cable."""
    me, other = H.Run("swap_a"), H.Run("swap_b")
    sock = os.path.join(H.WORK, "swap.sock")
    a = ("pad calc out\n" + START_GAME + "pad in xinput\nwait 3\npad out\nwait 2\npad calc in\nwait 10\n"
         "pad calc out\nwait 3\npad in xinput\nwait 4\nquit\n")
    b = START_GAME + "wait 40\nquit\n"
    procs = [x.start(sc, "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-auto"])
             for p, (x, sc) in enumerate(zip((me, other), (a, b)), 1)]
    for x, proc in zip((me, other), procs):
        x.finish(proc)
    shown = [text[4:] for _, text in pad_log(me) if text.startswith("osd ")]
    lines = link_log(me)
    n = lambda w: len([s for s in shown if w in s])
    H.check(n("Controller connected") == 2, "swap: the pad's toast twice (once a time): %s" % shown)
    H.check(n("Link cable") == 1, "swap: the Linked toast once: %s" % shown)
    H.check(n("The USB port has a controller") == 0, "swap: no refusal: %s" % shown)
    H.check(len([l for l in lines if "Link on its own" in l]) == 1, "swap: linked on its own once: %s" % lines)
    H.check(has(lines, "The link cable came out"), "swap: the link ended with the cable: %s" % lines)
    H.check(n("Controller disconnected") >= 1, "swap: the first pad left: %s" % shown)
    # From pad.txt (the arbiter's host-only note): the link log keeps only its
    # last 3 KB before the link (DETECT log_begin), which with the driver's
    # offer lines (match-fix) no longer reaches back to the first pad.
    holders = [m.group(1) for _, text in pad_log(me) for m in [re.match(r"arbiter: port owner (\w+) \(was", text)] if m]
    H.check([o for o in holders if o != "WAIT"] == ["PAD", "NONE", "LINK", "NONE", "PAD"],
            "swap: the arbiter's holders PAD, NONE, LINK, NONE, PAD: %s" % holders)
    H.check(not has(lines, "seamless: stage"), "swap: no stage ran (the OS offered at once): %s" % holders)
    order = [t for t, text in pad_log(me) if "Controller connected" in text or "Link cable" in text]
    H.check(len(order) == 3, "swap: pad, link, pad in that order: %s" % order)


def stat_lines(run):
    """The 'pad stat' lines: dicts of the counters."""
    out = []
    for _, text in pad_log(run):
        if text.startswith("stat: "):
            out.append({k: float(v) for k, v in re.findall(r"(\w+)=([\d.]+)", text)})
    return out


def budget(name, port_line):
    """S8: 3000 frames (50 s) in one port state: the counters' difference
    between two 'pad stat' lines."""
    r = H.Run(name).run(START_GAME + port_line + "wait 1\npad stat\nwait 50\npad stat\nquit\n")
    st = stat_lines(r)
    H.check(len(st) == 2, "%s: two stat lines: %s" % (name, st))
    if len(st) != 2:
        return {}
    d = {k: st[1][k] - st[0][k] for k in st[0]}
    print("  S8 %s: %s" % (name, " ".join("%s=%g" % kv for kv in sorted(d.items()))))
    return d


def test_s8_budgets():
    """S8: the per-frame cost of each port state over 3000 frames (50 s of
    1/60 s, counted by 'pad stat'): nothing on the port (a CX II): no OS
    sleep at all, the observer's looks 1 per 8 frames (the other register
    reads there are nspire_link.c's diag timeline and nspire_usbpad_glue.c's,
    each a few MMIO words, at most 3 plugged and 2.5 role reads a frame:
    counted so they don't grow); something on the port: the OS's tick each
    frame for the link's watch (about 1.5 a frame with the driver's); a
    gated CX: one OS tick per 250 ms (a look per 6 s is link_usb's test)."""
    for name, line in (("s8_empty", ""), ("s8_host", "pad port 1\n"), ("s8_device", "pad port 2\n"),
                       ("s8_gated", "pad port -1\n"), ("s8_ci", "pad port 3\n"), ("s8_pad", "pad in xinput\n")):
        d = budget(name, line)
        if not d:
            continue
        H.check(d["t"] >= 49.5 and 3000 / 8 - 3 <= d["samples"] <= 3000 / 8 + 1,
                "%s: the observer looks once in 8 frames: %g in %g s" % (name, d["samples"], d["t"]))
        H.check(d["plugged_reads"] <= 3 * 3000 and d["bits_reads"] <= 2.5 * 3000,
                "%s: register reads: %g plugged, %g role" % (name, d["plugged_reads"], d["bits_reads"]))
        if name == "s8_empty":
            H.check(d["sleeps"] == 0, "%s: no OS sleep: %g" % (name, d["sleeps"]))
        elif name == "s8_gated":
            H.check(150 <= d["sleeps"] <= 50 * 4 + 4, "%s: one OS tick per 250 ms: %g in 50 s" % (name, d["sleeps"]))
        elif name != "s8_pad":
            H.check(2900 <= d["sleeps"] <= 5000, "%s: the watch's tick a frame: %g" % (name, d["sleeps"]))


def test_s8_latency():
    """S8: a plug-in during play is acted on within the targets, 3 s for a
    pad ('Controller connected') and 5 s for a link ('Linked'), with the S2
    simulators; the port is seen within one look (8 frames, 133 ms)."""
    r = H.Run("s8_lat_pad").run(START_GAME + "wait 3\npad in xinput\nwait 4\nquit\n")
    t_in = [t for t, text in pad_log(r) if text.startswith("pad in ")]
    t_on = [t for t, text in pad_log(r) if text == "osd Controller connected (Xbox 360)"]
    H.check(len(t_in) == 1 and len(t_on) == 1 and t_on[0] - t_in[0] <= 4.5,
            "s8 latency: a pad plugged in during play: connected %s s after" % ([b - a for a in t_in for b in t_on]))
    print("  S8 latency pad in play: %s s" % ([round(b - a, 3) for a in t_in for b in t_on]))
    r = H.Run("s8_lat_gated").run(START_GAME + "pad port -1\nwait 3\npad port 1\nwait 1\npad in xinput\nwait 5\nquit\n")
    t_in = [t for t, text in pad_log(r) if text.startswith("pad port 1")]
    t_on = [t for t, text in pad_log(r) if text == "osd Controller connected (Xbox 360)"]
    H.check(len(t_in) == 1 and len(t_on) == 1 and t_on[0] - t_in[0] <= 3.0 + 1.0,
            "s8 latency: a pad plugged in on a gated CX: %s" % ([b - a for a in t_in for b in t_on]))
    print("  S8 latency pad on gated CX (port, then the pad 1 s later): %s s" % ([round(b - a, 3) for a in t_in for b in t_on]))
    # a pad the OS doesn't offer (plugged in before the driver registered, or
    # at once): the arbiter's bus power cycle brings it in
    r = H.Run("s8_lat_cycle").run(START_GAME + "wait 3\npad cycle ok\npad preplug xinput\nwait 8\nquit\n")
    t_in = [t for t, text in pad_log(r) if text.startswith("pad preplug")]
    t_on = [t for t, text in pad_log(r) if text == "osd Controller connected (Xbox 360)"]
    H.check(len(t_in) == 1 and len(t_on) == 1 and t_on[0] - t_in[0] <= 4.5,
            "s8 latency: a pad the OS doesn't offer, brought in by the power cycle: %s" % ([b - a for a in t_in for b in t_on]))
    print("  S8 latency pad via the power cycle: %s s" % ([round(b - a, 3) for a in t_in for b in t_on]))
    # the link's: test_swap's two processes; the cable goes in during play
    for end in ("host", "device"):
        link_latency(end)


# DETECT (2026-11-11): the cable plugged in during play, with the simulator's
# OS times corrected to the r5 hardware logs (host_link.c CALC_NODE_PLUG_S:
# the other calculator's node listed 5 s after the plug; a NavNet service's
# callback starts at the first data, which the knock sends). What is ours:
# the first message on screen (at once at the cable's end, after 1 s with
# nobody claiming the port at the adapter's end: BRING_HOST_QUIET_MS), the
# node seen within a look (250 ms in the hurry), and from the node to
# "Linked" (connect, knock, hellos). The OS's 5 s is the floor.
NODE_S = 5.0
# R11w part 2 (2026-11-16): the device end's limit was 0.3 s ("Link cable in:
# connecting..." at once at the plain end). The plain end is also a
# computer's cable, and nothing is said about the port there until the other
# calculator's node is listed: the first message comes with the node now
# (the OS's 5 s and a look), the same limit as the node's.
FIRST_MSG_S = {"device": NODE_S + 0.3, "host": 1.3}
NODE_TO_LINKED_S = 1.0


def link_latency(end):
    sock = os.path.join(H.WORK, "s8lat_%s.sock" % end)
    me, other = H.Run("s8_lat_%s_a" % end), H.Run("s8_lat_%s_b" % end)
    prefix = os.path.join(H.WORK, "s8lat_%s.sy" % end)
    for q in (1, 2):
        for k in ("", "b"):
            try:
                os.remove("%s%s.%d" % (prefix, k, q))
            except OSError:
                pass
    # Two meetings: at the plug, and just before the node is listed (the two
    # processes drift apart in wall time, and a message's time is its
    # sender's: without the second, how far they drifted in 5 s is measured).
    a = ("pad calc out\npad calc end %s\n" % end + START_GAME + "wait 3\npad sync %s 1\npad calc in\nwait 4.8\n"
         "pad sync %sb 1\nwait 6\nquit\n" % (prefix, prefix))
    b = START_GAME + "wait 3\npad sync %s 2\nwait 4.8\npad sync %sb 2\nwait 8\nquit\n" % (prefix, prefix)
    procs = [x.start(sc, "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-auto"])
             for p, (x, sc) in enumerate(zip((me, other), (a, b)), 1)]
    for x, proc in zip((me, other), procs):
        x.finish(proc)
    log = pad_log(me)
    t_in = [t for t, text in log if text.startswith("pad calc in")]
    t_on = [t for t, text in log if text.startswith("osd Linked:")]
    t_msg = [t for t, text in log if text.startswith("osd ") and t_in and t >= t_in[0]]
    lines = link_log(me)
    ends = [m for l in lines for m in [re.search(r"come-up: linked (\d+) ms after the USB port's change; node listed (-?\d+)", l)] if m]
    linked = [int(m.group(1)) / 1000.0 for m in ends]
    node = [int(m.group(2)) / 1000.0 for m in ends if int(m.group(2)) >= 0]
    over = [l for l in lines if "come-up: over" in l or "come-up: not linked" in l or "come-up: the link ended" in l]
    name = "s8 latency (%s end)" % end
    H.check(len(t_in) == 1 and len(t_on) == 1, "%s: one plug, one link: %s %s" % (name, t_in, t_on))
    if not (t_in and t_on and t_msg and node and linked):
        H.check(False, "%s: the steps' times in the logs: msg %s node %s linked %s" % (name, t_msg, node, linked))
        return
    first = t_msg[0] - t_in[0]
    H.check(first <= FIRST_MSG_S[end], "%s: the first message %.2f s after the plug (limit %.1f): %s" % (
        name, first, FIRST_MSG_S[end], [text for t, text in log if text.startswith("osd ")][:3]))
    H.check(node[0] <= NODE_S + 0.3, "%s: the node seen %.2f s after the plug (the OS's %.1f s and a look)" % (name, node[0], NODE_S))
    H.check(linked[0] - node[0] <= NODE_TO_LINKED_S, "%s: node to linked %.2f s (limit %.1f)" % (
        name, linked[0] - node[0], NODE_TO_LINKED_S))
    H.check(t_on[0] - t_in[0] <= NODE_S + 0.3 + NODE_TO_LINKED_S, "%s: linked %.2f s after the plug" % (name, t_on[0] - t_in[0]))
    H.check(not over, "%s: a message kept up from the first to Linked (no come-up ended early): %s" % (name, over))
    print("  S8 latency link in play (%s end): first message %.2f s, node %.2f s, linked %.2f s after the plug" % (
        end, first, node[0], t_on[0] - t_in[0]))



# ---- PAD3: the Controller status page and the pad log ----

CONTROLLER_PAGE = N.settings_tile("controller") + "press enter\nwait 0.6\n"
# r19: the Controller list: D opens Details; there right gives the OS 30 s
CONTROLLER_DETAILS = "press d\nwait 0.6\n"


def page_lines(run):
    """The Controller page's states as pad.txt has them: (time, verdict, {label: value}, note)."""
    out = []
    for t, text in pad_log(run):
        if not text.startswith("page: "):
            continue
        parts = [p.strip() for p in text[6:].split("|")]
        fields = dict(p.split(": ", 1) for p in parts[1:] if ": " in p)
        out.append((t, parts[0], fields, fields.get("note", "")))
    return out


def pad_file(run, name="gpsp_pad.txt.tns"):
    path = run.save(name)
    return open(path, encoding="utf-8").read() if os.path.exists(path) else ""


def test_pad3_page():
    """The Controller status page (More settings > Controller status), live:
    what it says for nothing plugged in, a CX's gated controller, the cable's
    plain end, the adapter alone, a device the OS never starts its host
    controller for (R9F's CX II: PORTSC 00000800, USBCMD stopped), and that
    device once enter restarted the bus power (the OS then offers it, the
    driver takes it). The pad log has each step, with the raw words. (PAD4:
    the come-up restarts the power itself at once for this device: here its
    restart does nothing, so the page still shows the device stopped, then
    the one by hand works.)"""
    r = H.Run("pad3_page").run(START_GAME + "press menu\nwait 0.3\n" + CONTROLLER_PAGE +
                               "pad port 0\nwait 0.6\npad port -1\nwait 0.6\npad port 2\nwait 0.6\npad port 1\nwait 0.6\n"
                               "pad cycle ignored\npad hcstop xinput\nwait 4\npad cycle ok\npress enter\nwait 3\npress esc\nwait 0.5\n"
                               "press esc\nwait 0.5\npress esc\nwait 4\nquit\n")
    pages = page_lines(r)
    seen = [v for _, v, _, _ in pages]
    want = ["nothing", "unreadable", "device_end", "empty_adapter", "host_stopped", "working"]
    order = [v for v in seen if v in want]
    H.check([v for i, v in enumerate(order) if i == 0 or order[i - 1] != v] == want,
            "pad3 page: the states in order %s: %s" % (want, seen))
    by = {v: f for _, v, f, _ in pages}
    f = by.get("device_end", {})
    H.check(f.get("Port") == "device end: use adapter end", "pad3 page: the cable's plain end says to use the adapter end: %s" % f)
    f = by.get("empty_adapter", {})
    H.check(f.get("Port") == "host: the adapter's end" and f.get("Bus power") == "on" and f.get("Device") == "nothing on the data wires",
            "pad3 page: the adapter alone: %s" % f)
    f = by.get("host_stopped", {})
    H.check(f.get("Device") == "on wires, not connected" and f.get("OS USB") == "host controller stopped" and
            f.get("OS reported") == "none reported by the OS" and f.get("Driver") == "waiting for OS offer",
            "pad3 page: the host controller stopped: %s" % f)
    f = by.get("working", {})
    H.check(f.get("Driver") == "took it: Xbox 360 (slot 1)" and f.get("OS reported") == "045e:028e, interface ff/5d/01" and
            f.get("OS USB") == "host controller running" and f.get("Port enabled") == "yes",
            "pad3 page: working after the restart: %s" % f)
    notes = [n for _, _, _, n in pages]
    H.check("power restarted" in notes, "pad3 page: enter restarted the bus power: %s" % sorted(set(notes)))
    text = pad_file(r)
    H.check("user: opened the Controller status page" in text and "user: power restart 9 -> 1 (power back)" in text and
            "port: host end (adapter), power on, a device on the wires (not connected), not enabled, host controller STOPPED | "
            "OTGCSR 00080000" in text and "status: host_stopped" in text and "OS offer #1: 045e:028e interface ff/5d/01 -> taken (5)" in text
            and "driver: attach (connected), slot 0, 045e:028e" in text and "try ended: a controller attached" in text,
            "pad3 page: the pad log has the steps:\n%s" % text)
    H.check(text.find("arbiter: power restart 1 -> 1 (power back)") >= 0 and
            text.find("arbiter: power restart 1 -> 1 (power back)") < text.find("user: power restart 9 -> 1"),
            "pad3 page: the come-up's own restart came first (PAD4):\n%s" % text[-3000:])
    H.check("Controller connected (Xbox 360)" in [t for _, t in toasts(r)], "pad3 page: the pad connected: %s" % toasts(r))


def test_pad3_hcstop_game():
    """In the game, a device the OS doesn't start: the arbiter's power cycle
    (not held off: nothing offered it before) brings it, as before PAD3; the
    pad log is written when the try ends (the pad attached). With the cycle
    ignored: the arbiter's own last resort, no PAD3 toast over it, and the pad
    log written on its timer with the device's words."""
    r = H.Run("pad3_hcstop_ok").run(START_GAME + "pad hcstop xinput\npad cycle ok\nwait 8\nquit\n")
    H.check("Controller connected (Xbox 360)" in [t for _, t in toasts(r)], "pad3 hcstop: the cycle brings the pad: %s" % toasts(r))
    text = pad_file(r)
    writes = re.search(r"\(write (\d+)\)", text)
    H.check("arbiter: power restart 1 -> 1 (power back)" in text and "try ended: a controller attached" in text and
            writes and int(writes.group(1)) >= 2, "pad3 hcstop: the pad log says how it came, written before exit too:\n%s" % text)
    r = H.Run("pad3_hcstop_ignored").run(START_GAME + "pad hcstop xinput\npad cycle ignored\nwait 25\nquit\n")
    shown = [t[4:] for _, t in pad_log(r) if t.startswith("osd ")]
    # R1 had ours follow the arbiter's last resort. R11w part 2: one
    # message that asks something per plug: the last resort (new words) is
    # it, ours isn't said after it (the page names the cause when opened)
    H.check(shown.count("Unplug the USB device and plug it in again") == 1 and H_TOAST not in shown,
            "pad3 hcstop ignored: the arbiter's last resort once, not ours after it: %s" % shown)
    text = pad_file(r)
    H.check("on the timer" in text or "the arbiter gave up" in text, "pad3 hcstop ignored: the pad log written:\n%s" % text[:400])
    H.check("status: host_stopped" in text and "arbiter: stage 4 (gave up)" in text, "pad3 hcstop ignored: the story:\n%s" % text)


H_TOAST = "USB problem: Settings > Controller"   # R11w part 2 (UI4 Settings tree)


def test_pad3_pads_in_turn():
    """R1: the user's flow, four pads in turn into one adapter that
    stays in (it never reads EMPTY, so the session never ends). Each pad the
    OS never starts its host controller for is a new try: its own two power
    cycles and its own messages. Before, the first pad used up both cycles
    and the last resort's toast, and pads 2-4 got no cycle and no message."""
    sc = START_GAME + "pad port 1\nwait 1\npad cycle ignored\n"
    for kind in ("switchpro", "xinput", "ds4", "hid"):
        sc += "pad hcstop %s\nwait 22\npad unplug\nwait 3\n" % kind
    r = H.Run("pad3_in_turn").run(sc + "quit\n")
    events = [(t, x) for t, x in pad_log(r) if x.startswith("reenum cycle") or x.startswith("osd ") or x.startswith("pad hcstop")]
    tries, cur = [], None
    for _, x in events:
        if x.startswith("pad hcstop"):
            cur = {"cycles": 0, "shown": []}
            tries.append(cur)
        elif cur is not None and x.startswith("reenum cycle"):
            cur["cycles"] += 1
        elif cur is not None:
            cur["shown"].append(x[4:])
    H.check(len(tries) == 4 and all(t["cycles"] == 2 for t in tries),
            "pads in turn: two power cycles for each pad: %s" % [t["cycles"] for t in tries])
    # (R11w part 2: was the last resort and the page's toast for each pad: one
    # message that asks something per plug now, the last resort's, new words;
    # each new pad is a new plug, so each gets it)
    H.check(len(tries) == 4 and all(t["shown"].count("Unplug the USB device and plug it in again") == 1 and
                                    H_TOAST not in t["shown"] for t in tries),
            "pads in turn: each pad gets the last resort once, no second message: %s" % [t["shown"] for t in tries])


def test_pad3_after_link():
    """R9F's CX II: the other calculator on the adapter's end, linked, its
    cable pulled out of the adapter that stays in, then a controller in the
    adapter whose host controller the OS doesn't start. Before PAD3 the OS's
    offer of the calculator (SEAM-8's mark, reset only by a port reading 0,
    which a CX II's never does) and the link's claim held off every power
    cycle and every message: the controller stayed dead. Now the cycle comes
    and the pad connects. Before the link, eight pad plug-ins fill the link
    log, whose trim before the link drops them; the pad log keeps them."""
    me, other = H.Run("pad3_link_a"), H.Run("pad3_link_b")
    sock = os.path.join(H.WORK, "pad3_link.sock")
    a = ("pad calc out\n" + START_GAME + "pad in xinput\nwait 1.5\npad out\nwait 1\n" * 8 +
         "pad port 1\npad calc in\nwait 10\npad calc out\nwait 3\npad hcstop xinput\npad cycle ok\nwait 10\nquit\n")
    b = START_GAME + "wait 50\nquit\n"
    procs = [x.start(sc, "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-auto"])
             for p, (x, sc) in enumerate(zip((me, other), (a, b)), 1)]
    for x, proc in zip((me, other), procs):
        x.finish(proc)
    shown = [t[4:] for _, t in pad_log(me) if t.startswith("osd ")]
    linked = [i for i, t in enumerate(shown) if t.startswith("Linked")]
    after = shown[linked[0] + 1:] if linked else []
    H.check(linked and "Controller connected (Xbox 360)" in after, "pad3 after link: linked, then the controller connects: %s" % shown)
    cyc = [t for _, t in pad_log(me) if t.startswith("reenum")]
    H.check(len(cyc) >= 1, "pad3 after link: a bus power cycle for the controller: %s" % cyc)
    H.check(not [t for _, t in pad_log(me) if "bus power dropped under the other calculator" in t],
            "pad3 after link: never a cycle under the calculator")
    text = pad_file(me)
    # R11w: the events' list only (the milestones section after it, R11w's,
    # repeats each driver event: counted there too, this counted the file)
    events = text.split("\nMilestones (")[0]
    attaches = events.count("driver: attach (connected)")
    H.check("driver: attach (connected)" in text.split("\nMilestones (")[-1], "pad3 after link: the milestones have the attaches")
    H.check(attaches == 9 and "refused: a TI calculator" in text and "link: linked" in text,
            "pad3 after link: the pad log has all 9 attaches (%d), the calculator's offer and the link:\n%s" % (attaches, text[-3000:]))
    lines = link_log(me)
    H.check(has(lines, "lines dropped: the log keeps the lines just before a link") and len([l for l in lines if "attach (connected)" in l]) < 9,
            "pad3 after link: the link log was trimmed before the link (%d attaches in it)" %
            len([l for l in lines if "attach (connected)" in l]))


def test_pad3_idle_churn():
    """R2: after a try, the adapter pulled out and the OS's idle
    off-and-on (nothing <-> unreadable; a CX CAS every ~0.1 s) for minutes:
    the glue's verdict lines broke the idle line's merge, so each flip took
    two of the ring's 256 entries and the try was churned out of the file
    the exit wrote. Now the flips are one counted line and the try stays."""
    sc = START_GAME + "pad hcstop xinput\npad cycle ignored\nwait 15\npad unplug\nwait 1\n"
    sc += "pad port -1\nwait 0.3\npad port 0\nwait 0.3\n" * 300
    r = H.Run("pad3_idle_churn").run(sc + "quit\n")
    text = pad_file(r)
    kept = re.search(r"(\d+) kept, (\d+) older dropped", text)
    H.check("status: host_stopped" in text and "arbiter: stage 4 (gave up)" in text and "at exit" in text,
            "idle churn: the try is still in the exit's file:\n%s" % text[-1500:])
    H.check(kept and int(kept.group(2)) == 0 and int(kept.group(1)) < 100,
            "idle churn: 600 flips don't fill the ring: %s" % (str(kept.groups()) if kept else text[:600]))


def test_pad3_os_time():
    """R4: the page's right arrow gives the OS up to 30 s (until its
    host controller runs): 5 s was under the +1280..+1362 OS sleeps after
    which R9F's and r5's CX II started it. The pad log has the sleeps and
    the time. (ui18: in Details, where its band says it: on the page the
    arrows move among the tiles.)"""
    r = H.Run("pad3_os_time").run(START_GAME + "press menu\nwait 0.3\n" + CONTROLLER_PAGE + CONTROLLER_DETAILS +
                                  "pad hcstop xinput\npad cycle ignored\nwait 1\npress right\nwait 32\npress esc\nwait 0.5\n"
                                  "press esc\nwait 0.5\npress esc\nwait 0.5\npress esc\nwait 1\nquit\n")
    text = pad_file(r)
    m = re.search(r"user: gave the OS (\d+) ms \(until its host controller runs, 30 s at most\): (\d+) OS sleeps, the host controller never ran", text)
    H.check(m and 29500 <= int(m.group(1)) <= 31000 and int(m.group(2)) > 0, "pad3 os time: 30 s, the sleeps counted:\n%s" % text[-1500:])
    notes = [n for _, _, _, n in page_lines(r)]
    H.check("after 30 s: the host controller never ran" in notes, "pad3 os time: the page's note: %s" % sorted(set(notes)))


def test_pad3_os_time_no_timer():
    """The manual OS wait stays bounded and Escape still cancels it when
    the CX's timer only advances in frame waits, outside this action."""
    start = START_GAME + "press menu\nwait 0.3\n" + CONTROLLER_PAGE + CONTROLLER_DETAILS + \
        "pad hcstop xinput\npad cycle ignored\nwait 1\npress right\n"
    for name, wait, expected in (("bounded", 32, "after 30 s:"), ("cancel", 2, "after 2 s:")):
        r = H.Run("pad3_os_time_no_timer_" + name).run(
            start + "wait %s\npress esc\nwait 0.5\npress esc\nwait 1\nquit\n" % wait,
            options=["--model", "cx", "--timer-dead", "1"])
        notes = [n for _, _, _, n in page_lines(r)]
        H.check(any(n.startswith(expected) for n in notes),
                "pad3 OS wait without timer, %s: returned to the page: %s" % (name, sorted(set(notes))))
        check_shutdown(r, "pad3 OS wait without timer " + name, timer=False)


# ---- PAD4: the come-up (the OS's time in one go at the plug, the steps' times) ----

COMEUP_TEXT = "USB device in: connecting..."
HEAVY = "cost frame 17\n"   # a game with no spare time (R10a's Emerald: the OS got a forced tick every ~33 ms)
CU_BURST_MS, CU_BURST_MAX_MS = 3000, 5000


def comeup_lines(text):
    """The pad log's come-up lines: (ms since gpSP started, text after 'come-up: ')."""
    out = []
    for line in text.split("\n"):
        m = re.match(r"\s*(\d+) come-up: (.*)$", line)
        if m:
            out.append((int(m.group(1)), m.group(2)))
    return out


def step_ms(text, words):
    """The come-up step lines' ms after the plug for a step's words ('the driver attached it')."""
    return [int(m.group(1)) for _, x in comeup_lines(text) for m in [re.match(r"\+(\d+) ms: %s" % re.escape(words), x)] if m]


def bursts(text):
    """(ms, OS sleeps, why, host controller) for each 'the OS ran alone' line."""
    return [(int(m.group(1)), int(m.group(2)), m.group(3), m.group(4)) for _, x in comeup_lines(text)
            for m in [re.match(r"the OS ran alone (\d+) ms \((\d+) OS sleeps, [^)]*\): (.*); the host controller (runs|is STOPPED)", x)] if m]


def plug_to(run, plug_word, toast="Controller connected (Xbox 360)"):
    """Seconds from the script's plug line to the toast (None if either is missing)."""
    t_in = [t for t, text in pad_log(run) if text.startswith(plug_word)]
    t_on = [t for t, text in pad_log(run) if text == "osd " + toast and t_in and t >= t_in[0]]
    return t_on[0] - t_in[0] if t_in and t_on else None


def test_pad4_running():
    """The common case (any pad after the first, R10a's Switch Pro): the OS's
    host controller already runs; its hub resets the port and reads the
    descriptors in ~26 of its ticks (R10a: ~13 OS sleeps from the reset to
    the port enabled). The come-up gives them in one go: the message, then
    the game stands still for ~0.3 s, the pad works: plug to 'Controller
    connected' < 1 s in a game with no spare time; no power restart; the
    pad log and the page say each step's time."""
    r = H.Run("pad4_running").run(HEAVY + START_GAME + "wait 1\npad slowhc ticks 0 26 xinput\nwait 3\n" +
                                  held_peek("Pad_A", "a") + "quit\n")
    dt = plug_to(r, "pad slowhc")
    H.check(dt is not None and dt <= 1.0, "pad4 running: plug to connected %s s (<= 1 s)" % dt)
    H.check(p1(r, "a") == "A", "pad4 running: the pad's A reaches the game: %s" % p1(r, "a"))
    H.check([x for _, x in osd(r)] == [COMEUP_TEXT, "Controller connected (Xbox 360)"],
            "pad4 running: the come-up's message, then the pad's: %s" % osd(r))
    H.check(not [1 for _, t in pad_log(r) if t.startswith("reenum cycle")], "pad4 running: no power restart")
    text = pad_file(r)
    b = bursts(text)
    H.check(len(b) == 1 and b[0][0] <= 600 and b[0][2] == "the controller works" and b[0][3] == "runs",
            "pad4 running: one burst of the OS's time, ended by the pad: %s" % b)
    for words in ("the device is connected", "the OS's host controller runs", "the port is enabled", "the OS offered it",
                  "the driver attached it", "the first report (buttons) came"):
        H.check(len(step_ms(text, words)) == 1, "pad4 running: the step '%s' in the pad log:\n%s" % (words, text[-2500:]))
    rep = step_ms(text, "the first report (buttons) came")
    H.check(rep and rep[0] <= 900, "pad4 running: the first report %s ms after the plug" % rep)
    H.check("come-up: ended after" in text and "the controller works" in text, "pad4 running: the come-up's end line")
    print("  PAD4 running host controller: plug to connected %.2f s, first report %s ms, burst %s" % (dt or -1, rep, b))


def test_pad4_os_time():
    """R10a's 8BitDo, as a model: the stopped host controller starts after
    1000 of the OS's ticks (R10a: ~1060 OS sleeps from the adapter), then 26
    more to the offer. The old glue gave ~60 ticks a second for 12 s of polls
    after a port change and then the driver's idle look (~5 a second): ~68 s.
    PAD4 gave a 3 s burst (~300 ticks, the game standing still), then a
    forced tick every ~33 ms (~52 ticks a second): ~17 s. R11t (the new
    rule; this test changed with it): the burst goes on while the device is
    on the wires and the controller stopped (CU_HC_WAIT_MS at most), so the
    OS has its 1000 ticks in one go, the controller starts inside the burst
    and the pad comes before it ends: ~10.6 s, one burst ended by the pad.
    In the menus (the file list) the same, the burst at once."""
    r = H.Run("pad4_os_time").run(HEAVY + START_GAME + "wait 1\npad slowhc ticks 1000 26 xinput\nwait 25\nquit\n")
    dt = plug_to(r, "pad slowhc")
    H.check(dt is not None and dt <= 12.0, "pad4 os time: plug to connected %s s (<= 12 s; PAD4's ~17 s, the old glue's ~68 s)" % dt)
    text = pad_file(r)
    b = bursts(text)
    H.check(len(b) == 1 and 9000 <= b[0][0] <= 12000 and b[0][2] == "the controller works" and b[0][3] == "runs" and
            950 <= b[0][1] <= 1150,
            "pad4 os time: one burst until the pad works, ~1000 OS sleeps, the controller running at its end: %s" % b)
    H.check("waiting for the host controller to start" in text, "pad4 os time: the burst says it waited for the controller")
    hc = step_ms(text, "the OS's host controller runs")
    H.check(hc and hc[0] >= 8500, "pad4 os time: the host controller's start in the log: %s ms" % hc)
    H.check(not [1 for _, t in pad_log(r) if t.startswith("reenum cycle")], "pad4 os time: no power restart (connected, stopped)")
    H.check([x for _, x in osd(r)] == [COMEUP_TEXT, "Controller connected (Xbox 360)"],
            "pad4 os time: the come-up's message kept while the OS works (no 'Unplug and replug', no page toast): %s" % osd(r))
    menu = H.Run("pad4_os_time_menu").run("wait 1\npad slowhc ticks 300 26 xinput\nwait 6\npress enter\nwait 2\nquit\n")
    mt = pad_file(menu)
    mb = bursts(mt)
    H.check(len(mb) == 1 and "menus waited" in mt and step_ms(mt, "the driver attached it") and
            step_ms(mt, "the driver attached it")[0] <= 3500,
            "pad4 os time in the menus: the burst there brings it (attached %s ms): %s" % (step_ms(mt, "the driver attached it"), mb))
    print("  PAD4 stopped host controller (1000 OS ticks): plug to connected %.2f s, burst %s, controller at %s ms" % (dt or -1, b, hc))


def test_pad4_wall():
    """If what the OS waits for is the clock (not its ticks), the burst can't
    hurry it. PAD4: the game stood still CU_BURST_MS at most, then played on
    with the OS's ticks. R11t (the new rule; this test changed with it): the
    burst goes on while the device is on the wires and the controller
    stopped, so the game stands still until the OS's own 6 s are over (in
    this model; CU_HC_WAIT_MS at most) and the pad comes inside the burst."""
    r = H.Run("pad4_wall").run(HEAVY + START_GAME + "wait 1\npad slowhc wall 6 0.3 xinput\nwait 10\nquit\n")
    dt = plug_to(r, "pad slowhc")
    H.check(dt is not None and 6.2 <= dt <= 7.2, "pad4 wall: connected when the OS's own 6 s are over: %s s" % dt)
    b = bursts(pad_file(r))
    H.check(len(b) == 1 and 5900 <= b[0][0] <= 7200 and b[0][3] == "runs" and b[0][2] == "the controller works",
            "pad4 wall: the game stood still until the OS's 6 s were over and the pad worked: %s" % b)
    H.check([x for _, x in osd(r)] == [COMEUP_TEXT, "Controller connected (Xbox 360)"], "pad4 wall: the messages: %s" % osd(r))


def test_pad4_noconn():
    """R10a 400981 (the 8BitDo's PORTSC 00000800: on the wires, never latched
    as connected) after an adapter put in empty (397138: 'nothing to
    restart', which latched and kept the 8BitDo from any restart): the
    come-up's restart at ~0.3 s brings it; one restart, in a game and with
    the adapter in first."""
    r = H.Run("pad4_noconn").run(HEAVY + START_GAME + "pad port 1\nwait 5\npad hcstop xinput\npad cycle ok\nwait 4\nquit\n")
    dt = plug_to(r, "pad hcstop")
    cyc = [t for _, t in pad_log(r) if t.startswith("reenum cycle")]
    H.check(dt is not None and dt <= 1.6, "pad4 noconn: the restart at once brings it: %s s (<= 1.6)" % dt)
    H.check(len(cyc) == 1, "pad4 noconn: one restart: %s" % cyc)
    text = pad_file(r)
    H.check("arbiter: power restart 1 -> 1 (power back)" in text, "pad4 noconn: the restart in the pad log:\n%s" % text[-2000:])


def test_pad4_no_chase():
    """A slow OS at work on a connected device (its controller running, the
    port in reset for 600 of its ticks: the Switch Pro's 9.8 s at R10a's
    sleeps): no power restart while it works (the stages' two at 3.3 s and
    8.1 s set it back before), the burst goes on to CU_BURST_MAX_MS while
    the OS works, the rest in the game's ticks."""
    r = H.Run("pad4_no_chase").run(HEAVY + START_GAME + "wait 1\npad slowhc ticks 0 600 switchpro\nwait 14\nquit\n")
    log = [t for _, t in pad_log(r)]
    H.check(not [t for t in log if t.startswith("reenum cycle") or "set the OS back" in t], "pad4 no chase: no restart: %s" %
            [t for t in log if "reenum" in t or "slow:" in t])
    dt = plug_to(r, "pad slowhc", "Controller connected (Switch Pro)")
    H.check(dt is not None and dt <= 8.0, "pad4 no chase: connected %s s after the plug (<= 8)" % dt)
    b = bursts(pad_file(r))
    H.check(len(b) == 1 and CU_BURST_MS + 400 <= b[0][0] <= CU_BURST_MAX_MS + 200, "pad4 no chase: the burst went on while the OS worked: %s" % b)


def test_pad4_in_turn():
    """Step 3: one pad after another into an adapter that stays in, each the
    same way with nothing of the one before: a pad the OS starts at once, a
    Switch Pro the driver gives up on ('not supported'), the next pad, a pad
    the port never latched (the restart), then the first kind again. Each
    its own come-up from the plug; the ones the OS offers attach < 1 s."""
    sc = HEAVY + START_GAME + "pad port 1\nwait 1\n"
    sc += "pad slowhc ticks 0 26 xinput\nwait 3\npad out\npad port 1\nwait 2\n"
    sc += "pad preplug switchpro\nwait 0.5\npad event unsupported switchpro\nwait 3\npad unplug\npad port 1\nwait 2\n"
    sc += "pad slowhc ticks 0 26 ds4\nwait 3\npad out\npad port 1\nwait 2\n"
    sc += "pad cycle ok\npad hcstop hid\nwait 3\npad out\npad port 1\nwait 2\n"
    sc += "pad slowhc ticks 0 26 xinput\nwait 3\nquit\n"
    r = H.Run("pad4_in_turn").run(sc)
    text = pad_file(r)
    begins = [x for _, x in comeup_lines(text) if x.startswith("a device came onto")]
    H.check(len(begins) == 5, "pad4 in turn: five come-ups: %s" % begins)
    att = step_ms(text, "the driver attached it")
    H.check(len(att) == 4 and all(a <= 900 for a in att[:2]) and att[2] <= 1600 and att[3] <= 900,
            "pad4 in turn: each pad attached as fast as the first, the unsupported one in between: %s ms" % att)
    ends = [x for _, x in comeup_lines(text) if x.startswith("ended after")]
    H.check(len(ends) == 5 and "the driver gave its answer" in ends[1], "pad4 in turn: how each ended: %s" % ends)
    cyc = [t for _, t in pad_log(r) if t.startswith("reenum cycle")]
    H.check(len(cyc) == 1, "pad4 in turn: one restart, for the pad the port never latched: %s" % cyc)


# ---- R11t: the burst waits for the OS's host controller (CU_HC_WAIT_MS at most) ----

CU_HC_WAIT_MS = 15000
HC_NEVER = "pad slowhc ticks 100000 26 xinput\n"   # connected, the controller not started in the run


def region_diff(a, b, x0, y0, x1, y1):
    """How many pixels differ between two shots in [x0, x1) x [y0, y1)."""
    return sum(1 for y in range(y0, y1) for x in range(x0, x1) if a.at(x, y) != b.at(x, y))


def test_r11t_hc_never():
    """R8's CX II (a device connected at the
    adapter's end, the OS's host controller stopped 69 s and more; PAD4's
    burst ended at 3 s with it stopped): the burst gives the OS the CPU while
    the controller is stopped, CU_HC_WAIT_MS at most (~1460 OS sleeps), the
    come-up's message on the game's screen; then the game plays on and the
    log says why the burst ended. A key pressed ends it sooner."""
    r = H.Run("r11t_hc_never").run(HEAVY + START_GAME + "wait 1\n" + HC_NEVER + "wait 17\nnote a\nwait 2\nnote b\nquit\n")
    text = pad_file(r)
    b = bursts(text)
    H.check(len(b) == 1 and CU_HC_WAIT_MS - 100 <= b[0][0] <= CU_HC_WAIT_MS + 300 and b[0][3] == "is STOPPED" and
            b[0][2].startswith("the host controller didn't start in 15 s"), "r11t never: the burst's bound: %s" % b)
    H.check(bool(b) and 1300 <= b[0][1] <= 1600, "r11t never: ~1460 OS sleeps in it: %s" % b)
    H.check("waiting for the host controller to start" in text, "r11t never: the burst says it waited for the controller")
    a, c = r.notes["a"], r.notes["b"]
    H.check(c["frame"] - a["frame"] >= 30, "r11t never: the game plays on after it: %d frames in 2 s" % (c["frame"] - a["frame"]))
    H.check([x for _, x in osd(r)][:1] == [COMEUP_TEXT], "r11t never: the come-up's message while the game stood still: %s" % osd(r))
    k = H.Run("r11t_hc_never_key").run(HEAVY + START_GAME + "wait 1\n" + HC_NEVER + "wait 3\npress up\nwait 3\nquit\n")
    kb = bursts(pad_file(k))
    H.check(len(kb) == 1 and kb[0][2] == "a key was pressed" and 1500 <= kb[0][0] <= 3500, "r11t never: a key ends it: %s" % kb)


def test_r11t_menu_notice():
    """In the menus (here the file list at start) nothing is drawn while the
    burst runs: once it waits for the host controller a box says so over the
    screen, and the pixels under it come back when it ends (the controller
    starts after 300 of the OS's ticks, ~3 s; the pad attaches in the burst)."""
    r = H.Run("r11t_menu_notice").run("wait 1\nshot before\npad slowhc ticks 300 26 xinput\nwait 2\nshot notice\nwait 4\n"
                                     "shot after\nquit\n")
    text = pad_file(r)
    b = bursts(text)
    H.check(len(b) == 1 and "menus waited, waiting for the host controller to start" in text and b[0][3] == "runs",
            "r11t menu notice: one burst in the menus that waited for the controller: %s" % b)
    before, notice, after = r.shot("before"), r.shot("notice"), r.shot("after")
    changed = region_diff(before, notice, 30, 88, 290, 148)
    H.check(changed > 2000, "r11t menu notice: the box on the screen during the wait (%d pixels)" % changed)
    H.check(region_diff(before, after, 0, 0, 320, 240) == 0, "r11t menu notice: the screen as before after it")
    H.check(step_ms(text, "the driver attached it"), "r11t menu notice: the pad attached")


def test_r11t_kinds():
    """The same wait whatever is on the wires (the come-up looks at the port,
    not at the device): a controller the OS starts late, and a device another
    driver takes (a drive, a sound adapter: the arbiter's OTHER) once the
    controller runs, end the burst as soon as each is taken."""
    for kind, ends, how in (("xinput", "the controller works", "the controller works"),
                            ("ds4", "the controller works", "the controller works"),
                            ("drive", "the come-up ended", "another driver has the port")):
        r = H.Run("r11t_kind_" + kind).run(HEAVY + START_GAME + "wait 1\npad slowhc ticks 600 26 %s\nwait 12\nquit\n" % kind)
        text = pad_file(r)
        b = bursts(text)
        H.check(len(b) == 1 and 5500 <= b[0][0] <= 7500 and b[0][2] == ends and b[0][3] == "runs",
                "r11t kinds, %s: one burst through the controller's start to the device: %s" % (kind, b))
        H.check(re.search(r"come-up: ended after \d+ ms \(OS sleeps \+\d+\): " + re.escape(how), text),
                "r11t kinds, %s: the come-up ended as %r:\n%s" % (kind, how, text[-1500:]))


# ---- R12: the timer block left to the OS (its 44-tick USB wait) ----

# The CX II's select as its OS had it (00000001), both fields' clocks at
# 32.768 MHz (so 44 of the OS's 0x8000-count ticks are 44 ms), the OS's USB
# timer (900C0020) from the adapter's arrival, periodic every 0x8000 counts.
R12_CLOCKS = ["--timer-clock-os", "1", "--timer-sel1-hz", "32768000", "--timer-sel0-hz", "32768000",
              "--os-usb-timer", "7fff"]
R12_MODES = (("own", []), ("borrowed", ["--timer1-os", "free"]), ("legacy", ["--timer1-os", "periodic:ffffff"]))


def usb_reset_at(run):
    """Seconds from the plug to the OS's port reset (host_usbpad.c's usbtick line), or None."""
    for _, text in pad_log(run):
        m = re.match(r"usbtick: the OS resets the port after \d+ of its USB ticks, ([\d.]+) s after the plug", text)
        if m:
            return float(m.group(1))
    return None


def timer_lines(text):
    """The pad log's R12 timers lines (at the connect, at the reset)."""
    return [x for x in text.split("\n") if re.match(r"\s*\d+ timers \(", x)]


def test_r12_usb_wait():
    """R12 (the CX II): with the adapter in and
    the OS's host controller running, a device showed connected at once and
    the OS reset its port 44.0 s after the first connect, every plug: 44
    ticks of a 0x8000-count wait counted at 32768 Hz. R11t had put 2 (32 kHz)
    into the select's bits 1:0. The model has both readings of it
    (--os-usb-wait block: those bits slow the whole block, the OS's USB timer
    too; timer1: the OS times its wait on 900C0000's count) and an OS that
    resets the port 44 of its ticks after the connect (pad usbtick).

    R11t's rule (GPSP_HOST_TIMER=r11t) gives the calculator's 44 s in both.
    The new rule (nothing written to the select; the OS's 900C0000 followed
    or left as it runs) gives the reset within 0.15 s of the plug in each
    time base (own, borrowed, legacy: there the block is lent back to the OS
    for the come-up's burst) and both readings, in the menus (the burst at
    once), and the pad attaches. The pad log says the time base and has the
    timers' lines at the connect and the reset (900C0020 counted at ~32768
    a ms, gpSP's own and borrowed count). In a game with no spare time the
    same: the reset within 0.3 s, the pad connected within 1 s."""
    script = "wait 1\npad usbtick 44 0.3 xinput\nwait 4\nquit\n"
    for wait in ("block", "timer1"):
        old = H.Run("r12_old_" + wait)
        old.select_writes_ok = True
        old.run("wait 1\npad usbtick 44 0.3 xinput\nwait 50\nquit\n", options=R12_CLOCKS + ["--os-usb-wait", wait],
                env={"GPSP_HOST_TIMER": "r11t"})
        dt = usb_reset_at(old)
        H.check(dt is not None and 40 <= dt <= 48, "r12 %s, R11t's rule: the reset %s s after the plug (the calculator: "
                "44.0 s)" % (wait, dt))
        print("  R12 %s wait, R11t's rule: the port reset %s s after the plug" % (wait, dt))
        for mode, extra in R12_MODES:
            r = H.Run("r12_%s_%s" % (wait, mode))
            r.select_writes_ok = mode == "legacy"
            r.run(script, options=R12_CLOCKS + ["--os-usb-wait", wait] + extra)
            dt = usb_reset_at(r)
            H.check(dt is not None and dt <= 0.15, "r12 %s, %s: the reset %s s after the plug (<= 0.15)" % (wait, mode, dt))
            text = pad_file(r)
            H.check("time base: %s (" % mode in text, "r12 %s, %s: the pad log's time base:\n%s" % (wait, mode, text[:2500]))
            H.check(step_ms(text, "the driver attached it"), "r12 %s, %s: the pad attached:\n%s" % (wait, mode, text[-2500:]))
            lines = timer_lines(text)
            H.check(any("timers (the device is connected): 900c0020 control e2, load 00007fff" in x for x in lines) and
                    any("timers (the OS resets the port): 900c0020 control e2, load 00007fff" in x for x in lines),
                    "r12 %s, %s: the timers' lines at the connect and the reset: %s" % (wait, mode, lines))
            if mode != "legacy":
                rates = [float(m.group(1)) for x in lines for m in [re.search(r"load 00007fff, ([\d.]+) counts a ms", x)] if m]
                H.check(len(rates) >= 2 and all(abs(v - 32768) / 32768 < 0.02 for v in rates),
                        "r12 %s, %s: the OS's USB timer counted at its own speed: %s" % (wait, mode, lines))
            print("  R12 %s wait, %s: the port reset %s s after the plug; %s" % (wait, mode, dt, lines[-1:]))
    g = H.Run("r12_game").run(HEAVY + START_GAME + "wait 1\npad usbtick 44 0.3 xinput\nwait 3\nquit\n",
                              options=R12_CLOCKS + ["--os-usb-wait", "block"])
    dt = usb_reset_at(g)
    on = plug_to(g, "pad usbtick")
    H.check(dt is not None and dt <= 0.3 and on is not None and on <= 1.0,
            "r12 in a game: the reset %s s, connected %s s after the plug" % (dt, on))
    print("  R12 in a game: the port reset %s s, the pad connected %s s after the plug" % (dt, on))


def test_pad4_page():
    """Step 4: the Controller page's 'Since plug' line has the times of the
    last come-up (host controller, offer, pad, first input)."""
    r = H.Run("pad4_page").run(START_GAME + "wait 1\npad slowhc ticks 0 26 xinput\nwait 2\npress menu\nwait 0.3\n" +
                               CONTROLLER_PAGE + "wait 1\npress esc\nwait 0.5\npress esc\nwait 0.5\npress esc\nwait 1\nquit\n")
    vals = [f.get("Since plug", "") for _, v, f, _ in page_lines(r) if v == "working"]
    H.check(vals and re.match(r"HC \d+\.\d off \d+\.\d pad \d+\.\d in \d+\.\d s$", vals[-1]),
            "pad4 page: the come-up's times: %s" % vals)


# ---- R10b (2026-11-15): failed bring-ups (driver API 6), pads in turn ----

def test_r10b_fail_then_good():
    """R10a's Switch Pro, as the driver now reports a failed bring-up (API 6:
    the step, endpoint and error): its toast names it, the Controller page's
    Driver line says where it stopped in a few words, the pad log has the
    event's facts and every trace record in words, the link log the line.
    Unplugged (the adapter stays in), the next pad gets the port at once: no
    hold-off, connected within 1 s, its A reaches the game, and the page no
    longer shows the old failure."""
    sc = START_GAME + "pad port 1\nwait 1\npad preplug switchpro\nwait 0.5\npad event unsupported switchpro out 01 19\nwait 1\n"
    sc += "press menu\nwait 0.3\n" + CONTROLLER_PAGE + "wait 1\npress esc\nwait 0.5\npress esc\nwait 0.5\npress esc\nwait 1\n"
    sc += "pad unplug\nwait 0.5\npad slowhc ticks 0 26 xinput\nwait 3\n" + held_peek("Pad_A", "a")
    sc += "press menu\nwait 0.3\n" + CONTROLLER_PAGE + "wait 1\npress esc\nwait 0.5\npress esc\nwait 0.5\npress esc\nwait 1\nquit\n"
    r = H.Run("r10b_fail_then_good").run(sc)
    shown = [x for _, x in osd(r)]
    # R11w part 2: the message's new words (it says what to do), and the
    # width limit the one rule's 50 characters (was 36)
    failed = "Switch Pro didn't start. Plug it in again."
    H.check(failed in shown and "Controller connected (Xbox 360)" in shown and
            shown.index(failed) < shown.index("Controller connected (Xbox 360)"),
            "r10b fail then good: the messages: %s" % shown)
    H.check(all(len(t) <= 50 for t in shown if "didn't start" in t), "r10b: the message fits: %s" % shown)
    pages = page_lines(r)
    bad = [f for _, v, f, _ in pages if v == "unsupported"]
    H.check(bad and bad[0].get("Driver") == "Switch Pro: start-up refused (out, ep 01, OS 19)",
            "r10b: the page names the pad and the step: %s" % (bad[0] if bad else [v for _, v, _, _ in pages]))
    good = [f for _, v, f, _ in pages if v == "working"]
    H.check(good and good[-1].get("Driver") == "took it: Xbox 360 (slot 1)", "r10b: the next pad's page: %s" % good[-1:])
    dt = plug_to(r, "pad slowhc")
    H.check(dt is not None and dt <= 1.0, "r10b: the next pad connected %s s after its plug (<= 1 s)" % dt)
    H.check(p1(r, "a") == "A", "r10b: the next pad's A reaches the game: %s" % p1(r, "a"))
    text = pad_file(r)
    H.check("driver: not supported, slot -1, 057e:2009, kind 6; failed at out: OS 19 (ep 01)" in text,
            "r10b: the pad log has the failure's step, endpoint and error:\n%s" % text[-2500:])
    for words in ("U p0 057e:2009 desc ep81 arg 64: err 0", "U p0 057e:2009 claim ep00 arg 0: err 0",
                  "U p0 057e:2009 out ep01 arg 0: OS 19", "U p0 057e:2009 quiet ep00 arg 6: OS 19",
                  "U p0 045e:028e live ep00 arg 2: err 0"):
        H.check(re.search(r"bring-up #\d+, poll \d+: " + re.escape(words), text) is not None,
                "r10b: the trace record '%s' in the pad log:\n%s" % (words, text[-2500:]))
    lines = link_log(r)
    H.check(has(lines, "pad: bring-up failed: 057e:2009 Switch Pro: step out (6), ep 01, error 19: out: OS 19 (ep 01)"),
            "r10b: the link log's line: %s" % [l for l in lines if "bring-up" in l])
    H.check(not has(lines, "seamless: no power cycle") and not [1 for _, t in pad_log(r) if t.startswith("reenum cycle")],
            "r10b: no power restart for either")


def test_r10b_halt():
    """R10a 685064: 68 ms after the Switch Pro's failed bring-up the OS halted
    its host controller (USBCMD 00080000, PORTSC 0000088d) with the pad still
    in. The glue logs it with the registers, frees the port (no 'another
    driver' holder), and a new come-up gives the OS its time in one go, which
    waits for the controller (R11t) and says when it ran again. The
    arbiter's power restart gives the device a fresh try (here it then
    works): at most two, and (2026-11-17) no longer held back while the
    controller is stopped: the OS holds the device suspended there and
    never starts it again by itself (nspire-usbshim usbshim_awake.h), and
    the restarts PAD4 saw do nothing into a stopped controller were made
    with the transceiver left asleep, which the restart's drop now wakes
    (nspire_usbcycle.c). Then the next pad works at once."""
    sc = START_GAME + "pad port 1\nwait 1\npad preplug switchpro\nwait 0.5\npad event unsupported switchpro out 01 19\n"
    sc += "wait 0.1\npad halt 300\nwait 12\npad out\npad unplug\nwait 1\npad slowhc ticks 0 26 xinput\nwait 3\n"
    sc += held_peek("Pad_A", "a")
    r = H.Run("r10b_halt").run(sc + "quit\n")
    runs = [t for t, x in pad_log(r) if x.startswith("halt: the host controller runs again")]
    cycles = [t for t, x in pad_log(r) if x.startswith("reenum cycle")]
    H.check(runs and cycles, "r10b halt: the controller ran again and the device got its restart: %s, %s" % (runs, cycles))
    H.check(len(cycles) <= 2 and "Controller connected (Switch Pro)" in [x for _, x in osd(r)],
            "r10b halt: then the restart's fresh try: %s, %s" % (cycles, osd(r)))
    text = pad_file(r)
    i_stop = text.find("pad glue: the OS's host controller STOPPED")
    H.check(i_stop >= 0 and "(USBCMD 00080000 PORTSC 0000088d): the port is free again" in text,
            "r10b halt: the pad log says the OS stopped its host controller, with the registers:\n%s" % text[-3000:])
    after = text[i_stop:] if i_stop >= 0 else ""
    H.check("come-up: a device came onto" in after and "waiting for the host controller to start" in after and
            "pad glue: the OS's host controller runs again" in after,
            "r10b halt: a come-up after it, its burst waited for the controller, which ran again:\n%s" % after[:3000])
    H.check("owner: WAIT (was OTHER)" in after, "r10b halt: the port no longer another driver's after it:\n%s" % after[:1500])
    dt = plug_to(r, "pad slowhc")
    H.check(dt is not None and dt <= 1.0 and p1(r, "a") == "A", "r10b halt: the next pad connected %s s after its plug, A: %s" %
            (dt, p1(r, "a")))


ROUNDS_KINDS = ("xinput", "gip", "ds4", "dualsense", "switchpro", "hid")
KIND_NAMES = {"xinput": "Xbox 360", "gip": "Xbox One", "ds4": "DualShock 4", "dualsense": "DualSense",
              "switchpro": "Switch Pro", "hid": "USB gamepad"}


def test_r10b_rounds():
    """Ten rounds of unplug and plug across every family, into an adapter that
    stays in, during a game with no spare time; in round 4 a pad whose
    bring-up fails comes first. Each pad: connected within 1 s of its plug,
    its A and B reach the game, let go at its unplug; the failed one never
    holds the next one off."""
    sc = HEAVY + START_GAME + "pad port 1\nwait 1\n"
    plan = []
    for i in range(10):
        kind = ROUNDS_KINDS[i % len(ROUNDS_KINDS)]
        if i == 3:
            sc += "pad preplug ds4\nwait 0.5\npad event unsupported ds4 in_open 84 11\nwait 1.5\npad unplug\nwait 0.5\n"
        sc += "pad slowhc ticks 0 26 %s\nwait 1.5\n" % kind
        sc += held_peek("Pad_A", "a%d" % i) + held_peek("Pad_B", "b%d" % i)
        sc += "hold Pad_A\nwait 0.2\npad out\nwait 0.2\npeek out%d 4000130:2\nrelease Pad_A\npad port 1\nwait 1\n" % i
        plan.append(kind)
    r = H.Run("r10b_rounds").run(sc + "quit\n")
    plugs = [t for t, x in pad_log(r) if x.startswith("pad slowhc")]
    shown = osd(r)
    for i, kind in enumerate(plan):
        want = "Controller connected (%s)" % KIND_NAMES[kind]
        nxt = plugs[i + 1] if i + 1 < len(plugs) else 1e9
        on = [t for t, x in shown if x == want and i < len(plugs) and plugs[i] <= t < nxt]
        dt = on[0] - plugs[i] if on else None
        H.check(dt is not None and dt <= 1.0, "rounds: round %d (%s) connected %s s after its plug" % (i + 1, kind, dt))
        H.check(p1(r, "a%d" % i) == "A" and p1(r, "b%d" % i) == "B",
                "rounds: round %d (%s): A and B reach the game: %s, %s" % (i + 1, kind, p1(r, "a%d" % i), p1(r, "b%d" % i)))
        H.check(p1(r, "out%d" % i) == "none", "rounds: round %d (%s): let go at the unplug: %s" % (i + 1, kind, p1(r, "out%d" % i)))
    # (R11w part 2: the message's new words)
    H.check("DualShock 4 didn't start. Plug it in again." in [x for _, x in shown],
            "rounds: the failed one said so: %s" % [x for _, x in shown])
    # The ring (256 entries) keeps the newest of the tries that worked; the
    # failed try is held through the nine after it (R10a's lost lines).
    text = pad_file(r)
    held = re.search(r"(\d+) held for tries that failed", text)
    H.check("failed at in_open: OS 11 (ep 84)" in text and "U p0 054c:09cc in_open ep84 arg 0: OS 11" in text and
            held and int(held.group(1)) > 0 and "older dropped" in text,
            "rounds: the failed try held in the exit's pad log:\n%s" % text[:3000])
    H.check(len([1 for _, x in pad_log(r) if x.startswith("osd Controller connected")]) == 10, "rounds: ten pads connected")
    check_shutdown(r, "rounds")


def test_r11w_exit_bounded():
    """R11w (2026-11-15): the run's quit wrote the pad log and the perf log,
    then never finished (the marker said "running", the keys dead, a reset).
    Now every file is written before the USB shutdowns, each shutdown is
    bounded, and one that does not finish (here: the pad driver, a callback
    that never leaves gpSP) restarts the calculator after "exit: USB
    shutdown timed out at pad driver" in the pad log and the link log and
    two seconds of a plain message. A normal quit arms the watchdog backstop
    for the shutdowns and has it off again at the end."""
    # gpSP's own quit (the quit hotkey: nspire_quit_game, quit()), not the
    # script's end (which leaves through exit's handlers only)
    r = H.Run("r11w_exit_ok").run(START_GAME + "pad in xinput\nwait 1\npress Q\nwait 8\nnote after\nquit\n")
    H.check("after" not in r.notes, "r11w exit ok: gpSP quit by itself")
    H.check(r.report.get("restarted") == "0", "r11w exit ok: no restart: %s" % r.report.get("restarted"))
    H.check(r.report.get("exit_backstop_arms") == "1" and r.report.get("exit_backstop_end") == "off",
            "r11w exit ok: the backstop armed once, off at the end: %s %s" %
            (r.report.get("exit_backstop_arms"), r.report.get("exit_backstop_end")))
    log = pad_file(r)
    H.check("exit: the files are written; the USB shutdowns come next (1 pad(s) in)" in log,
            "r11w exit ok: the pad log's milestone before the shutdowns:\n%s" % log[-3000:])
    H.check("pad log written at exit, before the USB shutdowns" in log, "r11w exit ok: the pad log's header: %s" % log[:300])
    H.check("Milestones (" in log and "The driver's diag at exit" in log,
            "r11w exit ok: the milestones and the driver's block in the pad log:\n%s" % log[-3000:])
    H.check(run_marker(r).startswith("clean"), "r11w exit ok: the clean marker: %s" % run_marker(r))
    lines = link_log(r)
    H.check(has(lines, "exit: the USB shutdowns done in"), "r11w exit ok: the link log says the shutdowns finished: %s" %
            lines[-20:])

    r = H.Run("r11w_exit_noretire").run(START_GAME + "pad in xinput\nwait 1\npad noretire\npress Q\nwait 12\n"
                                                      "note after\nquit\n")
    H.check("after" not in r.notes, "r11w no retire: it restarted before the script's end")
    H.check(r.report.get("restarted") == "1", "r11w no retire: the calculator restarted: %s" % r.report.get("restarted"))
    log = pad_file(r)
    H.check("exit: USB shutdown timed out at pad driver" in log,
            "r11w no retire: the pad log says where:\n%s" % log[-3000:])
    H.check("exit: the files are written; the USB shutdowns come next" in log,
            "r11w no retire: the files were written first:\n%s" % log[-3000:])
    lines = link_log(r)
    H.check(has(lines, "exit: USB shutdown timed out at pad driver"), "r11w no retire: the link log: %s" % lines[-20:])
    osd = [(t, text) for t, text in pad_log(r) if "USB did not shut down. The calculator restarts now." in text]
    H.check(len(osd) == 1, "r11w no retire: the message once: %s" % pad_log(r)[-10:])
    quit_at = [t for t, text in pad_log(r) if text.startswith("shutdown:")]
    at = float(r.report.get("restarted_at", "-1"))
    if osd and quit_at:
        H.check(1.9 <= at - osd[0][0] <= 2.5, "r11w no retire: the message for 2 s before the restart: %.2f s" %
                (at - osd[0][0]))
        H.check(at - quit_at[0] <= 4.0 + 2.5, "r11w no retire: the restart %.2f s after the driver's shutdown" %
                (at - quit_at[0]))
    H.check(not run_marker(r).startswith("clean"), "r11w no retire: no clean marker (it restarted): %s" % run_marker(r))
    # the shutdown's own bound: the driver was asked to wait no longer than the budget
    H.check(any("shutdown_ticks 400" in text or "shutdown_ticks 39" in text for _, text in pad_log(r)),
            "r11w no retire: the driver's wait bounded by the exit's budget: %s" %
            [text for _, text in pad_log(r) if "shutdown" in text])


def test_r11w_volume_keys():
    """R11w part 2: with a sound adapter in, times louder, divide quieter, a
    step of Volume's twenty a press, "Volume N/20" for a moment, the
    setting saved as the page's (at the quit); no adapter: the keys do
    nothing of the kind."""
    script = START_GAME + "press *\nwait 0.3\npress *\nwait 0.3\npress /\nwait 0.3\npress Q\nwait 2\nquit\n"
    r = H.Run("r11w_volume").run(script, env={"GPSP_HOST_AUDIO": "1"})
    shown = [t[4:] for _, t in pad_log(r) if t.startswith("osd Volume")]
    H.check(shown == ["Volume 15/20", "Volume 16/20", "Volume 15/20"], "volume keys: one step a press: %s" % shown)
    cfg = H.read_words(r.save("gpsp.cfg.tns"))
    H.check(cfg[3] & 0x200000 and (cfg[3] >> 16) & 31 == 15 and (cfg[3] >> 4) & 15 == 8,
            "volume keys: saved as Settings' Volume saves it (bit 21, 15 in bits 16-20, half of it in 4-7): %08x" % cfg[3])
    r = H.Run("r11w_volume_none").run(script)
    H.check(not [t for _, t in pad_log(r) if t.startswith("osd Volume")], "volume keys: no adapter, no volume")


def test_r13_quit_order_and_audio_ff():
    """R13: pad driver stops before NavNet/drive on quit, and fast-forward
    keeps the normal OSD while running."""
    script = START_GAME + "press F\nwait 0.3\npress Q\nwait 4\nquit\n"
    r = H.Run("r13_audio_ff").run(script, env={"GPSP_HOST_AUDIO": "1"})
    osds = [t[4:] for _, t in pad_log(r) if t.startswith("osd ")]
    H.check("Fast forward on" in osds, "r13: fast forward OSD present: %s" % osds)
    marker = run_marker(r)
    i_pad = marker.find("pad driver shut down")
    i_link = marker.find("link shut down (NavNet StopService)")
    i_drv = marker.find("USB drive shut down")
    H.check(0 <= i_pad < i_link < i_drv, "r13: shutdown order pad < link < drive (%d, %d, %d)\n%s" %
            (i_pad, i_link, i_drv, marker))



def test_pad_silent():
    """A CX log (r19, 926fa73): an 8BitDo
    attached and sent nothing for 23 s ("0 reports" at its detach) while the
    page and the pad log said "working: The controller works.". A pad attached
    that has sent no report 3 s after its attach is "silent" (not working, not
    a failure: no "USB problem" toast), the pad log says so once, and its first
    report makes it "working"."""
    r = H.Run("pad_silent").run(START_GAME + "press menu\nwait 0.3\n" + CONTROLLER_PAGE +
                                "pad reports silent on\npad in xinput\nwait 6\npad reports silent off\nwait 2\n"
                                "press esc\nwait 0.5\npress esc\nwait 0.5\npress esc\nwait 1\nquit\n")
    pages = page_lines(r)
    seen = [v for _, v, _, _ in pages]
    order = [v for i, v in enumerate(seen) if i == 0 or seen[i - 1] != v]
    H.check("silent" in order and order.index("silent") < len(order) - 1 and order[-1] == "working",
            "pad silent: the page says silent, then working after the first report: %s" % order)
    t_in = [t for t, text in pad_log(r) if text.startswith("pad in xinput")]
    t_sil = [t for t, v, _, _ in pages if v == "silent"]
    H.check(t_in and t_sil and 2.5 <= t_sil[0] - t_in[0] <= 4.5,
            "pad silent: silent 3 s after the attach: %s" % ([round(t_sil[0] - t_in[0], 2)] if t_in and t_sil else seen))
    tops = [f.get("top", "") for _, v, f, _ in pages if v == "silent"]
    H.check(tops and tops[-1].startswith("Connected, but nothing has come from it."),
            "pad silent: the page's words: %s" % tops[-1:])
    text = pad_file(r)
    H.check(text.count("has sent NO REPORT") == 1 and "attached 3000 ms ago" in text,
            "pad silent: the pad log says it once:\n%s" % "\n".join(l for l in text.splitlines() if "pad:" in l))
    H.check("status: silent:" in text, "pad silent: the status line says silent:\n%s" % text[-2500:])
    H.check("USB problem: Settings > Controller" not in [t for _, t in toasts(r)],
            "pad silent: no USB problem toast: %s" % toasts(r))
    r = H.Run("pad_not_silent").run(START_GAME + "pad in xinput\nwait 6\nquit\n")
    H.check("has sent NO REPORT" not in pad_file(r), "pad silent: a pad that reports is never silent")


def test_pad_no_adapter():
    """A CX II log (r19, 958921d) with a controller on a cable: no adapter's end
    in 899 s, the OS powering the idle port down and up ("went off and on 61
    time(s)", then 188), the page between "empty" and "controller is off".
    The page says one thing through it (no adapter in the port, and that a
    plain cable won't work), the sentence under the list never comes and goes,
    and the pad log's line says what every reading said and that gpSP wrote
    nothing to the port (no power restart, no cycle)."""
    flap = "pad port -2\nwait 0.3\npad port 0\nwait 0.3\n" * 12
    r = H.Run("pad_no_adapter").run(START_GAME + "press menu\nwait 0.3\n" + CONTROLLER_PAGE + "pad port 0\nwait 0.6\n" +
                                    flap + "press esc\nwait 0.5\npress esc\nwait 0.5\npress esc\nwait 1\nquit\n")
    pages = page_lines(r)
    seen = [v for _, v, _, _ in pages]
    H.check("nothing" in seen and "unreadable" in seen, "pad no adapter: both states came (the case): %s" % sorted(set(seen)))
    said = [f.get("said", "") for _, v, f, _ in pages if v in ("nothing", "unreadable")]
    H.check(said and all(x == said[0] for x in said) and said[0].startswith("No controller: no OTG adapter is in the USB port.")
            and "a plain cable won't work" in said[0],
            "pad no adapter: the page says one thing, no adapter and the plain cable: %s" % sorted(set(said)))
    H.check(all(" / " not in x for x in said), "pad no adapter: no sentence comes and goes under it: %s" % sorted(set(said)))
    tops = set(f.get("top", "") for _, v, f, _ in pages if v in ("nothing", "unreadable"))
    H.check(len(tops) == 1, "pad no adapter: its words are one state's: %s" % tops)
    text = pad_file(r)
    idle = [l for l in text.splitlines() if "went off and on" in l]
    H.check(idle and "NO OTG ADAPTER in the port" in idle[-1] and "gpSP wrote nothing to the port" in idle[-1],
            "pad no adapter: the pad log's line says what the readings said: %s" % idle)
    m = re.search(r"(\d+) reading\(s\) of the ID pin at the device's end \(NO OTG ADAPTER in the port\), (\d+) with the "
                  r"controller off", idle[-1] if idle else "")
    H.check(m and int(m.group(1)) >= 5 and int(m.group(2)) >= 5,
            "pad no adapter: both kinds of reading counted: %s" % ((m.groups(),) if m else idle))
    H.check("power restart" not in text and not [t for t, x in pad_log(r) if x.startswith("reenum cycle")],
            "pad no adapter: gpSP restarts nothing")


class _Done:
    """A gpsp-host already waited for (bounded_run): what Run.finish asks of a Popen."""
    def __init__(self, err, code):
        self.err, self.returncode = err, code

    def communicate(self, timeout=None):
        return "", self.err


def bounded_run(r, script, secs=120, env=None):
    """r.run(script), but a gpsp-host still running after secs (an exit that
    never ends: the calculator's freeze) is killed and said: None then."""
    proc = r.start(script, env=env)
    try:
        _, err = proc.communicate(timeout=secs)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.communicate()
        return None
    return r.finish(_Done(err, proc.returncode))


def trail_lines(r):
    path = r.save("gpsp_exit.run")
    return open(path).read().splitlines() if os.path.exists(path) else None


def test_exit_trail():
    """A CX II run (e4d92fa): a hub
    with the Bluetooth dongle and a USB drive on it, Bluetooth on; the quit
    froze on the quitting screen after "the USB shutdowns come next", the
    marker still "running", nothing saying where. Now every step of the way
    out leaves a line in gpsp_exit.run first (the hand-back removes it), the
    next start puts the last line of a trail left behind in its pad log
    ("LAST RUN: ..."), and a wait in the USB shutdowns that still sleeps past
    their time is cut there: the drive's stop waiting for a callback that
    never returns, Bluetooth's stop waiting for a dongle gone silent (both
    looped for ever before: the run never ended)."""
    quit_q = "wait 2\npress Q\nwait 14\nnote after\nquit\n"
    # a USB drive (nspire-usbms's fake over a FAT image: the real nspire_drive.c and usbms.c) and the dongle
    img = os.path.join(H.WORK, "exit_trail.img")
    os.makedirs(H.WORK, exist_ok=True)
    subprocess.run(["truncate", "-s", "16M", img], check=True)
    subprocess.run(["mkfs.vfat", "-F", "16", img], check=True, capture_output=True)
    drive = {"GPSP_USB_IMAGE": img}
    r = H.Run("exit_trail_ok").run(START_GAME + "bt dongle in\n" + quit_q, env=drive)
    H.check("USB drive shut down" in run_marker(r) and "usbms_stop" not in run_marker(r),
            "exit trail: the drive was in and stopped: %s" % run_marker(r)[:900])
    H.check("after" not in r.notes and run_marker(r).startswith("clean"), "exit trail: a normal quit ends clean")
    H.check(trail_lines(r) is None, "exit trail: the hand-back removed the trail: %s" % trail_lines(r))
    H.check("LAST RUN" not in pad_file(r), "exit trail: no last run's trail said after a clean one")
    for kind, step in (("drive", "usbms_stop"), ("bluetooth", "Bluetooth's stop")):
        name = "exit trail, the %s's stop never ends" % kind
        r = H.Run("exit_trail_hang_" + kind)
        done = bounded_run(r, START_GAME + "bt dongle in\npad exit hang %s\n" % kind + quit_q, env=drive)
        H.check(done is not None, "%s: the run ends (it looped for ever before)" % name)
        if done is None:
            continue
        H.check(r.report.get("restarted") == "1", "%s: the exit's restart: %s" % (name, r.report.get("restarted")))
        log = pad_file(r)
        H.check("exit: USB shutdown timed out at %s" % step in log and "would never end" in log,
                "%s: the pad log says which wait:\n%s" % (name, "\n".join(l for l in log.splitlines() if "exit" in l)))
        lines = trail_lines(r) or []
        H.check(len(lines) >= 10 and lines[0].startswith("exit step 1 at ") and step in lines[-1] and
                "the restart" in lines[-1],
                "%s: the trail on the disk, its last line the step and the restart:\n%s" % (name, "\n".join(lines)))
        H.check(any("the pad log" in l for l in lines) and any("Bluetooth's stop" in l for l in lines),
                "%s: a line before each step: %s" % (name, lines))
        t = [int(l.split(" at ")[1].split(" ms")[0]) for l in lines if l.startswith("exit step ")]
        H.check(t == sorted(t), "%s: the lines in order, timed: %s" % (name, t))
        r2 = H.Run("exit_trail_next_" + kind, reuse=r).run(START_GAME + "wait 1\nquit\n")
        log2 = pad_file(r2)
        last = [l for l in log2.splitlines() if l.startswith("LAST RUN:")]
        H.check(last and "never reached the hand-back" in last[0] and step in last[0] and "the restart" in last[0],
                "%s: the next start's pad log says where the last exit ended: %s" % (name, last or log2[:600]))
        H.check(trail_lines(r2) is None, "%s: the next run's own clean exit leaves no trail" % name)
        H.check(any("exit trail:" in l for l in link_log(r2)), "%s: the link log has it too" % name)


def test_pad_power_held():
    """A CX II log (567381a): the adapter's end with its power turned on and
    held down for 8.8 s (OTGCSR 00012e10: A_BUS_REQ set, no A VBUS, B session
    end), nothing on the wires, then on and a hub on it 0.37 s later; the pad
    log said "no_power: ... unplug the adapter and plug it in again" and, as
    the power came, "the device was unplugged with nothing working". Now that
    state is its own (power_held: what is plugged in draws more than the port
    gives), said on the page in plain words after it has held, once in the pad
    log with its time, and the power coming up ends the try as that, not as an
    unplug. The OS's own start of the port (the power not yet asked for, then
    on: the model's bus power off and on) ends nothing and says its time."""
    r = H.Run("pad_power_held").run(START_GAME + "press menu\nwait 0.3\n" + CONTROLLER_PAGE +
                                    "pad port 1\npad load heavy on\nwait 5\npad load heavy off\nwait 2\n"
                                    "press esc\nwait 0.5\npress esc\nwait 0.5\npress esc\nwait 1\nquit\n")
    pages = page_lines(r)
    held = [f for _, v, f, _ in pages if v == "power_held"]
    H.check(held and any(f.get("top", "").startswith("Not enough power for what is plugged in.") for f in held),
            "pad power held: the page says it: %s" % sorted(set(v for _, v, _, _ in pages)))
    H.check(any("Not enough power for what is plugged in." in f.get("said", "") for f in held),
            "pad power held: the sentence under the list: %s" % [f.get("said") for f in held][-1:])
    text = pad_file(r)
    # (once: its event line and the milestones' copy of it)
    H.check(text.count("port: the adapter's power was turned on and stays off") == 2 and "OTGCSR or OTGSC 00010010" in text,
            "pad power held: the pad log says it once, with the register:\n%s" %
            "\n".join(l for l in text.splitlines() if "power" in l))
    H.check("port: the adapter's power came up after" in text and
            "try ended: the adapter's power came up after it was held down" in text and
            "try ended: the device was unplugged with nothing working" not in text,
            "pad power held: its end said as that, not as an unplug:\n%s" %
            "\n".join(l for l in text.splitlines() if "try ended" in l or "power" in l))
    r = H.Run("pad_power_os_start").run(START_GAME + "pad port 1\npad buspower off\nwait 3\npad buspower on\nwait 3\n"
                                        "quit\n")
    text = pad_file(r)
    H.check("port: the adapter's power came on" in text and "the OS's own start of the port" in text and
            "try ended: the device was unplugged" not in text and "stays off" not in text,
            "pad power os start: the power's coming said with its time, no try ended:\n%s" %
            "\n".join(l for l in text.splitlines() if "try ended" in l or "power" in l or "status:" in l))

TESTS = [test_r13_quit_order_and_audio_ff, test_r11w_exit_bounded, test_r11w_volume_keys, test_s8_budgets, test_s8_latency, test_pad_play, test_pad_one_source, test_pad_menu, test_pad_bind, test_pad_turbo, test_pad_no_doubles, test_pad_two,
         test_pad_settings, test_pad_os_time,
         test_pad_stale_hints, test_pad_failed, test_pad_replug_port, test_pad_attach_time, test_pad_attach_ends,
         test_pad_link_refused, test_pad_gated, test_pad_gated_quit, test_exit_untouched, test_exit_log_buffers, test_pad_toasts_wait, test_pad_no_timer, test_pad_log,
         test_pad_log_input, test_pad_log_replug, test_pad_log_never_offered, test_pad_log_exit, test_pad_log_stages, test_pad_log_offers,
         test_pad_log_quiet,
         test_pad_preplug, test_pad_cycle, test_pad_resident, test_pad_direct, test_pad_reenum, test_toasts_seamless, test_swap,
         test_pad3_page, test_pad3_hcstop_game, test_pad3_after_link, test_pad3_pads_in_turn,
         test_pad3_idle_churn, test_pad3_os_time, test_pad3_os_time_no_timer,
         test_pad4_running, test_pad4_os_time, test_pad4_wall, test_pad4_noconn, test_pad4_no_chase, test_pad4_in_turn,
         test_pad4_page, test_r11t_hc_never, test_r11t_menu_notice, test_r11t_kinds, test_r12_usb_wait,
         test_r10b_fail_then_good, test_r10b_halt, test_r10b_rounds,
         test_pad_silent, test_pad_no_adapter, test_exit_trail, test_pad_power_held]


def main():
    if not os.path.exists(USBPAD_H):
        print("pad tests skipped: no %s (gpsp-host has no pads)" % os.path.normpath(USBPAD_H))
        return 0
    wanted = sys.argv[1:]
    os.makedirs(H.WORK, exist_ok=True)
    for test in TESTS:
        name = test.__name__[5:]
        if wanted and name not in wanted:
            continue
        print(name)
        test()
    print("%d checks, %d failed" % (H.checks, len(H.failures)))
    return 1 if H.failures else 0


if __name__ == "__main__":
    sys.exit(main())
