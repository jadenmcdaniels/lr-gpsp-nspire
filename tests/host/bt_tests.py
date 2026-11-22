#!/usr/bin/env python3
"""Bluetooth in gpSP (nspire_bt_glue.c, nspire_bt_page.c, the sound's routing
in nspire_audio.c) on the simulated calculator, with host_bt.c standing in
for nspire-bt (its "bt ..." script lines: a dongle, pads, headphones, what a
search finds):

 - no dongle: nothing of Bluetooth's runs per frame (no poll, no window, no
   OS sleep), the same game speed as with Bluetooth off;
 - slots: a Bluetooth pad after a USB one is "Controller 2", its lights
   say 2, its buttons reach the game; alone it is "Controller 1";
 - the sound: headphones take it (Output Automatic or Bluetooth), not with
   Output USB adapter; paused in the menus, fast forward's mode in fast
   forward;
 - the page: Settings > Bluetooth, a search listing every device found
   (controllers, headphones, the rest), pairing one, the paired list,
   forgetting one (asked first), Bluetooth off and on;
 - the settings (word 22 of gpsp.cfg.tns), written only when not the
   defaults;
 - the exit: the stop within the exit's time, gpsp_bt.txt.tns written; a
   callback left inside gpSP: the exit's restart.

    bt_tests.py [NAME...]      all tests, or the ones named"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import host_tests as H
import r19nav as N  # noqa: E402
import pad_tests as P  # noqa: E402

BT_H = os.path.join(os.environ.get("BT_DIR", os.path.join(HERE, "..", "..", "..", "nspire-bt")), "include",
                    "nspire_bt.h")
TESTS = []
# (esc three times first: from a page, its grid, the tabs: back to the game; in the game it does nothing)
QUIT = "press esc\nwait 0.3\npress esc\nwait 0.3\npress esc\nwait 0.3\npress menu\nwait 0.5\n" + H.to_main("exit") + "press enter\nwait 3\nquit\n"


def bt_log(run):
    """host_bt.c's lines in out/pad.txt: (time, text after "bt ")."""
    return [(t, text[3:]) for t, text in P.pad_log(run) if text.startswith("bt ")]


def osd(run):
    return [(t, text[4:]) for t, text in P.pad_log(run) if text.startswith("osd ")]


def said(run, text):
    return any(text in s for _, s in osd(run))


def stop_line(run):
    lines = [s for _, s in bt_log(run) if s.startswith("stop within")]
    return lines[-1] if lines else ""


def polls(run):
    line = stop_line(run)
    return int(line.split("polls ")[1].split(",")[0]) if "polls " in line else -1


def test_no_dongle_costs_nothing():
    """Bluetooth on, no dongle, 10 s of a game: the glue never polls the
    library (polls 0: the per-frame sites read a word), no OS sleep of its
    own, the game's frames as with Bluetooth off; the stop at the exit."""
    words = list(H.owner_config())
    on = H.Run("bt_no_dongle", files={"gpsp_qol.cfg.tns": list(words)}).run(
        P.START_GAME + "wait 2\nnote a\nwait 10\nnote b\n" + QUIT)
    name = "Bluetooth on, no dongle"
    H.check(any(s.startswith("start") for _, s in bt_log(on)), "%s: it started (the driver registered)" % name)
    H.check(polls(on) == 1, "%s: the library polled once (at the start), never per frame: %s" % (name, stop_line(on)))
    H.check("stop within" in stop_line(on), "%s: stopped at the exit" % name)
    if len(words) > 22:
        words[22] = 0xB7000000   # Bluetooth off (its mark, bit 0 clear)
        r = H.Run("bt_off", files={"gpsp_qol.cfg.tns": words}).run(P.START_GAME + "wait 2\nnote a\nwait 10\nnote b\n" + QUIT)
        H.check(not any(s.startswith("start") for _, s in bt_log(r)), "Bluetooth off: never started")
        f_on = on.notes["b"]["frame"] - on.notes["a"]["frame"]
        f_off = r.notes["b"]["frame"] - r.notes["a"]["frame"]
        H.check(abs(f_on - f_off) <= 2, "%s: %d game frames in 10 s, %d with Bluetooth off" % (name, f_on, f_off))


TESTS.append(test_no_dongle_costs_nothing)


def test_slots():
    """A USB controller in, then a Bluetooth DualSense: it is Controller 2
    (its toast, its lights), and its buttons reach the game; the USB one
    out and the Bluetooth one alone again: a second Bluetooth pad is
    Controller 2 after it."""
    r = H.Run("bt_slots").run(P.START_GAME + """pad port 1
wait 1
pad in xinput
wait 4
bt dongle in
wait 2
bt pad in dualsense DualSense Wireless Controller
wait 4
""" + P.held_peek("Pad_A", "a") + """pad out
wait 4
bt pad in switchpro Pro Controller
wait 4
""" + P.held_peek("Pad_B", "b") + """bt pad out 1
wait 3
quit
""")
    name = "a Bluetooth pad after a USB one"
    H.check(said(r, "Controller 2 connected (Bluetooth)"), "%s: the toast says Controller 2: %s" % (name, osd(r)))
    H.check(any(s == "lights slot 0 player 2" for _, s in bt_log(r)), "%s: its lights say 2: %s" %
            (name, [s for _, s in bt_log(r) if s.startswith("lights")]))
    H.check(P.p1(r, "a") == "A", "%s: its A reaches the game: %s" % (name, P.p1(r, "a")))
    H.check(any(s == "lights slot 1 player 2" for _, s in bt_log(r)),
            "a second Bluetooth pad (the USB one gone): Controller 2, lights 2: %s" %
            [s for _, s in bt_log(r) if s.startswith("lights")])
    H.check(len([1 for _, t in osd(r) if t == "Controller 2 connected (Bluetooth)"]) >= 2,
            "the second Bluetooth pad's toast: %s" % osd(r))
    H.check(P.p1(r, "b") == "B", "the Bluetooth pads' B reaches the game: %s" % P.p1(r, "b"))
    H.check(said(r, "Controller 1 disconnected"), "the first one out: its toast: %s" % osd(r))
    H.check(any(s == "lights slot 0 player 1" for _, s in bt_log(r)),
            "the USB controller out: the DualSense is Controller 1, its lights say so: %s" %
            [s for _, s in bt_log(r) if s.startswith("lights")])


TESTS.append(test_slots)


def test_pad_alone():
    """A Bluetooth pad alone is Controller 1; the dongle pulled out: its
    toast; plugged in again: Bluetooth on again, said."""
    r = H.Run("bt_pad_alone").run(P.START_GAME + """bt dongle in
wait 2
bt pad in dualsense DualSense Wireless Controller
wait 3
""" + P.held_peek("Pad_A", "a") + """bt dongle out
wait 3
""" + P.held_peek("Pad_A", "gone") + """bt dongle in
wait 3
quit
""")
    H.check(said(r, "Controller 1 connected (Bluetooth)"), "a Bluetooth pad alone: Controller 1: %s" % osd(r))
    H.check(P.p1(r, "a") == "A", "its A reaches the game: %s" % P.p1(r, "a"))
    H.check(said(r, "Bluetooth: dongle unplugged"), "the dongle pulled out: said: %s" % osd(r))
    H.check(P.p1(r, "gone") == "none", "with the dongle out, nothing held: %s" % P.p1(r, "gone"))
    H.check(said(r, "Bluetooth on"), "the dongle back: \"Bluetooth on\": %s" % osd(r))


TESTS.append(test_pad_alone)


def bt_cfg(output=0, codec=0):
    """A settings file with the Bluetooth word set (word 22: mark, Bluetooth on, Output in bits 4-5, BT quality in 8-9).
    r19's fourth pass took Sound output and BT quality out of the menus: the file is how they are set."""
    o = H.owner_config()
    o[22] = 0xB7000001 | (output << 4) | (codec << 8)
    return {"gpsp.cfg.tns": o}


def bt_run(name, script, output=0, codec=0):
    return H.Run(name, files=bt_cfg(output, codec)).run(script)


def audio_modes(run):
    return [(t, int(s.split()[-1])) for t, s in bt_log(run) if s.startswith("audio mode")]


def test_sound_routing():
    """Headphones in: the game's sound goes to them (mode 1), the menu pauses it (0), fast forward is mode 2; with
    Output = USB adapter (the settings file: no tile any more) they get nothing (mode 0 only); headphones out: nothing
    more."""
    r = bt_run("bt_sound", P.START_GAME + """bt dongle in
wait 2
bt headphones in WH-1000XM4
wait 2
note playing
press menu
wait 1
note menu
press esc
wait 1
press F
wait 2
note ff
press F
wait 1
bt headphones out
wait 1
quit
""", 0)
    modes = audio_modes(r)
    n = r.notes

    def mode_at(t):
        t = float(t)
        m = [v for at, v in modes if at < t]
        return m[-1] if m else None
    times = dict((k, n[k]["time"]) for k in ("playing", "menu", "ff") if k in n)
    H.check(mode_at(n["playing"]["time"]) == 1, "headphones in: the sound to them (mode 1): %s %s" % (modes, times))
    H.check(mode_at(n["menu"]["time"]) == 0, "the menu: paused (mode 0): %s" % modes)
    H.check(mode_at(n["ff"]["time"]) == 2, "fast forward: mode 2: %s" % modes)
    H.check(said(r, "Headphones connected"), "their toast: %s" % osd(r))
    u = bt_run("bt_sound_usb", P.START_GAME + "bt dongle in\nwait 2\nbt headphones in WH-1000XM4\nwait 2\nnote usb\n"
                                  "bt headphones out\nwait 1\nquit\n", 1)
    um = audio_modes(u)
    H.check(not [v for at, v in um if v == 1 and at < float(u.notes["usb"]["time"])],
            "Output: USB adapter: the headphones get nothing (never mode 1): %s" % um)


TESTS.append(test_sound_routing)


def test_codec_default():
    """The headphones' codec by the model: mono on a CX, joint stereo on a CX II."""
    cx2 = H.Run("bt_codec_cx2").run(P.START_GAME + "wait 1\nquit\n")
    cx = H.Run("bt_codec_cx").run(P.START_GAME + "wait 1\nquit\n", options=["--model", "cx"])
    H.check(any(s.startswith("codec joint stereo") for _, s in bt_log(cx2)), "a CX II: joint stereo: %s" % bt_log(cx2)[:6])
    H.check(any(s.startswith("codec mono") for _, s in bt_log(cx)), "a CX: mono: %s" % bt_log(cx)[:6])


TESTS.append(test_codec_default)


def test_codec_high():
    """bt10: BT quality High (the settings file: no tile any more): joint stereo bitpool 53 asked for the stream, and
    kept in the file at the exit."""
    r = bt_run("bt_codec_high", P.START_GAME + "bt dongle in\nwait 2\n" + QUIT, 0, 3)
    H.check(any(s == "codec joint stereo bitpool 53" for _, s in bt_log(r)), "High: bitpool 53: %s" % bt_log(r)[:8])
    words = H.read_words(r.save("gpsp.cfg.tns")) if os.path.exists(r.save("gpsp.cfg.tns")) else []
    H.check(len(words) > 22 and (words[22] >> 8) & 3 == 3, "BT sound High kept: %s" % (hex(words[22]) if len(words) > 22 else words))


TESTS.append(test_codec_high)


def to_bluetooth():
    """Script lines: from the game into Settings > Bluetooth (ui7's list page: Bluetooth, Search, PAIRED, its
    devices, FOUND, its devices; ui18: Bluetooth and Search two tiles side by side, down from them the list's
    first device; the page starts a search as it opens)."""
    return "press menu\nwait 0.5\n" + N.settings_tile("bluetooth") + "wait 0.3\npress enter\nwait 0.5\n"


def test_page_search_and_pair():
    """Settings > Bluetooth: the page searches as it opens and lists everything heard (controllers and
    headphones first, each the stronger first, then the rest: r19-bt, as a phone's list); enter on the
    headphones pairs them (Pairing, then connected); the phone, the strongest, is listed last as PHONE, and
    enter tries it: "Not a controller or headphones.")."""
    r = H.Run("bt_page_search").run(P.START_GAME + """bt dongle in
bt near phone -30 Pixel 8
bt near headphones -50 WH-1000XM4
bt near pad -45 DualSense Wireless Controller
wait 2
""" + to_bluetooth() + """wait 1
shot page
press down
wait 0.3
shot controller
press down
wait 0.3
shot headphones
press enter
wait 0.3
shot pairing
wait 1.5
shot paired_hp
press esc
wait 0.5
press enter
wait 1.5
press down
press down
press down
wait 0.3
shot other
press enter
wait 1.5
shot other_said
press esc
press esc
press esc
wait 4
""" + QUIT)
    H.check(r.notes["page"]["ui"] == "bluetooth:0", "the page opens on its first line: %s" % r.notes["page"]["ui"])
    log = [s for _, s in bt_log(r)]
    H.check("search 10 s" in log, "opening the page started a search: %s" % log)
    H.check("connect WH-1000XM4" in log, "enter on the headphones (after the controller, before the phone): %s" % log)
    H.check(any(s.startswith("headphones in WH-1000XM4") for s in log), "they connected: %s" % log)
    H.check("connect Pixel 8" in log, "the phone is listed last; enter on it tries: %s" % log)
    H.check(said(r, "Headphones connected"), "their toast (ui7's words): %s" % osd(r))


TESTS.append(test_page_search_and_pair)


def test_page_le_pad():
    """r19-bt (an Xbox Wireless Controller over Bluetooth LE): an LE controller is listed as a
    controller by its name; enter pairs it (PAIRING, ENTER STOP) and it connects, bonded, as Controller 1 with the
    kind of an Xbox pad on USB (GIP: "Xbox One"); its A reaches the game; the old "LE isn't supported" is gone."""
    r = H.Run("bt_page_le").run(P.START_GAME + """bt dongle in
bt near pad-le -44 Xbox Wireless Controller
wait 2
""" + to_bluetooth() + """wait 1
press down
wait 0.3
shot le
press enter
wait 0.4
shot pairing
wait 1.5
shot connected
press esc
press esc
press esc
wait 2
""" + P.held_peek("Pad_A", "a") + QUIT)
    log = [x for _, x in bt_log(r)]
    said_ = picked_said(r)
    H.check(r.notes["le"]["ui"] == "bluetooth:3", "the LE controller listed under FOUND: %s" % r.notes["le"]["ui"])
    H.check("Xbox Wireless Controller=Controller, keys enter Pair" in said_, "listed as a controller: %s" % said_)
    H.check("Xbox Wireless Controller=Pairing, keys enter Stop" in said_, "pairing: PAIRING, ENTER STOP: %s" % said_)
    H.check(any(x.startswith("le paired Xbox Wireless Controller (bonded), kind 3") for x in log),
            "paired and bonded, kind GIP (Xbox One): %s" % log)
    H.check(any(x.startswith("pad in slot 0 kind 3 Xbox Wireless Controller") for x in log), "in slot 0: %s" % log)
    H.check(said(r, "Controller 1 connected (Bluetooth)"), "its toast, as a Classic pad's: %s" % osd(r))
    H.check(P.p1(r, "a") == "A", "its A reaches the game: %s" % P.p1(r, "a"))
    H.check(not any("isn't supported" in x or "aren't supported" in x for x in said_),
            "no \"LE isn't supported\" any more: %s" % said_)


TESTS.append(test_page_le_pad)


def test_page_states():
    """The page's band says what Bluetooth is like, each line fitting its place (gpsp-host fails a run whose text is
    cut): no dongle yet, its firmware missing, the dongle pulled out, Bluetooth given up."""
    r = H.Run("bt_page_states").run(P.START_GAME + to_bluetooth() + """shot none
press esc
wait 0.3
bt firmware missing
bt dongle in
wait 1.5
press enter
wait 0.5
shot firmware
press esc
wait 0.3
bt dongle out
wait 1
press enter
wait 0.5
shot unplugged
press esc
wait 0.3
bt fail
wait 1
press enter
wait 0.5
shot failed
press esc
press esc
wait 1
""" + QUIT)
    for shot in ("none", "firmware", "unplugged", "failed"):
        H.check(r.notes[shot]["ui"] == "bluetooth:0", "%s: the page: %s" % (shot, r.notes[shot]["ui"]))


TESTS.append(test_page_states)


def test_page_forget_and_off():
    """The paired devices: del asks (No, keep it: kept; Forget: forgotten); the switch: Bluetooth off (the
    radio off), on again (ui18: the page's first tile, left of Search, the list empty after the forget);
    Settings' grid says "Off" / "1 connected"."""
    r = H.Run("bt_page_paired").run(P.START_GAME + """bt dongle in
wait 2
bt pad in dualsense DualSense Wireless Controller
wait 2
""" + to_bluetooth() + """press down
wait 0.3
shot paired
press del
wait 0.5
shot ask
press enter
wait 0.5
press del
wait 0.5
press right
press enter
wait 0.5
shot forgot
press left
wait 0.3
shot switch
press enter
wait 0.5
shot off
press esc
wait 0.5
shot list_off
press enter
wait 0.5
press enter
wait 1
press esc
wait 0.5
shot list_on
press esc
wait 1
""" + QUIT)
    log = [s for _, s in bt_log(r)]
    H.check(r.notes["paired"]["ui"] == "bluetooth:3", "the paired DualSense picked: %s" % r.notes["paired"]["ui"])
    H.check(r.notes["ask"]["ui"].startswith("dialog:"), "del asks first: %s" % r.notes["ask"]["ui"])
    H.check(sum(1 for s in log if s.startswith("forget")) == 1, "No, keep it kept it, Forget forgot it: %s" % log)
    H.check("power off" in log and "power on" in log, "Bluetooth off, then on: %s" % log)
    H.check(r.notes["switch"]["ui"] == "bluetooth:0", "the switch line: %s" % r.notes["switch"]["ui"])


TESTS.append(test_page_forget_and_off)


def test_toast_lengths():
    """Every Bluetooth toast fits the message line (OSD_TEXT_MAX, 63 characters, the pinned game code's), whatever
    the device's name (31 characters, the most a name keeps): each event the library posts, and the controllers'
    and headphones' in ui7's words."""
    long = "W" * 31
    script = P.START_GAME + "bt dongle in\nwait 2\n"
    for ev in ("on", "off", "failed", "unplugged", "pad", "padout", "refused", "headphones", "headphonesout",
               "paired", "failedconnect", "notsupported"):
        script += "bt event %s %s\nwait 1.3\n" % (ev, long)
    r = H.Run("bt_toasts").run(script + "wait 2\n" + QUIT)
    texts = [t for _, t in osd(r)]
    bt = [t for t in texts if "Bluetooth" in t or "Controller" in t or "Headphones" in t or long[:20] in t]
    H.check(len(bt) >= 10, "the events' toasts were shown: %d: %s" % (len(bt), bt))
    H.check(all(len(t) <= 63 for t in texts), "every toast 63 characters at the most: %s" %
            [(len(t), t) for t in texts if len(t) > 63])
    H.check("Controller 1 connected (Bluetooth)" in texts and "Controller 1 disconnected" in texts,
            "a controller's, ui7's words: %s" % bt)


TESTS.append(test_toast_lengths)


def test_settings_persist():
    """Bluetooth off (its page), with Output Bluetooth and BT quality Mono from the settings file (no tiles any more):
    all three in word 22 with its mark; a default file has word 22 at 0 (as older builds write it)."""
    r = bt_run("bt_settings", P.START_GAME + to_bluetooth() + """press enter
wait 0.3
press esc
wait 0.5
press esc
wait 0.5
press esc
wait 1
""" + QUIT, 2, 1)
    words = H.read_words(r.save("gpsp.cfg.tns")) if os.path.exists(r.save("gpsp.cfg.tns")) else []
    H.check(len(words) > 22 and words[22] >> 24 == 0xB7, "word 22 marked: %s" % (hex(words[22]) if len(words) > 22 else words))
    if len(words) > 22:
        H.check(words[22] & 1 == 0, "Bluetooth off kept: %08x" % words[22])
        H.check((words[22] >> 4) & 3 == 2, "Output Bluetooth kept: %08x" % words[22])
        H.check((words[22] >> 8) & 3 == 1, "BT sound Mono kept: %08x" % words[22])
    d = H.Run("bt_settings_default").run(P.START_GAME + "wait 1\n" + QUIT)
    dw = H.read_words(d.save("gpsp.cfg.tns")) if os.path.exists(d.save("gpsp.cfg.tns")) else []
    H.check(len(dw) > 22 and dw[22] == 0, "the defaults: word 22 stays 0: %s" % (hex(dw[22]) if len(dw) > 22 else dw))


TESTS.append(test_settings_persist)


def test_exit():
    """The exit stops Bluetooth within the exit's time (1.5 s at the most)
    and writes gpsp_bt.txt.tns; a stop that leaves a callback inside gpSP:
    the exit's restart, said."""
    r = H.Run("bt_exit").run(P.START_GAME + "bt dongle in\nwait 2\nbt pad in\nwait 2\n" + QUIT)
    line = stop_line(r)
    H.check(line.startswith("stop within") and int(line.split()[2]) <= 1500, "the stop's budget: %s" % line)
    spent = [s for _, s in bt_log(r) if s.startswith("stopped:")]
    H.check(spent and "polite" in spent[-1] and int(spent[-1].split("(")[1].split()[0]) > 0,
            "the stop's polite part (the pad's link) ran, in OS ticks: %s" % spent)
    H.check(os.path.exists(r.save("gpsp_bt.txt.tns")), "gpsp_bt.txt.tns written")
    if os.path.exists(r.save("gpsp_bt.txt.tns")):
        text = open(r.save("gpsp_bt.txt.tns")).read()
        H.check("bt_port: start" in text and "the library" in text, "it holds the port's log and the counters")
    pl = r.save("gpsp_pad.txt.tns")
    if os.path.exists(pl):
        text = open(pl, errors="replace").read()
        H.check("bt: " in text, "the pad log has Bluetooth's lines")
    s = H.Run("bt_exit_stuck").run(P.START_GAME + "bt dongle in\nwait 2\nbt stuck on\n" + QUIT)
    link = s.save("gpsp_link.txt.tns")
    text = open(link, errors="replace").read() if os.path.exists(link) else ""
    H.check("USB shutdown timed out at Bluetooth" in text and said(s, "The calculator restarts now"),
            "a callback left inside gpSP: the exit's restart, at Bluetooth (the link log), said")


TESTS.append(test_exit)


def test_atexit_order():
    """An exit() that didn't come through quit() (the script's quit): Bluetooth's handler runs before the pad
    driver's (its stop while the pad driver and its keep-awake are up), within half of the exit's USB time."""
    r = H.Run("bt_atexit").run(P.START_GAME + "pad port 1\nwait 1\npad in xinput\nwait 3\nbt dongle in\nwait 2\n"
                               "bt pad in dualsense DualSense Wireless Controller\nwait 2\nquit\n")
    lines = [t for _, t in P.pad_log(r)]
    bt = [i for i, t in enumerate(lines) if t.startswith("bt stop within")]
    pad = [i for i, t in enumerate(lines) if t.startswith("shutdown:")]
    H.check(bt and pad and bt[0] < pad[0], "Bluetooth's stop before the pad driver's shutdown: %s" %
            [t for t in lines if t.startswith("bt stop") or t.startswith("shutdown")])
    H.check(bt and int(lines[bt[0]].split()[3]) <= 2000, "within half of the exit's 4 s: %s" % (lines[bt[0]] if bt else ""))


TESTS.append(test_atexit_order)


def test_timed_out_writes_log():
    """A stop that leaves a callback inside gpSP: the exit's restart, and gpsp_bt.txt.tns written on that path
    too; with the timer stopped (an older CX's: the clock moves only in waits) the stop still ends (its waits are
    counted in sleeps too)."""
    for name, opts in (("bt_timed_out", []), ("bt_timed_out_frozen", ["--timer-dead", "1"])):
        r = H.Run(name).run(P.START_GAME + "bt dongle in\nwait 2\nbt stuck on\n" + QUIT, options=opts)
        link = r.save("gpsp_link.txt.tns")
        text = open(link, errors="replace").read() if os.path.exists(link) else ""
        H.check("USB shutdown timed out at Bluetooth" in text, "%s: the exit's restart at Bluetooth" % name)
        H.check(os.path.exists(r.save("gpsp_bt.txt.tns")), "%s: gpsp_bt.txt.tns written on the restart's path" % name)


TESTS.append(test_timed_out_writes_log)


def test_failed_stays_failed():
    """A BTstack assert (the library gives the dongle up): one toast, the dongle no longer a working device of
    gpSP's (the port's restart can act as the replug asked for), and leaving Settings > Sound does not start
    Bluetooth again."""
    r = H.Run("bt_failed").run(P.START_GAME + "bt dongle in\nwait 2\nbt pad in\nwait 2\nbt assert\nwait 3\n"
                               "press menu\nwait 0.5\n" + N.more_tile("save_to") + "wait 0.5\npress esc\n"
                               "wait 0.5\npress esc\nwait 0.5\npress esc\nwait 2\n" + QUIT)
    log = [s for _, s in bt_log(r)]
    H.check(sum(1 for _, t in osd(r) if t.startswith("Bluetooth stopped")) == 1, "one toast: %s" % osd(r))
    H.check(not any(s.startswith("power on") for s in log), "More (Sound delay, Sound output) restarted nothing: %s" % log)
    path = r.save("gpsp_pad.txt.tns")
    text = open(path, errors="replace").read() if os.path.exists(path) else ""
    H.check("a working device of gpSP's: no" in text, "the given-up dongle: no working device (the pad log)")


TESTS.append(test_failed_stays_failed)


def test_fatal_hook():
    """A BTstack assert with no guarded call to return to (bt_port.h's fatal hook, the backstop): the game's save
    and the logs (gpsp_bt.txt.tns too), then the exit's restart, never a hang."""
    r = H.Run("bt_fatal").run(P.START_GAME + "bt dongle in\nwait 2\nbt fatal\nwait 3\n" + QUIT)
    link = r.save("gpsp_link.txt.tns")
    text = open(link, errors="replace").read() if os.path.exists(link) else ""
    H.check("the game's save and the logs, then a restart" in text, "the hook's line in the link log")
    H.check("USB shutdown timed out at Bluetooth (an internal check failed)" in text, "the exit's restart")
    H.check(os.path.exists(r.save("gpsp_bt.txt.tns")), "gpsp_bt.txt.tns written before the restart")


TESTS.append(test_fatal_hook)


def test_output_exclusive():
    """Output = USB adapter with headphones streaming: they are paused (mode 0: the library's cached silence, no
    encoding) while the game runs; Output = Bluetooth: the headphones take it (mode 1). (Set in the settings file: the
    tile is gone.)"""
    def mode_run(name, output):
        r = bt_run(name, P.START_GAME + "bt dongle in\nwait 2\nbt headphones in WH-1000XM4\nwait 3\nnote running\n"
                   "wait 1\nquit\n", output)
        modes = audio_modes(r)
        m = [v for at, v in modes if at < float(r.notes["running"]["time"])]
        return (m[-1] if m else None), modes
    usb, modes = mode_run("bt_output_usb", 1)
    H.check(usb == 0, "Output USB adapter: the headphones paused (mode 0): %s" % modes)
    bt, modes = mode_run("bt_output_bt", 2)
    H.check(bt == 1, "Output Bluetooth: the headphones take it (mode 1): %s" % modes)


TESTS.append(test_output_exclusive)


def test_numbering_by_slot():
    """USB pads keep their slots: with only the second USB slot in use a Bluetooth pad is Controller 3 (toast,
    lights, the Controller page); that USB pad out, it is Controller 1, its lights moved too."""
    r = H.Run("bt_numbering").run(P.START_GAME + """pad port 1
wait 1
pad in xinput
wait 3
pad in xinput
wait 3
pad out 1
wait 3
bt dongle in
wait 2
bt pad in dualsense DualSense Wireless Controller
wait 3
pad out 2
wait 4
""" + QUIT)
    lights = [s for _, s in bt_log(r) if s.startswith("lights")]
    H.check(said(r, "Controller 3 connected (Bluetooth)"), "after USB slot 2: Controller 3: %s" % osd(r))
    H.check("lights slot 0 player 3" in lights, "its lights say 3: %s" % lights)
    H.check(lights and lights[-1] == "lights slot 0 player 1", "the USB pad out: its lights say 1: %s" % lights)


TESTS.append(test_numbering_by_slot)


def test_poll_in_os_time():
    """In a game the dongle's poll (what may block: an HCI command, a step of the firmware's read) runs in the
    frame's OS-time slot, before its wait, not inside its keys' read: the glue's log counts where each ran."""
    r = H.Run("bt_poll_place").run(P.START_GAME + "bt dongle in\nwait 2\nbt pad in\nwait 3\n" + QUIT)
    path = r.save("gpsp_bt.txt.tns")
    text = open(path, errors="replace").read() if os.path.exists(path) else ""
    m = re.search(r"glue polls (\d+) \((\d+) in the frames' OS time\)", text)
    H.check(m is not None, "the glue's poll counts in gpsp_bt.txt.tns")
    if m:
        polls, framed = int(m.group(1)), int(m.group(2))
        # (5 s of game: ~300 frames, one poll each in the slot; the rest are the menus' before the game)
        H.check(framed >= 250, "polls in the frames' OS time: %d of %d" % (framed, polls))


TESTS.append(test_poll_in_os_time)


def test_hung_restarted():
    """bt10, the reference r16 run: a DualSense on Bluetooth after a USB controller, headphones streaming; the port's
    watchdog finds the dongle hung (a power blip, no detach): the Bluetooth pad's slot is freed with its toast
    ("Controller 2 disconnected"), the headphones' too, one "stopped answering: restarting it"; the USB controller
    plays on through it; the restart works: "Bluetooth on" and the pad comes back as Controller 2."""
    r = H.Run("bt_hung").run(P.START_GAME + """pad port 1
wait 1
pad in xinput
wait 4
bt dongle in
wait 2
bt pad in dualsense DualSense Wireless Controller
bt headphones in WH-1000XM4
wait 3
bt hung
wait 2
bt keys 0
""" + P.held_peek("Pad_A", "usb") + """wait 1
bt hung back
wait 2
bt keys 3FFFF
bt pad in dualsense DualSense Wireless Controller
wait 3
""" + QUIT)
    t = [x for _, x in osd(r)]
    H.check("Controller 2 disconnected" in t, "the hung dongle's pad: its slot freed, said: %s" % t)
    H.check(any(x.startswith("Headphones") and "disconnected" in x for x in t), "the headphones gone, said: %s" % t)
    H.check(t.count("Bluetooth dongle stopped answering: restarting it") == 1, "one 'restarting it' toast: %s" % t)
    H.check(all(len(x) <= 63 for x in t), "every toast fits OSD_TEXT_MAX: %s" % [x for x in t if len(x) > 63])
    H.check(P.p1(r, "usb") == "A", "the USB controller plays on while Bluetooth restarts: %s" % P.p1(r, "usb"))
    H.check("Bluetooth on" in t, "the restart worked: said: %s" % t)
    H.check(t.count("Controller 2 connected (Bluetooth)") == 2, "the pad back as Controller 2: %s" % t)
    path = r.save("gpsp_bt.txt.tns")
    text = open(path, errors="replace").read() if os.path.exists(path) else ""
    H.check("watchdog fired 1, restarts that worked 1, now fine" in text, "gpsp_bt.txt.tns counts the watchdog")


TESTS.append(test_hung_restarted)


def test_hung_given_up():
    """The restart does not bring the dongle back (or it hung again): one "replug it" toast even though the library
    says "failed" twice (BTstack off, then the port's verdict), the dongle no longer a working device of gpSP's (the
    port's own restart may act), the USB controller unaffected; unplugged and plugged in again: Bluetooth on."""
    r = H.Run("bt_hung_giveup").run(P.START_GAME + """pad port 1
wait 1
pad in xinput
wait 4
bt dongle in
wait 2
bt pad in switchpro Pro Controller
wait 3
bt hung
bt hung giveup
wait 3
bt keys 0
""" + P.held_peek("Pad_A", "usb") + """bt keys 3FFFF
bt dongle out
wait 2
bt dongle in
wait 3
""" + QUIT)
    t = [x for _, x in osd(r)]
    H.check(t.count("Bluetooth dongle stopped answering: replug it") == 1, "one 'replug it' toast: %s" % t)
    H.check("Bluetooth dongle stopped answering: restarting it" not in t,
            "given up at once: no 'restarting it' (the same words said once): %s" % t)
    H.check("Controller 2 disconnected" in t, "the Switch Pro's slot freed, said: %s" % t)
    H.check(P.p1(r, "usb") == "A", "the USB controller plays on: %s" % P.p1(r, "usb"))
    H.check("Bluetooth on" in t, "replugged: Bluetooth on again: %s" % t)
    path = r.save("gpsp_pad.txt.tns")
    text = open(path, errors="replace").read() if os.path.exists(path) else ""
    H.check("bt: watchdog fired 1" in text, "the pad log's Bluetooth lines count the watchdog")


TESTS.append(test_hung_given_up)


def bt_and_usb(kind, name):
    """One Bluetooth pad kind with a USB controller: the Bluetooth pad first (Controller 1), a USB controller plugged
    in (it is Controller 1, the Bluetooth one Controller 2, its lights say 2), both play (the USB one's A, the
    Bluetooth one's Up); the Bluetooth link dies: its slot freed, the USB one plays on; the USB one unplugged: the
    Bluetooth pad (back) unaffected, Controller 1 again."""
    r = H.Run("bt_usb_" + kind).run(P.START_GAME + """bt dongle in
wait 2
bt pad in %s %s
wait 3
pad port 1
wait 1
pad in xinput
wait 4
bt keys 0 1
""" % (kind, name) + P.held_peek("Pad_A", "both") + """bt pad out 1
wait 2
bt keys 0 1
""" + P.held_peek("Pad_A", "usb_alone") + """bt keys 3FFFF
bt pad in %s %s
wait 3
pad out
wait 4
""" % (kind, name) + P.held_peek("Pad_B", "bt_alone") + QUIT)
    t = [x for _, x in osd(r)]
    lights = [s for _, s in bt_log(r) if s.startswith("lights")]
    who = "%s on Bluetooth with a USB controller" % name
    H.check("Controller 1 connected (Bluetooth)" in t, "%s: alone first, Controller 1: %s" % (who, t))
    H.check("lights slot 0 player 2" in lights, "%s: the USB one in, its lights say 2: %s" % (who, lights))
    H.check(P.p1(r, "both") == "A Up", "%s: both play (USB A, Bluetooth Up): %s" % (who, P.p1(r, "both")))
    H.check("Controller 2 disconnected" in t, "%s: its link dies: Controller 2 freed: %s" % (who, t))
    H.check(P.p1(r, "usb_alone") == "A", "%s: the USB one plays on alone: %s" % (who, P.p1(r, "usb_alone")))
    H.check(lights and lights[-1] == "lights slot 0 player 1",
            "%s: the USB one out: the Bluetooth one is Controller 1 (its lights): %s" % (who, lights))
    H.check(P.p1(r, "bt_alone") == "B", "%s: the Bluetooth one plays after the USB one left: %s" %
            (who, P.p1(r, "bt_alone")))


def test_bt_and_usb_pads():
    bt_and_usb("dualsense", "DualSense Wireless Controller")
    bt_and_usb("ds4", "Wireless Controller")
    bt_and_usb("switchpro", "Pro Controller")


TESTS.append(test_bt_and_usb_pads)


def test_live_log():
    """The Bluetooth log survives a run that never exits: gpsp_bt_live.txt.tns (the port's latest lines and the
    counters) is written while a dongle is in and new lines come; a clean exit removes it; a run that finds one
    (the last run froze) keeps it as gpsp_bt_froze.txt.tns and says so."""
    r = H.Run("bt_live").run(P.START_GAME + "bt dongle in\nwait 2\nbt pad in\nwait 1\nbt hung\nwait 3\n"
                             "bt hung back\nwait 3\n" + QUIT)
    H.check(not os.path.exists(r.save("gpsp_bt_live.txt.tns")), "a clean exit removes the live log")
    writes = re.search(r"live log writes (\d+)", open(r.save("gpsp_bt.txt.tns"), errors="replace").read())
    H.check(writes is not None and int(writes.group(1)) >= 1, "live writes happened: %s" %
            (writes.group(0) if writes else "none counted"))
    # a run that "froze": its live log left behind (the files of the last run are this run's starting files)
    frozen = "gpSP Bluetooth log, live at 152231 ms (write 7; the exit removes this file)\nwatchdog: ...\n"
    r2 = H.Run("bt_live_froze")
    os.makedirs(os.path.dirname(r2.save("gpsp_bt_live.txt.tns")), exist_ok=True)
    with open(r2.save("gpsp_bt_live.txt.tns"), "w") as f:
        f.write(frozen)
    r2.run(P.START_GAME + "bt dongle in\nwait 2\n" + QUIT)
    path = r2.save("gpsp_bt_froze.txt.tns")
    H.check(os.path.exists(path) and "152231" in open(path, errors="replace").read(),
            "the frozen run's live log kept as gpsp_bt_froze.txt.tns")
    H.check(not os.path.exists(r2.save("gpsp_bt_live.txt.tns")), "and this run's own removed at its exit")


TESTS.append(test_live_log)


def test_reoffered():
    """bt10 (hub17's recovery: a hub power-cycled after a brown-out offers the dongle again, no detach for the old
    one): the Bluetooth pad's slot is freed and said, the headphones' too, "restarting the dongle" (no "replug", no
    "unplugged"); Bluetooth on again, the headphones back (the sound theirs again), the pad back as Controller 2
    after the USB one, which played on throughout."""
    r = H.Run("bt_reoffer").run(P.START_GAME + """pad port 1
wait 1
pad in xinput
wait 4
bt dongle in
wait 2
bt pad in ds4 Wireless Controller
bt headphones in WH-1000XM4
wait 3
bt dongle reoffer
wait 0.2
bt keys 0
""" + P.held_peek("Pad_A", "usb") + """wait 3
bt keys 3FFFF
bt pad in ds4 Wireless Controller
wait 3
""" + QUIT)
    t = [x for _, x in osd(r)]
    H.check("Controller 2 disconnected" in t, "re-offered: the pad's slot freed, said: %s" % t)
    H.check("Bluetooth: restarting the dongle" in t, "re-offered: said as a restart: %s" % t)
    H.check(not any("replug" in x or "unplugged" in x for x in t), "re-offered: no replug, no unplugged: %s" % t)
    H.check("Bluetooth on" in t, "re-offered: on again: %s" % t)
    H.check(t.count("Headphones connected") == 2, "re-offered: the headphones back: %s" % t)
    at = [tt for tt, x in bt_log(r) if x == "script: dongle reoffer"]
    H.check(at and any(m == 1 for tt, m in audio_modes(r) if tt > at[0]),
            "re-offered: the sound is the headphones' again: %s" % audio_modes(r)[-3:])
    H.check(t.count("Controller 2 connected (Bluetooth)") == 2, "re-offered: the pad back as Controller 2: %s" % t)
    H.check(P.p1(r, "usb") == "A", "re-offered: the USB controller plays on: %s" % P.p1(r, "usb"))


TESTS.append(test_reoffered)


def test_out_of_memory():
    """bt10 (r17 reserves ~1 MB for saves): Bluetooth short of memory says so on screen. The dongle given up for want
    of its rings: "Bluetooth off: not enough memory (29 KB)", its pad's slot freed, the game on, gpsp_bt.txt.tns
    names the allocation; a replug with memory again: Bluetooth on. The start itself refused (the USB slots): the
    same words over the game, nothing of Bluetooth's polled."""
    r = H.Run("bt_nomem").run(P.START_GAME + """bt dongle in
wait 2
bt pad in dualsense DualSense Wireless Controller
wait 3
bt nomem
wait 3
bt dongle out
wait 1
bt dongle in
wait 3
""" + QUIT)
    t = [x for _, x in osd(r)]
    H.check("Bluetooth off: not enough memory (29 KB)" in t, "no memory for the dongle: said: %s" % t)
    H.check("Controller 1 disconnected" in t, "no memory: the pad's slot freed: %s" % t)
    H.check("Bluetooth on" in t, "no memory: a replug with memory: on again: %s" % t)
    text = open(r.save("gpsp_bt.txt.tns"), errors="replace").read() if os.path.exists(r.save("gpsp_bt.txt.tns")) else ""
    H.check(re.search(r"bt: memory: \d+ KB held now, peak \d+ KB", text) is not None,
            "gpsp_bt.txt.tns: Bluetooth's heap, now and peak")
    r2 = H.Run("bt_nomem_start").run(P.START_GAME + "wait 4\n" + QUIT, env={"GPSP_HOST_BT_NOMEM": "1"})
    t2 = [x for _, x in osd(r2)]
    H.check("Bluetooth off: not enough memory (13 KB)" in t2, "the start refused for memory: said over the game: %s" % t2)
    H.check(all(len(x) <= 63 for x in t + t2), "the toasts fit OSD_TEXT_MAX")


TESTS.append(test_out_of_memory)


def live_writes(run):
    path = run.save("gpsp_bt.txt.tns")
    text = open(path, errors="replace").read() if os.path.exists(path) else ""
    m = re.search(r"live log writes (\d+)", text)
    return int(m.group(1)) if m else -1


def test_live_log_holds():
    """bt10: the live log's writes stall the USB callbacks 100-290 ms (R11t): none while a controller on
    the cable plays, a sound adapter or headphones stream, but for the watchdog and a given-up port (the moments a
    freeze would follow)."""
    usb = P.START_GAME + "pad port 1\nwait 1\npad in xinput\nwait 4\nbt dongle in\nwait 2\nbt pad in\nwait 2\nbt pad out 1\nwait 2\n"
    r = H.Run("bt_live_hold_usb").run(usb + QUIT)
    H.check(live_writes(r) == 0, "a USB controller in play: no live write for the dongle and a pad: %d" % live_writes(r))
    r = H.Run("bt_live_hold_usb_hung").run(usb + "bt hung\nwait 2\n" + QUIT)
    H.check(live_writes(r) >= 1, "... but the watchdog's moment is written: %d" % live_writes(r))
    base = P.START_GAME + "bt dongle in\nwait 2\n"
    pads = "bt pad in\nwait 2\nbt pad out 1\nwait 2\nbt pad in\nwait 2\nbt pad out 1\nwait 2\n"
    hp = H.Run("bt_live_hold_hp").run(base + "bt headphones in WH-1000XM4\nwait 2\n" + pads + QUIT)
    no = H.Run("bt_live_hold_none").run(base + pads + QUIT)
    H.check(0 <= live_writes(hp) and live_writes(no) >= live_writes(hp) + 3,
            "headphones streaming hold the pads' writes: %d with them, %d without" % (live_writes(hp), live_writes(no)))


TESTS.append(test_live_log_holds)


def test_heartbeat_out_of_ring():
    """bt10: the heartbeat (every 60 s while a dongle is in) is the glue's own line, never in the port's log
    ring (a long session's would push the incident's lines out); the files carry the last one."""
    r = H.Run("bt_heartbeat").run(P.START_GAME + "bt dongle in\nwait 130\n" + QUIT)
    H.check(not any("glue: alive" in s for _, s in bt_log(r)), "no heartbeat line in the port's log")
    text = open(r.save("gpsp_bt.txt.tns"), errors="replace").read() if os.path.exists(r.save("gpsp_bt.txt.tns")) else ""
    H.check("glue: alive" in text and "the glue's last heartbeat" in text, "gpsp_bt.txt.tns has the last heartbeat")


TESTS.append(test_heartbeat_out_of_ring)


def test_unplugged_twice():
    """bt10: only the pair BTstack-off-then-the-port's-verdict is said once; the dongle pulled out, back
    ("Bluetooth on"), and out again within 10 s: "dongle unplugged" twice."""
    r = H.Run("bt_unplug_twice").run(P.START_GAME + """bt dongle in
wait 2
bt dongle out
wait 2
bt dongle in
wait 2
bt dongle out
wait 2
""" + QUIT)
    t = [x for _, x in osd(r)]
    H.check(t.count("Bluetooth: dongle unplugged") == 2, "unplugged twice, said twice: %s" % t)


TESTS.append(test_unplugged_twice)


def test_headphones_seconds():
    """bt10: while headphones stream in a game, gpsp_bt.txt.tns gets a line a second (the frame's window, the
    poll, the sound's write, packets, SBC frames, the ring, the rate, late / underruns / trims), the next
    hardware run's account of where the time goes; nothing while none stream."""
    r = H.Run("bt_hp_seconds").run(P.START_GAME + "bt dongle in\nwait 2\nbt headphones in Example Headphones\n"
                                     "wait 4\n" + QUIT)
    path = r.save("gpsp_bt.txt.tns")
    text = open(path, errors="replace").read() if os.path.exists(path) else ""
    lines = [l for l in text.split("\n") if re.match(r"bt \d+s \d+, \d+ polls; window", l)]
    H.check("headphones' seconds" in text, "the seconds' header in gpsp_bt.txt.tns")
    H.check(len(lines) >= 3, "a line a second while they streamed: %s" % lines[:2])
    if lines:
        m = re.match(r"bt \d+s (\d+), (\d+) polls", lines[-1])
        H.check(m and 50 <= int(m.group(1)) <= 62 and int(m.group(2)) >= 50,
                "a second's game frames to them and polls (~60): %s" % lines[-1])
    q = H.Run("bt_hp_none").run(P.START_GAME + "bt dongle in\nwait 2\nbt pad in\nwait 3\n" + QUIT)
    path = q.save("gpsp_bt.txt.tns")
    text = open(path, errors="replace").read() if os.path.exists(path) else ""
    H.check(os.path.exists(path) and "headphones' seconds" not in text, "no headphones: no seconds")


TESTS.append(test_headphones_seconds)


def test_headphones_delay_and_ff_rate():
    """bt10: the delay a USB card beside the headphones is held back by follows what their ring and lead
    measure (averaged), not their targets: a ring that sits at 200 ms after a stall shows as 200 + lead + their
    buffer; their delay report replaces the 150 ms guess. Fast forward's factor (16x) is logged saturated, not
    wrapped round to a negative rate (bt10's int32 product overflowed past ~4.28x)."""
    r = H.Run("bt_hp_delay").run(P.START_GAME + "bt dongle in\nwait 2\nbt headphones in Example Headphones\n"
                                  "bt audio 1.0 70 40\nwait 4\nbt audio 1.0 200 40\nwait 4\n"
                                  "bt audio 1.0 70 40 90\nwait 4\nbt audio 16.0 70 40 90\nwait 3\n"
                                  "bt audio 0.5 70 40 90\nwait 3\n" + QUIT)
    path = r.save("gpsp_bt.txt.tns")
    text = open(path, errors="replace").read() if os.path.exists(path) else ""
    rows = re.findall(r"bt (\d+)s .*; rate ([+-]\d+\.\d+)%; late \d+ under \d+ trims \d+; delay (\d+) ms", text)
    H.check(len(rows) >= 15, "the seconds with the delay: %s" % rows[:3])
    d = [int(x[2]) for x in rows]
    rate = [float(x[1]) for x in rows]
    H.check(any(v == 70 + 40 + 150 for v in d), "their ring 70, lead 40, no report: 260 ms: %s" % d)
    H.check(any(v == 200 + 40 + 150 for v in d), "the ring at 200 after a stall: 390 ms: %s" % d)
    H.check(any(v == 70 + 40 + 90 for v in d), "a report of 90 ms replaces the guess: 200 ms: %s" % d)
    H.check(all(v <= 390 for v in d), "no more than the ring and lead and buffer: %s" % d)
    H.check(any(v == 327.67 for v in rate), "16x: saturated at +327.67%%: %s" % rate)
    H.check(all(v >= -50.0 for v in rate), "never wrapped round below 0.5x's -50%%: %s" % rate)
    H.check(any(v == -50.0 for v in rate), "0.5x: -50.00%%: %s" % rate)


TESTS.append(test_headphones_delay_and_ff_rate)


def test_hub_recovering_quiet():
    """While hub17 restarts the hub (nspire_hub_recovering 1), the dongle given up ("replug it") and the dongle gone
    ("unplugged") are that restart's doing: logged, the page's note kept, no toast; the pad's slot still freed and
    said. Once the hub is back (0), Bluetooth on is said, and a later unplug is said again."""
    r = H.Run("bt_hub_recovering").run(P.START_GAME + """wait 1
bt dongle in
wait 2
bt pad in switchpro Pro Controller
wait 3
bt hub recovering on
bt dongle out
wait 2
bt dongle in
wait 3
bt hung
bt hung giveup
wait 2
bt hub recovering off
bt dongle out
wait 1
bt dongle in
wait 3
bt dongle out
wait 2
""" + QUIT)
    t = [x for _, x in osd(r)]
    H.check(not any("replug" in x for x in t), "hub restarting: no 'replug it' toast: %s" % t)
    H.check(t.count("Bluetooth: dongle unplugged") == 1,
            "hub restarting: its unplug not said, the later one said: %s" % t)
    H.check("Controller 1 disconnected" in t or "Controller 2 disconnected" in t,
            "the pad's slot freed and said all the same: %s" % t)
    H.check(t.count("Bluetooth on") == 2, "back after the restart, and after the replug: Bluetooth on said: %s" % t)
    logged = [x for _, x in bt_log(r) if "not shown: the hub is being restarted" in x]
    H.check(len(logged) == 2 and any("replug it" in x for x in logged) and any("unplugged" in x for x in logged),
            "both held toasts logged: %s" % logged)


TESTS.append(test_hub_recovering_quiet)


# ---- r19-bt: a connect stopped, one attempt at a time, the signal's word -------------------------------------
def page_said(run):
    """nspire_devices.c's words as they changed (host_bt_page_said): (time, "list ..." | "picked ...")."""
    return [(t, x[5:]) for t, x in bt_log(run) if x.startswith("page ")]


def picked_said(run):
    return [x[7:] for _, x in page_said(run) if x.startswith("picked ")]


def lists_said(run):
    return [dict(item.rsplit("/", 1)[0].split("=", 1) for item in x[5:].split("; ") if "=" in item)
            for _, x in page_said(run) if x.startswith("list ")]


def test_page_like_a_phone():
    """r19-bt (a phone's list): every named device is listed with the word
    for what it says it is (TV, PHONE, COMPUTER, ...; none when nothing says); a nameless LE device is not (a crowded
    room's beacons), except one showing the HID service (a pad in pairing mode, by its address, as
    CONTROLLER); paired first, then what gpSP can use, then the rest, each the stronger first; the page's 24
    lines keep the strongest and never drop a paired one; enter on the TV tries it (ENTER PAIR, no refusal)
    and says "Not a controller or headphones." when it is not one."""
    pcs = "".join("bt near computer %d PC %02d\n" % (-45 - k, k) for k in range(24))
    r = H.Run("bt_page_phone").run(P.START_GAME + """bt dongle in
bt paired headphones Example Headphones
bt near unknown-le -40
bt near pad-le -70
bt near tv -41 Example TV Set Model 0001
""" + pcs + """wait 2
""" + to_bluetooth() + """wait 1
shot page
press down
wait 0.3
press down
wait 0.3
press down
wait 0.3
shot tv
press enter
wait 1.5
shot tv_said
press esc
press esc
press esc
wait 1
""" + QUIT)
    lists = lists_said(r)
    full = [l for l in lists if "PC 00" in l]
    H.check(full, "the search's devices listed: %s" % lists[-3:])
    l = full[-1] if full else {}
    names = list(l.keys())
    H.check(l.get("Example Headphones") == "Paired", "the paired headphones kept (not heard): %s" % names)
    H.check(l.get("Example TV Set Model 0001") == "TV", "the TV says TV: %s" % l)
    H.check(l.get("PC 00") == "Computer", "a computer says COMPUTER: %s" % l)
    pads = [n for n, k in l.items() if k == "Controller" and n.count(":") == 5]
    H.check(len(pads) == 1, "the nameless LE pad listed by its address as CONTROLLER: %s" % l)
    H.check(len([n for n in names if n.count(":") == 5]) == 1, "the nameless LE beacon not listed: %s" % names)
    H.check(len(l) == 24 - 4, "24 lines in all (2 tiles, 2 headings, 20 devices): %d" % len(l))
    H.check("PC 16" in l and "PC 17" not in l, "the strongest kept, the weakest left out: %s" % names)
    H.check(names[0] == "Example Headphones" and names[1] == pads[0] if pads else False,
            "paired first, then the controller, then the rest: %s" % names)
    H.check(names[2] == "Example TV Set Model 0001" and names[3] == "PC 00", "the rest, the stronger first: %s" % names)
    said = picked_said(r)
    H.check(said[2].endswith("keys enter Pair") and "Not a controller" not in said[2],
            "a found line picked: ENTER PAIR, no sentence: %s" % said)
    H.check("Example TV Set Model 0001=Pairing, keys enter Stop" in said,
            "enter on the TV: tried (PAIRING, ENTER STOP), not refused: %s" % said)
    log = [x for _, x in bt_log(r)]
    H.check("connect Example TV Set Model 0001" in log, "enter on the TV tries it: %s" % log)
    H.check(any("status Not a controller or headphones." in x for x in said),
            "the band says it in words: %s" % said)


TESTS.append(test_page_like_a_phone)


def test_page_searches_on():
    """r19-bt (a pad put in pairing mode after the page's one search had ended was never heard): the page searches on while it is open, as a phone does, keeping its lines; not after the player
    stopped the search on its tile."""
    r = H.Run("bt_page_on").run(P.START_GAME + """bt dongle in
bt near pad -50 Pro Controller
wait 1
""" + to_bluetooth() + """wait 13
press right
wait 0.3
press enter
wait 14
press esc
press esc
press esc
wait 1
""" + QUIT)
    log = [x for _, x in bt_log(r)]
    starts = [i for i, x in enumerate(log) if x == "search 10 s"]
    stopped = [i for i, x in enumerate(log) if x == "search stopped"]
    H.check(len(starts) >= 2, "a second search by itself after the first: %s" % log)
    H.check(stopped and not [i for i in starts if i > stopped[-1]], "none after the player stopped it: %s" % log)
    lists = lists_said(r)
    H.check(all("Pro Controller" in l for l in lists[1:]), "its lines kept between searches: %s" % lists)


TESTS.append(test_page_searches_on)


def test_switch_waits():
    """r19-bt (the second freeze: the dongle plugged in and Bluetooth switched off and on in its start-up): the
    switch's tile says STARTING while the dongle's start-up runs and STOPPING while the stop catches up (the
    library finishes a start-up before an off), so the player sees why nothing happened yet; and each step of
    the bring-up is in the live log before it runs: a run that never ends cleanly leaves the step it was in."""
    r = H.Run("bt_switch_waits")
    r.start(P.START_GAME + """bt dongle in
wait 1
""" + to_bluetooth() + """wait 0.5
bt starting 1.5
wait 0.5
shot starting
wait 1.5
bt stopping 1.5
press enter
wait 0.5
shot stopping
wait 1.5
press enter
wait 0.5
bt startup
wait 0.5
crash
""").communicate(timeout=120)
    tiles = [x for x in picked_said(r) if x.startswith("Bluetooth=")]
    H.check(any(x.startswith("Bluetooth=Starting") for x in tiles), "STARTING on the tile: %s" % tiles)
    H.check(any(x.startswith("Bluetooth=Stopping") for x in tiles), "STOPPING on the tile: %s" % tiles)
    H.check(any(x.startswith("Bluetooth=On") for x in tiles), "then ON: %s" % tiles)
    path = r.save("gpsp_bt_live.txt.tns")
    text = open(path, errors="replace").read() if os.path.exists(path) else ""
    H.check("bt start: step 4: the firmware upload" in text and text.rstrip().endswith("one a poll)"),
            "the run that did not end: its live log ends in the bring-up's step: %s" % text[-300:])


TESTS.append(test_switch_waits)


def test_restart_ends_connect():
    """A connect ended by the dongle's restart (the watchdog's, or the dongle gone and back): the band says so in
    words that fit, the device's line as it was."""
    r = H.Run("bt_restart_ends").run(P.START_GAME + """bt dongle in
bt near pad -50 Pro Controller
wait 1
""" + to_bluetooth() + """wait 0.5
bt event restarted Pro Controller
wait 0.5
press esc
press esc
press esc
wait 1
""" + QUIT)
    said = picked_said(r)
    H.check(any("status Bluetooth restarted: Pro Controller stopped" in x for x in said),
            "the band says the restart stopped it: %s" % said)


TESTS.append(test_restart_ends_connect)


def test_stop_pairing():
    """Pairing can be stopped with a second enter, if it was started by mistake. A found controller: enter pairs it (its line PAIRING, the band's key ENTER STOP); enter again stops it
    (the band STOPPED., the line CONTROLLER again: found, as before; the library told: nspire_bt_cancel); enter
    once more starts it again, and it connects."""
    r = H.Run("bt_stop_pairing").run(P.START_GAME + """bt dongle in
bt connect hold
bt near pad -50 Pro Controller
wait 2
""" + to_bluetooth() + """wait 1
press down
wait 0.3
press enter
wait 0.5
shot pairing
press enter
wait 0.5
shot stopped
press enter
wait 0.5
bt connect release
wait 1.5
press esc
press esc
press esc
wait 1
""" + QUIT)
    log = [x for _, x in bt_log(r)]
    said = picked_said(r)
    H.check(r.notes["pairing"]["ui"] == "bluetooth:3", "the found controller picked: %s" % r.notes["pairing"]["ui"])
    H.check(log.count("connect Pro Controller") == 2, "enter, stop, enter: two connects asked: %s" % log)
    H.check("cancel Pro Controller (under way)" in log, "enter on it while it pairs: the library's cancel: %s" % log)
    i = said.index("Pro Controller=Pairing, keys enter Stop") if "Pro Controller=Pairing, keys enter Stop" in said else -1
    H.check(i >= 0, "pairing: its line PAIRING, the band's key ENTER STOP: %s" % said)
    H.check(i >= 0 and "Pro Controller=Controller, status Stopped." in said[i:],
            "enter again: STOPPED. on the band, the line found again (CONTROLLER): %s" % said)
    H.check(said and said[-1].startswith("Pro Controller=Connected"), "enter once more: it connects: %s" % said)
    H.check(any(x.startswith("pad in slot 0") for x in log), "the controller came in: %s" % log)
    H.check(not said or not any("No answer" in x or "Couldn't" in x for x in said),
            "a stop is not a failure: nothing else said: %s" % said)


TESTS.append(test_stop_pairing)


def test_stop_connecting_paired():
    """A paired controller (turned off): enter connects it (CONNECTING, the band ENTER STOP DEL FORGET); enter again
    stops it (STOPPED., the line PAIRED again); del on it while it connects still asks to forget it."""
    r = H.Run("bt_stop_connecting").run(P.START_GAME + """bt dongle in
wait 1
bt pad in switchpro Pro Controller
wait 1
bt pad out
bt connect hold
wait 1
""" + to_bluetooth() + """wait 1
press down
wait 0.3
press enter
wait 0.5
shot connecting
press del
wait 0.5
shot ask
press esc
wait 0.5
press enter
wait 0.5
shot stopped
press esc
press esc
press esc
wait 1
""" + QUIT)
    log = [x for _, x in bt_log(r)]
    said = picked_said(r)
    H.check("Pro Controller=Connecting, keys enter Stop del Forget" in said,
            "connecting: CONNECTING, the band ENTER STOP DEL FORGET: %s" % said)
    H.check(r.notes["ask"]["ui"].startswith("dialog:"), "del while it connects: asked first: %s" % r.notes["ask"]["ui"])
    H.check("cancel Pro Controller (under way)" in log and not any(x.startswith("forget") for x in log),
            "kept (No, keep it), then stopped: %s" % log)
    H.check("Pro Controller=Paired, status Stopped." in said, "stopped: PAIRED again, STOPPED.: %s" % said)
    H.check(not any(x.startswith("pad in slot") for x in log[log.index("cancel Pro Controller (under way)"):]),
            "nothing came in after the stop: %s" % log)


TESTS.append(test_stop_connecting_paired)


def test_reconnect_gives_way():
    """One attempt at a time (r19's CX II log: the last headphones' reconnect paged them 15 s of every 20 and the
    controller the player asked for never got a page in time): the headphones' automatic reconnect shows as
    CONNECTING on their line; the player's enter on a controller stops it first (the library cancels its page) and
    the controller connects; the headphones' line is PAIRED again, not held (the reconnect tries again later). Then
    the reconnect under way stopped by the player on the headphones' own line: held (the next reconnect skips them)
    until the player connects them."""
    r = H.Run("bt_reconnect_gives_way").run(P.START_GAME + """bt dongle in
bt paired headphones Example Headphones
bt near pad -50 Pro Controller
wait 1
bt reconnect
wait 1
""" + to_bluetooth() + """wait 1
shot reconnecting
press down
wait 0.3
press down
wait 0.3
press enter
wait 1.5
shot controller_in
bt reconnect
wait 0.5
wait 0.3
shot headphones_reconnecting
press enter
wait 0.5
shot headphones_stopped
bt reconnect
wait 0.5
press enter
wait 1.5
press esc
press esc
press esc
wait 1
""" + QUIT)
    log = [x for _, x in bt_log(r)]
    said = picked_said(r)
    lists = lists_said(r)
    H.check(any(l.get("Example Headphones") == "Connecting" for l in lists),
            "the reconnect under way: the headphones' line CONNECTING: %s" % lists)
    H.check("reconnect Example Headphones: stopped for the player's connect" in log and
            log.index("reconnect Example Headphones: stopped for the player's connect") < log.index("connect Pro Controller"),
            "the player's connect: the reconnect stopped first: %s" % log)
    H.check(any(x.startswith("pad in slot 0") for x in log), "the controller connected: %s" % log)
    after = [l for l in lists if l.get("Pro Controller") == "Connected"]
    H.check(after and after[0].get("Example Headphones") == "Paired",
            "the headphones PAIRED again once the controller is in: %s" % lists)
    H.check("Example Headphones=Connecting, keys enter Stop del Forget" in said,
            "the headphones' reconnect picked: ENTER STOP: %s" % said)
    H.check("cancel Example Headphones (the reconnect)" in log, "enter on it: the reconnect stopped: %s" % log)
    H.check("Example Headphones=Paired, status Stopped." in said, "STOPPED., PAIRED: %s" % said)
    stopped_at = log.index("cancel Example Headphones (the reconnect)")
    H.check("reconnect: nothing to reconnect now" in log[stopped_at:],
            "held: the next reconnect leaves them alone: %s" % log[stopped_at:])
    H.check("connect Example Headphones" in log[stopped_at:] and any(x.startswith("headphones in Example Headphones")
                                                                     for x in log[stopped_at:]),
            "enter on them: asked again (the hold ends) and they connect: %s" % log[stopped_at:])
    H.check(not said or not any("Couldn't" in x for x in said), "no failure said for the stopped reconnect: %s" % said)


TESTS.append(test_reconnect_gives_way)


def test_signal_words():
    """The signal steps (3/4, 1/4, ...) meant nothing to a reader. A signal is said only when it is weak
    (the lowest two of the four steps: under -75 dBm), as WEAK after what the device is; no line draws its "N/4"
    any more (signal -1); a connected device says nothing of the search's old signal."""
    r = H.Run("bt_signal_words").run(P.START_GAME + """bt dongle in
bt near pad -50 Near Pad
bt near pad -75 Edge Pad
bt near pad -76 Far Pad
bt near headphones -84 Far Buds
bt near other -95 Neighbour TV
bt near unknown-le -60 
wait 2
""" + to_bluetooth() + """wait 1.5
press down
wait 0.3
shot list
press esc
press esc
press esc
wait 1
""" + QUIT)
    lists = lists_said(r)
    last = lists[-1] if lists else {}
    H.check(last.get("Near Pad") == "Controller" and last.get("Edge Pad") == "Controller",
            "a normal signal (-50, -75 dBm) says nothing: %s" % last)
    H.check(last.get("Far Pad") == "Controller . Weak" and last.get("Far Buds") == "Headphones . Weak" and
            last.get("Neighbour TV") == "Weak", "a weak one (-76 dBm and under): WEAK after its kind (r19-bt: alone "
            "when nothing says what it is): %s" % last)
    signals = [x for _, x in page_said(r) if x.startswith("list ")]
    H.check(signals and all(item.endswith("/-1") for x in signals for item in x[5:].split("; ")),
            "no line draws N/4 (every signal -1): %s" % signals[-1:])


TESTS.append(test_signal_words)


def test_le_drop_back_forget():
    """An LE controller's link lost: its toast, its slot freed; it advertises again (turned on): the library's
    whitelist connects it back, its toast again; Forget on it: it advertises, nothing (not bonded any more)."""
    r = H.Run("bt_le_drop_back").run(P.START_GAME + """bt dongle in
bt near pad-le -44 Xbox Wireless Controller
wait 2
""" + to_bluetooth() + """wait 1
press down
wait 0.3
press enter
wait 2
press esc
press esc
press esc
wait 1
bt le drop
wait 3
bt le advertise
wait 3
""" + P.held_peek("Pad_A", "back") + """press menu
wait 0.5
""" + N.settings_tile("bluetooth") + """wait 0.3
press enter
wait 1
press down
wait 0.3
press del
wait 0.5
press right
press enter
wait 0.5
press esc
press esc
press esc
wait 1
bt le advertise
wait 2
""" + QUIT)
    log = [x for _, x in bt_log(r)]
    toasts = [x for _, x in osd(r)]
    H.check(toasts.count("Controller 1 connected (Bluetooth)") == 2, "connected, then back: said twice: %s" % toasts)
    H.check("Controller 1 disconnected" in toasts, "its link lost: said: %s" % toasts)
    H.check("le back Xbox Wireless Controller (the whitelist)" in log, "advertising: connected back: %s" % log)
    H.check(P.p1(r, "back") == "A", "back: its A reaches the game: %s" % P.p1(r, "back"))
    H.check(any(x.startswith("forget Xbox Wireless Controller") for x in log), "Forget asked and done: %s" % log)
    H.check("le advertise Xbox Wireless Controller: not taken (not bonded)" in log,
            "forgotten: its advertising connects nothing: %s" % log)


TESTS.append(test_le_drop_back_forget)


def test_le_two_and_headphones():
    """Two LE controllers (an Xbox pad and an LE gamepad) are Controllers 1 and 2; with headphones in as well, the
    sound goes to the headphones (Output Automatic) and both pads' buttons reach the game."""
    r = H.Run("bt_le_two").run(P.START_GAME + """bt dongle in
bt near pad-le -44 Xbox Wireless Controller
bt near pad-le -52 LE Gamepad
wait 2
""" + to_bluetooth() + """wait 1
press down
wait 0.3
press enter
wait 2
press down
wait 0.3
press enter
wait 2
press esc
press esc
press esc
wait 1
bt headphones in Example Headphones
wait 2
""" + P.held_peek("Pad_B", "b") + QUIT)
    log = [x for _, x in bt_log(r)]
    toasts = [x for _, x in osd(r)]
    H.check(any(x.startswith("pad in slot 0 kind 3 Xbox Wireless Controller") for x in log) and
            any(x.startswith("pad in slot 1 kind 1 LE Gamepad") for x in log), "two LE pads in slots 0 and 1: %s" % log)
    H.check("Controller 1 connected (Bluetooth)" in toasts and "Controller 2 connected (Bluetooth)" in toasts,
            "Controllers 1 and 2: %s" % toasts)
    H.check(any(x.startswith("headphones in Example Headphones") for x in log), "the headphones in too: %s" % log)
    modes = [x for x in log if x.startswith("audio mode")]
    H.check(modes and modes[-1] in ("audio mode 1", "audio mode 0"), "the headphones take the sound: %s" % modes)
    H.check(P.p1(r, "b") == "B", "the pads' B reaches the game with the headphones in: %s" % P.p1(r, "b"))


TESTS.append(test_le_two_and_headphones)


def test_le_not_a_controller():
    """An LE device that is not a controller (a TV). r19-bt (a phone's list): its line is picked like any other (ENTER PAIR, no sentence of its own), enter tries it
    (PAIRING, ENTER STOP) and the band says "Not a controller or headphones."; nothing is paired with."""
    r = H.Run("bt_le_other").run(P.START_GAME + """bt dongle in
bt near other-le -78 Example TV Set Model 0001
wait 2
""" + to_bluetooth() + """wait 1
press down
wait 0.3
press enter
wait 1.5
press esc
press esc
press esc
wait 1
""" + QUIT)
    log = [x for _, x in bt_log(r)]
    said_ = picked_said(r)
    H.check(not any("help " in x for x in said_), "no sentence of its own on its line: %s" % said_)
    H.check("Example TV Set Model 0001=Weak, keys enter Pair" in said_, "ENTER PAIR: %s" % said_)
    H.check(any(x.startswith("Example TV Set Model 0001=Pairing") for x in said_), "enter tries it: %s" % said_)
    H.check(any("status Not a controller or headphones." in x for x in said_),
            "then: not a controller or headphones: %s" % said_)
    H.check(not any(x.startswith("pad in") or x.startswith("le paired") for x in log), "nothing paired: %s" % log)


TESTS.append(test_le_not_a_controller)


def test_le_off_on_this_dongle():
    """A dongle that refuses LE connects: the LE controller's line says, in one sentence, that this dongle can't
    connect Bluetooth LE controllers (and enter says the same); a Classic controller still connects."""
    r = H.Run("bt_le_refused").run(P.START_GAME + """bt dongle in
bt le refuse
bt near pad-le -44 Xbox Wireless Controller
bt near pad -60 Pro Controller
wait 2
""" + to_bluetooth() + """wait 1
press down
wait 0.3
shot le_off
press enter
wait 0.5
press down
wait 0.3
press enter
wait 2
press esc
press esc
press esc
wait 1
""" + QUIT)
    log = [x for _, x in bt_log(r)]
    said_ = picked_said(r)
    H.check(any(x.startswith("Xbox Wireless Controller=Controller, help This dongle can't connect LE controllers.")
                for x in said_), "the LE controller's line: one sentence: %s" % said_)
    H.check(any("status This dongle can't connect LE controllers." in x for x in said_),
            "enter on it: the same: %s" % said_)
    H.check(any(x.startswith("pad in slot 0 kind 6 Pro Controller") for x in log), "a Classic pad still connects: %s" % log)


TESTS.append(test_le_off_on_this_dongle)


def test_le_stop():
    """An LE controller's pairing stopped with enter (ENTER STOP, STOPPED., found again); its advertising then
    connects nothing (never bonded); enter again pairs it."""
    r = H.Run("bt_le_stop").run(P.START_GAME + """bt dongle in
bt connect hold
bt near pad-le -44 Xbox Wireless Controller
wait 2
""" + to_bluetooth() + """wait 1
press down
wait 0.3
press enter
wait 0.5
press enter
wait 0.5
bt le advertise
wait 0.5
bt connect release
press enter
wait 2
press esc
press esc
press esc
wait 1
""" + QUIT)
    log = [x for _, x in bt_log(r)]
    said_ = picked_said(r)
    H.check("Xbox Wireless Controller=Pairing, keys enter Stop" in said_, "pairing: ENTER STOP: %s" % said_)
    H.check("Xbox Wireless Controller=Controller, status Stopped." in said_, "stopped: found again: %s" % said_)
    H.check("le advertise Xbox Wireless Controller: not taken (not bonded)" in log, "not bonded: not taken: %s" % log)
    H.check(any(x.startswith("pad in slot 0 kind 3 Xbox") for x in log), "enter again: it connects: %s" % log)


TESTS.append(test_le_stop)


def main():
    if not os.path.exists(BT_H):
        print("bt tests skipped: no %s (gpsp-host has no Bluetooth)" % os.path.normpath(BT_H))
        return 0
    wanted = sys.argv[1:]
    for t in TESTS:
        if wanted and t.__name__[5:] not in wanted and t.__name__ not in wanted:
            continue
        t()
    print("bt tests: %d checks, %d failed" % (H.checks, len(H.failures)))
    return 1 if H.failures else 0


if __name__ == "__main__":
    sys.exit(main())
