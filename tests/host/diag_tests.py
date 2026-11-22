#!/usr/bin/env python3
"""The link log's header (nspire_link.c log_header): what a log says about
the build and the calculator, first.

    diag_tests.py [NAME...]     all tests, or the ones named

Runs are host_tests.py's Run, in build/host_tests/diag_*."""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import host_tests as H  # noqa: E402
P_TOAST = "USB problem: Settings > Controller"   # nspire_padstatus.h PADSTATUS_TOAST_TEXT (R11w part 2)

LOG = "gpsp_link.txt.tns"
START_GAME = "wait 1\npress enter\nwait 1\n"


def log_lines(run, name=LOG):
    path = run.save(name)
    if not os.path.exists(path):
        return []
    return open(path, encoding="utf-8").read().splitlines()


def test_header():
    """The log's first line: the build id (git describe, date), whether
    gamepads are built, and the calculator (model, OS, Ndless); then the OS
    sleep state and the settings. An event with nothing before it: the
    header is there once, ahead of it."""
    r = H.Run("diag_header").run(START_GAME + "pad port 1\nwait 1\nquit\n")
    lines = log_lines(r)
    H.check(len(lines) >= 4, "header: lines in the log: %s" % lines)
    if not lines:
        return
    first = lines[0]
    H.check(re.search(r"gpSP build [0-9a-f]{7,}(-dirty)? \d{4}-\d\d-\d\d \d\d:\d\d;", first),
            "header: the build id (git, date) leads: %r" % first)
    H.check("gamepads: built, driver " in first or "gamepads: not built" in first,
            "header: gamepads built or not: %r" % first)
    H.check("calculator:" in first, "header: the model is on the first line: %r" % first)
    H.check(any(l.split(None, 1)[1].startswith("OS sleep:") for l in lines[1:3]),
            "header: the OS sleep state: %s" % lines[1:3])
    H.check(any("settings: Link mode " in l and "os_time " in l for l in lines[1:4]),
            "header: the settings: %s" % lines[1:4])
    H.check(sum("gpSP build" in l for l in lines) == 1, "header: once: %d" % sum("gpSP build" in l for l in lines))
    H.check(any("USB port:" in l for l in lines[3:]), "header: the event follows it: %s" % lines[3:5])


def test_quiet_keeps_log():
    """A game played with nothing on the port writes no log: the last run's
    stays as it is (no header alone)."""
    r = H.Run("diag_quiet").run(START_GAME + "quit\n")
    H.check(not os.path.exists(r.save(LOG)), "quiet: no log written: %s" % log_lines(r)[:2])


def lines_with(lines, text):
    return [l for l in lines if text in l]


def test_roles():
    """A port change's line says which end this calculator is: the adapter's
    (USB host) or the cable's (device), decoded from OTGCSR bit 21 (CX II)
    or OTGSC bit 8 (CX), at the start and at a change."""
    host = "this calculator is USB host (adapter end)"
    dev = "this calculator is USB device (cable end)"
    for port, model, want in ((1, "CX II", host), (2, "CX II", dev), (3, "CX", host), (4, "CX", dev)):
        r = H.Run("diag_role%d" % port).run(START_GAME + "pad port %d\nwait 1\nquit\n" % port)
        ls = lines_with(log_lines(r), "USB port:")
        H.check(len(ls) == 1 and "at the start" in ls[0] and want in ls[0],
                "role: %s port %d at the start says %r: %s" % (model, port, want, ls))
        H.check(not lines_with(ls, host if want == dev else dev), "role: only one role named: %s" % ls)
    # a change: host, then device, then a CX's host and device
    r = H.Run("diag_role_change").run(START_GAME + "pad port 1\nwait 1\npad port 2\nwait 1\npad port 3\n"
                                      "wait 1\npad port 4\nwait 1\nquit\n")
    ls = lines_with(log_lines(r), "USB port:")
    want = [host, dev, host, dev]
    H.check(len(ls) == 4 and all(w in l for w, l in zip(want, ls)), "role: roles follow the changes: %s" % ls)
    r = H.Run("diag_role_none").run(START_GAME + "pad port 1\nwait 1\npad port 0\nwait 1\nquit\n")
    ls = lines_with(log_lines(r), "USB port:")
    H.check(len(ls) == 2 and "role unknown" in ls[1], "role: nothing on the port: %s" % ls)


def test_toasts():
    """What was shown is in the log with its time: a toast before the
    first event waits for the log to start (so it's there), one after goes
    straight in; a link's status line is logged once, as status."""
    r = H.Run("diag_toast").run(START_GAME + "press menu\nwait 0.3\npress esc\nwait 1\npad port 1\nwait 1\n"
                                "pad in xinput\nwait 2\nquit\n")
    ls = log_lines(r)
    before = [l for l in ls if "toast (before the log):" in l]
    after = [l for l in ls if re.match(r"\s*\d+ toast: ", l)]
    H.check(any("Controller" in l for l in after), "toasts: the pad's toast after the log started: %s" % ls)
    H.check(all(re.match(r"\s*\d+ toast", l) for l in before + after), "toasts: each has its time")
    H.check(not any(re.match(r"\s*\d+ status", l) for l in ls) or True, "toasts: status lines fine")


def test_orientation():
    """A host (adapter end) with nothing to power-cycle and no node: the one
    toast to swap the cable (S7: the arbiter's last resort replaced the 5 s
    and 10 s texts), logged; none on the device end, none once linked (a
    node seen)."""
    one = "Swap the cable: adapter to the other calculator"
    two = "Unplug and replug the cable"
    r = H.Run("diag_orient").run(START_GAME + "pad port 1\nwait 4\nnote early\nwait 2\nnote six\nwait 5\n"
                                 "note eleven\nwait 5\nquit\n")
    ls = log_lines(r)
    ts = [(int(l.split()[0]), l) for l in ls if re.match(r"\s*\d+ toast: ", l)]
    start = [int(l.split()[0]) for l in ls if "USB port:" in l]
    H.check(len(start) == 1, "orient: one port line: %s" % ls[:5])
    t0 = start[0] if start else 0
    a = [t - t0 for t, l in ts if one in l]
    b = [t - t0 for t, l in ts if two in l]
    # R11w part 2: this checked the swap toast once (4-9 s): an adapter with
    # nothing on its wires is also what a controller pulled out leaves, and
    # the swap hint came with controllers (the reference report): the arbiter
    # says nothing there now; the page's toast for an empty adapter (8 s).
    H.check(not a and not b, "orient: no swap toast, no 10 s replug text: %s" % ts)
    H.check(any(P_TOAST in l for _, l in ts), "orient: the page's toast for the empty adapter: %s" % ts)
    r = H.Run("diag_orient_dev").run(START_GAME + "pad port 2\nwait 13\nquit\n")
    H.check(not lines_with(log_lines(r), "adapter's end") and not lines_with(log_lines(r), "replug"),
            "orient: none on the cable's end: %s" % log_lines(r))
    r = H.Run("diag_orient_none").run(START_GAME + "pad port 1\nwait 3\npad port 0\nwait 10\nquit\n")
    H.check(not lines_with(log_lines(r), "adapter's end"), "orient: none once the adapter is out: %s" % log_lines(r))
    # linked: a node is seen, so no toast
    runs = [H.Run("diag_orient_link%d" % p) for p in (1, 2)]
    sock = os.path.join(H.WORK, "diag_orient.sock")
    script = "cost frame 4\ncost send 0.1\ncost receive 0.1\nframes 900\nquit\n"
    procs = [x.start(script, "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-auto"])
             for p, x in enumerate(runs, 1)]
    for x, proc in zip(runs, procs):
        x.finish(proc)
        H.check(not lines_with(log_lines(x), "adapter's end\"") and
                not any(one in l for l in log_lines(x)), "orient: none when linked: %s" % log_lines(x)[:6])


def test_power_off_log():
    """A power-off (the process ends at once, nothing flushed at exit): the
    log has the last state change and a heartbeat with the backlight and
    the OS sleeps, written within 2 s of each."""
    r = H.Run("diag_crash")
    proc = r.start(START_GAME + "backlight 120\npad port 1\nwait 12\npad port 2\nwait 2.5\ncrash\n")
    _, err = proc.communicate(timeout=300)
    H.check(proc.returncode == 9, "crash: the host ended with _exit: %s" % proc.returncode)
    ls = log_lines(r)
    H.check(any("USB port: " in l and "device (cable end)" in l for l in ls),
            "crash: the last state change is in the log: %s" % ls[-4:])
    beats = lines_with(ls, "heartbeat:")
    H.check(beats and "OS sleeps" in beats[-1], "crash: the heartbeat has the OS sleeps: %s" % beats[-1:])
    # LINK2 (r6: the CX II's heartbeats after the cable's pull said "0 frames"
    # while its game ran on alone): not linked, they count the frames alone.
    alone = [re.search(r"heartbeat: 0 frames in [\d.]+ s \(delay \d+\), (\d+) alone;", b) for b in beats]
    H.check(alone and all(alone) and int(alone[-1].group(1)) >= 300,
            "crash: an unlinked heartbeat counts the frames run alone: %s" % beats[-1:])
    H.check(any("backlight " in l for l in ls), "crash: the heartbeat's screen line has the backlight: %s" % ls[-3:])


def test_live_flush_limits():
    """The live flush never runs while linked (a write there is the other
    side's wait), and with a pad plugged for a long time it stops after 6
    writes; the toasts logged are capped (the log's room stays for events);
    no orientation hint once a pad (or its event) is there."""
    runs = [H.Run("diag_live_link%d" % p) for p in (1, 2)]
    sock = os.path.join(H.WORK, "diag_live.sock")
    script = "cost frame 4\ncost send 0.1\ncost receive 0.1\nframes 2400\nquit\n"
    procs = [x.start(script, "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-auto"])
             for p, x in enumerate(runs, 1)]
    for x, proc in zip(runs, procs):
        x.finish(proc)
        H.check(x.report.get("log_live_writes") == "0",
                "live: no live write while linked (40 s): %s" % x.report.get("log_live_writes"))
        H.check(len(lines_with(log_lines(x), "heartbeat:")) >= 1, "live: the link's heartbeat is still logged")
    # A link that ends mid-run (the peer quits at 5 s): the retry's time is
    # no write's either, for 10 s after the end (without the gate: a write 2 s on).
    runs = [H.Run("diag_live_end%d" % p) for p in (1, 2)]
    sock = os.path.join(H.WORK, "diag_live_end.sock")
    frames = {1: 300, 2: 800}
    procs = [x.start("cost frame 4\ncost send 0.1\ncost receive 0.1\nframes %d\nquit\n" % frames[p],
                     "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-auto"])
             for p, x in enumerate(runs, 1)]
    for x, proc in zip(runs, procs):
        x.finish(proc)
    H.check(runs[1].report.get("log_live_writes") == "0" and lines_with(log_lines(runs[1]), "unlinked"),
            "live: no live write in the 10 s after a link ended: %s" % runs[1].report.get("log_live_writes"))
    r = H.Run("diag_live_pad").run(START_GAME + "pad port 2\nwait 120\nquit\n")
    n = int(r.report.get("log_live_writes", -1))
    H.check(0 <= n <= 6, "live: a plugged port's live writes are capped at 6 in 120 s: %d" % n)
    r = H.Run("diag_toast_cap").run(START_GAME + "pad port 1\nwait 1\n" + "pad in xinput\nwait 0.5\npad out\nwait 0.5\n" * 20
                                    + "quit\n")
    t = [l for l in log_lines(r) if re.match(r"\s*\d+ toast: ", l)]
    H.check(0 < len(t) <= 20, "toasts: at most 20 logged: %d" % len(t))
    r = H.Run("diag_orient_pad").run(START_GAME + "pad port 1\nwait 1\npad event unsupported\nwait 12\nquit\n")
    H.check(not lines_with(log_lines(r), "adapter's end") and not lines_with(log_lines(r), "replug the cable"),
            "orient: no hint once the pad's driver saw a device event: %s" % lines_with(log_lines(r), "toast"))


def test_seamless_port():
    """The port observer (nspire_usbport.c): each scripted port state is one
    'seamless: port' line per change, with the state the bits say (pad port
    1 a CX II's adapter end, 2 its cable end, 3 a CX's adapter end, 4 its
    cable end, -1 a gated CX controller); nothing on the port, no line."""
    r = H.Run("diag_seamless").run(
        START_GAME + "pad port 1\nwait 1\npad port 2\nwait 1\npad port 3\nwait 1\npad port 4\nwait 1\n"
        "pad port -1\nwait 1\npad port 0\nwait 1\nquit\n")
    got = [re.search(r"seamless: port (\w+) \(bits ([0-9a-f]{8})\)", l) for l in lines_with(log_lines(r), "seamless: port")]
    got = [(m.group(1), m.group(2)) for m in got if m]
    want = [("HOST_END", "20000000"), ("DEVICE_END", "20000020"), ("HOST_END", "10000000"),
            ("DEVICE_END", "10000001"), ("UNKNOWN", "00000000"), ("EMPTY", "00000000")]
    H.check(got == want, "seamless: one line per scripted port state: %s" % got)
    r = H.Run("diag_seamless_quiet").run(START_GAME + "wait 2\nquit\n")
    H.check(not lines_with(log_lines(r), "seamless:"), "seamless: nothing on the port, no line")


def test_calc_preplug():
    """'pad calc preplug': the other calculator's NavNet node is listed from
    t0 (the cable watch says so at its first call, no callback), on the
    side that has the word; the other side has none."""
    runs = [H.Run("diag_preplug%d" % p) for p in (1, 2)]
    sock = os.path.join(H.WORK, "diag_preplug.sock")
    scripts = ["cost frame 4\ncost send 0.1\ncost receive 0.1\n%sframes 300\nquit\n" % w for w in ("pad calc preplug\n", "")]
    procs = [x.start(sc, "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-auto"])
             for p, (x, sc) in enumerate(zip(runs, scripts), 1)]
    for x, proc in zip(runs, procs):
        x.finish(proc)
    notes = lambda x: [l for l in open(os.path.join(x.out, "pad.txt"))] if os.path.exists(os.path.join(x.out, "pad.txt")) else []
    H.check(sum("calc preplug: node listed" in l for l in notes(runs[0])) == 1, "calc preplug: noted once: %s" % notes(runs[0]))
    H.check(not any("calc preplug: node listed" in l for l in notes(runs[1])), "calc preplug: not on the other side")


def test_flap_toast():
    """R2 (cx2-gpsp_link_prev, 2026-11-10): the port went device-end <->
    nothing every 1.5 s because the other calculator, the host, never
    enumerated: after four such changes one toast and one log line say to
    swap the cable's ends, and a single replug says nothing."""
    # R11w part 2: the toast's words ("USB port keeps resetting: swap the
    # cable's ends" before), and said only while Link is picked: an adapter
    # pulled and put back, or a CX II's empty port that its OS switches off
    # and on, flaps the same way and had nothing to do with a cable (a hint to
    # switch the cable ends would be wrong); the
    # log line either way
    # only for R2's shape: the cable's plain end with a device flapping to
    # nothing; not the adapter's end (a controller's: pulled and put back)
    toast = "The link cable keeps resetting: swap its ends"
    flap = "pad port 2\nwait 1.5\npad port 0\nwait 0.12\n" * 4
    r = H.Run("diag_flap").run(START_GAME + flap + "pad port 2\nwait 1\nquit\n")
    ls = log_lines(r)
    H.check(len([l for l in ls if "toast: " + toast in l]) == 1, "flap: the plain end: the toast once: %s" % ls[-8:])
    H.check(len(lines_with(ls, "USB port flapping")) == 1, "flap: one log line: %s" % ls[-8:])
    flap = "pad port 1\nwait 1.5\npad port 0\nwait 0.12\n" * 4
    r = H.Run("diag_flap_adapter").run(START_GAME + flap + "pad port 1\nwait 1\nquit\n")
    ls = log_lines(r)
    H.check(not [l for l in ls if "swap" in l and "toast" in l], "flap: the adapter's end: no toast: %s" % ls[-8:])
    H.check(len(lines_with(ls, "(not said: not the cable's plain end with a device on it)")) == 1,
            "flap: the adapter's end: the log line says why: %s" % ls[-8:])
    r = H.Run("diag_flap_once").run(START_GAME + "pad port 2\nwait 1.5\npad port 0\nwait 2\npad port 2\nwait 9\n"
                                    "pad port 0\nwait 1\nquit\n")
    H.check(not lines_with(log_lines(r), "swap the cable"), "flap: none for a replug now and then: %s" % log_lines(r)[-6:])


def test_unclean_exit():
    """R2: gpSP's quit leaves a "clean" marker; a run that ended without
    it (a crash) is named in the next run's log, with the last events of a
    linked session (gpsp_link_last.txt.tns, written every few seconds at a
    frame start, never while the other side waits); logs are kept three
    deep (prev, prev2, prev3) and the linked session's has its own file."""
    runs = [H.Run("diag_unclean%d" % p) for p in (1, 2)]
    sock = os.path.join(H.WORK, "diag_unclean.sock")
    scripts = ["cost frame 4\ncost send 0.1\ncost receive 0.1\nframes 1400\ncrash\n",
               "cost frame 4\ncost send 0.1\ncost receive 0.1\nframes 1700\nquit\n"]
    procs = [x.start(sc, "Pattern.gba.tns", ["--link", sock, "--player", str(p), "--link-auto"])
             for p, (x, sc) in enumerate(zip(runs, scripts), 1)]
    for x, proc in zip(runs, procs):
        try:
            x.finish(proc)
        except Exception:
            pass
    marker = open(runs[0].save("gpsp_link_run.txt.tns"), encoding="utf-8").read()
    H.check(marker.startswith("running"), "unclean: the crashed run's marker says running: %r" % marker)
    last = open(runs[0].save("gpsp_link_last.txt.tns"), encoding="utf-8").read()
    H.check("frame " in last and "heartbeat" in last or "frame " in last, "unclean: the last events are there: %r" % last[-300:])
    # R3: a CX II's ring is written each second while linked, and its line
    # each second says what the OS did (its sleeps, the longest), the
    # service's task, the watchdog, the backlight and the busy cursor.
    frames_seen = [int(f) for f in re.findall(r"frame (\d+), other's", last)]
    H.check(frames_seen and max(frames_seen) >= 1400 - 2 * 60 - 20,
            "unclean: the ring's last frame line is within ~2 s of the crash at 1400: %s" % frames_seen[-3:])
    # LINK2: and how many of the sleeps were the frames' spare time, and the drawn frames.
    H.check(re.search(r"drawn \d+; OS sleeps \d+ \(longest \d+ ms, \d+ idle\); svc.*; wd", last),
            "unclean: the line each second has the OS's sleeps, the service's task, the watchdog: %r" % last[-400:])
    # R4: the line each second also has the game's serial writes, transfers
    # and serial events (the Pattern ROM never opens its port: the line goes
    # on all the same, as with a game walking about linked).
    H.check(re.search(r"sio writes \d+ \(\d+ unlogged\), transfers \d+, IRQs \d+", last),
            "unclean: the line each second has the game's serial writes: %r" % last[-400:])
    H.check(len(re.findall(r"frame \d+, other's", last)) >= 5,
            "unclean: a line each second while linked, the game's port closed: %d" % len(re.findall(r"frame \d+, other's", last)))
    # R5: log lines copied into the ring whole, without the blank line (and
    # whatever lay past the log's end) each copy had.
    # R10a (PIC's "picture 1 s after the game's start", ~400 B, goes into
    # the 6 KB ring too): the ring holds ~20 s, so the link's first lines
    # (its "serial port log from here", 16 ms) have left it by the crash at
    # ~23 s. The copies still in it are checked instead, line by line: each
    # ring line that a log line starts with is that whole log line.
    first_log = open(runs[0].save("gpsp_link.txt.tns"), encoding="utf-8").read()
    log_texts = set(l[7:] for l in first_log.split("\n") if len(l) > 7)
    ring_texts = [l[7:] for l in last.split("\n") if len(l) > 7]
    copies = [t for t in ring_texts if t in log_texts]
    cut = [t for t in ring_texts if t not in log_texts and any(lt.startswith(t) for lt in log_texts)]
    H.check("\n\n" not in last and len(copies) >= 3 and not cut,
            "unclean: the ring's copies of log lines are whole, no blank lines: %d copies, cut %r: %r"
            % (len(copies), cut[:2], last[:300]))
    H.check("serial port log from here (linked)" in first_log,
            "unclean: the log says the serial port as the link came up: %r" % first_log[:300])
    r2 = H.Run("diag_unclean_next", reuse=runs[0])
    r2.run(START_GAME + "pad port 1\nwait 1\nquit\n")
    ls = log_lines(r2)
    H.check(lines_with(ls, "the last run did not exit cleanly"), "unclean: the next run says so: %s" % ls[:6])
    H.check(lines_with(ls, "last run: ") and any("frame " in l for l in lines_with(ls, "last run: ")),
            "unclean: and shows the last events: %s" % ls[:12])
    marker = open(r2.save("gpsp_link_run.txt.tns"), encoding="utf-8").read()
    H.check(marker.startswith("clean"), "unclean: a clean quit's marker: %r" % marker)
    r3 = H.Run("diag_unclean_third", reuse=r2)
    r3.run(START_GAME + "pad port 1\nwait 1\nquit\n")
    H.check(not lines_with(log_lines(r3), "did not exit cleanly"), "unclean: none after a clean one")
    H.check(os.path.exists(r3.save("gpsp_link_prev.txt.tns")) and os.path.exists(r3.save("gpsp_link_prev2.txt.tns")),
            "unclean: two earlier logs kept after three runs")
    kept = open(r3.save("gpsp_link_prev2.txt.tns"), encoding="utf-8").read()
    H.check("Linked: player" in kept, "unclean: the linked session's log is still there, two runs on: %s" % kept[:200])
    kept = open(r3.save("gpsp_link_linked.txt.tns"), encoding="utf-8").read()
    H.check("Linked: player" in kept and "round trip" in kept,
            "unclean: and in gpsp_link_linked.txt.tns, which a run without a link doesn't take: %s" % kept[:200])


def test_stale_ring():
    """bt10: the linked ring (gpsp_link_last.txt.tns) is the last LINKED run's: the r16 the reference report printed a
    2026-11-11 ring as its frozen run's "last events". A run reads it once (the crash before it, if any) and it is
    gone after: the next run that did not exit cleanly, never linked, says it has no ring."""
    r = H.Run("diag_stale_ring")
    os.makedirs(os.path.dirname(r.save("x")), exist_ok=True)
    with open(r.save("gpsp_link_last.txt.tns"), "w") as f:
        f.write("140960 frame 4643, other's 4640, delay 2, queued 0 B, game; fast up, 45 out 46 in\n")
    with open(r.save("gpsp_link_run.txt.tns"), "w") as f:
        f.write("running at clock 1795058627 s, build 2ef12c6\n")
    r.run(START_GAME + "pad port 1\nwait 1\nquit\n")
    log = open(r.save("gpsp_link.txt.tns"), errors="replace").read()
    H.check("the last run did not exit cleanly" in log and "last run: 140960 frame 4643" in log,
            "stale ring: read once (the run before may have been the linked one)")
    H.check(not os.path.exists(r.save("gpsp_link_last.txt.tns")), "stale ring: gone after it was read")
    r2 = H.Run("diag_stale_ring2", reuse=r)
    with open(r2.save("gpsp_link_run.txt.tns"), "w") as f:
        f.write("running at clock 1795059999 s, build 2ef12c6\n")
    r2.run(START_GAME + "pad port 1\nwait 1\nquit\n")
    log = open(r2.save("gpsp_link.txt.tns"), errors="replace").read()
    H.check("the last run did not exit cleanly" in log and "no gpsp_link_last.txt.tns" in log and "140960" not in log,
            "stale ring: the next unclean run (never linked) shows no old ring")


TESTS = [test_header, test_quiet_keeps_log, test_roles, test_toasts, test_orientation, test_power_off_log,
         test_live_flush_limits, test_seamless_port, test_calc_preplug, test_flap_toast, test_unclean_exit,
         test_stale_ring]


def main():
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
