#!/usr/bin/env python3
"""A USB hub on the calculator's one port (2026-11-16), on the simulated
calculator (build/gpsp-host; host_usbpad.c's "pad hub in|quiet|preplug|out"
and what is plugged into and pulled out of the hub while it stays in: a
controller, a sound adapter, a drive). The port's registers read the same
for as long as the hub is in, so nothing here may come from them:

 - a controller, sound and a drive at once, each coming and going;
 - devices moved from one hub port to another, many times;
 - no power restart of the port while a hub is in (it would cut everything
   on it), no "USB problem", "Unplug the USB device" or "USB device in:
   connecting..." said of a hub or of an emptied hub port;
 - a hub that was in before gpSP started (one power restart, then none);
 - a hub the OS never offers to gpSP's drivers (told by a second device);
 - the hub pulled out with everything on it, then a controller alone.

    hub_tests.py [NAME...]      all tests, or the ones named

The runs are host_tests.py's, in build/host_tests/hub_*."""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import host_tests as H  # noqa: E402
import pad_tests as P  # noqa: E402

PROBLEM = "USB problem: Settings > Controller"
REPLUG = "Unplug the USB device and plug it in again"
STARTING = "Starting the USB device..."
DEVICE_IN = "USB device in: connecting..."


def osd(run):
    """Every message put over the picture: (time, text)."""
    return [(t, text[4:]) for t, text in P.pad_log(run) if text.startswith("osd ")]


def padlog(run):
    """The pad log gpSP wrote (gpsp_pad.txt.tns), as lines."""
    path = run.save("gpsp_pad.txt.tns")
    return open(path, errors="replace").read().splitlines() if os.path.exists(path) else []


def cycles(run):
    """The port's power restarts that did something."""
    return [text for _, text in P.pad_log(run) if text.startswith("reenum cycle")]


def hub_drops(run):
    return [text for _, text in P.pad_log(run) if text.startswith("hub: bus power dropped")]


def nothing_wrong_said(run, name):
    said = [text for _, text in osd(run)]
    for bad in (PROBLEM, REPLUG, STARTING):
        H.check(bad not in said, "%s: \"%s\" is never said: %s" % (name, bad, said))
    H.check(not hub_drops(run), "%s: the hub's power is never cut: %s" % (name, hub_drops(run)))


def statuses(run):
    """The pad log's "status:" lines' verdict names, in order."""
    out = []
    for line in padlog(run):
        _, _, rest = line.partition(" status: ")
        if rest:
            out.append(rest.split(":")[0])
    return out


def test_hub_all_three():
    """A hub, then a sound adapter, a controller and a drive on it at once;
    the controller's buttons reach the game; the controller out and another
    one in; then the hub out with what is left on it."""
    r = H.Run("hub_all_three").run(P.START_GAME + """pad hub in
wait 3
pad in sound
wait 2
pad in xinput
wait 1
pad in drive
wait 1
""" + P.held_peek("Pad_A", "a1") + """pad out
wait 3
pad in dualsense
wait 1
""" + P.held_peek("Pad_B", "b1") + """pad out drive
wait 1
""" + P.held_peek("Pad_A", "a2") + """pad out sound
wait 1
""" + P.held_peek("Pad_A", "a3") + """pad out
wait 4
pad hub out
wait 3
quit
""")
    H.check(P.p1(r, "a1") == "A", "hub, all three: the controller's A with sound and a drive in: %s" % P.p1(r, "a1"))
    H.check(P.p1(r, "b1") == "B", "hub, all three: the second controller's B: %s" % P.p1(r, "b1"))
    H.check(P.p1(r, "a2") == "A", "hub, all three: A after the drive left the hub: %s" % P.p1(r, "a2"))
    H.check(P.p1(r, "a3") == "A", "hub, all three: A after the sound left the hub: %s" % P.p1(r, "a3"))
    said = [text for _, text in P.toasts(r)]
    H.check(said == ["Controller connected (Xbox 360)", "Controller disconnected",
                     "Controller connected (DualSense)", "Controller disconnected"],
            "hub, all three: the controllers' messages, nothing else: %s" % said)
    nothing_wrong_said(r, "hub, all three")
    H.check(not cycles(r), "hub, all three: no power restart: %s" % cycles(r))
    log = padlog(r)
    H.check(any("hub: a USB hub is on the port (214b:7260)" in line for line in log),
            "hub, all three: the pad log says the hub came")
    H.check(any("hub: the USB hub left the port" in line for line in log),
            "hub, all three: the pad log says the hub left")
    H.check(sum("hub: a device left the hub" in line for line in log) >= 2,
            "hub, all three: each controller's leaving is noted")
    st = statuses(r)
    H.check("hub" in st and "working" in st, "hub, all three: the verdicts hub and working: %s" % st)
    for bad in ("not_offered", "not_attached", "empty_adapter", "other", "starting"):
        H.check(bad not in st, "hub, all three: never the verdict %s: %s" % (bad, st))
    holders = [text for _, text in P.pad_log(r) if text.startswith("arbiter: port owner")]
    H.check(any("owner PAD" in o for o in holders) and any("owner OTHER" in o for o in holders),
            "hub, all three: the port's holder follows what is on the hub: %s" % holders)
    P.check_shutdown(r, "hub, all three")


def test_hub_empty():
    """An empty hub for 25 s of a game: nothing is restarted, nothing is said
    but that it came, its verdict is the hub's, and the OS still gets its
    tick (a device plugged in later is the OS's hub driver's to find)."""
    r = H.Run("hub_empty").run(P.START_GAME + "pad hub in\nwait 12\npad stat\nwait 12\npad stat\nwait 1\nquit\n")
    nothing_wrong_said(r, "empty hub")
    H.check(not cycles(r), "empty hub: no power restart in 25 s: %s" % cycles(r))
    said = [text for _, text in osd(r)]
    H.check(said.count(DEVICE_IN) <= 1, "empty hub: \"%s\" at most once: %s" % (DEVICE_IN, said))
    late = [text for t, text in osd(r) if t > 8]
    H.check(not late, "empty hub: nothing said after its first seconds: %s" % late)
    st = statuses(r)
    H.check(st and st[-1] == "hub", "empty hub: the verdict stays the hub's: %s" % st)
    for bad in ("not_offered", "not_attached", "not_a_pad", "starting"):
        H.check(bad not in st[1:], "empty hub: never the verdict %s after it came: %s" % (bad, st))
    stats = [text for _, text in P.pad_log(r) if text.startswith("stat:")]
    sleeps = [int(s.split("sleeps=")[1].split()[0]) for s in stats]
    H.check(len(sleeps) == 2 and sleeps[1] - sleeps[0] >= 12 * 8,
            "empty hub: the OS gets its tick 8 times a second at least while it is in: %s" % sleeps)
    log = padlog(r)
    H.check(not any("no pad came in" in line for line in open(r.save("gpsp_link.txt.tns"), errors="replace")),
            "empty hub: no \"no pad came in\" after 12 s")
    H.check(any("come-up: ended" in line and "it is a USB hub" in line for line in log),
            "empty hub: its come-up ends as a hub's")


def test_hub_port_moves():
    """A controller moved from hub port to hub port eight times, quickly and
    slowly, with the sound adapter staying in: it works after each move and
    nothing else is ever said."""
    moves = ""
    for i in range(8):
        gap = (0.3, 1.5, 0.15, 4)[i % 4]
        moves += "pad out\nwait %s\npad in %s\nwait 0.6\n" % (gap, ("xinput", "dualsense")[i % 2])
        moves += P.held_peek("Pad_A", "move%d" % i)
    r = H.Run("hub_port_moves").run(P.START_GAME + "pad hub in\nwait 2\npad in sound\nwait 1\npad in xinput\nwait 1\n" +
                                    moves + "wait 2\npad out sound\nwait 0.4\npad in sound\nwait 1\n" +
                                    P.held_peek("Pad_A", "after_sound") + "wait 6\nquit\n")
    for i in range(8):
        H.check(P.p1(r, "move%d" % i) == "A", "hub port moves: A after move %d: %s" % (i + 1, P.p1(r, "move%d" % i)))
    H.check(P.p1(r, "after_sound") == "A", "hub port moves: A after the sound adapter moved: %s" %
            P.p1(r, "after_sound"))
    nothing_wrong_said(r, "hub port moves")
    H.check(not cycles(r), "hub port moves: no power restart: %s" % cycles(r))
    said = [text for _, text in P.toasts(r)]
    H.check(all(text.startswith("Controller connected") or text == "Controller disconnected" for text in said),
            "hub port moves: only the controllers' own messages: %s" % said)
    st = statuses(r)
    for bad in ("not_offered", "not_attached", "empty_adapter", "unsupported", "failed"):
        H.check(bad not in st, "hub port moves: never the verdict %s: %s" % (bad, st))


def test_hub_pad_gone():
    """The controller out of a hub that stays in, and 20 s of game after:
    before, what was said of it stayed ("Starting the controller...", then
    "The controller didn't start" and the "USB problem" message), because
    the port's bits never told that it left."""
    r = H.Run("hub_pad_gone").run(P.START_GAME + "pad hub in\nwait 14\npad in xinput\nwait 2\npad out\nwait 20\nquit\n")
    nothing_wrong_said(r, "hub, controller out")
    H.check(not cycles(r), "hub, controller out: no power restart: %s" % cycles(r))
    st = statuses(r)
    H.check(st and st[-1] == "hub", "hub, controller out: the verdict is the hub's again: %s" % st)
    H.check("not_attached" not in st and "starting" not in st[st.index("working"):] if "working" in st else False,
            "hub, controller out: neither \"starting\" nor \"didn't start\" after it left: %s" % st)
    said = [text for _, text in P.toasts(r)]
    H.check(said == ["Controller connected (Xbox 360)", "Controller disconnected"],
            "hub, controller out: its two messages: %s" % said)


def test_hub_no_restart():
    """The window a power restart would have fired in: a controller onto the
    hub at once (the arbiter never looked at the hub alone), then out: the
    port reads enabled and running with nothing offered since (what was said
    of the controller left with it), the very picture of a device the OS
    enumerated before the driver registered, whose cure is a power restart.
    With a hub in it is never made: the sound adapter on the hub plays on."""
    r = H.Run("hub_no_restart").run(P.START_GAME + """pad hub in
pad in xinput
wait 1
pad out
wait 15
pad in sound
wait 1
pad in dualsense
wait 0.5
pad out
wait 15
quit
""")
    H.check(not cycles(r), "hub, no restart: no power restart in 30 s of an emptied hub: %s" % cycles(r))
    H.check(not hub_drops(r), "hub, no restart: the hub's power is never cut: %s" % hub_drops(r))
    link = open(r.save("gpsp_link.txt.tns"), errors="replace").read()
    H.check("a USB hub is in (a restart would cut every device on it)" in link,
            "hub, no restart: the log says why none was made")
    nothing_wrong_said(r, "hub, no restart")


def test_hub_failed_pad():
    """A controller on the hub that the driver gives up on says so (its own
    message, its own verdict), and once it is pulled out of the hub the next
    one is a new device: connected, no leftover."""
    r = H.Run("hub_failed_pad").run(P.START_GAME + """pad hub in
wait 2
pad event unsupported switchpro in_open 81 12
wait 6
pad stat
pad diag 9 3 12
wait 0.3
pad in xinput
wait 1
""" + P.held_peek("Pad_A", "a") + "wait 4\nquit\n")
    said = [text for _, text in osd(r)]
    H.check(any("didn't start" in text for text in said), "hub, failed controller: its own message: %s" % said)
    H.check(PROBLEM not in said and REPLUG not in said, "hub, failed controller: no other message of it: %s" % said)
    H.check(P.p1(r, "a") == "A", "hub, failed controller: the next controller works: %s" % P.p1(r, "a"))
    H.check(not cycles(r) and not hub_drops(r), "hub, failed controller: no power restart: %s" % cycles(r))
    st = statuses(r)
    H.check("unsupported" in st and st[-1] == "working", "hub, failed controller: its verdict, then working: %s" % st)


def test_hub_preplug():
    """A hub with a controller on it, there before gpSP started: the OS
    enumerated both with no driver registered, so one power restart (the
    arbiter's, as for any device there at the start) brings their offers;
    after the hub's offer there is never another."""
    r = H.Run("hub_preplug").run("pad hub preplug\npad preplug xinput\n" + P.START_GAME + "wait 9\n" +
                                 P.held_peek("Pad_A", "a") + "pad out\nwait 12\npad in dualsense\nwait 1\n" +
                                 P.held_peek("Pad_B", "b") + "wait 2\nquit\n")
    H.check(len(cycles(r)) == 1, "hub there at the start: one power restart: %s" % cycles(r))
    H.check(not hub_drops(r), "hub there at the start: none after its offer: %s" % hub_drops(r))
    H.check(P.p1(r, "a") == "A", "hub there at the start: the controller works after it: %s" % P.p1(r, "a"))
    H.check(P.p1(r, "b") == "B", "hub there at the start: the next controller too: %s" % P.p1(r, "b"))
    said = [text for _, text in osd(r)]
    H.check(PROBLEM not in said and REPLUG not in said, "hub there at the start: nothing wrong said: %s" % said)


def test_hub_quiet():
    """A hub the OS keeps to itself (never offered to gpSP's drivers:
    UNVERIFIED which the older CX's OS does): told by a second device coming
    while one is attached, which only a hub allows; from then on as any hub."""
    r = H.Run("hub_quiet").run(P.START_GAME + """pad hub quiet
pad in sound
wait 2
pad in xinput
wait 1
""" + P.held_peek("Pad_A", "a") + """pad out
wait 15
pad out sound
wait 15
quit
""")
    H.check(P.p1(r, "a") == "A", "quiet hub: the controller beside the sound works: %s" % P.p1(r, "a"))
    log = padlog(r)
    H.check(any("hub: a USB hub is on the port" in line for line in log), "quiet hub: told by the second device")
    H.check(not cycles(r), "quiet hub: no power restart once it is known, with both pulled out of it: %s" % cycles(r))
    said = [text for _, text in osd(r)]
    for bad in (PROBLEM, REPLUG, STARTING):
        H.check(bad not in said, "quiet hub: \"%s\" is never said: %s" % (bad, said))


def test_hub_out_then_pad():
    """The hub pulled out with a controller and the sound on it; then a
    controller alone on the adapter: the port is one device's again (its
    come-up, its messages), nothing of the hub is left."""
    r = H.Run("hub_out_then_pad").run(P.START_GAME + """pad hub in
wait 2
pad in sound
wait 1
pad in xinput
wait 2
pad hub out
wait 3
pad in dualsense
wait 2
""" + P.held_peek("Pad_A", "a") + """pad out
wait 3
pad stat
wait 1
quit
""")
    H.check(P.p1(r, "a") == "A", "hub out, then a controller: it works: %s" % P.p1(r, "a"))
    said = [text for _, text in P.toasts(r)]
    H.check(said == ["Controller connected (Xbox 360)", "Controller disconnected",
                     "Controller connected (DualSense)", "Controller disconnected"],
            "hub out, then a controller: the messages: %s" % said)
    log = padlog(r)
    H.check(any("hub: the USB hub left the port" in line for line in log), "hub out, then a controller: the hub left")
    st = statuses(r)
    H.check(st and st[-1] in ("nothing", "empty_adapter"), "hub out, then a controller: the port is empty at the end: %s"
            % st)
    H.check("hub" not in st[len(st) - 1 - st[::-1].index("working"):] if "working" in st else False,
            "hub out, then a controller: no hub verdict after the lone controller: %s" % st)
    H.check(not hub_drops(r), "hub out, then a controller: the hub's power was never cut: %s" % hub_drops(r))


def test_hub_menu():
    """A controller plugged into the hub while the menu is open: the OS gets
    its time there too (the menus' point), and the controller's arrow works
    in the menu."""
    r = H.Run("hub_menu").run(P.START_GAME + """pad hub in
wait 2
press menu
wait 0.5
pad in xinput
wait 1.5
shot menu_pad
pad out
wait 1
pad in xinput
wait 1
press esc
wait 0.5
""" + P.held_peek("Pad_A", "a") + "wait 1\nquit\n")
    H.check(P.p1(r, "a") == "A", "hub, in the menu: the controller plugged in there works in the game: %s" %
            P.p1(r, "a"))
    log = padlog(r)
    H.check(any("hub: the OS ran alone" in line and "the menus waited" in line for line in log),
            "hub, in the menu: the OS's time in one go there")
    nothing_wrong_said(r, "hub, in the menu")


def test_os_stuck():
    """The OS's USB stops (as four hardware sessions ended): from then on
    it resets and enables the port for every device plugged in and never
    offers one. One such plug says nothing (it could be the device); the
    second, after USB had worked this session, is the verdict "os_stuck":
    the page says to take the adapter out and put it back, and quit restarts
    the calculator (it froze after a clean exit in that state)."""
    r = H.Run("os_stuck").run(P.START_GAME + """pad port 1
pad in xinput
wait 2
pad out
wait 2
pad wedge on
pad in xinput
wait 7
pad stat
pad unplug
wait 2
pad in dualsense
wait 7
quit
wait 5
shot after
""")
    st = statuses(r)
    H.check("os_stuck" in st, "OS's USB stopped: the verdict after the second dead plug: %s" % st)
    first = [line for line in padlog(r) if "plug 1 of this kind in a row" in line or "STOPPED" in line]
    H.check(st.count("os_stuck") == 1 and "os_stuck" not in st[:st.index("not_offered") + 1],
            "OS's USB stopped: not after the first one alone: %s" % st)
    H.check(any("the OS's USB has STOPPED" in line for line in padlog(r)), "OS's USB stopped: the pad log says so")
    H.check(r.report.get("restarted") == "1", "OS's USB stopped: quit restarts the calculator: %s" %
            r.report.get("restarted"))
    H.check("after" not in r.notes, "OS's USB stopped: it restarted before the script's end")
    link = open(r.save("gpsp_link.txt.tns"), errors="replace").read()
    H.check("exit: USB shutdown timed out at the OS's USB (it had stopped before the exit)" in link,
            "OS's USB stopped: the link log says why it restarted")
    H.check("exit: the files are written; the USB shutdowns come next" in open(r.save("gpsp_pad.txt.tns"),
                                                                              errors="replace").read(),
            "OS's USB stopped: the files were written before the restart")


def test_os_stuck_not():
    """What must not count as the OS's USB having stopped: one dead plug
    (a device or cable that doesn't answer); two of them when nothing was
    ever offered this session; and two with a working plug between them.
    Each run quits cleanly, no restart. And a stopped USB that comes back
    (the adapter out and in) is no longer one."""
    r = H.Run("os_stuck_one").run(P.START_GAME + "pad port 1\npad in xinput\nwait 2\npad out\nwait 1\npad wedge on\n"
                                  "pad in xinput\nwait 9\npad unplug\nwait 2\nquit\n")
    H.check("os_stuck" not in statuses(r) and r.report.get("restarted") != "1",
            "one dead plug: not the OS's USB: %s" % statuses(r))
    r = H.Run("os_stuck_never").run(P.START_GAME + "pad port 1\npad wedge on\npad in xinput\nwait 7\npad unplug\nwait 2\n"
                                    "pad in dualsense\nwait 7\npad unplug\nwait 1\nquit\n")
    H.check("os_stuck" not in statuses(r) and r.report.get("restarted") != "1",
            "two dead plugs, nothing ever offered: not judged: %s" % statuses(r))
    r = H.Run("os_stuck_between").run(P.START_GAME + "pad port 1\npad in xinput\nwait 2\npad out\nwait 1\npad wedge on\n"
                                      "pad in xinput\nwait 7\npad unplug\nwait 1\npad wedge off\npad in dualsense\n"
                                      "wait 2\npad out\nwait 1\npad wedge on\npad in xinput\nwait 7\npad unplug\n"
                                      "wait 1\nquit\n")
    H.check("os_stuck" not in statuses(r) and r.report.get("restarted") != "1",
            "a working plug between two dead ones: not the OS's USB: %s" % statuses(r))
    r = H.Run("os_stuck_back").run(P.START_GAME + "pad port 1\npad in xinput\nwait 2\npad out\nwait 1\npad wedge on\n"
                                   "pad in xinput\nwait 7\npad unplug\nwait 2\npad in dualsense\nwait 7\n"
                                   "pad stat\npad unplug\nwait 1\npad port 0\nwait 2\npad wedge off\npad port 1\n"
                                   "pad in xinput\nwait 2\n" +
                                   P.held_peek("Pad_A", "a") + "wait 1\nquit\n")
    st = statuses(r)
    H.check("os_stuck" in st and st[-1] == "working", "USB back after the adapter out and in: stopped, then working: %s"
            % st)
    H.check(P.p1(r, "a") == "A" and r.report.get("restarted") != "1",
            "USB back: the controller works and quit is a plain quit: %s" % r.report.get("restarted"))
    # The adapter out ends the verdict (the next plug is judged anew, with
    # the OS's time), but not what quit does: nothing was offered since, so
    # the calculator is still restarted rather than handed back frozen.
    r = H.Run("os_stuck_out").run(P.START_GAME + "pad port 1\npad in xinput\nwait 2\npad out\nwait 1\npad wedge on\n"
                                  "pad in xinput\nwait 7\npad unplug\nwait 2\npad in dualsense\nwait 7\n"
                                  "pad unplug\nwait 1\npad port 0\nwait 3\nquit\nwait 5\nshot after\n")
    st = statuses(r)
    H.check("os_stuck" in st and st[-1] == "nothing", "stopped, then the adapter out: the verdict is the empty port's: %s"
            % st)
    H.check(r.report.get("restarted") == "1", "stopped, the adapter out, nothing offered since: quit still restarts: %s"
            % r.report.get("restarted"))
    # While it is stopped the game keeps its own time: the OS gets no tick
    # for a device it will never offer.
    # (the forced ticks, the ones that cost the game: the same count whether
    # the game goes on 5 s or 25 s after the verdict; the frames' spare time
    # is not counted, it costs nothing)
    forced = []
    for more in (5, 25):
        r = H.Run("os_stuck_time%d" % more).run(
            P.START_GAME + "pad port 1\npad in xinput\nwait 2\npad out\nwait 1\npad wedge on\n"
            "pad in xinput\nwait 7\npad unplug\nwait 2\npad in dualsense\nwait 9\nwait %d\nquit\nwait 5\n" % more)
        link = open(r.save("gpsp_link.txt.tns"), errors="replace").read()
        found = [line for line in link.splitlines() if "forced OS ticks" in line and "exit: shutting down" in line]
        forced.append(int(found[-1].split(" forced OS ticks")[0].split()[-1]) if found else -1)
        H.check("os_stuck" in statuses(r), "stopped (%d s more): the verdict: %s" % (more, statuses(r)))
    H.check(forced[0] >= 0 and 0 <= forced[1] - forced[0] <= 3,
            "stopped: no forced OS tick in 20 s more of game for a device the OS never offers: %s" % forced)


TESTS = [test_os_stuck, test_os_stuck_not, test_hub_all_three, test_hub_empty, test_hub_port_moves, test_hub_pad_gone, test_hub_no_restart,
         test_hub_failed_pad,
         test_hub_preplug, test_hub_quiet, test_hub_out_then_pad, test_hub_menu]


def model(run, prefix):
    """The simulated OS's own lines (host_usbpad.c): "sleep:", "awake:", "phy:"."""
    return [text for _, text in P.pad_log(run) if text.startswith(prefix)]


def test_awake_hub_then_pads():
    """TI's OS as its code reads ("pad sleepy on": what nobody uses is
    suspended 1.4 s later; a suspended hub sees nothing plugged into it; a
    suspended device pulled out leaves the port dead). A hub with nothing on
    it for 6 s, then a controller into it; that one out, 5 s, another one
    in; then the hub out for 4 s and a controller alone. Each works: the
    OS's USB is kept awake from the adapter's first sight to the exit."""
    r = H.Run("awake_hub_then_pads").run(P.START_GAME + """pad sleepy on
pad port 1
wait 2
pad hub in
wait 6
pad in xinput
wait 2
""" + P.held_peek("Pad_A", "a1") + """pad out
wait 5
pad in switchpro
wait 2
""" + P.held_peek("Pad_B", "b1") + """pad out
wait 4
pad hub out
wait 4
pad port 1
pad in dualsense
wait 2
""" + P.held_peek("Pad_A", "a2") + """pad out
wait 2
quit
""")
    name = "awake, hub then controllers"
    H.check(P.p1(r, "a1") == "A", "%s: a controller plugged into a hub that stood empty 6 s works: %s" %
            (name, P.p1(r, "a1")))
    H.check(P.p1(r, "b1") == "B", "%s: the Switch controller 5 s after the first left the hub works: %s" %
            (name, P.p1(r, "b1")))
    H.check(P.p1(r, "a2") == "A", "%s: a controller alone after the hub was pulled works: %s" % (name, P.p1(r, "a2")))
    said = [text for _, text in P.toasts(r)]
    H.check(said == ["Controller connected (Xbox 360)", "Controller disconnected",
                     "Controller connected (Switch Pro)", "Controller disconnected",
                     "Controller connected (DualSense)", "Controller disconnected"],
            "%s: the controllers' messages, nothing else: %s" % (name, said))
    H.check(not model(r, "sleep:"), "%s: the OS never suspends the bus: %s" % (name, model(r, "sleep:")))
    aw = model(r, "awake:")
    H.check(len(aw) == 2 and "changed" in aw[0] and "put back" in aw[1],
            "%s: the OS's code is changed once and put back at the exit: %s" % (name, aw))
    first_hub = min([t for t, text in P.pad_log(r) if text.startswith("hub: offered")] or [1e9])
    first_awake = min([t for t, text in P.pad_log(r) if text.startswith("awake: the OS's code changed")] or [1e9])
    H.check(first_awake < first_hub, "%s: changed before the hub is offered (%.2f < %.2f)" % (name, first_awake, first_hub))
    H.check(not model(r, "phy:"), "%s: the transceiver is never touched: %s" % (name, model(r, "phy:")))
    H.check(not cycles(r), "%s: no power restart: %s" % (name, cycles(r)))
    nothing_wrong_said(r, name)
    log = padlog(r)
    H.check(any("awake: the OS's USB is kept awake for this run" in line for line in log),
            "%s: the pad log says the OS's USB is kept awake" % name)
    P.check_shutdown(r, name)


def test_awake_not_before_adapter():
    """A run with nothing ever plugged in changes nothing of the OS."""
    r = H.Run("awake_no_usb").run(P.START_GAME + "pad sleepy on\nwait 6\nquit\n")
    H.check(not model(r, "awake:"), "no adapter: the OS's code is never touched: %s" % model(r, "awake:"))


def test_awake_direct_swap():
    """A device no driver of gpSP's takes (a keyboard, a controller it does
    not know: here one the driver refuses) directly on the adapter, 4 s,
    pulled; then a controller: it works. TI's OS suspends the first and
    leaves the port dead for the second; kept awake, it does neither."""
    r = H.Run("awake_direct_swap").run(P.START_GAME + """pad sleepy on
pad port 1
wait 2
pad preplug hid
wait 5
pad unplug
wait 2
pad port 1
pad in xinput
wait 2
""" + P.held_peek("Pad_A", "a") + """pad out
wait 2
quit
""")
    name = "awake, a device nobody takes then a controller"
    H.check(P.p1(r, "a") == "A", "%s: the controller works: %s" % (name, P.p1(r, "a")))
    H.check(not model(r, "sleep:"), "%s: the OS never suspends the bus: %s" % (name, model(r, "sleep:")))


def test_asleep_before_start():
    """What the OS did before gpSP ran: a hub and a controller on it were
    plugged in at the home screen, nobody had them, the OS suspended the bus
    (USBCMD 00080000, PORTSC 0000088d, the transceiver asleep). gpSP starts:
    one power restart of the port, the transceiver woken in its drop, the
    hub and the controller offered; the controller works, and the hub is
    never restarted after that."""
    r = H.Run("asleep_before_start").run("""pad port 1
pad hub preplug
pad preplug xinput
pad asleep
""" + P.START_GAME + """wait 8
""" + P.held_peek("Pad_A", "a") + """wait 6
quit
""")
    name = "asleep before the start"
    H.check(P.p1(r, "a") == "A", "%s: the controller on the hub works: %s" % (name, P.p1(r, "a")))
    H.check(len(cycles(r)) == 1, "%s: one power restart: %s" % (name, cycles(r)))
    ph = model(r, "phy:")
    H.check(len(ph) == 1 and "woken" in ph[0], "%s: the transceiver is woken once: %s" % (name, ph))
    H.check(not any("must never" in x or "other bits" in x or "under a device" in x for x in ph),
            "%s: woken with the device off the port, its other bits left: %s" % (name, ph))
    H.check(not hub_drops(r), "%s: the hub at work is never cut: %s" % (name, hub_drops(r)))
    said = [text for _, text in osd(r)]
    H.check(PROBLEM not in said and REPLUG not in said, "%s: no problem said: %s" % (name, said))


def test_asleep_unknown_os():
    """An OS build whose code is not the known one ("pad awake unknown"):
    nothing of it is changed, and it sleeps as TI's does. A hub with nothing
    on it is suspended; the hub is pulled (the transceiver left asleep);
    a controller alone on the adapter: gpSP wakes the transceiver while the
    port is empty, and the controller works."""
    r = H.Run("asleep_unknown_os").run(P.START_GAME + """pad awake unknown
pad sleepy on
pad port 1
wait 2
pad hub in
wait 5
pad hub unplug
wait 3
pad in xinput
wait 3
""" + P.held_peek("Pad_A", "a") + """pad out
wait 2
quit
""")
    name = "an OS that sleeps, hub then a controller"
    aw = model(r, "awake:")
    H.check(aw and all("not the known OS" in x for x in aw), "%s: nothing of the OS is changed: %s" % (name, aw))
    H.check(len(model(r, "sleep: the OS suspended")) >= 1, "%s: the OS suspended the empty hub: %s" %
            (name, model(r, "sleep:")))
    ph = model(r, "phy:")
    H.check(len(ph) >= 1 and "woken" in ph[0] and not any("other bits" in x or "under a device" in x for x in ph),
            "%s: the transceiver is woken once the hub is out: %s" % (name, ph))
    H.check(P.p1(r, "a") == "A", "%s: the controller after it works: %s" % (name, P.p1(r, "a")))
    log = padlog(r)
    H.check(any("the USB transceiver was asleep with nothing on the port" in line for line in log),
            "%s: the pad log says the transceiver was woken" % name)
    H.check(any("awake: not the OS build whose code is known" in line for line in log),
            "%s: the pad log says the OS is not the known one" % name)


def test_asleep_unknown_os_dead_plug():
    """The same OS, and the worst order: the hub is suspended, pulled, and a
    controller plugged in at once, before gpSP looked at the empty port: it
    sits on the sleeping transceiver, connected and never enabled (what the
    the reference log showed for 270 s). gpSP wakes the transceiver and restarts
    the port's power; the controller works. No 5 s standstill on the way."""
    r = H.Run("asleep_dead_plug").run(P.START_GAME + """pad awake unknown
pad sleepy on
pad port 1
wait 2
pad hub in
wait 5
pad hub unplug
pad in xinput
wait 8
""" + P.held_peek("Pad_A", "a") + """pad out
wait 2
quit
""")
    name = "an OS that sleeps, a controller onto the sleeping transceiver"
    H.check(any("never enabled" in x for x in model(r, "sleep:")), "%s: the controller sat dead first: %s" %
            (name, model(r, "sleep:")))
    H.check(any("woken" in x for x in model(r, "phy:")), "%s: the transceiver is woken: %s" % (name, model(r, "phy:")))
    H.check(len(cycles(r)) >= 1, "%s: the port's power is restarted: %s" % (name, cycles(r)))
    H.check(P.p1(r, "a") == "A", "%s: the controller works: %s" % (name, P.p1(r, "a")))
    log = padlog(r)
    alone = [line for line in log if "the OS ran alone" in line]
    longest = max([int(line.split("ran alone ")[1].split(" ms")[0]) for line in alone] or [0])
    H.check(longest < 2500, "%s: the game never stands still 2.5 s for it (longest %d ms)" % (name, longest))
    said = [text for _, text in osd(r)]
    H.check(PROBLEM not in said, "%s: \"%s\" is not said: %s" % (name, PROBLEM, said))


def sound_then_pad(name, extra=""):
    """A hub, a sound adapter streaming on it for 35 s (its sleeps give the OS
    its task time, one every 250 ms at the most; the glue's own hub tick is
    off), then a controller plugged into the hub, which the OS finds in 40
    runs of its USB task. A game with no spare time (in its spare time the
    frames' waits are the OS's anyway)."""
    return H.Run(name).run(P.HEAVY + P.START_GAME + extra + """pad hubslow 40
pad sound serves
pad port 1
wait 1
pad hub in
wait 3
pad in sound
wait 35
pad stat
pad in xinput
wait 3
""" + P.held_peek("Pad_A", "a") + """wait 12
pad stat
quit
""")


def test_hub_pad_while_sound():
    """A controller plugged into the hub while sound streams is found within
    2.5 s: TI's "a hub has news" flag makes the glue give the OS its tick on
    every poll until the OS has found it. Before the plug the sound's sleeps
    are all the OS gets (no hub tick of the glue's: at most ~6 a second)."""
    r = sound_then_pad("hub_pad_while_sound")
    name = "a controller into the hub while sound streams"
    dt = P.plug_to(r, "pad in xinput")
    H.check(dt is not None and dt <= 2.5, "%s: connected %s s after its plug (2.5 s at the most)" % (name, dt))
    H.check(P.p1(r, "a") == "A", "%s: its A reaches the game: %s" % (name, P.p1(r, "a")))
    log = padlog(r)
    H.check(any("hub: a port of the hub changed" in line for line in log), "%s: the pad log says the hub had news" % name)
    stats = [(t, text) for t, text in P.pad_log(r) if text.startswith("stat:")]
    plug = [t for t, text in P.pad_log(r) if text.startswith("pad in sound")]
    quiet = [text for t, text in P.pad_log(r) if text.startswith("pad stat") or text.startswith("stat:")]
    sl = [int(text.split("sleeps=")[1].split()[0]) for _, text in stats]
    H.check(len(sl) == 2 and plug, "%s: the two counts of OS sleeps: %s" % (name, quiet))
    # 35 s of sound before the controller: the first stat is taken right before its plug
    first = [int(text.split("OS sleeps so far ")[1].split(")")[0]) for t, text in P.pad_log(r)
             if text.startswith("pad in sound")]
    if len(sl) == 2 and first:
        rate = (sl[0] - first[0]) / 35.0
        H.check(rate <= 8, "%s: before the plug the OS gets the sound's sleeps only (%.1f a second, 8 at the most)" %
                (name, rate))
    nothing_wrong_said(r, name)
    H.check(not cycles(r), "%s: no power restart: %s" % (name, cycles(r)))


def test_hub_pad_while_sound_no_flag():
    """The same on an OS whose code is not the known one (the flag can't be
    read): the controller is still found, by the sound's sleeps alone, in
    about 10 s. The rule above is what makes the difference."""
    r = sound_then_pad("hub_pad_while_sound_no_flag", "pad explore unknown\n")
    name = "a controller into the hub while sound streams, no flag"
    dt = P.plug_to(r, "pad in xinput")
    H.check(dt is not None and 6.0 <= dt <= 13.0, "%s: connected %s s after its plug (the slow way: 6 to 13 s)" % (name, dt))
    log = padlog(r)
    H.check(not any("hub: a port of the hub changed" in line for line in log), "%s: no news line without the flag" % name)


def test_hub_news_stuck():
    """The OS's flag stays set whatever time the OS gets (its explore task
    does not run): the ticks for it end after 10 s; with the sound's sleeps
    only, the OS is back to ~6 a second."""
    r = H.Run("hub_news_stuck").run(P.HEAVY + P.START_GAME + """pad sound serves
pad port 1
wait 1
pad hub in
wait 3
pad in sound
wait 5
pad explore stuck
wait 13
pad stat
wait 10
pad stat
quit
""")
    name = "the hub's news flag stuck"
    sl = [int(text.split("sleeps=")[1].split()[0]) for _, text in P.pad_log(r) if text.startswith("stat:")]
    H.check(len(sl) == 2 and (sl[1] - sl[0]) / 10.0 <= 8,
            "%s: 13 s on, the OS gets the sound's sleeps only again (%s a second over 10 s, 8 at the most)" %
            (name, (sl[1] - sl[0]) / 10.0 if len(sl) == 2 else sl))
    log = padlog(r)
    H.check(any("flag stays set through the OS's ticks" in line for line in log), "%s: the pad log says so" % name)


def deaf_then_pad(name, extra=""):
    """A controller works on the hub and is pulled; from then on the hub says
    nothing of its ports (the reference r15 run); a PlayStation controller is
    plugged in 3 s later."""
    return H.Run(name).run(P.START_GAME + extra + """pad port 1
wait 1
pad hub in
wait 2
pad in xinput
wait 2
""" + P.held_peek("Pad_A", "a1") + """pad out
wait 3
pad hub deaf
pad in dualsense
wait 7
""" + P.held_peek("Pad_B", "b1") + """pad out
wait 2
quit
""")


def test_hub_deaf_pad():
    """The hub says nothing of a controller plugged in after another was
    pulled: gpSP asks the OS to look at the hub's ports every 2 s, the OS
    finds it, and it works within 5 s of its plug."""
    r = deaf_then_pad("hub_deaf_pad")
    name = "a hub that says nothing"
    H.check(P.p1(r, "a1") == "A", "%s: the first controller works: %s" % (name, P.p1(r, "a1")))
    dt = P.plug_to(r, "pad in dualsense", "Controller connected (DualSense)")
    H.check(dt is not None and dt <= 5.0, "%s: the next controller connected %s s after its plug (5 s at the most)" %
            (name, dt))
    H.check(P.p1(r, "b1") == "B", "%s: its B reaches the game: %s" % (name, P.p1(r, "b1")))
    log = padlog(r)
    H.check(any("hub: its ports as the OS sees them" in line for line in log), "%s: the pad log has the hub's ports" % name)
    H.check(any("look(s) asked of the OS" in line for line in log), "%s: the pad log counts the looks asked" % name)
    nothing_wrong_said(r, name)
    H.check(not cycles(r), "%s: no power restart: %s" % (name, cycles(r)))


def test_hub_deaf_pad_no_look():
    """The same on an OS whose code is not the known one (it can't be asked
    to explore): the controller is never found. The look is what finds it."""
    r = deaf_then_pad("hub_deaf_pad_no_look", "pad explore unknown\n")
    name = "a hub that says nothing, the OS not asked"
    dt = P.plug_to(r, "pad in dualsense", "Controller connected (DualSense)")
    H.check(dt is None, "%s: the controller is not found (%s)" % (name, dt))


def test_hub_idle_looks_cost():
    """An idle hub with sound streaming, a game with no spare time, 30 s: the
    looks asked of the OS add no OS sleeps of gpSP's own (the sound's are
    all there are: 8 a second at the most), and nothing is said."""
    r = H.Run("hub_idle_looks").run(P.HEAVY + P.START_GAME + """pad sound serves
pad port 1
wait 1
pad hub in
wait 3
pad in sound
wait 6
pad stat
wait 30
pad stat
quit
""")
    name = "an idle hub, looked at every 2 s"
    sl = [int(text.split("sleeps=")[1].split()[0]) for _, text in P.pad_log(r) if text.startswith("stat:")]
    H.check(len(sl) == 2 and (sl[1] - sl[0]) / 30.0 <= 8, "%s: %s OS sleeps a second (8 at the most)" %
            (name, (sl[1] - sl[0]) / 30.0 if len(sl) == 2 else sl))
    nothing_wrong_said(r, name)
    late = [text for t, text in osd(r) if t > 12]
    H.check(not late, "%s: nothing said: %s" % (name, late))


def test_hub_port_trouble():
    """A port of the hub that stays connected with no device made of it
    (a controller the hub can't power): said once after 8 s, and the OS's
    extra ticks for it end; the hub's over-current bit: said once."""
    r = H.Run("hub_port_trouble").run(P.HEAVY + P.START_GAME + """pad sound serves
pad port 1
wait 1
pad hub in
wait 3
pad in sound
wait 3
pad hub stuckport on
wait 12
pad stat
wait 10
pad stat
pad hub stuckport off
wait 2
pad hub overcurrent on
wait 3
pad hub overcurrent off
wait 2
quit
""")
    name = "a hub port in trouble"
    said = [text for _, text in osd(r)]
    H.check(said.count("A hub device didn't start: try another port") == 1,
            "%s: the device that does not start is said once: %s" % (name, said))
    H.check(said.count("Hub draws too much current: use a powered hub") == 1,
            "%s: the over-current is said once: %s" % (name, said))
    sl = [int(text.split("sleeps=")[1].split()[0]) for _, text in P.pad_log(r) if text.startswith("stat:")]
    H.check(len(sl) == 2 and (sl[1] - sl[0]) / 10.0 <= 8,
            "%s: once given up, the OS gets the sound's sleeps only again (%s a second, 8 at the most)" %
            (name, (sl[1] - sl[0]) / 10.0 if len(sl) == 2 else sl))
    log = padlog(r)
    H.check(any("A DEVICE ON IT DOES NOT START" in line for line in log), "%s: the pad log says the device does not start"
            % name)
    H.check(any("THE HUB REPORTS TOO MUCH CURRENT" in line for line in log), "%s: the pad log says over-current" % name)


TESTS += [test_hub_pad_while_sound, test_hub_pad_while_sound_no_flag, test_hub_news_stuck]
def pad_with_sound(name, extra=""):
    """A sound adapter streaming and a controller, both on the hub, a game
    with no spare time, 20 s."""
    return H.Run(name).run(P.HEAVY + P.START_GAME + extra + """pad sound serves
pad port 1
wait 1
pad hub in
wait 3
pad in sound
wait 3
pad in xinput
wait 4
pad stat
wait 20
pad stat
""" + P.held_peek("Pad_A", "a") + P.held_peek("Pad_B", "b") + """quit
""")


def test_pad_with_sound_ticks():
    """A controller in while sound streams: its reports come in the sound's
    interrupt windows, so no forced OS tick of its own: at most 8 OS sleeps a
    second (it was ~30), and its buttons still reach the game."""
    r = pad_with_sound("pad_with_sound_ticks")
    name = "a controller in while sound streams"
    sl = [int(text.split("sleeps=")[1].split()[0]) for _, text in P.pad_log(r) if text.startswith("stat:")]
    H.check(len(sl) == 2 and (sl[1] - sl[0]) / 20.0 <= 8, "%s: %s OS sleeps a second (8 at the most)" %
            (name, (sl[1] - sl[0]) / 20.0 if len(sl) == 2 else sl))
    H.check(P.p1(r, "a") == "A" and P.p1(r, "b") == "B", "%s: A and B reach the game: %s %s" %
            (name, P.p1(r, "a"), P.p1(r, "b")))


def test_pad_with_sound_onchange():
    """The same with a controller that reports only what changes: while
    nothing is pressed no report comes, and the pad may get a tick of its
    own twice a second at the most (10 OS sleeps a second in all at the
    most); its buttons reach the game all the same."""
    r = pad_with_sound("pad_with_sound_onchange", "pad reports onchange\n")
    name = "a controller that reports only changes, while sound streams"
    sl = [int(text.split("sleeps=")[1].split()[0]) for _, text in P.pad_log(r) if text.startswith("stat:")]
    rate = (sl[1] - sl[0]) / 20.0 if len(sl) == 2 else None
    H.check(rate is not None and rate <= 10, "%s: %s OS sleeps a second (the sound's 4 and the pad's 2 at the most: "
            "10 at the most)" % (name, rate))
    H.check(P.p1(r, "a") == "A" and P.p1(r, "b") == "B", "%s: A and B reach the game: %s %s" %
            (name, P.p1(r, "a"), P.p1(r, "b")))


def pad_alone(name, extra=""):
    """A controller alone on the adapter, a game with no spare time; its
    reports reach the driver only where the USB interrupt is taken (as on
    the calculator)."""
    return H.Run(name).run(P.HEAVY + P.START_GAME + extra + """pad reports byirq
pad port 1
wait 1
pad in xinput
wait 4
pad stat
wait 20
pad stat
""" + P.held_peek("Pad_A", "a") + P.held_peek("Pad_B", "b") + """quit
""")


def test_pad_windows():
    """A controller alone: its reports come in interrupt windows, one at each
    poll; the OS gets a task sleep every 250 ms (6 OS sleeps a second at the
    most; it was ~30 forced ones), and A and B reach the game."""
    r = pad_alone("pad_windows")
    name = "a controller by interrupt windows"
    sl = [int(text.split("sleeps=")[1].split()[0]) for _, text in P.pad_log(r) if text.startswith("stat:")]
    H.check(len(sl) == 2 and (sl[1] - sl[0]) / 20.0 <= 6, "%s: %s OS sleeps a second (6 at the most)" %
            (name, (sl[1] - sl[0]) / 20.0 if len(sl) == 2 else sl))
    H.check(P.p1(r, "a") == "A" and P.p1(r, "b") == "B", "%s: A and B reach the game: %s %s" %
            (name, P.p1(r, "a"), P.p1(r, "b")))
    log = padlog(r)
    H.check(any("the controller's reports come in interrupt windows" in line for line in log),
            "%s: the pad log says the windows serve it" % name)


def test_pad_windows_not_here():
    """An OS on which a transfer's completion does not run in the interrupt:
    the windows bring no report, three task sleeps do: the pad's tick every
    33 ms is back within 2 s (25 OS sleeps a second at the least), said in
    the pad log, and A and B reach the game."""
    r = pad_alone("pad_windows_not_here", "pad reports nowindows\n")
    name = "a controller where windows bring nothing"
    sl = [int(text.split("sleeps=")[1].split()[0]) for _, text in P.pad_log(r) if text.startswith("stat:")]
    H.check(len(sl) == 2 and (sl[1] - sl[0]) / 20.0 >= 25, "%s: %s OS sleeps a second (25 at the least: the tick)" %
            (name, (sl[1] - sl[0]) / 20.0 if len(sl) == 2 else sl))
    H.check(P.p1(r, "a") == "A" and P.p1(r, "b") == "B", "%s: A and B reach the game: %s %s" %
            (name, P.p1(r, "a"), P.p1(r, "b")))
    log = padlog(r)
    H.check(any("interrupt windows brought no report" in line for line in log), "%s: the pad log says so" % name)


FF_COSTS = "cost frame 5\ncost line 25\ncost turn 4.5\ncost touchpad 0.4\n"   # host_tests' heavy fast-forward game
EVERYTHING = ("pad sound serves\npad reports byirq\npad port 1\nwait 1\npad hub in\nwait 2\npad in sound\nwait 2\n"
              "pad in drive\nwait 2\npad in xinput\nwait 2\n"
              # (2026-11-17, Bluetooth) and the dongle on the hub, a Bluetooth pad and headphones on it
              "bt dongle in\nwait 2\nbt pad in dualsense DualSense Wireless Controller\nwait 1\n"
              "bt headphones in WH-1000XM4\nwait 1\n")


def speed_run(name, plug):
    """10 s of normal play, then 10 s of fast forward (the reference settings):
    OS sleeps, game frames and pictures a second in each."""
    r = H.Run(name, files={"gpsp_qol.cfg.tns": H.owner_config()}).run(
        FF_COSTS + "wait 1\npress enter\nwait 1\n" + plug + "wait 6\npad stat\nnote n_a\nwait 10\npad stat\nnote n_b\n"
        "press F\nwait 2\npad stat\nnote f_a\nwait 10\npad stat\nnote f_b\npress F\nwait 1\nquit\n")
    n = r.notes
    st = [int(x.split("sleeps=")[1].split()[0]) for _, x in P.pad_log(r) if x.startswith("stat:")]
    per = lambda a, b, k: (n[b][k] - n[a][k]) / 10.0
    return {"sleeps_1x": (st[1] - st[0]) / 10.0 if len(st) == 4 else None,
            "sleeps_ff": (st[3] - st[2]) / 10.0 if len(st) == 4 else None,
            "game_1x": per("n_a", "n_b", "frame"), "shown_1x": per("n_a", "n_b", "shown"),
            "game_ff": per("f_a", "f_b", "frame"), "shown_ff": per("f_a", "f_b", "shown")}


def test_everything_plugged_speed():
    """The reference bar (2026-11-17): fast forward with a hub, a controller, a
    USB sound adapter, a drive and a Bluetooth dongle (a Bluetooth pad and
    headphones on it) all in. Against the same game with nothing
    plugged in: in fast forward at most 6 OS sleeps a second of USB's, 95%
    of the game frames, a picture at each refresh; at normal speed 60 and
    60. (With the pad's old tick every 33 ms: 26 sleeps and 76% in the same
    model.)"""
    base = speed_run("speed_nothing", "")
    full = speed_run("speed_everything", EVERYTHING)
    name = "everything plugged in"
    H.check(full["sleeps_ff"] is not None and full["sleeps_ff"] <= 6, "%s, fast forward: %s OS sleeps a second (6 at the "
            "most)" % (name, full["sleeps_ff"]))
    H.check(full["game_ff"] >= 0.95 * base["game_ff"], "%s, fast forward: %.1f game frames a second, %.1f with nothing "
            "plugged in (95%% at the least)" % (name, full["game_ff"], base["game_ff"]))
    H.check(full["shown_ff"] >= 58, "%s, fast forward: %.1f pictures a second (58 at the least)" % (name, full["shown_ff"]))
    H.check(full["game_1x"] >= 59 and full["shown_1x"] >= 58, "%s, normal speed: %.1f game frames, %.1f pictures" %
            (name, full["game_1x"], full["shown_1x"]))
    print("  speed: nothing plugged 1x %.1f/%.1f, ff %.1f/%.1f; everything 1x %.1f/%.1f (%.1f OS sleeps), ff %.1f/%.1f "
          "(%.1f OS sleeps)" % (base["game_1x"], base["shown_1x"], base["game_ff"], base["shown_ff"], full["game_1x"],
                                full["shown_1x"], full["sleeps_1x"], full["game_ff"], full["shown_ff"], full["sleeps_ff"]))


TESTS += [test_pad_with_sound_ticks, test_pad_with_sound_onchange, test_pad_windows, test_pad_windows_not_here,
          test_everything_plugged_speed]
TESTS += [test_hub_deaf_pad, test_hub_deaf_pad_no_look, test_hub_idle_looks_cost, test_hub_port_trouble]
TESTS += [test_awake_hub_then_pads, test_awake_not_before_adapter, test_awake_direct_swap, test_asleep_before_start,
          test_asleep_unknown_os, test_asleep_unknown_os_dead_plug]


# Hub 5 (2026-11-18): the hub gone dead under the OS. The reference r16 run
# (a CX II's pad log of the r16 run): a bus-powered
# hub with a drive, a Switch controller and a sound adapter streaming; the
# port's power fell ~100 ms (153987 -> 154093 ms), the sound stalled, the
# looks asked of the OS were not taken (166214 ... 275606 ms), nothing was
# offered again until the hub was pulled (308672 ms).
LOSTPOWER = "Hub lost power, restarted it. A powered hub helps"
WEDGED = "USB hub stopped answering: restarted it"
STILLDEAD = "USB hub still dead: unplug it and plug it back in"


def hub_said(run):
    return [text for _, text in osd(run) if text in (LOSTPOWER, WEDGED, STILLDEAD)]


def check_probe_gaps(r, name, answer):
    """The hub's probe never held the frame loop: between its start and its
    answer no two polls of the driver were more than 50 ms apart."""
    lines = [text for _, text in P.pad_log(r) if text.startswith("hub: asked whether it answers")]
    H.check(lines and all(answer + " after" in t for t in lines), "%s: the hub's answer %s: %s" % (name, answer, lines))
    gaps = [float(t.split("meanwhile ")[1].split(" ms")[0]) for t in lines]
    H.check(gaps and max(gaps) <= 50, "%s: the frame loop never waited for the probe (longest gap %s ms, 50 at most)" %
            (name, gaps))


def recover_lines(log):
    return [line for line in log if "hub: DEAD" in line or "hub: recovery" in line or "recovery FAILED" in line]


def blip_run(name, extra="", after=""):
    """The r16 log's sequence on the simulated calculator: the hub, a drive,
    a Switch controller and a sound adapter streaming on it; the power falls
    100 ms under it; 20 s on."""
    return H.Run(name).run(P.HEAVY + P.START_GAME + extra + """pad sound serves
pad port 1
wait 1
pad hub in
wait 2
pad in drive
wait 1
pad in switchpro
wait 1
pad in sound
wait 3
""" + P.held_peek("Pad_A", "a0") + """pad hub blip 100
wait 2
""" + P.held_peek("Pad_B", "b_dead") + after + """wait 18
""" + P.held_peek("Pad_A", "a1") + """pad stat
quit
""")


def test_hub_blip_recovers():
    """The power falls under the hub and it goes dead: within 5 s gpSP
    restarts the port's power once, says so in one line, and the hub, the
    controller, the drive and the sound are offered again; the controller's
    buttons reach the game again."""
    r = blip_run("hub_blip_recovers")
    name = "the hub's power fell"
    H.check(P.p1(r, "a0") == "A", "%s: the controller works before: %s" % (name, P.p1(r, "a0")))
    H.check(P.p1(r, "b_dead") == "none", "%s: nothing gets through the dead hub (the model): %s" %
            (name, P.p1(r, "b_dead")))
    H.check(P.p1(r, "a1") == "A", "%s: the controller works again after the recovery: %s" % (name, P.p1(r, "a1")))
    blip = [t for t, text in P.pad_log(r) if text.startswith("pad hub blip")]
    restarts = [t for t, text in P.pad_log(r) if text.startswith("hub: power restarted under the dead hub")]
    H.check(len(restarts) == 1, "%s: one restart of the port's power: %s" % (name, restarts))
    if blip and restarts:
        dt = restarts[0] - blip[0]
        # its 3 s look, then the hub asked: behind the OS's explore request to the dead hub (5 s), its own 1 s limit
        H.check(2.5 <= dt <= 10.0, "%s: the restart %.2f s after the fall (3 s, then the hub's answer awaited)" % (name, dt))
    back = [text for _, text in P.pad_log(r) if text.startswith("hub: back after the restart")]
    H.check(back == ["hub: back after the restart: 3 device(s) on it offered again"],
            "%s: the drive, the sound and the controller offered again: %s" % (name, back))
    H.check(hub_said(r) == [LOSTPOWER], "%s: said once, in one line: %s" % (name, hub_said(r)))
    H.check(len(LOSTPOWER) <= 50, "%s: the toast fits one line (%d characters)" % (name, len(LOSTPOWER)))
    log = padlog(r)
    for want in ("hub: the port's power FELL under the hub", "hub: the port's power is back (seen off for",
                 "hub: DEAD (its power fell and nothing on it answers since): recovery #1",
                 "hub recovery: power restart 10 -> 1", "hub: recovery: the hub is offered again",
                 "device(s) on the hub offered again within"):
        H.check(any(want in line for line in log), "%s: the pad log says \"%s\"" % (name, want))
    offered = [line for line in log if "device(s) on the hub offered again within" in line]
    H.check(offered and int(offered[-1].split("hub: recovery: ")[1].split()[0]) >= 2,
            "%s: the devices' offers counted: %s" % (name, offered))
    H.check(not hub_drops(r), "%s: no other cut of the hub's power: %s" % (name, hub_drops(r)))
    said = [text for _, text in osd(r)]
    H.check(PROBLEM not in said and REPLUG not in said, "%s: no USB problem said: %s" % (name, said))
    check_probe_gaps(r, name, "no")
    # the sound adapter on the dead hub (as the r16 log shows): still "active" with no completions,
    # it keeps nothing alive; the restart's detach takes it away, the re-offer takes it again and its
    # completions resume
    snd = [(t, text) for t, text in P.pad_log(r) if text.startswith("sound: ")]
    H.check(len(snd) == 3 and "detach" in snd[1][1] and "taken" in snd[2][1] and restarts and
            snd[1][0] >= restarts[0] - 1, "%s: sound taken, detached at the restart, taken again: %s" % (name, snd))
    comp = [int(text.split("completions so far ")[1]) for _, text in snd]
    fell = [int(text.split("completions so far ")[1].split(")")[0]) for _, text in P.pad_log(r)
            if text.startswith("hub: the bus power falls")]
    end = [int(text.split("sound_completions=")[1]) for _, text in P.pad_log(r) if text.startswith("stat:")]
    H.check(len(comp) == 3 and fell and comp[1] == fell[0],
            "%s: no completion from the dead adapter (still active) from the fall to the restart: %s %s" % (name, fell, comp))
    H.check(len(comp) == 3 and end and end[-1] > comp[2] + 100, "%s: its completions resume after: %s %s" % (name, comp, end))
    P.check_shutdown(r, name)


def test_hub_blip_second_pad():
    """After the recovery a second controller plugged into the hub works
    beside the first; one
    plugged into the dead hub before the restart comes back with it."""
    r = blip_run("hub_blip_second_pad", after="pad in dualsense\nwait 0.5\n")
    name = "a controller into the dead hub"
    restarts = [t for t, text in P.pad_log(r) if text.startswith("hub: power restarted under the dead hub")]
    H.check(len(restarts) == 1, "%s: one restart: %s" % (name, restarts))
    back = [text for _, text in P.pad_log(r) if text.startswith("hub: back after the restart")]
    H.check(back == ["hub: back after the restart: 4 device(s) on it offered again"],
            "%s: the controller plugged in meanwhile is offered with the rest: %s" % (name, back))
    conn = [text for _, text in P.toasts(r) if "connected" in text and "dis" not in text]
    H.check(any("DualSense" in text for text in conn) and any("Switch" in text for text in conn[1:]),
            "%s: both controllers connected after the recovery: %s" % (name, conn))
    H.check(P.p1(r, "a1") == "A", "%s: A reaches the game with both in: %s" % (name, P.p1(r, "a1")))


def test_hub_dip_alive():
    """The power dips and the hub rides it out (its controller's reports go
    on): nothing is restarted, nothing said."""
    r = H.Run("hub_dip_alive").run(P.START_GAME + """pad port 1
wait 1
pad hub in
wait 2
pad in xinput
wait 2
pad hub dip 100
wait 6
""" + P.held_peek("Pad_A", "a1") + """quit
""")
    name = "a dip the hub rides out"
    H.check(P.p1(r, "a1") == "A", "%s: the controller works on: %s" % (name, P.p1(r, "a1")))
    H.check(not cycles(r) and not hub_said(r), "%s: no restart, nothing said: %s %s" % (name, cycles(r), hub_said(r)))
    log = padlog(r)
    H.check(any("hub: the port's power FELL" in line for line in log), "%s: the fall is in the pad log" % name)
    H.check(any("its devices still answer (data came in)" in line for line in log),
            "%s: the pad log says the hub still answers" % name)
    nothing_wrong_said(r, name)


def test_hub_wedge_recovers():
    """No fall of the power, but the OS's explore never gets through (its
    flag set through all the OS's ticks): after 12 s gpSP restarts the port's
    power once, says so, and the hub and the sound come back; with the sound's
    sleeps only, the OS is back to ~6 a second after it."""
    r = H.Run("hub_wedge_recovers").run(P.HEAVY + P.START_GAME + """pad sound serves
pad port 1
wait 1
pad hub in
wait 3
pad in sound
wait 5
pad hub wedge
wait 20
pad stat
wait 10
pad stat
quit
""")
    name = "the OS's explore wedged"
    stuck = [t for t, text in P.pad_log(r) if text.startswith("pad hub wedge")]
    restarts = [t for t, text in P.pad_log(r) if text.startswith("reenum cycle 10")]
    H.check(len(restarts) == 1, "%s: one restart: %s" % (name, restarts))
    if stuck and restarts:
        dt = restarts[0] - stuck[0]
        H.check(12.0 <= dt <= 21.0, "%s: the restart %.2f s after the flag stuck (12 s, then the hub's answer)" % (name, dt))
    H.check(hub_said(r) == [WEDGED], "%s: said once: %s" % (name, hub_said(r)))
    log = padlog(r)
    H.check(any("hub: DEAD (the OS's explore of the hub has not run): recovery #1" in line for line in log),
            "%s: the pad log says why" % name)
    H.check(any("hub: recovery: the hub is offered again" in line for line in log), "%s: the hub is back" % name)
    sl = [int(text.split("sleeps=")[1].split()[0]) for _, text in P.pad_log(r) if text.startswith("stat:")]
    H.check(len(sl) == 2 and (sl[1] - sl[0]) / 10.0 <= 8,
            "%s: after it the OS gets the sound's sleeps only (%s a second, 8 at the most)" %
            (name, (sl[1] - sl[0]) / 10.0 if len(sl) == 2 else sl))


def test_hub_blip_still_dead():
    """A restart that brings nothing back (the model's 'cycle ignored'): the
    hub is not offered again within 40 s (the OS may finish a ~25 s explore
    run first); gpSP says to replug it, once, and restarts no more than its
    limit (3 for one hub, 30 s apart)."""
    r = H.Run("hub_blip_still_dead").run(P.START_GAME + """pad port 1
wait 1
pad hub in
wait 2
pad in xinput
wait 2
pad cycle ignored
pad hub blip 100
wait 56
quit
""")
    name = "a hub the restart does not bring back"
    restarts = [t for t, text in P.pad_log(r) if text.startswith("reenum cycle 10")]
    H.check(len(restarts) == 1, "%s: one restart (the hub gone after it: nothing more to restart): %s" % (name, restarts))
    told = [t for t, text in osd(r) if text == STILLDEAD]
    H.check(len(told) == 1 and restarts and 39.5 <= told[0] - restarts[0] <= 41.5,
            "%s: \"still dead\" 40 s after the restart: %s %s" % (name, told, restarts))
    H.check(hub_said(r) == [LOSTPOWER, STILLDEAD], "%s: said, then to replug: %s" % (name, hub_said(r)))
    H.check(any("recovery FAILED" in line for line in padlog(r)), "%s: the pad log says it failed" % name)


def test_hub_toasts_fit():
    """Each of the recovery's toasts is one band line (50 characters, OSD_TEXT_MAX 63)."""
    for text in (LOSTPOWER, WEDGED, STILLDEAD):
        H.check(len(text) <= 50, "toast \"%s\": %d characters (50 at the most)" % (text, len(text)))


TESTS += [test_hub_blip_recovers, test_hub_blip_second_pad, test_hub_dip_alive, test_hub_wedge_recovers,
          test_hub_blip_still_dead, test_hub_toasts_fit]


# A hub that is alive is never cut;
# what is due belongs to its hub; the port leaving ends it; late returns.
def dip_run(name, setup, extra=""):
    return H.Run(name).run(P.START_GAME + setup + """pad port 1
wait 1
pad hub in
wait 2
""" + extra + """wait 2
pad hub dip 100
wait 8
quit
""")


def alive_not_cut(r, name, probe=True):
    H.check(not cycles(r) and not hub_said(r), "%s: no restart, nothing said: %s %s" % (name, cycles(r), hub_said(r)))
    log = padlog(r)
    H.check(any("hub: the port's power FELL" in line for line in log), "%s: the fall is in the pad log" % name)
    if probe:
        H.check(any("not cut: the hub answers a request" in line for line in log),
                "%s: the hub's own answer kept it: %s" % (name, recover_lines(log)))
        asked = [text for _, text in P.pad_log(r) if text.startswith("hub: asked whether it answers")]
        H.check(len(asked) == 1, "%s: the hub asked once: %s" % (name, asked))
        check_probe_gaps(r, name, "yes")


def test_hub_dip_drive():
    """A dip under a hub with only a drive on it (it brings nothing in while
    nobody reads it): the hub's own answer keeps it."""
    alive_not_cut(dip_run("hub_dip_drive", "", "pad in drive\n"), "a dip, a drive alone")


def test_hub_dip_onchange():
    """A dip under a hub with a controller that reports only on a change
    (nothing pressed: no reports): the hub's own answer keeps it."""
    r = dip_run("hub_dip_onchange", "pad reports onchange\n", "pad in xinput\n")
    alive_not_cut(r, "a dip, a quiet controller")


def test_hub_dip_empty():
    """A dip under an empty hub: asked, it answers, left as it is."""
    alive_not_cut(dip_run("hub_dip_empty", ""), "a dip, an empty hub")


def test_hub_stuck_explore_alive():
    """The OS's explore flag stuck (a device that never starts on an
    overloaded hub) with an idle drive and a quiet controller on the hub:
    nothing brings data in, but the hub answers: not cut, and not asked
    again for 60 s."""
    r = H.Run("hub_stuck_explore_alive").run(P.START_GAME + """pad reports onchange
pad port 1
wait 1
pad hub in
wait 2
pad in drive
wait 1
pad in xinput
wait 2
pad explore stuck
wait 40
quit
""")
    name = "a stuck explore on a hub that answers"
    H.check(not cycles(r) and not hub_said(r), "%s: no restart, nothing said: %s %s" % (name, cycles(r), hub_said(r)))
    asked = [text for _, text in P.pad_log(r) if text.startswith("hub: asked whether it answers")]
    H.check(len(asked) == 1, "%s: asked once in 40 s: %s" % (name, asked))
    H.check(not [t for _, t in P.pad_log(r) if "power restarted under" in t or "bus power dropped" in t],
            "%s: the hub's power never cut" % name)


def test_hub_flapping():
    """The power falls again and again (a hub at its limit): restarts at most
    one in 30 s, three for the hub, then 'still dead' once."""
    r = H.Run("hub_flapping").run(P.START_GAME + """pad port 1
wait 1
pad hub in
wait 2
pad in xinput
wait 2
pad hub blip 100
wait 8
pad hub blip 100
wait 30
pad hub blip 100
wait 40
pad hub blip 100
wait 40
pad hub blip 100
wait 20
quit
""")
    name = "a port that keeps losing power"
    restarts = [t for t, text in P.pad_log(r) if text.startswith("reenum cycle 10")]
    H.check(len(restarts) == 3, "%s: three restarts, the limit: %s" % (name, restarts))
    gaps = [b - a for a, b in zip(restarts, restarts[1:])]
    H.check(all(g >= 30.0 for g in gaps), "%s: 30 s apart at least: %s" % (name, gaps))
    said = hub_said(r)
    H.check(said.count(STILLDEAD) == 1 and said.count(LOSTPOWER) == 3, "%s: three restarts said, then the limit once: %s" %
            (name, said))


def test_hub_stale_due():
    """The hub dies while the menus are open (no restart from the menus); it
    is pulled and another, alive, put in before the game goes on: what was
    due for the first is not done to the second."""
    r = H.Run("hub_stale_due").run(P.START_GAME + """pad port 1
wait 1
pad hub in
wait 2
pad in xinput
wait 2
pad hub blip 100
press menu
wait 6
pad hub out
wait 1
pad port 1
pad hub in
wait 2
pad in xinput
wait 1
press esc
wait 6
""" + P.held_peek("Pad_A", "a1") + """quit
""")
    name = "a recovery due, then another hub"
    H.check(not cycles(r) and not hub_said(r), "%s: no restart of the new hub: %s %s" % (name, cycles(r), hub_said(r)))
    H.check(any("hub: the recovery due ends" in line for line in padlog(r)), "%s: the pad log says it ended: %s" %
            (name, recover_lines(padlog(r))))
    H.check(P.p1(r, "a1") == "A", "%s: the new hub's controller works: %s" % (name, P.p1(r, "a1")))


def port_during_recovery(name, port):
    return H.Run(name).run(P.START_GAME + """pad port 1
wait 1
pad hub in
wait 2
pad in xinput
wait 2
pad hub late 20
pad hub blip 100
wait 11
pad hub out
pad port %d
wait 50
quit
""" % port)


def test_hub_recovery_port_empty():
    """The adapter taken out while the hub's return is awaited: the recovery
    ends, nothing more said or done."""
    r = port_during_recovery("hub_recovery_port_empty", 0)
    name = "the adapter out during the recovery"
    H.check(len([c for c in cycles(r) if c.startswith("reenum cycle 10")]) == 1, "%s: the one restart: %s" % (name, cycles(r)))
    H.check(hub_said(r) == [LOSTPOWER], "%s: no \"still dead\" after: %s" % (name, hub_said(r)))
    H.check(any("hub: the recovery under way ends" in line for line in padlog(r)), "%s: said in the pad log: %s" %
            (name, recover_lines(padlog(r))))


def test_hub_recovery_device_end():
    """The cable's device end put in while the hub's return is awaited: the
    same."""
    r = port_during_recovery("hub_recovery_device_end", 2)
    name = "the device end during the recovery"
    H.check(hub_said(r) == [LOSTPOWER], "%s: no \"still dead\" after: %s" % (name, hub_said(r)))
    H.check(any("hub: the recovery under way ends" in line for line in padlog(r)), "%s: said in the pad log" % name)


def test_hub_back_late():
    """The hub comes back 12 s after the restart (the OS's explore finishing
    a run against the dead hub first): no 'still dead', its devices offered,
    the controller works; and 45 s after: 'still dead' said at 40 s, the
    return then taken as the recovery's."""
    r = H.Run("hub_back_late").run(P.START_GAME + """pad port 1
wait 1
pad hub in
wait 2
pad in xinput
wait 2
pad hub late 12
pad hub blip 100
wait 25
""" + P.held_peek("Pad_A", "a1") + """quit
""")
    name = "the hub back late"
    H.check(hub_said(r) == [LOSTPOWER], "%s: no \"still dead\": %s" % (name, hub_said(r)))
    back = [line for line in padlog(r) if "hub: recovery: the hub is offered again" in line]
    H.check(back and 11500 <= int(back[0].split("again ")[1].split()[0]) <= 13500, "%s: back ~12 s after: %s" % (name, back))
    H.check(P.p1(r, "a1") == "A", "%s: the controller works: %s" % (name, P.p1(r, "a1")))
    r = H.Run("hub_back_very_late").run(P.START_GAME + """pad port 1
wait 1
pad hub in
wait 2
pad in xinput
wait 2
pad hub late 45
pad hub blip 100
wait 55
""" + P.held_peek("Pad_A", "a1") + """quit
""")
    name = "the hub back after 'still dead'"
    H.check(hub_said(r) == [LOSTPOWER, STILLDEAD], "%s: said: %s" % (name, hub_said(r)))
    H.check(any("(late: after" in line for line in padlog(r)), "%s: its return logged as late" % name)
    H.check(P.p1(r, "a1") == "A", "%s: the controller works: %s" % (name, P.p1(r, "a1")))


def test_hub_dead_quit():
    """The hub dead with the sound adapter on it still "active" (its detach
    never called: the OS was told nothing), then the quit before any
    recovery: nothing in gpSP waits on the old adapter, the driver's
    shutdown runs once, the exit is clean."""
    r = H.Run("hub_dead_quit").run(P.START_GAME + """pad sound serves
pad port 1
wait 1
pad hub in
wait 2
pad in sound
wait 2
pad hub blip 100
wait 1.5
quit
""")
    name = "a quit with the dead hub's sound still active"
    snd = [text for _, text in P.pad_log(r) if text.startswith("sound: ")]
    H.check(len(snd) == 1 and "taken" in snd[0], "%s: never detached: %s" % (name, snd))
    H.check(not cycles(r), "%s: no restart before the quit: %s" % (name, cycles(r)))
    P.check_shutdown(r, name)


def test_hub_dip_stall():
    """A dip under a hub with only an idle drive on it, a hub that answers
    the probe with a stall: a stall is an answer, the hub is not cut."""
    r = dip_run("hub_dip_stall", "", "pad hub stall\npad in drive\n")
    name = "a dip, a hub that stalls the probe"
    H.check(not cycles(r) and not hub_said(r), "%s: no restart: %s %s" % (name, cycles(r), hub_said(r)))
    asked = [text for _, text in P.pad_log(r) if text.startswith("hub: asked whether it answers")]
    H.check(len(asked) == 1 and "yes (a stall)" in asked[0], "%s: the stall taken as an answer: %s" % (name, asked))


def test_hub_probe_hub_leaves():
    """The hub found dead and asked (its request queued behind the OS's
    explore of the dead hub); the hub is pulled with the request under way,
    another plugged in; then the quit: the old request is cancelled, nothing
    is cut, the new hub's controller works, and the exit is clean."""
    r = H.Run("hub_probe_hub_leaves").run(P.START_GAME + """pad port 1
wait 1
pad hub in
wait 2
pad in xinput
wait 2
pad hub blip 100
wait 4.5
pad hub out
wait 1
pad port 1
pad hub in
wait 2
pad in xinput
wait 3
""" + P.held_peek("Pad_A", "a1") + """quit
""")
    name = "the hub pulled with the probe under way"
    asked = [text for _, text in P.pad_log(r) if text.startswith("hub: asked whether it answers")]
    H.check(asked and "cancelled: the hub left" in asked[0], "%s: its request cancelled: %s" % (name, asked))
    H.check(not cycles(r) and not hub_said(r), "%s: nothing cut, nothing said: %s %s" % (name, cycles(r), hub_said(r)))
    H.check(P.p1(r, "a1") == "A", "%s: the new hub's controller works: %s" % (name, P.p1(r, "a1")))
    P.check_shutdown(r, name)


def idle_run(name, extra, blip):
    return H.Run(name).run(P.START_GAME + extra + """pad port 1
wait 1
pad hub in
wait 2
pad in sound
wait 2
pad sound idle on
wait 1
pad hub %s 100
wait 16
quit
""" % blip)


def test_hub_idle_sound_no_probe():
    """(nspire-usbaudio ua17's idle mode: the game's sound on Bluetooth) An
    idle sound card brings no completions while healthy. On an OS where the
    hub can't be asked, a dip with only that card on the hub: "stalled" is
    the only dead sign there, and an idle card is not stalled: no cut. The
    same with the card dead and not idle: cut."""
    r = idle_run("hub_idle_sound_no_probe", "pad explore unknown\n", "dip")
    name = "an idle card, a dip, no probe"
    H.check(not cycles(r) and not hub_said(r), "%s: no restart: %s %s" % (name, cycles(r), hub_said(r)))
    H.check(any("the hub can't be asked and nothing on it says it is dead" in line for line in padlog(r)),
            "%s: the pad log says why: %s" % (name, recover_lines(padlog(r))))
    r = H.Run("hub_dead_sound_no_probe").run(P.START_GAME + """pad explore unknown
pad port 1
wait 1
pad hub in
wait 2
pad in sound
wait 3
pad hub blip 100
wait 16
quit
""")
    name = "a dead card, no probe"
    H.check(len([c for c in cycles(r) if c.startswith("reenum cycle 10")]) == 1,
            "%s: the stalled sound says dead: one restart: %s" % (name, cycles(r)))


def test_hub_idle_sound_dead_probe():
    """An idle card on a hub that is dead, the hub asked: the probe says it,
    the hub is restarted as before."""
    r = idle_run("hub_idle_sound_dead_probe", "", "blip")
    name = "an idle card on a dead hub"
    H.check(len([c for c in cycles(r) if c.startswith("reenum cycle 10")]) == 1, "%s: one restart: %s" % (name, cycles(r)))
    H.check(hub_said(r) == [LOSTPOWER], "%s: said: %s" % (name, hub_said(r)))


def hubrec(r):
    """nspire_hub_recovering's changes: (time, value)."""
    return [(t, int(text.split()[1])) for t, text in P.pad_log(r) if text.startswith("hubrec: ")]


def test_hub_recovering_flag():
    """nspire_hub_recovering (for Bluetooth's toasts): 1 from the fall
    through the restart until 5 s after the hub's devices are back, then 0;
    a dip the hub survives: 1 while it is watched, 0 once found alive; a
    give-up: 0 once "still dead" is said."""
    r = blip_run("hub_recovering_blip")
    name = "the recovering flag through a recovery"
    ch = hubrec(r)
    fell = [t for t, text in P.pad_log(r) if text.startswith("hub: the bus power falls")]
    restart = [t for t, text in P.pad_log(r) if text.startswith("reenum cycle 10")]
    back = [t for t, text in P.pad_log(r) if text.startswith("hub: back after the restart")]
    ones = [t for t, v in ch if v == 1]
    zeros = [t for t, v in ch if v == 0 and ones and t > ones[0]]
    H.check(len(ones) == 1 and fell and ones[0] - fell[0] <= 0.1, "%s: 1 at the fall: %s %s" % (name, ch, fell))
    H.check(len(zeros) == 1 and back and restart and zeros[0] > restart[0] and 4.8 <= zeros[0] - back[0] <= 5.5,
            "%s: 1 through the restart, 0 5 s after the hub is back: %s back %s" % (name, ch, back))
    r = dip_run("hub_recovering_dip", "", "pad in xinput\n")
    name = "the recovering flag through a dip it survives"
    ch = hubrec(r)
    H.check([v for _, v in ch][-2:] == [1, 0] and ch[-1][0] - ch[-2][0] < 1.0,
            "%s: 1 at the fall, 0 once its controller's reports say alive: %s" % (name, ch))
    r = H.Run("hub_recovering_giveup").run(P.START_GAME + """pad port 1
wait 1
pad hub in
wait 2
pad in xinput
wait 2
pad cycle ignored
pad hub blip 100
wait 56
quit
""")
    name = "the recovering flag at a give-up"
    ch = hubrec(r)
    told = [t for t, text in osd(r) if text == STILLDEAD]
    H.check(told and ch and ch[-1][1] == 0 and abs(ch[-1][0] - told[0]) < 0.1,
            "%s: 0 when \"still dead\" is said: %s %s" % (name, ch, told))


def test_hub_sound_1000ms():
    """speed18 (NSPIRE_AUDIO_HUB_MS 1000 + HUB_WEDGE_SLEEPS 10): with the OS's
    explore flag known, streaming sound gives the hub ~1 sleep/s, a stuck
    news flag leaves only those 1 Hz sleeps when the hub wedges (still
    detected in ~12 s and recovered), and hot-plug after it still connects
    within 2.5 s."""
    r = H.Run("hub_sound_1000ms").run(P.HEAVY + P.START_GAME + """pad hubslow 40
pad sound serves
pad port 1
wait 1
pad hub in
wait 3
pad in sound
wait 6
pad stat
wait 10
pad stat
pad explore stuck
wait 12
pad stat
pad hub wedge
wait 20
pad in xinput
wait 3
""" + P.held_peek("Pad_A", "a") + """quit
""")
    name = "1000 ms hub cadence, wedge & hot-plug"
    sl = [int(text.split("sleeps=")[1].split()[0]) for _, text in P.pad_log(r) if text.startswith("stat:")]
    H.check(len(sl) == 3 and (sl[1] - sl[0]) / 10.0 <= 2.0,
            "%s: idle sound+hub OS cost %.1f sleeps/s (2.0 at the most)" %
            (name, (sl[1] - sl[0]) / 10.0 if len(sl) >= 2 else -1))
    stuck = [t for t, text in P.pad_log(r) if text.startswith("pad hub wedge")]
    restarts = [t for t, text in P.pad_log(r) if text.startswith("reenum cycle 10")]
    H.check(len(restarts) == 1, "%s: one restart after wedge at 1 Hz sleeps: %s" % (name, restarts))
    if stuck and restarts:
        dt_w = restarts[0] - stuck[0]
        H.check(12.0 <= dt_w <= 21.0, "%s: wedge restart %.2f s after wedge (12..21 s)" % (name, dt_w))
    H.check(hub_said(r) == [WEDGED], "%s: wedged toast once: %s" % (name, hub_said(r)))
    dt_p = P.plug_to(r, "pad in xinput")
    H.check(dt_p is not None and dt_p <= 2.5, "%s: hot-plug after recovery connected %s s (<= 2.5 s)" % (name, dt_p))
    H.check(P.p1(r, "a") == "A", "%s: pad A reaches game: %s" % (name, P.p1(r, "a")))



NOSTART = "A hub device didn't start: try another port"


def swap_stale(name, gap=1, deaf=True):
    """A CX II log (r19, 926fa73): a controller pulled from
    the hub leaves its port as the hub last answered ("p!": powered, not
    connected, a change) until TI's explore reads it again; the next controller
    is plugged in GAP s after the pull (into a hub that says nothing of it when
    DEAF, as an r15 log's hub, or one that does)."""
    return H.Run(name).run(P.START_GAME + """pad hub stale
pad port 1
wait 1
pad hub in
wait 2
pad in xinput
wait 2
""" + P.held_peek("Pad_A", "a1") + """pad out
wait %s
%spad in dualsense
wait 14
""" % (gap, "pad hub deaf\n" if deaf else "") + P.held_peek("Pad_B", "b1") + """pad out
wait 2
quit
""")


def test_hub_pull_stale():
    """A pulled controller's port read "p!" (the hub's last answer) is not a
    device that does not start: no "A hub device didn't start" toast, no "A
    DEVICE ON IT DOES NOT START" line, and the looks asked of the OS go on, so
    a controller plugged in right after the pull, into a hub that says nothing
    of it, connects within 5 s of its plug (r19 evening: the toast 8 s after
    each pull, 139438 and 261275, and the port read again ~10 s after it)."""
    for deaf, gap in ((True, 1), (True, 3), (False, 1)):
        name = "a pulled port read stale (%s hub, the next pad %d s after)" % ("a deaf" if deaf else "a", gap)
        r = swap_stale("hub_pull_stale_%s%d" % ("deaf" if deaf else "news", gap), gap, deaf)
        said = [text for _, text in osd(r)]
        H.check(NOSTART not in said, "%s: \"%s\" is never said of a pulled port: %s" % (name, NOSTART, said))
        log = padlog(r)
        H.check(not any("DOES NOT START" in line for line in log), "%s: the pad log never says a device does not start:\n%s"
                % (name, "\n".join(line for line in log if "hub:" in line)))
        dt = P.plug_to(r, "pad in dualsense", "Controller connected (DualSense)")
        H.check(dt is not None and dt <= 5.0, "%s: the next controller connected %s s after its plug (5 s at the most)" %
                (name, dt))
        H.check(P.p1(r, "a1") == "A" and P.p1(r, "b1") == "B", "%s: both controllers' buttons reach the game: %s %s" %
                (name, P.p1(r, "a1"), P.p1(r, "b1")))
        H.check(any("p!" in line and "its ports as the OS sees them" in line for line in log),
                "%s: the pad log shows the stale port (the case is the r19 one)" % name)
        nothing_wrong_said(r, name)
        H.check(not cycles(r), "%s: no power restart: %s" % (name, cycles(r)))



def test_hub_page_unseen():
    """The Controller page with a hub in and no controller on it says, under
    the list, what a controller plugged in and never listed means (r19's CX
    II: a Switch Pro and a DualSense that no port of the hub ever saw)."""
    r = H.Run("hub_page_unseen").run(P.START_GAME + """pad port 1
wait 1
pad hub in
wait 2
pad in sound
wait 2
press menu
wait 0.3
""" + P.CONTROLLER_PAGE + """wait 4
press esc
wait 0.5
press esc
wait 0.5
press esc
wait 1
quit
""")
    said = [f.get("said", "") for _, v, f, _ in P.page_lines(r) if v == "hub"]
    H.check(said and "isn't seen by the hub: use a data cable (not charge-only), or a hub with its own power." in said[-1],
            "hub page: a controller the hub never saw is explained: %s" % said[-1:])


TESTS += [test_hub_pull_stale, test_hub_page_unseen]

TESTS += [test_hub_recovering_flag, test_hub_idle_sound_no_probe, test_hub_idle_sound_dead_probe, test_hub_dip_stall, test_hub_probe_hub_leaves, test_hub_dead_quit, test_hub_dip_drive, test_hub_dip_onchange, test_hub_dip_empty, test_hub_stuck_explore_alive, test_hub_flapping,
          test_hub_stale_due, test_hub_recovery_port_empty, test_hub_recovery_device_end, test_hub_back_late,
          test_hub_sound_1000ms]


def main():
    if not os.path.exists(P.USBPAD_H):
        print("hub tests skipped: no %s (gpsp-host has no pads)" % os.path.normpath(P.USBPAD_H))
        return 0
    wanted = sys.argv[1:]
    os.makedirs(H.WORK, exist_ok=True)
    for test in TESTS:
        name = test.__name__[5:]
        if wanted and name not in wanted:
            continue
        print(name)
        test()
    print("hub: %d checks, %d failed" % (H.checks, len(H.failures)))
    return 1 if H.failures else 0


if __name__ == "__main__":
    sys.exit(main())

